/* targets.c — is everything she has to press big enough to press?
 *
 * docs/EASY.md rule 4: every interactive element is at least 44 pixels
 * on its shorter side AT 1024x600, the bottom of the range, because
 * that is where the old machines are. A target measured at 1920x1080
 * and allowed to shrink with the panel is a target that fails exactly
 * where it matters.
 *
 * The rule caught its own author within an hour of being written. The
 * always-present band at the bottom of the screen -- the one piece of
 * furniture in this product that exists specifically for a person whose
 * hands are not steady -- was sized at 46 pixels tall and then had
 * padding taken out of it, leaving 27-pixel buttons. Nobody had to be
 * careless for that to happen; the two numbers were three lines apart.
 *
 * WHAT THIS CAN AND CANNOT SEE
 *
 * It measures the controls that publish their geometry: the band's
 * buttons, through foot_buttons(), and every screen of the wifi panel,
 * through net_targets(). Archetype internals -- a dock cell, a window
 * close button, a rail row -- do not publish theirs, so they are not
 * measured here and this harness says so rather than implying coverage
 * it does not have. Exposing that geometry is how they get covered,
 * and the reason to expose it is this file.
 *
 * The wifi panel is measured the way it is BECAUSE of this file. Its
 * layout takes a view -- which screen, how many networks -- rather
 * than reading the live state, so every one of its five screens can be
 * asked about here without a radio, a daemon or a person pressing
 * things. A geometry function that can only be asked about the state
 * the machine happens to be in is a geometry function nothing can
 * check.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "../src/aurshell/shell.h"
#include "../src/aurshell/foot.h"
#include "../src/aurshell/net.h"
#include "../src/aurshell/settings.h"
#include "../src/aurshell/bt.h"

#define FLOOR 44

static int fail = 0, checked = 0;
/* The band has a handful of buttons and every one is worth reading.
 * The wifi panel is five screens x four text sizes x three panel sizes
 * x how many networks are in range, which is thousands of rectangles
 * and nobody reads thousands of lines of "ok" -- so that sweep prints
 * its failures and a count, and nothing else. A harness whose output
 * nobody reads is a harness nobody runs. */
static int quiet = 0;

static void measure(const char *what, rect r, int sw, int sh)
{
    int shorter = r.w < r.h ? r.w : r.h;
    checked++;
    if (shorter < FLOOR) {
        printf("    %-38s %4dx%-4d at %dx%d   FAIL (%dpx side)\n",
               what, r.w, r.h, sw, sh, shorter);
        fail++;
        return;
    }
    if (!quiet)
        printf("    %-38s %4dx%-4d at %dx%d   ok\n", what, r.w, r.h, sw, sh);
}

/* Both ends of the range she can choose, because the band's height
 * follows the type and a control that is fine at one size can be
 * squeezed out of the rule at another. */
static const float SCALES[] = { 0.80f, 1.00f, 1.40f, 2.00f };

