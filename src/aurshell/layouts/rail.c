/* layouts/rail.c — the "Everything in a row" archetype.
 *
 * Everything open is one horizontal row of large cards. The focused card
 * is centred; its neighbours peek in at both edges. Click a peeking card
 * and it slides to the centre. Nothing overlaps, nothing minimises,
 * nothing hides. Card 0 is Home and cannot be closed.
 *
 * The point: every other desktop splits the screen into the thing you
 * are looking at and the machinery for reaching other things. That
 * second layer is where people lose things. Here the row is both.
 */
#include "../shell.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define CARD_W_FRAC 0.62f
#define CARD_GAP    28
#define HOME_TILES  6

typedef struct { tween slide; int hover_tile; } rail_priv;

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static rail_priv *P(shell_ctx *c) { return (rail_priv *)c->priv; }

/* Cards are the Home card followed by one per open window. */
static int card_count(shell_ctx *c) { return 1 + c->n_wins; }

/* One function owns card geometry. Hit-testing and painting both call
 * it, so a click always lands on what the user sees; deriving them
 * separately is how a UI ends up off by the width of a shadow. */
static rect card_rect(shell_ctx *c, int w, int h, int index)
{
    rail_priv *p = P(c);
    int top = c->bar_h + 8 + c->margin;
    int cw  = (int)((float)w * CARD_W_FRAC);
    int ch  = h - top - c->margin - 26;
    float off = (float)index - (p ? p->slide.value : 0.f);

    float d = fabsf(off);
    float scale = 1.0f - clampf(d, 0.f, 2.f) * 0.055f;
    int sw = (int)((float)cw * scale), sh = (int)((float)ch * scale);

    rect r;
    r.x = (w - cw) / 2 + (int)(off * (float)(cw + CARD_GAP)) + (cw - sw) / 2;
    r.y = top + (ch - sh) / 2;
    r.w = sw; r.h = sh;
    return r;
}

static rect tile_rect(shell_ctx *c, rect a, int index)
{
    const int COLS = 3, ROWS = 2;
    int pad = c->padding * 2, gap = c->padding + 6;
    int gw = a.w - pad*2 - gap*(COLS-1);
    int gh = a.h - pad*2 - gap*(ROWS-1) - 96;
    int tw = gw / COLS, th = gh / ROWS;
    int col = index % COLS, row = index / COLS;
    rect t = { a.x + pad + col*(tw+gap), a.y + pad + 96 + row*(th+gap), tw, th };
    return t;
}

static void l_init(shell_ctx *c)
{
    static rail_priv priv;
    memset(&priv, 0, sizeof priv);
    priv.hover_tile = -1;
    tween_set(&priv.slide, 0.f);
    c->priv = &priv;
    c->focus = -1;          /* -1 means Home is centred */
}

static void focus_card(shell_ctx *c, int idx, float secs)
{
    rail_priv *p = P(c);
    int n = card_count(c);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    c->focus = idx - 1;     /* card 0 is Home, so window i is card i+1 */
    tween_to(&p->slide, (float)idx, secs, EASE_SPRING);
}

