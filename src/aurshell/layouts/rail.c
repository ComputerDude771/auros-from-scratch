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

/* How tall the Home card's headline block is. Deliberately FONT-FREE:
 * l_motion hit-tests the rows and has no fonts to measure with, so a
 * header measured from a font here and from a constant there would put
 * the hover highlight a line away from the row under the pointer. A
 * fraction of the card, clamped, is the same number in both passes on
 * every panel. paint_home then sets the type INSIDE this band. */
static int home_head(shell_ctx *c, rect a)
{
    /* Measured from the THEME's type scale rather than from the loaded
     * font: font_size_lg is what load_fonts builds the display steps
     * from, so this tracks a retheme without either pass having to
     * open a font. Guessing a fraction of the card instead put the
     * closing rule straight through the sub-line on a 1024x600 panel,
     * where a serif's ascent is a larger share of a shorter card. */
    float em = (float)theme_int(&c->theme, "font_size_lg", 19) * 2.25f;
    float sm = (float)theme_int(&c->theme, "font_size", 13);
    int head = c->padding + 10 + (int)(em * 1.45f) + 14 + (int)(sm * 1.5f) + 40;
    int lo = (int)((float)a.h * 0.18f), hi = (int)((float)a.h * 0.42f);
    if (head < lo) head = lo;
    if (head > hi) head = hi;
    return head;
}

/* Home is an INDEX, not a grid of cards.
 *
 * It used to be COLS=3 ROWS=2: six identical rounded rectangles, each
 * with a tinted halo disc, a centred icon, a centred name and a
 * centred hint. That is the forbidden pattern exactly, and it is also
 * a poor answer to the question the card is asking — six equal boxes
 * say "these are six of the same kind of thing", when what the user
 * needs is a list they can read down.
 *
 * So: full-bleed rows, one per app, ruled apart by a hairline, with
 * the first entry set noticeably larger than the rest. The rhythm is
 * uneven ON PURPOSE (LEAD below); a list whose every line is the same
 * height has no first item, and a screen with no first item gives the
 * eye nowhere to start.
 *
 * These rows are hover-only: l_click never asks for them (a Home card
 * that is already centred has nothing to click through to), so the
 * rect is free to be whatever composition reads best. It is still one
 * function, still called by both the painter and l_motion. */
