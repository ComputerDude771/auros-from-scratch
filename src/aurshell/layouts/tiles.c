/* layouts/tiles.c — the "One thing at a time" archetype.
 *
 * Boot lands on a page of large labelled buttons. Press one and it grows
 * into the whole screen. A single Home control, always in the same place
 * along the bottom, brings the page back. Nothing overlaps, nothing
 * minimises, nothing is ever hidden behind anything else.
 *
 * The point: for someone whose only computer is a phone, "a desktop" is
 * a second thing to learn before they can do the first thing — windows
 * that move, stack, shrink to an icon and reappear somewhere else. This
 * archetype asks them to learn nothing: it is a phone home screen at
 * desk size, and it behaves the way their thumb already expects.
 *
 * The cost is on the tin in tiles.shell and is real: you cannot see two
 * things at once, and switching goes via Home. What it buys is a
 * guarantee no other archetype here can make — there is exactly ONE
 * control for moving around, it is always drawn, it is always in the
 * same pixels, and nothing can ever cover it. Every other way of getting
 * lost needs a second place for things to be. This has no second place.
 */
#include "../shell.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define OPEN_SECS   0.42f
#define CLOSE_SECS  0.34f
#define DOTS_BAND   34      /* the page-dot strip, directly above Home   */
#define DOT_PITCH   30      /* also the dot's hit width: big targets     */

typedef struct {
    tween open;      /* 0 = the page of buttons, 1 = one thing, full screen */
    int   win;       /* the window being shown or shrinking, -1 for none    */
    int   page;      /* which page of buttons                               */
    int   hot;       /* app index under the pointer / key cursor, -1 none   */
    int   home_hot;  /* pointer is over the Home control                    */
    int   sw, sh;    /* the size we last PAINTED — see l_click              */
} tiles_priv;

static tiles_priv *P(shell_ctx *c) { return (tiles_priv *)c->priv; }

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerpf (float a, float b, float t) { return a + (b - a) * t; }

/* Font metrics with fallbacks, because a theme may name a font that did
 * not load and a NULL font must degrade to a plausible layout rather
 * than to nonsense. Ascent and descent only: every block in this file
 * is measured as the ink the glyphs occupy, never as line height, so
 * that a measurement and the baseline derived from it cannot disagree. */
static float fa (font *f, float d) { return f ? font_ascent(f)  : d; }
static float fdc(font *f, float d) { return f ? font_descent(f) : d; }

/* Blend two THEME colours channel-wise. Not a hardcoded colour: both
 * ends come from the theme, and only the point between them is
 * computed. Used where a label has to stay one crisp glyph while the
 * surface under it changes colour — drawing the same word twice in two
 * colours at half alpha gives two ghosts and no contrast. */
static uint32_t mix_rgb(uint32_t a, uint32_t b, float t)
{
    float k = clampf(t, 0.f, 1.f);
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        float ca = (float)((a >> sh) & 0xFFu), cb = (float)((b >> sh) & 0xFFu);
        out |= ((uint32_t)(ca + (cb - ca) * k) & 0xFFu) << sh;
    }
    return out;
}

static rect rect_lerp(rect a, rect b, float t)
{
    rect r;
    r.x = (int)lerpf((float)a.x, (float)b.x, t);
    r.y = (int)lerpf((float)a.y, (float)b.y, t);
    r.w = (int)lerpf((float)a.w, (float)b.w, t);
    r.h = (int)lerpf((float)a.h, (float)b.h, t);
    return r;
}

/* Paint-only: used to let the page recede while something opens over it.
 * Deliberately NOT part of tile_rect — the grid is not hit-testable
 * during the transition, so a scale that only exists in the painter can
 * never put a click somewhere the user did not aim. */
static rect rect_scale(rect r, float k)
{
    rect o;
    o.w = (int)((float)r.w * k); o.h = (int)((float)r.h * k);
    o.x = r.x + (r.w - o.w) / 2; o.y = r.y + (r.h - o.h) / 2;
    return o;
}

/* ── the two permanent bands ─────────────────────────────────────────
 * Both are chrome, both are painted last, and the open thing's rect is
 * defined to stop short of them. That is the whole anti-dead-end
 * mechanism, expressed as arithmetic rather than as a promise: there is
 * no value of anything for which Home is off screen or covered. */
static int  strip_h  (shell_ctx *c) { return c->bar_h + 8; }
static int  homebar_h(shell_ctx *c) { return c->target_large ? 76 : 58; }

