/* layouts/workbench.c — the "Panes and keyboard" archetype.
 *
 * Windows never overlap. They divide the screen between them, so
 * everything open is visible at once, and you move between them with
 * the keyboard. Six workspaces sit behind the six number keys.
 *
 * This is the one archetype allowed to require learning, and the
 * chooser says so plainly so that nobody picks it by accident. What it
 * is NOT allowed to do is the thing real tiling window managers do on
 * first launch: hand someone a blank screen and no way to discover what
 * any key does. That is an inherited defect, not an inherent one, so
 * there is one small hint strip along the bottom which the user
 * dismisses once. It reserves its own band rather than floating over a
 * pane, because "nothing ever overlaps" is the whole promise here and
 * our own UI is the worst possible place to make an exception.
 *
 * Nothing else is softened. There is no dock, no home screen, no
 * minimise, and the mouse does almost nothing.
 */
#include "../shell.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define WB_MAX_WS  12       /* indicator slots; the .shell asks for 6 */
#define WB_HINT_H  28       /* height of the reserved hint band       */

typedef struct {
    /* Which workspace each window lives on. The ctx has no per-window
     * workspace field, and it should not grow one for a single
     * archetype's sake — five other layouts have no concept of a
     * workspace at all. So the map lives here, which is exactly what
     * c->priv is for. */
    int   ws_of[SHELL_MAX_WINS];

    /* The screen the last paint used. click() and motion() are handed
     * coordinates but not a size, so geometry is answered against the
     * frame the user is actually looking at rather than a guess. */

    int   hover_ws;          /* workspace chip under the pointer, -1   */
    int   hover_hint;        /* pointer is over the hint strip         */
    tween hint;              /* 1 = hints shown, 0 = dismissed         */

    /* Chip and pill rects depend on text metrics, which only paint()
     * has. Measured there, remembered here for the hit tests. */
    rect  ws_r[WB_MAX_WS];
    rect  hint_r;

    float mem, load;         /* -1 when unreadable */
    float sample_age;
} wb_priv;

static wb_priv *P(shell_ctx *c) { return (wb_priv *)c->priv; }

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

/* ── theme-derived metrics ───────────────────────────────────────────
 * Spacing is the theme's business. `gap` is literally the key for
 * "space between tiled windows"; padding is the sane fallback for a
 * theme written before it existed. */
static int wb_gap(shell_ctx *c)
{
    int g = theme_int(&c->theme, "gap", c->padding);
    if (g < 2)  g = 2;
    if (g > 40) g = 40;
    return g;
}

static int wb_ws_count(shell_ctx *c)
{
    int n = c->workspaces > 0 ? c->workspaces : 1;
    return n > WB_MAX_WS ? WB_MAX_WS : n;
}

/* target_size="dense": titlebars are as short as the type allows. */
static int wb_title_h(shell_ctx *c, shell_fonts *f)
{
    float lh = (f && f->small) ? font_line_height(f->small) : 17.f;
    int h = (int)(lh + (c->target_large ? 18.f : 11.f));
    return h < 22 ? 22 : h;
}

/* The hint band shrinks to nothing as the strip fades, so the panes
 * grow into the space instead of the space merely emptying. */
static int wb_hint_band(shell_ctx *c)
{
    wb_priv *p = P(c);
    float v = p ? p->hint.value : 0.f;
    if (v <= 0.005f) return 0;
    return (int)((float)(WB_HINT_H + wb_gap(c)) * v + 0.5f);
}

/* ── the tiler ───────────────────────────────────────────────────────
 *
 * A balanced binary space partition: split the region along its LONGER
 * axis into two parts whose sizes are proportional to how many windows
 * each will hold, then recurse. Every window ends up with the same
 * area, and index order runs top-left to bottom-right.
 *
 * Master/stack was the other candidate and it is the more famous
 * model, but a fixed master fraction starves the stack: at seven
 * windows on a 16:9 screen the stack columns come out barely two
 * hundred pixels wide, which is the sliver problem the whole archetype
 * is supposed to avoid. The BSP has no fixed fraction to starve: over
 * one through sixteen panes on a 1600x900 screen the narrowest pane it
 * produces is 1:2.58 and the widest 3.01:1, both shapes you can
 * actually read a document in. Promoting a pane is still available; it
 * just swaps into slot 0 (top-left) rather than into a privileged
 * region, since with equal areas there is no master to promote into.
 *
 * Each split subtracts one gap and gives the remainder to the second
 * child, so the parts always sum back to exactly the parent: no
 * rounding crack between panes, no unfilled strip at the edge.
 */
