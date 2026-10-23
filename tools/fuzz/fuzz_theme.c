/* fuzz_theme.c -- theme files, the wallpaper, and the drawing they drive.
 *
 * The shell reads HER copy of shell.conf whenever one exists
 * (aurshell/main.c, PICK_HERS), and her home directory is writable by
 * every program she runs. So the theme parser is not reading a file
 * the build wrote; it is reading one anything she installed may have
 * rewritten. What matters is not only that theme_load() survives the
 * bytes, but that the NUMBERS it hands out survive the code they flow
 * into: corner radii, border widths, blur and shadow radii, opacities,
 * wallpaper intensity/grain/vignette, as int and as float, including
 * nan, inf, 1e308 and INT_MIN.
 *
 * So this harness loads the fuzzer's bytes as a theme (through the
 * real theme_load(), via a memfd), reads every key the shell and the
 * wallpaper read, renders the wallpaper at a size the input picks,
 * and pushes the same values through every draw.c primitive the
 * layouts call with them, on a small surface where any write outside
 * it is caught by AddressSanitizer.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../../src/common/theme.h"
#include "../../src/common/wall.h"
#include "../../src/aurshell/draw.h"

static int  memfd = -1;
static char memfd_path[64];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 64 * 1024) return 0;
    if (memfd < 0) {
        memfd = memfd_create("fuzz-theme", MFD_CLOEXEC);
        if (memfd < 0) abort();
        snprintf(memfd_path, sizeof memfd_path, "/proc/self/fd/%d", memfd);
    }
    if (ftruncate(memfd, 0) < 0) abort();
    if (size && pwrite(memfd, data, size, 0) != (ssize_t)size) abort();

    static theme_t t;                 /* 64 KiB: not on the stack */
    memset(&t, 0, sizeof t);
    if (theme_load(&t, memfd_path) < 0) return 0;

    /* Everything a theme is asked, the way shell_theme_load() and the
     * archetype loader ask it. */
    int radius    = theme_int(&t, "radius", 14);
    int radius_sm = theme_int(&t, "radius_sm", 8);
    int margin    = theme_int(&t, "margin", 14);
    int padding   = theme_int(&t, "padding", 14);
    int border    = theme_int(&t, "border", 2);
    int bar_h     = theme_int(&t, "bar_height", 38);
    int blur_r    = theme_int(&t, "blur_radius", 20);
    int shadow_r  = theme_int(&t, "shadow_radius", 28);
    int ws        = theme_int(&t, "workspaces", 0);
    float panel_a  = (float)theme_num(&t, "opacity_panel", 0.88);
    float shadow_a = (float)theme_num(&t, "shadow_opacity", 0.50);
    uint32_t bg = theme_color(&t, "col_bg", theme_color(&t, "bg", 0x0B0E14));
    uint32_t fg = theme_color(&t, "col_fg", theme_color(&t, "fg", 0xD4DCEA));
    (void)theme_str(&t, "brand_text", "AurOS");
    (void)theme_str(&t, "layout", "rail");
    (void)ws;

    /* The wallpaper, at a size the input picks: 1x1 and odd sizes are
     * where off-by-ones in a renderer live. */
    uint32_t hsh = 2166136261u;
    for (size_t i = 0; i < size; i++) hsh = (hsh ^ data[i]) * 16777619u;
    int ww = 1 + (int)(hsh % 40), wh = 1 + (int)((hsh >> 8) % 30);
    uint32_t *wp = malloc((size_t)ww * (size_t)wh * sizeof *wp);
    if (wp) { wall_render(wp, ww, wh, &t); free(wp); }

    surface *s = surface_new(64, 48);
    if (!s) return 0;
    surface_fill(s, 0xFF000000u | bg);

    rect r  = { margin, bar_h, padding, radius };
    /* Negated as the layouts do (x - margin), without the harness
     * itself overflowing on INT_MIN. */
    rect r2 = { margin == INT32_MIN ? INT32_MAX : -margin,
                padding == INT32_MIN ? INT32_MAX : -padding, bar_h, border };
    draw_rect(s, r, fg, panel_a);
    draw_round_rect(s, r, corners_all((float)radius), fg, panel_a);
    draw_round_rect(s, r2, (corners){ (float)radius, (float)radius_sm, 0, (float)border }, bg, 1.0f);
    draw_round_rect_border(s, r, corners_all((float)radius_sm), (float)border, fg, shadow_a);
    draw_round_rect_gradient(s, r, corners_all((float)radius), fg, bg, panel_a, 1);
    draw_round_rect_gradient(s, r2, corners_all((float)radius_sm), bg, fg, shadow_a, 0);
    draw_hrule(s, margin, padding, bar_h, border, fg, panel_a);
    draw_vrule(s, padding, margin, radius, border, fg, panel_a);
    draw_frame(s, r, border, fg, shadow_a);
    draw_circle(s, (float)margin, (float)padding, (float)radius, fg, panel_a);
    draw_line(s, (float)margin, (float)padding, (float)bar_h, (float)radius,
              (float)border, fg, panel_a);
    draw_blend_px(s, margin, padding, fg, panel_a);
    draw_blur_region(s, r, blur_r);
    draw_blur_region(s, (rect){ 0, 0, 64, 48 }, blur_r);
    draw_round_rect_shadow(s, r, corners_all((float)radius), (float)shadow_r,
                           0x000000, shadow_a, padding);

    surface *t2 = surface_new(16, 12);
    if (t2) {
        surface_fill(t2, 0xFF000000u | fg);
        draw_copy(s, t2, margin, padding);
        draw_copy(s, t2, radius == INT32_MIN ? INT32_MAX : -radius, bar_h);
        surface_free(t2);
    }
    surface_free(s);
    return 0;
}
