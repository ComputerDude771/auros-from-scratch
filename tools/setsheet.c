/* setsheet.c — every screen of Settings, in one picture.
 *
 * The same argument as tools/netsheet.c. A settings panel cannot be
 * judged one screen at a time: what decides whether it is usable is
 * whether the pages hold together -- whether the way back is in the
 * same place, whether a slider and the row above it line up, whether
 * the thing that is currently in force is obvious on every list.
 *
 * And it cannot be judged by running it, because it draws a laptop's
 * battery, a laptop's backlight and a sound server, and a build host
 * has none of the three. So the machines are made out of directories
 * (see tools/powertest.c) and the panel is rendered against them.
 *
 * It reaches the panel's private state by including settings.c rather
 * than adding a way in for it: a door cut into shipping code so a
 * picture can be taken is a door that is there on the machine too.
 *
 *   cc -O2 -std=gnu11 -o /tmp/setsheet tools/setsheet.c \
 *      src/aurshell/power.c src/aurshell/run.c src/aurshell/foot.c \
 *      src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
 *      src/aurshell/layouts/[*].c src/common/theme.c src/common/font.c \
 *      src/common/png.c -lm
 *   /tmp/setsheet /tmp/nocturne.conf /tmp/set.png 1024 600 1.0
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aurshell/settings.c"
#include "../src/aurshell/foot.h"
#include "../src/common/png.h"

#define COLS 2

static const struct { int page; const char *cap; } SHOTS[] = {
    { SET_PAGE_MAIN,  "what she can change" },
    { SET_PAGE_SHELL, "how this computer works" },
    { SET_PAGE_TIME,  "where she is" },
    { SET_PAGE_LOOK,  "how it looks" },
};
#define NSHOTS ((int)(sizeof SHOTS / sizeof SHOTS[0]))

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: setsheet <shell.conf> <out.png> "
                        "[w h [text-scale]]\n");
        return 2;
    }
    int tw = argc > 3 ? atoi(argv[3]) : 1024;
    int th = argc > 4 ? atoi(argv[4]) : 600;
    float scale = argc > 5 ? (float)atof(argv[5]) : 1.f;
    if (scale < 0.8f) scale = 0.8f;
    if (scale > 2.0f) scale = 2.0f;

    theme_t t = {0};
    if (theme_load(&t, argv[1]) < 0) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    const int pad = 18, cap = 26, rows = (NSHOTS + COLS - 1) / COLS;
    const int sw = COLS * tw + (COLS + 1) * pad;
    const int sh = rows * (th + cap) + (rows + 1) * pad;
    surface *sheet = surface_new(sw, sh);
    if (!sheet) return 1;
    uint32_t page = theme_color(&t, "col_bg_alt", 0x10151F);
    for (int i = 0; i < sw * sh; i++) sheet->px[i] = 0xFF000000u | page;

    shell_ctx c;
    memset(&c, 0, sizeof c);
    shell_theme_load(&c, &t);
    c.allow_settings = c.allow_network = c.allow_theme_change = 1;
    c.text_scale = scale;
    c.foot_hover = -1;
    c.settings_open = 1;
    snprintf(c.shell_name, sizeof c.shell_name, "%s", "Everything in a row");
    snprintf(c.layout_id, sizeof c.layout_id, "%s", "rail");
    c.screen_w = tw;
    c.screen_h = th - foot_height(&c);

    shell_fonts f;
    shell_fonts_load(&f, &c);
    font *label = f.tiny ? f.tiny : f.small;
    uint32_t ink = theme_color(&t, "col_fg", 0xD4DCEA);

    for (int k = 0; k < NSHOTS; k++) {
        settings_opened(&c);
        S.page = SHOTS[k].page;
        if (S.page != SET_PAGE_MAIN) fill_page(&c);
        S.sel = (S.page == SET_PAGE_MAIN) ? -1 : 1;

        surface *tile = surface_new(tw, th);
        if (!tile) break;
        for (int i = 0; i < tw * th; i++)
            tile->px[i] = 0xFF000000u | theme_color(&t, "col_bg", 0x0B0E14);
        settings_paint(&c, tile, &f);
        foot_paint(&c, tile, &f);

        int col = k % COLS, row = k / COLS;
        int x = pad + col * (tw + pad);
        int y = pad + row * (th + cap + pad);
        draw_copy(sheet, tile, x, y);
        if (label)
            shell_text(sheet, label, (float)x, (float)(y + th + 17),
                       SHOTS[k].cap, ink, 0.75f);
        surface_free(tile);
    }

    int rc = png_write_rgb(argv[2], sheet->px, sw, sh);
    surface_free(sheet);
    shell_fonts_free(&f);
    if (rc < 0) { fprintf(stderr, "could not write %s\n", argv[2]); return 1; }
    printf("%d screens at %dx%d, text %.0f%% -> %s\n",
           NSHOTS, tw, th, (double)(scale * 100.f), argv[2]);
    return 0;
}
