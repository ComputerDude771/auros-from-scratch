/* Do the painted pixels and the clickable region agree?
 *
 * docs/SHELLS.md states the rule: one function owns geometry, called by
 * both painting and hit-testing. Deriving them separately is how a UI
 * ends up off by the width of a shadow and feeling haunted. `rail`
 * broke it -- it painted with the surface's size and hit-tested against
 * a hardcoded 1600x900 -- and rail is the archetype every configuration
 * error falls back to, so on the 1366x768 panels this product targets,
 * the default desktop's clicks landed in the wrong place.
 *
 * Checking the shape of the clickable region is not enough: a region
 * clipped by the screen edge changes size even when the geometry behind
 * it is a constant, so that test passes with the bug still in. This
 * compares the two things that must actually agree:
 *
 *   painted   -- paint over a flat wallpaper of a known colour and mark
 *                every pixel that is no longer that colour.
 *   clickable -- sweep click() and mark every point it consumes.
 *
 * A click consumed over untouched wallpaper is a click on nothing. A
 * few are legitimate (a full-screen dismiss layer, the transparent
 * margin of a card), so the bar is a percentage, not zero -- and the
 * bug moves rail's number far past it.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/aurshell/shell.h"

#define WALL_RGB 0x00FF00u      /* nothing in any theme is pure green */
#define STEP     4
#define SLACK    3              /* px of tolerance for shadow and AA  */

static void seed(shell_ctx *c, const char *id, int nwin)
{
    memset(c, 0, sizeof *c);
    theme_t t = {0};
    shell_theme_load(c, &t);
    snprintf(c->layout_id, sizeof c->layout_id, "%s", id);
    c->show_clock = 1;
    c->allow_install = c->allow_settings = c->allow_theme_change = 1;
    c->allow_tty = 1;
    static const char *names[] = { "Internet","Email","Photos","My Files",
                                   "Writing","Music","Settings","Help" };
    for (int i = 0; i < 8; i++) {
        snprintf(c->apps[i].id,   sizeof c->apps[i].id,   "app%d", i);
        snprintf(c->apps[i].name, sizeof c->apps[i].name, "%s", names[i]);
        snprintf(c->apps[i].hint, sizeof c->apps[i].hint, "does a thing");
        c->apps[i].icon = (shell_icon)i;
        c->apps[i].pinned = (i < 5);
        c->n_apps++;
    }
    for (int i = 0; i < nwin; i++) {
        c->wins[i].app = i % c->n_apps;
        snprintf(c->wins[i].title, sizeof c->wins[i].title, "%s",
                 c->apps[c->wins[i].app].name);
        c->n_wins++;
    }
    c->focus = nwin ? 0 : -1;
    c->hover = -1;
}

/* Is any pixel within SLACK of (x,y) painted? */
static int near_painted(const unsigned char *mask, int w, int h, int x, int y)
{
    int x0 = x - SLACK < 0 ? 0 : x - SLACK, x1 = x + SLACK >= w ? w - 1 : x + SLACK;
    int y0 = y - SLACK < 0 ? 0 : y - SLACK, y1 = y + SLACK >= h ? h - 1 : y + SLACK;
    for (int j = y0; j <= y1; j++)
        for (int i = x0; i <= x1; i++)
            if (mask[(size_t)j * w + i]) return 1;
    return 0;
}

static int check(const shell_layout *L, const char *id, int w, int h, int verbose)
{
    shell_ctx c;
    seed(&c, id, 3);
    c.screen_w = w; c.screen_h = h;
    if (L->init) L->init(&c);

    surface *wall = surface_new(w, h), *s = surface_new(w, h);
    for (int i = 0; i < w * h; i++) wall->px[i] = 0xFF000000u | WALL_RGB;
    shell_fonts f = {0};
    c.mouse_x = -10000; c.mouse_y = -10000;     /* no hover highlight */
    L->paint(&c, s, &f, wall);

    unsigned char *mask = calloc((size_t)w * h, 1);
    for (int i = 0; i < w * h; i++)
        mask[i] = ((s->px[i] & 0xFFFFFFu) != WALL_RGB);

    int consumed = 0, on_nothing = 0;
    for (int y = 0; y < h; y += STEP)
        for (int x = 0; x < w; x += STEP) {
            if (!L->click) continue;
            /* Re-init per probe. A click mutates layout state -- the
             * first one opens the taskbar's menu, and after that every
             * probe hits the menu's dismiss path and reports a hit for
             * something that is not on screen. Without this the sweep
             * measures its own side effects. init() is cheap now that
             * geometry comes from the context rather than the last
             * paint, which is the whole point of the change this test
             * exists to protect. */
            shell_ctx probe;
            seed(&probe, id, 3);
            probe.screen_w = w; probe.screen_h = h;
            probe.mouse_x = -10000; probe.mouse_y = -10000;
            if (L->init) L->init(&probe);
            if (!L->click(&probe, x, y)) continue;
            consumed++;
            if (!near_painted(mask, w, h, x, y)) on_nothing++;
        }

    free(mask); surface_free(s); surface_free(wall);
    if (L->fini) L->fini(&c);

    double pct = consumed ? 100.0 * on_nothing / consumed : 0.0;
    if (verbose)
        printf("   %4dx%-4d consumed=%-6d on bare wallpaper=%-5d (%5.1f%%)%s\n",
               w, h, consumed, on_nothing, pct,
               consumed == 0 ? "  NOTHING IS CLICKABLE" : "");
    if (consumed == 0) return 1;
    return pct > 0.25;
}

int main(int argc, char **argv)
{
    static const char *ids[] = { "rail","tiles","locked","taskbar","dock","workbench" };
    struct { int w, h; } res[] = { {1366,768}, {1920,1080}, {1024,600}, {2560,1440} };
    int fail = 0, verbose = (argc < 2 || strcmp(argv[1], "-q") != 0);

    for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
        const shell_layout *L = shell_layout_by_id(ids[i]);
        if (verbose) printf("%s\n", ids[i]);
        for (unsigned r = 0; r < sizeof res / sizeof *res; r++)
            if (check(L, ids[i], res[r].w, res[r].h, verbose)) {
                printf("   FAIL %s at %dx%d: clicks land where nothing is drawn\n",
                       ids[i], res[r].w, res[r].h);
                fail = 1;
            }
    }
    printf("%s\n", fail ? "FAIL" :
           "every archetype hit-tests where it paints, at every resolution");
    return fail;
}