static rect homebar_rect(shell_ctx *c, int w, int h)
{
    rect r = { 0, h - homebar_h(c), w, homebar_h(c) };
    return r;
}

static rect home_btn_rect(shell_ctx *c, int w, int h)
{
    int bh  = homebar_h(c);
    int hgt = bh - (c->target_large ? 18 : 14);
    int wid = c->target_large ? 240 : 180;
    int max = w - c->margin * 4;
    if (wid > max) wid = max > 60 ? max : 60;
    rect r = { (w - wid) / 2, h - bh + (bh - hgt) / 2, wid, hgt };
    return r;
}

/* The dot strip's top edge, owned here so grid_metrics() can leave room
 * above it and page_dot_rect() can put the dots in it without the two
 * drifting apart. */
static int dots_band_y(shell_ctx *c, int h)
{
    return h - homebar_h(c) - c->margin - DOTS_BAND;
}

/* The area a thing fills when it is open: everything that is not
 * permanent chrome. */
static rect full_rect(shell_ctx *c, int w, int h)
{
    int top = strip_h(c);
    int bot = h - homebar_h(c);
    rect r = { 0, top, w, bot - top > 1 ? bot - top : 1 };
    return r;
}

/* ── grid geometry, owned in one place ───────────────────────────────
 * Painting and hit-testing both come through grid_metrics()/tile_rect().
 * Deriving the two separately is how a UI ends up off by the width of a
 * shadow; here it would be worse than cosmetic, because the entire
 * promise of this archetype is that pressing a picture of a thing opens
 * that thing. */
typedef struct { int cols, rows, per, pages, tw, th, gap, x0, y0; } grid_m;

static grid_m grid_metrics(shell_ctx *c, int w, int h)
{
    grid_m g;
    /* target_large is the .shell file saying this person aims with an
     * unsteady hand and reads with glasses on: fewer, bigger buttons. */
    g.cols = c->target_large ? 4 : 5;
    if (w < 1150) g.cols = c->target_large ? 3 : 4;
    if (w <  800) g.cols = 2;
    g.rows = c->target_large ? 2 : 3;
    g.per  = g.cols * g.rows;

    int n = c->n_apps > 0 ? c->n_apps : 1;
    g.pages = (n + g.per - 1) / g.per;

    g.gap = c->padding + 8;
    int left  = c->margin * 2;
    int right = w - c->margin * 2;
    int top   = strip_h(c) + c->margin;
    /* Reserve the dot strip only when there is a second page, so a
     * one-page grid does not carry a band of nothing. */
    int bot   = (g.pages > 1) ? dots_band_y(c, h) - c->padding
                              : h - homebar_h(c) - c->margin;

    int availw = (right - left) - g.gap * (g.cols - 1);
    int availh = (bot - top)    - g.gap * (g.rows - 1);
    if (availw < g.cols) availw = g.cols;
    if (availh < g.rows) availh = g.rows;

    int tw = availw / g.cols, th = availh / g.rows;
    /* Fill the space, but cap the aspect: a button stretched to 2:1 by a
     * wide screen stops reading as one target and starts reading as a
     * row. Whatever is left over becomes even margin, not dead space at
     * one edge. */
    if (tw > th * 5 / 4) tw = th * 5 / 4;
    if (th > tw * 5 / 4) th = tw * 5 / 4;
    g.tw = tw; g.th = th;

    int bw = tw * g.cols + g.gap * (g.cols - 1);
    int bh = th * g.rows + g.gap * (g.rows - 1);
    g.x0 = left + ((right - left) - bw) / 2;
    g.y0 = top  + ((bot - top)    - bh) / 2;
    return g;
}

static int page_of_app(shell_ctx *c, int app, int w, int h)
{
    grid_m g = grid_metrics(c, w, h);
    int p = app / g.per;
    return (p < 0) ? 0 : (p >= g.pages ? g.pages - 1 : p);
}

/* Keyed by GLOBAL app index rather than by slot, because that is the
 * only identity the rest of the layout cares about: the tile a thing
 * came from is the tile it shrinks back into, whatever page you are on
 * and whatever the screen got resized to in between. */
