/* layouts/dock.c — the "Favourites along the edge" archetype.
 *
 * A fixed strip of the programs you actually use sits at the bottom
 * edge, in one order that never changes, whether they are running or
 * not. The thing you want is always in the same spot, so reaching it
 * stops being a decision and becomes a reflex. Everything else you find
 * by typing its name. Windows overlap and can be dragged around.
 *
 * The distinction this archetype exists to make: a taskbar lists what
 * is OPEN, a dock lists what you USE. "Which of my favourites" and
 * "which of my windows" are different questions, and people have strong
 * preferences about which one they want to be asked. So nothing here is
 * allowed to reorder: no most-recent-first, no grouping, no sliding
 * cells under the pointer. Position is the entire value of the thing;
 * anything that moves an icon spends the only currency this design has.
 *
 * The honest cost is written on the tin: a program that is open but not
 * a favourite has nowhere of its own to live. See dock_slots().
 */
#include "../shell.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define DOCK_CELL     62      /* fixed pitch of one favourite            */
#define DOCK_GAP      10
#define DOCK_SEP      22      /* space a divider sits in                 */
#define DOCK_FIND_W   88
#define DOCK_PAD_T    10
#define DOCK_DOT_BAND 20      /* room under the icons for running dots   */
#define DOCK_ICON     40.f
#define DOCK_MAG      0.30f   /* peak icon growth under the pointer      */
#define DOCK_LIFT     7.f
#define DOCK_SPREAD   1.35f   /* cells the lift falls off over           */
#define WIN_TITLE_H   46
#define CASCADE_STEP  110
#define FIND_MAX      6

enum { SLOT_FIND, SLOT_FAV, SLOT_RUN };
typedef struct { int kind, app; } dock_slot;

#define MAX_SLOTS (1 + SHELL_MAX_APPS + SHELL_MAX_WINS)

typedef struct {
    tween mag;              /* 0..1 magnification under the pointer   */
    tween veil;             /* 0..1 the finder's scrim                */
    int   hover;            /* dock slot under the pointer, -1 none   */
    int   drag, grab_x, grab_y;
    uint32_t moved;         /* windows the user has dragged           */
    int   find_open, find_sel;
    char  q[48];
    int   qn;
} dock_priv;

static dock_priv *P(shell_ctx *c) { return (dock_priv *)c->priv; }

/* The text rasteriser writes 0x00RRGGBB: it leaves the alpha byte at
 * zero on every pixel a glyph touches, while the compositor reads that
 * byte as real coverage. Anything translucent painted over a glyph
 * afterwards therefore blends against "nothing" and leaves a ghost in
 * the shape of the letters. In an overlapping-window model that is not
 * a corner case — the front window's shadow lands squarely on the title
 * of the window behind it, every frame. Restoring the byte across the
 * region about to be painted costs one OR per pixel and keeps the
 * workaround inside this file, where it can be deleted the day the
 * rasteriser and the compositor agree about alpha. */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static int   clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

/* ─────────────────────────────────────────────────────────────────
 * What is in the dock, and in what order.
 *
 * One function owns this so that paint, hit-testing and the keyboard
 * can never disagree about which icon is in which position — which, in
 * an archetype whose whole promise is "always the same spot", would not
 * be a cosmetic bug.
 *
 * Order: the finder, then every pinned app in the order the app list
 * declares them, then any app that is running but NOT pinned.
 *
 * That last group is the compromise. Those windows have to be reachable
 * from somewhere — a program you opened once and cannot find again is
 * the worst failure a desktop has — but giving them permanent dock
 * positions would turn the dock into a taskbar, which is the one thing
 * it must not become. So they are appended after a divider: present,
 * clearly not favourites, and the only part of the strip that is
 * allowed to move. They vanish when closed, and if the user wants one
 * to stop moving they pin it, which is the same gesture as "this is a
 * favourite now". A user who lives in unpinned windows is telling us
 * they wanted the taskbar archetype.
 * ───────────────────────────────────────────────────────────────── */
static int dock_slots(shell_ctx *c, dock_slot *out)
{
    int n = 0;
    out[n].kind = SLOT_FIND; out[n].app = -1; n++;

    for (int i = 0; i < c->n_apps && n < MAX_SLOTS; i++)
        if (c->apps[i].pinned) { out[n].kind = SLOT_FAV; out[n].app = i; n++; }

    for (int w = 0; w < c->n_wins && n < MAX_SLOTS; w++) {
        int a = c->wins[w].app;
        if (a < 0 || a >= c->n_apps || c->apps[a].pinned) continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (out[k].kind == SLOT_RUN && out[k].app == a) dup = 1;
        if (!dup) { out[n].kind = SLOT_RUN; out[n].app = a; n++; }
    }
    return n;
}