static void band(const char *label, int kiosk, int allow_settings,
                 int allow_network)
{
    static const struct { int w, h; } RES[] = {
        { 1024, 600 },            /* the floor the rule is written for */
        { 1366, 768 },
        { 1920, 1080 },
    };
    printf("  %s\n", label);
    for (size_t r = 0; r < sizeof RES / sizeof RES[0]; r++)
        for (size_t k = 0; k < sizeof SCALES / sizeof SCALES[0]; k++) {
            shell_ctx c; memset(&c, 0, sizeof c);
            theme_t t = {0};
            shell_theme_load(&c, &t);
            c.kiosk = kiosk;
            c.allow_settings = allow_settings;
            c.allow_network = allow_network;
            c.text_scale = SCALES[k];
            c.foot_hover = -1;
            c.screen_w = RES[r].w;
            c.screen_h = RES[r].h - foot_height(&c);

            rect b[FOOT_MAX]; int which[FOOT_MAX];
            int n = foot_buttons(&c, RES[r].w, RES[r].h, b, which);
            for (int i = 0; i < n; i++) {
                char nm[64];
                snprintf(nm, sizeof nm, "band button %d (text %.0f%%)",
                         i + 1, (double)(SCALES[k] * 100.f));
                measure(nm, b[i], RES[r].w, RES[r].h);
            }
            /* And they must not overlap each other, which is the other
             * way a row of controls becomes unpressable. */
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++) {
                    int ox = !(b[i].x + b[i].w <= b[j].x || b[j].x + b[j].w <= b[i].x);
                    int oy = !(b[i].y + b[i].h <= b[j].y || b[j].y + b[j].h <= b[i].y);
                    if (ox && oy) {
                        printf("    FAIL band buttons %d and %d overlap at %dx%d\n",
                               i + 1, j + 1, RES[r].w, RES[r].h);
                        fail++;
                    }
                }
            /* And they must be on the screen. */
            for (int i = 0; i < n; i++)
                if (b[i].x < 0 || b[i].y < 0 ||
                    b[i].x + b[i].w > RES[r].w || b[i].y + b[i].h > RES[r].h) {
                    printf("    FAIL band button %d is off the screen at %dx%d\n",
                           i + 1, RES[r].w, RES[r].h);
                    fail++;
                }

            /* And the three things Turn off asks before it does
             * anything. That screen is reached by a person who may
             * have pressed the wrong thing, so the way back off it has
             * to be as hittable as the way on. */
            rect p[FOOT_POWER_MAX]; int pw[FOOT_POWER_MAX];
            int pn = foot_power_buttons(&c, RES[r].w, RES[r].h, p, pw);
            for (int i = 0; i < pn; i++) {
                char nm[64];
                snprintf(nm, sizeof nm, "power choice %d (text %.0f%%)",
                         i + 1, (double)(SCALES[k] * 100.f));
                measure(nm, p[i], RES[r].w, RES[r].h);
            }
            for (int i = 0; i < pn; i++) {
                for (int j = i + 1; j < pn; j++) {
                    int ox = !(p[i].x + p[i].w <= p[j].x ||
                               p[j].x + p[j].w <= p[i].x);
                    int oy = !(p[i].y + p[i].h <= p[j].y ||
                               p[j].y + p[j].h <= p[i].y);
                    if (ox && oy) {
                        printf("    FAIL power choices %d and %d overlap "
                               "at %dx%d text %.0f%%\n", i + 1, j + 1,
                               RES[r].w, RES[r].h,
                               (double)(SCALES[k] * 100.f));
                        fail++;
                    }
                }
                if (p[i].x < 0 || p[i].y < 0 ||
                    p[i].x + p[i].w > RES[r].w ||
                    p[i].y + p[i].h > c.screen_h) {
                    printf("    FAIL power choice %d is outside the screen "
                           "at %dx%d text %.0f%%\n", i + 1, RES[r].w,
                           RES[r].h, (double)(SCALES[k] * 100.f));
                    fail++;
                }
            }
        }
}

/* Every screen of the wifi panel, at every size she can choose, on
 * every panel this product supports. The interesting ones are the
 * extremes in both directions: the list at the largest text (rows get
 * tall, so few fit, and the paging button appears) and at the
 * smallest, and the trouble screens, which are where a person already
 * having a bad time is asked to press something. */
static void wifi(const char *label, int allow_settings)
{
    static const struct { int w, h; } RES[] = {
        { 1024, 600 }, { 1366, 768 }, { 1920, 1080 },
    };
    static const char *PAGE[P_N] = {
        "the list", "typing the password", "joining", "joined", "trouble"
    };
    printf("  %s\n", label);
    for (size_t r = 0; r < sizeof RES / sizeof RES[0]; r++)
        for (size_t k = 0; k < sizeof SCALES / sizeof SCALES[0]; k++)
            for (int page = 0; page < P_N; page++)
                for (int tr = 0; tr < (page == P_TROUBLE ? T_N : 1); tr++)
                    /* 0 networks is the empty list, 3 is a house, 40 is
                     * a block of flats -- more than the panel keeps, so
                     * it also exercises the paging. */
                    for (int naps = 0; naps <= 40; naps += (naps < 3 ? 3 : 37)) {
                        shell_ctx c; memset(&c, 0, sizeof c);
                        theme_t t = {0};
                        shell_theme_load(&c, &t);
                        c.allow_settings = allow_settings;
                        c.allow_network = 1;
                        c.text_scale = SCALES[k];
                        c.screen_w = RES[r].w;
                        c.screen_h = RES[r].h - foot_height(&c);

                        net_view v = { page, tr, naps, 0 };
                        rect b[64];
                        int n = net_targets(&c, c.screen_w, c.screen_h,
                                            &v, b, 64);
                        for (int i = 0; i < n; i++) {
                            char nm[80];
                            snprintf(nm, sizeof nm, "%s, %d near, text %.0f%%",
                                     PAGE[page], naps,
                                     (double)(SCALES[k] * 100.f));
                            measure(nm, b[i], RES[r].w, RES[r].h);
                        }
                        /* And nothing overlaps, and nothing is off the
                         * screen -- the other two ways a row of
                         * controls becomes unpressable. */
                        for (int i = 0; i < n; i++) {
                            for (int j = i + 1; j < n; j++) {
                                int ox = !(b[i].x + b[i].w <= b[j].x ||
                                           b[j].x + b[j].w <= b[i].x);
                                int oy = !(b[i].y + b[i].h <= b[j].y ||
                                           b[j].y + b[j].h <= b[i].y);
                                if (ox && oy) {
                                    printf("    FAIL %s: targets %d and %d "
                                           "overlap at %dx%d\n", PAGE[page],
                                           i + 1, j + 1, RES[r].w, RES[r].h);
                                    fail++;
                                }
                            }
                            if (b[i].x < 0 || b[i].y < 0 ||
                                b[i].x + b[i].w > c.screen_w ||
                                b[i].y + b[i].h > c.screen_h) {
                                printf("    FAIL %s: target %d is outside the "
                                       "panel at %dx%d\n", PAGE[page], i + 1,
                                       RES[r].w, RES[r].h);
                                fail++;
                            }
                        }
                    }
}