static rect tile_rect(shell_ctx *c, int w, int h, int app)
{
    grid_m g = grid_metrics(c, w, h);
    int page = app / g.per;
    int slot = app - page * g.per;
    int row  = slot / g.cols, col = slot % g.cols;

    /* A last page holding two buttons must not read as a page with six
     * missing ones, so a short final row (and a short page) is centred
     * inside the full block instead of being left-and-top aligned. */
    int on_page = c->n_apps - page * g.per;
    if (on_page > g.per) on_page = g.per;
    if (on_page < 1)     on_page = 1;
    int rows_used = (on_page + g.cols - 1) / g.cols;
    int in_row = (row == rows_used - 1) ? on_page - row * g.cols : g.cols;
    if (in_row > g.cols) in_row = g.cols;
    if (in_row < 1)      in_row = 1;

    int bw = g.tw * in_row    + g.gap * (in_row - 1);
    int bh = g.th * rows_used + g.gap * (rows_used - 1);
    int fw = g.tw * g.cols    + g.gap * (g.cols - 1);
    int fh = g.th * g.rows    + g.gap * (g.rows - 1);

    rect r;
    r.x = g.x0 + (fw - bw) / 2 + col * (g.tw + g.gap);
    r.y = g.y0 + (fh - bh) / 2 + row * (g.th + g.gap);
    r.w = g.tw; r.h = g.th;
    return r;
}

static rect page_dot_rect(shell_ctx *c, int w, int h, int i, int pages)
{
    int x0 = w / 2 - (pages * DOT_PITCH) / 2;
    rect r = { x0 + i * DOT_PITCH, dots_band_y(c, h), DOT_PITCH, DOTS_BAND };
    return r;
}

/* Where a thing shrinks back to. A window with no app entry has no
 * button to return into, so it collapses to a point at the middle of the
 * grid rather than to (0,0), which would fly off the corner. */
static rect origin_rect(shell_ctx *c, int w, int h, int wi)
{
    if (wi >= 0 && wi < c->n_wins && c->wins[wi].app >= 0 && c->wins[wi].app < c->n_apps)
        return tile_rect(c, w, h, c->wins[wi].app);
    grid_m g = grid_metrics(c, w, h);
    rect r = { w / 2 - g.tw / 2, g.y0 + (g.th * g.rows) / 2 - g.th / 2, g.tw, g.th };
    return r;
}

/* Bounded copy that does not go through snprintf, whose restrict
 * contract forbids the source and destination sharing an object — and
 * here both live inside the same shell_ctx. */
static void copy_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int app_window(shell_ctx *c, int app)
{
    for (int i = 0; i < c->n_wins; i++)
        if (c->wins[i].app == app) return i;
    return -1;
}

/* ── state changes ───────────────────────────────────────────────── */

static void open_app(shell_ctx *c, int app, int w, int h)
{
    tiles_priv *p = P(c);
    if (app < 0 || app >= c->n_apps) return;

    int wi = app_window(c, app);
    if (wi < 0) {
        /* Starting a program is the compositor's job and the shell
         * contract has no hook for it, so a placeholder entry stands in.
         * Without one, pressing a button for something not already
         * running would do nothing at all — the single outcome this
         * archetype is not allowed to have. */
        if (c->n_wins >= SHELL_MAX_WINS) return;
        wi = c->n_wins++;
        memset(&c->wins[wi], 0, sizeof c->wins[wi]);
        c->wins[wi].app = app;
        /* memcpy rather than snprintf: source and destination are both
         * inside *c, which is exactly the aliasing snprintf is allowed
         * to assume away. */
        copy_str(c->wins[wi].title,    sizeof c->wins[wi].title,    c->apps[app].name);
        copy_str(c->wins[wi].subtitle, sizeof c->wins[wi].subtitle, c->apps[app].hint);
    }
    p->win  = wi;
    p->page = page_of_app(c, app, w, h);
    c->focus = wi;
    /* EASE_OUT_CUBIC, not the theme's spring: a spring overshoots past
     * 1, and past 1 means a button drawn larger than the screen it is
     * growing into. Overshoot is charm everywhere except at the edges. */
    tween_to(&p->open, 1.f, OPEN_SECS, EASE_OUT_CUBIC);
}

static void go_home(shell_ctx *c)
{
    tiles_priv *p = P(c);
    p->hot = -1;
    if (p->win < 0) { p->page = 0; return; }   /* already home: first page */
    tween_to(&p->open, 0.f, CLOSE_SECS, EASE_IN_OUT_CUBIC);
    /* c->focus is left alone until the shrink lands: the thing you are
     * on is still the thing you are on until it is visibly back in its
     * button. l_step clears it. */
}

/* ── painting ────────────────────────────────────────────────────── */