/* MASTER_BIAS applies to the FIRST cut only. A BSP of exactly equal
 * halves produces a screen with no first pane and no second — every
 * region the same size, every boundary on a midline, which is the
 * tiler equivalent of centring everything. One biased cut at the top
 * of the tree gives the work you are actually doing a visibly larger
 * region and leaves the rest of the recursion untouched, so it does
 * NOT reintroduce the starved-stack problem a fixed master fraction
 * at every level would: the bias is applied once, and it is clamped,
 * so the smallest pane at sixteen windows is the same shape it was. */
#define MASTER_BIAS 1.20f

static void wb_split_d(rect r, int first, int n, int gap, rect *out, int depth)
{
    if (n <= 0) return;
    if (n == 1) { out[first] = r; return; }

    /* At the top of the tree the cut favours the FIRST group and makes
     * it the smaller COUNT, so slot 0 ends up in a region with fewer
     * panes and more room: a master. Below the top it is the plain
     * balanced BSP again, which is what keeps the sixteen-window case
     * out of the sliver regime a fixed master fraction at every level
     * would put it in. */
    int a = (depth == 0) ? (n / 2 < 1 ? 1 : n / 2) : (n + 1) / 2;
    int b = n - a;
    float frac = (float)a / (float)n;
    if (depth == 0) {
        frac *= MASTER_BIAS;
        if (frac > 0.70f) frac = 0.70f;
        if (frac < 0.28f) frac = 0.28f;
    }

    if (r.w >= r.h) {
        int avail = r.w - gap;
        if (avail < 2) avail = 2;          /* unreachable at sane sizes */
        int wa = (int)((float)avail * frac + 0.5f);
        if (wa < 1) wa = 1;
        if (wa > avail - 1) wa = avail - 1;
        rect ra = { r.x,            r.y, wa,          r.h };
        rect rb = { r.x + wa + gap, r.y, avail - wa,  r.h };
        wb_split_d(ra, first,     a, gap, out, depth + 1);
        wb_split_d(rb, first + a, b, gap, out, depth + 1);
    } else {
        int avail = r.h - gap;
        if (avail < 2) avail = 2;
        int ha = (int)((float)avail * frac + 0.5f);
        if (ha < 1) ha = 1;
        if (ha > avail - 1) ha = avail - 1;
        rect ra = { r.x, r.y,            r.w, ha         };
        rect rb = { r.x, r.y + ha + gap, r.w, avail - ha };
        wb_split_d(ra, first,     a, gap, out, depth + 1);
        wb_split_d(rb, first + a, b, gap, out, depth + 1);
    }
}

static void wb_split(rect r, int first, int n, int gap, rect *out)
{
    wb_split_d(r, first, n, gap, out, 0);
}

/* The area the panes get: everything below the status strip, above the
 * hint band, inset by one gap so the outer margin matches the inner
 * ones and the spacing reads as even all the way to the edge. */
static rect wb_content(shell_ctx *c, int W, int H)
{
    int gap  = wb_gap(c);
    int top  = c->bar_h + gap;
    int bot  = gap + wb_hint_band(c);
    rect a = { gap, top, W - gap * 2, H - top - bot };
    if (a.w < 1) a.w = 1;
    if (a.h < 1) a.h = 1;
    return a;
}

/* ONE function owns pane geometry. paint() draws what it returns and
 * click() hit-tests what it returns, so a click always lands on what
 * the user can see; deriving the two separately is how a UI ends up
 * off by the width of a border. */
static int wb_panes(shell_ctx *c, int W, int H, rect *out, int *idx)
{
    wb_priv *p = P(c);
    int n = 0;
    for (int i = 0; i < c->n_wins && n < SHELL_MAX_WINS; i++) {
        /* minimise="no" here, so the flag is always clear; honouring it
         * anyway costs one comparison and avoids a surprise later. */
        if (c->wins[i].minimised) continue;
        if (p && p->ws_of[i] != c->workspace) continue;
        idx[n++] = i;
    }
    if (n == 0) return 0;
    wb_split(wb_content(c, W, H), 0, n, wb_gap(c), out);
    return n;
}

static int wb_slot_of(int n, const int *idx, int wi)
{
    for (int k = 0; k < n; k++) if (idx[k] == wi) return k;
    return -1;
}

static void wb_focus_first(shell_ctx *c)
{
    wb_priv *p = P(c);
    c->focus = -1;
    for (int i = 0; i < c->n_wins; i++)
        if (p->ws_of[i] == c->workspace) { c->focus = i; return; }
}

/* ── system readouts ─────────────────────────────────────────────────
 * Real numbers, not decoration. Sampling is file I/O, so it happens in
 * init and step — never in paint, which has to stay a pure function of
 * state. A shell that is asleep does not resample, which is correct:
 * nobody is looking. */
