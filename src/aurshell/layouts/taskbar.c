/* layouts/taskbar.c — the "taskbar" archetype: the familiar one.
 *
 * A bar along the bottom, one button on it for every window, a menu at
 * the left that lists every program, and windows that overlap, move,
 * maximise and minimise. None of this is new, and that is the entire
 * point: the person who picks this archetype is picking not to learn
 * anything. Wherever this model and the novel ones disagree, this file
 * copies what thirty years of office machines already taught them.
 *
 * The documented tradeoff is real and is NOT engineered away: windows
 * cover each other and get lost behind each other. Removing that would
 * mean not overlapping, and someone who wanted that would have chosen
 * the archetype that tiles. What this file does remove is the harm
 * that is not part of the bargain — a window never opens off the edge
 * of the screen, and no window can ever reach under the bar. So a
 * window can be lost behind another one, but the button that brings it
 * back is always visible, and that button is the whole mechanism.
 */
#include "../shell.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define TRAY_N       2      /* network + sound: a couple of indicators  */
#define WIN_W_FRAC   0.48f  /* a cascaded window against the screen     */
#define WIN_H_FRAC   0.66f
#define MIN_VISIBLE  170    /* px of a window that must stay on screen  */
#define MENU_RISE    18.f   /* how far the programs menu slides up      */

/* Bar slots. Non-negative slots are window buttons, so one function can
 * own every rectangle the bar contains. */
enum { SLOT_NONE = -1, SLOT_BAR = -2, SLOT_LAUNCH = -3,
       SLOT_TRAY = -4, SLOT_CLOCK = -5 };

/* Parts of a window, likewise, so one function owns window geometry. */
enum { WP_FRAME = 0, WP_TITLE, WP_BODY, WP_MIN, WP_MAX, WP_CLOSE };

typedef struct {
    int   scr_w, scr_h;     /* last painted size; hit-tests use it      */

    int   menu_open;
    tween menu;             /* 0 shut … 1 fully open                    */

    int   hover_slot;       /* bar slot under the pointer               */
    int   hover_row;        /* programs-menu row, -1 for none           */
    int   hover_win;        /* window under the pointer, -1 for none    */
    int   hover_btn;        /* titlebar button, WP_* or -1              */

    int   z[SHELL_MAX_WINS], n_z;   /* stacking order, bottom … top     */
    int   slot[SHELL_MAX_WINS];     /* cascade slot for a window with no geometry */
    int   maxed[SHELL_MAX_WINS];
    rect  saved[SHELL_MAX_WINS];    /* geometry from before maximising  */
    tween shade[SHELL_MAX_WINS];    /* 1 on screen, 0 folded into a bar button */

    int   drag_win, drag_dx, drag_dy;
} tb_priv;

static tb_priv *P(shell_ctx *c) { return (tb_priv *)c->priv; }

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static int   clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
static int   pt_in(rect r, int x, int y)
{ return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
/* Both ends live inside the same shell_ctx, so this copies by hand
 * rather than through snprintf, which may not alias its arguments. */
static void copy_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (!n) return;
    for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

static rect  lerp_rect(rect a, rect b, float t)
{
    rect r;
    r.x = a.x + (int)((float)(b.x - a.x) * t);
    r.y = a.y + (int)((float)(b.y - a.y) * t);
    r.w = a.w + (int)((float)(b.w - a.w) * t);
    r.h = a.h + (int)((float)(b.h - a.h) * t);
    return r;
}

/* font_draw() targets a plain 0x00RRGGBB framebuffer and writes no
 * alpha byte (see font.h), while draw.c composites against whatever
 * alpha it finds in the destination. Every glyph therefore leaves fully
 * transparent pixels behind it, and anything blended over them later
 * treats them as empty and dissolves them — a black smear along the
 * text of the window underneath. This is the archetype that puts
 * windows on top of each other on purpose, so it is the one that hits
 * it. Restoring the documented invariant ("the shell's own framebuffer
 * is opaque") over the region just painted costs one pass and keeps the
 * repair here, rather than changing a file five other renderers are
 * being written against at the same time. */
static void seal(surface *s, rect r)
{
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > s->w ? s->w : r.x + r.w;
    int y1 = r.y + r.h > s->h ? s->h : r.y + r.h;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = s->px + (size_t)y * s->stride;
        for (int x = x0; x < x1; x++) row[x] |= 0xFF000000u;
    }
}

/* ── the three bands of the screen ──────────────────────────────────
 * The bar is taller than a status strip because it holds real targets,
 * not just read-only text. */
static int bar_height(shell_ctx *c) { return c->bar_h + (c->target_large ? 14 : 6); }
static int title_h(shell_ctx *c)    { return c->target_large ? 44 : 36; }

/* Each window in the cascade steps down by slightly more than a
 * titlebar. That is the one free improvement this model allows: the
 * windows still cover each other — that is the bargain — but every one
 * behind keeps the strip that names it and can be grabbed, so the pile
 * reads as a pile of labelled things rather than as one window with
 * debris behind it. */
static int cascade_step(shell_ctx *c) { return title_h(c) + 6; }

/* status_strip="full" means this bar IS the strip: it launches, it
 * switches and it carries the clock, so there is nothing to put at the
 * top. A distribution that turns that off still gets the bar — take it
 * away and the archetype is gone — but the clock and indicators move
 * up into a thin read-only strip, and windows start below it. */
static int strip_h(shell_ctx *c)   { return c->status_full ? 0 : c->bar_h - 6; }
static int work_top(shell_ctx *c)  { return strip_h(c); }

/* ── ONE owner of bar geometry ──────────────────────────────────────
 * Painting and hit-testing both come through here, so a click always
 * lands on the thing the user is pointing at. A button that does not
 * fit comes back with w == 0 and is skipped by both. Nothing here may
 * depend on a font: hit-testing has no fonts to measure with, and
 * geometry that disagrees between the two passes is the classic way a
 * bar ends up "off by one label". */