static void paint_tile(shell_ctx *c, surface *s, shell_fonts *f, int app,
                       rect t, float alpha)
{
    tiles_priv *p = P(c);
    int hot = (p->hot == app);
    int run = app_window(c, app) >= 0;
    uint32_t tint = c->apps[app].tint;
    corners cr = corners_all((float)c->radius);

    if (hot)
        draw_round_rect_shadow(s, t, cr, (float)c->shadow_r * 0.7f, 0x000000,
                               c->shadow_a * 0.6f * alpha, 6);
    draw_round_rect(s, t, cr, hot ? c->surface_hi : c->surface_c,
                    alpha * (hot ? 1.f : c->panel_a));
    draw_round_rect_border(s, t, cr,
                           hot ? (float)c->border : (run ? 2.f : 1.f),
                           (hot || run) ? tint : c->overlay,
                           alpha * (hot ? 0.95f : (run ? 0.6f : 0.7f)));

    /* Centre icon + name (+ hint) as one block. Pinning the icon to a
     * fraction of the tile height leaves dead air under the text on a
     * tall tile, and dead air inside a button reads as "something is
     * missing here". */
    float icx  = (float)(t.x + t.w / 2);
    /* The icon is the target; the words confirm it. Capped so a 4K tile
     * does not get a 200px glyph, but high enough that a large tile is
     * not a big empty box with a small picture floating in it. */
    float isz  = clampf((float)t.h * 0.32f, 30.f, 128.f);
    font *nf   = c->target_large ? f->big : f->mid;

    /* Measure the block with ascent+descent, the ink the glyphs actually
     * occupy, and NOT with line height. Line height carries the font's
     * leading, so a block sized by it is shorter than what gets drawn —
     * which centres fine on a roomy tile and drops the hint's descenders
     * through the bottom border on a 600px-tall screen. Same numbers
     * here as in the baselines below, by construction. */
    float la = fa(nf, 22.f), ld = fdc(nf, 6.f);
    float ha = fa(f->small, 13.f), hd = fdc(f->small, 3.f);
    float head  = isz * 1.56f + 18.f;
    float block = head + la + ld + 10.f + ha + hd;
    /* The hint is the first thing to go when the tile is short. Deciding
     * that from the measured block rather than from a magic tile height
     * means it degrades at exactly the size where it would stop fitting,
     * whatever font the theme named. */
    int hint = (block <= (float)t.h - 12.f);
    if (!hint) block = head + la + ld;
    float top   = (float)t.y + ((float)t.h - block) * 0.5f;
    float icy   = top + isz * 0.78f;

    draw_circle(s, icx, icy, isz * 0.78f, tint, alpha * (hot ? 0.22f : 0.14f));
    shell_icon_draw(s, c->apps[app].icon, icx, icy, isz, tint, alpha * (hot ? 1.f : 0.92f));

    float ly = top + head + la;
    shell_text_centred(s, nf, icx, ly, c->apps[app].name, c->fg_hi, alpha * 0.97f);
    if (hint)
        shell_text_centred(s, f->small, icx, ly + ld + 10.f + ha,
                           c->apps[app].hint, c->subtle, alpha * 0.78f);

    /* Running marker. "Go back to the thing I was doing" is the one
     * question this archetype makes the user work for — switching goes
     * via Home — so the grid has to answer it without being asked. A dot
     * in the tint, ringed so it survives both a light and a dark
     * surface underneath. */
    if (run) {
        float dx = (float)(t.x + t.w) - (float)c->padding - 12.f;
        float dy = (float)t.y + (float)c->padding + 12.f;
        draw_circle(s, dx, dy, 9.f, tint, alpha * 0.22f);
        draw_circle(s, dx, dy, 5.f, tint, alpha * 0.95f);
    }
}

/* The growing thing. One composition, two sets of words: the button's
 * own label fades out over the first third of the move, the app's own
 * title fades in over the last third, and the icon scales continuously
 * across the whole of it. That continuity IS the explanation — the user
 * watches the button they pressed become the screen, so they never have
 * to be told where this came from or where the page went. */