static void wb_sample(wb_priv *p)
{
    p->sample_age = 0.f;
    p->mem = p->load = -1.f;

    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[160];
        double total = 0, avail = -1;
        while (fgets(line, sizeof line, fp)) {
            double v;
            if (sscanf(line, "MemTotal: %lf", &v) == 1) total = v;
            else if (sscanf(line, "MemAvailable: %lf", &v) == 1) { avail = v; break; }
        }
        fclose(fp);
        if (total > 0 && avail >= 0)
            p->mem = clampf((float)((total - avail) / total), 0.f, 1.f);
    }
    fp = fopen("/proc/loadavg", "r");
    if (fp) {
        double l;
        if (fscanf(fp, "%lf", &l) == 1) p->load = (float)l;
        fclose(fp);
    }
}

/* ── text fitting ────────────────────────────────────────────────────
 * A title that runs into the clock is worse than a title with its tail
 * cut off, so measure and cut. */
static void wb_fit(char *dst, size_t n, font *f, const char *src, float maxw)
{
    char buf[120];
    snprintf(buf, sizeof buf, "%s", src);
    if (!f) { snprintf(dst, n, "%s", buf); return; }
    if (maxw <= 10.f) { dst[0] = 0; return; }
    if (shell_text_w(f, buf) <= maxw) { snprintf(dst, n, "%s", buf); return; }

    size_t len = strlen(buf);
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--;
        buf[len] = 0;
        char probe[128];
        snprintf(probe, sizeof probe, "%s…", buf);
        if (shell_text_w(f, probe) <= maxw) { snprintf(dst, n, "%s", probe); return; }
    }
    dst[0] = 0;
}

/* ── lifecycle ───────────────────────────────────────────────────────*/
static void l_init(shell_ctx *c)
{
    static wb_priv priv;        /* one layout is live at a time, as in rail.c */
    memset(&priv, 0, sizeof priv);
    priv.hover_ws = -1;
    tween_set(&priv.hint, 1.f); /* hints start shown; they exist for day one */
    c->priv = &priv;

    if (c->workspace < 0 || c->workspace >= wb_ws_count(c)) c->workspace = 0;
    for (int i = 0; i < SHELL_MAX_WINS; i++) priv.ws_of[i] = c->workspace;

    /* Everything already open belongs to the workspace you are on —
     * which is what a real tiler does, since a window lands on whatever
     * workspace was current when it opened. The other five therefore
     * start genuinely empty, and the indicators say so rather than
     * pretending otherwise. */
    if (c->focus < 0 || c->focus >= c->n_wins) wb_focus_first(c);
    wb_sample(&priv);
    c->hover = -1;
}

static void l_fini(shell_ctx *c) { c->priv = NULL; }

/* ── painting ────────────────────────────────────────────────────────*/
static void paint_pane(shell_ctx *c, surface *s, shell_fonts *f,
                       rect a, int wi, int focused, int hovered)
{
    const win_entry *w = &c->wins[wi];
    uint32_t tint = (w->app >= 0) ? c->apps[w->app].tint : c->fg;
    shell_icon ic = (w->app >= 0) ? c->apps[w->app].icon : ICON_WINDOW;
    corners  cr   = corners_all((float)c->radius);
    float    dim  = focused ? 1.f : 0.85f;

    /* No shadow at all. At a ten pixel gutter a soft shadow just fills
     * the gutters with smudge and the grid stops reading as a grid —
     * which is the one thing this archetype is selling. The gutter IS
     * the separation; a rule closes each pane and that is enough. */
    uint32_t paper = focused ? c->surface_c : c->bg_alt;
    draw_round_rect(s, a, cr, paper, 1.f);

    int th = wb_title_h(c, f);
    if (th > a.h) th = a.h;

    rect tb = { a.x, a.y, a.w, th };
    corners tc = { (float)c->radius, (float)c->radius, 0, 0 };
    draw_round_rect(s, tb, tc,
                    focused ? c->bg_alt : (hovered ? c->surface_hi : c->bg), 1.f);
    draw_hrule(s, a.x, a.y + th - 1, a.w, 1, c->overlay, 1.f);

    float pad = (float)c->padding * 0.55f;
    float icx = (float)a.x + pad + 7.f;
    float tx  = icx + 14.f;
    float by  = shell_baseline(f->label, (float)a.y, (float)th);
    shell_icon_draw(s, ic, icx, (float)(a.y + th / 2), 14.f, tint,
                    focused ? c->bg_alt : c->bg, dim);

    char title[128];
    wb_fit(title, sizeof title, f->label, w->title,
           (float)(a.x + a.w) - pad - tx);
    shell_text(s, f->label, tx, by, title, focused ? c->fg_hi : c->subtle, dim);

    draw_frame(s, a, focused ? 2 : 1, focused ? c->fg : c->overlay, 1.f);
    if (focused) draw_hrule(s, a.x, a.y, a.w, 3, c->accent, 1.f);

    rect body = { a.x, a.y + th, a.w, a.h - th };
    if (body.h <= 2 || body.w <= 2) return;

    if (w->content) {
        corners bc = { 0, 0, (float)c->radius, (float)c->radius };
        draw_content_fit(s, w->content, body, bc, dim);
        return;
    }

    /* Placeholder, sized to the pane it is in: a fixed block looks
     * stranded in a big pane and overflows a small one. */
    float x   = (float)body.x + (float)c->padding * 0.9f;
    float isz = clampf((body.w < body.h ? (float)body.w : (float)body.h) * 0.16f, 16.f, 46.f);
    float room = (float)body.w - (float)c->padding * 1.8f;

    int show_name = body.h >= 96 && body.w >= 110;
    int show_sub  = body.h >= 152 && body.w >= 150 && w->subtitle[0];
    float top = (float)body.y + (float)body.h * 0.30f;

    shell_icon_draw(s, ic, x + isz * 0.5f, top, isz, tint, paper, dim * 0.9f);

    if (show_name) {
        font *nf = (body.w >= 260) ? f->dmid : f->mid;
        float ny = top + isz * 0.5f + 20.f + (nf ? font_ascent(nf) : 16.f);
        char nm[128];
        wb_fit(nm, sizeof nm, nf, w->title, room);
        shell_text(s, nf, x, ny, nm, c->fg_hi, dim * 0.92f);
        if (show_sub) {
            draw_hrule(s, (int)x, (int)(ny + (nf ? font_descent(nf) : 6.f) + 11.f),
                       (int)(room * 0.22f), 1, c->overlay, dim);
            char sb[128];
            wb_fit(sb, sizeof sb, f->small, w->subtitle, room);
            shell_text(s, f->small, x,
                       ny + (nf ? font_descent(nf) : 6.f) + 24.f
                          + (f->small ? font_ascent(f->small) : 11.f),
                       sb, c->subtle, dim * 0.88f);
        }
    }
}

