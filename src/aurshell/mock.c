/* mock.c — render a still of the AurOS desktop straight to a PNG.
 *
 * The shell's look has to be reviewable without a machine to run it on,
 * so the compositing path is exercised here exactly as it is on screen:
 * same wallpaper renderer, same rasterizer, same theme file. If it
 * looks right here it looks right on the framebuffer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "../common/theme.h"
#include "../common/wall.h"
#include "../common/png.h"

static uint32_t C(const theme_t *t, const char *k, uint32_t fb) { return theme_color(t, k, fb); }

int main(int argc, char **argv)
{
    const char *themef = argc > 1 ? argv[1] : "themes/nocturne.theme";
    const char *out    = argc > 2 ? argv[2] : "/tmp/auros-desktop.png";
    int W = argc > 3 ? atoi(argv[3]) : 1600;
    int H = argc > 4 ? atoi(argv[4]) : 900;
    /* "hero" drops the tiled windows so the wallpaper and the frosted
     * panels are actually visible. The tiled layout is what the desktop
     * looks like in use; this is what it looks like when you arrive. */
    int hero = (argc > 5 && strcmp(argv[5], "hero") == 0);

    theme_t t = {0};
    if (theme_load(&t, themef) < 0) { fprintf(stderr, "cannot read %s\n", themef); return 1; }

    /* Theme-driven geometry. Nothing below is a magic number. */
    int   BAR    = theme_int(&t, "bar_height", 38);
    int   RAD    = theme_int(&t, "radius", 14);
    int   RADSM  = theme_int(&t, "radius_sm", 8);
    int   GAP    = theme_int(&t, "gap", 12);
    int   MARGIN = theme_int(&t, "margin", 14);
    int   PAD    = theme_int(&t, "padding", 14);
    int   BORDER = theme_int(&t, "border", 2);
    float PANEL_A = (float)theme_num(&t, "opacity_panel", 0.88);
    float SHADOW_A = (float)theme_num(&t, "shadow_opacity", 0.50);
    int   SHADOW_R = theme_int(&t, "shadow_radius", 28);
    int   BLUR_R = theme_int(&t, "blur_radius", 20);

    uint32_t bg        = C(&t,"bg",0x0B0E14),      bg_alt  = C(&t,"bg_alt",0x10151F);
    uint32_t surf      = C(&t,"surface",0x161C28), surf_hi = C(&t,"surface_hi",0x1F2735);
    uint32_t overlay   = C(&t,"overlay",0x2B3542), muted   = C(&t,"muted",0x55606E);
    uint32_t subtle    = C(&t,"subtle",0x8793A4),  fg      = C(&t,"fg",0xD4DCEA);
    uint32_t fg_hi     = C(&t,"fg_hi",0xF3F7FD);
    uint32_t accent    = C(&t,"accent",0x7DD3C0),  accent2 = C(&t,"accent_alt",0xA78BFA);
    uint32_t warm      = C(&t,"accent_warm",0xF2B880);

    surface *s = surface_new(W, H);
    if (!s) return 1;

    /* 1. Wallpaper, generated from the same theme. */
    uint32_t *wp = malloc((size_t)W * H * sizeof *wp);
    if (!wp) return 1;
    wall_render(wp, W, H, &t);
    for (int i = 0; i < W*H; i++) s->px[i] = 0xFF000000u | wp[i];
    free(wp);

    corners RC = corners_all((float)RAD);
    corners RS = corners_all((float)RADSM);

  if (!hero) {
    /* 2. A tiled window. Blur what is behind it first, so the
     *    translucency reads as frosted glass rather than as a flat
     *    wash -- that difference is most of why this looks modern. */
    rect win = { MARGIN, BAR + MARGIN + GAP, (W - MARGIN*2) * 62 / 100,
                 H - (BAR + MARGIN + GAP) - MARGIN };
    draw_round_rect_shadow(s, win, RC, (float)SHADOW_R, 0x000000, SHADOW_A, 10);
    draw_blur_region(s, win, BLUR_R);
    draw_round_rect(s, win, RC, surf, PANEL_A);
    draw_round_rect_border(s, win, RC, (float)BORDER, accent, 0.85f);   /* focused */

    /* window titlebar */
    rect tb = { win.x, win.y, win.w, 40 };
    corners tbc = { (float)RAD, (float)RAD, 0, 0 };
    draw_round_rect(s, tb, tbc, bg_alt, 0.55f);
    draw_line(s, (float)win.x + 1, (float)(win.y + 40), (float)(win.x + win.w - 1),
              (float)(win.y + 40), 1.f, overlay, 0.9f);
    draw_circle(s, (float)(win.x + PAD + 5), (float)(win.y + 20), 5.f, accent, 1.f);
    /* terminal-ish content: rows of varying length in the ansi palette */
    uint32_t ansi[6] = { C(&t,"ansi2",0x7DD3C0), C(&t,"ansi4",0x82AAFF), C(&t,"ansi5",0xA78BFA),
                         C(&t,"ansi3",0xF2B880), C(&t,"ansi6",0x6FD8DC), C(&t,"fg",0xD4DCEA) };
    for (int i = 0; i < 16; i++) {
        int rowy = win.y + 40 + PAD + i * 22;
        if (rowy > win.y + win.h - 30) break;
        int len = 120 + ((i * 137) % (win.w - PAD*2 - 160));
        uint32_t col = (i % 5 == 0) ? ansi[i % 6] : fg;
        float al = (i % 5 == 0) ? 0.85f : 0.42f;
        rect ln = { win.x + PAD, rowy, len, 7 };
        draw_round_rect(s, ln, corners_all(3.f), col, al);
    }

    /* 3. Side panel */
    rect side = { win.x + win.w + GAP, win.y, W - MARGIN - (win.x + win.w + GAP), win.h };
    draw_round_rect_shadow(s, side, RC, (float)SHADOW_R, 0x000000, SHADOW_A * 0.8f, 10);
    draw_blur_region(s, side, BLUR_R);
    draw_round_rect(s, side, RC, surf, PANEL_A * 0.95f);
    draw_round_rect_border(s, side, RC, 1.f, overlay, 0.8f);
    for (int i = 0; i < 7; i++) {
        rect row = { side.x + PAD, side.y + PAD + i * 46, side.w - PAD*2, 38 };
        if (i == 1) draw_round_rect(s, row, RS, surf_hi, 1.f);
        draw_circle(s, (float)(row.x + 16), (float)(row.y + 19), 9.f,
                    i == 1 ? accent : muted, i == 1 ? 1.f : 0.55f);
        rect lbl = { row.x + 36, row.y + 15, (side.w - PAD*2) * (55 + (i*13)%35) / 100, 7 };
        draw_round_rect(s, lbl, corners_all(3.f), i == 1 ? fg_hi : subtle, i == 1 ? 0.9f : 0.5f);
    }

  }
    /* 4. The floating top bar. Detached from the screen edge -- the
     *    single strongest signal that this is not a Windows taskbar. */
    rect bar = { MARGIN, MARGIN, W - MARGIN*2, BAR };
    draw_round_rect_shadow(s, bar, RC, (float)SHADOW_R * 0.7f, 0x000000, SHADOW_A * 0.75f, 6);
    draw_blur_region(s, bar, BLUR_R);
    draw_round_rect(s, bar, RC, bg_alt, PANEL_A);
    draw_round_rect_border(s, bar, RC, 1.f, overlay, 0.75f);

    draw_circle(s, (float)(bar.x + PAD + 6), (float)(bar.y + BAR/2), 6.f, accent, 1.f);

    /* workspace pills: active one is a wide accent capsule */
    int wx = bar.x + PAD + 30;
    for (int i = 0; i < 5; i++) {
        int pw = (i == 1) ? 26 : 10;
        rect pill = { wx, bar.y + BAR/2 - 5, pw, 10 };
        draw_round_rect(s, pill, corners_all(5.f), i == 1 ? accent : muted,
                        i == 1 ? 1.f : 0.45f);
        wx += pw + 8;
    }
    /* right cluster: indicators + clock block */
    int rx = bar.x + bar.w - PAD;
    rect clock = { rx - 68, bar.y + BAR/2 - 6, 68, 12 };
    draw_round_rect(s, clock, corners_all(4.f), fg_hi, 0.82f);
    rx -= 68 + 18;
    for (int i = 0; i < 3; i++) {
        uint32_t c = (i == 0) ? accent : (i == 1) ? warm : subtle;
        draw_circle(s, (float)(rx - i*22), (float)(bar.y + BAR/2), 5.f, c, 0.8f);
    }

    /* 5. Command palette, centred and floating. Ctrl-Space is the
     *    primary way you do anything; there is no start menu. */
    int pw2 = 620, ph = 330;
    rect pal = { (W - pw2)/2, (H - ph)/2 - 40, pw2, ph };
    draw_round_rect_shadow(s, pal, RC, (float)SHADOW_R * 1.6f, 0x000000, SHADOW_A, 18);
    draw_blur_region(s, pal, BLUR_R + 8);
    draw_round_rect(s, pal, RC, surf, 0.94f);
    draw_round_rect_border(s, pal, RC, 1.f, overlay, 0.9f);

    rect inp = { pal.x + PAD, pal.y + PAD, pal.w - PAD*2, 44 };
    draw_round_rect(s, inp, RS, bg, 0.9f);
    draw_round_rect_border(s, inp, RS, 1.f, accent, 0.55f);
    draw_circle(s, (float)(inp.x + 20), (float)(inp.y + 22), 7.f, subtle, 0.75f);
    rect q = { inp.x + 38, inp.y + 18, 150, 8 };
    draw_round_rect(s, q, corners_all(3.f), fg_hi, 0.85f);
    /* caret */
    draw_round_rect(s, (rect){ inp.x + 38 + 158, inp.y + 13, 2, 18 }, corners_all(1.f), accent, 0.95f);

    for (int i = 0; i < 5; i++) {
        rect row = { pal.x + PAD, pal.y + PAD + 44 + 10 + i * 46, pal.w - PAD*2, 40 };
        if (i == 0) {
            draw_round_rect(s, row, RS, surf_hi, 1.f);
            rect mark = { row.x, row.y + 8, 3, 24 };
            draw_round_rect(s, mark, corners_all(1.5f), accent, 1.f);
        }
        draw_round_rect(s, (rect){ row.x + 14, row.y + 11, 18, 18 }, corners_all(5.f),
                        i == 0 ? accent : muted, i == 0 ? 0.95f : 0.5f);
        rect lbl = { row.x + 44, row.y + 12, 120 + (i*67)%230, 8 };
        draw_round_rect(s, lbl, corners_all(3.f), i == 0 ? fg_hi : fg, i == 0 ? 0.95f : 0.6f);
        rect hint = { row.x + row.w - 70, row.y + 13, 56, 7 };
        draw_round_rect(s, hint, corners_all(3.f), muted, 0.45f);
    }

    /* 6. A notification, bottom right, accent-led. */
    rect nt = { W - MARGIN - 340, H - MARGIN - 96, 340, 84 };
    draw_round_rect_shadow(s, nt, RC, (float)SHADOW_R, 0x000000, SHADOW_A, 10);
    draw_blur_region(s, nt, BLUR_R);
    draw_round_rect(s, nt, RC, surf, 0.95f);
    draw_round_rect_border(s, nt, RC, 1.f, overlay, 0.9f);
    draw_round_rect(s, (rect){ nt.x, nt.y + 16, 3, nt.h - 32 }, corners_all(1.5f), accent2, 1.f);
    draw_circle(s, (float)(nt.x + 30), (float)(nt.y + 30), 10.f, accent2, 0.9f);
    draw_round_rect(s, (rect){ nt.x + 52, nt.y + 24, 150, 8 }, corners_all(3.f), fg_hi, 0.9f);
    draw_round_rect(s, (rect){ nt.x + 52, nt.y + 44, 240, 7 }, corners_all(3.f), subtle, 0.6f);
    draw_round_rect(s, (rect){ nt.x + 52, nt.y + 60, 190, 7 }, corners_all(3.f), subtle, 0.45f);

    uint32_t *outpx = malloc((size_t)W * H * sizeof *outpx);
    for (int i = 0; i < W*H; i++) outpx[i] = s->px[i] & 0xFFFFFFu;
    int rc = png_write_rgb(out, outpx, W, H);
    free(outpx); surface_free(s);
    fprintf(stderr, "%s  %dx%d  theme=%s\n", out, W, H, theme_str(&t, "theme_name", "?"));
    return rc;
}