static void paint_open(shell_ctx *c, surface *s, shell_fonts *f,
                       rect a, float rad, int wi, float t)
{
    const win_entry *win = &c->wins[wi];
    int app = (win->app >= 0 && win->app < c->n_apps) ? win->app : -1;
    uint32_t tint = (app >= 0) ? c->apps[app].tint : c->accent;
    shell_icon ic = (app >= 0) ? c->apps[app].icon : ICON_WINDOW;
    corners cr = corners_all(rad);

    if (t < 0.995f) {
        draw_round_rect_shadow(s, a, cr, (float)c->shadow_r * 0.9f, 0x000000,
                               c->shadow_a * (1.f - t), 8);
        draw_blur_region(s, a, c->blur_r);
    }
    /* Opaque by the time it is full screen: a translucent "full screen"
     * shows the wallpaper through the thing you are working in, which
     * puts two things on screen again through the back door. */
    draw_round_rect(s, a, cr, c->surface_c, lerpf(c->panel_a, 1.f, t));
    if (rad > 0.5f)
        draw_round_rect_border(s, a, cr, (float)c->border, tint, 0.85f * (1.f - t));

    if (win->content)
        draw_scaled_rounded(s, win->content, a, cr, t);

    /* Text does not scale, so it cannot ride the move the way the icon
     * does: it leaves early and arrives late, and for the fifth of a
     * second in between only the icon is on the card. Overlapping the
     * two sets instead just prints one on top of the other. */
    float ta = clampf(1.f - t * 2.5f, 0.f, 1.f);        /* the button's words  */
    float tb = clampf(t * 2.5f - 1.5f, 0.f, 1.f);       /* the app's own words */

    float icx = (float)(a.x + a.w / 2);
    float isz = clampf((float)a.h * 0.26f, 30.f, 132.f);
    font *nf  = c->target_large ? f->big : f->mid;

    /* Two block heights — the button's and the app's — and the icon
     * rides between them, so each end of the move is exactly centred and
     * the middle, where no text is drawn at all, simply glides. Measured
     * in ink, as in paint_tile, and for the same reason. */
    float la = fa(nf, 22.f),        ld = fdc(nf, 6.f);
    float Ta = fa(f->huge, 32.f),   Td = fdc(f->huge, 10.f);
    float Sa = fa(f->mid, 16.f),    Sd = fdc(f->mid, 5.f);
    float ha = fa(f->small, 13.f),  hd = fdc(f->small, 3.f);
    float head = isz * 1.56f + 18.f;
    float blkA = head + la + ld + 10.f + ha + hd;
    /* Wider leading between the full-screen lines than between a tile's
     * two: the same 10px that separates 26px and 13px type reads as a
     * collision under a 40px title. */
    float blkB = head + Ta + Td + 18.f + Sa + Sd + 14.f + ha + hd;
    float top  = lerpf((float)a.y + ((float)a.h - blkA) * 0.5f,
                       (float)a.y + ((float)a.h - blkB) * 0.5f, t);
    float icy  = top + isz * 0.78f;

    /* With live content the icon is scaffolding for the move and nothing
     * more, so it gets out of the way once the move is over. */
    float ia = win->content ? (1.f - t) : 1.f;
    draw_circle(s, icx, icy, isz * 0.78f, tint, ia * 0.16f);
    shell_icon_draw(s, ic, icx, icy, isz, tint, ia * 0.95f);

    if (ta > 0.f && app >= 0) {
        float ly = top + head + la;
        shell_text_centred(s, nf, icx, ly, c->apps[app].name, c->fg_hi, ta * 0.97f);
        shell_text_centred(s, f->small, icx, ly + ld + 10.f + ha,
                           c->apps[app].hint, c->subtle, ta * 0.78f);
    }
    if (tb > 0.f && !win->content) {
        float ly = top + head + Ta;
        shell_text_centred(s, f->huge, icx, ly, win->title, c->fg_hi, tb * 0.97f);
        float sy = ly + Td + 18.f + Sa;
        shell_text_centred(s, f->mid, icx, sy,
                           win->subtitle[0] ? win->subtitle : "Opening…",
                           c->subtle, tb * 0.85f);
        /* Only ever shown on the placeholder, i.e. exactly when there is
         * nothing here yet and the user is most likely to wonder whether
         * they have broken something. */
        float hy = sy + Sd + 14.f + ha;
        shell_text_centred(s, f->small, icx, hy,
                           "Press Home below to come back.", c->muted, tb * 0.95f);
    }
}

static void paint_dots(shell_ctx *c, surface *s, shell_fonts *f,
                       int w, int h, int pages, float alpha)
{
    if (pages < 2 || alpha <= 0.01f) return;
    tiles_priv *p = P(c);
    grid_m g = grid_metrics(c, w, h);

    /* Dots alone are a small affordance for someone who is new to this,
     * so they are spelled out in words as well. The words sit at the
     * grid's left edge and the dots stay centred, which also keeps the
     * dot geometry independent of a font — l_click has no fonts. */
    char lab[40];
    snprintf(lab, sizeof lab, "Page %d of %d", p->page + 1, pages);
    rect b0 = page_dot_rect(c, w, h, 0, pages);
    shell_text(s, f->small, (float)g.x0,
               shell_baseline(f->small, (float)b0.y, (float)DOTS_BAND),
               lab, c->subtle, alpha * 0.8f);

    for (int i = 0; i < pages; i++) {
        rect d = page_dot_rect(c, w, h, i, pages);
        int cur = (i == p->page);
        draw_circle(s, (float)d.x + (float)d.w * 0.5f,
                    (float)d.y + (float)DOTS_BAND * 0.5f,
                    cur ? 6.5f : 4.5f, cur ? c->accent : c->fg,
                    alpha * (cur ? 1.f : 0.32f));
    }
}