static void wb_meter(surface *s, float x, float y, float w, float h,
                     float frac, uint32_t col)
{
    /* A gauge rule, not a pill: two square spans, the ground and the
     * reading. Same shape as every other rule on the screen. */
    rect back = { (int)x, (int)y, (int)w, (int)h };
    draw_rect(s, back, col, 0.22f);
    if (frac < 0.f) return;
    int fw = (int)(w * clampf(frac, 0.f, 1.f) + 0.5f);
    if (fw < 2) fw = 2;
    rect fill = { (int)x, (int)y, fw, (int)h };
    draw_rect(s, fill, col, 0.95f);
}

/* status_strip="full": workspaces, what has focus, where you are in the
 * set, the clock and two honest readouts. The workspace chips are the
 * one thing on it the pointer can usefully hit. */
static void paint_status(shell_ctx *c, surface *s, shell_fonts *f,
                         int n, const int *idx)
{
    wb_priv *p = P(c);
    int bh  = c->bar_h;
    int nws = wb_ws_count(c);
    rect bar = { 0, 0, s->w, bh };

    draw_rect(s, bar, c->bg_alt, 1.f);
    draw_hrule(s, 0, bh, s->w, 1, c->overlay, 1.f);

    float by = shell_baseline(f->small, 0.f, (float)bh);
    float x  = (float)c->margin;

    draw_rect(s, (rect){ (int)x, bh/2 - 4, 8, 8 }, c->accent, 1.f);
    x += 20.f;

    /* How many windows each workspace holds. Occupied and current are
     * three visibly different states, not two shades of the same one. */
    int count[WB_MAX_WS];
    memset(count, 0, sizeof count);
    for (int i = 0; i < c->n_wins; i++) {
        int w = p->ws_of[i];
        if (w >= 0 && w < nws && !c->wins[i].minimised) count[w]++;
    }

    int chip_h = bh - 16; if (chip_h < 16) chip_h = 16;
    float chip_y = ((float)bh - (float)chip_h) * 0.5f;
    for (int i = 0; i < nws; i++) {
        char lab[16];   /* room for any workspace count a theme dreams up */
        snprintf(lab, sizeof lab, "%d", i + 1);
        float tw = shell_text_w(f->small, lab);
        int cw = (int)(tw + 14.f); if (cw < chip_h + 2) cw = chip_h + 2;
        rect ch = { (int)x, (int)chip_y, cw, chip_h };
        p->ws_r[i] = ch;

        int cur = (i == c->workspace);
        int occ = count[i] > 0;
        int hot = (p->hover_ws == i);

        if (cur) {
            draw_round_rect(s, ch, corners_all((float)c->radius_sm), c->accent, 1.f);
        } else if (occ) {
            draw_round_rect(s, ch, corners_all((float)c->radius_sm),
                            hot ? c->surface_hi : c->bg, 1.f);
            draw_frame(s, ch, 1, c->overlay, 1.f);
        } else {
            if (hot) draw_round_rect(s, ch, corners_all((float)c->radius_sm),
                                     c->bg, 0.8f);
            draw_round_rect_border(s, ch, corners_all((float)c->radius_sm), 1.f,
                                   c->overlay, 0.6f);
        }
        shell_text_centred(s, f->small, x + (float)cw * 0.5f,
                           shell_baseline(f->small, chip_y, (float)chip_h), lab,
                           cur ? c->bg : (occ ? c->accent : c->muted),
                           cur ? 1.f : (occ ? 0.95f : 0.7f));
        x += (float)cw + 5.f;
    }
    for (int i = nws; i < WB_MAX_WS; i++) { rect z = {0,0,0,0}; p->ws_r[i] = z; }

    x += 8.f;
    draw_line(s, x, (float)bh * 0.28f, x, (float)bh * 0.72f, 1.f, c->overlay, 0.7f);
    x += 13.f;

    /* Build the right-hand side first so the title knows where it must
     * stop. Right to left: clock, date, load, memory. */
    float rx = (float)(s->w - c->margin);
    if (c->show_clock) {
        char hm[32], dt[48];
        shell_clock(hm, sizeof hm, dt, sizeof dt);
        rx -= shell_text_w(f->small, hm);
        shell_text(s, f->small, rx, by, hm, c->fg_hi, 0.96f);
        rx -= 11.f + shell_text_w(f->small, dt);
        shell_text(s, f->small, rx, by, dt, c->subtle, 0.75f);
        rx -= 15.f;
        draw_line(s, rx, (float)bh * 0.28f, rx, (float)bh * 0.72f, 1.f, c->overlay, 0.6f);
        rx -= 15.f;
    }
    if (p->load >= 0.f) {
        char v[24];
        snprintf(v, sizeof v, "%.2f", (double)p->load);
        rx -= shell_text_w(f->small, v);
        shell_text(s, f->small, rx, by, v, c->subtle, 0.85f);
        rx -= 6.f + shell_text_w(f->small, "load");
        shell_text(s, f->small, rx, by, "load", c->muted, 0.75f);
        rx -= 16.f;
    }
    if (p->mem >= 0.f) {
        rx -= 38.f;
        /* Ink until it matters. A readout that is the same colour as
         * "this is the thing you are in" spends the signal on a number
         * nobody is looking at; it goes red only when the machine is
         * genuinely close to out of memory, which is a state. */
        wb_meter(s, rx, (float)bh * 0.5f - 2.5f, 38.f, 5.f, p->mem,
                 p->mem > 0.88f ? c->accent : c->fg);
        rx -= 6.f + shell_text_w(f->small, "mem");
        shell_text(s, f->small, rx, by, "mem", c->muted, 0.75f);
        rx -= 16.f;
    }

    /* What has focus, and where it sits in the set. */
    float avail = rx - x;
    if (avail < 40.f) return;

    if (c->focus >= 0 && c->focus < c->n_wins) {
        const win_entry *w = &c->wins[c->focus];
        uint32_t tint = (w->app >= 0) ? c->apps[w->app].tint : c->fg;
        shell_icon ic = (w->app >= 0) ? c->apps[w->app].icon : ICON_WINDOW;
        shell_icon_draw(s, ic, x + 7.f, (float)bh * 0.5f, 14.f, tint, c->bg_alt, 0.95f);
        x += 20.f; avail -= 20.f;

        char pos[24]; pos[0] = 0;
        if (c->show_positions && n > 0) {
            int slot = wb_slot_of(n, idx, c->focus);
            if (slot >= 0) snprintf(pos, sizeof pos, "%d/%d", slot + 1, n);
        }
        float posw = pos[0] ? shell_text_w(f->small, pos) + 12.f : 0.f;

        char title[128];
        wb_fit(title, sizeof title, f->small, w->title, avail - posw);
        shell_text(s, f->small, x, by, title, c->fg_hi, 0.96f);
        x += shell_text_w(f->small, title);

        if (pos[0]) {
            shell_text(s, f->small, x + 12.f, by, pos, c->muted, 0.75f);
            x += 12.f + shell_text_w(f->small, pos);
        }

        /* The subtitle is the first thing to go when the bar is tight. */
        if (w->subtitle[0] && rx - x > 90.f) {
            char sub[128];
            wb_fit(sub, sizeof sub, f->small, w->subtitle, rx - x - 18.f);
            if (sub[0]) shell_text(s, f->small, x + 14.f, by, sub, c->subtle, 0.6f);
        }
    } else {
        shell_text(s, f->small, x, by, "Nothing open", c->muted, 0.7f);
    }
}

