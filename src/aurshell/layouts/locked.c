/* layouts/locked.c — the "Just these apps" archetype.
 *
 * There is no desktop here. Not a hidden one, not a locked one: the
 * machine does the handful of jobs an administrator chose, and nothing
 * else exists to be found. One allowed app fills the screen and stays;
 * several allowed apps get a permanent strip of large labelled buttons
 * along the bottom, and that strip is the complete list of places this
 * computer can go.
 *
 * The point: every other archetype answers "how do I get to something
 * else?" with a gesture. This one answers it with the truth — there is
 * nothing else. A school, a library or a clinic does not want a desktop
 * with the dangerous parts greyed out, because a greyed-out part is an
 * invitation to keep pressing it and a support call when someone does.
 * They want a machine where the question never forms.
 *
 * The hard part is tone, not mechanism. Restriction renders as poverty
 * by default: flat background, one small button, a border that says you
 * may not. A school laptop that looks like a prison teaches a child to
 * hate it, and a library terminal that looks like a cash machine gets
 * used once. So this layout spends exactly the same theme, spacing,
 * shadow and type budget as the archetypes with a hundred apps. Fewer
 * things, drawn as well as many things: the screen should read as
 * focus, not as a computer with its features taken away.
 */
#include "../shell.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* From shells/locked.shell. These three keys are this archetype's alone,
 * so they have no field in shell_ctx (the contract carries what every
 * archetype shares). The file's documented values are compiled in here,
 * behind names, so that wiring them to real ctx fields later is a rename
 * rather than a hunt through the renderer:
 *
 *   locked_autostart="first"     the first allowed app is already showing
 *   locked_show_switcher="auto"  the strip exists only when >1 app allowed
 *   locked_exit_combo="none"     NO key sequence leaves this shell
 */
#define LOCKED_AUTOSTART_FIRST 1
#define LOCKED_SWITCHER_AUTO   1
#define LOCKED_EXIT_COMBO_NONE 1

/* Seconds of no input before the machine tidies itself up. Three
 * minutes is long enough that a child reading a page is not thrown out
 * mid-paragraph, and short enough that the next person in the queue
 * does not inherit the last one's session. */
#define LOCKED_IDLE_S 180.0f

enum { SW_BAND = -1, SW_FINISH = -2 };   /* non-app items of the strip */

typedef struct {
    int   cur;          /* index into the ALLOWED list, not into c->apps */
    int   hover;        /* allowed index, SW_FINISH, or -1              */
    int   px, py;       /* last pointer position, to tell moves from jitter */
    float idle;         /* seconds since a human last did anything      */
    tween swap;         /* 0->1 as a newly chosen app takes the stage   */
    tween attract;      /* 0 = in use, 1 = the between-people screen    */
} locked_priv;

