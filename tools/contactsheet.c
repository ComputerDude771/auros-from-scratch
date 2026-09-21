/* One image, all six archetypes, one theme.
 *
 * A design decision is not judged one screen at a time. The thing that
 * separates a system from a look is whether it survives six different
 * interaction models, and you can only see that side by side.
 *
 *   cc -O2 -std=gnu11 -o /tmp/sheet tools/contactsheet.c \
 *      src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
 *      src/aurshell/layouts/ (every .c in it) src/common/theme.c src/common/wall.c \
 *      src/common/font.c src/common/png.c -lm
 *   /tmp/sheet <shell.conf> <out.png> [tile_w tile_h]
 *
 * The conf is a RESOLVED shell.conf (col_bg = 0x...), not a .theme --
 * see tools/README.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/aurshell/shell.h"
#include "../src/common/wall.h"
#include "../src/common/png.h"

static const char *IDS[] = { "rail", "tiles", "locked", "taskbar", "dock", "workbench" };
#define NIDS 6
#define COLS 2

static font *pick_font(float px)
{
    static const char *fb[] = {
        "/usr/share/auros/fonts/Inter.ttf",
        "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL
    };
    for (int i = 0; fb[i]; i++) { font *f = font_load(fb[i], px); if (f) return f; }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: contactsheet <shell.conf> <out.png> [tile_w tile_h]\n");
        return 2;
    }
    /* The default is a REAL panel size -- the smallest machine this
     * product targets. Rendering the tiles smaller than any real screen
     * makes every archetype look crowded and invents bugs that do not
     * exist: at 683x384 rail's hint text collides with the row below,
     * and at 1024x600 it does not. A contact sheet that lies about
     * crowding is worse than no contact sheet. */
    int tw = argc > 3 ? atoi(argv[3]) : 1024;
    int th = argc > 4 ? atoi(argv[4]) : 600;
    if (tw < 200 || th < 120) { fprintf(stderr, "tile too small\n"); return 2; }

    theme_t t = {0};
    if (theme_load(&t, argv[1]) < 0) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    const int pad = 18, cap = 26, rows = (NIDS + COLS - 1) / COLS;
    const int sheet_w = COLS * tw + (COLS + 1) * pad;
    const int sheet_h = rows * (th + cap) + (rows + 1) * pad;

    surface *sheet = surface_new(sheet_w, sheet_h);
    if (!sheet) { fprintf(stderr, "out of memory\n"); return 1; }
    uint32_t page = theme_color(&t, "col_bg_alt", 0x10151F);
    for (int i = 0; i < sheet_w * sheet_h; i++) sheet->px[i] = 0xFF000000u | page;

    /* One wallpaper, built once and shared: it is the same on every
     * tile, and it is the single most expensive thing here. */
    surface *wall = surface_new(tw, th);
    if (wall) {
        uint32_t *tmp = malloc((size_t)tw * th * sizeof *tmp);
        if (tmp) {
            wall_render(tmp, tw, th, &t);
            for (int i = 0; i < tw * th; i++) wall->px[i] = 0xFF000000u | tmp[i];
            free(tmp);
        }
    }

    font *label = pick_font(15.f);
    uint32_t ink = theme_color(&t, "col_fg", 0xD4DCEA);

    for (int k = 0; k < NIDS; k++) {
        const shell_layout *L = shell_layout_by_id(IDS[k]);
        shell_ctx c;
        memset(&c, 0, sizeof c);
        shell_theme_load(&c, &t);
        shell_seed_apps(&c);
        snprintf(c.layout_id, sizeof c.layout_id, "%s", IDS[k]);
        c.show_clock = 1;
        c.allow_install = c.allow_settings = c.allow_theme_change = c.allow_tty = 1;
        c.screen_w = tw; c.screen_h = th;
        c.hover = -1;
        /* Three things open, none of them the first: enough state for
         * every archetype to show what it actually does. */
        for (int i = 0; i < 3 && i < SHELL_MAX_WINS; i++) {
            c.wins[i].app = i % c.n_apps;
            snprintf(c.wins[i].title, sizeof c.wins[i].title, "%s", c.apps[c.wins[i].app].name);
            snprintf(c.wins[i].subtitle, sizeof c.wins[i].subtitle, "%s", c.apps[c.wins[i].app].hint);
            c.n_wins++;
        }
        c.focus = 0;
        c.mouse_x = c.mouse_y = -10000;

        if (L->init) L->init(&c);
        surface *tile = surface_new(tw, th);
        if (!tile) continue;
        /* The shared loader, not four hand-picked sizes. There are
         * seven faces now; a copy of this that fills four leaves three
         * NULL, and every element drawn in one of those silently
         * vanishes from the sheet -- which is a picture of a desktop
         * that does not exist, captioned as if it did. */
        shell_fonts f = {0};
        shell_fonts_load(&f, &c);
        L->paint(&c, tile, &f, wall);
        if (L->fini) L->fini(&c);

        int col = k % COLS, row = k / COLS;
        int x0 = pad + col * (tw + pad);
        int y0 = pad + row * (th + cap + pad) + cap;
        for (int y = 0; y < th; y++)
            memcpy(sheet->px + (size_t)(y0 + y) * sheet->stride + x0,
                   tile->px + (size_t)y * tile->stride, (size_t)tw * sizeof *tile->px);
        if (label)
            shell_text(sheet, label, (float)x0, (float)y0 - 8.f, IDS[k], ink, 0.85f);

        surface_free(tile);
        shell_fonts_free(&f);
    }

    uint32_t *out = malloc((size_t)sheet_w * sheet_h * sizeof *out);
    if (!out) { fprintf(stderr, "out of memory\n"); return 1; }
    for (size_t i = 0; i < (size_t)sheet_w * sheet_h; i++) out[i] = sheet->px[i] & 0xFFFFFFu;
    int rc = png_write_rgb(argv[2], out, sheet_w, sheet_h);
    free(out); surface_free(sheet); surface_free(wall);
    if (label) font_free(label);
    fprintf(stderr, "%s  %dx%d  six archetypes, theme %s\n",
            argv[2], sheet_w, sheet_h, theme_str(&t, "theme_name", "?"));
    return rc;
}