/* ── the one humane touch ────────────────────────────────────────────
 *
 * Six bindings, one row, the theme's own colours, dismissed with a
 * click or with Esc. That is the whole concession: this archetype is
 * hard on day one by design, but there is a difference between hard and
 * undiscoverable, and a blank screen with no clue is the second one.
 */
static const struct { const char *cap, *label; } WB_HINTS[] = {
    { "1-6",     "workspace" },
    { "h j k l", "focus"     },
    { "arrows",  "swap pane" },
    { "enter",   "to front"  },
    { "q",       "close"     },
    { "esc",     "hide this" },
};
#define WB_N_HINTS ((int)(sizeof WB_HINTS / sizeof WB_HINTS[0]))

static void paint_hints(shell_ctx *c, surface *s, shell_fonts *f)
{
    wb_priv *p = P(c);
    float a = p->hint.value;
    if (a <= 0.01f) { rect z = {0,0,0,0}; p->hint_r = z; return; }

    float capw[WB_N_HINTS], labw[WB_N_HINTS], total = 0.f;
    for (int i = 0; i < WB_N_HINTS; i++) {
        capw[i] = shell_text_w(f->label, WB_HINTS[i].cap) + 13.f;
        labw[i] = shell_text_w(f->small, WB_HINTS[i].label);
        total += capw[i] + 7.f + labw[i];
    }
    total += 15.f * (float)(WB_N_HINTS - 1);

    float pad = (float)c->padding;
    int pw = (int)(total + pad * 2.f);
    if (pw > s->w - c->margin * 2) pw = s->w - c->margin * 2;
    int gap = wb_gap(c);
    /* Off the axis: it sits on the same left measure as the pane grid
     * above it rather than centred under it. Two things stacked on the
     * screen's midline is not a composition. */
    rect pill = { gap, s->h - gap - WB_HINT_H, pw, WB_HINT_H };
    p->hint_r = pill;

    corners pc = corners_all((float)c->radius_sm);
    draw_round_rect(s, pill, pc, c->surface_c, a);
    draw_frame(s, pill, 1, p->hover_hint ? c->fg : c->overlay, a);

    float x  = (float)pill.x + pad;
    float cy = (float)pill.y + (float)pill.h * 0.5f;
    int   chh = 17;
    for (int i = 0; i < WB_N_HINTS; i++) {
        rect cap = { (int)x, (int)(cy - (float)chh * 0.5f), (int)capw[i], chh };
        draw_round_rect(s, cap, corners_all((float)c->radius_sm), c->fg_hi, 0.92f * a);
        shell_text_centred(s, f->label, x + capw[i] * 0.5f,
                           shell_baseline(f->label, (float)cap.y, (float)chh),
                           WB_HINTS[i].cap, c->bg, a);
        x += capw[i] + 7.f;
        shell_text(s, f->small, x, shell_baseline(f->small, (float)pill.y, (float)pill.h),
                   WB_HINTS[i].label, c->subtle, 0.9f * a);
        x += labw[i];
        if (i < WB_N_HINTS - 1) {
            draw_vrule(s, (int)x + 7, pill.y + 7, pill.h - 14, 1, c->overlay, 0.9f * a);
            x += 15.f;
        }
    }
}