static void paint_home(shell_ctx *c, surface *s, rect a, shell_fonts *f,
                       float alpha, int focused)
{
    rail_priv *p = P(c);
    if (!focused) {
        /* Peeking Home gets a labelled spine, not a shrunken grid: six
         * unreadable tiles say nothing, "Home" says what a click does. */
        int vx = a.x < 0 ? 0 : a.x;
        int vw = (a.x + a.w) - vx;
        if (vw <= 0) return;
        float cx = (float)(vx + (vw < 140 ? vw/2 : 46));
        draw_circle(s, cx, (float)(a.y + 40), 15.f, c->accent, alpha * 0.18f);
        shell_icon_draw(s, ICON_WINDOW, cx, (float)(a.y + 40), 20.f, c->accent, alpha * 0.9f);
        if (vw >= 140)
            shell_text(s, f->mid, (float)(vx + 70), shell_baseline(f->mid, (float)(a.y + 26), 28.f),
                       "Home", c->fg_hi, alpha * 0.95f);
        return;
    }

    shell_text(s, f->big, (float)(a.x + c->padding*2),
               (float)(a.y + c->padding*2) + (f->big ? font_ascent(f->big) : 22.f),
               "What would you like to do?", c->fg_hi, alpha * 0.96f);
    shell_text(s, f->small, (float)(a.x + c->padding*2),
               (float)(a.y + c->padding*2) + (f->big ? font_line_height(f->big) : 34.f)
               + (f->small ? font_ascent(f->small) : 14.f) + 4.f,
               "Pick one. You can always come back here.", c->subtle, alpha * 0.75f);

    for (int i = 0; i < HOME_TILES && i < c->n_apps; i++) {
        rect t = tile_rect(c, a, i);
        int hot = (p->hover_tile == i);
        uint32_t tint = c->apps[i].tint;

        if (hot) draw_round_rect_shadow(s, t, corners_all((float)c->radius),
                                        (float)c->shadow_r * 0.6f, 0x000000,
                                        c->shadow_a * 0.55f * alpha, 5);
        draw_round_rect(s, t, corners_all((float)c->radius),
                        hot ? c->surface_hi : c->surface_c, alpha * (hot ? 1.f : 0.88f));
        draw_round_rect_border(s, t, corners_all((float)c->radius), hot ? 2.f : 1.f,
                               hot ? tint : c->overlay, alpha * (hot ? 0.95f : 0.7f));

        /* Centre icon+label+hint as a block: anchoring the icon to a
         * fraction of tile height leaves dead space underneath on tall
         * tiles, which reads as "something is missing". */
        float icx = (float)(t.x + t.w/2);
        float isz = (float)t.h * 0.30f; if (isz > 64.f) isz = 64.f;
        float lab_h  = f->mid   ? font_line_height(f->mid)   : 22.f;
        float hint_h = f->small ? font_line_height(f->small) : 18.f;
        float block  = isz*1.56f + 14.f + lab_h + 4.f + hint_h;
        float top    = (float)t.y + ((float)t.h - block) * 0.5f;
        float icy    = top + isz*0.78f;

        draw_circle(s, icx, icy, isz*0.78f, tint, alpha * (hot ? 0.20f : 0.13f));
        shell_icon_draw(s, c->apps[i].icon, icx, icy, isz, tint, alpha * (hot ? 1.f : 0.9f));

        float ly = top + isz*1.56f + 14.f + (f->mid ? font_ascent(f->mid) : 16.f);
        shell_text_centred(s, f->mid, icx, ly, c->apps[i].name, c->fg_hi, alpha * 0.97f);
        shell_text_centred(s, f->small, icx,
                           ly + lab_h - (f->mid ? font_descent(f->mid) : 4.f) + 4.f
                           + (f->small ? font_ascent(f->small) : 13.f),
                           c->apps[i].hint, c->subtle, alpha * 0.78f);
    }
}

static void paint_win(shell_ctx *c, surface *s, rect a, int wi, shell_fonts *f,
                      float alpha, int focused)
{
    const win_entry *w = &c->wins[wi];
    uint32_t tint = (w->app >= 0) ? c->apps[w->app].tint : c->accent;
    shell_icon ic = (w->app >= 0) ? c->apps[w->app].icon : ICON_WINDOW;

    int th = 52;
    rect tb = { a.x, a.y, a.w, th };
    corners tc = { (float)c->radius, (float)c->radius, 0, 0 };
    draw_round_rect(s, tb, tc, c->bg_alt, alpha * 0.55f);
    draw_line(s, (float)a.x+1, (float)(a.y+th), (float)(a.x+a.w-1), (float)(a.y+th),
              1.f, c->overlay, alpha * 0.85f);

    /* Anchor the header to the VISIBLE region: a peeking card hangs off
     * the screen, and a header at a.x lands out of view, leaving the
     * card the user is invited to click with no label at all. */
    int vx = a.x < 0 ? 0 : a.x;
    shell_icon_draw(s, ic, (float)(vx + c->padding + 14), (float)(a.y + th/2), 22.f,
                    tint, alpha * 0.95f);
    shell_text(s, f->mid, (float)(vx + c->padding + 38),
               shell_baseline(f->mid, (float)a.y, (float)th), w->title, c->fg_hi, alpha * 0.96f);

    if (focused) {
        float ccx = (float)(a.x + a.w - c->padding - 12), ccy = (float)(a.y + th/2);
        draw_circle(s, ccx, ccy, 13.f, c->overlay, alpha * 0.55f);
        draw_line(s, ccx-4.5f, ccy-4.5f, ccx+4.5f, ccy+4.5f, 1.8f, c->subtle, alpha);
        draw_line(s, ccx+4.5f, ccy-4.5f, ccx-4.5f, ccy+4.5f, 1.8f, c->subtle, alpha);
    }

    if (w->content) {
        rect body = { a.x, a.y + th, a.w, a.h - th };
        corners bc = { 0, 0, (float)c->radius, (float)c->radius };
        draw_scaled_rounded(s, w->content, body, bc, alpha);
    } else {
        float cx = (float)(a.x + a.w/2), cy = (float)(a.y + th + (a.h - th)/2);
        draw_circle(s, cx, cy - 30.f, 40.f, tint, alpha * 0.10f);
        shell_icon_draw(s, ic, cx, cy - 30.f, 46.f, tint, alpha * 0.55f);
        shell_text_centred(s, f->mid, cx, cy + 34.f, w->title, c->fg, alpha * 0.8f);
        shell_text_centred(s, f->small, cx, cy + 60.f,
                           w->subtitle[0] ? w->subtitle : "Opening…", c->muted, alpha * 0.7f);
    }
}

