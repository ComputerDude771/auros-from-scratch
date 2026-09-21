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
#define DOCK_MAG      0.14f   /* peak icon growth under the pointer      */
#define DOCK_LIFT     4.f
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
    /* Anchored to the left measure, not derived from the screen's
     * centre. "Always in the same spot" means ABSOLUTELY fixed, and a
     * centred strip is not: add one unpinned running app and every
     * favourite in it slides sideways by half a cell. Pinning it to
     * the margin makes the promise literally true, and puts the strip
     * on the same measure as everything else on the page. */
    int sx = c->margin * 2;
    if (sx + inner + 2 * pad > sw - c->margin) sx = sw - c->margin - (inner + 2 * pad);
    if (sx < c->margin) sx = c->margin;
    rect strip = { sx, sh - c->margin - strip_h, inner + 2 * pad, strip_h };
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

/* Everything in the finder is drawn in the same ink. This used to
 * cycle accent / accent_alt / accent_warm / info down the rows purely
 * for variety — which is what docs/THEMING.md forbids in as many
 * words: accent_alt is for a different KIND of thing, not for making
 * a list look less samey. A list of files is one kind of thing. */
static uint32_t tint_of(shell_ctx *c, int i)
{
    (void)i;
    return c->fg;
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
    int pw = sw - 2 * (c->margin + 56); if (pw > 660) pw = 660; if (pw < 320) pw = 320;
    int ph = 72 + (nres ? nres * 50 : 0) + 36;
    /* On the same left measure as the dock below it, so the two things
     * the archetype has line up down one edge instead of both floating
     * on the screen's axis. */
    int px = c->margin * 2;
    if (px + pw > sw - c->margin) px = sw - c->margin - pw;
    if (px < c->margin) px = c->margin;
    return (rect){ px, (int)((float)sh * 0.17f), pw, ph };
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

    uint32_t paper = focused ? c->surface_c : c->bg_alt;
    draw_round_rect(s, a, rc, paper, 1.f);

    rect tb = { a.x, a.y, a.w, WIN_TITLE_H };
    corners tc = { (float)c->radius, (float)c->radius, 0, 0 };
    draw_round_rect(s, tb, tc, focused ? c->bg_alt : c->bg, 1.f);
    draw_hrule(s, a.x, a.y + WIN_TITLE_H - 1, a.w, 1, c->overlay, 1.f);

    shell_icon_draw(s, ic, (float)(a.x + c->padding + 8), (float)(a.y + WIN_TITLE_H / 2),
                    18.f, tint, focused ? c->bg_alt : c->bg, al * (focused ? 1.f : 0.7f));
    float tx = (float)(a.x + c->padding + 28);
    float by = shell_baseline(f->mid, (float)a.y, (float)WIN_TITLE_H);
    shell_text(s, f->mid, tx, by, w->title, focused ? c->fg_hi : c->subtle, al);
    if (w->subtitle[0]) {
        float dx = tx + shell_text_w(f->mid, w->title) + 14.f;
        draw_vrule(s, (int)dx, a.y + 13, WIN_TITLE_H - 26, 1, c->overlay, al);
        shell_text(s, f->small, dx + 12.f, shell_baseline(f->small, (float)a.y, (float)WIN_TITLE_H),
                   w->subtitle, c->subtle, al * (focused ? 0.9f : 0.6f));
    }

    /* Minimise then close. Minimising is safe here precisely because the
     * dock is a fixed place to get the window back from. Two square
     * wells, not two discs: a circle behind a glyph is the one shape
     * this whole theme is trying not to repeat. */
    float cy = (float)(a.y + WIN_TITLE_H / 2);
    float bx = (float)(a.x + a.w - c->padding - 11);
    draw_line(s, bx - 4.f, cy - 4.f, bx + 4.f, cy + 4.f, 1.7f, c->fg, al * 0.95f);
    draw_line(s, bx + 4.f, cy - 4.f, bx - 4.f, cy + 4.f, 1.7f, c->fg, al * 0.95f);
    bx -= 30.f;
    draw_hrule(s, (int)(bx - 5.f), (int)(cy + 3.f), 11, 2, c->fg, al * 0.95f);

    rect body = { a.x, a.y + WIN_TITLE_H, a.w, a.h - WIN_TITLE_H };
    if (w->content) {
        corners bc = { 0, 0, (float)c->radius, (float)c->radius };
        draw_content_fit(s, w->content, body, bc, al);
    } else {
        /* Centre icon and captions as one block sized to the body, and
         * drop the second line when there is no room for it. A cascade
         * on a small panel makes windows genuinely short, and a
         * placeholder anchored to fixed offsets spills out of them. */
        float x    = (float)body.x + (float)c->padding * 1.6f;
        float isz  = clampf((float)body.h * 0.18f, 20.f, 54.f);
        const char *hint = (w->app >= 0) ? c->apps[w->app].hint : "";
        int two = (body.h > 170 && hint[0]);
        float top = (float)body.y + (float)body.h * 0.30f;

        shell_icon_draw(s, ic, x + isz * 0.5f, top, isz, tint, paper, al * 0.9f);
        float y1 = top + isz * 0.5f + 24.f + (f->big ? font_ascent(f->big) : 26.f);
        shell_text(s, f->big, x, y1, w->title, c->fg_hi, al * 0.9f);
        if (two) {
            draw_hrule(s, (int)x, (int)(y1 + (f->big ? font_descent(f->big) : 8.f) + 13.f),
                       (int)((float)body.w * 0.16f), 1, c->overlay, al);
            shell_text(s, f->small, x,
                       y1 + (f->big ? font_descent(f->big) : 8.f) + 28.f
                          + (f->small ? font_ascent(f->small) : 11.f),
                       w->subtitle[0] ? w->subtitle : hint, c->subtle, al * 0.85f);
        }
    }

    /* An unfocused window still needs an edge: on a light theme its
     * surface and the wallpaper are close enough in value that without
     * one the window stops having a shape. */
    draw_frame(s, a, focused ? 2 : 1, focused ? c->fg : c->overlay, 1.f);
    if (focused) draw_hrule(s, a.x, a.y, a.w, 3, c->accent, 1.f);
}