static void l_paint(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall)
{
    c->screen_w = s->w; c->screen_h = s->h;

    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else surface_fill(s, 0xFF000000u | c->bg);

    rect r[SHELL_MAX_WINS];
    int  idx[SHELL_MAX_WINS];
    int  n = wb_panes(c, s->w, s->h, r, idx);

    for (int k = 0; k < n; k++)
        paint_pane(c, s, f, r[k], idx[k],
                   idx[k] == c->focus, idx[k] == c->hover);

    if (n == 0) {
        /* An empty workspace states what it is. It does not explain how
         * to leave — the hint strip already says the number keys switch,
         * and saying it twice would be nagging. */
        rect a = wb_content(c, s->w, s->h);
        char msg[64];
        snprintf(msg, sizeof msg, "Workspace %d is empty", c->workspace + 1);
        shell_text_centred(s, f->small, (float)a.x + (float)a.w * 0.5f,
                           (float)a.y + (float)a.h * 0.5f, msg, c->muted, 0.6f);
    }

    paint_status(c, s, f, n, idx);
    paint_hints(c, s, f);
}

/* ── input ───────────────────────────────────────────────────────────*/
static int l_click(shell_ctx *c, int x, int y)
{
    wb_priv *p = P(c);

    /* The whole hint strip is the dismiss target. Asking someone to hit
     * a twelve pixel × to get rid of the thing that exists to help them
     * would be a small joke at their expense. */
    if (p->hint.value > 0.5f) {
        rect h = p->hint_r;
        if (h.w > 0 && x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
            tween_to(&p->hint, 0.f, 0.22f, EASE_OUT_CUBIC);
            return 1;
        }
    }

    int nws = wb_ws_count(c);
    for (int i = 0; i < nws; i++) {
        rect w = p->ws_r[i];
        if (w.w <= 0) continue;
        if (x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h) {
            if (i != c->workspace) { c->workspace = i; wb_focus_first(c); }
            return 1;
        }
    }

    rect r[SHELL_MAX_WINS]; int idx[SHELL_MAX_WINS];
    int n = wb_panes(c, c->screen_w, c->screen_h, r, idx);
    for (int k = 0; k < n; k++)
        if (x >= r[k].x && x < r[k].x + r[k].w && y >= r[k].y && y < r[k].y + r[k].h) {
            c->focus = idx[k];
            return 1;
        }
    return 0;
}

