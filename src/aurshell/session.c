/* session.c — the join between the compositor and the shell.
 *
 * aurwl knows about clients, buffers and protocol objects. The shell
 * knows about archetypes, icons and what the user meant by a click.
 * Neither should learn the other's vocabulary, so this file is the only
 * place that speaks both, and it is deliberately small: a window list
 * to reconcile, a pointer and a keyboard to route, and popups to paint
 * on top.
 *
 * The one genuinely interesting decision here is how a click finds its
 * way into a window. The shell does not ask the archetype where it drew
 * the window -- it reads the record the scaled blit left behind while
 * painting it (anim.c's tracker). Painting and hit-testing therefore
 * cannot disagree, because the second one reads the first one's
 * output rather than a second opinion about it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <string.h>

#include "session.h"
#include "../aurwl/aurwl.h"

/* An application that has not shown a window in this long has failed to
 * start in a way it is not going to recover from. The slot says so
 * rather than spinning forever: a user who clicked something that is
 * never coming needs to be told, not reassured. */
#define START_TIMEOUT_MS 25000

static uint32_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ── matching a window back to the icon that started it ─────────── */

/* Toolkits report an app_id that is usually, but not always, the stem
 * of the desktop file. GTK reverses the domain ("org.gnome.Epiphany"),
 * Qt often uses the binary name, and some report nothing at all. So the
 * match is tried from most to least specific and gives up rather than
 * guessing -- an unmatched window still appears, it just does not claim
 * to be an icon it may not be. */
static int last_segment(const char *s)
{
    const char *dot = strrchr(s, '.');
    return dot ? (int)(dot - s) + 1 : 0;
}

static int app_for_window(const shell_ctx *c, const char *app_id)
{
    if (!app_id || !*app_id) return -1;
    for (int i = 0; i < c->n_apps; i++)
        if (c->apps[i].wm_class[0] && !strcasecmp(c->apps[i].wm_class, app_id)) return i;
    for (int i = 0; i < c->n_apps; i++)
        if (!strcasecmp(c->apps[i].id, app_id)) return i;

    const char *tail = app_id + last_segment(app_id);
    for (int i = 0; i < c->n_apps; i++) {
        if (!strcasecmp(c->apps[i].id, tail)) return i;
        if (c->apps[i].wm_class[0]) {
            const char *wt = c->apps[i].wm_class + last_segment(c->apps[i].wm_class);
            if (!strcasecmp(wt, tail)) return i;
        }
    }
    return -1;
}

static int slot_for_wid(shell_ctx *c, uint32_t wid)
{
    for (int i = 0; i < c->n_wins; i++) if (c->wins[i].wid == wid) return i;
    return -1;
}

/* ── the reconcile ──────────────────────────────────────────────── */