static void paint_strip(shell_ctx *c, surface *s, shell_fonts *f, int w, float t)
{
    tiles_priv *p = P(c);
    int bh = strip_h(c);
    rect r = { 0, 0, w, bh };

    draw_blur_region(s, r, c->blur_r);
    draw_rect(s, r, c->bg, 0.58f);
    draw_line(s, 0, (float)bh, (float)w, (float)bh, 1.f, c->overlay, 0.5f);

    draw_circle(s, (float)(c->margin + 8), (float)(bh / 2), 6.f, c->accent, 1.f);
    shell_text(s, f->small, (float)(c->margin + 24),
               shell_baseline(f->small, 0.f, (float)bh), c->brand, c->subtle, 0.9f);

    if (c->show_clock) {
        char hm[32], dt[48];
        shell_clock(hm, sizeof hm, dt, sizeof dt);
        float by = shell_baseline(f->small, 0.f, (float)bh);
        float rx = (float)(w - c->margin);
        shell_text(s, f->small, rx - shell_text_w(f->small, hm), by, hm, c->fg_hi, 0.95f);
        rx -= shell_text_w(f->small, hm) + 14.f;
        shell_text(s, f->small, rx - shell_text_w(f->small, dt), by, dt, c->subtle, 0.75f);
    }

    /* The centre of the strip is a single slot that says where you are,
     * and it crossfades in place. The question and the answer occupying
     * the same pixels is the point: there is one "where am I", not a
     * title bar that appears and a greeting that disappears. Nothing
     * here is clickable — this is NOT a taskbar, and a strip you can
     * launch or switch from would re-create the second place to look
     * that this whole archetype exists to delete. */
    float cx = (float)w * 0.5f;
    float qa = clampf(1.f - t * 2.5f, 0.f, 1.f);
    float na = clampf(t * 2.5f - 1.5f, 0.f, 1.f);
    if (qa > 0.f)
        shell_text_centred(s, f->small, cx, shell_baseline(f->small, 0.f, (float)bh),
                           "What would you like to do?", c->subtle, qa * 0.85f);
    if (na > 0.f && p->win >= 0) {
        const win_entry *win = &c->wins[p->win];
        uint32_t tint = (win->app >= 0) ? c->apps[win->app].tint : c->accent;
        shell_icon ic = (win->app >= 0) ? c->apps[win->app].icon : ICON_WINDOW;
        float tw = shell_text_w(f->mid, win->title);
        float gx = cx - (tw + 28.f) * 0.5f;
        shell_icon_draw(s, ic, gx + 9.f, (float)bh * 0.5f, 18.f, tint, na * 0.95f);
        shell_text(s, f->mid, gx + 28.f, shell_baseline(f->mid, 0.f, (float)bh),
                   win->title, c->fg_hi, na * 0.97f);
    }
}

