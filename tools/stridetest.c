/* stridetest.c — does the shell paint correctly into a padded buffer?
 *
 * A scanout buffer's stride is almost never its width. The kernel pads
 * each row out to the hardware's pitch alignment: on Intel graphics a
 * 1366-pixel row at 32bpp becomes 1376 pixels of stride. kms.c reports
 * that faithfully (s.stride = pitch / 4). Every rasterizer that assumes
 * stride equals width then writes each row a few pixels further left
 * than the last, and the whole desktop is sheared diagonally.
 *
 * Nothing we had could see it. surface_new() sets stride = w, so every
 * PNG render, the contact sheet, the hit-test harness and the preview
 * tool all run at stride == width. QEMU's virtual display is 1024 wide
 * and 1024 * 4 is already aligned, so it does not reproduce there
 * either. The bug was invisible in every environment we test in and
 * present on every machine we ship to.
 *
 * This renders each archetype twice -- once into a tight surface, once
 * into one padded by an odd number of pixels -- and requires the two to
 * be identical within the visible rectangle. Any primitive that ignores
 * stride fails, not just the one that did.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aurshell/shell.h"
#include "../src/aurshell/draw.h"
#include "../src/aurshell/anim.h"
#include "../src/common/theme.h"
#include "../src/common/font.h"

/* An odd pad, deliberately: a multiple of 4 or 8 can be accidentally
 * right when the arithmetic happens to work out. */
#define PAD 13
#define W   1366
#define H    768

extern const shell_layout layout_rail, layout_tiles, layout_locked,
                          layout_taskbar, layout_dock, layout_workbench;

static const shell_layout *ALL[] = {
    &layout_rail, &layout_tiles, &layout_locked,
    &layout_taskbar, &layout_dock, &layout_workbench,
};

/* A surface whose rows are further apart than they are wide -- which is
 * what kms_back_surface() hands the shell on real hardware. */
static surface *padded_surface(int w, int h, int pad)
{
    surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->stride = w + pad;
    s->px = calloc((size_t)s->stride * h, sizeof *s->px);
    if (!s->px) { free(s); return NULL; }
    s->w = w; s->h = h;
    return s;
}

static void fill_ctx(shell_ctx *c, const shell_layout *L, const theme_t *t)
{
    memset(c, 0, sizeof *c);
    shell_theme_load(c, t);
    snprintf(c->layout_id, sizeof c->layout_id, "%s", L->id);
    c->screen_w = W; c->screen_h = H;
    c->show_clock = 1; c->status_full = 1;
    c->allow_install = c->allow_settings = c->allow_theme_change = 1;
    c->mouse_x = W / 2; c->mouse_y = H / 2;
    c->hover = -1; c->focus = -1;
    shell_seed_apps(c);
    /* Windows too: their titles are the longest text the shell draws,
     * and a placeholder window exercises the icon and body paths. */
    for (int i = 0; i < 3 && i < SHELL_MAX_APPS; i++) {
        c->wins[i].app = i;
        snprintf(c->wins[i].title, sizeof c->wins[i].title, "%s", c->apps[i].name);
        snprintf(c->wins[i].subtitle, sizeof c->wins[i].subtitle, "%s", c->apps[i].hint);
        c->n_wins++;
    }
    c->focus = 0;
}

int main(int argc, char **argv)
{
    theme_t t = {0};
    if (argc > 1 && theme_load(&t, argv[1]) < 0)
        fprintf(stderr, "stridetest: could not read %s — using defaults\n", argv[1]);

    /* The clock must not tick between the two renders of a pair. */
    if (!getenv("AURSHELL_CLOCK")) setenv("AURSHELL_CLOCK", "1700000000", 1);

    /* One set of fonts for every archetype: loading is slow and the
     * glyph cache is what makes the second render of a pair cheap. */
    shell_ctx fc; memset(&fc, 0, sizeof fc);
    shell_theme_load(&fc, &t);
    shell_fonts f = {0};
    shell_fonts_load(&f, &fc);

    int bad = 0;
    for (size_t k = 0; k < sizeof ALL / sizeof ALL[0]; k++) {
        const shell_layout *L = ALL[k];

        surface *tight = surface_new(W, H);
        surface *padded = padded_surface(W, H, PAD);
        if (!tight || !padded) { fprintf(stderr, "out of memory\n"); return 1; }

        shell_ctx a, b;
        fill_ctx(&a, L, &t); fill_ctx(&b, L, &t);
        /* Real fonts, or this test proves nothing about the primitive
         * that was actually broken: with NULL fonts shell_text() returns
         * immediately and font_draw() -- which was the bug -- never
         * runs. A harness that cannot fail is not a harness. */
        if (!f.small) {
            fprintf(stderr, "stridetest: no usable font found. This test is "
                            "meaningless without one; install fonts-dejavu-core.\n");
            return 2;
        }

        if (L->init) { L->init(&a); L->init(&b); }
        draw_track_reset(); L->paint(&a, tight,  &f, NULL);
        draw_track_reset(); L->paint(&b, padded, &f, NULL);

        long diff = 0; int fx = -1, fy = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t p = tight->px[(size_t)y * tight->stride + x];
                uint32_t q = padded->px[(size_t)y * padded->stride + x];
                if (p != q) { if (fx < 0) { fx = x; fy = y; } diff++; }
            }

        /* The padding itself must stay untouched. A primitive that runs
         * off the end of a row is writing into the next one on a tight
         * surface, where it merely looks wrong -- here it is provable. */
        long spill = 0;
        for (int y = 0; y < H; y++)
            for (int x = W; x < padded->stride; x++)
                if (padded->px[(size_t)y * padded->stride + x] != 0) spill++;

        printf("%-10s %8ld differing px", L->id, diff);
        if (spill) printf(", %ld px written into the row padding", spill);
        if (diff || spill) {
            printf("   FAIL");
            if (fx >= 0) printf(" (first at %d,%d)", fx, fy);
            printf("\n");
            bad++;
        } else printf("   ok\n");

        if (L->fini) { L->fini(&a); L->fini(&b); }
        surface_free(tight); surface_free(padded);
    }

    printf("\n");
    if (bad) {
        printf("%d archetype%s paint differently into a padded buffer.\n",
               bad, bad == 1 ? "" : "s");
        printf("On any display whose pitch is not exactly 4 * width -- which is\n"
               "most of them -- that is what the user sees.\n");
        return 1;
    }
    printf("every archetype paints the same picture whatever the row stride is\n");
    return 0;
}
