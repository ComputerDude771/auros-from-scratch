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
 * buttons, through foot_buttons(). Archetype internals -- a dock cell,
 * a window close button, a rail row -- do not publish theirs, so they
 * are not measured here and this harness says so rather than implying
 * coverage it does not have. Exposing that geometry is how they get
 * covered, and the reason to expose it is this file.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "../src/aurshell/shell.h"
#include "../src/aurshell/foot.h"

#define FLOOR 44

static int fail = 0, checked = 0;

static void measure(const char *what, rect r, int sw, int sh)
{
    int shorter = r.w < r.h ? r.w : r.h;
    checked++;
    printf("    %-34s %4dx%-4d at %dx%d", what, r.w, r.h, sw, sh);
    if (shorter < FLOOR) { printf("   FAIL (%dpx side)\n", shorter); fail++; }
    else printf("   ok\n");
}

/* Both ends of the range she can choose, because the band's height
 * follows the type and a control that is fine at one size can be
 * squeezed out of the rule at another. */
static const float SCALES[] = { 0.80f, 1.00f, 1.40f, 2.00f };

static void band(const char *label, int kiosk, int allow_settings)
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
        }
}

int main(void)
{
    printf("is everything she has to press big enough to press?\n");
    band("an ordinary machine", 0, 1);
    band("settings locked down", 0, 0);
    band("a kiosk", 1, 0);

    printf("\n");
    if (fail) {
        printf("%d target%s below the %dpx floor. See docs/EASY.md rule 4.\n",
               fail, fail == 1 ? " is" : "s are", FLOOR);
        return 1;
    }
    printf("%d measured, all at least %dpx on their shorter side\n", checked, FLOOR);
    printf("\nNot measured: archetype internals -- dock cells, window buttons,\n");
    printf("rail rows. They do not publish their geometry, so this cannot see\n");
    printf("them. Exposing it is how they get covered.\n");
    return 0;
}
