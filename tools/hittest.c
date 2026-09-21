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
 *
 * The second check is for a different lie. An element that lights up
 * under the pointer has promised it can be clicked. `rail` -- the
 * DEFAULT archetype -- highlighted the six tiles on its Home card and
 * its click handler never looked at them: the card underneath swallowed
 * the click and did nothing, so the first thing anyone ever clicked in
 * this operating system silently failed. Nothing caught it, because the
 * click WAS consumed and it DID land on painted pixels.
 *
 * So: wherever moving the pointer changes the frame, clicking must
 * change the frame too. A highlight that leads nowhere is a UI telling
 * the user something untrue.
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
    /* Real faces. With none, every region whose only painted content is
     * text reads as bare wallpaper -- so an element that lights up, can
     * be clicked, and draws nothing but a word would pass unnoticed.
     * shell_fonts_load() falls back through the system fonts. */
    shell_fonts_load(&f, &c);
    c.mouse_x = -10000; c.mouse_y = -10000;     /* no hover highlight */
    L->paint(&c, s, &f, wall);

    unsigned char *mask = calloc((size_t)w * h, 1);
    for (int i = 0; i < w * h; i++)
        mask[i] = ((s->px[i] & 0xFFFFFFu) != WALL_RGB);

    int consumed = 0, on_nothing = 0;
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
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
            if (!near_painted(mask, w, h, x, y)) {
                on_nothing++;
                /* "0.4% of clicks land on nothing" is a number nobody
                 * can act on. The rectangle they land in is. */
                if (bx0 > x) bx0 = x;
                if (by0 > y) by0 = y;
                if (bx1 < x) bx1 = x;
                if (by1 < y) by1 = y;
            }
        }

    if (on_nothing)
        printf("             they land in x %d..%d, y %d..%d "
               "(screen is %dx%d)\n", bx0, bx1, by0, by1, w, h);

    shell_fonts_free(&f);
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

/* How far from the pointer a change still counts as "this element
 * responded". Generous enough to include a card's border, a lift, a
 * focus ring and a label beside the icon; small enough to exclude a
 * status readout in the top bar.
 *
 * Without this the check reads the whole frame, and an archetype that
 * names the hovered item somewhere else on screen -- workbench and
 * locked both do -- reports that EVERY point reacts to the pointer.
 * Half of them then "fail" for ignoring a click they were never
 * offering, and the real lying affordances are lost in the noise. The
 * rule is "what lights up can be clicked", and what lights up is the
 * thing under the cursor. */
#define NEAR_R 96

/* Render one settled frame and hash it.
 *
 * Only the neighbourhood of the pointer is hashed, for the reason
 * above. `doclick` also dispatches a press/release at (mx,my) first.
 * Both paths run step() to completion afterwards, so a change that is
 * only animated still shows up, and so the two hashes are comparable. */
static unsigned long render_state(const shell_layout *L, const char *id,
                                  int w, int h, int mx, int my,
                                  int doclick, int nwin, int wx, int wy)
{
    shell_ctx c;
    seed(&c, id, nwin);
    c.screen_w = w; c.screen_h = h;
    c.mouse_x = mx; c.mouse_y = my;
    if (L->init) L->init(&c);

    if (L->motion) L->motion(&c, mx, my);
    if (doclick) {
        c.mouse_down = 1;
        if (L->click) L->click(&c, mx, my);
        c.mouse_down = 0;
        if (L->motion) L->motion(&c, mx, my);
    }
    for (int k = 0; k < 200 && L->step; k++)
        if (!L->step(&c, 1.f / 60.f)) break;

    surface *wall = surface_new(w, h), *s = surface_new(w, h);
    for (int i = 0; i < w * h; i++) wall->px[i] = 0xFF000000u | WALL_RGB;
    shell_fonts f = {0};
    /* Real faces. With none, every region whose only painted content is
     * text reads as bare wallpaper -- so an element that lights up, can
     * be clicked, and draws nothing but a word would pass unnoticed.
     * shell_fonts_load() falls back through the system fonts. */
    shell_fonts_load(&f, &c);
    L->paint(&c, s, &f, wall);

    unsigned long hsh = 1469598103934665603UL;          /* FNV-1a */
    /* The window is (wx,wy), NOT the pointer: the resting frame is
     * rendered with the pointer off screen, and a window that followed
     * it there would hash nothing at all and differ from every hover
     * frame -- every probe would "react". */
    int x0 = wx - NEAR_R < 0 ? 0 : wx - NEAR_R;
    int y0 = wy - NEAR_R < 0 ? 0 : wy - NEAR_R;
    int x1 = wx + NEAR_R > w ? w : wx + NEAR_R;
    int y1 = wy + NEAR_R > h ? h : wy + NEAR_R;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            hsh ^= s->px[(size_t)y * s->stride + x];
            hsh *= 1099511628211UL;
        }
    shell_fonts_free(&f);
    surface_free(s); surface_free(wall);
    if (L->fini) L->fini(&c);
    return hsh;
}