static void l_paint(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall)
{
    rail_priv *p = P(c);
    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else surface_fill(s, 0xFF000000u | c->bg);

    int n = card_count(c);
    for (int pass = 2; pass >= 0; pass--) {
        for (int i = 0; i < n; i++) {
            int dist = (int)fabsf((float)i - p->slide.value + 0.001f);
            if (dist != pass) continue;
            rect a = card_rect(c, s->w, s->h, i);
            if (a.x > s->w || a.x + a.w < 0) continue;

            float d = fabsf((float)i - p->slide.value);
            float alpha = clampf(1.f - d * 0.18f, 0.62f, 1.f);
            int focused = (d < 0.5f);
            uint32_t ring = (i == 0) ? c->accent
                          : (c->wins[i-1].app >= 0 ? c->apps[c->wins[i-1].app].tint : c->accent);

            draw_round_rect_shadow(s, a, corners_all((float)c->radius),
                                   (float)c->shadow_r * (focused ? 1.25f : 0.8f),
                                   0x000000, c->shadow_a * alpha, focused ? 14 : 8);
            draw_blur_region(s, a, c->blur_r);
            draw_round_rect(s, a, corners_all((float)c->radius), c->surface_c, alpha * c->panel_a);
            draw_round_rect_border(s, a, corners_all((float)c->radius),
                                   focused ? (float)c->border : 1.f,
                                   focused ? ring : c->overlay, alpha * (focused ? 0.9f : 0.6f));

            if (i == 0) paint_home(c, s, a, f, alpha, focused);
            else paint_win(c, s, a, i - 1, f, alpha, focused);
        }
    }

    /* Status strip. Deliberately NOT a taskbar: nothing here launches or
     * switches, so there is exactly one way to move around. */
    int bh = c->bar_h + 8;
    draw_rect(s, (rect){ 0, 0, s->w, bh }, c->bg, 0.55f);
    draw_line(s, 0, (float)bh, (float)s->w, (float)bh, 1.f, c->overlay, 0.5f);
    draw_circle(s, (float)(c->margin + 8), (float)(bh/2), 6.f, c->accent, 1.f);
    shell_text(s, f->small, (float)(c->margin + 24), shell_baseline(f->small, 0.f, (float)bh),
               c->brand, c->subtle, 0.9f);

    if (c->show_clock) {
        char hm[32], dt[48];
        shell_clock(hm, sizeof hm, dt, sizeof dt);
        float by = shell_baseline(f->small, 0.f, (float)bh);
        float rx = (float)(s->w - c->margin);
        shell_text(s, f->small, rx - shell_text_w(f->small, hm), by, hm, c->fg_hi, 0.95f);
        rx -= shell_text_w(f->small, hm) + 14.f;
        shell_text(s, f->small, rx - shell_text_w(f->small, dt), by, dt, c->subtle, 0.75f);
    }

    if (c->show_positions && n > 1) {
        float dw = 9.f, dg = 9.f;
        float tot = (float)n*dw + (float)(n-1)*dg;
        float x0 = (float)s->w/2.f - tot/2.f, y = (float)s->h - (float)c->margin - 8.f;
        for (int i = 0; i < n; i++) {
            float d = fabsf((float)i - p->slide.value);
            float k = clampf(1.f - d, 0.f, 1.f);
            draw_circle(s, x0 + (float)i*(dw+dg) + dw/2.f, y, 3.2f + k*1.8f,
                        (i == 0) ? c->accent : c->fg, 0.30f + k*0.65f);
        }
    }
}