static void paint_homebar(shell_ctx *c, surface *s, shell_fonts *f, int w, int h, float t)
{
    tiles_priv *p = P(c);
    rect bar = homebar_rect(c, w, h);

    draw_blur_region(s, bar, c->blur_r);
    draw_rect(s, bar, c->bg_alt, 0.75f);
    draw_line(s, 0, (float)bar.y, (float)w, (float)bar.y, 1.f, c->overlay, 0.7f);

    rect b = home_btn_rect(c, w, h);
    corners cr = corners_all((float)b.h * 0.5f);
    int hot = p->home_hot;

    /* Quiet when you are already here, filled and loud when you are not
     * — but never absent, never moved and never a different size. The
     * user learns one target once.
     *
     * The state change runs at twice the speed of the move, on purpose:
     * halfway through, this control is the one thing on screen that must
     * not be a half-blended nothing-colour. It commits early on the way
     * out and gives up late on the way back. */
    float k = clampf(t * 2.f, 0.f, 1.f);
    if (k > 0.005f)
        draw_round_rect_shadow(s, b, cr, (float)c->shadow_r * 0.6f, 0x000000,
                               c->shadow_a * k * 0.8f, 5);
    draw_round_rect(s, b, cr, hot ? c->surface_hi : c->surface_c,
                    (1.f - k) * (hot ? 1.f : c->panel_a));
    draw_round_rect_border(s, b, cr, 1.5f, c->overlay, (1.f - k) * 0.75f);
    if (k > 0.005f)
        draw_round_rect(s, b, cr, c->accent, k * (hot ? 1.f : 0.94f));

    /* The word, not a glyph. The shared icon set has no house and no
     * grid, and inventing one here would fork the set six ways — which
     * is exactly what shellcommon.c exists to prevent. It is also the
     * better call on its own merits: an icon that needs interpreting is
     * a label that failed, and this is the one control that cannot
     * afford to be interpreted.
     *
     * On the accent fill the label is drawn in the background colour.
     * That is the one colour a theme guarantees contrasts with its
     * accent — the accent was chosen to be legible against it — so this
     * stays readable in Nocturne's pale mint and in Sandstone's
     * terracotta without a single hardcoded value. */
    font *hf = c->target_large ? f->big : f->mid;
    float by = shell_baseline(hf, (float)b.y, (float)b.h);
    float bx = (float)(b.x + b.w / 2);
    shell_text_centred(s, hf, bx, by, "Home", mix_rgb(c->fg, c->bg, k), 0.85f + 0.15f * k);
}

static void l_paint(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall)
{
    tiles_priv *p = P(c);
    int w = s->w, h = s->h;

    /* Remember what we actually drew. l_click gets screen coordinates
     * but no surface, and guessing a size there would put every hit
     * rect in the wrong place on any display that is not the one the
     * guess was written for. */
    p->sw = w; p->sh = h;

    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else surface_fill(s, 0xFF000000u | c->bg);

    float t = clampf(p->open.value, 0.f, 1.f);
    grid_m g = grid_metrics(c, w, h);

    if (p->page >= g.pages) p->page = g.pages - 1;
    if (p->page < 0) p->page = 0;
    /* While something is open, the page under it is the page that thing
     * lives on — so closing always puts you back where you pressed. */
    if (p->win >= 0 && c->wins[p->win].app >= 0)
        p->page = page_of_app(c, c->wins[p->win].app, w, h);

    if (t < 0.999f) {
        int first = p->page * g.per;
        int last  = first + g.per;
        if (last > c->n_apps) last = c->n_apps;
        for (int i = first; i < last; i++) {
            if (t > 0.f && p->win >= 0 && c->wins[p->win].app == i)
                continue;                       /* this one IS the growing card */
            rect r = tile_rect(c, w, h, i);
            if (t <= 0.001f) draw_blur_region(s, r, c->blur_r);
            else             r = rect_scale(r, 1.f - 0.07f * t);
            paint_tile(c, s, f, i, r, 1.f - t);
        }
        paint_dots(c, s, f, w, h, g.pages, 1.f - t);
    }

    if (p->win >= 0)
        paint_open(c, s, f,
                   rect_lerp(origin_rect(c, w, h, p->win), full_rect(c, w, h), t),
                   (float)c->radius * (1.f - t), p->win, t);

    /* Chrome last, always. Nothing the user opens can paint over the one
     * way back. */
    paint_strip(c, s, f, w, t);
    paint_homebar(c, s, f, w, h, t);
}

/* ── input ───────────────────────────────────────────────────────── */