static void l_motion(shell_ctx *c, int x, int y)
{
    wb_priv *p = P(c);
    p->hover_ws = -1;
    p->hover_hint = 0;
    c->hover = -1;
    c->mouse_x = x; c->mouse_y = y;

    rect h = p->hint_r;
    if (p->hint.value > 0.5f && h.w > 0 &&
        x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
        p->hover_hint = 1;
        return;
    }
    int nws = wb_ws_count(c);
    for (int i = 0; i < nws; i++) {
        rect w = p->ws_r[i];
        if (w.w > 0 && x >= w.x && x < w.x + w.w && y >= w.y && y < w.y + w.h) {
            p->hover_ws = i;
            return;
        }
    }
    rect r[SHELL_MAX_WINS]; int idx[SHELL_MAX_WINS];
    int n = wb_panes(c, c->screen_w, c->screen_h, r, idx);
    for (int k = 0; k < n; k++)
        if (x >= r[k].x && x < r[k].x + r[k].w && y >= r[k].y && y < r[k].y + r[k].h) {
            c->hover = idx[k];
            return;
        }
}

/* Directional movement. A BSP has no rows or columns to index, so
 * "what is to my left" can only be answered geometrically: among panes
 * whose centre lies that way, prefer the ones that actually share an
 * edge span with this one, then the nearest. */
static int wb_neighbour(shell_ctx *c, int dx, int dy)
{
    rect r[SHELL_MAX_WINS]; int idx[SHELL_MAX_WINS];
    int n = wb_panes(c, c->screen_w, c->screen_h, r, idx);
    int me = wb_slot_of(n, idx, c->focus);
    if (me < 0) return -1;

    float mx = (float)r[me].x + (float)r[me].w * 0.5f;
    float my = (float)r[me].y + (float)r[me].h * 0.5f;
    int best = -1; float bestsc = 0.f;

    for (int k = 0; k < n; k++) {
        if (k == me) continue;
        float cx = (float)r[k].x + (float)r[k].w * 0.5f;
        float cy = (float)r[k].y + (float)r[k].h * 0.5f;
        float along = (cx - mx) * (float)dx + (cy - my) * (float)dy;
        if (along <= 1.f) continue;
        float perp = fabsf((cx - mx) * (float)dy - (cy - my) * (float)dx);
        int over = dx ? (r[k].y < r[me].y + r[me].h && r[me].y < r[k].y + r[k].h)
                      : (r[k].x < r[me].x + r[me].w && r[me].x < r[k].x + r[k].w);
        float sc = along + perp * 0.35f + (over ? 0.f : 4000.f);
        if (best < 0 || sc < bestsc) { best = idx[k]; bestsc = sc; }
    }
    return best;
}

/* Swapping two windows in the list swaps their slots in the tiling,
 * because the tiler walks the list in order. Focus travels with the
 * window, not with the slot — the user moved a thing, they did not ask
 * to start looking at a different one. */
static void wb_swap(shell_ctx *c, int a, int b)
{
    wb_priv *p = P(c);
    if (a < 0 || b < 0 || a == b || a >= c->n_wins || b >= c->n_wins) return;
    win_entry tw = c->wins[a]; c->wins[a] = c->wins[b]; c->wins[b] = tw;
    int ts = p->ws_of[a]; p->ws_of[a] = p->ws_of[b]; p->ws_of[b] = ts;
    if (c->focus == a) c->focus = b;
    else if (c->focus == b) c->focus = a;
}

/* Which workspace each window is on is stored here, indexed by slot.
 * When the compositor drops a slot it has to move with it, or a window
 * on workspace 3 silently arrives on workspace 0 because the window
 * below it in the list quit. */
static void l_removed(shell_ctx *c, int i)
{
    wb_priv *p = P(c);
    if (i < 0 || i >= c->n_wins) return;
    for (int j = i; j + 1 < c->n_wins; j++) p->ws_of[j] = p->ws_of[j + 1];
}

/* And a window that has just appeared belongs on the workspace the user
 * is looking at. ws_of[] is filled once at init, so a slot reused by a
 * new application inherited whatever workspace it last held -- you
 * clicked an icon, the window mapped, and nothing appeared, because it
 * had opened on a workspace you were not on. */
