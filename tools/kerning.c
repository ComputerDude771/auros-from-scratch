/* How much kerning does the engine actually apply, per font?
 *
 * This exists because the answer was silently "none" for most modern
 * fonts. src/common/font.c read only the legacy `kern` table, and
 * almost every typeface drawn this century ships its kerning solely in
 * GPOS -- Inter, IBM Plex, Charis SIL, Alegreya Sans and the whole URW
 * base35 set among them. At 14px nobody notices. In a 36px headline
 * "Ta", "Wo" and "P." visibly fall apart, and badly spaced display type
 * is itself a thing that makes software look machine-made.
 *
 * It measures the real thing: width("AV") minus width("A") minus
 * width("V"), through the same font_text_width() the shell uses. A
 * monospace or symbol font reporting 0.00 is correct and expected.
 *
 *   cc -O2 -std=gnu11 -I src/common -o /tmp/kerning tools/kerning.c \
 *      src/common/font.c -lm
 *   /tmp/kerning 36 /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"

static const char *PAIRS[] = { "AV","Ta","Wo","P.","Yo","LT","F,","r.","AW","To" };
#define NPAIRS ((int)(sizeof PAIRS / sizeof *PAIRS))

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: kerning <px> <font.ttf> [more fonts...]\n");
        return 2;
    }
    float px = (float)atof(argv[1]);
    if (px < 1.f) px = 36.f;

    for (int a = 2; a < argc; a++) {
        font *f = font_load(argv[a], px);
        const char *name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];
        if (!f) { printf("  %-42s  could not load\n", name); continue; }

        float total = 0;
        int kerned = 0;
        for (int i = 0; i < NPAIRS; i++) {
            char l[2] = { PAIRS[i][0], 0 }, r[2] = { PAIRS[i][1], 0 };
            float k = font_text_width(f, PAIRS[i])
                    - font_text_width(f, l) - font_text_width(f, r);
            total += k;
            if (k < -0.01f || k > 0.01f) kerned++;
        }
        printf("  %-42s %7.2f px total, %2d of %d pairs kerned\n",
               name, total, kerned, NPAIRS);
        font_free(f);
    }
    return 0;
}