static rect bar_slot(shell_ctx *c, int W, int H, int slot)
{
    int bh = bar_height(c), by = H - bh;
    rect none = { 0, 0, 0, 0 };
    if (slot == SLOT_BAR) { rect b = { 0, by, W, bh }; return b; }

    int pad = 7, gap = 7, side = 10;
    int ih  = bh - pad * 2;

    int lw = (int)((float)bh * 3.05f);
    lw = clampi(lw, ih + 6, W / 3);
    rect lr = { side, by + pad, lw, ih };
    if (slot == SLOT_LAUNCH) return lr;

    int clock_w = c->show_clock ? (int)((float)bh * 2.15f) : 0;
    int tray_w  = TRAY_N * ih + gap;
    rect cr = { W - side - clock_w, by + pad, clock_w, ih };
    if (slot == SLOT_CLOCK) return c->show_clock ? cr : none;

    rect tr = { cr.x - (clock_w ? gap : 0) - tray_w, by + pad, tray_w, ih };
    if (slot == SLOT_TRAY) return tr;

    if (slot < 0 || slot >= c->n_wins) return none;

    int x0    = lr.x + lr.w + gap * 2;
    int avail = tr.x - gap * 2 - x0;
    if (avail < 0) avail = 0;
    int n  = c->n_wins < 1 ? 1 : c->n_wins;
    int bw = (avail - gap * (n - 1)) / n;
    bw = clampi(bw, ih + 10, c->target_large ? 208 : 176);

    rect b = { x0 + slot * (bw + gap), by + pad, bw, ih };
    if (b.x + b.w > tr.x - gap) b.w = 0;    /* no room left: not drawn, not clickable */
    return b;
}

/* Where a window folds down to when it is minimised. Its own button if
 * that button fits, the left end of the strip otherwise, so the fold
 * always points at the bar rather than at nowhere. */
static rect fold_target(shell_ctx *c, int W, int H, int i)
{
    rect b = bar_slot(c, W, H, i);
    if (b.w > 0) { b.w = b.w > 48 ? 48 : b.w; return b; }
    rect l = bar_slot(c, W, H, SLOT_LAUNCH);
    rect f = { l.x + l.w + 14, l.y, 48, l.h };
    return f;
}

/* ── ONE owner of window geometry ───────────────────────────────────
 * Frame, titlebar, body and the three titlebar buttons all come from
 * here. The clamping is the honest part of this archetype: windows are
 * allowed to bury each other — that is the model the user chose — but
 * the vertical clamp is absolute, so no window ever reaches under the
 * bar, and no window can be dragged entirely off the screen. Losing a
 * window behind another one is the documented tradeoff; losing it
 * behind the one control that gets it back would just be a bug. */
static rect cascade_geom(shell_ctx *c, int W, int H, int i)
{
    int top = work_top(c), bot = H - bar_height(c);
    int m = c->margin;
    int ww = clampi((int)((float)W * WIN_W_FRAC), 380, 940);
    int wh = clampi((int)((float)(bot - top) * WIN_H_FRAC), 240, 700);
    if (ww > W - 2 * m) ww = W - 2 * m;
    if (wh > bot - top - 2 * m) wh = bot - top - 2 * m;

    /* The pile starts a little in from the top-left corner rather than
     * jammed against it, so a screen with two or three things open
     * looks composed instead of swept into one corner — and so the
     * first window a person opens is not sitting under the pointer. */
    /* Give the pile the room it needs first and spread whatever is left
     * over around it, so two windows look composed on the desktop and
     * eight still step cleanly instead of being shoved into a corner. */
    int n = c->n_wins < 1 ? 1 : c->n_wins;
    int step = cascade_step(c);
    int need = (n - 1) * step;
    int room_x = (W - m) - (m + 16) - ww;
    int room_y = (bot - m) - (top + m) - wh;
    if (room_x < 0) room_x = 0;
    if (room_y < 0) room_y = 0;
    int x0 = m + 16 + clampi((room_x - need) / 3, 0, room_x);
    int y0 = top + m + clampi((room_y - need) / 3, 0, room_y);

    int span_x = (W - m - ww) - x0, span_y = (bot - m - wh) - y0;
    if (span_x < 0) span_x = 0;
    if (span_y < 0) span_y = 0;
    int span = span_x < span_y ? span_x : span_y;

    /* Tighten the step until the whole pile fits in one run. A cascade
     * that wraps puts the newest window BEHIND the corner of the pile,
     * where the front one is no longer the one nearest the bottom
     * right — and a pile that does not read front-to-back is worse than
     * a tight one. Below a floor they stop being separate things at
     * all, so past that it wraps and the bar carries the load, which is
     * what the bar is for. */
    if (n > 1 && need > span) step = span / (n - 1);
    if (step < 22) step = 22;
    int per = 1 + span / step;

    int k = i % per, band = i / per;
    rect r = { x0 + k * step + band * 18, y0 + k * step, ww, wh };
    if (r.x + r.w > W - m) r.x = W - m - r.w;
    if (r.x < 0) r.x = 0;
    return r;
}

static rect win_part(shell_ctx *c, int W, int H, int i, int part)
{
    tb_priv *p = P(c);
    int top = work_top(c), bot = H - bar_height(c), th = title_h(c);
    rect a;

    if (p && p->maxed[i]) {
        rect m = { 0, top, W, bot - top };
        a = m;
    } else {
        a = c->wins[i].geom;
        if (a.w <= 0 || a.h <= 0) a = cascade_geom(c, W, H, p ? p->slot[i] : i);
        a.w = clampi(a.w, 260, W);
        a.h = clampi(a.h, th + 90, bot - top);
        if (a.y < top) a.y = top;
        if (a.y + a.h > bot) a.y = bot - a.h;
        if (a.x + a.w < MIN_VISIBLE) a.x = MIN_VISIBLE - a.w;
        if (a.x > W - MIN_VISIBLE) a.x = W - MIN_VISIBLE;
    }

    if (part == WP_FRAME) return a;
    if (part == WP_TITLE) { rect t = { a.x, a.y, a.w, th }; return t; }
    if (part == WP_BODY)  { rect b = { a.x, a.y + th, a.w, a.h - th }; return b; }

    int bs = th - 14; if (bs < 18) bs = 18;
    int idx = (part == WP_CLOSE) ? 0 : (part == WP_MAX ? 1 : 2);
    rect b = { a.x + a.w - 8 - bs - idx * (bs + 4), a.y + (th - bs) / 2, bs, bs };
    return b;
}