static void l_present(shell_ctx *c, int win)
{
    wb_priv *p = P(c);
    if (win < 0 || win >= c->n_wins) return;
    p->ws_of[win] = c->workspace;
    c->focus = win;
}

static void wb_close_focused(shell_ctx *c)
{
    wb_priv *p = P(c);
    int i = c->focus;
    if (i < 0 || i >= c->n_wins) return;
    /* The shipped shell asks the compositor to close and the list
     * arrives updated on the next frame; editing the model directly
     * keeps the archetype testable on its own. */
    for (int j = i; j + 1 < c->n_wins; j++) {
        c->wins[j] = c->wins[j + 1];
        p->ws_of[j] = p->ws_of[j + 1];
    }
    c->n_wins--;
    if (c->n_wins <= 0) { c->focus = -1; return; }
    c->focus = (i < c->n_wins) ? i : c->n_wins - 1;
    if (p->ws_of[c->focus] != c->workspace) wb_focus_first(c);
}

/* ── bindings ────────────────────────────────────────────────────────
 *
 * keyboard_optional="no". This is the only archetype where the keyboard
 * is the primary instrument, so it is the only one with a real table.
 * The compositor delivers these already filtered — everything below
 * arrives with the system modifier held — so a plain "q" typed into a
 * terminal never reaches this function.
 *
 *   key          evdev   does
 *   ───────────  ─────   ──────────────────────────────────────────────
 *   1 … 6        2 … 7   go to workspace N
 *   h j k l      35 36 37 38
 *                        move focus left / down / up / right
 *   ← ↓ ↑ →      105 108 103 106
 *                        swap the focused pane with its neighbour in
 *                        that direction; the panes exchange places
 *   Tab          15      focus the next pane in tiling order, wrapping
 *   Enter        28      swap the focused pane into slot 0 (top-left)
 *   q            16      close the focused window
 *   Esc          1       show or hide the key-hint strip
 */
static void l_key(shell_ctx *c, int k)
{
    wb_priv *p = P(c);
    if (!p) return;

    if (k >= 2 && k <= 7) {                       /* KEY_1 .. KEY_6 */
        int ws = k - 2;
        if (ws < wb_ws_count(c) && ws != c->workspace) {
            c->workspace = ws;
            wb_focus_first(c);
        }
        return;
    }

    int dx = 0, dy = 0, swap = 0;
    switch (k) {
    case 35: dx = -1; break;                      /* KEY_H  focus left  */
    case 36: dy =  1; break;                      /* KEY_J  focus down  */
    case 37: dy = -1; break;                      /* KEY_K  focus up    */
    case 38: dx =  1; break;                      /* KEY_L  focus right */
    case 105: dx = -1; swap = 1; break;           /* KEY_LEFT   swap    */
    case 108: dy =  1; swap = 1; break;           /* KEY_DOWN   swap    */
    case 103: dy = -1; swap = 1; break;           /* KEY_UP     swap    */
    case 106: dx =  1; swap = 1; break;           /* KEY_RIGHT  swap    */

    case 15: {                                    /* KEY_TAB  cycle     */
        rect r[SHELL_MAX_WINS]; int idx[SHELL_MAX_WINS];
        int n = wb_panes(c, c->screen_w, c->screen_h, r, idx);
        if (n > 0) {
            int slot = wb_slot_of(n, idx, c->focus);
            c->focus = idx[(slot < 0 ? 0 : (slot + 1) % n)];
        }
        return; }

    case 28: {                                    /* KEY_ENTER  promote */
        rect r[SHELL_MAX_WINS]; int idx[SHELL_MAX_WINS];
        int n = wb_panes(c, c->screen_w, c->screen_h, r, idx);
        if (n > 1 && c->focus != idx[0]) wb_swap(c, c->focus, idx[0]);
        return; }

    case 16: wb_close_focused(c); return;         /* KEY_Q  close       */

    case 1:                                       /* KEY_ESC  hints     */
        tween_to(&p->hint, p->hint.to > 0.5f ? 0.f : 1.f, 0.22f, EASE_OUT_CUBIC);
        return;

    default: return;
    }

    int other = wb_neighbour(c, dx, dy);
    if (other < 0) return;
    if (swap) wb_swap(c, c->focus, other);
    else      c->focus = other;
}

static int l_step(shell_ctx *c, float dt)
{
    wb_priv *p = P(c);
    if (!p) return 0;
    p->sample_age += dt;
    if (p->sample_age > 2.f) wb_sample(p);
    return tween_step(&p->hint, dt);
}

const shell_layout layout_workbench = {
    .id = "workbench",
    .init = l_init,
    .paint = l_paint,
    .click = l_click,
    .motion = l_motion,
    .key = l_key,
    .step = l_step,
    .fini = l_fini,
    .present = l_present,
    .removed = l_removed,
};