/* Every screen of Settings, on every machine it can be shown on: with
 * a battery and without, with a backlight and without, with sound and
 * without -- because which rows exist depends on what the machine has,
 * and a row that only appears on a laptop is a row only a laptop can
 * prove is big enough. */
static void settings(const char *label)
{
    static const struct { int w, h; } RES[] = {
        { 1024, 600 }, { 1366, 768 }, { 1920, 1080 },
    };
    printf("  %s\n", label);
    for (size_t r = 0; r < sizeof RES / sizeof RES[0]; r++)
     for (size_t k = 0; k < sizeof SCALES / sizeof SCALES[0]; k++)
      for (int page = 0; page < SET_PAGE_N; page++)
       for (int hw = 0; hw < 8; hw++)        /* battery/backlight/sound */
        for (int first = 0; first < 2; first++) {
            shell_ctx c; memset(&c, 0, sizeof c);
            theme_t t = {0};
            shell_theme_load(&c, &t);
            c.allow_settings = c.allow_network = c.allow_theme_change = 1;
            c.text_scale = SCALES[k];
            c.screen_w = RES[r].w;
            c.screen_h = RES[r].h - foot_height(&c);

            set_view v;
            memset(&v, 0, sizeof v);
            v.page      = page;
            v.battery   = (hw >> 0) & 1;
            v.backlight = (hw >> 1) & 1;
            v.sound     = (hw >> 2) & 1;
            /* A list page with forty places on it, and the same page
             * scrolled -- the paged view has its own geometry and the
             * wifi panel's harness never measured one. */
            v.n_rows    = (page == SET_PAGE_MAIN) ? 0 : 40;
            v.first_row = first ? 20 : 0;

            rect b[96];
            int n = settings_targets(&c, c.screen_w, c.screen_h, &v, b, 96);
            for (int i = 0; i < n; i++) {
                char nm[80];
                snprintf(nm, sizeof nm, "settings page %d, text %.0f%%",
                         page, (double)(SCALES[k] * 100.f));
                measure(nm, b[i], RES[r].w, RES[r].h);
            }
            for (int i = 0; i < n; i++) {
                for (int j = i + 1; j < n; j++) {
                    int ox = !(b[i].x + b[i].w <= b[j].x ||
                               b[j].x + b[j].w <= b[i].x);
                    int oy = !(b[i].y + b[i].h <= b[j].y ||
                               b[j].y + b[j].h <= b[i].y);
                    if (ox && oy) {
                        printf("    FAIL settings page %d: targets %d and %d "
                               "overlap at %dx%d text %.0f%%\n", page,
                               i + 1, j + 1, RES[r].w, RES[r].h,
                               (double)(SCALES[k] * 100.f));
                        fail++;
                    }
                }
                if (b[i].x < 0 || b[i].y < 0 ||
                    b[i].x + b[i].w > c.screen_w ||
                    b[i].y + b[i].h > c.screen_h) {
                    printf("    FAIL settings page %d: target %d is outside "
                           "the panel at %dx%d\n", page, i + 1,
                           RES[r].w, RES[r].h);
                    fail++;
                }
            }
        }
}