/* ── ONE owner of programs-menu geometry ────────────────────────────
 * The list has to hold EVERY program, so its shape is derived from how
 * many rows actually fit above the bar; when they do not all fit it
 * says so on the last row rather than quietly dropping the tail. */
static void menu_shape(shell_ctx *c, int W, int H, int *cols, int *rows, int *shown)
{
    int row_h = c->target_large ? 46 : 38;
    int head  = c->target_large ? 56 : 46;
    int bot   = H - bar_height(c) - 8;
    int avail = bot - (work_top(c) + c->margin);

    int fit = (avail - head - c->padding * 2) / row_h;
    if (fit < 1) fit = 1;

    int n = c->n_apps, cc = 1, rr = n < 1 ? 1 : n;
    if (rr > fit) {
        cc = 2; rr = (n + 1) / 2;
        if (rr > fit) rr = fit;
    }
    int sh = rr * cc;
    if (sh > n) sh = n;
    else if (sh < n) sh = rr * cc - 1;      /* last slot becomes "…and N more" */
    if (sh < 0) sh = 0;
    (void)W;
    *cols = cc; *rows = rr; *shown = sh;
}

static rect menu_part(shell_ctx *c, int W, int H, int part)
{
    tb_priv *p = P(c);
    int cols, rows, shown;
    menu_shape(c, W, H, &cols, &rows, &shown);

    int row_h = c->target_large ? 46 : 38;
    int col_w = c->target_large ? 262 : 226;
    int head  = c->target_large ? 56 : 46;
    int pad   = c->padding;

    int pw = pad * 2 + cols * col_w;
    if (pw > W - 20) pw = W - 20;
    int ph = pad * 2 + head + rows * row_h;
    int bot = H - bar_height(c) - 8;

    rect panel = { 10, bot - ph, pw, ph };
    /* The rise is baked in here, not added at paint time, so a click
     * during the animation still hits what is on screen. */
    if (p) panel.y += (int)((1.f - clampf(p->menu.value, 0.f, 1.f)) * MENU_RISE);

    if (part < 0) return panel;

    int col = part / rows, r = part % rows;
    int iw  = (pw - pad * 2) / (cols < 1 ? 1 : cols);
    rect row = { panel.x + pad + col * iw, panel.y + pad + head + r * row_h, iw, row_h };
    return row;
}

/* ── stacking ───────────────────────────────────────────────────────── */
static void stack_sync(shell_ctx *c)
{
    tb_priv *p = P(c);
    int seen[SHELL_MAX_WINS], n = 0;
    memset(seen, 0, sizeof seen);
    for (int k = 0; k < p->n_z; k++) {
        int w = p->z[k];
        if (w < 0 || w >= c->n_wins || seen[w]) continue;
        seen[w] = 1; p->z[n++] = w;
    }
    for (int i = 0; i < c->n_wins; i++) if (!seen[i]) p->z[n++] = i;
    p->n_z = n;
}

static void raise_win(shell_ctx *c, int i)
{
    tb_priv *p = P(c);
    stack_sync(c);
    int n = 0;
    for (int k = 0; k < p->n_z; k++) if (p->z[k] != i) p->z[n++] = p->z[k];
    p->z[n++] = i;
    p->n_z = n;
}

static int topmost_visible(shell_ctx *c)
{
    tb_priv *p = P(c);
    for (int k = p->n_z - 1; k >= 0; k--)
        if (!c->wins[p->z[k]].minimised) return p->z[k];
    return -1;
}

static void minimise_win(shell_ctx *c, int i)
{
    tb_priv *p = P(c);
    if (i < 0 || i >= c->n_wins || c->wins[i].minimised) return;
    c->wins[i].minimised = 1;
    tween_to(&p->shade[i], 0.f, 0.20f, EASE_OUT_CUBIC);
    if (c->focus == i) c->focus = topmost_visible(c);
}

static void restore_win(shell_ctx *c, int i)
{
    tb_priv *p = P(c);
    if (i < 0 || i >= c->n_wins) return;
    c->wins[i].minimised = 0;
    tween_to(&p->shade[i], 1.f, 0.22f, EASE_OUT_CUBIC);
    raise_win(c, i);
    c->focus = i;
}

static void close_win(shell_ctx *c, int i)
{
    tb_priv *p = P(c);
    if (i < 0 || i >= c->n_wins) return;
    for (int k = i; k < c->n_wins - 1; k++) {
        c->wins[k]  = c->wins[k + 1];
        p->slot[k]  = p->slot[k + 1];
        p->maxed[k] = p->maxed[k + 1];
        p->saved[k] = p->saved[k + 1];
        p->shade[k] = p->shade[k + 1];
    }
    c->n_wins--;
    int n = 0;
    for (int k = 0; k < p->n_z; k++) {
        int w = p->z[k];
        if (w == i) continue;
        p->z[n++] = (w > i) ? w - 1 : w;
    }
    p->n_z = n;
    if (p->drag_win == i) p->drag_win = -1;
    c->focus = topmost_visible(c);
}

static void toggle_max(shell_ctx *c, int i)
{
    tb_priv *p = P(c);
    if (i < 0 || i >= c->n_wins) return;
    if (p->maxed[i]) { c->wins[i].geom = p->saved[i]; p->maxed[i] = 0; }
    else {
        p->saved[i] = win_part(c, p->scr_w, p->scr_h, i, WP_FRAME);
        p->maxed[i] = 1;
    }
    raise_win(c, i);
    c->focus = i;
}

static void open_app(shell_ctx *c, int app)
{
    tb_priv *p = P(c);
    if (app < 0 || app >= c->n_apps || c->n_wins >= SHELL_MAX_WINS) return;
    int i = c->n_wins++;
    memset(&c->wins[i], 0, sizeof c->wins[i]);
    c->wins[i].app = app;
    copy_str(c->wins[i].title,    sizeof c->wins[i].title,    c->apps[app].name);
    copy_str(c->wins[i].subtitle, sizeof c->wins[i].subtitle, c->apps[app].hint);
    p->slot[i]  = i;            /* the next place in the cascade */
    p->maxed[i] = 0;
    tween_set(&p->shade[i], 0.f);
    tween_to(&p->shade[i], 1.f, 0.24f, EASE_OUT_CUBIC);
    raise_win(c, i);
    c->focus = i;
}