/* THE one place dock geometry is decided. slot < 0 returns the strip
 * itself; otherwise the cell for that slot. Both painting and hit
 * testing go through here, so a click always lands on the icon the user
 * is looking at.
 *
 * Note what this does NOT return: the hover lift. Magnification is a
 * paint-time offset only, never a change of cell. A dock whose cells
 * widen under the pointer is a dock whose positions are not fixed —
 * the target slides out from under you at the exact moment you are
 * aiming at it, and the muscle memory this archetype is selling is
 * undermined by its own animation. The icon grows; the slot does not. */
static rect dock_rect(shell_ctx *c, int sw, int sh, int slot)
{
    dock_slot sl[MAX_SLOTS];
    int n = dock_slots(c, sl);
    int n_fav = 0, n_run = 0;
    for (int i = 0; i < n; i++) {
        if (sl[i].kind == SLOT_FAV) n_fav++;
        else if (sl[i].kind == SLOT_RUN) n_run++;
    }

    int cell = c->target_large ? DOCK_CELL : DOCK_CELL - 10;
    int gap = DOCK_GAP, sep = DOCK_SEP, pad = 12;
    int avail = sw - 2 * c->margin, inner;

    /* Shrink rather than scroll or wrap: a dock you have to scroll has
     * stopped being a place and started being a list. */
    for (;;) {
        inner = DOCK_FIND_W + sep
              + n_fav * cell + (n_fav ? (n_fav - 1) * gap : 0)
              + (n_run ? sep + n_run * cell + (n_run - 1) * gap : 0);
        if (inner + 2 * pad <= avail || cell <= 34) break;
        cell -= 2;
        if (gap > 6) gap--;
    }

    int strip_h = DOCK_PAD_T + cell + DOCK_DOT_BAND;
    rect strip = { (sw - (inner + 2 * pad)) / 2, sh - c->margin - strip_h,
                   inner + 2 * pad, strip_h };
    if (slot < 0) return strip;
    if (slot >= n) return (rect){ 0, 0, 0, 0 };

    int x = strip.x + pad;
    for (int i = 0; i < n; i++) {
        int w = (sl[i].kind == SLOT_FIND) ? DOCK_FIND_W : cell;
        if (i == slot) return (rect){ x, strip.y + DOCK_PAD_T, w, cell };
        x += w;
        if (i + 1 < n) x += (sl[i + 1].kind != sl[i].kind) ? sep : gap;
    }
    return (rect){ 0, 0, 0, 0 };
}

/* ─────────────────────────────────────────────────────────────────
 * Where the windows are.
 *
 * The other single owner of geometry. A window the user has dragged
 * keeps the position they put it in, for ever; one that has never been
 * touched is cascaded from the current screen size every time it is
 * asked for. Computing it rather than caching it means the cascade is
 * still correct after a resolution change, and it is a handful of
 * arithmetic — cheaper than the bookkeeping to invalidate a cache.
 *
 * This cannot happen in init(): the contract hands init() no surface,
 * so the screen size is not knowable until the first paint. Cascading
 * against an assumed size would put windows off the bottom of every
 * panel that is not 1600x900.
 * ───────────────────────────────────────────────────────────────── */
static rect win_rect(shell_ctx *c, int sw, int sh, int i)
{
    dock_priv *p = P(c);
    win_entry *w = &c->wins[i];
    if (p && (p->moved & (1u << i))) return w->geom;
    if (w->geom.w > 0 && w->geom.h > 0) return w->geom;   /* supplied by the compositor */

    rect strip = dock_rect(c, sw, sh, -1);
    int top    = c->bar_h + 8 + c->margin;
    int bottom = strip.y - c->margin;
    int availh = bottom - top;
    if (availh < 200) availh = 200;

    int n     = c->n_wins < 1 ? 1 : c->n_wins;
    int stepy = (int)((float)CASCADE_STEP * 0.46f);
    int spanx = (n - 1) * CASCADE_STEP, spany = (n - 1) * stepy;

    /* The whole cascade is sized to fit between the strip and the dock,
     * so the last window down is never tucked behind either of them. */
    int ww = clampi((int)((float)sw * 0.44f), 360, 880);
    int wh = clampi((int)((float)availh * 0.76f), 220, 660);
    if (ww > sw - 2 * c->margin - spanx) ww = sw - 2 * c->margin - spanx;
    if (wh > availh - spany - 8) wh = availh - spany - 8;
    if (ww < 260) ww = 260;
    if (wh < 180) wh = 180;

    /* Step DOWN the index, not up: window 0 is the newest, so it lands
     * front-and-lowest where the eye already is, and everything older
     * steps back up and to the left with its title bar still showing.
     * Cascading the other way buries the window you just opened. */
    int k = (n - 1) - i;
    rect g;
    g.w = ww; g.h = wh;
    g.x = (sw - (ww + spanx)) / 2 + k * CASCADE_STEP;
    g.y = top + (availh - (wh + spany)) / 2 + k * stepy;
    g.x = clampi(g.x, c->margin, sw - c->margin - ww);
    g.y = clampi(g.y, c->bar_h + 8, bottom - 80);
    return g;
}