static locked_priv *P(shell_ctx *c) { return (locked_priv *)c->priv; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

/* ── the allowed set ──────────────────────────────────────────────────
 *
 * Policy decides what is on this machine; this layout only decides how
 * it is reached. But two of the policy flags have a rendering
 * consequence that a switch statement elsewhere cannot express: when an
 * administrator turns settings or a console off, the button does not
 * get dimmed, it ceases to exist. Hiding is not the polite version of
 * disabling — it is a different promise. The administrator is relying
 * on absence: a child who never sees a Settings tile never spends a
 * lesson trying to get into it, and never has to be told no. */
static int policy_allows(const shell_ctx *c, const app_entry *a)
{
    if (a->icon == ICON_SETTINGS && !c->allow_settings) return 0;
    if (a->icon == ICON_PLUS     && !c->allow_install)  return 0;  /* "get more apps" */
    if (a->icon == ICON_TERMINAL && c->kiosk)           return 0;  /* allow_tty="no"  */
    /* A file manager is the classic way out of a kiosk: an open dialog
     * is a file browser, and a file browser is the whole disk. */
    if (a->icon == ICON_FILES    && c->kiosk)           return 0;
    return 1;
}

/* The allowed apps, in the order the administrator listed them.
 *
 * A locked machine has no launcher, so an app is either already running
 * or it is unreachable: the windows the session manager holds open ARE
 * the allowed set, one per app. That is also what makes the switcher
 * honest — a button here never "opens" anything that was not already
 * running and permitted. c->apps is the fallback for the moment before
 * autostart has opened anything, and for any caller that hands us a
 * profile-filtered app list with no windows yet. */
/* Every application this machine is allowed to run, in the order the
 * app table declares them. Always all of them, running or not.
 *
 * This used to prefer "the apps that currently have windows", falling
 * back to the full list only when nothing was running -- which was
 * harmless while nothing could ever start, because the fallback was the
 * only branch that ever ran. The moment selecting an application
 * actually started it, the first branch became non-empty and the
 * switcher collapsed to the single app already open, with no way to
 * reach any other.
 *
 * A fixed list is also what a locked machine wants: the same button in
 * the same place every time, whether or not the thing behind it has
 * been opened yet. c->apps has already been filtered to the profile's
 * allowed_apps by shell_scan_apps(); policy_allows() applies the rules
 * that depend on what KIND of application it is. */
static int allowed_list(shell_ctx *c, int *out)
{
    int n = 0;
    for (int i = 0; i < c->n_apps && n < SHELL_MAX_APPS; i++)
        if (policy_allows(c, &c->apps[i])) out[n++] = i;
    return n;
}

static int win_of_app(shell_ctx *c, int app)
{
    for (int i = 0; i < c->n_wins; i++) if (c->wins[i].app == app) return i;
    return -1;
}

/* ── geometry ─────────────────────────────────────────────────────────
 *
 * ONE function owns the strip. Painting and hit-testing both ask it, so
 * a finger always lands on the thing under it; deriving the two
 * separately is how a kiosk ends up with a button that is live a
 * centimetre above where it is drawn, which nobody debugs because
 * nobody can reproduce it from the other side of a reception desk.
 *
 * item >= 0  the app button at that index in the allowed list
 * SW_BAND    the whole strip, for its panel
 * SW_FINISH  the end-of-session button at the right
 * A zero rect means "does not exist", which for a one-app machine is
 * every one of them: locked_show_switcher="auto". */
static rect switcher_rect(shell_ctx *c, int w, int h, int n, int item)
{
    rect z = { 0, 0, 0, 0 };
    if (n < 2 || !LOCKED_SWITCHER_AUTO) return z;

    int gap  = c->padding;
    int pad  = c->padding + 4;
    int mg   = c->margin + 4;
    /* pointer_first + target_size="large": these get pressed by six
     * year olds, by people wearing gloves and by someone holding a
     * library card in the other hand. */
    int minw = c->target_large ? 188 : 148;
    int finw = c->target_large ? 184 : 148;

    int band_w = w - 2*mg;
    int inner  = band_w - 2*pad;
    int area   = inner - finw - gap*2;
    if (area < minw) { area = inner; finw = 0; }   /* absurdly narrow screen */

    int cols = (area + gap) / (minw + gap);
    if (cols < 1) cols = 1;
    if (cols > n) cols = n;
    int rows = (n + cols - 1) / cols;
    /* Three rows of buttons is already a wall; past that, buttons get
     * narrower rather than the strip eating the app it is there to
     * serve. A machine with that many allowed apps is not a kiosk. */
    if (rows > 3) rows = 3;
    cols = (n + rows - 1) / rows;
    /* A handful of apps stays on one line even on a small panel. Three
     * buttons wrapped into two rows to protect a minimum width reads as
     * a bug to everyone who sees it, and 140px is still a target you
     * cannot miss. */
    if (rows > 1 && n <= 4 && area >= n*140 + (n - 1)*gap) { cols = n; rows = 1; }

    /* Buttons grow to fill the strip, but only so far. Two allowed apps
     * on a wide screen would otherwise give each a half-metre target
     * with a small label marooned in the middle of it, which reads as a
     * layout accident rather than as generosity. */
    int bw = (area - (cols - 1)*gap) / cols;
    int maxw = c->target_large ? 400 : 320;
    if (bw > maxw) bw = maxw;
    int bh = rows == 1 ? (c->target_large ? 104 : 84)
           : rows == 2 ? (c->target_large ?  88 : 72)
                       : (c->target_large ?  74 : 62);

    int band_h = 2*pad + rows*bh + (rows - 1)*gap;
    rect band = { mg, h - c->margin - band_h, band_w, band_h };
    if (item == SW_BAND) return band;
    if (item == SW_FINISH) {
        if (!finw) return z;
        rect f = { band.x + band.w - pad - finw, band.y + (band.h - bh)/2, finw, bh };
        return f;
    }
    if (item < 0 || item >= n) return z;

    int row = item / cols, col = item % cols;
    int in_row = n - row*cols; if (in_row > cols) in_row = cols;
    /* A short last row LINES UP with the row above it. Centring it was
     * the fifth axis of symmetry in this product and it is also the
     * wrong answer: a row of buttons that shifts sideways depending on
     * how many there are is a row whose positions you cannot learn,
     * which on a reception desk is the only thing that matters. */
    (void)in_row;
    rect b = { band.x + pad + col*(bw + gap),
               band.y + pad + row*(bh + gap), bw, bh };
    return b;
}

/* Room for a display-face brand, a rule under it and one line of
 * plain language. The old 48 was sized for one line of 14px sans; a
 * 27px serif with a rule under it needs the extra, and getting it
 * wrong clips the descenders of the one sentence on this screen that
 * explains what the machine is. */
static int header_h(const shell_ctx *c) { return c->margin + c->bar_h + 62; }

/* The area the chosen app occupies. With one app it is the whole
 * screen — window_mode="fullscreen" means exactly that, not "maximised
 * inside furniture". */
static rect stage_rect(shell_ctx *c, int w, int h, int n)
{
    if (n < 2) { rect full = { 0, 0, w, h }; return full; }
    rect band = switcher_rect(c, w, h, n, SW_BAND);
    int top = header_h(c), bot = band.y - c->margin;
    rect r = { c->margin + 4, top, w - 2*(c->margin + 4), bot - top };
    return r;
}

/* ── session ──────────────────────────────────────────────────────────
 *
 * "Finish" and the idle timeout are the same thing reached two ways.
 * This machine is left mid-session by strangers — that is the normal
 * case, not the edge one — so the end of a session cannot depend on
 * anybody remembering to end it. The timeout is what actually keeps the
 * promise; the button is for the person who is done and would like to
 * be sure, which is a real feeling and worth a target.
 *
 * Both land on the same welcome screen rather than dumping the next
 * person into a half-scrolled page. A dark or blank idle screen is
 * worse than useless here: an unattended machine showing nothing reads
 * as broken, gets reported as broken, and gets unplugged. */
static void select_app(shell_ctx *c, int k, int n, const int *app, int animate)
{
    locked_priv *p = P(c);
    if (n <= 0) return;
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    p->cur   = k;
    /* shell_launch(), not win_of_app(): on a kiosk the chosen
     * application is usually not running yet, and win_of_app() returned
     * -1 for it -- so pressing the only button on a locked machine put
     * the stage on nothing at all. This finds the window or starts the
     * program. */
    c->focus = shell_launch(c, app[k]);
    if (animate) { tween_set(&p->swap, 0.f); tween_to(&p->swap, 1.f, 0.24f, EASE_OUT_CUBIC); }
    else tween_set(&p->swap, 1.f);
}

static void end_session(shell_ctx *c)
{
    locked_priv *p = P(c);
    p->idle = 0.f; p->hover = -1;
    tween_to(&p->attract, 1.f, 0.45f, EASE_OUT_CUBIC);
}

static void begin_session(shell_ctx *c)
{
    locked_priv *p = P(c);
    int app[SHELL_MAX_APPS];
    int n = allowed_list(c, app);
    p->idle = 0.f;
    select_app(c, c->locked_autostart ? 0 : p->cur, n, app, 1);
    tween_to(&p->attract, 0.f, 0.30f, EASE_OUT_CUBIC);
}

static void l_init(shell_ctx *c)
{
    static locked_priv priv;
    memset(&priv, 0, sizeof priv);
    priv.hover = -1;
    priv.px = priv.py = -9999;             /* no pointer has been seen yet */
    tween_set(&priv.swap, 1.f);
    tween_set(&priv.attract, 0.f);
    c->priv = &priv;

    /* locked_autostart="first": there is no "no app showing" state to
     * boot into, because there is no home screen to show instead. */
    int app[SHELL_MAX_APPS];
    int n = allowed_list(c, app);
    select_app(c, 0, n, app, 0);
}

/* ── painting ─────────────────────────────────────────────────────── */

static void paint_wall(surface *s, const surface *wall, uint32_t bg)
{
    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else surface_fill(s, 0xFF000000u | bg);
}

/* The clock is the only status this archetype carries. status_strip is
 * "none" on purpose: a battery icon invites a question about power
 * settings, a network icon invites a question about wifi, and neither
 * has an answer on a machine whose settings do not exist. The time
 * does have an answer, and in a classroom it is the thing people
 * actually look up. */
static void paint_clock(shell_ctx *c, surface *s, shell_fonts *f, float rx, float top, float a)
{
    char hm[32], dt[48];
    shell_clock(hm, sizeof hm, dt, sizeof dt);
    /* The time in the display face — on a wall-mounted terminal this is
     * the most-read thing on the screen, so it gets the one size that
     * can be read from across a room. */
    float ty = top + (f->big ? font_ascent(f->big) : 20.f);
    shell_text(s, f->big, rx - shell_text_w(f->big, hm), ty, hm, c->fg_hi, a);
    float dy = ty + (f->big ? font_descent(f->big) : 6.f) + 10.f
                  + (f->small ? font_ascent(f->small) : 11.f);
    draw_hrule(s, (int)(rx - shell_text_w(f->big, hm)),
               (int)(ty + (f->big ? font_descent(f->big) : 6.f) + 4.f),
               (int)shell_text_w(f->big, hm), 1, c->overlay, a);
    shell_text(s, f->small, rx - shell_text_w(f->small, dt), dy, dt, c->subtle, a * 0.85f);
}

/* What the app itself looks like before its surface exists. Not an
 * error state and not a spinner in a void: the same icon, halo and
 * words the button uses, so the machine looks like it is doing the
 * thing you just pressed rather than thinking about it. */
static void paint_placeholder(shell_ctx *c, surface *s, shell_fonts *f, rect a,
                              int app, const char *line2, float alpha, float scale,
                              int on_wall)
{
    uint32_t ink   = c->apps[app].tint;
    uint32_t paper = on_wall ? c->bg : c->surface_c;
    float x   = (float)a.x + (float)c->padding * 2.2f;
    float isz = clampf((float)a.h * 0.16f, 18.f, 104.f) * scale;

    /* Set as a title page: the mark on the left measure, the name
     * under it in the display face, a rule, then the line of plain
     * language. Not a centred mark with two centred captions — that
     * block was reimplemented six times across the six archetypes and
     * is the single reason they all read as one generic product.
     *
     * No disc behind the mark either. The marks are solid silhouettes
     * now, so they have their own contrast; the three stacked tinted
     * circles that used to sit under one were a workaround for an
     * outline glyph that could not hold a surface on its own. */
    font *nf = (isz > 76.f && f->huge) ? f->huge : (isz > 34.f ? f->big : f->dmid);
    float a_n = nf ? font_ascent(nf) : 22.f, d_n = nf ? font_descent(nf) : 6.f;
    float a_h = f->mid ? font_ascent(f->mid) : 14.f;
    float d_h = f->mid ? font_descent(f->mid) : 5.f;

    /* Degrade in the order a printed page would: the rule and the
     * standfirst go first, then the mark, and the name is the last
     * thing standing. A locked machine can be a 1024x600 panel with
     * nine allowed apps, which leaves the stage about ninety pixels
     * tall — measured, not guessed, so it degrades at exactly the
     * height where it stops fitting rather than spilling out of the
     * frame and over the buttons below. */
    float mark  = isz + 34.f;
    float words = a_n + d_n;
    float tail  = 20.f + 18.f + a_h + d_h;
    int show_tail = 1, show_mark = 1;
    if (mark + words + tail > (float)a.h - 12.f) show_tail = 0;
    if (mark + words > (float)a.h - 12.f)        show_mark = 0;

    float block = (show_mark ? mark : 0.f) + words + (show_tail ? tail : 0.f);
    float top   = (float)a.y + ((float)a.h - block) * 0.42f;
    if (top < (float)a.y + 6.f) top = (float)a.y + 6.f;

    if (show_mark)
        shell_icon_draw(s, c->apps[app].icon, x + isz * 0.5f, top + isz * 0.5f,
                        isz, ink, paper, alpha);

    float ny = top + (show_mark ? mark : 0.f) + a_n;
    shell_text(s, nf, x, ny, c->apps[app].name, c->fg_hi, alpha * 0.98f);
    if (show_tail) {
        draw_hrule(s, (int)x, (int)(ny + d_n + 20.f),
                   (int)((float)a.w * 0.18f), 2, c->fg_hi, alpha * 0.85f);
        shell_text(s, f->mid, x, ny + d_n + 20.f + 18.f + a_h, line2,
                   c->subtle, alpha * 0.88f);
    }
}

/* The stage: one app, framed. The frame exists only when there is a
 * strip below it — something has to separate the app from its own
 * buttons — and it carries the app's tint on its edge so that the
 * pressed button and the thing that appeared are visibly the same
 * object. That is the whole answer to "where did it go?" here. */
static void paint_stage(shell_ctx *c, surface *s, shell_fonts *f, rect a, int app)
{
    locked_priv *p = P(c);
    /* Looked up from the app the strip says is selected, not from
     * c->focus: the two agree, but only one of them is the thing the
     * user just pressed, and a stage that disagrees with the lit button
     * is the single most confusing thing this screen could do. */
    int wi = win_of_app(c, app);
    const win_entry *w = (wi >= 0) ? &c->wins[wi] : NULL;
    uint32_t ink = c->apps[app].tint;
    float k = clampf(p->swap.value, 0.f, 1.f);
    float alpha = 0.55f + 0.45f * k;

    /* Arriving from below, from the strip it was pressed on. Small
     * because a fullscreen card sliding a long way reads as a system
     * doing something dramatic; this is just a tab change. */
    a.y += (int)((1.f - k) * 14.f);

    corners cr = corners_all((float)c->radius);
    draw_round_rect(s, a, cr, c->surface_c, 1.f);
    draw_frame(s, a, 1, c->overlay, 1.f);

    /* The title band gives way when the stage is short: on a small
     * panel with several allowed apps the strip legitimately takes
     * most of the screen, and a fixed 60px header would leave the app
     * itself a sliver. */
    int th = c->bar_h + 26;
    if (th > (int)((float)a.h * 0.34f)) th = (int)((float)a.h * 0.34f);
    if (th < 26) th = 26;
    rect tb = { a.x, a.y, a.w, th };
    draw_rect(s, tb, c->bg_alt, alpha);
    draw_hrule(s, a.x, a.y + th, a.w, 1, c->overlay, alpha);
    /* The rule along the top edge is what ties the button you pressed
     * to the thing that appeared: the same signal colour, in the same
     * two places, and nowhere else on the screen. Drawn AFTER the
     * title band, which starts at the same y and would cover it. */
    draw_hrule(s, a.x, a.y, a.w, 3, c->accent, alpha);

    float ix  = (float)(a.x + c->padding * 2);
    float tsz = clampf((float)th * 0.40f, 13.f, 22.f);
    shell_icon_draw(s, c->apps[app].icon, ix + tsz * 0.5f, (float)(a.y + th/2), tsz,
                    ink, c->bg_alt, alpha * 0.95f);
    shell_text(s, f->dmid, ix + tsz + 12.f, shell_baseline(f->dmid, (float)a.y, (float)th),
               c->apps[app].name, c->fg_hi, alpha * 0.97f);

    /* Right of the title: what this window is actually showing. It is
     * the one piece of state a returning user needs to recognise the
     * session they left, and it is read-only — there is no address bar
     * here, because an address bar is a launcher. */
    const char *sub = (w && w->subtitle[0]) ? w->subtitle : c->apps[app].hint;
    float sw = shell_text_w(f->small, sub);
    float sx = (float)(a.x + a.w - c->padding*2) - sw;
    if (sx > ix + tsz + 12.f + shell_text_w(f->dmid, c->apps[app].name) + 40.f)
        shell_text(s, f->small, sx, shell_baseline(f->small, (float)a.y, (float)th),
                   sub, c->subtle, alpha * 0.85f);

    rect body = { a.x, a.y + th, a.w, a.h - th };
    if (w && w->content) {
        corners bc = { 0, 0, (float)c->radius, (float)c->radius };
        draw_content_fit(s, w->content, body, bc, alpha);
    } else {
        paint_placeholder(c, s, f, body, app, c->apps[app].hint, alpha * 0.95f, 0.8f, 0);
    }
}

static void paint_button(shell_ctx *c, surface *s, shell_fonts *f, rect b,
                         int app, int selected, int hot)
{
    uint32_t ink   = c->apps[app].tint;
    uint32_t paper = selected ? c->surface_c : (hot ? c->surface_hi : c->bg_alt);
    corners cr = corners_all((float)c->radius);

    /* Selection is a fill plus a bar down the left edge, and that bar
     * is the only red on the strip. It used to be a tint wash, a
     * coloured border AND a tab mark — three cues for one bit — which
     * on a light theme came out as three pastel suggestions and no
     * answer. */
    draw_round_rect(s, b, cr, paper, 1.f);
    if (selected) draw_rect(s, (rect){ b.x, b.y, 5, b.h }, c->accent, 1.f);
    draw_frame(s, b, selected ? 2 : 1, selected ? c->fg : c->overlay,
               hot && !selected ? 1.f : 0.9f);

    const char *name = c->apps[app].name;
    const char *hint = c->apps[app].hint;
    float pad = (float)c->padding + 6.f;
    float avail = (float)b.w - pad * 2.f;
    float isz = clampf((float)b.h * 0.38f, 22.f, 42.f);

    /* Pick the largest type that fits rather than shrinking every label
     * to the width of the longest one: a three-app machine should have
     * labels readable from across a classroom. */
    font *lf = (b.w >= 250 && b.h >= 84) ? f->big : f->dmid;
    if (shell_text_w(lf, name) > avail - isz - 18.f) lf = f->dmid;
    if (shell_text_w(lf, name) > avail - isz - 18.f) lf = f->mid;

    /* Hints only on the tall single-row strip. Deciding it per button
     * from whether that button's own hint happened to fit gave a row
     * where two labels had a second line and three did not, which
     * reads as three of them having failed to load. */
    int show_hint = (b.h >= 100 &&
                     shell_text_w(f->small, hint) <= avail - isz - 18.f);

    /* Mark left, words left. These buttons are wide and short, so a
     * stacked mark over a centred label leaves a hole under each one —
     * and left-aligning them gives the row a common left measure the
     * eye can run down, which a row of centred labels never has. */
    float icx = (float)b.x + pad + isz * 0.5f;
    float icy = (float)(b.y + b.h/2);
    shell_icon_draw(s, c->apps[app].icon, icx, icy, isz, ink, paper,
                    selected ? 1.f : 0.9f);

    float tx = (float)b.x + pad + isz + 18.f;
    uint32_t nc = selected ? c->fg_hi : c->fg;
    if (show_hint) {
        float la = lf ? font_ascent(lf) : 20.f, ld = lf ? font_descent(lf) : 6.f;
        float ha = f->small ? font_ascent(f->small) : 11.f;
        float hd = f->small ? font_descent(f->small) : 4.f;
        float top = icy - (la + ld + 6.f + ha + hd) * 0.5f;
        shell_text(s, lf, tx, top + la, name, nc, 1.f);
        shell_text(s, f->small, tx, top + la + ld + 6.f + ha, hint, c->subtle, 0.9f);
    } else {
        shell_text(s, lf, tx, shell_baseline(lf, (float)b.y, (float)b.h), name, nc, 1.f);
    }
}

/* "Finish" is deliberately the quietest target on the strip: outlined,
 * uncoloured, no icon. It must be findable by someone who wants it and
 * uninteresting to a child looking for something to press. It is also
 * the only button here that does not open an app, so it is separated by
 * a rule rather than sitting in the row as if it were a fourth app. */
static void paint_finish(shell_ctx *c, surface *s, shell_fonts *f, rect b, int hot)
{
    if (!b.w) return;
    draw_vrule(s, b.x - (int)((float)c->padding * 0.8f), b.y + 8, b.h - 16, 1,
               c->overlay, 0.9f);

    corners cr = corners_all((float)c->radius);
    if (hot) draw_round_rect(s, b, cr, c->surface_hi, 1.f);
    draw_frame(s, b, hot ? 2 : 1, hot ? c->fg : c->overlay, 1.f);

    float x = (float)b.x + (float)c->padding;
    float la = f->dmid ? font_ascent(f->dmid) : 16.f;
    float ld = f->dmid ? font_descent(f->dmid) : 5.f;
    float ha = f->small ? font_ascent(f->small) : 11.f;
    float hd = f->small ? font_descent(f->small) : 4.f;
    float top = (float)b.y + ((float)b.h - (la + ld + 6.f + ha + hd)) * 0.5f;
    shell_text(s, f->dmid, x, top + la, "Finish", hot ? c->fg_hi : c->fg, 0.95f);
    shell_text(s, f->small, x, top + la + ld + 6.f + ha, "Start over", c->subtle, 0.85f);
}

/* The strip. show_positions is "no" in the .shell and would be noise
 * here anyway: the lit button IS the position indicator, and a row of
 * dots under a row of buttons says the same thing twice. */
static void paint_switcher(shell_ctx *c, surface *s, shell_fonts *f, const int *app, int n)
{
    locked_priv *p = P(c);
    rect band = switcher_rect(c, s->w, s->h, n, SW_BAND);
    if (!band.w) return;

    draw_rect(s, band, c->bg_alt, 1.f);
    draw_hrule(s, band.x, band.y, band.w, 2, c->overlay, 1.f);
    draw_frame(s, band, 1, c->overlay, 0.85f);

    for (int i = 0; i < n; i++)
        paint_button(c, s, f, switcher_rect(c, s->w, s->h, n, i), app[i],
                     i == p->cur, i == p->hover);
    paint_finish(c, s, f, switcher_rect(c, s->w, s->h, n, SW_FINISH), p->hover == SW_FINISH);
}

/* The between-people screen. It says three things and no more: whose
 * machine this is, that it is ready, and what it can do. The last one
 * matters most — a stranger walking up to a locked terminal cannot
 * discover its capabilities any other way, because there is no menu to
 * open and nothing to browse. This is the archetype's only "home", and
 * it is not reachable while someone is working: it arrives when the
 * session ends, never as a place to go back to. */
static void paint_attract(shell_ctx *c, surface *s, shell_fonts *f,
                          const int *app, int n, float k)
{
    const char *TITLE = "Ready when you are";
    const char *SUB   = "Touch the screen or move the mouse to start.";
    const char *FOOT  = "Nothing from the last session was kept.";

    /* A flat veil. The version this replaces blurred the whole
     * 1366x768 framebuffer at radius 28 and then laid 86% of the
     * background colour over it — the most expensive single operation
     * in the product, performed to produce something a flat fill
     * produces exactly as well. */
    rect full = { 0, 0, s->w, s->h };
    draw_rect(s, full, c->bg, k);

    font *tf = f->huge ? f->huge : f->big;
    float pad = (float)c->padding * 2.f;
    float bh  = f->label ? font_line_height(f->label) : 15.f;
    float th  = tf ? font_line_height(tf) : 46.f;
    float sh  = f->mid ? font_line_height(f->mid) : 24.f;

    /* Off the axis and off the centre. A panel centred in both
     * directions on an otherwise empty screen is the shape of a modal
     * dialog, which is the one thing a welcome screen must not look
     * like. This sits on the left measure with the page falling away
     * to the right, the way a title page is set. */
    int pw = (int)((float)s->w * 0.62f);
    int want = (int)(shell_text_w(tf, TITLE) + pad * 2.f);
    if (pw < want) pw = want;
    if (pw > s->w - c->margin * 3) pw = s->w - c->margin * 3;

    /* The roster WRAPS rather than squeezing. Dividing the panel width
     * by the number of apps is what put nine 40px marks into 90px
     * cells and printed them through each other's names — and a kiosk
     * is exactly the machine most likely to be allowed nine apps on a
     * small panel. Measure the widest entry, fit as many as go on a
     * line, and take a second line. */
    float isz = 38.f;
    int cell = 120;
    for (int i = 0; i < n; i++) {
        float nw = shell_text_w(f->small, c->apps[app[i]].name) + isz + 30.f;
        if (nw > (float)cell) cell = (int)nw;
    }
    int inner = pw - (int)pad * 2;
    int cols  = inner / (cell > 1 ? cell : 1);
    if (cols < 1) cols = 1;
    if (cols > n) cols = n;
    int rows  = (n + cols - 1) / cols;
    float rowh = isz + 18.f;

    int ph = (int)(pad * 2.f + bh + 30.f + th + 10.f + sh + pad + 22.f
                   + (float)rows * rowh + pad + bh);
    if (ph > s->h - c->margin * 2) ph = s->h - c->margin * 2;

    rect pnl = { c->margin * 2, (int)((float)s->h * 0.24f) + (int)((1.f - k) * 16.f),
                 pw, ph };
    if (pnl.y + pnl.h > s->h - c->margin) pnl.y = s->h - c->margin - pnl.h;
    draw_round_rect(s, pnl, corners_all((float)c->radius), c->surface_c, k);
    draw_frame(s, pnl, 1, c->overlay, k);
    draw_hrule(s, pnl.x, pnl.y, pnl.w, 3, c->accent, k);

    float x = (float)pnl.x + pad;
    float y = (float)pnl.y + pad;

    /* Whose machine this is. On a public terminal this line is the
     * only answer to "who do I complain to?", so it is set as a
     * running head: a block of ink, the name tracked out, a rule. */
    draw_rect(s, (rect){ (int)x, (int)(y + bh * 0.28f), 8, 8 }, c->accent, k);
    shell_text_tracked(s, f->label, x + 20.f,
                       y + (f->label ? font_ascent(f->label) : 11.f),
                       c->brand, c->subtle, k * 0.95f, 1.6f);
    y += bh + 30.f;

    shell_text(s, tf, x, y + (tf ? font_ascent(tf) : 30.f), TITLE, c->fg_hi, k * 0.98f);
    y += th + 10.f;
    shell_text(s, f->mid, x, y + (f->mid ? font_ascent(f->mid) : 14.f),
               SUB, c->subtle, k * 0.9f);
    y += sh + pad;

    draw_hrule(s, (int)x, (int)y, (int)((float)pnl.w * 0.24f), 2, c->fg_hi, k * 0.85f);
    y += 22.f;

    /* What is on this machine, stated rather than offered: none of
     * these marks is a target, because the first touch anywhere starts
     * the session and nobody walking up should have to aim. */
    for (int i = 0; i < n; i++) {
        float ix = x + (float)((i % cols) * cell);
        float iy = y + (float)(i / cols) * rowh + isz * 0.5f;
        shell_icon_draw(s, c->apps[app[i]].icon, ix + isz * 0.5f, iy,
                        isz, c->apps[app[i]].tint, c->surface_c, k);
        shell_text(s, f->small, ix + isz + 12.f,
                   iy + (f->small ? font_ascent(f->small) : 11.f) * 0.5f,
                   c->apps[app[i]].name, c->fg, k * 0.95f);
    }
    y += (float)rows * rowh + 10.f;

    shell_text(s, f->small, x, y + (f->small ? font_ascent(f->small) : 11.f),
               FOOT, c->muted, k * 0.85f);

    /* The clock survives the dim. A wall clock that goes blank when
     * nobody is at the desk is a clock nobody trusts. */
    if (c->show_clock)
        paint_clock(c, s, f, (float)(s->w - c->margin), (float)c->margin, k * 0.9f);
}

static void l_paint(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall)
{
    locked_priv *p = P(c);
    int app[SHELL_MAX_APPS];
    int n = allowed_list(c, app);
    c->screen_w = s->w; c->screen_h = s->h;
    if (p->cur >= n) p->cur = n ? n - 1 : 0;

    if (n <= 0) {                     /* an empty allowed set is a broken policy */
        paint_wall(s, wall, c->bg);
        shell_text_centred(s, f->big, (float)s->w/2.f, (float)s->h/2.f,
                           "This computer has no apps set up yet.", c->subtle, 0.85f);
        return;
    }

    int cur_app = app[p->cur];

    /* locked_show_switcher, from the .shell file: "auto" shows the strip
     * only when more than one app is allowed, "always" shows it even for
     * one, and "never" suppresses it entirely — which is how an
     * administrator pins a machine to a single app without having to
     * uninstall the others. */
    int show_strip = (c->locked_show_switcher == 1) ? 1
                   : (c->locked_show_switcher == 2) ? 0
                   : (n > 1);

    if (!show_strip) {
        /* One allowed app: it IS the machine. No strip, no header, no
         * brand, nothing to press — the only pixels that are not the
         * app are the clock, and only because the .shell asked for it.
         * A single permanent button that switches to the app you are
         * already in would be furniture pretending to be a choice. */
        int wi = win_of_app(c, cur_app);
        const win_entry *w = (wi >= 0) ? &c->wins[wi] : NULL;
        rect full = { 0, 0, s->w, s->h };
        if (w && w->content) draw_content_fit(s, w->content, full, corners_all(0.f), 1.f);
        else {
            paint_wall(s, wall, c->bg);
            paint_placeholder(c, s, f, full, cur_app,
                              (w && w->subtitle[0]) ? w->subtitle : c->apps[cur_app].hint,
                              1.f, 1.f, 1);
        }
        if (c->show_clock) {
            char hm[32], dt[48];
            shell_clock(hm, sizeof hm, dt, sizeof dt);
            float tw = shell_text_w(f->big, hm), dw = shell_text_w(f->small, dt);
            float bw = (tw > dw ? tw : dw) + (float)c->padding * 2.f;
            rect pill = { s->w - c->margin - (int)bw - 6, c->margin,
                          (int)bw + 6, c->bar_h + 30 };
            /* A clock over a fullscreen app needs its own ground, or it
             * lands on whatever the app happens to be showing. */
            draw_round_rect(s, pill, corners_all((float)c->radius), c->bg_alt, 0.92f);
            draw_frame(s, pill, 1, c->overlay, 0.8f);
            paint_clock(c, s, f, (float)(pill.x + pill.w) - (float)c->padding,
                        (float)pill.y + 10.f, 0.95f);
        }
    } else {
        paint_wall(s, wall, c->bg);
        paint_stage(c, s, f, stage_rect(c, s->w, s->h, n), cur_app);
        paint_switcher(c, s, f, app, n);

        /* Header last: it sits on bare wallpaper, and the stage below
         * casts a shadow that reaches up past its own top edge. Text
         * painted before that shadow gets smeared by it — a bug that
         * looks like a font problem and is really a paint order one.
         *
         * Drawn straight onto the wallpaper with no bar behind it: a
         * filled strip across the top reads as a taskbar, and a
         * taskbar is a promise of somewhere else to go. */
        float by = (float)c->margin + 10.f;
        /* The brand is a profile's own words — "Lincoln High Chromebook
         * Replacement" is a real one — so it is measured, not assumed.
         * A school name that runs under the clock is the first thing
         * the person who signed off on this deployment would notice. */
        font *bf = f->big;
        float bavail = (float)(s->w - 2*(c->margin + 10)) - (c->show_clock ? 190.f : 0.f);
        if (shell_text_w(bf, c->brand) > bavail) bf = f->dmid;
        float bx = (float)(c->margin + 10);
        shell_text(s, bf, bx, by + (bf ? font_ascent(bf) : 22.f),
                   c->brand, c->fg_hi, 0.98f);
        float ry = by + (bf ? font_ascent(bf) + font_descent(bf) : 30.f) + 8.f;
        draw_hrule(s, (int)bx, (int)ry,
                   (int)(shell_text_w(bf, c->brand) * 0.55f), 2, c->fg_hi, 0.85f);
        shell_text(s, f->small, bx,
                   ry + 12.f + (f->small ? font_ascent(f->small) : 11.f),
                   "The apps below are everything this computer does.",
                   c->subtle, 0.88f);
        if (c->show_clock)
            paint_clock(c, s, f, (float)(s->w - c->margin - 10), by, 0.95f);
    }

    if (p->attract.value > 0.003f)
        paint_attract(c, s, f, app, n, clampf(p->attract.value, 0.f, 1.f));
}

/* ── input ──────────────────────────────────────────────────────────── */

static int l_click(shell_ctx *c, int x, int y)
{
    locked_priv *p = P(c);
    int app[SHELL_MAX_APPS];
    int n = allowed_list(c, app);
    p->idle = 0.f;

    /* The touch that wakes the machine only wakes it. Letting it fall
     * through would mean the first thing a new person does is press a
     * button they never saw. */
    if (p->attract.to > 0.5f) { begin_session(c); return 1; }

    /* Hit-test against the frame the user actually looked at. The size
     * is whatever was last painted, so a resolution change between
     * paint and click can never leave live buttons behind. */
    int w = c->screen_w, h = c->screen_h;
    for (int i = 0; i < n; i++) {
        rect b = switcher_rect(c, w, h, n, i);
        if (b.w && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
            if (i != p->cur) select_app(c, i, n, app, 1);
            return 1;
        }
    }
    rect fb = switcher_rect(c, w, h, n, SW_FINISH);
    if (fb.w && x >= fb.x && x < fb.x + fb.w && y >= fb.y && y < fb.y + fb.h) {
        end_session(c);
        return 1;
    }

    /* Everything else belongs to the app. Crucially there is no
     * "clicked the background" case: no desktop to reveal, no menu to
     * summon, no long-press. The absence is the feature. */
    return 0;
}

static void l_motion(shell_ctx *c, int x, int y)
{
    locked_priv *p = P(c);
    int app[SHELL_MAX_APPS];
    int n = allowed_list(c, app);

    /* Only a real move counts as a person being here. An optical mouse
     * on a desk someone leans on reports jitter all night, and a shell
     * that reads jitter as presence never ends the session — the one
     * failure this archetype cannot have. The same threshold stops that
     * jitter from waking the welcome screen at 3am. */
    int dx = x - p->px, dy = y - p->py;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int moved = (dx > 3 || dy > 3);
    p->px = x; p->py = y;
    if (!moved) return;
    p->idle  = 0.f;
    p->hover = -1;

    /* The welcome screen says moving the mouse starts a session, so it
     * does. A kiosk with a mouse and no touchscreen otherwise strands
     * anyone who reads the instruction literally. */
    if (p->attract.to > 0.5f) { begin_session(c); return; }

    for (int i = 0; i < n; i++) {
        rect b = switcher_rect(c, c->screen_w, c->screen_h, n, i);
        if (b.w && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) { p->hover = i; break; }
    }
    if (p->hover < 0) {
        rect fb = switcher_rect(c, c->screen_w, c->screen_h, n, SW_FINISH);
        if (fb.w && x >= fb.x && x < fb.x + fb.w && y >= fb.y && y < fb.y + fb.h)
            p->hover = SW_FINISH;
    }
    c->hover = p->hover;
}

/* keyboard_optional="yes": the keys below are a courtesy for a machine
 * that happens to have a keyboard, never the only way to do anything.
 *
 * locked_exit_combo="none" is enforced by what is NOT here. There is no
 * escape sequence, no magic chord, no hidden admin key — an
 * administrator who needs the machine back uses the enrolment console,
 * not a secret a student will eventually find written on a desk. Every
 * unhandled key deliberately falls through to the app. */
static void l_key(shell_ctx *c, int k)
{
    locked_priv *p = P(c);
    int app[SHELL_MAX_APPS];
    int n = allowed_list(c, app);
    p->idle = 0.f;

    if (p->attract.to > 0.5f) { begin_session(c); return; }
    if (n < 2) return;                     /* nothing to move between */

    switch (k) {
        case 105: select_app(c, p->cur - 1, n, app, 1); break;            /* KEY_LEFT  */
        case 106: select_app(c, p->cur + 1, n, app, 1); break;            /* KEY_RIGHT */
        case  15: select_app(c, (p->cur + 1) % n, n, app, 1); break;      /* KEY_TAB   */
        default: break;
    }
}

/* Called every frame with real elapsed time. The idle countdown lives
 * here, which means a compositor that sleeps until the next input will
 * never reach it: a locked machine must be woken at least once a second
 * for the session to time out. That is the one thing this archetype
 * asks of the frame loop, and it is cheaper than the alternative of
 * keeping the screen animating forever. */
static int l_step(shell_ctx *c, float dt)
{
    locked_priv *p = P(c);
    int busy = 0;
    p->idle += dt;
    if (p->idle >= LOCKED_IDLE_S && p->attract.to < 0.5f) end_session(c);
    busy |= tween_step(&p->swap, dt);
    busy |= tween_step(&p->attract, dt);
    return busy;
}

static void l_fini(shell_ctx *c) { c->priv = NULL; }

/* On a machine that shows one application at a time, "a window
 * appeared" and "put it on the stage" are the same event. Without this,
 * a kiosk started the browser and went on showing the previous app --
 * which to the person standing at it is a machine that ignored them. */
static void l_present(shell_ctx *c, int win)
{
    if (win < 0 || win >= c->n_wins) return;
    int app[SHELL_MAX_APPS];
    int n = allowed_list(c, app);
    for (int k = 0; k < n; k++)
        if (app[k] == c->wins[win].app) { select_app(c, k, n, app, 1); return; }
    c->focus = win;
}

const shell_layout layout_locked = {
    .id = "locked",
    .init = l_init,
    .paint = l_paint,
    .click = l_click,
    .motion = l_motion,
    .key = l_key,
    .step = l_step,
    .fini = l_fini,
    .present = l_present,
};