/* Both sides of this copy live inside the same shell_ctx, so the
 * compiler cannot prove they do not overlap and warns about snprintf.
 * They never do -- they are different members -- but a bounded copy
 * says so plainly and costs nothing. */
static void put_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    while (i + 1 < n && src[i]) { dst[i] = src[i]; i++; }
    if (n) dst[i] = '\0';
}

/* Open a thing from the Home card.
 *
 * If it is already open, go to it rather than opening a second copy.
 * Rail's whole promise is that nothing can hide, and two cards showing
 * the same thing is a way to get lost in the one archetype that is
 * supposed to make that impossible. */
static void open_app(shell_ctx *c, int app)
{
    if (app < 0 || app >= c->n_apps) return;
    for (int i = 0; i < c->n_wins; i++)
        if (c->wins[i].app == app) { focus_card(c, i + 1, 0.36f); return; }
    if (c->n_wins >= SHELL_MAX_WINS) return;
    int i = c->n_wins++;
    c->wins[i].app = app;
    c->wins[i].minimised = 0;
    put_str(c->wins[i].title,    sizeof c->wins[i].title,    c->apps[app].name);
    put_str(c->wins[i].subtitle, sizeof c->wins[i].subtitle, c->apps[app].hint);
    focus_card(c, i + 1, 0.36f);
}

static int l_click(shell_ctx *c, int x, int y)
{
    int n = card_count(c);

    /* The Home card's tiles, FIRST -- before the card hit-test below,
     * which would otherwise swallow the click.
     *
     * They highlight under the pointer, so they promise they can be
     * clicked. Without this they were a lie: the card consumed the
     * click and did nothing, so the very first thing anyone ever
     * clicks in this operating system silently failed. Found by
     * clicking one on a booted machine. */
    if ((int)(P(c)->slide.value + 0.5f) == 0) {
        rect a = card_rect(c, c->screen_w, c->screen_h, 0);
        for (int i = 0; i < HOME_TILES && i < c->n_apps; i++) {
            rect t = tile_rect(c, a, i);
            if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
                open_app(c, i);
                return 1;
            }
        }
    }

    /* Front to back: the focused card is on top. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            int cur = (i == (int)(P(c)->slide.value + 0.5f));
            if ((pass == 0) != cur) continue;
            rect a = card_rect(c, c->screen_w, c->screen_h, i);
            if (x < a.x || x >= a.x + a.w || y < a.y || y >= a.y + a.h) continue;
            if (!cur) { focus_card(c, i, 0.36f); return 1; }
            return 1;
        }
    }
    return 0;
}

static void l_motion(shell_ctx *c, int x, int y)
{
    rail_priv *p = P(c);
    p->hover_tile = -1;
    if (c->focus != -1) return;
    rect a = card_rect(c, c->screen_w, c->screen_h, 0);
    for (int i = 0; i < HOME_TILES && i < c->n_apps; i++) {
        rect t = tile_rect(c, a, i);
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) { p->hover_tile = i; return; }
    }
}

static void l_key(shell_ctx *c, int k)
{
    int cur = (int)(P(c)->slide.value + 0.5f);
    switch (k) {
        case 105: focus_card(c, cur - 1, 0.30f); break;   /* KEY_LEFT  */
        case 106: focus_card(c, cur + 1, 0.30f); break;   /* KEY_RIGHT */
        case 102: focus_card(c, 0, 0.36f); break;         /* KEY_HOME  */
        default: break;
    }
}

static int  l_step(shell_ctx *c, float dt) { return tween_step(&P(c)->slide, dt); }
static void l_fini(shell_ctx *c) { c->priv = NULL; }

const shell_layout layout_rail = {
    "rail", l_init, l_paint, l_click, l_motion, l_key, l_step, l_fini
};