/* ── the finder ──────────────────────────────────────────────────
 * Find-by-name, not a command palette. The rows are things — programs
 * and the files you saved — with the icon they have everywhere else and
 * the name you would say out loud. Nothing here is a verb, because a
 * user who has to learn a vocabulary of commands to reach their fifth
 * program has been handed a worse dock, not a better one.
 *
 * The ctx carries no file index, so this ships a small standing set to
 * show the row shape; a real build replaces find_docs with the
 * indexer's answer and changes nothing else. */
static const struct { const char *name, *where; shell_icon ic; int tint; } find_docs[] = {
    { "Holiday photos",        "Folder in My Files", ICON_PHOTOS, 2 },
    { "Phone bill.pdf",        "In Downloads",       ICON_TEXT,   3 },
    { "Photos of the garden",  "In My Files",        ICON_PHOTOS, 1 },
    { "Letter to the council", "In My Files",        ICON_TEXT,   3 },
    { "Shopping list",         "In My Files",        ICON_TEXT,   0 },
    { "Music for the drive",   "In Music",           ICON_MUSIC,  1 },
};

typedef struct { const char *name, *where; shell_icon ic; uint32_t tint; int app; } find_hit;

static uint32_t tint_of(shell_ctx *c, int i)
{
    switch (i & 3) {
        case 0:  return c->accent;
        case 1:  return c->accent_alt;
        case 2:  return c->accent_warm;
        default: return c->info;
    }
}
static int lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
static int name_has(const char *hay, const char *needle)
{
    if (!needle || !*needle) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* Programs first, then saved things. Someone typing two letters is
 * usually reaching for a program; someone typing a whole phrase is
 * usually reaching for a document, and by then the match has narrowed
 * anyway. With nothing typed the list is the programs that are NOT on
 * the dock, which is exactly the set the dock cannot answer for. */
static int find_search(shell_ctx *c, const char *q, find_hit *out)
{
    int n = 0, empty = !q || !*q;
    for (int i = 0; i < c->n_apps && n < FIND_MAX; i++) {
        if (empty ? c->apps[i].pinned : !name_has(c->apps[i].name, q)) continue;
        out[n].name = c->apps[i].name; out[n].where = "Program";
        out[n].ic = c->apps[i].icon;   out[n].tint = c->apps[i].tint;
        out[n].app = i; n++;
    }
    for (int i = 0; i < (int)(sizeof find_docs / sizeof find_docs[0]) && n < FIND_MAX; i++) {
        if (!name_has(find_docs[i].name, q)) continue;
        out[n].name = find_docs[i].name; out[n].where = find_docs[i].where;
        out[n].ic = find_docs[i].ic;     out[n].tint = tint_of(c, find_docs[i].tint);
        out[n].app = -1; n++;
    }
    return n;
}

static rect find_panel_rect(shell_ctx *c, int sw, int sh, int nres)
{
    int pw = sw - 2 * (c->margin + 56); if (pw > 620) pw = 620; if (pw < 320) pw = 320;
    int ph = 68 + (nres ? 8 + nres * 48 : 0) + 34 + 10;
    return (rect){ (sw - pw) / 2, (int)((float)sh * 0.19f), pw, ph };
}

/* ── helpers over the window list ───────────────────────────────── */
static int app_window(shell_ctx *c, int app)
{
    if (c->focus >= 0 && c->focus < c->n_wins && c->wins[c->focus].app == app) return c->focus;
    for (int i = 0; i < c->n_wins; i++) if (c->wins[i].app == app && !c->wins[i].minimised) return i;
    for (int i = 0; i < c->n_wins; i++) if (c->wins[i].app == app) return i;
    return -1;
}

/* ── init ───────────────────────────────────────────────────────── */
static void l_init(shell_ctx *c)
{
    static dock_priv priv;
    memset(&priv, 0, sizeof priv);
    priv.hover = -1; priv.drag = -1;
    tween_set(&priv.mag, 0.f);
    tween_set(&priv.veil, 0.f);
    c->priv = &priv;

    /* No pointer yet: a still frame, or the first frame before any
     * input arrives. Park the dock on the app the user is actually in
     * so the strip shows its live state instead of sitting inert, and —
     * with nothing open at all — start the finder, mid-word, because
     * with no window to switch to, typing a name is the only move the
     * archetype has and a blank screen should say so. */
    if (c->mouse_x < 0 && c->mouse_y < 0) {
        int app = (c->focus >= 0 && c->focus < c->n_wins) ? c->wins[c->focus].app : -1;
        if (app >= 0) {
            dock_slot sl[MAX_SLOTS];
            int n = dock_slots(c, sl);
            for (int i = 0; i < n; i++)
                if (sl[i].app == app) { priv.hover = i; break; }
            if (priv.hover >= 0) tween_set(&priv.mag, 1.f);
        }
        if (c->n_wins == 0) {
            snprintf(priv.q, sizeof priv.q, "pho");
            priv.qn = 3;
            priv.find_open = 1;
            tween_set(&priv.veil, 1.f);
        }
    }
}

/* ── painting: windows ──────────────────────────────────────────── */
static void paint_win(shell_ctx *c, surface *s, shell_fonts *f, int i, int focused)
{
    const win_entry *w = &c->wins[i];
    rect a = win_rect(c, s->w, s->h, i);
    uint32_t tint = (w->app >= 0) ? c->apps[w->app].tint : c->accent;
    shell_icon ic = (w->app >= 0) ? c->apps[w->app].icon : ICON_WINDOW;
    corners rc = corners_all((float)c->radius);
    float al = focused ? 1.f : 0.93f;

    draw_round_rect_shadow(s, a, rc, (float)c->shadow_r * (focused ? 1.3f : 0.9f),
                           0x000000, c->shadow_a * (focused ? 1.f : 0.85f), focused ? 14 : 9);
    draw_blur_region(s, a, c->blur_r);
    draw_round_rect(s, a, rc, c->surface_c, al * c->panel_a);

    rect tb = { a.x, a.y, a.w, WIN_TITLE_H };
    corners tc = { (float)c->radius, (float)c->radius, 0, 0 };
    draw_round_rect(s, tb, tc, c->bg_alt, al * 0.55f);
    draw_line(s, (float)a.x + 1, (float)(a.y + WIN_TITLE_H), (float)(a.x + a.w - 1),
              (float)(a.y + WIN_TITLE_H), 1.f, c->overlay, al * 0.8f);

    shell_icon_draw(s, ic, (float)(a.x + c->padding + 12), (float)(a.y + WIN_TITLE_H / 2),
                    20.f, tint, al * (focused ? 1.f : 0.75f));
    float tx = (float)(a.x + c->padding + 34);
    float by = shell_baseline(f->mid, (float)a.y, (float)WIN_TITLE_H);
    shell_text(s, f->mid, tx, by, w->title, c->fg_hi, al * (focused ? 0.97f : 0.7f));
    if (w->subtitle[0]) {
        float dx = tx + shell_text_w(f->mid, w->title) + 14.f;
        draw_circle(s, dx, (float)(a.y + WIN_TITLE_H / 2), 1.8f, c->muted, al * 0.8f);
        shell_text(s, f->small, dx + 10.f, shell_baseline(f->small, (float)a.y, (float)WIN_TITLE_H),
                   w->subtitle, c->subtle, al * (focused ? 0.8f : 0.55f));
    }

    /* Minimise then close. Minimising is safe here precisely because the
     * dock is a fixed place to get the window back from. */
    float cy = (float)(a.y + WIN_TITLE_H / 2);
    float bx = (float)(a.x + a.w - c->padding - 12);
    draw_circle(s, bx, cy, 12.f, c->overlay, al * 0.5f);
    draw_line(s, bx - 4.f, cy - 4.f, bx + 4.f, cy + 4.f, 1.7f, c->subtle, al * 0.95f);
    draw_line(s, bx + 4.f, cy - 4.f, bx - 4.f, cy + 4.f, 1.7f, c->subtle, al * 0.95f);
    bx -= 30.f;
    draw_circle(s, bx, cy, 12.f, c->overlay, al * 0.5f);
    draw_line(s, bx - 4.5f, cy + 3.f, bx + 4.5f, cy + 3.f, 1.7f, c->subtle, al * 0.95f);

    rect body = { a.x, a.y + WIN_TITLE_H, a.w, a.h - WIN_TITLE_H };
    if (w->content) {
        corners bc = { 0, 0, (float)c->radius, (float)c->radius };
        draw_scaled_rounded(s, w->content, body, bc, al);
    } else {
        /* Centre icon and captions as one block sized to the body, and
         * drop the second line when there is no room for it. A cascade
         * on a small panel makes windows genuinely short, and a
         * placeholder anchored to fixed offsets spills out of them. */
        float ccx  = (float)(body.x + body.w / 2);
        float isz  = clampf((float)body.h * 0.26f, 20.f, 52.f);
        float l1   = f->mid   ? font_line_height(f->mid)   : 22.f;
        float l2   = f->small ? font_line_height(f->small) : 18.f;
        const char *hint = (w->app >= 0) ? c->apps[w->app].hint : "";
        int two = (body.h > 170 && hint[0]);
        float block = isz * 1.5f + 16.f + l1 + (two ? 4.f + l2 : 0.f);
        float top = (float)body.y + ((float)body.h - block) * 0.5f;
        float icy = top + isz * 0.75f;

        draw_circle(s, ccx, icy, isz * 0.88f, tint, al * 0.11f);
        shell_icon_draw(s, ic, ccx, icy, isz, tint, al * 0.55f);
        float y1 = top + isz * 1.5f + 16.f + (f->mid ? font_ascent(f->mid) : 16.f);
        shell_text_centred(s, f->mid, ccx, y1,
                           w->subtitle[0] ? w->subtitle : w->title, c->fg, al * 0.85f);
        if (two)
            shell_text_centred(s, f->small, ccx,
                               y1 + l1 - (f->mid ? font_descent(f->mid) : 4.f) + 4.f
                               + (f->small ? font_ascent(f->small) : 13.f),
                               hint, c->muted, al * 0.7f);
    }

    /* An unfocused window still needs an edge: on a light theme its
     * surface and the wallpaper are close enough in value that without
     * one the window stops having a shape. */
    draw_round_rect_border(s, a, rc, focused ? (float)c->border : 1.f,
                           focused ? tint : c->overlay, al * (focused ? 0.9f : 0.88f));
}

/* ── painting: the dock ─────────────────────────────────────────── */
static void paint_dock(shell_ctx *c, surface *s, shell_fonts *f, float alpha)
{
    dock_priv *p = P(c);
    dock_slot sl[MAX_SLOTS];
    int n = dock_slots(c, sl);
    rect strip = dock_rect(c, s->w, s->h, -1);
    corners rc = corners_all((float)c->radius + 6.f);

    draw_round_rect_shadow(s, strip, rc, (float)c->shadow_r, 0x000000, c->shadow_a * alpha, 10);
    draw_blur_region(s, strip, c->blur_r);
    draw_round_rect(s, strip, rc, c->surface_c, alpha * c->panel_a);
    draw_round_rect_border(s, strip, rc, 1.f, c->overlay, alpha * 0.85f);

    int cell_h = strip.h - DOCK_PAD_T - DOCK_DOT_BAND;
    float doty = (float)(strip.y + DOCK_PAD_T + cell_h) + 8.f;
    int tip = -1; float tip_x = 0.f;

    for (int i = 0; i < n; i++) {
        rect r = dock_rect(c, s->w, s->h, i);
        float d = (p->hover >= 0) ? fabsf((float)(i - p->hover)) : 99.f;
        float k = p->mag.value * expf(-(d * d) / (DOCK_SPREAD * DOCK_SPREAD));
        float cx = (float)r.x + (float)r.w * 0.5f;
        float cy = (float)r.y + (float)r.h * 0.5f - DOCK_LIFT * k;

        /* Divider wherever the kind changes: the favourites are one
         * thing, what merely happens to be open is another. */
        if (i + 1 < n && sl[i + 1].kind != sl[i].kind) {
            float lx = (float)(r.x + r.w) + (float)DOCK_SEP * 0.5f;
            draw_line(s, lx, (float)(strip.y + 16), lx, (float)(strip.y + strip.h - 16),
                      1.f, c->overlay, alpha * 0.9f);
        }

        if (sl[i].kind == SLOT_FIND) {
            /* The one item that is not a program. It is pinned to the
             * far end, outside the run of favourites, so that opening it
             * up never shifts a favourite by a pixel — and it is spelt
             * out in a word because the archetype's stated cost is that
             * typing to search is a habit some people never form. A
             * button they can see is the cheapest way to start it. */
            rect fp = { r.x, r.y + (r.h - 40) / 2, r.w, 40 };
            corners fc = corners_all((float)c->radius_sm + 4.f);
            draw_round_rect(s, fp, fc, c->accent, alpha * (0.13f + 0.12f * k));
            draw_round_rect_border(s, fp, fc, 1.f, c->accent, alpha * (0.38f + 0.32f * k));
            shell_icon_draw(s, ICON_PLUS, (float)fp.x + 22.f, (float)fp.y + 20.f,
                            16.f, c->accent, alpha * 0.95f);
            shell_text(s, f->small, (float)fp.x + 36.f,
                       shell_baseline(f->small, (float)fp.y, 40.f), "Find",
                       c->accent, alpha * 0.95f);
            continue;
        }

        const app_entry *ap = &c->apps[sl[i].app];
        int wi = app_window(c, sl[i].app);
        int running = (wi >= 0);
        int is_focus = (wi >= 0 && wi == c->focus);

        if (i == p->hover)
            draw_round_rect(s, r, corners_all((float)c->radius_sm + 2.f),
                            c->surface_hi, alpha * 0.55f * p->mag.value);

        float isz = DOCK_ICON * (1.f + DOCK_MAG * k);
        draw_circle(s, cx, cy, isz * 0.72f, ap->tint, alpha * (0.13f + 0.09f * k));
        shell_icon_draw(s, ap->icon, cx, cy, isz, ap->tint, alpha * (running ? 1.f : 0.92f));

        /* Running indicator. One mark per APP, never one per window:
         * "how many windows do I have" is the question a taskbar
         * answers, and answering it here would quietly turn this strip
         * into one. The app you are in gets a wider mark. */
        if (running) {
            if (is_focus)
                draw_round_rect(s, (rect){ (int)(cx - 8.f), (int)(doty - 2.5f), 16, 5 },
                                corners_all(2.5f), c->accent, alpha * 0.95f);
            else
                draw_circle(s, cx, doty, 3.0f, ap->tint, alpha * 0.85f);
        }

        if (i == p->hover) { tip = sl[i].app; tip_x = cx; }
    }

    /* Name above the hovered icon — drawn last so it sits over its
     * neighbours. An icon-only strip is fast once you know it and
     * opaque until you do; this is the bridge, and it costs nothing
     * when the pointer is elsewhere. */
    if (tip >= 0 && p->mag.value > 0.05f) {
        const char *nm = c->apps[tip].name;
        float tw = shell_text_w(f->small, nm);
        rect tp = { (int)(tip_x - tw * 0.5f) - 12, strip.y - 36, (int)tw + 24, 26 };
        if (tp.x < c->margin) tp.x = c->margin;
        if (tp.x + tp.w > s->w - c->margin) tp.x = s->w - c->margin - tp.w;
        corners tc = corners_all((float)c->radius_sm);
        float ta = alpha * p->mag.value;
        draw_round_rect_shadow(s, tp, tc, (float)c->shadow_r * 0.5f, 0x000000, c->shadow_a * ta, 4);
        draw_round_rect(s, tp, tc, c->bg_alt, ta * 0.96f);
        draw_round_rect_border(s, tp, tc, 1.f, c->overlay, ta * 0.9f);
        shell_text_centred(s, f->small, (float)tp.x + (float)tp.w * 0.5f,
                           shell_baseline(f->small, (float)tp.y, (float)tp.h), nm, c->fg_hi, ta);
    }
}

/* ── painting: the finder ───────────────────────────────────────── */
static void paint_find(shell_ctx *c, surface *s, shell_fonts *f)
{
    dock_priv *p = P(c);
    find_hit hit[FIND_MAX];
    int nres = find_search(c, p->q, hit);
    rect a = find_panel_rect(c, s->w, s->h, nres);
    float al = clampf(p->veil.value, 0.f, 1.f);
    corners rc = corners_all((float)c->radius + 2.f);

    draw_round_rect_shadow(s, a, rc, (float)c->shadow_r * 1.4f, 0x000000, c->shadow_a * al, 16);
    draw_blur_region(s, a, c->blur_r);
    draw_round_rect(s, a, rc, c->surface_c, al * clampf(c->panel_a + 0.07f, 0.f, 1.f));
    draw_round_rect_border(s, a, rc, 1.f, c->overlay, al * 0.9f);

    /* A standing "Find" chip inside the field, so the box says what it
     * is even once the placeholder has been typed over. */
    float chw = shell_text_w(f->small, "Find") + 24.f;
    rect chip = { a.x + 18, a.y + (68 - 28) / 2, (int)chw, 28 };
    draw_round_rect(s, chip, corners_all((float)c->radius_sm), c->accent, al * 0.16f);
    shell_text_centred(s, f->small, (float)chip.x + chw * 0.5f,
                       shell_baseline(f->small, (float)chip.y, 28.f), "Find", c->accent, al * 0.95f);

    float qx = (float)(chip.x + chip.w) + 16.f;
    float qb = shell_baseline(f->big, (float)a.y, 68.f);
    if (p->qn) {
        shell_text(s, f->big, qx, qb, p->q, c->fg_hi, al * 0.98f);
        qx += shell_text_w(f->big, p->q) + 3.f;
    }
    /* A caret that does not blink. Blinking would keep the shell
     * redrawing for ever on hardware that would rather be asleep, and
     * it tells the user nothing the shape has not already told them. */
    draw_round_rect(s, (rect){ (int)qx, a.y + 22, 2, 26 }, corners_all(1.f), c->accent, al * 0.9f);
    if (!p->qn)
        shell_text(s, f->mid, qx + 12.f, shell_baseline(f->mid, (float)a.y, 68.f),
                   "the name of a program or a file", c->muted, al * 0.9f);

    draw_line(s, (float)(a.x + 16), (float)(a.y + 68), (float)(a.x + a.w - 16),
              (float)(a.y + 68), 1.f, c->overlay, al * 0.85f);

    for (int i = 0; i < nres; i++) {
        rect r = { a.x + 10, a.y + 76 + i * 48, a.w - 20, 48 };
        if (i == p->find_sel) {
            corners sc = corners_all((float)c->radius_sm);
            draw_round_rect(s, r, sc, c->surface_hi, al * 0.95f);
            draw_round_rect_border(s, r, sc, 1.f, c->accent, al * 0.5f);
        }
        float icx = (float)r.x + 30.f, icy = (float)r.y + (float)r.h * 0.5f;
        draw_circle(s, icx, icy, 16.f, hit[i].tint, al * 0.14f);
        shell_icon_draw(s, hit[i].ic, icx, icy, 21.f, hit[i].tint, al * 0.95f);
        shell_text(s, f->mid, (float)r.x + 56.f, shell_baseline(f->mid, (float)r.y, (float)r.h),
                   hit[i].name, (i == p->find_sel) ? c->fg_hi : c->fg, al * 0.97f);
        float ww = shell_text_w(f->small, hit[i].where);
        shell_text(s, f->small, (float)(r.x + r.w) - 18.f - ww,
                   shell_baseline(f->small, (float)r.y, (float)r.h), hit[i].where,
                   c->subtle, al * 0.8f);
    }
    if (!nres)
        shell_text(s, f->mid, (float)a.x + 30.f, (float)a.y + 104.f,
                   "Nothing by that name.", c->muted, al * 0.9f);

    float fb = shell_baseline(f->small, (float)(a.y + a.h - 34), 34.f);
    shell_text(s, f->small, (float)a.x + 24.f, fb, "Enter opens it", c->muted, al * 0.85f);
    const char *esc = "Esc goes back";
    shell_text(s, f->small, (float)(a.x + a.w) - 24.f - shell_text_w(f->small, esc), fb,
               esc, c->muted, al * 0.85f);
}

/* ── paint ──────────────────────────────────────────────────────── */
static void l_paint(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall)
{
    dock_priv *p = P(c);
    c->screen_w = s->w; c->screen_h = s->h;

    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else surface_fill(s, 0xFF000000u | c->bg);

    /* Back to front, and the focused window last of all: the one the
     * user is in is never covered, and the rest stack in the same order
     * the cascade steps them, so the depth you see is the depth that
     * clicks. Painting these forwards would put the oldest window on
     * top of the newest and the picture would lie. */
    for (int i = c->n_wins - 1; i >= 0; i--)
        if (i != c->focus && !c->wins[i].minimised) paint_win(c, s, f, i, 0);
    if (c->focus >= 0 && c->focus < c->n_wins && !c->wins[c->focus].minimised)
        paint_win(c, s, f, c->focus, 1);

    float veil = clampf(p->veil.value, 0.f, 1.f);
    if (veil > 0.01f) {
        rect full = { 0, 0, s->w, s->h };
        /* Enough softening to push the desktop behind the field, not
         * so much that the wallpaper stops being anything at all. */
        draw_blur_region(s, full, (int)((float)c->blur_r * 0.5f * veil));
        draw_rect(s, full, c->bg, 0.36f * veil);
    }

    paint_dock(c, s, f, 1.f);
    if (veil > 0.01f) paint_find(c, s, f);

    /* The status strip. This archetype asks for a minimal one and gets
     * exactly that: a name and the time. Nothing on it launches and
     * nothing on it switches, because the moment a second place can
     * raise a window, "always in the same spot" stops being true —
     * there would be two spots. */
    int bh = c->bar_h;
    draw_rect(s, (rect){ 0, 0, s->w, bh }, c->bg, 0.5f);
    draw_line(s, 0, (float)bh, (float)s->w, (float)bh, 1.f, c->overlay, 0.45f);
    draw_circle(s, (float)(c->margin + 8), (float)(bh / 2), 6.f, c->accent, 0.95f);
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
}

/* ── input ──────────────────────────────────────────────────────── */
static void find_open(shell_ctx *c, int open)
{
    dock_priv *p = P(c);
    p->find_open = open;
    p->find_sel = 0;
    if (!open) { p->q[0] = 0; p->qn = 0; }
    tween_to(&p->veil, open ? 1.f : 0.f, 0.18f, EASE_OUT_CUBIC);
}

static void raise_app(shell_ctx *c, int app)
{
    int wi = app_window(c, app);
    if (wi < 0) return;                 /* not running: a real shell spawns it here */
    c->wins[wi].minimised = 0;
    c->focus = wi;
}

static int l_click(shell_ctx *c, int x, int y)
{
    dock_priv *p = P(c);
    int sw = c->screen_w, sh = c->screen_h;

    /* The finder sits on top, so it is asked first. */
    if (p->find_open) {
        find_hit hit[FIND_MAX];
        int nres = find_search(c, p->q, hit);
        rect a = find_panel_rect(c, sw, sh, nres);
        for (int i = 0; i < nres; i++) {
            rect r = { a.x + 10, a.y + 76 + i * 48, a.w - 20, 48 };
            if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) continue;
            if (hit[i].app >= 0) raise_app(c, hit[i].app);
            find_open(c, 0);
            return 1;
        }
        if (x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h) return 1;
    }

    /* The dock stays live underneath the finder, and is not dimmed with
     * the rest of the desktop, because it is the one thing that is
     * always there. Someone who started typing and then caught sight of
     * the icon they wanted should not have to dismiss anything first:
     * a favourite is one click away from anywhere, which is the promise
     * the whole archetype makes. */
    dock_slot sl[MAX_SLOTS];
    int n = dock_slots(c, sl);
    rect strip = dock_rect(c, sw, sh, -1);
    if (y >= strip.y && y < strip.y + strip.h && x >= strip.x && x < strip.x + strip.w) {
        for (int i = 0; i < n; i++) {
            rect r = dock_rect(c, sw, sh, i);
            if (x < r.x || x >= r.x + r.w) continue;
            if (sl[i].kind == SLOT_FIND) { find_open(c, !p->find_open); return 1; }
            if (p->find_open) find_open(c, 0);
            raise_app(c, sl[i].app);
            return 1;
        }
        return 1;                       /* the strip itself swallows the click */
    }

    if (p->find_open) { find_open(c, 0); return 1; }   /* anywhere else dismisses */

    /* Windows, front to back. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < c->n_wins; i++) {
            int front = (i == c->focus);
            if ((pass == 0) != front || c->wins[i].minimised) continue;
            rect a = win_rect(c, sw, sh, i);
            if (x < a.x || x >= a.x + a.w || y < a.y || y >= a.y + a.h) continue;
            c->focus = i;
            if (y < a.y + WIN_TITLE_H) {
                float bx = (float)(a.x + a.w - c->padding - 12);
                if (x > bx - 14.f) return 1;                    /* close   */
                if (x > bx - 44.f) { c->wins[i].minimised = 1; return 1; }
                p->drag = i; p->grab_x = x - a.x; p->grab_y = y - a.y;
            }
            return 1;
        }
    }
    return 0;
}