/* ── text that has to fit a fixed box ───────────────────────────────── */
static const char *fit_text(font *f, const char *src, char *dst, size_t n, float maxw)
{
    if (!f || !src || !*src) return src;
    if (shell_text_w(f, src) <= maxw) return src;
    size_t cut = strlen(src);
    while (cut > 0) {
        cut--;
        /* never cut a UTF-8 sequence in half */
        while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
        if (cut + 4 >= n) continue;
        memcpy(dst, src, cut);
        memcpy(dst + cut, "\xE2\x80\xA6", 3);
        dst[cut + 3] = 0;
        if (shell_text_w(f, dst) <= maxw) return dst;
    }
    dst[0] = 0;
    return dst;
}

static uint32_t win_tint(shell_ctx *c, int i)
{ return c->wins[i].app >= 0 ? c->apps[c->wins[i].app].tint : c->accent; }
static shell_icon win_icon(shell_ctx *c, int i)
{ return c->wins[i].app >= 0 ? c->apps[c->wins[i].app].icon : ICON_WINDOW; }

/* ── init ───────────────────────────────────────────────────────────── */
static void l_init(shell_ctx *c)
{
    static tb_priv priv;
    memset(&priv, 0, sizeof priv);
    priv.scr_w = 1600; priv.scr_h = 900;
    priv.hover_slot = SLOT_NONE;
    priv.hover_row = priv.hover_win = -1;
    priv.hover_btn = -1;
    priv.drag_win = -1;
    c->priv = &priv;

    for (int i = 0; i < SHELL_MAX_WINS; i++)
        tween_set(&priv.shade[i], 1.f);

    stack_sync(c);
    if (c->focus >= 0 && c->focus < c->n_wins) raise_win(c, c->focus);
    else c->focus = topmost_visible(c);

    /* A session can hand us windows with no geometry at all, so give
     * each one a place in the cascade — by STACK position, not by array
     * index. Slot 0 is the back of the pile and the last slot is the
     * front, which puts the focused window at the bottom of the cascade
     * with every other titlebar stepping up behind it: the shape anyone
     * reads instantly as "several things are open". The slot is only a
     * fallback; the moment a window is dragged it owns its geometry and
     * nothing moves it again. Resolving it at paint time rather than
     * here means it is sized for the screen actually in front of the
     * user, not for whatever this process guessed at start-up. */
    for (int k = 0; k < priv.n_z; k++) {
        int i = priv.z[k];
        priv.slot[i] = k;
        if (c->wins[i].minimised) tween_set(&priv.shade[i], 0.f);
    }

    /* With something already open the menu starts shut, as it should.
     * With NOTHING open there is no button to press and no way to guess
     * what the machine can do, so the list of programs is the answer to
     * "what now?" and opens by itself. It is also the only way a
     * preview — which never sends a click — can show the menu at all. */
    if (c->n_wins == 0) { priv.menu_open = 1; tween_set(&priv.menu, 1.f); }
}

/* ── painting: windows ──────────────────────────────────────────────── */
static void paint_ctrl(shell_ctx *c, surface *s, rect b, int part, int hot, float a)
{
    float cx = (float)b.x + (float)b.w * 0.5f, cy = (float)b.y + (float)b.h * 0.5f;
    float r  = (float)b.w * 0.5f;
    uint32_t glyph = c->subtle;

    if (hot) {
        draw_circle(s, cx, cy, r, part == WP_CLOSE ? c->err : c->overlay,
                    a * (part == WP_CLOSE ? 0.85f : 0.9f));
        glyph = c->fg_hi;
    }
    float k = r * 0.42f;
    if (part == WP_MIN) {
        draw_line(s, cx - k, cy + k * 0.55f, cx + k, cy + k * 0.55f, 1.7f, glyph, a);
    } else if (part == WP_MAX) {
        rect q = { (int)(cx - k), (int)(cy - k), (int)(k * 2.f), (int)(k * 2.f) };
        draw_round_rect_border(s, q, corners_all(2.f), 1.6f, glyph, a);
    } else {
        draw_line(s, cx - k, cy - k, cx + k, cy + k, 1.7f, glyph, a);
        draw_line(s, cx + k, cy - k, cx - k, cy + k, 1.7f, glyph, a);
    }
}