/* ── painting: the dock ─────────────────────────────────────────── */
static void paint_dock(shell_ctx *c, surface *s, shell_fonts *f, float alpha)
{
    dock_priv *p = P(c);
    dock_slot sl[MAX_SLOTS];
    int n = dock_slots(c, sl);
    rect strip = dock_rect(c, s->w, s->h, -1);
    corners rc = corners_all((float)c->radius);

    draw_round_rect(s, strip, rc, c->bg_alt, alpha);
    draw_frame(s, strip, 1, c->overlay, alpha);
    draw_hrule(s, strip.x, strip.y, strip.w, 2, c->fg, alpha * 0.85f);

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
        if (i + 1 < n && sl[i + 1].kind != sl[i].kind)
            draw_vrule(s, r.x + r.w + DOCK_SEP / 2, strip.y + 14,
                       strip.h - 28, 1, c->overlay, alpha);

        if (sl[i].kind == SLOT_FIND) {
            /* The one item that is not a program. It is pinned to the
             * far end, outside the run of favourites, so that opening it
             * up never shifts a favourite by a pixel — and it is spelt
             * out in a word because the archetype's stated cost is that
             * typing to search is a habit some people never form. A
             * button they can see is the cheapest way to start it. */
            rect fp = { r.x, r.y + (r.h - 40) / 2, r.w, 40 };
            int fhot = (i == p->hover);
            uint32_t fpaper = fhot ? c->surface_hi : c->bg_alt;
            if (fhot) draw_round_rect(s, fp, corners_all((float)c->radius_sm), fpaper, alpha);
            draw_frame(s, fp, fhot ? 2 : 1, fhot ? c->fg : c->overlay, alpha);
            shell_icon_draw(s, ICON_PLUS, (float)fp.x + 18.f, (float)fp.y + 20.f,
                            13.f, c->fg, fpaper, alpha * 0.95f);
            shell_text(s, f->mid, (float)fp.x + 30.f,
                       shell_baseline(f->mid, (float)fp.y, 40.f), "Find",
                       c->fg_hi, alpha);
            continue;
        }

        const app_entry *ap = &c->apps[sl[i].app];
        int wi = app_window(c, sl[i].app);
        int running = (wi >= 0);
        int is_focus = (wi >= 0 && wi == c->focus);

        uint32_t paper = c->bg_alt;
        if (i == p->hover && p->mag.value > 0.02f) {
            draw_round_rect(s, r, corners_all((float)c->radius_sm),
                            c->surface_hi, alpha * p->mag.value);
            if (p->mag.value > 0.5f) paper = c->surface_hi;
        }

        float isz = DOCK_ICON * (1.f + DOCK_MAG * k);
        /* A favourite that is not running is still a favourite, so it
         * is drawn in the same ink as the rest. Greying it out would
         * say "unavailable", which is the opposite of what a dock is
         * for; whether it is open is said by the rule beneath it. */
        shell_icon_draw(s, ap->icon, cx, cy, isz, ap->tint, paper,
                        alpha * (running ? 1.f : 0.88f));

        /* Running indicator. One mark per APP, never one per window:
         * "how many windows do I have" is the question a taskbar
         * answers, and answering it here would quietly turn this strip
         * into one. The app you are in gets a wider mark. */
        /* A rule, not a dot. The app you are in gets a wide one in the
         * signal colour; the others get a short one in ink. Two
         * lengths and two values, no second shape. */
        if (running) {
            if (is_focus)
                draw_hrule(s, (int)(cx - 14.f), (int)doty - 2, 28, 4, c->accent, alpha);
            else
                draw_hrule(s, (int)(cx - 6.f), (int)doty, 12, 2, c->fg, alpha * 0.8f);
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
        rect tp = { (int)(tip_x - tw * 0.5f) - 12, strip.y - 34, (int)tw + 24, 26 };
        if (tp.x < c->margin) tp.x = c->margin;
        if (tp.x + tp.w > s->w - c->margin) tp.x = s->w - c->margin - tp.w;
        float ta = alpha * p->mag.value;
        draw_round_rect(s, tp, corners_all((float)c->radius_sm), c->fg_hi, ta);
        /* Ink block, paper letters. A label that has to survive
         * landing on any part of the wallpaper is more legible as a
         * reversed slug than as a translucent chip with a border. */
        shell_text_centred(s, f->mid, (float)tp.x + (float)tp.w * 0.5f,
                           shell_baseline(f->mid, (float)tp.y, (float)tp.h), nm, c->bg, ta);
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

    draw_round_rect(s, a, corners_all((float)c->radius), c->surface_c, al);
    draw_frame(s, a, 1, c->fg, al);
    draw_hrule(s, a.x, a.y, a.w, 3, c->accent, al);

    int pad = c->padding;
    /* A tracked rubric above the field rather than a tinted chip
     * inside it: the box says what it is the way a printed form says
     * what a box is for, in small capitals over the rule. */
    shell_text_tracked(s, f->label, (float)(a.x + pad), (float)(a.y + pad + 12),
                       "FIND", c->subtle, al * 0.95f, 1.8f);

    float qx = (float)(a.x + pad);
    float qb = (float)(a.y + 62);
    if (p->qn) {
        shell_text(s, f->big, qx, qb, p->q, c->fg_hi, al * 0.98f);
        qx += shell_text_w(f->big, p->q) + 4.f;
    }
    /* A caret that does not blink. Blinking would keep the shell
     * redrawing for ever on hardware that would rather be asleep, and
     * it tells the user nothing the shape has not already told them. */
    draw_rect(s, (rect){ (int)qx, (int)qb - 22, 2, 26 }, c->accent, al * 0.95f);
    if (!p->qn)
        shell_text(s, f->mid, qx + 12.f, qb,
                   "the name of a program or a file", c->muted, al * 0.9f);

    draw_hrule(s, a.x + pad, a.y + 72, a.w - pad * 2, 2, c->fg_hi, al * 0.85f);

    for (int i = 0; i < nres; i++) {
        rect r = { a.x, a.y + 76 + i * 50, a.w, 50 };
        uint32_t paper = c->surface_c;
        if (i == p->find_sel) {
            draw_rect(s, r, c->surface_hi, al);
            draw_rect(s, (rect){ r.x, r.y, 4, r.h }, c->accent, al);
            paper = c->surface_hi;
        }
        if (i) draw_hrule(s, r.x + pad, r.y, r.w - pad * 2, 1, c->overlay, al * 0.9f);
        shell_icon_draw(s, hit[i].ic, (float)r.x + (float)pad + 10.f,
                        (float)r.y + (float)r.h * 0.5f, 19.f, hit[i].tint, paper, al);
        shell_text(s, f->dmid, (float)(r.x + pad) + 34.f,
                   shell_baseline(f->dmid, (float)r.y, (float)r.h),
                   hit[i].name, c->fg_hi, al * 0.98f);
        float ww = shell_text_w(f->small, hit[i].where);
        shell_text(s, f->small, (float)(r.x + r.w - pad) - ww,
                   shell_baseline(f->small, (float)r.y, (float)r.h), hit[i].where,
                   c->subtle, al * 0.88f);
    }
    if (!nres)
        shell_text(s, f->dmid, (float)(a.x + pad), (float)a.y + 110.f,
                   "Nothing by that name.", c->muted, al * 0.9f);

    rect foot = { a.x, a.y + a.h - 36, a.w, 36 };
    draw_hrule(s, foot.x + pad, foot.y, foot.w - pad * 2, 1, c->overlay, al * 0.9f);
    float fb = shell_baseline(f->small, (float)foot.y, (float)foot.h);
    shell_text(s, f->small, (float)(a.x + pad), fb, "Enter opens it", c->muted, al * 0.9f);
    const char *esc = "Esc goes back";
    shell_text(s, f->small, (float)(a.x + a.w - pad) - shell_text_w(f->small, esc), fb,
               esc, c->muted, al * 0.9f);
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
        /* A flat veil of the desk colour. Blurring the whole
         * framebuffer to push the desktop back was the single most
         * expensive thing this archetype did per frame, and a wash
         * does the same job: what is behind is still there, still
         * recognisable, and visibly not the thing you are using. */
        draw_rect(s, full, c->bg, 0.62f * veil);
    }

    paint_dock(c, s, f, 1.f);
    if (veil > 0.01f) paint_find(c, s, f);

    /* The status strip. This archetype asks for a minimal one and gets
     * exactly that: a name and the time. Nothing on it launches and
     * nothing on it switches, because the moment a second place can
     * raise a window, "always in the same spot" stops being true —
     * there would be two spots. */
    int bh = c->bar_h;
    draw_rect(s, (rect){ 0, 0, s->w, bh }, c->bg_alt, 1.f);
    draw_hrule(s, 0, bh, s->w, 1, c->overlay, 1.f);
    draw_rect(s, (rect){ c->margin, bh/2 - 4, 8, 8 }, c->accent, 1.f);
    shell_text_tracked(s, f->label, (float)(c->margin + 20),
                       shell_baseline(f->label, 0.f, (float)bh),
                       c->brand, c->subtle, 0.95f, 1.6f);

    if (c->show_clock) {
        char hm[32], dt[48];
        shell_clock(hm, sizeof hm, dt, sizeof dt);
        float rx = (float)(s->w - c->margin);
        shell_text(s, f->mid, rx - shell_text_w(f->mid, hm),
                   shell_baseline(f->mid, 0.f, (float)bh), hm, c->fg_hi, 1.f);
        rx -= shell_text_w(f->mid, hm) + 13.f;
        draw_vrule(s, (int)rx, 8, bh - 16, 1, c->overlay, 1.f);
        rx -= 13.f;
        shell_text(s, f->small, rx - shell_text_w(f->small, dt),
                   shell_baseline(f->small, 0.f, (float)bh), dt, c->subtle, 0.9f);
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

/* Which dock slot is under (x,y)? -1 for none.
 *
 * ONE function, called by both motion and click. They used to test
 * slightly different bands: motion accepted a 4px lead-in above the
 * strip so an icon starts growing as the pointer arrives, and click did
 * not -- so that 4px row magnified an icon and then swallowed the click
 * without doing anything. That is the exact row a person hits when they
 * shove the pointer at the dock and click the moment it responds, which
 * is how docks are used. Found by the affordance check in
 * tools/hittest.c, which flags anything that highlights and then
 * ignores a click.
 *
 * `band` says whether (x,y) is on the strip at all, so the strip can
 * still swallow clicks that land between icons. */
/* The hit region IS the painted strip. Exactly, on both paths.
 *
 * There used to be a four-pixel lead-in above it, so the icons would
 * light up as the pointer arrived rather than once it was exactly
 * inside. That band was not painted, and a band that is not painted can
 * only be wrong in one of two ways: honour the click there and clicking
 * bare wallpaper activates whatever is under that column (tools/hittest
 * found 109 such clicks, on one scanline); refuse the click and the
 * dock lights up and then ignores you, which docs/DESIGN.md forbids by
 * name. Splitting it -- hover yes, click no -- is just choosing the
 * second failure.
 *
 * So there is no band. The pointer engages when it is on the dock, and
 * the dock is where it is drawn. */
static int dock_hit(shell_ctx *c, int sw, int sh, int x, int y, int *band)
{
    rect strip = dock_rect(c, sw, sh, -1);
    int on = (y >= strip.y && y < strip.y + strip.h &&
              x >= strip.x && x < strip.x + strip.w);
    if (band) *band = on;
    if (!on) return -1;

    dock_slot sl[MAX_SLOTS];
    int n = dock_slots(c, sl);
    for (int i = 0; i < n; i++) {
        rect r = dock_rect(c, sw, sh, i);
        if (x >= r.x && x < r.x + r.w) return i;
    }
    return -1;
}

/* Activate a favourite.
 *
 * If it is running, raise it. If it is NOT running, START it -- which
 * is the primary action of this entire archetype and used to be a
 * `return` with a comment saying a real shell would do it here. The
 * dock's promise, in docs/SHELLS.md, is a strip of your programs
 * "always in the same order, WHETHER RUNNING OR NOT, so the thing you
 * want is always in the same spot". Clicking a favourite that was not
 * already open did nothing at all, silently, while the icon obligingly
 * magnified under the pointer to say it could be clicked. */
/* Starting the program is shell_launch()'s job: it finds an existing
 * window for the application or creates a slot AND asks the host to
 * spawn it. Every archetype used to hand-roll the slot and never spawn
 * anything, so every icon in this product opened a rectangle with a
 * name in it and nothing behind the rectangle. */
static void raise_app(shell_ctx *c, int app)
{
    int wi = shell_launch(c, app);
    if (wi < 0) return;
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
            rect r = { a.x, a.y + 76 + i * 50, a.w, 50 };
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
    int on_strip = 0;
    int slot = dock_hit(c, sw, sh, x, y, &on_strip);
    if (slot >= 0) {
        dock_slot sl[MAX_SLOTS];
        dock_slots(c, sl);
        if (sl[slot].kind == SLOT_FIND) { find_open(c, !p->find_open); return 1; }
        if (p->find_open) find_open(c, 0);
        /* Clicking the one you are already looking at puts it away.
         * Otherwise that click is the one place in the dock where
         * something magnifies under the pointer and then does nothing
         * at all -- and `taskbar` already behaves this way, so the two
         * archetypes agree rather than each inventing an answer. */
        int wi = app_window(c, sl[slot].app);
        if (wi >= 0 && wi == c->focus && !c->wins[wi].minimised) {
            c->wins[wi].minimised = 1;
            c->focus = -1;
            for (int i = 0; i < c->n_wins; i++)
                if (!c->wins[i].minimised) { c->focus = i; break; }
            return 1;
        }
        raise_app(c, sl[slot].app);
        return 1;
    }
    if (on_strip) return 1;             /* the strip itself swallows the click */

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

    int hv = dock_hit(c, sw, sh, x, y, NULL);
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
    .id = "dock",
    .init = l_init,
    .paint = l_paint,
    .click = l_click,
    .motion = l_motion,
    .key = l_key,
    .step = l_step,
    .fini = l_fini,
};
