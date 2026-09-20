/* aurwall — render the active theme's wallpaper.
 *
 * Two jobs: aurshell links the renderer directly and paints straight
 * into its framebuffer, while this CLI exports a PNG for previews,
 * documentation and anything that wants a file on disk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/theme.h"
#include "../common/wall.h"
#include "../common/png.h"

static void usage(void) {
    fputs("usage: aurwall [--theme FILE] [--conf FILE] [--out FILE]\n"
          "               [--width N] [--height N] [--style NAME]\n\n"
          "  --theme FILE   a .theme source file\n"
          "  --conf  FILE   a rendered shell.conf (default /etc/auros/shell.conf)\n"
          "  --style NAME   override wall_style: aurora|mesh|waves|gradient|noise|solid\n"
          "  --out   FILE   output PNG (default /var/cache/auros/wallpaper.png)\n", stderr);
}

int main(int argc, char **argv)
{
    const char *conf = "/etc/auros/shell.conf";
    const char *themef = NULL;
    const char *out = "/var/cache/auros/wallpaper.png";
    const char *style = NULL;
    int w = 1920, h = 1080;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int last = (i + 1 >= argc);
        if      (!strcmp(a, "--theme")  && !last) themef = argv[++i];
        else if (!strcmp(a, "--conf")   && !last) conf   = argv[++i];
        else if (!strcmp(a, "--out")    && !last) out    = argv[++i];
        else if (!strcmp(a, "--style")  && !last) style  = argv[++i];
        else if (!strcmp(a, "--width")  && !last) w = atoi(argv[++i]);
        else if (!strcmp(a, "--height") && !last) h = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    if (w < 16 || h < 16 || w > 16384 || h > 16384) {
        fprintf(stderr, "aurwall: implausible size %dx%d\n", w, h);
        return 2;
    }

    theme_t t = {0};
    /* A .theme source wins over the rendered conf, so you can preview a
     * theme without applying it system-wide. */
    if (themef) {
        if (theme_load(&t, themef) < 0) {
            fprintf(stderr, "aurwall: cannot read theme '%s'\n", themef);
            return 1;
        }
    } else if (theme_load(&t, conf) < 0) {
        fprintf(stderr, "aurwall: cannot read '%s'\n", conf);
        return 1;
    }
    if (style) {
        /* theme_load's map takes the last write, so this overrides. */
        theme_t o = {0};
        snprintf(o.pairs[0].key, THEME_KEY_LEN, "wall_style");
        snprintf(o.pairs[0].val, THEME_VAL_LEN, "%s", style);
        o.n = 1;
        for (int i = 0; i < o.n; i++) {
            int found = 0;
            for (int j = 0; j < t.n; j++)
                if (!strcmp(t.pairs[j].key, o.pairs[i].key)) {
                    memcpy(t.pairs[j].val, o.pairs[i].val, THEME_VAL_LEN); found = 1; break;
                }
            if (!found && t.n < THEME_MAX_KEYS) t.pairs[t.n++] = o.pairs[i];
        }
    }

    uint32_t *px = malloc((size_t)w * h * sizeof *px);
    if (!px) { fprintf(stderr, "aurwall: out of memory for %dx%d\n", w, h); return 1; }
    wall_render(px, w, h, &t);

    if (png_write_rgb(out, px, w, h) != 0) {
        fprintf(stderr, "aurwall: failed to write '%s'\n", out);
        free(px); return 1;
    }
    free(px);
    fprintf(stderr, "aurwall: %s  %dx%d  style=%s\n",
            out, w, h, style ? style : theme_str(&t, "wall_style", "aurora"));
    return 0;
}