int main(void)
{
    printf("is everything she has to press big enough to press?\n");
    band("an ordinary machine", 0, 1, 1);
    band("settings locked down", 0, 0, 1);
    band("the network pinned as well", 0, 0, 0);
    band("a kiosk", 1, 0, 0);

    printf("\nand the headphones panel, on every screen it has\n");
    {
        int before = checked;
        quiet = 1;
        static const struct { int w, h; } RES[] = {
            { 1024, 600 }, { 1366, 768 }, { 1920, 1080 },
        };
        for (size_t r = 0; r < sizeof RES / sizeof RES[0]; r++)
         for (size_t k = 0; k < sizeof SCALES / sizeof SCALES[0]; k++)
          for (int page = 0; page < BT_PAGE_N; page++)
           for (int tr = 0; tr < (page == BT_TROUBLE ? BTT_N : 1); tr++)
            for (int nd = 0; nd <= 24; nd += (nd < 3 ? 3 : 21)) {
                shell_ctx c; memset(&c, 0, sizeof c);
                theme_t t = {0};
                shell_theme_load(&c, &t);
                c.allow_settings = 1;
                c.text_scale = SCALES[k];
                c.screen_w = RES[r].w;
                c.screen_h = RES[r].h - foot_height(&c);
                bt_view v = { page, tr, nd, 0 };
                rect b[64];
                int n = bt_targets(&c, c.screen_w, c.screen_h, &v, b, 64);
                for (int i = 0; i < n; i++) {
                    char nm[72];
                    snprintf(nm, sizeof nm, "bt page %d, %d near, text %.0f%%",
                             page, nd, (double)(SCALES[k] * 100.f));
                    measure(nm, b[i], RES[r].w, RES[r].h);
                }
                for (int i = 0; i < n; i++) {
                    for (int j = i + 1; j < n; j++) {
                        int ox = !(b[i].x + b[i].w <= b[j].x ||
                                   b[j].x + b[j].w <= b[i].x);
                        int oy = !(b[i].y + b[i].h <= b[j].y ||
                                   b[j].y + b[j].h <= b[i].y);
                        if (ox && oy) {
                            printf("    FAIL bt page %d: targets %d and %d "
                                   "overlap at %dx%d\n", page, i + 1, j + 1,
                                   RES[r].w, RES[r].h);
                            fail++;
                        }
                    }
                    if (b[i].x < 0 || b[i].y < 0 ||
                        b[i].x + b[i].w > c.screen_w ||
                        b[i].y + b[i].h > c.screen_h) {
                        printf("    FAIL bt page %d: target %d is outside the "
                               "panel at %dx%d\n", page, i + 1,
                               RES[r].w, RES[r].h);
                        fail++;
                    }
                }
            }
        quiet = 0;
        printf("    %d measured across %d screens, every text size and\n",
               checked - before, BT_PAGE_N);
        printf("    every panel size, with nothing near and a room full\n");
    }

    printf("\nand Settings, on every screen and every machine\n");
    {
        int before = checked;
        quiet = 1;
        settings("an ordinary machine");
        quiet = 0;
        printf("    %d measured across %d pages, every text size, every\n",
               checked - before, SET_PAGE_N);
        printf("    panel size, and every combination of battery, backlight\n");
        printf("    and sound a machine can have\n");
    }

    printf("\nand the wifi panel, on every screen it has\n");
    int before = checked;
    quiet = 1;
    wifi("an ordinary machine", 1);
    wifi("settings locked down", 0);
    quiet = 0;
    printf("    %d measured across %d screens, every text size and every\n",
           checked - before, P_N);
    printf("    panel size, with nothing near, a house near and a block near\n");

    printf("\n");
    if (fail) {
        printf("%d target%s below the %dpx floor. See docs/EASY.md rule 4.\n",
               fail, fail == 1 ? " is" : "s are", FLOOR);
        return 1;
    }
    printf("%d measured, all at least %dpx on their shorter side\n", checked, FLOOR);
    printf("\nNot measured: archetype internals -- dock cells, window buttons,\n");
    printf("rail rows. They do not publish their geometry, so this cannot see\n");
    printf("them. Exposing it is how they get covered -- which is what the\n");
    printf("wifi panel did, and why it is in this list.\n");
    return 0;
}