void session_sync(shell_ctx *c, void (*present)(shell_ctx *, int),
                  void (*removed)(shell_ctx *, int))
{
    if (!c->wl) return;
    aurwl *wl = c->wl;

    int seen[SHELL_MAX_WINS];
    memset(seen, 0, sizeof seen);

    int n = aurwl_window_count(wl);
    for (int k = 0; k < n; k++) {
        aurwl_win *w = aurwl_window_at(wl, k);
        if (!w || aurwl_win_is_popup(w)) continue;      /* popups paint above */
        uint32_t id = aurwl_win_id(w);
        const char *app_id = aurwl_win_app_id(w);

        int i = slot_for_wid(c, id);
        if (i < 0) {
            /* Adopt the placeholder the user's click created, so the
             * window appears where they were already looking instead of
             * arriving somewhere else while the placeholder lingers. */
            int app = app_for_window(c, app_id);
            /* Only a placeholder for THIS application. Adopting any
             * pending one when the window reported no app_id -- which
             * the comment above says is a normal thing for a toolkit to
             * do -- meant that starting two applications in a row gave
             * the second one's window the first one's name and icon.
             * And it stuck: the first application's icon then raised
             * the wrong window forever, and never launched again. */
            if (app >= 0)
                for (int j = 0; j < c->n_wins && i < 0; j++)
                    if (c->wins[j].starting && !c->wins[j].wid && c->wins[j].app == app) i = j;
            if (i < 0 && c->n_wins < SHELL_MAX_WINS) {
                i = c->n_wins++;
                memset(&c->wins[i], 0, sizeof c->wins[i]);
                c->wins[i].app = app;
            }
            if (i < 0) continue;                         /* list is full */
            c->wins[i].wid = id;
            c->wins[i].starting = 0;
            if (c->wins[i].app < 0) c->wins[i].app = app;
            /* A window that has just appeared is the one the user is
             * waiting for, so the archetype is asked to bring it into
             * view rather than told which index is focused: a carousel
             * has to scroll, a stack has to raise, and only the
             * archetype knows which it is. */
            if (present) present(c, i); else c->focus = i;
        }
        seen[i] = 1;

        win_entry *e = &c->wins[i];
        const char *title = aurwl_win_title(w);
        if (title && *title) {
            size_t k2 = 0;
            for (; k2 + 1 < sizeof e->title && title[k2]; k2++) e->title[k2] = title[k2];
            e->title[k2] = 0;
        }
        if (e->app >= 0 && e->app < c->n_apps && !e->subtitle[0]) {
            /* Both sides live inside the same shell_ctx, so snprintf
             * cannot prove they do not overlap and warns. They never do
             * -- they are different members -- but a bounded copy says
             * that rather than asserting it in a comment. */
            const char *h = c->apps[e->app].hint;
            size_t m = 0;
            for (; m + 1 < sizeof e->subtitle && h[m]; m++) e->subtitle[m] = h[m];
            e->subtitle[m] = 0;
        }
        e->content = aurwl_win_content(w);

        /* Ask the client to draw at exactly the size the archetype set
         * aside for it, so its text is rendered rather than resampled.
         * The size comes from where the archetype actually put it last
         * frame -- no layout has to describe itself, and an archetype
         * that changes its mind about window size is followed without
         * being asked. On the very first frame there is no record and
         * the client keeps its own size, which is the right answer for
         * one frame and self-corrects on the next.
         *
         * A dialog that refuses to resize simply keeps refusing; the
         * configure is idempotent, and draw_fit_rect() centres it at
         * its natural size rather than stretching it. */
        rect slot;
        if (e->content && draw_track_slot(e->content, &slot) && slot.w > 0 && slot.h > 0)
            aurwl_win_configure(w, slot.w, slot.h, i == c->focus, 0, 0);
        else if (e->geom.w > 0 && e->geom.h > 0)
            aurwl_win_configure(w, e->geom.w, e->geom.h, i == c->focus, 0, 0);
        else
            aurwl_win_configure(w, 0, 0, i == c->focus, 0, 0);
    }

    uint32_t t = now_ms();
    for (int i = c->n_wins - 1; i >= 0; i--) {
        win_entry *e = &c->wins[i];
        if (e->wid && !seen[i]) {
            /* The client went away.
             *
             * Archetypes keep their own arrays indexed by the same slot
             * number -- which workspace a window is on, whether it is
             * maximised, the geometry to restore it to, its place in
             * the stack. Shifting c->wins[] without telling them left
             * every one of those naming a different window: quit a
             * window and the one after it changes workspace, or claims
             * to be maximised with a rectangle saved from somebody
             * else. Their own close buttons always did this
             * bookkeeping; the path a real application exits through
             * did not, and that is the path that actually happens. */
            if (removed) removed(c, i);
            for (int j = i; j + 1 < c->n_wins; j++) c->wins[j] = c->wins[j + 1];
            c->n_wins--;
            /* A decrement, not a clamp. When the slot that went was
             * below the focused one, everything above it moved down and
             * the focus index silently named its neighbour -- so the
             * window drawn as active and the window receiving keys were
             * two different windows. */
            if (c->focus > i) c->focus--;
            else if (c->focus == i) c->focus = c->n_wins - 1;
            if (c->focus >= c->n_wins) c->focus = c->n_wins - 1;
            continue;
        }
        if (e->starting) {
            if (!e->start_ms) e->start_ms = t;
            else if (t - e->start_ms > START_TIMEOUT_MS) {
                e->starting = 0;
                snprintf(e->subtitle, sizeof e->subtitle, "This did not start");
            }
        }
    }
}

/* ── input ──────────────────────────────────────────────────────── */

/* Which window is under the pointer, using the rectangles painting
 * recorded. Walked back to front so the topmost window wins, which is
 * the same order the user sees. */
