#include "rail.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

/* ── geometry ─────────────────────────────────────────────────────
 * One function owns the card layout. Hit-testing and painting both
 * call it, so a click always lands on what the user sees -- deriving
 * them separately is how you get a UI that is off by the width of a
 * shadow and feels haunted. */
#define CARD_W_FRAC 0.62f
#define CARD_GAP    28

rect rail_card_rect(const rail *r, int w, int h, int index)
{
    int top = r->bar_h + r->margin;
    int cw  = (int)((float)w * CARD_W_FRAC);
    int ch  = h - top - r->margin;
    float off = (float)index - r->slide.value;

    /* Off-centre cards sit slightly smaller and lower, which reads as
     * depth without needing any 3D. */
    float d = fabsf(off);
    float scale = 1.0f - clampf(d, 0.f, 2.f) * 0.055f;
    int   sw = (int)((float)cw * scale);
    int   sh = (int)((float)ch * scale);

    rect c;
    c.x = (w - cw) / 2 + (int)(off * (float)(cw + CARD_GAP)) + (cw - sw) / 2;
    c.y = top + (ch - sh) / 2;
    c.w = sw;
    c.h = sh;
    return c;
}

rect rail_tile_rect(const rail *r, rect a, int index)
{
    /* 3 x 2, deliberately large. This is read by someone in reading
     * glasses, sometimes at arm's length, and a tile you have to aim
     * at is a tile that gets mis-clicked. */
    const int COLS = 3, ROWS = 2;
    int pad = r->padding * 2;
    int gap = r->padding + 6;
    int gw = a.w - pad * 2 - gap * (COLS - 1);
    int gh = a.h - pad * 2 - gap * (ROWS - 1) - 96;   /* 96: heading block */
    int tw = gw / COLS, th = gh / ROWS;
    int col = index % COLS, row = index / COLS;

    rect t;
    t.x = a.x + pad + col * (tw + gap);
    t.y = a.y + pad + 96 + row * (th + gap);
    t.w = tw;
    t.h = th;
    return t;
}

