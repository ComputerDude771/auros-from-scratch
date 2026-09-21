/* railpreview — render the Rail to a PNG through the real paint path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rail.h"
#include "../common/wall.h"
#include "../common/png.h"

static font *openf(const char *p, float px)
{
    font *f = p && *p ? font_load(p, px) : NULL;
    if (f) return f;
    const char *fb[] = {
        "/home/user/auros-from-scratch/work/forge/desktop/rootfs/usr/share/fonts/opentype/inter/Inter-Regular.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL };
    for (int i = 0; fb[i]; i++) { f = font_load(fb[i], px); if (f) return f; }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *themef = argc > 1 ? argv[1] : "themes/nocturne.theme";
    const char *out    = argc > 2 ? argv[2] : "/tmp/rail.png";
    int W = argc > 3 ? atoi(argv[3]) : 1600;
    int H = argc > 4 ? atoi(argv[4]) : 900;
    const char *mode = argc > 5 ? argv[5] : "home";

    theme_t t = {0};
    if (theme_load(&t, themef) < 0) { fprintf(stderr, "no theme %s\n", themef); return 1; }

    rail r;
    rail_init(&r, &t);

    const char *fs = theme_str(&t, "font_sans", "");
    font *big   = openf(fs, 26.f);
    font *mid   = openf(fs, 17.f);
    font *small = openf(fs, 13.f);

    if (!strcmp(mode, "open")) {
        rail_open(&r, "Internet", "auros.example", ICON_GLOBE, 0x7DD3C0);
        rail_open(&r, "Email",    "3 new messages", ICON_MAIL,  0x82AAFF);
        rail_focus(&r, 1, 0.f);
        tween_set(&r.slide, 1.f);
        r.hover = -1;
    } else if (!strcmp(mode, "hover")) {
        r.hover_tile = 0;
    } else if (!strcmp(mode, "many")) {
        rail_open(&r, "Internet", "auros.example",  ICON_GLOBE,  0x7DD3C0);
        rail_open(&r, "Email",    "3 new messages", ICON_MAIL,   0x82AAFF);
        rail_open(&r, "Photos",   "Last summer",    ICON_PHOTOS, 0xA78BFA);
        rail_open(&r, "My Files", "Documents",      ICON_FILES,  0xF2B880);
        rail_focus(&r, 2, 0.f);
        tween_set(&r.slide, 2.f);
    }

    surface *s = surface_new(W, H);
    surface *wall = surface_new(W, H);
    uint32_t *tmp = malloc((size_t)W * H * sizeof *tmp);
    wall_render(tmp, W, H, &t);
    for (int i = 0; i < W*H; i++) wall->px[i] = 0xFF000000u | tmp[i];
    free(tmp);

    rail_paint(&r, s, big, mid, small, wall);

    uint32_t *o = malloc((size_t)W * H * sizeof *o);
    for (int i = 0; i < W*H; i++) o[i] = s->px[i] & 0xFFFFFFu;
    int rc = png_write_rgb(out, o, W, H);
    free(o); surface_free(s); surface_free(wall);
    fprintf(stderr, "%s  %dx%d  %s  theme=%s\n", out, W, H, mode,
            theme_str(&t, "theme_name", "?"));
    return rc;
}
