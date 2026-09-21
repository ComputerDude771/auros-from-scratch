/* foot.c — see foot.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "foot.h"
#include "draw.h"
#include "anim.h"

/* ── the fixed look ─────────────────────────────────────────────────
 *
 * Deliberately not from the theme. See foot.h. These two are a 13.9:1
 * pair, which is well past the 4.5:1 text floor with room for a theme
 * author to have done something unexpected around them. */
#define FOOT_BG     0x14171Bu
#define FOOT_INK    0xECEFF2u
#define FOOT_DIM    0x9BA3ABu
#define FOOT_RULE   0x6E7680u
#define FOOT_HOT    0x23272Du

/* The BUTTONS are at least 44px on their shorter side, per docs/EASY.md
 * rule 4, so the band has to be tall enough to hold one with room to
 * breathe. The first version of this sized the band at 46 and then took
 * padding out of it, which left 27px targets in the one piece of
 * furniture that exists for a person whose hands are not steady. */
#define FOOT_TARGET 44
#define FOOT_PAD     6
#define FOOT_MIN_H  (FOOT_TARGET + 2 * FOOT_PAD)
/* And the words in it are never smaller than this, whatever she has
 * chosen, because this is the control she reaches for when everything
 * else has become unreadable. */
#define FOOT_MIN_PX 15.f

/* The range she can choose within. Below 0.8 the shell's own furniture
 * stops fitting; above 2.0 a 1024x600 panel holds almost nothing, and
 * "almost nothing, very large" is a different product. */
#define SCALE_MIN   0.80f
#define SCALE_MAX   2.00f
#define SCALE_STEP  0.15f

enum { B_HELP, B_SMALLER, B_BIGGER, B_POWER, B_N };

static const char *LABEL[B_N] = { "Help", "Smaller", "Bigger", "Turn off" };

/* ── where the buttons are ──────────────────────────────────────────
 *
 * ONE function, called by painting and by hit-testing, per
 * docs/SHELLS.md. Returns how many buttons this machine actually has --
 * a build that forbids changing settings has no size controls, and a
 * kiosk has no way to turn the machine off.
 */
int foot_buttons(const shell_ctx *c, int sw, int sh, rect *out, int *which)
{
    int h = foot_height(c);
    if (h <= 0) return 0;
    int top = sh - h;
    int pad = FOOT_PAD;

    /* Width follows the type, so the band stays proportionate when she
     * makes everything bigger. */
    int bw = (int)(96.f * (c->text_scale < 1.f ? 1.f : c->text_scale));
    if (bw < FOOT_TARGET) bw = FOOT_TARGET;
    if (bw > sw / 5) bw = sw / 5;
    int bh = h - 2 * pad;
    if (bh < FOOT_TARGET) bh = FOOT_TARGET;
    if (bh > h) bh = h;

    int n = 0;
    /* Help on the left, flush to the corner. A corner cannot be
     * overshot by a hand that is not steady. */
    out[n] = (rect){ pad, top + (h - bh) / 2, bw, bh };
    which[n] = B_HELP; n++;

    /* The rest on the right, in the order she would reach for them. */
    int x = sw - pad - bw;
    if (!c->kiosk) {
        out[n] = (rect){ x, top + (h - bh) / 2, bw, bh };
        which[n] = B_POWER; n++;
        x -= bw + 6;
    }
    if (c->allow_settings) {
        out[n] = (rect){ x, top + (h - bh) / 2, bw, bh };
        which[n] = B_BIGGER; n++;
        x -= bw + 6;
        out[n] = (rect){ x, top + (h - bh) / 2, bw, bh };
        which[n] = B_SMALLER; n++;
    }
    return n;
}

int foot_height(const shell_ctx *c)
{
    if (!c || c->no_foot) return 0;
    /* Grows with her choice. The floor is a floor, not a ceiling: the
     * person who picks the biggest text needs the exit biggest of all. */
    float px = FOOT_MIN_PX * (c->text_scale < 1.f ? 1.f : c->text_scale);
    int h = (int)(px * 2.1f) + 2 * FOOT_PAD;
    return h < FOOT_MIN_H ? FOOT_MIN_H : h;
}

int foot_help_open(const shell_ctx *c) { return c && c->help_open; }

/* ── her chosen size, kept where she can own it ─────────────────────
 *
 * Her own settings file, not the system theme: changing how big the
 * words are must not need a password, and must survive a restart. */
static void scale_path(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";
    snprintf(out, n, "%s/.config/auros", home);
    mkdir(out, 0755);
    snprintf(out, n, "%s/.config/auros/text-scale", home);
}

float foot_load_text_scale(void)
{
    char p[512]; scale_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 1.f;
    float v = 1.f;
    if (fscanf(f, "%f", &v) != 1) v = 1.f;
    fclose(f);
    if (v < SCALE_MIN) v = SCALE_MIN;
    if (v > SCALE_MAX) v = SCALE_MAX;
    return v;
}

void foot_save_text_scale(float scale)
{
    char p[512]; scale_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;                      /* not being able to remember
                                          * is not a reason to refuse */
    fprintf(f, "%.2f\n", (double)scale);
    fclose(f);
}

/* ── painting ───────────────────────────────────────────────────────*/

