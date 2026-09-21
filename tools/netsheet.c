/* netsheet.c — every screen of the wifi panel, in one picture.
 *
 * The same argument as tools/contactsheet.c, applied to the one part
 * of this product a person reaches when the machine is not yet working
 * for them. A panel is not judged one screen at a time: what decides
 * whether it is usable is whether the five screens hold together --
 * whether the way out is in the same place on all of them, whether the
 * sentence at the top changes voice halfway through, whether the row
 * she has to press is where her eye already is.
 *
 * And it cannot be judged by running it, because running it requires a
 * wifi radio, a daemon, and standing somewhere with networks in range.
 * So it renders instead: the panel's own painting code, against a real
 * theme, at a real panel size, with a house's worth of invented
 * networks in it.
 *
 * It reaches the panel's private state by including net.c rather than
 * by adding a way in for it. A door cut into shipping code so that a
 * picture can be taken is a door that is there on the machine too; a
 * translation unit that includes another one is a thing that exists
 * only here.
 *
 *   cc -O2 -std=gnu11 -o /tmp/netsheet tools/netsheet.c \
 *      src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
 *      src/aurshell/foot.c src/aurshell/layouts/[*].c \
 *      src/common/theme.c src/common/font.c src/common/png.c -lm
 *   /tmp/netsheet /tmp/nocturne.conf /tmp/net.png
 *
 * The conf is a RESOLVED shell.conf, not a .theme -- see tools/README.md.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aurshell/net.c"
#include "../src/aurshell/foot.h"
#include "../src/common/png.h"

#define COLS 3

static const struct { int page, trouble; const char *cap; } SHOTS[] = {
    { P_LIST,     0,            "what is near" },
    { P_PASSWORD, 0,            "typing the password" },
    { P_JOINING,  0,            "joining" },
    { P_JOINED,   0,            "joined" },
    { P_TROUBLE,  T_PASSWORD,   "the password did not work" },
    { P_TROUBLE,  T_NOWIFI,     "this computer has no wifi" },
};
#define NSHOTS ((int)(sizeof SHOTS / sizeof SHOTS[0]))

/* A house: hers, a neighbour's through the wall, a coffee shop down
 * the road, a phone, and one with a long name because names are long. */
static void seed(void)
{
    static char list[] =
        "*:Sky-8F2A:76:WPA2\n"
        " :BT-HomeHub-2200:52:WPA2\n"
        " :VM1234567:44:WPA2\n"
        " :Ann's iPhone:38:WPA2\n"
        " :The Coffee House Free Customer Wifi:21:\n"
        " :EE-BrightBox-9j3k:14:WPA2\n";
    static char saved_src[] = "Sky-8F2A:802-11-wireless\n";
    char buf[512];
    snprintf(buf, sizeof buf, "%s", saved_src);
    N.n_saved = net_parse_saved(buf, N.saved, NET_MAX_SAVED);
    char l[1024];
    snprintf(l, sizeof l, "%s", list);
    N.n_aps = net_parse_list(l, N.aps, NET_MAX_AP,
                             (const char (*)[NET_NAME_MAX])N.saved, N.n_saved);
    snprintf(N.pick, NET_NAME_MAX, "%s", "BT-HomeHub-2200");
    N.pick_secure = 1;
    N.have_wifi = 1;
    N.asked_devices = 1;
    snprintf(N.pw, PW_MAX, "%s", "Th1stle-Road!");
    N.pw_n = (int)strlen(N.pw);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: netsheet <shell.conf> <out.png> [w h [text-scale]]\n");
        return 2;
    }
    int tw = argc > 3 ? atoi(argv[3]) : 1024;
    int th = argc > 4 ? atoi(argv[4]) : 600;
    /* Her chosen text size. 2.0 on a 1024x600 panel is the hardest
     * case in the product and the one worth looking at most. */
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
    c.allow_settings = c.allow_network = 1;
    c.text_scale = scale;
    c.foot_hover = -1;
    c.net_open = 1;
    c.screen_w = tw;
    c.screen_h = th - foot_height(&c);

    shell_fonts f;
    shell_fonts_load(&f, &c);

    font *label = f.tiny ? f.tiny : f.small;
    uint32_t ink = theme_color(&t, "col_fg", 0xD4DCEA);

    for (int k = 0; k < NSHOTS; k++) {
        seed();
        N.page = SHOTS[k].page;
        N.trouble = SHOTS[k].trouble;
        N.hover = (SHOTS[k].page == P_LIST) ? 1 : -1;
        N.hover_act = -1;
        N.scanning = 0;
        if (SHOTS[k].trouble == T_NOWIFI) N.n_aps = 0;

        surface *tile = surface_new(tw, th);
        if (!tile) break;
        for (int i = 0; i < tw * th; i++)
            tile->px[i] = 0xFF000000u | theme_color(&t, "col_bg", 0x0B0E14);
        net_paint(&c, tile, &f);
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