static void paint_window(shell_ctx *c, surface *s, shell_fonts *f, int i,
                         rect a, int focused, float alpha)
{
    tb_priv *p = P(c);
    uint32_t tint = win_tint(c, i);
    int th = title_h(c);
    float r = (float)c->radius;
    corners fc = p->maxed[i] ? (corners){ r, r, 0.f, 0.f } : corners_all(r);
    corners tc = { fc.tl, fc.tr, 0.f, 0.f };
    corners bc = { 0.f, 0.f, fc.br, fc.bl };

    /* Depth is what makes a stack readable: the focused window throws a
     * deeper shadow and sits on a brighter surface, so "which one am I
     * typing into" is answered before the titlebar is read. */
    draw_round_rect_shadow(s, a, fc, (float)c->shadow_r * (focused ? 1.35f : 0.72f),
                           0x000000, c->shadow_a * alpha * (focused ? 1.f : 0.75f),
                           focused ? 16 : 7);
    draw_blur_region(s, a, c->blur_r);

    float body_a = alpha * c->panel_a * (focused ? 1.f : 0.96f);
    draw_round_rect(s, a, fc, c->surface_c, body_a);
    /* An unfocused window is washed toward the bar colour rather than
     * just faded: on a light theme a plain fade leaves it the brightest
     * thing on the screen, competing with the window actually in use. */
    if (!focused) draw_round_rect(s, a, fc, c->bg_alt, alpha * 0.3f);

    rect tb = { a.x, a.y, a.w, th };
    if (focused)
        draw_round_rect_gradient(s, tb, tc, c->surface_hi, c->bg_alt, alpha * 0.97f, 1);
    else
        draw_round_rect(s, tb, tc, c->bg_alt, alpha * 0.9f);
    draw_line(s, (float)a.x + 1.f, (float)(a.y + th) - 0.5f,
              (float)(a.x + a.w - 1), (float)(a.y + th) - 0.5f, 1.f, c->overlay,
              alpha * (focused ? 0.9f : 0.6f));

    /* Focus is drawn in the THEME accent, not in the app's own colour: a
     * ring around a window answers "which one am I typing into", and an
     * answer that changes colour with the program is a worse answer.
     * The app's colour stays in its icon, where it identifies rather
     * than signals. It also keeps focus legible in a light theme, where
     * a pastel app tint on a white surface is nearly invisible. */
    if (focused) {
        rect g = { a.x - 1, a.y - 1, a.w + 2, a.h + 2 };
        draw_round_rect_border(s, g, corners_all(r + 1.f), 1.f, c->accent, alpha * 0.25f);
    }
    draw_round_rect_border(s, a, fc, focused ? (float)c->border : 1.f,
                           focused ? c->accent : c->overlay, alpha * (focused ? 0.9f : 0.95f));

    /* Title row: icon, name, and the subtitle trailing it when there is
     * room — the same line a browser would put a page title on. */
    float tx = (float)(a.x + 12);
    shell_icon_draw(s, win_icon(c, i), tx + 10.f, (float)(a.y + th / 2), 20.f,
                    tint, alpha * (focused ? 1.f : 0.72f));
    tx += 30.f;
    rect cb = win_part(c, p->scr_w, p->scr_h, i, WP_MIN);
    float room = (float)cb.x - tx - 12.f;
    char buf[80];
    const char *name = fit_text(f->mid, c->wins[i].title, buf, sizeof buf, room);
    float by = shell_baseline(f->mid, (float)a.y, (float)th);
    shell_text(s, f->mid, tx, by, name, focused ? c->fg_hi : c->subtle,
               alpha * (focused ? 0.98f : 0.85f));

    float used = shell_text_w(f->mid, name);
    if (c->wins[i].subtitle[0] && room - used > 90.f) {
        char sb[110];
        const char *sub = fit_text(f->small, c->wins[i].subtitle, sb, sizeof sb,
                                   room - used - 16.f);
        shell_text(s, f->small, tx + used + 14.f,
                   shell_baseline(f->small, (float)a.y, (float)th), sub,
                   c->muted, alpha * (focused ? 0.85f : 0.6f));
    }

    for (int part = WP_MIN; part <= WP_CLOSE; part++) {
        rect b = win_part(c, p->scr_w, p->scr_h, i, part);
        paint_ctrl(c, s, b, part, p->hover_win == i && p->hover_btn == part,
                   alpha * (focused ? 1.f : 0.7f));
    }

    rect body = win_part(c, p->scr_w, p->scr_h, i, WP_BODY);
    if (c->wins[i].content) {
        draw_scaled_rounded(s, c->wins[i].content, body, bc, alpha);
        return;
    }
    if (body.h < 80) return;
    float cx = (float)body.x + (float)body.w * 0.5f;
    float cy = (float)body.y + (float)body.h * 0.5f;
    float isz = clampf((float)body.h * 0.20f, 28.f, 52.f);
    draw_circle(s, cx, cy - isz * 0.62f, isz * 0.92f, tint, alpha * 0.10f);
    shell_icon_draw(s, win_icon(c, i), cx, cy - isz * 0.62f, isz, tint, alpha * 0.55f);
    shell_text_centred(s, f->mid, cx, cy + isz * 0.72f, c->wins[i].title,
                       c->fg, alpha * 0.82f);
    shell_text_centred(s, f->small, cx, cy + isz * 0.72f + 24.f,
                       c->wins[i].subtitle[0] ? c->wins[i].subtitle : "Opening…",
                       c->muted, alpha * 0.7f);
}

/* ── painting: the bar ──────────────────────────────────────────────── */
static void paint_task_button(shell_ctx *c, surface *s, shell_fonts *f, int i, rect b)
{
    tb_priv *p = P(c);
    if (b.w <= 0) return;
    int mini = c->wins[i].minimised;
    int on   = (i == c->focus) && !mini;
    int hot  = (p->hover_slot == i);
    uint32_t tint = win_tint(c, i);
    float r = (float)c->radius_sm;

    /* The three fills are built from `overlay` and a wash of the accent
     * rather than from `surface`, because a light theme's surface is
     * white and its bar is nearly white: a surface-filled button would
     * be the BRIGHTEST thing on the bar whether or not it was the
     * active one. Overlay sits between bar and text in every theme, so
     * the order stays right in both directions. */
    if (on) {
        draw_round_rect(s, b, corners_all(r), c->surface_hi, 0.98f);
        draw_round_rect(s, b, corners_all(r), c->accent, 0.16f);
        draw_round_rect_border(s, b, corners_all(r), 1.5f, c->accent, 0.85f);
    } else if (mini) {
        /* Minimised: still open, still here, just not on the screen.
         * Barely filled, so the bar shows at a glance what is in front
         * of you and what is only "somewhere". */
        draw_round_rect(s, b, corners_all(r), c->overlay, hot ? 0.34f : 0.12f);
        draw_round_rect_border(s, b, corners_all(r), 1.f, c->overlay, 0.55f);
    } else {
        draw_round_rect(s, b, corners_all(r), c->overlay, hot ? 0.52f : 0.3f);
        draw_round_rect_border(s, b, corners_all(r), 1.f, c->overlay, hot ? 0.85f : 0.6f);
    }

    /* Running indicator. A wide bar under the focused window, a short
     * one under the others, a dot for the minimised — three states, one
     * shape, readable without reading a word. */
    float iw = on ? (float)b.w * 0.44f : (mini ? 5.f : (float)b.w * 0.18f);
    rect ind = { b.x + b.w / 2 - (int)(iw * 0.5f), b.y + b.h - 4, (int)iw, 3 };
    draw_round_rect(s, ind, corners_all(1.5f), c->accent, on ? 0.95f : (mini ? 0.55f : 0.6f));

    float cx = (float)b.x + 18.f;
    shell_icon_draw(s, win_icon(c, i), cx, (float)b.y + (float)b.h * 0.46f, 19.f,
                    tint, mini ? 0.55f : (on ? 1.f : 0.85f));

    float tx = cx + 16.f;
    float room = (float)(b.x + b.w) - tx - 10.f;
    if (room < 24.f) return;
    char buf[80];
    const char *t = fit_text(f->small, c->wins[i].title, buf, sizeof buf, room);
    shell_text(s, f->small, tx, shell_baseline(f->small, (float)b.y, (float)b.h - 3.f),
               t, on ? c->fg_hi : (mini ? c->subtle : c->fg), on ? 1.f : 0.9f);
}