int rail_hit_card(const rail *r, int w, int h, int mx, int my)
{
    /* Front-to-back: the focused card is on top, so test it first. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < r->n_cards; i++) {
            int is_focus = (i == r->focus);
            if (pass == 0 && !is_focus) continue;
            if (pass == 1 && is_focus) continue;
            rect c = rail_card_rect(r, w, h, i);
            if (mx >= c.x && mx < c.x + c.w && my >= c.y && my < c.y + c.h) return i;
        }
    }
    return -1;
}

int rail_hit_tile(const rail *r, int w, int h, int mx, int my)
{
    if (r->focus != 0 || r->n_cards == 0) return -1;
    rect a = rail_card_rect(r, w, h, 0);
    for (int i = 0; i < r->n_tiles; i++) {
        rect t = rail_tile_rect(r, a, i);
        if (mx >= t.x && mx < t.x + t.w && my >= t.y && my < t.y + t.h) return i;
    }
    return -1;
}

/* ── state ────────────────────────────────────────────────────────── */
void rail_theme(rail *r, const theme_t *t)
{
    r->radius    = theme_int(t, "radius", 14);
    r->radius_sm = theme_int(t, "radius_sm", 8);
    r->margin    = theme_int(t, "margin", 14);
    r->padding   = theme_int(t, "padding", 14);
    r->border    = theme_int(t, "border", 2);
    r->bar_h     = theme_int(t, "bar_height", 38);
    r->blur_r    = theme_int(t, "blur_radius", 20);
    r->shadow_r  = theme_int(t, "shadow_radius", 28);
    r->panel_a   = (float)theme_num(t, "opacity_panel", 0.88);
    r->shadow_a  = (float)theme_num(t, "shadow_opacity", 0.50);

    r->bg         = theme_color(t, "col_bg",          theme_color(t, "bg", 0x0B0E14));
    r->bg_alt     = theme_color(t, "col_bar_bg",      theme_color(t, "bg_alt", 0x10151F));
    r->surface_c  = theme_color(t, "col_surface",     theme_color(t, "surface", 0x161C28));
    r->surface_hi = theme_color(t, "col_surface_hi",  theme_color(t, "surface_hi", 0x1F2735));
    r->overlay    = theme_color(t, "col_overlay",     theme_color(t, "overlay", 0x2B3542));
    r->muted      = theme_color(t, "col_muted",       theme_color(t, "muted", 0x55606E));
    r->subtle     = theme_color(t, "col_subtle",      theme_color(t, "subtle", 0x8793A4));
    r->fg         = theme_color(t, "col_fg",          theme_color(t, "fg", 0xD4DCEA));
    r->fg_hi      = theme_color(t, "col_fg_hi",       theme_color(t, "fg_hi", 0xF3F7FD));
    r->accent     = theme_color(t, "col_accent",      theme_color(t, "accent", 0x7DD3C0));
    r->accent_alt = theme_color(t, "col_accent_alt",  theme_color(t, "accent_alt", 0xA78BFA));
    r->accent_warm= theme_color(t, "col_accent_warm", theme_color(t, "accent_warm", 0xF2B880));
    r->err        = theme_color(t, "col_err",         theme_color(t, "err", 0xF2788D));
    r->ok         = theme_color(t, "col_ok",          theme_color(t, "ok", 0x7DD3C0));
    r->info       = theme_color(t, "col_info",        theme_color(t, "info", 0x82AAFF));
    snprintf(r->brand, sizeof r->brand, "%s", theme_str(t, "brand_text", "AurOS"));
}

void rail_init(rail *r, const theme_t *t)
{
    memset(r, 0, sizeof *r);
    rail_theme(r, t);
    r->hover = r->hover_tile = -1;

    /* Home is card 0 and cannot be closed. The words are the words a
     * non-technical person uses: "Internet", not "Web Browser"; "My
     * Files", not "Files"; "Help" is a first-class tile, not buried. */
    r->cards[0].kind = CARD_HOME;
    snprintf(r->cards[0].title, sizeof r->cards[0].title, "Home");
    r->cards[0].tint = r->accent;
    r->n_cards = 1;
    r->focus = 0;
    tween_set(&r->slide, 0.f);

    const home_tile T[] = {
        { "Internet",  "Browse the web",        ICON_GLOBE,    0x7DD3C0 },
        { "Email",     "Read your messages",    ICON_MAIL,     0x82AAFF },
        { "Photos",    "Pictures and videos",   ICON_PHOTOS,   0xA78BFA },
        { "My Files",  "Documents you saved",   ICON_FILES,    0xF2B880 },
        { "Settings",  "Change how this works", ICON_SETTINGS, 0x8793A4 },
        { "Help",      "Show me how",           ICON_HELP,     0x6FD8DC },
    };
    r->n_tiles = (int)(sizeof T / sizeof T[0]);
    for (int i = 0; i < r->n_tiles; i++) r->tiles[i] = T[i];
}

void rail_focus(rail *r, int index, float anim_s)
{
    if (index < 0) index = 0;
    if (index >= r->n_cards) index = r->n_cards - 1;
    r->focus = index;
    tween_to(&r->slide, (float)index, anim_s, EASE_SPRING);
}

void rail_open(rail *r, const char *title, const char *sub, tile_icon ic, uint32_t tint)
{
    if (r->n_cards >= RAIL_MAX_CARDS) return;
    card *c = &r->cards[r->n_cards];
    memset(c, 0, sizeof *c);
    c->kind = CARD_APP;
    snprintf(c->title, sizeof c->title, "%s", title);
    snprintf(c->subtitle, sizeof c->subtitle, "%s", sub ? sub : "");
    c->icon = (int)ic;
    c->tint = tint;
    r->n_cards++;
    /* New cards land at the right-hand end and slide in. The motion is
     * the explanation: you watch where it went, so you know where to
     * look for it later. */
    rail_focus(r, r->n_cards - 1, 0.42f);
}

void rail_close(rail *r, int index)
{
    if (index <= 0 || index >= r->n_cards) return;   /* Home is permanent */
    for (int i = index; i < r->n_cards - 1; i++) r->cards[i] = r->cards[i+1];
    r->n_cards--;
    if (r->focus >= r->n_cards) r->focus = r->n_cards - 1;
    tween_to(&r->slide, (float)r->focus, 0.30f, EASE_OUT_CUBIC);
}

int rail_step(rail *r, float dt) { return tween_step(&r->slide, dt); }

/* ── icons ────────────────────────────────────────────────────────
 * Drawn from primitives rather than loaded from a theme pack: they
 * inherit the theme's colours for free, scale to any size without
 * assets, and add nothing to the image. Geometric and plain on
 * purpose -- an icon that needs interpreting is a label that failed. */
static void icon_draw(surface *s, tile_icon ic, float cx, float cy, float sz, uint32_t col, float a)
{
    float u = sz / 24.f;   /* design grid is 24px */
    switch (ic) {
    case ICON_GLOBE: {
        draw_circle(s, cx, cy, 10*u, col, a * 0.22f);
        /* ring */
        for (float ang = 0; ang < 6.2832f; ang += 0.06f)
            draw_circle(s, cx + cosf(ang)*10*u, cy + sinf(ang)*10*u, 1.1f*u, col, a);
        draw_line(s, cx-10*u, cy, cx+10*u, cy, 1.6f*u, col, a * 0.85f);
        /* two meridians, as flattened ellipses */
        for (float t = -1.f; t <= 1.f; t += 0.02f) {
            float y = t * 10*u;
            float x = sqrtf(fmaxf(0.f, 1.f - t*t)) * 4.6f*u;
            draw_circle(s, cx - x, cy + y, 0.85f*u, col, a * 0.8f);
            draw_circle(s, cx + x, cy + y, 0.85f*u, col, a * 0.8f);
        }
        break; }
    case ICON_MAIL: {
        rect e = { (int)(cx-11*u), (int)(cy-8*u), (int)(22*u), (int)(16*u) };
        draw_round_rect(s, e, corners_all(3*u), col, a * 0.22f);
        draw_round_rect_border(s, e, corners_all(3*u), 1.6f*u, col, a);
        draw_line(s, cx-9.5f*u, cy-6*u, cx, cy+1.5f*u, 1.6f*u, col, a);
        draw_line(s, cx+9.5f*u, cy-6*u, cx, cy+1.5f*u, 1.6f*u, col, a);
        break; }
    case ICON_PHOTOS: {
        rect f = { (int)(cx-11*u), (int)(cy-9*u), (int)(22*u), (int)(18*u) };
        draw_round_rect(s, f, corners_all(3*u), col, a * 0.22f);
        draw_round_rect_border(s, f, corners_all(3*u), 1.6f*u, col, a);
        draw_circle(s, cx-4.5f*u, cy-3.5f*u, 2.2f*u, col, a);
        /* a hill, drawn as a stack of shortening lines */
        for (int i = 0; i < (int)(8*u); i++) {
            float yy = cy + 7*u - (float)i;
            float hw = (float)i * 1.35f;
            draw_line(s, cx + 1*u - hw, yy, cx + 1*u + hw, yy, 1.f, col, a * 0.9f);
        }
        break; }
    case ICON_FILES: {
        rect d = { (int)(cx-8*u), (int)(cy-11*u), (int)(16*u), (int)(22*u) };
        draw_round_rect(s, d, corners_all(2.5f*u), col, a * 0.22f);
        draw_round_rect_border(s, d, corners_all(2.5f*u), 1.6f*u, col, a);
        for (int i = 0; i < 3; i++)
            draw_line(s, cx-4.5f*u, cy-4*u + (float)i*5*u, cx+4.5f*u, cy-4*u + (float)i*5*u,
                      1.4f*u, col, a * 0.8f);
        break; }
    case ICON_SETTINGS: {
        for (int i = 0; i < 8; i++) {
            float ang = (float)i * 0.7854f;
            draw_circle(s, cx + cosf(ang)*9.5f*u, cy + sinf(ang)*9.5f*u, 2.1f*u, col, a);
        }
        draw_circle(s, cx, cy, 7.f*u, col, a * 0.32f);
        for (float ang = 0; ang < 6.2832f; ang += 0.08f)
            draw_circle(s, cx + cosf(ang)*7*u, cy + sinf(ang)*7*u, 1.1f*u, col, a);
        draw_circle(s, cx, cy, 2.6f*u, col, a);
        break; }
    case ICON_HELP: {
        draw_circle(s, cx, cy, 11*u, col, a * 0.22f);
        for (float ang = 0; ang < 6.2832f; ang += 0.055f)
            draw_circle(s, cx + cosf(ang)*11*u, cy + sinf(ang)*11*u, 1.1f*u, col, a);
        /* a question mark built from an arc and a dot */
        for (float ang = 2.5f; ang > -1.5f; ang -= 0.06f)
            draw_circle(s, cx + cosf(ang)*4.2f*u, cy - 3.2f*u + sinf(ang)*4.2f*u, 1.3f*u, col, a);
        draw_line(s, cx + 0.4f*u, cy + 0.2f*u, cx + 0.4f*u, cy + 3.4f*u, 2.4f*u, col, a);
        draw_circle(s, cx + 0.4f*u, cy + 7.2f*u, 1.5f*u, col, a);
        break; }
    case ICON_WINDOW: {
        rect wq = { (int)(cx-11*u), (int)(cy-8*u), (int)(22*u), (int)(16*u) };
        draw_round_rect(s, wq, corners_all(3*u), col, a * 0.22f);
        draw_round_rect_border(s, wq, corners_all(3*u), 1.6f*u, col, a);
        draw_line(s, cx-11*u, cy-3.2f*u, cx+11*u, cy-3.2f*u, 1.4f*u, col, a * 0.8f);
        break; }
    case ICON_PLUS:
        draw_round_rect(s, (rect){ (int)(cx-9*u), (int)(cy-1.6f*u), (int)(18*u), (int)(3.2f*u) },
                        corners_all(1.6f*u), col, a);
        draw_round_rect(s, (rect){ (int)(cx-1.6f*u), (int)(cy-9*u), (int)(3.2f*u), (int)(18*u) },
                        corners_all(1.6f*u), col, a);
        break;
    }
}

/* ── text helpers ─────────────────────────────────────────────────── */
static void text_at(surface *s, font *f, float x, float y, const char *t, uint32_t c, float a)
{
    if (f && t && *t) font_draw(f, s->px, s->w, s->h, x, y, t, c, a);
}
static float text_w(font *f, const char *t) { return (f && t) ? font_text_width(f, t) : 0.f; }
static void text_centred(surface *s, font *f, float cx, float y, const char *t, uint32_t c, float a)
{
    if (f && t && *t) font_draw(f, s->px, s->w, s->h, cx - text_w(f, t)/2.f, y, t, c, a);
}

/* ── the Home card ────────────────────────────────────────────────── */
static void paint_home(rail *r, surface *s, rect a, font *big, font *mid, font *small,
                       float alpha, int focused)
{
    if (!focused) {
        /* Peeking Home: a labelled spine, not a shrunken grid. Six tiny
         * unreadable tiles say nothing; "Home" with the house accent
         * says exactly what clicking will do. */
        int vis_x = a.x < 0 ? 0 : a.x;
        int vis_w = (a.x + a.w) - vis_x;
        if (vis_w > 0) {
            icon_draw(s, ICON_PLUS, (float)(vis_x + (vis_w < 120 ? vis_w/2 : 44)),
                      (float)(a.y + 34), 0.f, r->accent, 0.f);   /* no-op keeps sizes honest */
            draw_circle(s, (float)(vis_x + (vis_w < 140 ? vis_w/2 : 46)),
                        (float)(a.y + 40), 15.f, r->accent, alpha * 0.18f);
            icon_draw(s, ICON_WINDOW, (float)(vis_x + (vis_w < 140 ? vis_w/2 : 46)),
                      (float)(a.y + 40), 20.f, r->accent, alpha * 0.9f);
            text_at(s, mid, (float)(vis_x + (vis_w < 140 ? vis_w/2 - 20 : 70)),
                    (float)(a.y + 40) + (mid ? font_ascent(mid)*0.5f - font_descent(mid)*0.5f : 5.f),
                    "Home", r->fg_hi, alpha * 0.95f);
        }
        return;
    }
    text_at(s, big, (float)(a.x + r->padding*2),
            (float)(a.y + r->padding*2) + (big ? font_ascent(big) : 22.f),
            "What would you like to do?", r->fg_hi, alpha * 0.96f);
    text_at(s, small, (float)(a.x + r->padding*2),
            (float)(a.y + r->padding*2) + (big ? font_line_height(big) : 34.f)
            + (small ? font_ascent(small) : 14.f) + 4.f,
            "Pick one. You can always come back here.", r->subtle, alpha * 0.75f);

    for (int i = 0; i < r->n_tiles; i++) {
        rect t = rail_tile_rect(r, a, i);
        int  hot = (r->hover_tile == i);
        uint32_t tint = r->tiles[i].tint;

        if (hot) draw_round_rect_shadow(s, t, corners_all((float)r->radius),
                                        (float)r->shadow_r * 0.6f, 0x000000,
                                        r->shadow_a * 0.55f * alpha, 5);
        draw_round_rect(s, t, corners_all((float)r->radius),
                        hot ? r->surface_hi : r->surface_c, alpha * (hot ? 1.f : 0.88f));
        draw_round_rect_border(s, t, corners_all((float)r->radius), hot ? 2.f : 1.f,
                               hot ? tint : r->overlay, alpha * (hot ? 0.95f : 0.7f));

        /* Centre the icon+label+hint group as a block. Anchoring the
         * icon to a fraction of the tile height leaves a pool of dead
         * space underneath on tall tiles, which reads as "something is
         * missing here" rather than as generous spacing. */
        float icx  = (float)(t.x + t.w/2);
        float isz  = (float)t.h * 0.30f;
        if (isz > 64.f) isz = 64.f;
        float lab_h  = mid   ? font_line_height(mid)   : 22.f;
        float hint_h = small ? font_line_height(small) : 18.f;
        float block  = isz*1.56f + 14.f + lab_h + 4.f + hint_h;
        float top    = (float)t.y + ((float)t.h - block) * 0.5f;
        float icy    = top + isz*0.78f;

        draw_circle(s, icx, icy, isz*0.78f, tint, alpha * (hot ? 0.20f : 0.13f));
        icon_draw(s, r->tiles[i].icon, icx, icy, isz, tint, alpha * (hot ? 1.f : 0.9f));

        float ly = top + isz*1.56f + 14.f + (mid ? font_ascent(mid) : 16.f);
        text_centred(s, mid, icx, ly, r->tiles[i].label, r->fg_hi, alpha * 0.97f);
        text_centred(s, small, icx, ly + lab_h - (mid ? font_descent(mid) : 4.f)
                     + 4.f + (small ? font_ascent(small) : 13.f),
                     r->tiles[i].hint, r->subtle, alpha * 0.78f);
    }
}

/* ── an app card ──────────────────────────────────────────────────── */
static void paint_app(rail *r, surface *s, rect a, const card *c,
                      font *big, font *mid, font *small, float alpha, int focused)
{
    int th = 52;
    rect tb = { a.x, a.y, a.w, th };
    corners tc = { (float)r->radius, (float)r->radius, 0, 0 };
    draw_round_rect(s, tb, tc, r->bg_alt, alpha * 0.55f);
    draw_line(s, (float)a.x + 1, (float)(a.y + th), (float)(a.x + a.w - 1),
              (float)(a.y + th), 1.f, r->overlay, alpha * 0.85f);

    /* Anchor the header to the card's VISIBLE region, not its true left
     * edge. A peeking card hangs off the side of the screen, so a header
     * placed at a.x lands off-screen and the card the user is being
     * invited to click carries no label at all -- which is exactly the
     * "what is that?" the rail exists to prevent. */
    int vis_x = a.x < 0 ? 0 : a.x;
    icon_draw(s, (tile_icon)c->icon, (float)(vis_x + r->padding + 14),
              (float)(a.y + th/2), 22.f, c->tint, alpha * 0.95f);
    text_at(s, mid, (float)(vis_x + r->padding + 38),
            (float)(a.y + th/2) + (mid ? font_ascent(mid)*0.5f - font_descent(mid)*0.5f : 5.f),
            c->title, r->fg_hi, alpha * 0.96f);

    /* A close control only on the focused card. On the peeking ones it
     * would be a tiny target next to a big one, which is how people
     * close the thing they meant to switch to. */
    if (focused) {
        float ccx = (float)(a.x + a.w - r->padding - 12), ccy = (float)(a.y + th/2);
        draw_circle(s, ccx, ccy, 13.f, r->overlay, alpha * 0.55f);
        draw_line(s, ccx-4.5f, ccy-4.5f, ccx+4.5f, ccy+4.5f, 1.8f, r->subtle, alpha);
        draw_line(s, ccx+4.5f, ccy-4.5f, ccx-4.5f, ccy+4.5f, 1.8f, r->subtle, alpha);
    }

    if (c->content) {
        rect body = { a.x, a.y + th, a.w, a.h - th };
        corners bc = { 0, 0, (float)r->radius, (float)r->radius };
        draw_scaled_rounded(s, c->content, body, bc, alpha);
    } else {
        /* Stub content: a calm placeholder, not an error. */
        float cx = (float)(a.x + a.w/2), cy = (float)(a.y + th + (a.h - th)/2);
        draw_circle(s, cx, cy - 30.f, 40.f, c->tint, alpha * 0.10f);
        icon_draw(s, (tile_icon)c->icon, cx, cy - 30.f, 46.f, c->tint, alpha * 0.55f);
        text_centred(s, mid, cx, cy + 34.f, c->title, r->fg, alpha * 0.8f);
        text_centred(s, small, cx, cy + 60.f,
                     c->subtitle[0] ? c->subtitle : "Opening…", r->muted, alpha * 0.7f);
    }
    (void)big;
}

/* ── the whole surface ────────────────────────────────────────────── */
void rail_paint(rail *r, surface *s, font *big, font *mid, font *small,
                const surface *wall)
{
    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else {
        surface_fill(s, 0xFF000000u | r->bg);
    }

    /* Cards, far ones first so the focused one lands on top. */
    for (int pass = 2; pass >= 0; pass--) {
        for (int i = 0; i < r->n_cards; i++) {
            int dist = (int)fabsf((float)i - r->slide.value + 0.001f);
            if (dist != pass) continue;
            rect a = rail_card_rect(r, s->w, s->h, i);
            if (a.x > s->w || a.x + a.w < 0) continue;

            float d = fabsf((float)i - r->slide.value);
            float alpha = clampf(1.f - d * 0.18f, 0.62f, 1.f);
            int focused = (d < 0.5f);

            draw_round_rect_shadow(s, a, corners_all((float)r->radius),
                                   (float)r->shadow_r * (focused ? 1.25f : 0.8f),
                                   0x000000, r->shadow_a * alpha, focused ? 14 : 8);
            draw_blur_region(s, a, r->blur_r);
            draw_round_rect(s, a, corners_all((float)r->radius), r->surface_c,
                            alpha * r->panel_a);
            draw_round_rect_border(s, a, corners_all((float)r->radius),
                                   focused ? (float)r->border : 1.f,
                                   focused ? r->cards[i].tint : r->overlay,
                                   alpha * (focused ? 0.9f : 0.6f));

            if (r->cards[i].kind == CARD_HOME) paint_home(r, s, a, big, mid, small, alpha, focused);
            else paint_app(r, s, a, &r->cards[i], big, mid, small, alpha, focused);
        }
    }

    /* Status strip. Deliberately NOT a taskbar: you cannot launch or
     * switch from it. It states the time and who this machine is, and
     * nothing else, so there is exactly one way to move around. */
    rect bar = { 0, 0, s->w, r->bar_h + 8 };
    draw_rect(s, bar, r->bg, 0.55f);
    draw_line(s, 0, (float)(r->bar_h + 8), (float)s->w, (float)(r->bar_h + 8),
              1.f, r->overlay, 0.5f);
    draw_circle(s, (float)(r->margin + 8), (float)((r->bar_h + 8)/2), 6.f, r->accent, 1.f);
    text_at(s, small, (float)(r->margin + 24),
            (float)((r->bar_h + 8)/2) + (small ? font_ascent(small)*0.5f - font_descent(small)*0.5f : 5.f),
            r->brand, r->subtle, 0.9f);

    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    char clk[32], dte[48];
    strftime(clk, sizeof clk, "%H:%M", &tmv);
    strftime(dte, sizeof dte, "%a %d %b", &tmv);
    float by = (float)((r->bar_h + 8)/2) + (small ? font_ascent(small)*0.5f - font_descent(small)*0.5f : 5.f);
    float rx = (float)(s->w - r->margin);
    text_at(s, small, rx - text_w(small, clk), by, clk, r->fg_hi, 0.95f);
    rx -= text_w(small, clk) + 14.f;
    text_at(s, small, rx - text_w(small, dte), by, dte, r->subtle, 0.75f);

    /* Position dots: how many things are open and where you are among
     * them. The only piece of chrome that is purely orientation. */
    int n = r->n_cards;
    if (n > 1) {
        float dw = 9.f, dg = 9.f;
        float tot = (float)n * dw + (float)(n - 1) * dg;
        float x0 = (float)s->w / 2.f - tot / 2.f;
        float y  = (float)s->h - (float)r->margin - 8.f;
        for (int i = 0; i < n; i++) {
            float d = fabsf((float)i - r->slide.value);
            float k = clampf(1.f - d, 0.f, 1.f);
            uint32_t col = (i == 0) ? r->accent : r->fg;
            draw_circle(s, x0 + (float)i * (dw + dg) + dw/2.f, y, 3.2f + k * 1.8f,
                        col, 0.30f + k * 0.65f);
        }
    }
}