static rect tile_rect(shell_ctx *c, rect a, int index)
{
    const float LEAD = 1.45f;
    int top = a.y + home_head(c, a);
    int bot = a.y + a.h - c->padding;
    int avail = bot - top;
    if (avail < HOME_TILES) avail = HOME_TILES;

    float unit = (float)avail / (LEAD + (float)(HOME_TILES - 1));
    float y = (float)top;
    for (int i = 0; i < index; i++) y += unit * (i == 0 ? LEAD : 1.f);
    float hgt = unit * (index == 0 ? LEAD : 1.f);

    rect t = { a.x, (int)y, a.w, (int)hgt };
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

static float fa (font *f, float d) { return f ? font_ascent(f)  : d; }
static float fdc(font *f, float d) { return f ? font_descent(f) : d; }

static void paint_home(shell_ctx *c, surface *s, rect a, shell_fonts *f,
                       float alpha, int focused)
{
    rail_priv *p = P(c);
    int pad = c->padding + 10;

    if (!focused) {
        /* Peeking Home gets a labelled spine, not a shrunken index: six
         * unreadable rows say nothing, "Home" says what a click does. */
        int vx = a.x < 0 ? 0 : a.x;
        int vw = (a.x + a.w) - vx;
        if (vw <= 0) return;
        int mx = vx + (vw < 140 ? vw/2 - 5 : pad);
        rect mark = { mx, a.y + 34, 10, 10 };
        draw_rect(s, mark, c->accent, alpha);
        if (vw >= 140)
            shell_text(s, f->mid, (float)(mx + 22),
                       shell_baseline(f->mid, (float)(a.y + 26), 28.f),
                       "Home", c->fg_hi, alpha * 0.95f);
        return;
    }

    float x    = (float)(a.x + pad);
    float innw = (float)(a.w - pad * 2);

    /* The headline, set as large as the card can carry it. Falling back
     * a step rather than shrinking continuously keeps the two display
     * sizes in the system the only two on screen. */
    font *tf = f->huge;
    const char *Q = "What would you like to do?";
    if (shell_text_w(tf, Q) > innw) tf = f->big;
    if (shell_text_w(tf, Q) > innw) tf = f->dmid;

    float hy = (float)(a.y + pad) + fa(tf, 30.f);
    shell_text(s, tf, x, hy, Q, c->fg_hi, alpha * 0.98f);
    shell_text(s, f->small, x, hy + fdc(tf, 10.f) + 13.f + fa(f->small, 11.f),
               "Pick one. You can always come back here.", c->subtle, alpha * 0.92f);

    /* The rule that closes the headline. It stops at 46% of the measure
     * instead of running the full width: a rule that reaches both
     * margins is a border, and a border would box the headline back up
     * into the card this composition is trying not to be. */
    draw_hrule(s, (int)x, a.y + home_head(c, a) - 22, (int)(innw * 0.46f), 2,
               c->fg_hi, alpha * 0.9f);

    for (int i = 0; i < HOME_TILES && i < c->n_apps; i++) {
        rect t = tile_rect(c, a, i);
        int hot = (p->hover_tile == i);
        uint32_t paper = c->surface_c;

        /* Hover is a fill plus a bar down the left edge — the GOV.UK
         * inset device. No shadow, no glow, no ring: the row simply
         * becomes the piece of the page you are on. */
        if (hot) {
            draw_rect(s, t, c->surface_hi, alpha);
            draw_rect(s, (rect){ t.x, t.y, 4, t.h }, c->accent, alpha);
            paper = c->surface_hi;
        }
        if (i + 1 < HOME_TILES && i + 1 < c->n_apps)
            draw_hrule(s, t.x + pad, t.y + t.h - 1, (int)innw, 1,
                       c->overlay, alpha * 0.95f);

        font *nf = (i == 0) ? f->big : f->dmid;
        float by  = shell_baseline(nf, (float)t.y, (float)t.h);
        float cy  = (float)t.y + (float)t.h * 0.5f;
        float isz = clampf((float)t.h * 0.44f, 17.f, 34.f);

        /* The entry number. An annual-report device, and it does real
         * work: it gives the eye a fixed left edge to run down, which
         * is the thing a ragged list of names does not have. */
        char num[8];
        snprintf(num, sizeof num, "%02d", i + 1);
        shell_text_tracked(s, f->label, x, by, num,
                           hot ? c->accent : c->muted, alpha * 0.95f, 1.3f);

        shell_icon_draw(s, c->apps[i].icon, x + 52.f, cy, isz,
                        c->apps[i].tint, paper, alpha);
        /* Two columns, and the first one ends where the second begins.
         * The name column is measured rather than assumed, so a package
         * with a long name is cut instead of printing over the
         * description beside it.
         *
         * On a narrow panel the second column does not fit, and the
         * name wins: a row that says "Browse the web" with no name
         * above it is not a shorter row, it is a row that has lost the
         * only thing the user was looking for. 1024x600 is a real
         * target, so this is a layout the product has, not an edge. */
        float name_x = x + 52.f + isz * 0.5f + 20.f;
        float hint_x = x + innw * 0.42f;
        float name_w = hint_x - name_x - 16.f;
        if (name_w < 110.f) {
            shell_text_elided(s, nf, name_x, by, x + innw - name_x,
                              c->apps[i].name, c->fg_hi, alpha * 0.98f);
        } else {
            shell_text_elided(s, nf, name_x, by, name_w,
                              c->apps[i].name, c->fg_hi, alpha * 0.98f);
            shell_text_elided(s, f->small, hint_x, by, x + innw - hint_x,
                              c->apps[i].hint, c->subtle, alpha * 0.92f);
        }
    }
}

static void paint_win(shell_ctx *c, surface *s, rect a, int wi, shell_fonts *f,
                      float alpha, int focused)
{
    const win_entry *w = &c->wins[wi];
    uint32_t ink = (w->app >= 0) ? c->apps[w->app].tint : c->fg;
    shell_icon ic = (w->app >= 0) ? c->apps[w->app].icon : ICON_WINDOW;

    /* A masthead, not a titlebar: a band of the deeper stock closed by
     * a hairline, with the name set in the bold text cut. */
    int th = 50;
    rect tb = { a.x, a.y, a.w, th };
    draw_rect(s, tb, c->bg_alt, alpha);
    draw_hrule(s, a.x, a.y + th, a.w, 1, c->overlay, alpha);

    /* Anchor the header to the VISIBLE region: a peeking card hangs off
     * the screen, and a header at a.x lands out of view, leaving the
     * card the user is invited to click with no label at all. */
    int vx = a.x < 0 ? 0 : a.x;
    shell_icon_draw(s, ic, (float)(vx + c->padding + 8), (float)(a.y + th/2), 19.f,
                    ink, c->bg_alt, alpha * 0.95f);
    shell_text(s, f->mid, (float)(vx + c->padding + 28),
               shell_baseline(f->mid, (float)a.y, (float)th), w->title, c->fg_hi, alpha * 0.96f);

    if (focused) {
        int cs = 22;
        rect cb = { a.x + a.w - c->padding - cs, a.y + (th - cs)/2, cs, cs };
        float ccx = (float)cb.x + (float)cs * 0.5f, ccy = (float)cb.y + (float)cs * 0.5f;
        draw_frame(s, cb, 1, c->overlay, alpha * 0.9f);
        draw_line(s, ccx-4.5f, ccy-4.5f, ccx+4.5f, ccy+4.5f, 1.6f, c->subtle, alpha);
        draw_line(s, ccx+4.5f, ccy-4.5f, ccx-4.5f, ccy+4.5f, 1.6f, c->subtle, alpha);
    }

    if (w->content) {
        rect body = { a.x, a.y + th, a.w, a.h - th };
        corners bc = { 0, 0, (float)c->radius, (float)c->radius };
        draw_content_fit(s, w->content, body, bc, alpha);
        return;
    }

    /* Nothing loaded yet. Set as a title page rather than as a centred
     * icon with two centred captions under it: the mark sits on the
     * left measure, the words hang off it, and a rule closes the
     * block. The old version centred all three on the card's axis,
     * which is the sixth copy of the same composition in this product. */
    int pad = c->padding + 10;
    float x  = (float)(a.x + pad);
    float cy = (float)(a.y + th) + (float)(a.h - th) * 0.40f;
    float isz = clampf((float)(a.h - th) * 0.14f, 26.f, 54.f);

    shell_icon_draw(s, ic, x + isz * 0.5f, cy - isz * 1.35f, isz, ink,
                    c->surface_c, alpha * 0.9f);
    shell_text(s, f->big, x, cy + fa(f->big, 26.f) * 0.5f, w->title, c->fg_hi, alpha * 0.95f);
    float sy = cy + fa(f->big, 26.f) * 0.5f + fdc(f->big, 8.f) + 14.f + fa(f->small, 11.f);
    shell_text(s, f->small, x, sy,
               w->subtitle[0] ? w->subtitle : "Opening…", c->subtle, alpha * 0.9f);
    draw_hrule(s, (int)x, (int)(sy + fdc(f->small, 4.f) + 16.f),
               (int)((float)(a.w - pad * 2) * 0.30f), 1, c->overlay, alpha);
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
            /* Paper is opaque, so a card behind does NOT get more
             * see-through: the sheet is filled at full strength and
             * only its INK is stepped back. Fading the surface was
             * what let the wallpaper show through a window, which is
             * two things on screen where the archetype promises one. */
            float alpha = clampf(1.f - d * 0.15f, 0.72f, 1.f);
            int focused = (d < 0.5f);
            /* Colour means state here and nothing else, so the rule is
             * the accent for whichever card you are on, full stop. It
             * used to be the app's own tint, which made "where am I"
             * an answer that changed colour depending on which program
             * you were in — a worse answer, and on a light theme a
             * pastel one nobody could see. */
            uint32_t ring = c->accent;

            /* A sheet of paper: flat, opaque, square-cornered, and
             * edged with a hairline. What marks the one you are on is
             * a rule along its top edge in the signal colour — one
             * mark, in one place, on one card. The version this
             * replaces drew a 35px black shadow offset 14px, blurred
             * the wallpaper behind the card, and then ringed the whole
             * thing in the app's own pastel. Three depth cues for one
             * piece of information. */
            draw_round_rect(s, a, corners_all((float)c->radius), c->surface_c, 1.f);
            draw_frame(s, a, 1, c->overlay, 1.f);
            if (focused)
                draw_hrule(s, a.x, a.y, a.w, 3, ring, alpha);

            if (i == 0) paint_home(c, s, a, f, alpha, focused);
            else paint_win(c, s, a, i - 1, f, alpha, focused);
        }
    }

    /* The masthead. Deliberately NOT a taskbar: nothing here launches or
     * switches, so there is exactly one way to move around. A flat band
     * of the deeper stock, closed by one hairline — the way the top of
     * a printed page is closed. */
    int bh = c->bar_h + 8;
    draw_rect(s, (rect){ 0, 0, s->w, bh }, c->bg_alt, 1.f);
    draw_hrule(s, 0, bh, s->w, 1, c->overlay, 1.f);

    /* A solid square, not a dot. A circle in the accent beside a word
     * is the "brand bubble" every product page has; a square block of
     * ink is a printer's mark, and it is one draw_rect. */
    draw_rect(s, (rect){ c->margin, bh/2 - 4, 8, 8 }, c->accent, 1.f);
    shell_text_tracked(s, f->label, (float)(c->margin + 20),
                       shell_baseline(f->label, 0.f, (float)bh),
                       c->brand, c->subtle, 0.95f, 1.6f);

    if (c->show_clock) {
        char hm[32], dt[48];
        shell_clock(hm, sizeof hm, dt, sizeof dt);
        float by = shell_baseline(f->mid, 0.f, (float)bh);
        float rx = (float)(s->w - c->margin);
        shell_text(s, f->mid, rx - shell_text_w(f->mid, hm), by, hm, c->fg_hi, 1.f);
        rx -= shell_text_w(f->mid, hm) + 13.f;
        /* A rule between the date and the time, because they are two
         * different facts and a gap alone does not say so. */
        draw_vrule(s, (int)rx, 9, bh - 18, 1, c->overlay, 1.f);
        rx -= 13.f;
        shell_text(s, f->small, rx - shell_text_w(f->small, dt),
                   shell_baseline(f->small, 0.f, (float)bh), dt, c->subtle, 0.9f);
    }

    /* Where you are in the row, as a ruler rather than as dots, and
     * left-aligned to the page margin rather than centred on the
     * screen. Centred dots under a centred card was the fourth axis of
     * symmetry on one screen; a ruler that starts at the margin reads
     * as a measure, which is what it is. */
    if (c->show_positions && n > 1) {
        int seg = 26, gap = 5, y = s->h - c->margin + 2;
        for (int i = 0; i < n; i++) {
            float d = fabsf((float)i - p->slide.value);
            int cur = (d < 0.5f);
            int x = c->margin + i * (seg + gap);
            if (x + seg > s->w - c->margin) break;
            draw_hrule(s, x, cur ? y - 2 : y, seg, cur ? 4 : 2,
                       cur ? c->accent : c->muted, cur ? 1.f : 0.65f);
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

/* In a row of cards, "show me this window" means scroll the row until
 * that card is the one in the middle. Setting c->focus alone would move
 * the highlight without moving the row, leaving the focused card
 * halfway off the screen -- which is exactly what a newly started
 * application looked like before this existed. */
static void l_present(shell_ctx *c, int win) { focus_card(c, win + 1, 0.30f); }
static void l_fini(shell_ctx *c) { c->priv = NULL; }

const shell_layout layout_rail = {
    .id = "rail",
    .init = l_init,
    .paint = l_paint,
    .click = l_click,
    .motion = l_motion,
    .key = l_key,
    .step = l_step,
    .fini = l_fini,
    .present = l_present,
};
