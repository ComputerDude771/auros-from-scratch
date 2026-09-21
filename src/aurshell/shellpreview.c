/* shellpreview — render any archetype to a PNG through its real paint path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"
#include "../common/wall.h"
#include "../common/png.h"

static font *openf(const char *p, float px)
{
    font *f = (p && *p) ? font_load(p, px) : NULL;
    if (f) return f;
    const char *fb[] = {
      "/home/user/auros-from-scratch/work/forge/desktop/rootfs/usr/share/fonts/opentype/inter/Inter-Regular.otf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL };
    for (int i = 0; fb[i]; i++) { f = font_load(fb[i], px); if (f) return f; }
    return NULL;
}

/* A representative machine: some things open, some not. Every layout
 * renders the SAME state, so the previews are genuinely comparable.
 * The app set comes from shell_seed_apps() so this harness cannot drift
 * from the real one -- it used to carry its own copy of the table, with
 * the same nine colours hardcoded. */
static void seed(shell_ctx *c, int n_open)
{
    shell_seed_apps(c);

    const char *subs[] = { "auros.example", "3 new messages", "Last summer", "Documents" };
    c->n_wins = n_open;
    for (int i = 0; i < n_open && i < SHELL_MAX_WINS; i++) {
        c->wins[i].app = i % c->n_apps;
        snprintf(c->wins[i].title, sizeof c->wins[i].title, "%s", c->apps[c->wins[i].app].name);
        snprintf(c->wins[i].subtitle, sizeof c->wins[i].subtitle, "%s", subs[i % 4]);
        c->wins[i].content = NULL;
    }
    c->focus = n_open ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *shellf = argc > 1 ? argv[1] : "shells/rail.shell";
    const char *themef = argc > 2 ? argv[2] : "themes/nocturne.theme";
    const char *out    = argc > 3 ? argv[3] : "/tmp/shell.png";
    int W = argc > 4 ? atoi(argv[4]) : 1600;
    int H = argc > 5 ? atoi(argv[5]) : 900;
    int nopen = argc > 6 ? atoi(argv[6]) : 3;

    theme_t t = {0};
    if (theme_load(&t, themef) < 0) { fprintf(stderr, "no theme %s\n", themef); return 1; }

    shell_ctx c; memset(&c, 0, sizeof c);
    shell_theme_load(&c, &t);
    if (shell_archetype_load(&c, shellf) < 0) { fprintf(stderr, "no shell %s\n", shellf); return 1; }
    c.allow_install = c.allow_settings = c.allow_theme_change = 1;
    c.mouse_x = c.mouse_y = -1; c.hover = -1;
    seed(&c, nopen);

    const shell_layout *L = shell_layout_by_id(c.layout_id);
    c.screen_w = W; c.screen_h = H;   /* before init: hit-testing needs it */
    if (L->init) L->init(&c);

    /* The same two-face split the shell itself loads, so a preview is
     * a preview and not a differently-typeset picture. */
    const char *fs = theme_str(&t, "font_sans", "");
    const char *fd = theme_str(&t, "font_display", fs);
    const char *ft = theme_str(&t, "font_text", fs);
    const char *fbo = theme_str(&t, "font_text_bold", ft);
    float base = (float)theme_int(&t, "font_size", 14);
    float sm   = (float)theme_int(&t, "font_size_sm", 12);
    float lg   = (float)theme_int(&t, "font_size_lg", 19);
    shell_fonts f = {
        .huge  = openf(fd, lg * 2.25f),
        .big   = openf(fd, lg * 1.45f),
        .dmid  = openf(fd, lg * 0.95f),
        .mid   = openf(fbo, base * 1.10f),
        .label = openf(fbo, sm),
        .small = openf(ft, base),
        .tiny  = openf(ft, sm),
        .lh    = (float)theme_num(&t, "line_height", 1.45),
    };

    surface *s = surface_new(W, H), *wall = surface_new(W, H);
    uint32_t *tmp = malloc((size_t)W*H*sizeof *tmp);
    wall_render(tmp, W, H, &t);
    for (int i = 0; i < W*H; i++) wall->px[i] = 0xFF000000u | tmp[i];
    free(tmp);

    L->paint(&c, s, &f, wall);

    uint32_t *o = malloc((size_t)W*H*sizeof *o);
    for (int i = 0; i < W*H; i++) o[i] = s->px[i] & 0xFFFFFFu;
    int rc = png_write_rgb(out, o, W, H);
    free(o); surface_free(s); surface_free(wall);
    if (L->fini) L->fini(&c);
    fprintf(stderr, "%s  %dx%d  layout=%s  theme=%s  open=%d\n",
            out, W, H, c.layout_id, theme_str(&t, "theme_name", "?"), nopen);
    return rc;
}