/* Wherever hovering changes the frame, clicking must change it FURTHER.
 *
 * The comparison that matters is click-frame against HOVER-frame, not
 * against the resting frame. Comparing against rest was the first
 * version of this test and it passed with the bug still in: the
 * post-click frame still carries the hover highlight, so it always
 * differed from rest and nothing ever looked dead.
 *
 * Two practical concessions, both stated rather than hidden:
 *
 * - A full repaint per probe is the only generic way to ask "did
 *   hovering here change anything", and it is expensive. So this runs
 *   on a small canvas at a coarse step. It is built to catch a REGION
 *   of dead affordances -- six tiles that do nothing -- not a single
 *   two-pixel target.
 * - A few isolated dead points are legitimate: clicking the thing that
 *   is already selected can reasonably leave the frame unchanged. So
 *   the bar is a share of the reactive points, not zero -- though every
 *   archetype currently sits at zero. rail's bug put
 *   every one of its six Home tiles over it.
 */
#define AFF_W    800
#define AFF_H    500
#define AFF_STEP  25
#define AFF_TOLERANCE 0.02          /* share of reactive points allowed to be inert */

static int affordances(const shell_layout *L, const char *id, int verbose)
{
    const int w = AFF_W, h = AFF_H, nwin = 3;
    int hot = 0, dead = 0;
    int dx[12], dy[12], nd = 0;

    for (int y = 0; y < h; y += AFF_STEP)
        for (int x = 0; x < w; x += AFF_STEP) {
            /* Rest is re-measured per probe, over the same window as
             * the hover frame, so the two are comparable. */
            unsigned long rest  = render_state(L, id, w, h, -10000, -10000, 0, nwin, x, y);
            unsigned long hover = render_state(L, id, w, h, x, y, 0, nwin, x, y);
            if (hover == rest) continue;             /* nothing lit up */
            hot++;
            if (render_state(L, id, w, h, x, y, 1, nwin, x, y) != hover) continue;
            dead++;                                  /* lit up, then ignored the click */
            if (nd < 12) { dx[nd] = x; dy[nd] = y; nd++; }
        }

    double share = hot ? (double)dead / hot : 0.0;
    if (verbose) {
        printf("   %d points react to the pointer, %d of those ignore a click (%.0f%%)\n",
               hot, dead, share * 100.0);
        /* The opposite failure, and worth saying out loud rather than
         * passing in silence: an archetype that consumes clicks and
         * never changes under the pointer gives the user no way to tell
         * what is pressable before pressing it. Not a failure -- an
         * archetype may legitimately have one large target -- but it is
         * never an accident worth leaving unremarked. */
        if (!hot) printf("   NOTE: nothing in this archetype responds to hover at all\n");
    }
    if (share > AFF_TOLERANCE) {
        printf("   FAIL %s: %d of %d hover-reactive points highlight and then ignore "
               "a click, on a %dx%d screen. Dead points:\n", id, dead, hot, w, h);
        for (int i = 0; i < nd; i++) printf("          %d,%d\n", dx[i], dy[i]);
        if (dead > nd) printf("          ...and %d more\n", dead - nd);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const char *ids[] = { "rail","tiles","locked","taskbar","dock","workbench" };
    struct { int w, h; } res[] = { {1366,768}, {1920,1080}, {1024,600}, {2560,1440} };
    int fail = 0, verbose = (argc < 2 || strcmp(argv[1], "-q") != 0);

    printf("clicks land where the archetype paints\n");
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

    printf("\nwhat lights up can be clicked\n");
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
        const shell_layout *L = shell_layout_by_id(ids[i]);
        if (verbose) printf("%s\n", ids[i]);
        if (affordances(L, ids[i], verbose)) fail = 1;
    }

    printf("\n%s\n", fail ? "FAIL" :
           "every archetype hit-tests where it paints, and nothing highlights "
           "that cannot be clicked");
    return fail;
}