static int inside(rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int l_click(shell_ctx *c, int x, int y)
{
    tiles_priv *p = P(c);
    int w = p->sw, h = p->sh;
    float t = clampf(p->open.value, 0.f, 1.f);

    /* Home is tested first and wins every ambiguity, by construction. */
    if (inside(home_btn_rect(c, w, h), x, y)) { go_home(c); return 1; }
    if (inside(homebar_rect(c, w, h), x, y))  return 1;   /* chrome swallows the rest */

    /* Mid-transition nothing is where it will be, so no click is
     * honoured: half-delivered presses are how a user ends up somewhere
     * they did not choose. */
    if (t > 0.001f && t < 0.999f) return 1;

    /* Open and settled: the screen belongs to the client. Not consumed,
     * so the compositor routes it there. */
    if (t >= 0.999f) return 0;

    grid_m g = grid_metrics(c, w, h);
    if (g.pages > 1)
        for (int i = 0; i < g.pages; i++)
            if (inside(page_dot_rect(c, w, h, i, g.pages), x, y)) {
                p->page = i; p->hot = -1; return 1;
            }

    int first = p->page * g.per, last = first + g.per;
    if (last > c->n_apps) last = c->n_apps;
    for (int i = first; i < last; i++)
        if (inside(tile_rect(c, w, h, i), x, y)) { open_app(c, i, w, h); return 1; }

    return 0;
}

static void l_motion(shell_ctx *c, int x, int y)
{
    tiles_priv *p = P(c);
    int w = p->sw, h = p->sh;
    c->mouse_x = x; c->mouse_y = y;

    p->home_hot = inside(home_btn_rect(c, w, h), x, y);
    p->hot = -1;
    if (clampf(p->open.value, 0.f, 1.f) > 0.001f) return;

    grid_m g = grid_metrics(c, w, h);
    int first = p->page * g.per, last = first + g.per;
    if (last > c->n_apps) last = c->n_apps;
    for (int i = first; i < last; i++)
        if (inside(tile_rect(c, w, h, i), x, y)) { p->hot = i; return; }
}

/* The keyboard is a bonus here, not the model (keyboard_optional=yes),
 * so it drives the same single highlight the pointer does rather than
 * growing a second notion of "selected". */
static void move_sel(shell_ctx *c, int dcol, int drow)
{
    tiles_priv *p = P(c);
    grid_m g = grid_metrics(c, p->sw, p->sh);
    if (c->n_apps <= 0) return;

    if (p->hot < 0) { p->hot = p->page * g.per; if (p->hot >= c->n_apps) p->hot = c->n_apps - 1; return; }

    int page = p->hot / g.per, slot = p->hot - page * g.per;
    int col = slot % g.cols + dcol, row = slot / g.cols + drow;
    if (col < 0)       { page--; col = g.cols - 1; }
    if (col >= g.cols) { page++; col = 0; }
    if (page < 0)        { page = 0; col = 0; }
    if (page >= g.pages) { page = g.pages - 1; col = g.cols - 1; }
    if (row < 0) row = 0;
    if (row >= g.rows) row = g.rows - 1;

    int n = page * g.per + row * g.cols + col;
    if (n >= c->n_apps) n = c->n_apps - 1;
    if (n < 0) n = 0;
    p->hot  = n;
    p->page = n / g.per;
}

static void l_key(shell_ctx *c, int k)
{
    tiles_priv *p = P(c);
    float t = clampf(p->open.value, 0.f, 1.f);
    switch (k) {
        case 1:                                    /* KEY_ESC   */
        case 102: go_home(c); break;               /* KEY_HOME  */
        case 28:                                   /* KEY_ENTER */
            if (t <= 0.001f && p->hot >= 0) open_app(c, p->hot, p->sw, p->sh);
            break;
        /* Arrows never switch between open things: that would be a second
         * way to move around, and the tradeoff this archetype signed up
         * for is that switching goes via Home. */
        case 105: if (t <= 0.001f) move_sel(c, -1,  0); break;   /* LEFT  */
        case 106: if (t <= 0.001f) move_sel(c, +1,  0); break;   /* RIGHT */
        case 103: if (t <= 0.001f) move_sel(c,  0, -1); break;   /* UP    */
        case 108: if (t <= 0.001f) move_sel(c,  0, +1); break;   /* DOWN  */
        default: break;
    }
}

static int l_step(shell_ctx *c, float dt)
{
    tiles_priv *p = P(c);
    int moving = tween_step(&p->open, dt);
    if (!moving && p->open.value <= 0.001f && p->win >= 0) {
        p->win = -1;
        c->focus = -1;      /* back on the page of buttons, nothing is "on" */
    }
    return moving;
}

static void l_init(shell_ctx *c)
{
    static tiles_priv priv;
    memset(&priv, 0, sizeof priv);
    priv.hot = -1;
    priv.win = -1;
    priv.sw = 1600; priv.sh = 900;   /* replaced by the first paint */
    tween_set(&priv.open, 0.f);
    c->priv = &priv;

    /* Boot lands on the page of buttons. The one exception is a session
     * that already has something focused — a restored session, or the
     * preview harness — which is shown straight away with no animation,
     * because a transition nobody asked for explains nothing. */
    if (c->focus >= 0 && c->focus < c->n_wins) {
        priv.win = c->focus;
        if (c->wins[priv.win].app >= 0)
            priv.page = page_of_app(c, c->wins[priv.win].app, priv.sw, priv.sh);
        tween_set(&priv.open, 1.f);
    } else {
        c->focus = -1;
    }
}

static void l_fini(shell_ctx *c) { c->priv = NULL; }

const shell_layout layout_tiles = {
    "tiles", l_init, l_paint, l_click, l_motion, l_key, l_step, l_fini
};
