/* osd.c — see osd.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "osd.h"
#include "draw.h"

/* Long enough to read a number and see the bar move, short enough not
 * to sit over what she is doing. Every press restarts it. */
#define OSD_MS 1400

/* Fixed, like the band's, and for the same reason: this appears when
 * the screen has just been made darker or the sound has stopped, and
 * the control that explains what happened must not be styled by
 * whatever is underneath it. */
#define OSD_BG   0x14171Bu
#define OSD_INK  0xECEFF2u
#define OSD_DIM  0x6E7680u

static osd_kind  cur = OSD_NONE;
static int       cur_val;
static uint32_t  until_ms;

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

void osd_show(osd_kind kind, int value)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    cur = kind;
    cur_val = value;
    until_ms = now_ms() + OSD_MS;
}

int osd_visible(void)
{
    if (cur == OSD_NONE) return 0;
    /* Unsigned, so this is correct across the wrap that happens every
     * forty-nine days of uptime. */
    if ((int32_t)(now_ms() - until_ms) >= 0) { cur = OSD_NONE; return 0; }
    return 1;
}

void osd_paint(shell_ctx *c, surface *s, shell_fonts *f)
{
    if (!osd_visible()) return;

    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;
    int w = (int)(360.f * k); if (w > s->w - 40) w = s->w - 40;
    int h = (int)(96.f * k);
    int x = (s->w - w) / 2;
    /* Low, but clear of the band: over the middle of what she is
     * reading is the one place it must not be. */
    int y = s->h - h - (int)(120.f * k);
    if (y < 0) y = (s->h - h) / 2;

    rect box = { x, y, w, h };
    draw_rect(s, box, OSD_BG, 0.96f);
    draw_frame(s, box, 1, OSD_DIM, 0.8f);

    const char *label = cur == OSD_MUTED      ? "Sound off"
                      : cur == OSD_VOLUME     ? "Sound"
                      : cur == OSD_BRIGHTNESS ? "Screen"
                                              : "";
    font *ft = f->mid ? f->mid : f->small;
    int pad = (int)(18.f * k);
    if (ft)
        shell_text(s, ft, (float)(x + pad),
                   (float)y + pad + font_ascent(ft), label, OSD_INK, 1.f);

    /* The number as well as the bar. A bar alone cannot be compared
     * with the one she saw a moment ago. */
    if (ft && cur != OSD_MUTED) {
        char n[8];
        snprintf(n, sizeof n, "%d%%", cur_val);
        float nw = shell_text_w(ft, n);
        shell_text(s, ft, (float)(x + w - pad) - nw,
                   (float)y + pad + font_ascent(ft), n, OSD_INK, 0.75f);
    }

    int bh = (int)(10.f * k); if (bh < 8) bh = 8;
    int by = y + h - pad - bh;
    rect trough = { x + pad, by, w - 2 * pad, bh };
    draw_rect(s, trough, OSD_DIM, 0.35f);
    if (cur != OSD_MUTED) {
        rect fill = trough;
        fill.w = trough.w * cur_val / 100;
        if (fill.w > 0) draw_rect(s, fill, OSD_INK, 1.f);
    }
}