static aurwl_win *window_under(shell_ctx *c, int x, int y, int *sx, int *sy)
{
    if (!c->wl) return NULL;
    for (int i = draw_track_count() - 1; i >= 0; i--) {
        const surface *src; rect r;
        if (!draw_track_at(i, &src, &r)) continue;
        if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) continue;
        if (r.w <= 0 || r.h <= 0) continue;

        int n = aurwl_window_count(c->wl);
        for (int k = 0; k < n; k++) {
            aurwl_win *w = aurwl_window_at(c->wl, k);
            if (aurwl_win_content(w) != src) continue;
            surface *s = aurwl_win_content(w);
            /* The view is scaled to fit, so the surface-local point is
             * the screen point mapped back through that same scale --
             * anything else puts the client's idea of the cursor
             * somewhere other than where the user sees it. */
            if (sx) *sx = (int)((long)(x - r.x) * s->w / r.w);
            if (sy) *sy = (int)((long)(y - r.y) * s->h / r.h);
            return w;
        }
    }
    return NULL;
}

int session_motion(shell_ctx *c, int x, int y)
{
    if (!c->wl) return 0;
    int sx = 0, sy = 0;
    aurwl_win *w = window_under(c, x, y, &sx, &sy);
    aurwl_pointer_motion(c->wl, w, sx, sy, now_ms());
    return w != NULL;
}

int session_button(shell_ctx *c, int x, int y, uint32_t button, int pressed)
{
    if (!c->wl) return 0;
    int sx = 0, sy = 0;
    aurwl_win *w = window_under(c, x, y, &sx, &sy);
    if (!w) return 0;
    /* Clicking a window's content focuses it, the way every desktop
     * does; otherwise typing would go to whatever was focused before. */
    if (pressed) {
        aurwl_pointer_motion(c->wl, w, sx, sy, now_ms());
        aurwl_set_focus(c->wl, w);
        for (int i = 0; i < c->n_wins; i++)
            if (c->wins[i].content == aurwl_win_content(w)) { c->focus = i; break; }
    }
    aurwl_pointer_button(c->wl, button, pressed, now_ms());
    return 1;
}

int session_scroll(shell_ctx *c, int x, int y, int horizontal, double step)
{
    if (!c->wl) return 0;
    int sx = 0, sy = 0;
    if (!window_under(c, x, y, &sx, &sy)) return 0;
    aurwl_pointer_axis(c->wl, horizontal, step, now_ms());
    return 1;
}

int session_key(shell_ctx *c, int code, int pressed)
{
    if (!c->wl) return 0;
    return aurwl_key(c->wl, (uint32_t)code, pressed, now_ms());
}

/* ── popups ─────────────────────────────────────────────────────── */

/* Menus are separate surfaces the client positions against its parent.
 * They are painted here rather than by the archetypes because every
 * archetype's answer would be the same -- on top, at the offset the
 * client asked for -- and six copies of one answer is how the six
 * archetypes came to look identical in the first place.
 */
void session_paint_popups(shell_ctx *c, surface *fb)
{
    if (!c->wl) return;
    int n = aurwl_window_count(c->wl);
    for (int k = 0; k < n; k++) {
        aurwl_win *w = aurwl_window_at(c->wl, k);
        if (!aurwl_win_is_popup(w)) continue;
        surface *s = aurwl_win_content(w);
        if (!s) continue;

        /* A popup is placed against wherever its parent was actually
         * painted, scaled the same way, so a menu stays attached to its
         * button even when the window itself is shown scaled down. */
        aurwl_win *parent = aurwl_win_parent(w);
        rect pr = (rect){ 0, 0, c->screen_w, c->screen_h };
        double kx = 1.0, ky = 1.0;
        if (parent) {
            surface *ps = aurwl_win_content(parent);
            rect got;
            if (ps && draw_track_find(ps, &got)) {
                pr = got;
                if (ps->w > 0) kx = (double)got.w / ps->w;
                if (ps->h > 0) ky = (double)got.h / ps->h;
            }
        }
        int ox, oy; aurwl_win_popup_offset(w, &ox, &oy);
        rect dst = { pr.x + (int)(ox * kx), pr.y + (int)(oy * ky),
                     (int)(s->w * kx), (int)(s->h * ky) };
        if (dst.w <= 0 || dst.h <= 0) continue;
        draw_scaled(fb, s, dst, 1.f);
    }
}

/* ── the spawn hook the shell calls through ─────────────────────── */

int session_spawn(shell_ctx *c, const char *argv_blob, int n_args)
{
    if (!c->wl || n_args < 1 || n_args > APP_MAX_ARGS) return -1;
    /* Unpack the NUL-separated tokens into the vector execvp wants. */
    const char *argv[APP_MAX_ARGS + 1];
    const char *p = argv_blob;
    int i = 0;
    for (; i < n_args && *p; i++) { argv[i] = p; p += strlen(p) + 1; }
    argv[i] = NULL;
    if (!i) return -1;
    return aurwl_spawn(c->wl, argv) > 0 ? 0 : -1;
}