static void paint_bar(shell_ctx *c, surface *s, shell_fonts *f)
{
    tb_priv *p = P(c);
    int W = s->w, H = s->h;
    rect bar = bar_slot(c, W, H, SLOT_BAR);

    draw_blur_region(s, bar, c->blur_r);
    draw_rect(s, bar, c->bg_alt, c->panel_a);
    draw_line(s, 0.f, (float)bar.y + 0.5f, (float)W, (float)bar.y + 0.5f,
              1.f, c->overlay, 0.9f);

    /* Launcher. The label is the affordance: a migrant should not have
     * to learn what a glyph means to find their programs. */
    rect l = bar_slot(c, W, H, SLOT_LAUNCH);
    int lhot = (p->hover_slot == SLOT_LAUNCH) || p->menu_open;
    draw_round_rect(s, l, corners_all((float)c->radius_sm),
                    p->menu_open ? c->surface_hi : c->overlay, lhot ? 0.95f : 0.32f);
    if (p->menu_open) draw_round_rect(s, l, corners_all((float)c->radius_sm), c->accent, 0.16f);
    draw_round_rect_border(s, l, corners_all((float)c->radius_sm),
                           p->menu_open ? 1.5f : 1.f,
                           p->menu_open ? c->accent : c->overlay, lhot ? 0.85f : 0.6f);
    float lcx = (float)l.x + 20.f, lcy = (float)l.y + (float)l.h * 0.5f;
    draw_circle(s, lcx, lcy, 8.f, c->accent, 0.22f);
    draw_circle(s, lcx, lcy, 4.5f, c->accent, 0.95f);
    char lb[48];
    const char *lt = fit_text(f->mid, "All Programs", lb, sizeof lb,
                              (float)l.w - 42.f);
    shell_text(s, f->mid, (float)l.x + 34.f, shell_baseline(f->mid, (float)l.y, (float)l.h),
               lt, c->fg_hi, 0.96f);

    /* Window buttons. */
    if (c->n_wins == 0) {
        rect first = bar_slot(c, W, H, SLOT_LAUNCH);
        shell_text(s, f->small, (float)(first.x + first.w + 18),
                   shell_baseline(f->small, (float)first.y, (float)first.h),
                   "Nothing open yet — pick something from All Programs",
                   c->muted, 0.85f);
    }
    for (int i = 0; i < c->n_wins; i++)
        paint_task_button(c, s, f, i, bar_slot(c, W, H, i));

    /* Indicators. Read-only: this archetype keeps status where people
     * expect it and does not pretend the corner is a control panel. */
    rect tr = bar_slot(c, W, H, SLOT_TRAY);
    const shell_icon ind[TRAY_N] = { ICON_GLOBE, ICON_MUSIC };
    for (int i = 0; i < TRAY_N; i++) {
        float cx = (float)tr.x + ((float)i + 0.5f) * (float)tr.h;
        shell_icon_draw(s, ind[i], cx, (float)tr.y + (float)tr.h * 0.5f, 17.f,
                        i == 0 ? c->ok : c->subtle, i == 0 ? 0.9f : 0.75f);
    }

    if (c->show_clock && c->status_full) {
        rect cr = bar_slot(c, W, H, SLOT_CLOCK);
        char hm[32], dt[48];
        shell_clock(hm, sizeof hm, dt, sizeof dt);
        float rx = (float)(cr.x + cr.w);
        float y1 = (float)cr.y + (f->mid ? font_ascent(f->mid) : 16.f) + 1.f;
        float y2 = y1 + (f->small ? font_line_height(f->small) : 16.f) + 1.f;
        shell_text(s, f->mid, rx - shell_text_w(f->mid, hm), y1, hm, c->fg_hi, 0.97f);
        shell_text(s, f->small, rx - shell_text_w(f->small, dt), y2, dt, c->subtle, 0.8f);
    }
}