static void l_motion(shell_ctx *c, int x, int y)
{
    dock_priv *p = P(c);
    int sw = c->screen_w, sh = c->screen_h;
    c->mouse_x = x; c->mouse_y = y;

    if (p->drag >= 0) {
        if (!c->mouse_down) { p->drag = -1; }
        else {
            rect g = win_rect(c, sw, sh, p->drag);
            rect strip = dock_rect(c, sw, sh, -1);
            g.x = clampi(x - p->grab_x, -(g.w - 140), sw - 140);
            g.y = clampi(y - p->grab_y, c->bar_h + 4, strip.y - 40);
            c->wins[p->drag].geom = g;
            p->moved |= 1u << p->drag;
            return;
        }
    }

    int hv = -1;
    rect strip = dock_rect(c, sw, sh, -1);
    if (y >= strip.y - 4 && y < strip.y + strip.h) {
        dock_slot sl[MAX_SLOTS];
        int n = dock_slots(c, sl);
        for (int i = 0; i < n; i++) {
            rect r = dock_rect(c, sw, sh, i);
            if (x >= r.x && x < r.x + r.w) { hv = i; break; }
        }
    }
    if (hv != p->hover) {
        p->hover = hv;
        float want = (hv >= 0) ? 1.f : 0.f;
        if (fabsf(p->mag.to - want) > 0.001f) tween_to(&p->mag, want, 0.16f, EASE_OUT_CUBIC);
    }
    c->hover = hv;
}

