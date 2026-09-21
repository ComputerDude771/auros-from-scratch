/* Can the person this is for actually read it?
 *
 * The audience is someone whose ten-year-old laptop got too slow. That
 * means an aging TN panel with a washed-out gamma curve, often at an
 * angle, often in a bright room, and often eyes that are sixty years
 * old. A palette that reads beautifully on the designer's monitor can
 * be genuinely unusable there, and no amount of taste makes up for it.
 *
 * So every theme is checked against WCAG 2.1 relative-luminance
 * contrast, which is the only widely agreed numeric answer to "is this
 * legible". Text that carries meaning must clear 4.5:1. Large text and
 * non-text things you have to be able to FIND -- borders, focus rings,
 * the accent -- must clear 3:1.
 *
 * This is a floor, not a target. Clearing it does not make a design
 * good; failing it makes a design unusable, which is worse than ugly.
 *
 *   cc -O2 -std=gnu11 -o /tmp/contrast tools/contrast.c \
 *      src/common/theme.c -lm
 *   /tmp/contrast /tmp/nocturne.conf
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../src/common/theme.h"

static double chan(double c)
{
    c /= 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double lum(uint32_t c)
{
    return 0.2126 * chan((double)((c >> 16) & 0xFF))
         + 0.7152 * chan((double)((c >>  8) & 0xFF))
         + 0.0722 * chan((double)( c        & 0xFF));
}

static double ratio(uint32_t a, uint32_t b)
{
    double la = lum(a), lb = lum(b);
    if (la < lb) { double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

/* REQUIRED failures are unusable. ADVISORY ones depend on the design:
 * WCAG exempts a disabled control from contrast requirements, because
 * looking unavailable is the point, and a divider between two regions
 * that are already clearly separated is decoration.
 *
 * But that second one flips the moment a design replaces shadows with
 * hairlines -- then the rule IS the structure, and an invisible rule is
 * an invisible structure. Every current theme sits near 1.4:1 there.
 * Run with --strict to promote the advisories, which any direction
 * built on hairlines must pass. */
enum { REQUIRED, ADVISORY };
typedef struct { const char *fg, *bg, *what; double need; int sev; } pair;

/* Every one of these is a thing a person has to be able to see. The
 * threshold says which kind: 4.5 for text that carries meaning, 3.0 for
 * large text and for anything you merely have to be able to locate. */
static const pair PAIRS[] = {
  { "col_fg",      "col_bg",      "body text on the desktop",        4.5, REQUIRED },
  { "col_fg",      "col_surface", "body text on a card",             4.5, REQUIRED },
  { "col_fg_hi",   "col_surface", "a heading on a card",             4.5, REQUIRED },
  { "col_fg",      "col_bg_alt",  "text in the bar",                 4.5, REQUIRED },
  { "col_subtle",  "col_surface", "the hint under an app name",      4.5, REQUIRED },
  { "col_subtle",  "col_bg",      "secondary text on the desktop",   4.5, REQUIRED },
  { "col_accent",  "col_bg",      "the accent against the desktop",  3.0, REQUIRED },
  { "col_accent",  "col_surface", "the accent on a card",            3.0, REQUIRED },
  { "col_err",     "col_surface", "an error",                        4.5, REQUIRED },
  { "col_ok",      "col_surface", "a confirmation",                  3.0, REQUIRED },
  { "col_warn",    "col_surface", "a warning",                       3.0, REQUIRED },
  { "col_muted",   "col_surface", "text that is deliberately quiet", 3.0, ADVISORY },
  { "col_overlay", "col_surface", "a divider or border",             3.0, ADVISORY },
  { "col_overlay", "col_bg",      "a divider on the desktop",        3.0, ADVISORY },
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: contrast <shell.conf> [...]\n");
        return 2;
    }
    int fail = 0, strict = 0;
    for (int a = 1; a < argc; a++) if (!strcmp(argv[a], "--strict")) strict = 1;

    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--strict")) continue;
        theme_t t = {0};
        if (theme_load(&t, argv[a]) < 0) {
            fprintf(stderr, "cannot read %s\n", argv[a]);
            return 1;
        }
        printf("%s  (%s)%s\n", theme_str(&t, "theme_name", "?"), argv[a],
               strict ? "  [strict]" : "");
        int bad = 0, warned = 0;
        for (unsigned i = 0; i < sizeof PAIRS / sizeof *PAIRS; i++) {
            uint32_t fg = theme_color(&t, PAIRS[i].fg, 0);
            uint32_t bg = theme_color(&t, PAIRS[i].bg, 0);
            double r = ratio(fg, bg);
            int ok = r >= PAIRS[i].need;
            int hard = (PAIRS[i].sev == REQUIRED) || strict;
            const char *tag = ok ? "ok" : (hard ? "FAIL" : "warn");
            printf("  %-4s %5.2f:1  (needs %.1f)  %s\n", tag, r, PAIRS[i].need,
                   PAIRS[i].what);
            if (ok) continue;
            if (hard) { bad++; fail = 1; } else warned++;
        }
        if (bad)          printf("  UNREADABLE IN PLACES\n\n");
        else if (warned)  printf("  legible, but %d advisory below target "
                                 "— run --strict if the design leans on rules\n\n", warned);
        else              printf("  legible throughout\n\n");
    }
    return fail;
}