static void paint_button(surface *s, shell_fonts *f, rect r,
                         const char *label, int hot, float px)
{
    if (hot) draw_rect(s, r, FOOT_HOT, 1.f);
    font *ft = f->mid ? f->mid : f->small;
    if (!ft) return;
    float by = shell_baseline(ft, (float)r.y, (float)r.h);
    shell_text_elided(s, ft, (float)r.x + 12.f, by, (float)r.w - 24.f,
                      label, hot ? FOOT_INK : FOOT_DIM, 1.f);
    (void)px;
}

void foot_paint(shell_ctx *c, surface *s, shell_fonts *f)
{
    int h = foot_height(c);
    if (h <= 0) return;
    int top = s->h - h;

    /* The help panel sits above the archetype and below the band, so
     * the way out of it is never covered by it. */
    if (c->help_open) {
        rect all = { 0, 0, s->w, top };
        draw_rect(s, all, FOOT_BG, 0.94f);

        font *big = f->big ? f->big : f->mid;
        font *body = f->small ? f->small : f->mid;
        int x = s->w / 12, y = top / 6;
        if (big) {
            shell_text(s, big, (float)x, (float)y + font_ascent(big),
                       "Where you are", FOOT_INK, 1.f);
            y += (int)(font_line_height(big) * 1.6f);
        }
        if (body) {
            float lh = font_line_height(body) * 1.45f;
            static const char *LINES[] = {
                "This bar at the bottom is always here. It never goes away.",
                "",
                "Smaller and Bigger change the size of the words everywhere.",
                "If the screen has become hard to read, press Smaller.",
                "",
                "Turn off shuts the computer down properly.",
                "",
                "Press Help again to close this.",
                NULL
            };
            for (int i = 0; LINES[i]; i++) {
                if (LINES[i][0])
                    shell_text(s, body, (float)x, (float)y + font_ascent(body),
                               LINES[i], FOOT_INK, 0.92f);
                y += (int)lh;
            }
        }
    }

    rect band = { 0, top, s->w, h };
    draw_rect(s, band, FOOT_BG, 1.f);
    draw_hrule(s, 0, top, s->w, 1, FOOT_RULE, 1.f);

    rect r[B_N]; int which[B_N];
    int n = foot_buttons(c, s->w, s->h, r, which);
    float px = FOOT_MIN_PX * (c->text_scale < 1.f ? 1.f : c->text_scale);
    for (int i = 0; i < n; i++)
        paint_button(s, f, r[i], LABEL[which[i]], c->foot_hover == which[i], px);

    /* What the size is now, so pressing Smaller twice is not a guess.
     * Only when it is not the size it came at. */
    if (c->allow_settings && c->text_scale > 1.01f) {
        font *ft = f->small ? f->small : f->mid;
        if (ft) {
            char msg[48];
            snprintf(msg, sizeof msg, "Words are %d%% bigger",
                     (int)((c->text_scale - 1.f) * 100.f + 0.5f));
            shell_text(s, ft, (float)(r[0].x + r[0].w + 18),
                       shell_baseline(ft, (float)top, (float)h), msg, FOOT_DIM, 0.85f);
        }
    } else if (c->allow_settings && c->text_scale < 0.99f) {
        font *ft = f->small ? f->small : f->mid;
        if (ft)
            shell_text(s, ft, (float)(r[0].x + r[0].w + 18),
                       shell_baseline(ft, (float)top, (float)h),
                       "Words are smaller than usual", FOOT_DIM, 0.85f);
    }
}

/* ── input ──────────────────────────────────────────────────────────*/

void foot_motion(shell_ctx *c, int x, int y)
{
    c->foot_hover = -1;
    rect r[B_N]; int which[B_N];
    int n = foot_buttons(c, c->screen_w, c->screen_h + foot_height(c), r, which);
    for (int i = 0; i < n; i++)
        if (x >= r[i].x && x < r[i].x + r[i].w &&
            y >= r[i].y && y < r[i].y + r[i].h) { c->foot_hover = which[i]; return; }
}

int foot_click(shell_ctx *c, int x, int y)
{
    int h = foot_height(c);
    if (h <= 0) return 0;
    int full_h = c->screen_h + h;

    rect r[B_N]; int which[B_N];
    int n = foot_buttons(c, c->screen_w, full_h, r, which);
    for (int i = 0; i < n; i++) {
        if (x < r[i].x || x >= r[i].x + r[i].w ||
            y < r[i].y || y >= r[i].y + r[i].h) continue;
        switch (which[i]) {
        case B_HELP:
            c->help_open = !c->help_open;
            return 1;
        case B_SMALLER:
            c->text_scale -= SCALE_STEP;
            if (c->text_scale < SCALE_MIN) c->text_scale = SCALE_MIN;
            c->text_changed = 1;
            foot_save_text_scale(c->text_scale);
            return 1;
        case B_BIGGER:
            c->text_scale += SCALE_STEP;
            if (c->text_scale > SCALE_MAX) c->text_scale = SCALE_MAX;
            c->text_changed = 1;
            foot_save_text_scale(c->text_scale);
            return 1;
        case B_POWER:
            c->want_power_off = 1;
            return 1;
        }
    }

    /* A press anywhere on the help panel closes it. Asking her to find
     * the same small button again to get rid of the thing that exists
     * to help her would be a joke at her expense. */
    if (c->help_open && y < c->screen_h) { c->help_open = 0; return 1; }

    /* Inside the band but not on a button: swallowed, so a near-miss
     * does not fall through and press whatever the archetype has at the
     * bottom of its screen. */
    return y >= full_h - h;
}