/* ── painting: the programs menu ────────────────────────────────────── */
static void paint_menu(shell_ctx *c, surface *s, shell_fonts *f)
{
    tb_priv *p = P(c);
    float t = clampf(p->menu.value, 0.f, 1.f);
    if (t <= 0.002f) return;

    int W = s->w, H = s->h;
    int cols, rows, shown;
    menu_shape(c, W, H, &cols, &rows, &shown);
    rect a = menu_part(c, W, H, -1);
    float r = (float)c->radius;
    corners mc = corners_all(r);

    draw_round_rect_shadow(s, a, mc, (float)c->shadow_r * 1.3f, 0x000000,
                           c->shadow_a * t, 14);
    draw_blur_region(s, a, c->blur_r);
    draw_round_rect(s, a, mc, c->surface_c, t * c->panel_a);
    draw_round_rect_border(s, a, mc, 1.f, c->overlay, t * 0.8f);

    int head = c->target_large ? 56 : 46;
    float hx = (float)(a.x + c->padding + 4);
    shell_text(s, f->mid, hx,
               (float)(a.y + c->padding) + (f->mid ? font_ascent(f->mid) : 16.f) + 2.f,
               "All Programs", c->fg_hi, t * 0.98f);
    shell_text(s, f->small, hx,
               (float)(a.y + c->padding) + (f->mid ? font_line_height(f->mid) : 22.f)
               + (f->small ? font_ascent(f->small) : 13.f) + 2.f,
               "Everything on this computer", c->subtle, t * 0.8f);
    draw_line(s, (float)(a.x + c->padding), (float)(a.y + c->padding + head) - 6.5f,
              (float)(a.x + a.w - c->padding), (float)(a.y + c->padding + head) - 6.5f,
              1.f, c->overlay, t * 0.7f);

    for (int i = 0; i < shown; i++) {
        rect row = menu_part(c, W, H, i);
        int hot = (p->hover_row == i);
        rect hit = { row.x + 2, row.y + 1, row.w - 4, row.h - 2 };
        if (hot) {
            draw_round_rect(s, hit, corners_all((float)c->radius_sm), c->surface_hi, t);
            draw_round_rect_border(s, hit, corners_all((float)c->radius_sm), 1.f,
                                   c->accent, t * 0.55f);
        }
        float icx = (float)row.x + 24.f, icy = (float)row.y + (float)row.h * 0.5f;
        draw_circle(s, icx, icy, 15.f, c->apps[i].tint, t * (hot ? 0.24f : 0.14f));
        shell_icon_draw(s, c->apps[i].icon, icx, icy, 21.f, c->apps[i].tint, t * 0.95f);

        float tx = (float)row.x + 44.f;
        float room = (float)(row.x + row.w) - tx - 10.f;
        char nb[64], hb[80];
        const char *nm = fit_text(f->mid, c->apps[i].name, nb, sizeof nb, room);
        float ny = (float)row.y + (float)row.h * 0.5f - 3.f;
        shell_text(s, f->mid, tx, ny, nm, c->fg_hi, t * 0.97f);
        const char *ht = fit_text(f->small, c->apps[i].hint, hb, sizeof hb, room);
        shell_text(s, f->small, tx, ny + (f->small ? font_ascent(f->small) : 13.f) + 4.f,
                   ht, c->subtle, t * 0.8f);
    }

    /* Never silently swallow the tail of "all programs". */
    if (shown < c->n_apps) {
        rect row = menu_part(c, W, H, shown);
        char more[48];
        snprintf(more, sizeof more, "…and %d more", c->n_apps - shown);
        shell_text(s, f->small, (float)row.x + 44.f,
                   shell_baseline(f->small, (float)row.y, (float)row.h),
                   more, c->muted, t * 0.85f);
    }
}

/* ── paint ──────────────────────────────────────────────────────────── */
static void l_paint(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall)
{
    tb_priv *p = P(c);
    p->scr_w = s->w; p->scr_h = s->h;
    stack_sync(c);

    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else surface_fill(s, 0xFF000000u | c->bg);

    /* Only when the bar is NOT the whole strip; see strip_h(). */
    if (!c->status_full) {
        int sh = strip_h(c);
        rect st = { 0, 0, s->w, sh };
        draw_rect(s, st, c->bg, 0.55f);
        draw_line(s, 0.f, (float)sh, (float)s->w, (float)sh, 1.f, c->overlay, 0.5f);
        draw_circle(s, (float)(c->margin + 8), (float)(sh / 2), 5.f, c->accent, 1.f);
        shell_text(s, f->small, (float)(c->margin + 22),
                   shell_baseline(f->small, 0.f, (float)sh), c->brand, c->subtle, 0.9f);
        if (c->show_clock) {
            char hm[32], dt[48];
            shell_clock(hm, sizeof hm, dt, sizeof dt);
            float by = shell_baseline(f->small, 0.f, (float)sh);
            float rx = (float)(s->w - c->margin);
            shell_text(s, f->small, rx - shell_text_w(f->small, hm), by, hm, c->fg_hi, 0.95f);
            rx -= shell_text_w(f->small, hm) + 14.f;
            shell_text(s, f->small, rx - shell_text_w(f->small, dt), by, dt, c->subtle, 0.75f);
        }
    }

    /* Back to front, so the focused window is painted last and is on
     * top — the stacking IS the model, and painting it in any other
     * order would be a different archetype. */
    for (int k = 0; k < p->n_z; k++) {
        int i = p->z[k];
        float t = clampf(p->shade[i].value, 0.f, 1.f);
        if (t <= 0.004f) continue;               /* folded away into the bar */
        rect a = win_part(c, s->w, s->h, i, WP_FRAME);
        if (t < 0.999f) a = lerp_rect(a, fold_target(c, s->w, s->h, i), 1.f - t);
        paint_window(c, s, f, i, a, i == c->focus && !c->wins[i].minimised, t);
        seal(s, a);
    }

    paint_bar(c, s, f);
    seal(s, bar_slot(c, s->w, s->h, SLOT_BAR));
    paint_menu(c, s, f);
}

/* ── input ──────────────────────────────────────────────────────────── */
static int menu_hit(shell_ctx *c, int x, int y)
{
    int cols, rows, shown;
    menu_shape(c, P(c)->scr_w, P(c)->scr_h, &cols, &rows, &shown);
    for (int i = 0; i < shown; i++)
        if (pt_in(menu_part(c, P(c)->scr_w, P(c)->scr_h, i), x, y)) return i;
    return -1;
}

static void set_menu(shell_ctx *c, int open)
{
    tb_priv *p = P(c);
    p->menu_open = open;
    tween_to(&p->menu, open ? 1.f : 0.f, open ? 0.20f : 0.14f,
             open ? EASE_OUT_CUBIC : EASE_LINEAR);
    if (!open) p->hover_row = -1;
}