/* evdev keycodes. A real session gets characters from the keymap; this
 * is the fallback so the archetype is complete on its own, and it is a
 * table rather than a switch because that is all it deserves to be. */
static char key_char(int k)
{
    static const struct { int base; const char *row; } R[] = {
        {  2, "1234567890" }, { 16, "qwertyuiop" },
        { 30, "asdfghjkl"  }, { 44, "zxcvbnm"    },
    };
    if (k == 57) return ' ';
    for (int i = 0; i < 4; i++) {
        int len = (int)strlen(R[i].row);
        if (k >= R[i].base && k < R[i].base + len) return R[i].row[k - R[i].base];
    }
    return 0;
}

static void l_key(shell_ctx *c, int k)
{
    dock_priv *p = P(c);

    if (p->find_open) {
        find_hit hit[FIND_MAX];
        int nres = find_search(c, p->q, hit);
        switch (k) {
            case 1:   find_open(c, 0); return;                        /* ESC       */
            case 28:                                                  /* ENTER     */
                if (p->find_sel < nres && hit[p->find_sel].app >= 0)
                    raise_app(c, hit[p->find_sel].app);
                find_open(c, 0); return;
            case 103: p->find_sel = clampi(p->find_sel - 1, 0, nres ? nres - 1 : 0); return;
            case 108: p->find_sel = clampi(p->find_sel + 1, 0, nres ? nres - 1 : 0); return;
            case 14:  if (p->qn) p->q[--p->qn] = 0;                            /* BACKSPACE */
                      p->find_sel = 0; return;
            default: break;
        }
        char ch = key_char(k);
        if (ch && p->qn < (int)sizeof p->q - 1) { p->q[p->qn++] = ch; p->q[p->qn] = 0; p->find_sel = 0; }
        return;
    }

    if (k == 1 && c->focus >= 0) { c->focus = -1; return; }

    /* Typing anywhere starts the finder with what you typed. This is the
     * whole second half of the archetype: five favourites you point at,
     * everything else you name. Making people first find a search box
     * before they can type a name puts a lookup in front of the lookup. */
    char ch = key_char(k);
    if (ch && ch != ' ') {
        find_open(c, 1);
        p->q[0] = ch; p->q[1] = 0; p->qn = 1;
    }
}

static int l_step(shell_ctx *c, float dt)
{
    dock_priv *p = P(c);
    int a = tween_step(&p->mag, dt);
    int b = tween_step(&p->veil, dt);
    return a || b;
}

static void l_fini(shell_ctx *c) { c->priv = NULL; }

const shell_layout layout_dock = {
    "dock", l_init, l_paint, l_click, l_motion, l_key, l_step, l_fini
};