static int l_click(shell_ctx *c, int x, int y)
{
    tb_priv *p = P(c);
    int W = p->scr_w, H = p->scr_h;
    stack_sync(c);

    if (p->menu_open) {
        if (pt_in(menu_part(c, W, H, -1), x, y)) {
            int row = menu_hit(c, x, y);
            if (row >= 0) { open_app(c, row); set_menu(c, 0); }
            return 1;
        }
        /* A click anywhere else dismisses it and goes no further, which
         * is what every menu this one imitates already does. */
        if (!pt_in(bar_slot(c, W, H, SLOT_LAUNCH), x, y)) { set_menu(c, 0); return 1; }
    }

    rect bar = bar_slot(c, W, H, SLOT_BAR);
    if (pt_in(bar, x, y)) {
        if (pt_in(bar_slot(c, W, H, SLOT_LAUNCH), x, y)) {
            set_menu(c, !p->menu_open);
            return 1;
        }
        for (int i = 0; i < c->n_wins; i++) {
            if (!pt_in(bar_slot(c, W, H, i), x, y)) continue;
            if (c->wins[i].minimised) restore_win(c, i);
            else if (i == c->focus) minimise_win(c, i);  /* click the active one to put it away */
            else { raise_win(c, i); c->focus = i; }
            return 1;
        }
        return 1;               /* the bar swallows clicks; it is a surface, not a gap */
    }

    /* Windows, front to back: the one on top gets the click. */
    for (int k = p->n_z - 1; k >= 0; k--) {
        int i = p->z[k];
        if (c->wins[i].minimised) continue;
        if (!pt_in(win_part(c, W, H, i, WP_FRAME), x, y)) continue;

        if (pt_in(win_part(c, W, H, i, WP_CLOSE), x, y)) { close_win(c, i); return 1; }
        if (pt_in(win_part(c, W, H, i, WP_MAX), x, y))   { toggle_max(c, i); return 1; }
        if (pt_in(win_part(c, W, H, i, WP_MIN), x, y))   { minimise_win(c, i); return 1; }

        raise_win(c, i);
        c->focus = i;
        if (pt_in(win_part(c, W, H, i, WP_TITLE), x, y) && !p->maxed[i]) {
            rect a = win_part(c, W, H, i, WP_FRAME);
            p->drag_win = i; p->drag_dx = x - a.x; p->drag_dy = y - a.y;
        }
        return 1;
    }
    return 0;                   /* bare desktop: nothing to consume */
}

static void l_motion(shell_ctx *c, int x, int y)
{
    tb_priv *p = P(c);
    int W = p->scr_w, H = p->scr_h;
    stack_sync(c);

    /* There is no release callback in the contract, so a drag ends the
     * moment the button is no longer held. */
    if (p->drag_win >= 0) {
        if (!c->mouse_down || p->drag_win >= c->n_wins) { p->drag_win = -1; }
        else {
            rect g = c->wins[p->drag_win].geom;
            g.x = x - p->drag_dx; g.y = y - p->drag_dy;
            c->wins[p->drag_win].geom = g;      /* win_part() does the clamping */
            c->wins[p->drag_win].geom = win_part(c, W, H, p->drag_win, WP_FRAME);
            return;
        }
    }

    p->hover_slot = SLOT_NONE;
    p->hover_row = p->hover_win = -1;
    p->hover_btn = -1;
    c->hover = -1;

    if (p->menu_open && pt_in(menu_part(c, W, H, -1), x, y)) {
        p->hover_row = menu_hit(c, x, y);
        return;
    }
    if (pt_in(bar_slot(c, W, H, SLOT_BAR), x, y)) {
        if (pt_in(bar_slot(c, W, H, SLOT_LAUNCH), x, y)) { p->hover_slot = SLOT_LAUNCH; return; }
        for (int i = 0; i < c->n_wins; i++)
            if (pt_in(bar_slot(c, W, H, i), x, y)) { p->hover_slot = i; c->hover = i; return; }
        if (pt_in(bar_slot(c, W, H, SLOT_TRAY), x, y))  p->hover_slot = SLOT_TRAY;
        else if (pt_in(bar_slot(c, W, H, SLOT_CLOCK), x, y)) p->hover_slot = SLOT_CLOCK;
        return;
    }
    for (int k = p->n_z - 1; k >= 0; k--) {
        int i = p->z[k];
        if (c->wins[i].minimised) continue;
        if (!pt_in(win_part(c, W, H, i, WP_FRAME), x, y)) continue;
        p->hover_win = i;
        for (int part = WP_MIN; part <= WP_CLOSE; part++)
            if (pt_in(win_part(c, W, H, i, part), x, y)) { p->hover_btn = part; break; }
        return;
    }
}

static void cycle(shell_ctx *c, int dir)
{
    if (c->n_wins < 1) return;
    int cur = (c->focus >= 0 && c->focus < c->n_wins) ? c->focus : 0;
    int nxt = ((cur + dir) % c->n_wins + c->n_wins) % c->n_wins;
    restore_win(c, nxt);        /* cycling into a minimised window brings it back */
}

static void l_key(shell_ctx *c, int k)
{
    tb_priv *p = P(c);
    int cols, rows, shown;
    menu_shape(c, p->scr_w, p->scr_h, &cols, &rows, &shown);

    switch (k) {
    case 125:                                    /* KEY_LEFTMETA: the menu key */
        set_menu(c, !p->menu_open);
        if (p->menu_open && p->hover_row < 0) p->hover_row = 0;
        break;
    case 1:                                      /* KEY_ESC */
        if (p->menu_open) set_menu(c, 0);
        break;
    case 28:                                     /* KEY_ENTER */
        if (p->menu_open && p->hover_row >= 0 && p->hover_row < shown) {
            open_app(c, p->hover_row); set_menu(c, 0);
        }
        break;
    case 15:  cycle(c, 1); break;                /* KEY_TAB */
    case 103:                                    /* KEY_UP   */
        if (p->menu_open && shown) p->hover_row = (p->hover_row <= 0) ? shown - 1 : p->hover_row - 1;
        break;
    case 108:                                    /* KEY_DOWN */
        if (p->menu_open && shown) p->hover_row = (p->hover_row + 1) % shown;
        break;
    case 105:                                    /* KEY_LEFT  */
        if (p->menu_open && shown) p->hover_row = (p->hover_row - rows + shown) % shown;
        else cycle(c, -1);
        break;
    case 106:                                    /* KEY_RIGHT */
        if (p->menu_open && shown) p->hover_row = (p->hover_row + rows) % shown;
        else cycle(c, 1);
        break;
    default: break;
    }
}

static int l_step(shell_ctx *c, float dt)
{
    tb_priv *p = P(c);
    int busy = tween_step(&p->menu, dt);
    for (int i = 0; i < c->n_wins; i++)
        busy |= tween_step(&p->shade[i], dt);
    return busy;
}

static void l_fini(shell_ctx *c) { c->priv = NULL; }

const shell_layout layout_taskbar = {
    "taskbar", l_init, l_paint, l_click, l_motion, l_key, l_step, l_fini
};
