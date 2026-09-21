/* aurwl.c — see aurwl.h for why this exists and why it is built this way.
 *
 * The protocol surface here is not a guess. It is the set that a real
 * browser was observed to bind: WebKitGTK's Epiphany, run against a
 * headless reference compositor with WAYLAND_DEBUG=1, bound
 * wl_compositor v5, wl_subcompositor, wl_shm, wl_output v3,
 * zxdg_output_manager_v1, wl_data_device_manager, wp_viewporter,
 * wp_presentation and xdg_wm_base v5 -- and then rendered its entire
 * window through wl_shm, with no dmabuf and no GL at all. That last
 * fact is the one this whole design rests on: a CPU-only compositor is
 * enough to run a browser, which is exactly what a machine from 2013
 * with no usable GPU driver needs to hear.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-server.h"
#include "xdg-decoration-server.h"
#include "viewporter-server.h"
#include "xdg-output-server.h"

#include "aurwl.h"

/* ── limits ─────────────────────────────────────────────────────── */
/* Bounds, not capacity planning: a client that asks for a 40000x40000
 * buffer is either broken or hostile, and either way the answer is to
 * refuse rather than to try the allocation. */
#define MAX_DIM     16384
/* How deep a chain of parents or subsurfaces may be before we stop
 * walking it. Any real window tree is a handful deep; a client can make
 * one a hundred thousand deep for the price of some memory. */
#define PARENT_MAX    256
#define MAX_WINS      128

enum role { ROLE_NONE = 0, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_SUBSURFACE, ROLE_CURSOR };

/* The half of wl_surface state that is double-buffered. Wayland's
 * central promise is that a frame is atomic: everything between two
 * commits lands together or not at all. Getting this wrong produces
 * tearing that looks like a driver bug. */
typedef struct {
    struct wl_resource *buffer;       /* pending only; never kept past commit */
    int                 attached;     /* attach() was called this cycle */
    int                 dx, dy;
    int                 dmg_x0, dmg_y0, dmg_x1, dmg_y1;  /* buffer coords */
    int                 has_damage;
    int                 scale;
    int                 transform;
    /* viewporter */
    int                 vp_dst_w, vp_dst_h;
} surf_state;

struct aurwl_win {
    aurwl              *c;
    struct wl_resource *res;          /* wl_surface */
    uint32_t            id;

    surf_state          pending, current;
    /* A client may destroy a wl_buffer it has attached but not yet
     * committed -- and an ordinary toolkit does exactly that when a
     * synchronized subsurface parks a buffer until its parent commits.
     * Without this listener the next commit called wl_shm_buffer_get()
     * on freed memory and then sent a release event through it. */
    struct wl_listener  buf_gone;
    struct wl_resource *buf_listening;
    struct wl_list      frame_cbs;    /* wl_resource link list */

    /* Our own copy of the client's pixels. Owning a copy rather than
     * mapping the client's buffer is a deliberate trade: one memcpy of
     * the damaged region per commit, in exchange for the client getting
     * its buffer back immediately and for the shell being free to
     * repaint at any moment without asking anyone's permission. The
     * alternative -- holding client memory across our frame -- makes
     * every paint a lifetime question and every truncated shm file a
     * SIGBUS in the compositor. */
    surface            *store;
    int                 cw, ch;       /* client's own idea of its size  */

    int                 role;
    int                 mapped;
    int                 dead;

    /* xdg */
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    struct wl_resource *xdg_popup;
    struct wl_resource *decoration;
    struct wl_resource *viewport;
    char                title[128];
    char                app_id[128];
    int                 want_w, want_h;      /* last configure we sent  */
    int                 want_act, want_max, want_full;
    int                 acked;
    int                 geo_x, geo_y, geo_w, geo_h;   /* window geometry */
    int                 has_geo;

    /* popup placement, relative to the parent surface's origin */
    int                 px, py;
    struct aurwl_win   *parent;

    /* subsurface */
    struct wl_resource *subsurface;
    int                 sub_x, sub_y;
    int                 sub_sync;
    int                 sub_cached;         /* a synced commit is waiting */

    struct wl_list      children;            /* subsurfaces, by link     */
    struct wl_list      child_link;
    struct wl_list      link;                /* c->surfaces              */
};

struct aurwl {
    struct wl_display  *display;
    struct wl_event_loop *loop;
    const char         *socket;

    struct wl_global   *g_compositor, *g_subcompositor, *g_seat, *g_output;
    struct wl_global   *g_xdg, *g_ddm, *g_deco, *g_viewporter, *g_xdg_output;

    struct wl_list      surfaces;
    struct wl_list      seats;        /* wl_seat resources              */
    struct wl_list      pointers, keyboards, touches, outputs, devices;
    /* Live wl_data_offers. An offer holds its source resource as user
     * data, and the source can be destroyed while the offer is still on
     * a client's clipboard menu -- so the offers have to be findable
     * when that happens. */
    struct wl_list      offers;
    /* Tracked so a mode change can re-send the logical size. It could
     * not before, so after a resize every client kept the old screen
     * size and sized its full-screen windows to it. */
    struct wl_list      xdg_outputs;

    int                 ow, oh, refresh_mhz;
    uint32_t            next_id;

    /* input */
    struct xkb_context *xkb;
    struct xkb_keymap  *keymap;
    struct xkb_state   *xkb_state;
    int                 keymap_fd;
    size_t              keymap_size;

    aurwl_win          *focus;        /* keyboard                        */
    aurwl_win          *ptr_focus;    /* pointer                         */
    int                 ptr_x, ptr_y; /* surface-local, last sent        */

    /* clipboard: the one data source currently offered as the selection */
    struct wl_resource *selection;
    uint32_t            selection_serial;

    uint32_t            damage_seq;

    pid_t               kids[64];
    int                 n_kids;
};

/* ── small helpers ──────────────────────────────────────────────── */

static uint32_t serial_of(aurwl *c) { return wl_display_next_serial(c->display); }

static void noop_destroy(struct wl_client *cl, struct wl_resource *r)
{ (void)cl; wl_resource_destroy(r); }

static void res_unlink(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }

/* Send an event to every resource in a list. The seat lists are usually
 * one element long; they are lists because a client is entitled to hold
 * two wl_pointers and expect both to work. */
#define FOR_EACH_RES(var, list) \
    struct wl_resource *var; wl_resource_for_each(var, list)

static struct wl_client *win_client(aurwl_win *w);

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ── wl_region ──────────────────────────────────────────────────── */
/* Regions describe where a surface is opaque and where it accepts
 * input. We track the bounding box only. The opaque hint is an
 * optimisation we do not yet take, and the input hint matters for
 * exactly one thing we care about -- a client-side drop shadow that
 * should not swallow clicks -- which a bounding box gets right for
 * every real toolkit, because they all subtract a border rather than
 * carve a hole. */
typedef struct { int x0, y0, x1, y1; int set; } aurwl_region;

static void region_add(struct wl_client *cl, struct wl_resource *r,
                       int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)cl; aurwl_region *g = wl_resource_get_user_data(r);
    if (w <= 0 || h <= 0) return;
    if (!g->set) { g->x0 = x; g->y0 = y; g->x1 = x + w; g->y1 = y + h; g->set = 1; return; }
    if (x < g->x0) g->x0 = x;
    if (y < g->y0) g->y0 = y;
    if (x + w > g->x1) g->x1 = x + w;
    if (y + h > g->y1) g->y1 = y + h;
}
static void region_subtract(struct wl_client *cl, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)cl; (void)r; (void)x; (void)y; (void)w; (void)h; }

static const struct wl_region_interface region_impl = {
    .destroy = noop_destroy, .add = region_add, .subtract = region_subtract,
};
static void region_free(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

/* ── wp_viewport ────────────────────────────────────────────────── */

static void viewport_destroy(struct wl_client *cl, struct wl_resource *r)
{
    (void)cl;
    aurwl_win *w = wl_resource_get_user_data(r);
    if (w && w->viewport == r) { w->pending.vp_dst_w = w->pending.vp_dst_h = 0; w->viewport = NULL; }
    wl_resource_destroy(r);
}
static void viewport_gone(struct wl_resource *r)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    if (w && w->viewport == r) w->viewport = NULL;
}
static void viewport_set_source(struct wl_client *cl, struct wl_resource *r,
                                wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h)
{ (void)cl; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void viewport_set_destination(struct wl_client *cl, struct wl_resource *r,
                                     int32_t w, int32_t h)
{
    (void)cl;
    aurwl_win *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->pending.vp_dst_w = w > 0 ? w : 0;
    s->pending.vp_dst_h = h > 0 ? h : 0;
}
static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy, .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

/* ── the buffer copy ────────────────────────────────────────────── */

/* Copy the damaged rows of a freshly committed shm buffer into our own
 * store. Returns 0 if the buffer was unusable, in which case the caller
 * still releases it -- refusing to copy is not a reason to strand a
 * client waiting for its buffer back. */
static int take_buffer(aurwl_win *w, struct wl_resource *buf)
{
    struct wl_shm_buffer *shm = wl_shm_buffer_get(buf);
    if (!shm) {
        /* No dmabuf support: a client that somehow got a non-shm buffer
         * to us gets told plainly rather than silently showing nothing. */
        wl_resource_post_error(buf, 0, "aurwl accepts wl_shm buffers only");
        return 0;
    }
    int32_t  bw     = wl_shm_buffer_get_width(shm);
    int32_t  bh     = wl_shm_buffer_get_height(shm);
    int32_t  stride = wl_shm_buffer_get_stride(shm);
    uint32_t fmt    = wl_shm_buffer_get_format(shm);

    if (bw <= 0 || bh <= 0 || bw > MAX_DIM || bh > MAX_DIM) return 0;
    if (fmt != WL_SHM_FORMAT_ARGB8888 && fmt != WL_SHM_FORMAT_XRGB8888) {
        wl_resource_post_error(buf, 0, "unsupported shm format %u", fmt);
        return 0;
    }
    if (stride < bw * 4) return 0;

    int full = 0;
    if (!w->store || w->store->w != bw || w->store->h != bh) {
        if (w->store) surface_free(w->store);
        w->store = surface_new(bw, bh);
        if (!w->store) return 0;
        full = 1;                       /* a new store has no old pixels */
    }
    w->cw = bw; w->ch = bh;

    int x0 = 0, y0 = 0, x1 = bw, y1 = bh;
    if (!full && w->current.has_damage) {
        x0 = clampi(w->current.dmg_x0, 0, bw);
        y0 = clampi(w->current.dmg_y0, 0, bh);
        x1 = clampi(w->current.dmg_x1, 0, bw);
        y1 = clampi(w->current.dmg_y1, 0, bh);
    }
    if (x1 <= x0 || y1 <= y0) return 1;  /* committed with nothing new */

    /* begin_access installs libwayland's SIGBUS handler, so a client
     * that truncates the shm file under us fails the copy instead of
     * killing the compositor and everything else running on it. */
    wl_shm_buffer_begin_access(shm);
    const uint8_t *src = wl_shm_buffer_get_data(shm);
    if (src) {
        int opaque = (fmt == WL_SHM_FORMAT_XRGB8888);
        for (int y = y0; y < y1; y++) {
            const uint32_t *s = (const uint32_t *)(src + (size_t)y * stride) + x0;
            uint32_t *d = w->store->px + (size_t)y * w->store->stride + x0;
            if (opaque) for (int x = 0; x < x1 - x0; x++) d[x] = s[x] | 0xFF000000u;
            else        memcpy(d, s, (size_t)(x1 - x0) * 4);
        }
    }
    wl_shm_buffer_end_access(shm);
    return 1;
}

/* Resample a store to the size a viewport asks for.
 *
 * wp_viewporter says the surface's size is the viewport's destination,
 * not the buffer's -- the buffer is scaled to it. We advertised the
 * global and ignored the request, which happens to be harmless for
 * every client measured (GTK and Chromium both set a destination equal
 * to their buffer at scale 1) and is a lie the moment one does not.
 *
 * Bilinear, and only when the sizes actually differ, so the common case
 * costs a comparison. */
static void resample_store(aurwl_win *w, int dw, int dh)
{
    surface *src = w->store;
    if (!src || dw <= 0 || dh <= 0 || dw > MAX_DIM || dh > MAX_DIM) return;
    if (src->w == dw && src->h == dh) return;

    surface *dst = surface_new(dw, dh);
    if (!dst) return;
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)src->h / (float)dh - 0.5f;
        int y0 = (int)fy; if (y0 < 0) y0 = 0;
        int y1 = y0 + 1 < src->h ? y0 + 1 : src->h - 1;
        float ty = fy - (float)y0; if (ty < 0.f) ty = 0.f;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)src->w / (float)dw - 0.5f;
            int x0 = (int)fx; if (x0 < 0) x0 = 0;
            int x1 = x0 + 1 < src->w ? x0 + 1 : src->w - 1;
            float tx = fx - (float)x0; if (tx < 0.f) tx = 0.f;
            uint32_t a = src->px[(size_t)y0 * src->stride + x0];
            uint32_t b = src->px[(size_t)y0 * src->stride + x1];
            uint32_t cc = src->px[(size_t)y1 * src->stride + x0];
            uint32_t d = src->px[(size_t)y1 * src->stride + x1];
            uint32_t out = 0;
            for (int ch = 0; ch < 4; ch++) {
                int sh = ch * 8;
                float t0 = (float)((a >> sh) & 0xFF) + tx * (float)(((b >> sh) & 0xFF) - (int)((a >> sh) & 0xFF));
                float t1 = (float)((cc >> sh) & 0xFF) + tx * (float)(((d >> sh) & 0xFF) - (int)((cc >> sh) & 0xFF));
                int v = (int)(t0 + ty * (t1 - t0) + 0.5f);
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                out |= (uint32_t)v << sh;
            }
            dst->px[(size_t)y * dst->stride + x] = out;
        }
    }
    surface_free(w->store);
    w->store = dst;
    w->cw = dw; w->ch = dh;
}

/* ── wl_surface ─────────────────────────────────────────────────── */

static void surface_map(aurwl_win *w);
static void surface_unmap(aurwl_win *w);

static void buffer_gone(struct wl_listener *l, void *data)
{
    (void)data;
    aurwl_win *w = wl_container_of(l, w, buf_gone);
    /* Forget the attach rather than treating it as attach(NULL): a
     * client that throws away a buffer it never committed has not asked
     * to be unmapped, and unmapping it would make a window vanish
     * because of a bookkeeping detail the user never sees. */
    w->pending.buffer = NULL;
    w->pending.attached = 0;
    w->buf_listening = NULL;
}
static void watch_buffer(aurwl_win *w, struct wl_resource *buf)
{
    if (w->buf_listening == buf) return;
    if (w->buf_listening) { wl_list_remove(&w->buf_gone.link); w->buf_listening = NULL; }
    if (!buf) return;
    w->buf_gone.notify = buffer_gone;
    wl_resource_add_destroy_listener(buf, &w->buf_gone);
    w->buf_listening = buf;
}

static void surf_attach(struct wl_client *cl, struct wl_resource *r,
                        struct wl_resource *buf, int32_t dx, int32_t dy)
{
    (void)cl; aurwl_win *w = wl_resource_get_user_data(r);
    watch_buffer(w, buf);
    w->pending.buffer = buf;
    w->pending.attached = 1;
    if (wl_resource_get_version(r) < WL_SURFACE_OFFSET_SINCE_VERSION) {
        w->pending.dx = dx; w->pending.dy = dy;
    }
}
static void surf_offset(struct wl_client *cl, struct wl_resource *r, int32_t dx, int32_t dy)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); w->pending.dx = dx; w->pending.dy = dy; }

static void damage_accum(surf_state *st, int x, int y, int ww, int hh, int scale)
{
    if (ww <= 0 || hh <= 0) return;
    /* In 64 bits and clamped. A client may send any int32 for any of
     * these, and (x + ww) * scale overflows -- which is undefined
     * behaviour, not merely a wrong rectangle. The copy clamps to the
     * buffer afterwards, so this never reached out of bounds, but
     * undefined behaviour is not a thing to leave lying about in the
     * process that owns the display. */
    long long sc = scale < 1 ? 1 : scale;
    long long lx0 = (long long)x * sc,          ly0 = (long long)y * sc;
    long long lx1 = ((long long)x + ww) * sc,   ly1 = ((long long)y + hh) * sc;
    if (lx0 < 0) lx0 = 0;
    if (ly0 < 0) ly0 = 0;
    if (lx1 > MAX_DIM) lx1 = MAX_DIM;
    if (ly1 > MAX_DIM) ly1 = MAX_DIM;
    if (lx1 <= lx0 || ly1 <= ly0) return;
    int x0 = (int)lx0, y0 = (int)ly0, x1 = (int)lx1, y1 = (int)ly1;
    if (!st->has_damage) { st->dmg_x0 = x0; st->dmg_y0 = y0; st->dmg_x1 = x1; st->dmg_y1 = y1; st->has_damage = 1; return; }
    if (x0 < st->dmg_x0) st->dmg_x0 = x0;
    if (y0 < st->dmg_y0) st->dmg_y0 = y0;
    if (x1 > st->dmg_x1) st->dmg_x1 = x1;
    if (y1 > st->dmg_y1) st->dmg_y1 = y1;
}
static void surf_damage(struct wl_client *cl, struct wl_resource *r,
                        int32_t x, int32_t y, int32_t ww, int32_t hh)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r);
  damage_accum(&w->pending, x, y, ww, hh, w->pending.scale); }

static void surf_damage_buffer(struct wl_client *cl, struct wl_resource *r,
                               int32_t x, int32_t y, int32_t ww, int32_t hh)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r);
  damage_accum(&w->pending, x, y, ww, hh, 1); }

static void surf_frame(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(cl, &wl_callback_interface, 1, id);
    if (!cb) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(cb, NULL, NULL, res_unlink);
    wl_list_insert(w->frame_cbs.prev, wl_resource_get_link(cb));
}
static void surf_set_opaque(struct wl_client *cl, struct wl_resource *r, struct wl_resource *reg)
{ (void)cl; (void)r; (void)reg; }
static void surf_set_input(struct wl_client *cl, struct wl_resource *r, struct wl_resource *reg)
{ (void)cl; (void)r; (void)reg; }
static void surf_set_transform(struct wl_client *cl, struct wl_resource *r, int32_t t)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); w->pending.transform = t; }
static void surf_set_scale(struct wl_client *cl, struct wl_resource *r, int32_t s)
{
    (void)cl; aurwl_win *w = wl_resource_get_user_data(r);
    if (s < 1) { wl_resource_post_error(r, WL_SURFACE_ERROR_INVALID_SCALE, "scale %d", s); return; }
    w->pending.scale = s;
}

static void apply_commit(aurwl_win *w, int depth)
{
    /* Not an infinite recursion -- sub_cached is cleared before
     * recursing and nothing inside ever sets it, so a ring terminates.
     * What is unbounded is the DEPTH: a chain of a hundred thousand
     * synchronized subsurfaces, each committed once, recurses that far
     * on one commit and walks off the end of the stack. That is
     * affordable for a client to build. */
    if (depth > PARENT_MAX) return;
    surf_state *p = &w->pending;

    /* Carry damage into current before take_buffer reads it, since the
     * copy is bounded by the damage that arrived with this buffer. */
    w->current.has_damage = p->has_damage;
    w->current.dmg_x0 = p->dmg_x0; w->current.dmg_y0 = p->dmg_y0;
    w->current.dmg_x1 = p->dmg_x1; w->current.dmg_y1 = p->dmg_y1;
    w->current.scale = p->scale;
    w->current.transform = p->transform;
    w->current.vp_dst_w = p->vp_dst_w;
    w->current.vp_dst_h = p->vp_dst_h;

    if (p->attached) {
        if (p->buffer) {
            take_buffer(w, p->buffer);
            if (w->current.vp_dst_w > 0 && w->current.vp_dst_h > 0)
                resample_store(w, w->current.vp_dst_w, w->current.vp_dst_h);
            w->c->damage_seq++;
            wl_buffer_send_release(p->buffer);
            if (!w->mapped) surface_map(w);
        } else {
            /* attach(NULL) is how a client unmaps itself. */
            if (w->mapped) surface_unmap(w);
            if (w->store) { surface_free(w->store); w->store = NULL; }
            w->cw = w->ch = 0;
        }
    }
    p->attached = 0; p->buffer = NULL; p->has_damage = 0;
    watch_buffer(w, NULL);

    /* A synchronized subsurface's own commit does not take effect until
     * its parent commits; that is what makes a parent and its children
     * resize as one thing instead of tearing against each other. */
    aurwl_win *ch;
    wl_list_for_each(ch, &w->children, child_link)
        if (ch->sub_sync && ch->sub_cached) { ch->sub_cached = 0; apply_commit(ch, depth + 1); }
}

static void surf_commit(struct wl_client *cl, struct wl_resource *r)
{
    (void)cl; aurwl_win *w = wl_resource_get_user_data(r);

    if (w->role == ROLE_SUBSURFACE && w->sub_sync) { w->sub_cached = 1; return; }

    /* xdg-shell's handshake: the first commit carries no buffer and
     * asks us what size to be. Answering it is not optional -- a client
     * that never receives a configure never draws, which presents as a
     * window that silently fails to appear. */
    if (w->xdg_surface && !w->acked && !w->pending.buffer) {
        apply_commit(w, 0);
        if (w->xdg_toplevel) {
            struct wl_array states; wl_array_init(&states);
            xdg_toplevel_send_configure(w->xdg_toplevel, 0, 0, &states);
            wl_array_release(&states);
        }
        xdg_surface_send_configure(w->xdg_surface, serial_of(w->c));
        return;
    }
    apply_commit(w, 0);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = noop_destroy, .attach = surf_attach, .damage = surf_damage,
    .frame = surf_frame, .set_opaque_region = surf_set_opaque,
    .set_input_region = surf_set_input, .commit = surf_commit,
    .set_buffer_transform = surf_set_transform, .set_buffer_scale = surf_set_scale,
    .damage_buffer = surf_damage_buffer, .offset = surf_offset,
};

/* ── surface lifecycle ──────────────────────────────────────────── */

static void surface_map(aurwl_win *w)
{
    if (w->role != ROLE_TOPLEVEL && w->role != ROLE_POPUP) { w->mapped = 1; return; }
    w->mapped = 1;
    /* The client needs to know which output it is on before it can pick
     * a scale factor; GTK will not finish its first layout without it. */
    FOR_EACH_RES(o, &w->c->outputs)
        if (wl_resource_get_client(o) == wl_resource_get_client(w->res))
            wl_surface_send_enter(w->res, o);
}

static void surface_unmap(aurwl_win *w)
{
    aurwl *c = w->c;
    w->mapped = 0;
    /* An unmapped xdg_surface goes back to needing the initial
     * handshake: the client commits with no buffer and waits for a
     * configure. `acked` stayed set across the unmap, so that second
     * handshake was skipped, no configure was ever sent, and the client
     * waited forever -- a window that is hidden and never comes back.
     * GTK dodges it by destroying the role object on hide; a client
     * that follows the documented lighter path does not. */
    w->acked = 0;

    /* The surface object is still alive -- the client only attached a
     * null buffer -- so it has no way to find out it lost focus unless
     * we say so. Without the keyboard leave it must assume its keys are
     * still down, and key repeat runs forever. */
    uint32_t ser = serial_of(c);
    if (c->focus == w) {
        struct wl_client *cl = win_client(w);
        FOR_EACH_RES(k, &c->keyboards)
            if (wl_resource_get_client(k) == cl) wl_keyboard_send_leave(k, ser, w->res);
        c->focus = NULL;
    }
    if (c->ptr_focus == w) {
        struct wl_client *cl = win_client(w);
        FOR_EACH_RES(pp, &c->pointers)
            if (wl_resource_get_client(pp) == cl) {
                wl_pointer_send_leave(pp, ser, w->res);
                if (wl_resource_get_version(pp) >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pp);
            }
        c->ptr_focus = NULL;
    }
}

static void surface_free_res(struct wl_resource *r)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    if (!w) return;
    aurwl *c = w->c;
    if (c->focus == w)     c->focus = NULL;
    if (c->ptr_focus == w) c->ptr_focus = NULL;

    /* When a client dies, libwayland destroys its resources in an order
     * we do not choose -- and it destroyed the wl_surface BEFORE the
     * xdg_surface built on it. The xdg_surface's destructor then wrote
     * through this freed pointer.
     *
     * That is not a corner case. It is what happens every single time
     * an application crashes or is killed, and it took the compositor
     * down with it -- so one misbehaving program closed every other
     * window on the machine. Found by SIGKILLing a terminal under
     * AddressSanitizer; it is not reachable by any polite client, which
     * is exactly why it survived the polite tests.
     *
     * Every resource that points back here is disarmed before the
     * memory goes. Their destructors already check for NULL, so they
     * become no-ops in whatever order libwayland runs them. */
    struct wl_resource *dependents[] = {
        w->xdg_toplevel, w->xdg_popup, w->xdg_surface,
        w->subsurface, w->decoration, w->viewport,
    };
    for (size_t i = 0; i < sizeof dependents / sizeof dependents[0]; i++)
        if (dependents[i]) wl_resource_set_user_data(dependents[i], NULL);

    /* A parent may outlive its children or the other way round; both
     * directions have to be unhooked or the next walk of either list
     * follows a pointer into freed memory. */
    aurwl_win *ch, *tmp;
    wl_list_for_each_safe(ch, tmp, &w->children, child_link) {
        wl_list_remove(&ch->child_link);
        wl_list_init(&ch->child_link);
        ch->parent = NULL;
    }
    /* child_link is wl_list_init()'d at creation, so it is always a
     * valid node -- removing one that was never inserted just re-points
     * it at itself. */
    wl_list_remove(&w->child_link);
    w->parent = NULL;

    watch_buffer(w, NULL);

    struct wl_resource *cb, *cbt;
    wl_resource_for_each_safe(cb, cbt, &w->frame_cbs) wl_resource_destroy(cb);

    wl_list_remove(&w->link);
    if (w->store) surface_free(w->store);
    free(w);
}

/* ── wl_compositor ──────────────────────────────────────────────── */

static void comp_create_surface(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    aurwl *c = wl_resource_get_user_data(r);
    aurwl_win *w = calloc(1, sizeof *w);
    if (!w) { wl_client_post_no_memory(cl); return; }
    w->c = c;
    w->id = ++c->next_id;
    w->pending.scale = w->current.scale = 1;
    wl_list_init(&w->frame_cbs);
    wl_list_init(&w->children);
    wl_list_init(&w->child_link);

    w->res = wl_resource_create(cl, &wl_surface_interface, wl_resource_get_version(r), id);
    if (!w->res) { free(w); wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(w->res, &surface_impl, w, surface_free_res);
    wl_list_insert(c->surfaces.prev, &w->link);
}

static void comp_create_region(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    (void)r;
    aurwl_region *g = calloc(1, sizeof *g);
    if (!g) { wl_client_post_no_memory(cl); return; }
    struct wl_resource *res = wl_resource_create(cl, &wl_region_interface, 1, id);
    if (!res) { free(g); wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(res, &region_impl, g, region_free);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = comp_create_surface, .create_region = comp_create_region,
};

static void bind_compositor(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(cl, &wl_compositor_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &compositor_impl, data, NULL);
}

/* ── wl_subsurface ──────────────────────────────────────────────── */

static void sub_destroy(struct wl_client *cl, struct wl_resource *r)
{
    (void)cl;
    aurwl_win *w = wl_resource_get_user_data(r);
    if (w && w->subsurface == r) {
        if (w->parent) { wl_list_remove(&w->child_link); wl_list_init(&w->child_link); w->parent = NULL; }
        w->role = ROLE_NONE; w->subsurface = NULL;
    }
    wl_resource_destroy(r);
}
static void sub_set_position(struct wl_client *cl, struct wl_resource *r, int32_t x, int32_t y)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (w) { w->sub_x = x; w->sub_y = y; } }

static void sub_place(struct wl_resource *r, aurwl_win *w,
                     struct wl_resource *sib_res, int above)
{
    if (!w || !w->parent) return;
    aurwl_win *sib = sib_res ? wl_resource_get_user_data(sib_res) : NULL;

    /* The sibling must be an actual sibling, or the parent. A client
     * passing its OWN surface was accepted, and then:
     *
     *   place_below -- wl_list_remove() zeroes both links, so
     *     sib->child_link.prev is read as NULL and wl_list_insert()
     *     dereferences address 8. Four requests, and the compositor is
     *     dead along with every window on the machine.
     *
     *   place_above -- no crash, but the surface ends up in a self-ring
     *     while w->parent still names its parent. It is then invisible
     *     to the parent's walk in surface_free_res(), so when the parent
     *     goes, this child keeps a pointer to freed memory -- which the
     *     shell dereferences every frame in session_paint_popups().
     *
     * Two sources of truth, `parent` and `child_link`, and this was the
     * one function that could pull them apart. */
    if (sib == w) {
        wl_resource_post_error(r, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                               "a subsurface cannot be its own sibling");
        return;
    }
    if (sib && sib != w->parent && sib->parent != w->parent) {
        wl_resource_post_error(r, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                               "not a sibling of this subsurface");
        return;
    }
    wl_list_remove(&w->child_link);
    if (sib && sib != w->parent)
        wl_list_insert(above ? &sib->child_link : sib->child_link.prev, &w->child_link);
    else
        wl_list_insert(above ? w->parent->children.prev : &w->parent->children, &w->child_link);
}
static void sub_place_above(struct wl_client *cl, struct wl_resource *r, struct wl_resource *s)
{ (void)cl; sub_place(r, wl_resource_get_user_data(r), s, 1); }
static void sub_place_below(struct wl_client *cl, struct wl_resource *r, struct wl_resource *s)
{ (void)cl; sub_place(r, wl_resource_get_user_data(r), s, 0); }
static void sub_set_sync(struct wl_client *cl, struct wl_resource *r)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (w) w->sub_sync = 1; }
static void sub_set_desync(struct wl_client *cl, struct wl_resource *r)
{
    (void)cl; aurwl_win *w = wl_resource_get_user_data(r);
    if (!w) return;
    w->sub_sync = 0;
    if (w->sub_cached) { w->sub_cached = 0; apply_commit(w, 0); }
}
static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = sub_destroy, .set_position = sub_set_position,
    .place_above = sub_place_above, .place_below = sub_place_below,
    .set_sync = sub_set_sync, .set_desync = sub_set_desync,
};
static void subsurface_gone(struct wl_resource *r)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    if (w && w->subsurface == r) w->subsurface = NULL;
}

/* Would making `cand` the parent of `w` close a loop?
 *
 * `parent` is shared between subsurfaces and popups, and only the
 * subsurface path ever checked -- so two popups could name each other
 * and the subsurface check would then walk that ring forever, spinning
 * at 100% inside a client request with the event loop never reached
 * again. A hang in the process that owns the display is as fatal as a
 * crash, and harder to describe to the person it happens to.
 *
 * The step bound is belt and braces: it makes the walk terminate even
 * if some future path builds a ring this cannot see. */
static int parent_loop(aurwl_win *cand, aurwl_win *w)
{
    int steps = 0;
    for (aurwl_win *a = cand; a; a = a->parent) {
        if (a == w) return 1;
        if (++steps > PARENT_MAX) return 1;
    }
    return 0;
}

static void subcomp_get(struct wl_client *cl, struct wl_resource *r, uint32_t id,
                        struct wl_resource *surf, struct wl_resource *parent)
{
    (void)r;
    aurwl_win *w = surf ? wl_resource_get_user_data(surf) : NULL;
    aurwl_win *p = parent ? wl_resource_get_user_data(parent) : NULL;
    if (!w || !p || w == p) {
        wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "a surface cannot be its own subsurface");
        return;
    }
    if (parent_loop(p, w)) {
        wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "subsurface loop");
        return;
    }
    if (w->subsurface || w->xdg_toplevel || w->xdg_popup) {
        wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "this surface already has a role");
        return;
    }
    struct wl_resource *res = wl_resource_create(cl, &wl_subsurface_interface, 1, id);
    if (!res) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(res, &subsurface_impl, w, subsurface_gone);
    w->subsurface = res;
    w->role = ROLE_SUBSURFACE;
    w->sub_sync = 1;                 /* the protocol's default */
    if (w->parent) wl_list_remove(&w->child_link);
    w->parent = p;
    wl_list_insert(p->children.prev, &w->child_link);
}
static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = noop_destroy, .get_subsurface = subcomp_get,
};
static void bind_subcompositor(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(cl, &wl_subcompositor_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &subcompositor_impl, data, NULL);
}

/* ── wp_viewporter ──────────────────────────────────────────────── */

static void vper_get(struct wl_client *cl, struct wl_resource *r, uint32_t id,
                     struct wl_resource *surf)
{
    (void)r;
    struct wl_resource *res = wl_resource_create(cl, &wp_viewport_interface, 1, id);
    if (!res) { wl_client_post_no_memory(cl); return; }
    aurwl_win *w = surf ? wl_resource_get_user_data(surf) : NULL;
    if (w && w->viewport) {
        wl_resource_destroy(res);
        wl_resource_post_error(r, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                               "this surface already has a viewport");
        return;
    }
    wl_resource_set_implementation(res, &viewport_impl, w, viewport_gone);
    if (w) w->viewport = res;
}
static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = noop_destroy, .get_viewport = vper_get,
};
static void bind_viewporter(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(cl, &wp_viewporter_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &viewporter_impl, data, NULL);
}

/* ── wl_output ──────────────────────────────────────────────────── */

static void output_send(aurwl *c, struct wl_resource *r)
{
    int ver = wl_resource_get_version(r);
    /* Physical size 0x0 means "unknown", which is honest: we get the
     * mode from KMS but not reliably the panel's millimetres, and a
     * wrong number makes a toolkit choose a wrong DPI. */
    wl_output_send_geometry(r, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "AurOS", "Display", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        c->ow, c->oh, c->refresh_mhz);
    if (ver >= WL_OUTPUT_SCALE_SINCE_VERSION) wl_output_send_scale(r, 1);
    /* name and description are sent once per object and never again --
     * this function also runs on every mode change and VT return. */
    if (ver >= WL_OUTPUT_DONE_SINCE_VERSION)  wl_output_send_done(r);
}
static const struct wl_output_interface output_impl = { .release = noop_destroy };
static void bind_output(struct wl_client *cl, void *data, uint32_t ver_, uint32_t id)
{
    aurwl *c = data;
    struct wl_resource *r = wl_resource_create(cl, &wl_output_interface, (int)ver_, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &output_impl, c, res_unlink);
    wl_list_insert(&c->outputs, wl_resource_get_link(r));
    int ver = (int)ver_;
    if (ver >= WL_OUTPUT_NAME_SINCE_VERSION)        wl_output_send_name(r, "AUR-1");
    if (ver >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION) wl_output_send_description(r, "AurOS display");
    output_send(c, r);
}

/* zxdg_output_manager_v1 predates wl_output's name/description events
 * and GTK still asks for it; without it the toolkit waits for an output
 * description that never arrives before settling its first frame. */
static const struct zxdg_output_v1_interface xdg_output_impl = { .destroy = noop_destroy };
/* Send an xdg_output's properties and close the sequence.
 *
 * From version 3 the closing event is wl_output.done, not
 * zxdg_output_v1.done -- and a toolkit that follows that waits for the
 * wl_output.done which used to be sent before get_xdg_output was ever
 * called, so it never applied the logical size and reported the monitor
 * as unknown. The name and description guard was also inverted: they
 * exist since version 2, and were being sent to version 1 clients
 * (where libwayland calls a listener slot that does not exist and
 * aborts the client) and withheld from the versions that want them. */
static void xdg_output_send(aurwl *c, struct wl_resource *res)
{
    int ver = wl_resource_get_version(res);
    zxdg_output_v1_send_logical_position(res, 0, 0);
    zxdg_output_v1_send_logical_size(res, c->ow, c->oh);
    if (ver >= 2 && ver < 3) {
        zxdg_output_v1_send_name(res, "AUR-1");
        zxdg_output_v1_send_description(res, "AurOS display");
    }
    if (ver < 3) {
        zxdg_output_v1_send_done(res);
    } else {
        struct wl_client *cl = wl_resource_get_client(res);
        FOR_EACH_RES(o, &c->outputs)
            if (wl_resource_get_client(o) == cl &&
                wl_resource_get_version(o) >= WL_OUTPUT_DONE_SINCE_VERSION)
                wl_output_send_done(o);
    }
}

static void xdgout_get(struct wl_client *cl, struct wl_resource *r, uint32_t id,
                       struct wl_resource *out)
{
    aurwl *c = wl_resource_get_user_data(r);
    (void)out;
    struct wl_resource *res = wl_resource_create(cl, &zxdg_output_v1_interface,
                                                 wl_resource_get_version(r), id);
    if (!res) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(res, &xdg_output_impl, c, res_unlink);
    wl_list_insert(&c->xdg_outputs, wl_resource_get_link(res));
    xdg_output_send(c, res);
}
static const struct zxdg_output_manager_v1_interface xdg_output_mgr_impl = {
    .destroy = noop_destroy, .get_xdg_output = xdgout_get,
};
static void bind_xdg_output(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(cl, &zxdg_output_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &xdg_output_mgr_impl, data, NULL);
}

/* ── wl_seat ────────────────────────────────────────────────────── */

static const struct wl_pointer_interface pointer_impl_s;
static void ptr_set_cursor(struct wl_client *cl, struct wl_resource *r, uint32_t serial,
                           struct wl_resource *surf, int32_t hx, int32_t hy)
{
    (void)cl; (void)r; (void)serial; (void)hx; (void)hy;
    /* The shell draws one cursor for the whole system, from the theme,
     * so a client's cursor surface is accepted and not shown. Marking
     * it as a cursor keeps it out of the window list. */
    if (surf) { aurwl_win *w = wl_resource_get_user_data(surf); if (w) w->role = ROLE_CURSOR; }
}
static const struct wl_pointer_interface pointer_impl_s = {
    .set_cursor = ptr_set_cursor, .release = noop_destroy,
};
static const struct wl_keyboard_interface keyboard_impl_s = { .release = noop_destroy };
static const struct wl_touch_interface    touch_impl_s    = { .release = noop_destroy };

static void seat_get_pointer(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    aurwl *c = wl_resource_get_user_data(r);
    struct wl_resource *p = wl_resource_create(cl, &wl_pointer_interface,
                                               wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(p, &pointer_impl_s, c, res_unlink);
    wl_list_insert(&c->pointers, wl_resource_get_link(p));
}
static void seat_get_keyboard(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    aurwl *c = wl_resource_get_user_data(r);
    struct wl_resource *k = wl_resource_create(cl, &wl_keyboard_interface,
                                               wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(k, &keyboard_impl_s, c, res_unlink);
    wl_list_insert(&c->keyboards, wl_resource_get_link(k));
    /* An fd, always. libwayland dups every fd argument as it marshals,
     * and dup(-1) fails -- which drops the message and flags the client
     * errored. So a machine with no xkb data did not degrade to "no key
     * translation" as intended: it disconnected every client at
     * get_keyboard, and nothing could open a window at all. */
    wl_keyboard_send_keymap(k,
        c->keymap_size ? WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1
                       : WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP,
        c->keymap_fd, (uint32_t)c->keymap_size);
    if (wl_resource_get_version(k) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(k, 25, 400);
}
static void seat_get_touch(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    aurwl *c = wl_resource_get_user_data(r);
    struct wl_resource *t = wl_resource_create(cl, &wl_touch_interface,
                                               wl_resource_get_version(r), id);
    if (!t) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(t, &touch_impl_s, c, res_unlink);
    wl_list_insert(&c->touches, wl_resource_get_link(t));
}
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer, .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch, .release = noop_destroy,
};
static void bind_seat(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    aurwl *c = data;
    struct wl_resource *r = wl_resource_create(cl, &wl_seat_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &seat_impl, c, res_unlink);
    wl_list_insert(&c->seats, wl_resource_get_link(r));
    /* Touch is advertised unconditionally: these machines are often
     * convertibles, hotplug exists, and a client that learns about a
     * capability late handles it far worse than one that always had it. */
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER |
                                 WL_SEAT_CAPABILITY_KEYBOARD |
                                 WL_SEAT_CAPABILITY_TOUCH);
    if (wl_resource_get_version(r) >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(r, "seat0");
}

/* ── clipboard ──────────────────────────────────────────────────── */
/* Copy and paste is not a nicety on a machine whose purpose is a web
 * browser. The mechanism is a relay: the owning client keeps the data,
 * we keep only the offer, and when someone pastes we hand the reader's
 * pipe to the owner and step out of the way. */

typedef struct {
    aurwl              *c;
    struct wl_resource *res;
    char                mimes[16][64];
    int                 n_mimes;
    struct wl_list      link;
} data_source;

static void dsrc_offer(struct wl_client *cl, struct wl_resource *r, const char *mime)
{
    (void)cl; data_source *s = wl_resource_get_user_data(r);
    if (!s || s->n_mimes >= 16) return;
    snprintf(s->mimes[s->n_mimes], sizeof s->mimes[0], "%s", mime);
    s->n_mimes++;
}
static void dsrc_set_actions(struct wl_client *cl, struct wl_resource *r, uint32_t a)
{ (void)cl; (void)r; (void)a; }
static const struct wl_data_source_interface data_source_impl = {
    .offer = dsrc_offer, .destroy = noop_destroy, .set_actions = dsrc_set_actions,
};
static void dsrc_gone(struct wl_resource *r)
{
    data_source *s = wl_resource_get_user_data(r);
    if (!s) return;
    if (s->c->selection == r) s->c->selection = NULL;
    /* Any offer still pointing at this source now points at freed
     * memory. A paste from a menu that was open when the copying
     * application quit would have written through it -- which is a
     * perfectly ordinary thing for a person to do. */
    struct wl_resource *o, *ot;
    wl_resource_for_each_safe(o, ot, &s->c->offers)
        if (wl_resource_get_user_data(o) == r) wl_resource_destroy(o);
    /* And say so. Otherwise every client goes on showing a paste target
     * for a clipboard that no longer exists, and pasting yields a pipe
     * that closes immediately -- which reads as the application being
     * broken rather than the clipboard being empty. */
    struct wl_resource *dev;
    wl_resource_for_each(dev, &s->c->devices) wl_data_device_send_selection(dev, NULL);
    free(s);
}

static void doffer_accept(struct wl_client *cl, struct wl_resource *r, uint32_t serial, const char *m)
{ (void)cl; (void)r; (void)serial; (void)m; }
static void doffer_receive(struct wl_client *cl, struct wl_resource *r, const char *mime, int32_t fd)
{
    (void)cl;
    /* NULL means the application that copied has since quit. The read
     * end still has to be closed or the paste hangs forever. */
    struct wl_resource *src = wl_resource_get_user_data(r);
    if (src) wl_data_source_send_send(src, mime, fd);
    /* Our end of the pipe must go, or the reader never sees EOF and a
     * paste hangs forever on a mime type the owner declines to write. */
    close(fd);
}
static void doffer_finish(struct wl_client *cl, struct wl_resource *r) { (void)cl; (void)r; }
static void doffer_set_actions(struct wl_client *cl, struct wl_resource *r, uint32_t a, uint32_t b)
{ (void)cl; (void)r; (void)a; (void)b; }
static const struct wl_data_offer_interface data_offer_impl = {
    .accept = doffer_accept, .receive = doffer_receive, .destroy = noop_destroy,
    .finish = doffer_finish, .set_actions = doffer_set_actions,
};

/* Offer the current selection to one data_device. */
static void send_selection_to(aurwl *c, struct wl_resource *dev)
{
    if (!c->selection) { wl_data_device_send_selection(dev, NULL); return; }
    data_source *s = wl_resource_get_user_data(c->selection);
    if (!s) { wl_data_device_send_selection(dev, NULL); return; }
    struct wl_client *cl = wl_resource_get_client(dev);
    struct wl_resource *offer = wl_resource_create(cl, &wl_data_offer_interface,
                                                   wl_resource_get_version(dev), 0);
    if (!offer) return;
    wl_resource_set_implementation(offer, &data_offer_impl, c->selection, res_unlink);
    wl_list_insert(&c->offers, wl_resource_get_link(offer));
    wl_data_device_send_data_offer(dev, offer);
    for (int i = 0; i < s->n_mimes; i++) wl_data_offer_send_offer(offer, s->mimes[i]);
    wl_data_device_send_selection(dev, offer);
}

static void ddev_start_drag(struct wl_client *cl, struct wl_resource *r,
                            struct wl_resource *src, struct wl_resource *origin,
                            struct wl_resource *icon, uint32_t serial)
{
    (void)cl; (void)r; (void)origin; (void)serial;
    /* Drag and drop between applications is not wired up; a client that
     * starts one is told immediately that it ended, rather than being
     * left holding a drag that never resolves. */
    if (icon) { aurwl_win *w = wl_resource_get_user_data(icon); if (w) w->role = ROLE_CURSOR; }
    if (src) wl_data_source_send_cancelled(src);
}
static void ddev_set_selection(struct wl_client *cl, struct wl_resource *r,
                               struct wl_resource *src, uint32_t serial)
{
    (void)cl;
    aurwl *c = wl_resource_get_user_data(r);
    if (c->selection && c->selection != src) wl_data_source_send_cancelled(c->selection);
    c->selection = src;
    c->selection_serial = serial;
    /* Retire the offers for the previous selection. Leaving them alive
     * leaked one wl_data_offer per device per focus change, and -- worse
     * than the leak -- a client that kept an old offer could go on
     * reading the clipboard contents it described long after the user
     * had copied something else. */
    struct wl_resource *old, *oldt;
    wl_resource_for_each_safe(old, oldt, &c->offers) wl_resource_destroy(old);
    /* Only the client with keyboard focus. The protocol says the
     * selection event goes to a client immediately before it receives
     * keyboard focus, and while it has focus -- and it says so for a
     * reason. Broadcasting it handed every background process on the
     * machine a live wl_data_offer for whatever the user had just
     * copied, readable at any moment with nothing on screen to show it.
     * Copy a password out of a password manager and every running
     * program could read it. */
    struct wl_client *t = win_client(c->focus);
    if (!t) return;
    struct wl_resource *dev;
    wl_resource_for_each(dev, &c->devices)
        if (wl_resource_get_client(dev) == t) send_selection_to(c, dev);
}
static const struct wl_data_device_interface data_device_impl = {
    .start_drag = ddev_start_drag, .set_selection = ddev_set_selection,
    .release = noop_destroy,
};

static void ddm_create_source(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    aurwl *c = wl_resource_get_user_data(r);
    data_source *s = calloc(1, sizeof *s);
    if (!s) { wl_client_post_no_memory(cl); return; }
    s->c = c;
    s->res = wl_resource_create(cl, &wl_data_source_interface, wl_resource_get_version(r), id);
    if (!s->res) { free(s); wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(s->res, &data_source_impl, s, dsrc_gone);
}
static void ddm_get_device(struct wl_client *cl, struct wl_resource *r, uint32_t id,
                           struct wl_resource *seat)
{
    (void)seat;
    aurwl *c = wl_resource_get_user_data(r);
    struct wl_resource *dev = wl_resource_create(cl, &wl_data_device_interface,
                                                 wl_resource_get_version(r), id);
    if (!dev) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(dev, &data_device_impl, c, res_unlink);
    wl_list_insert(&c->devices, wl_resource_get_link(dev));
    /* Nothing is offered here. A client gets the selection when it is
     * given keyboard focus, not when it asks for a data device -- which
     * it does at startup, long before the user has chosen it. */
}
static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_source, .get_data_device = ddm_get_device,
};
static void bind_ddm(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(cl, &wl_data_device_manager_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &ddm_impl, data, NULL);
}

/* ── xdg-shell ──────────────────────────────────────────────────── */

typedef struct {
    int      w, h;
    int      ax, ay, aw, ah;      /* anchor rect, parent-local */
    uint32_t anchor, gravity, constraint;
    int      ox, oy;
} positioner;

static void pos_set_size(struct wl_client *cl, struct wl_resource *r, int32_t w, int32_t h)
{ (void)cl; positioner *p = wl_resource_get_user_data(r); p->w = w; p->h = h; }
static void pos_set_anchor_rect(struct wl_client *cl, struct wl_resource *r,
                                int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)cl; positioner *p = wl_resource_get_user_data(r); p->ax=x; p->ay=y; p->aw=w; p->ah=h; }
static void pos_set_anchor(struct wl_client *cl, struct wl_resource *r, uint32_t a)
{ (void)cl; ((positioner *)wl_resource_get_user_data(r))->anchor = a; }
static void pos_set_gravity(struct wl_client *cl, struct wl_resource *r, uint32_t g)
{ (void)cl; ((positioner *)wl_resource_get_user_data(r))->gravity = g; }
static void pos_set_constraint(struct wl_client *cl, struct wl_resource *r, uint32_t a)
{ (void)cl; ((positioner *)wl_resource_get_user_data(r))->constraint = a; }
static void pos_set_offset(struct wl_client *cl, struct wl_resource *r, int32_t x, int32_t y)
{ (void)cl; positioner *p = wl_resource_get_user_data(r); p->ox = x; p->oy = y; }
static void pos_set_reactive(struct wl_client *cl, struct wl_resource *r) { (void)cl; (void)r; }
static void pos_set_parent_size(struct wl_client *cl, struct wl_resource *r, int32_t w, int32_t h)
{ (void)cl; (void)r; (void)w; (void)h; }
static void pos_set_parent_configure(struct wl_client *cl, struct wl_resource *r, uint32_t s)
{ (void)cl; (void)r; (void)s; }
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = noop_destroy, .set_size = pos_set_size,
    .set_anchor_rect = pos_set_anchor_rect, .set_anchor = pos_set_anchor,
    .set_gravity = pos_set_gravity, .set_constraint_adjustment = pos_set_constraint,
    .set_offset = pos_set_offset, .set_reactive = pos_set_reactive,
    .set_parent_size = pos_set_parent_size,
    .set_parent_configure = pos_set_parent_configure,
};
static void positioner_free(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

/* Place a popup relative to its parent. Menus are the one piece of UI
 * where being a few pixels wrong is instantly visible, and being off
 * the bottom of the screen makes a browser unusable rather than ugly,
 * so the sliding correction is not optional garnish. */
static void positioner_resolve(const positioner *p, aurwl_win *parent, aurwl *c,
                               int *out_x, int *out_y)
{
    int x = p->ax, y = p->ay;
    uint32_t a = p->anchor;
    if (a == XDG_POSITIONER_ANCHOR_TOP || a == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
        a == XDG_POSITIONER_ANCHOR_TOP_RIGHT) y = p->ay;
    else if (a == XDG_POSITIONER_ANCHOR_BOTTOM || a == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT ||
             a == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) y = p->ay + p->ah;
    else y = p->ay + p->ah / 2;

    if (a == XDG_POSITIONER_ANCHOR_LEFT || a == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
        a == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT) x = p->ax;
    else if (a == XDG_POSITIONER_ANCHOR_RIGHT || a == XDG_POSITIONER_ANCHOR_TOP_RIGHT ||
             a == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) x = p->ax + p->aw;
    else x = p->ax + p->aw / 2;

    uint32_t g = p->gravity;
    if (g == XDG_POSITIONER_GRAVITY_LEFT || g == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
        g == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT) x -= p->w;
    else if (!(g == XDG_POSITIONER_GRAVITY_RIGHT || g == XDG_POSITIONER_GRAVITY_TOP_RIGHT ||
               g == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT)) x -= p->w / 2;

    if (g == XDG_POSITIONER_GRAVITY_TOP || g == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
        g == XDG_POSITIONER_GRAVITY_TOP_RIGHT) y -= p->h;
    else if (!(g == XDG_POSITIONER_GRAVITY_BOTTOM || g == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT ||
               g == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT)) y -= p->h / 2;

    x += p->ox; y += p->oy;

    /* Keep it on screen. The parent's own position is the shell's
     * business, so we correct against the output and let the shell's
     * clipping take care of the rest. */
    int px = parent ? parent->geo_x : 0, py = parent ? parent->geo_y : 0;
    int sx = px + x, sy = py + y;
    if (sx + p->w > c->ow) sx = c->ow - p->w;
    if (sy + p->h > c->oh) sy = c->oh - p->h;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    *out_x = sx - px; *out_y = sy - py;
}

/* ── xdg_toplevel ───────────────────────────────────────────────── */

static void configure_toplevel(aurwl_win *w)
{
    /* Both, separately. A client may destroy the xdg_surface and keep
     * the xdg_toplevel; the window then stays mapped and in the list,
     * so the shell configures it every frame, and
     * xdg_surface_send_configure(NULL) dereferences its client. That
     * crashes with no further help from the client at all. */
    if (!w->xdg_toplevel || !w->xdg_surface) return;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *st;
    if (w->want_act)  { st = wl_array_add(&states, 4); if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED; }
    if (w->want_max)  { st = wl_array_add(&states, 4); if (st) *st = XDG_TOPLEVEL_STATE_MAXIMIZED; }
    if (w->want_full) { st = wl_array_add(&states, 4); if (st) *st = XDG_TOPLEVEL_STATE_FULLSCREEN; }
    xdg_toplevel_send_configure(w->xdg_toplevel, w->want_w, w->want_h, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(w->xdg_surface, serial_of(w->c));
}

static void tl_set_parent(struct wl_client *cl, struct wl_resource *r, struct wl_resource *p)
{ (void)cl; (void)r; (void)p; }
/* Every handler below can be reached after its surface is gone: the
 * resource outlives the surface for as long as it takes libwayland to
 * work through a dead client's object list, and we disarm it rather
 * than destroy it. So `w` being NULL is ordinary, not exceptional. */
static void tl_set_title(struct wl_client *cl, struct wl_resource *r, const char *t)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return;
  snprintf(w->title, sizeof w->title, "%s", t ? t : ""); }
static void tl_set_app_id(struct wl_client *cl, struct wl_resource *r, const char *a)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return;
  snprintf(w->app_id, sizeof w->app_id, "%s", a ? a : ""); }
static void tl_show_menu(struct wl_client *cl, struct wl_resource *r, struct wl_resource *s,
                         uint32_t ser, int32_t x, int32_t y)
{ (void)cl; (void)r; (void)s; (void)ser; (void)x; (void)y; }
static void tl_move(struct wl_client *cl, struct wl_resource *r, struct wl_resource *s, uint32_t ser)
{ (void)cl; (void)r; (void)s; (void)ser; }
static void tl_resize(struct wl_client *cl, struct wl_resource *r, struct wl_resource *s,
                      uint32_t ser, uint32_t edges)
{ (void)cl; (void)r; (void)s; (void)ser; (void)edges; }
static void tl_set_max(struct wl_client *cl, struct wl_resource *r, int32_t w_, int32_t h_)
{ (void)cl; (void)r; (void)w_; (void)h_; }
static void tl_set_min(struct wl_client *cl, struct wl_resource *r, int32_t w_, int32_t h_)
{ (void)cl; (void)r; (void)w_; (void)h_; }
static void tl_maximize(struct wl_client *cl, struct wl_resource *r)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return; w->want_max = 1; configure_toplevel(w); }
static void tl_unmaximize(struct wl_client *cl, struct wl_resource *r)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return; w->want_max = 0; configure_toplevel(w); }
static void tl_fullscreen(struct wl_client *cl, struct wl_resource *r, struct wl_resource *o)
{ (void)cl; (void)o; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return; w->want_full = 1; configure_toplevel(w); }
static void tl_unfullscreen(struct wl_client *cl, struct wl_resource *r)
{ (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return; w->want_full = 0; configure_toplevel(w); }
static void tl_minimize(struct wl_client *cl, struct wl_resource *r) { (void)cl; (void)r; }

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = noop_destroy, .set_parent = tl_set_parent, .set_title = tl_set_title,
    .set_app_id = tl_set_app_id, .show_window_menu = tl_show_menu, .move = tl_move,
    .resize = tl_resize, .set_max_size = tl_set_max, .set_min_size = tl_set_min,
    .set_maximized = tl_maximize, .unset_maximized = tl_unmaximize,
    .set_fullscreen = tl_fullscreen, .unset_fullscreen = tl_unfullscreen,
    .set_minimized = tl_minimize,
};
static void toplevel_gone(struct wl_resource *r)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    if (!w) return;
    if (w->xdg_toplevel != r) return;      /* a stale duplicate */
    w->xdg_toplevel = NULL; w->role = ROLE_NONE; w->acked = 0;
    if (w->mapped) surface_unmap(w);
}

/* ── xdg_popup ──────────────────────────────────────────────────── */

static void pop_grab(struct wl_client *cl, struct wl_resource *r, struct wl_resource *seat, uint32_t s)
{ (void)cl; (void)r; (void)seat; (void)s; }
static void pop_reposition(struct wl_client *cl, struct wl_resource *r,
                           struct wl_resource *pos_res, uint32_t token)
{
    (void)cl;
    aurwl_win *w = wl_resource_get_user_data(r);
    positioner *p = wl_resource_get_user_data(pos_res);
    if (!w || !p) return;
    if (!w->xdg_surface) return;
    positioner_resolve(p, w->parent, w->c, &w->px, &w->py);
    w->want_w = p->w; w->want_h = p->h;
    xdg_popup_send_repositioned(r, token);
    xdg_popup_send_configure(r, w->px, w->py, p->w, p->h);
    xdg_surface_send_configure(w->xdg_surface, serial_of(w->c));
}
static const struct xdg_popup_interface popup_impl = {
    .destroy = noop_destroy, .grab = pop_grab, .reposition = pop_reposition,
};
static void popup_gone(struct wl_resource *r)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    if (!w) return;
    if (w->xdg_popup != r) return;
    w->xdg_popup = NULL; w->role = ROLE_NONE; w->acked = 0;
    if (w->parent) { wl_list_remove(&w->child_link); wl_list_init(&w->child_link); w->parent = NULL; }
    if (w->mapped) surface_unmap(w);
}

/* ── xdg_surface ────────────────────────────────────────────────── */

static void xs_get_toplevel(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    if (!w) { wl_resource_post_error(r, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "surface is gone"); return; }
    /* One role object per surface, which the spec requires anyway. The
     * surface stores a single pointer to each kind, so a second one is
     * invisible to the disarming in surface_free_res() -- it stays
     * armed with a pointer to the freed window, and its handlers then
     * write attacker-chosen bytes into the freed chunk. set_title alone
     * is 127 of them at a fixed offset. */
    if (w->xdg_toplevel || w->xdg_popup) {
        wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
                               "this surface already has a role");
        return;
    }
    struct wl_resource *tl = wl_resource_create(cl, &xdg_toplevel_interface,
                                                wl_resource_get_version(r), id);
    if (!tl) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(tl, &toplevel_impl, w, toplevel_gone);
    w->xdg_toplevel = tl;
    w->role = ROLE_TOPLEVEL;

    /* Version 5 says this must arrive before the first configure. What
     * goes in it is what we actually do: maximize and fullscreen are
     * handled; the window menu is not, and minimising is the shell's
     * own idea rather than something a client can ask for, so claiming
     * either would put a button in the client's title bar that does
     * nothing. */
    if (wl_resource_get_version(tl) >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
        struct wl_array caps; wl_array_init(&caps);
        uint32_t *cp;
        cp = wl_array_add(&caps, 4); if (cp) *cp = XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
        cp = wl_array_add(&caps, 4); if (cp) *cp = XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
        xdg_toplevel_send_wm_capabilities(tl, &caps);
        wl_array_release(&caps);
    }
}
static void xs_get_popup(struct wl_client *cl, struct wl_resource *r, uint32_t id,
                         struct wl_resource *parent_res, struct wl_resource *pos_res)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    positioner *p = pos_res ? wl_resource_get_user_data(pos_res) : NULL;
    if (!w || !p) { wl_resource_post_error(r, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                               "surface or positioner is gone"); return; }
    if (w->xdg_toplevel || w->xdg_popup) {
        wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
                               "this surface already has a role");
        return;
    }
    struct wl_resource *pr = wl_resource_create(cl, &xdg_popup_interface,
                                                wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(pr, &popup_impl, w, popup_gone);
    w->xdg_popup = pr;
    w->role = ROLE_POPUP;

    aurwl_win *parent = NULL;
    if (parent_res) {
        aurwl_win *pw = wl_resource_get_user_data(parent_res);   /* an xdg_surface */
        parent = pw;
    }
    if (parent) {
        if (parent_loop(parent, w)) {
            wl_resource_post_error(r, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                   "popup parent loop");
            return;
        }
        wl_list_remove(&w->child_link);
        w->parent = parent;
        wl_list_insert(parent->children.prev, &w->child_link);
    }
    positioner_resolve(p, parent, w->c, &w->px, &w->py);
    w->want_w = p->w; w->want_h = p->h;
    xdg_popup_send_configure(pr, w->px, w->py, p->w, p->h);
}
static void xs_set_geometry(struct wl_client *cl, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t ww, int32_t hh)
{
    (void)cl; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return;
    /* The visible window is usually smaller than the buffer: a toolkit
     * draws its own shadow into the margin. Without this the shell would
     * lay windows out by their shadows and everything would look loose
     * by a dozen pixels on every edge. */
    w->geo_w = ww; w->geo_h = hh; w->has_geo = (ww > 0 && hh > 0);
    w->geo_x = x;  w->geo_y = y;
}
static void xs_ack(struct wl_client *cl, struct wl_resource *r, uint32_t serial)
{ (void)cl; (void)serial; aurwl_win *w = wl_resource_get_user_data(r); if (!w) return; w->acked = 1; }

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = noop_destroy, .get_toplevel = xs_get_toplevel, .get_popup = xs_get_popup,
    .set_window_geometry = xs_set_geometry, .ack_configure = xs_ack,
};
static void xdg_surface_gone(struct wl_resource *r)
{
    aurwl_win *w = wl_resource_get_user_data(r);
    if (w && w->xdg_surface == r) w->xdg_surface = NULL;
}

/* ── xdg_wm_base ────────────────────────────────────────────────── */

static void wm_create_positioner(struct wl_client *cl, struct wl_resource *r, uint32_t id)
{
    (void)r;
    positioner *p = calloc(1, sizeof *p);
    if (!p) { wl_client_post_no_memory(cl); return; }
    struct wl_resource *res = wl_resource_create(cl, &xdg_positioner_interface,
                                                 wl_resource_get_version(r), id);
    if (!res) { free(p); wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(res, &positioner_impl, p, positioner_free);
}
static void wm_get_xdg_surface(struct wl_client *cl, struct wl_resource *r, uint32_t id,
                               struct wl_resource *surf)
{
    aurwl_win *w = surf ? wl_resource_get_user_data(surf) : NULL;
    if (!w) { wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE, "no such surface"); return; }
    if (w->xdg_surface) {
        wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
                               "this surface already has an xdg_surface");
        return;
    }
    struct wl_resource *xs = wl_resource_create(cl, &xdg_surface_interface,
                                                wl_resource_get_version(r), id);
    if (!xs) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(xs, &xdg_surface_impl, w, xdg_surface_gone);
    w->xdg_surface = xs;
    w->acked = 0;
}
static void wm_pong(struct wl_client *cl, struct wl_resource *r, uint32_t serial)
{ (void)cl; (void)r; (void)serial; }
static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = noop_destroy, .create_positioner = wm_create_positioner,
    .get_xdg_surface = wm_get_xdg_surface, .pong = wm_pong,
};
static void bind_wm_base(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(cl, &xdg_wm_base_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &wm_base_impl, data, NULL);
}

/* ── xdg-decoration ─────────────────────────────────────────────── */
/* We ask every client to let us draw its title bar. That is not a
 * performance argument -- it is the whole product argument. A desktop
 * where the browser's title bar is GTK's and the file manager's is
 * Qt's is a desktop assembled from parts; the shell's own chrome, on
 * every window, is what makes an AurOS theme mean something. Clients
 * that insist on drawing their own are not fought with. */

/* A decoration configure is state the client must acknowledge, and the
 * serial to acknowledge it with comes from xdg_surface.configure. Sent
 * on its own, the client was handed something to ack and no way to ack
 * it -- toolkits latch the decoration mode inside their xdg_surface
 * configure handler, so a set_mode after mapping did not take effect
 * until some unrelated later configure happened to arrive. Two title
 * bars, or none. */
static void deco_answer(struct wl_resource *r)
{
    zxdg_toplevel_decoration_v1_send_configure(r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    aurwl_win *w = wl_resource_get_user_data(r);
    if (w && w->xdg_surface) xdg_surface_send_configure(w->xdg_surface, serial_of(w->c));
}
static void deco_set_mode(struct wl_client *cl, struct wl_resource *r, uint32_t mode)
{ (void)cl; (void)mode; deco_answer(r); }
static void deco_unset_mode(struct wl_client *cl, struct wl_resource *r)
{ (void)cl; deco_answer(r); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    .destroy = noop_destroy, .set_mode = deco_set_mode, .unset_mode = deco_unset_mode,
};
static void deco_gone(struct wl_resource *r)
{ aurwl_win *w = wl_resource_get_user_data(r); if (w && w->decoration == r) w->decoration = NULL; }

static void decomgr_get(struct wl_client *cl, struct wl_resource *r, uint32_t id,
                        struct wl_resource *tl)
{
    aurwl_win *w = tl ? wl_resource_get_user_data(tl) : NULL;
    if (w && w->decoration) { wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                               "already decorated"); return; }
    struct wl_resource *d = wl_resource_create(cl, &zxdg_toplevel_decoration_v1_interface, 1, id);
    if (!d) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(d, &deco_impl, w, deco_gone);
    if (w) w->decoration = d;
    /* At construction the client has not made its initial commit yet,
     * so the configure that answers this one is the one that handshake
     * produces. Sending another here would be a second, unanswerable
     * sequence. */
    zxdg_toplevel_decoration_v1_send_configure(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static const struct zxdg_decoration_manager_v1_interface decomgr_impl = {
    .destroy = noop_destroy, .get_toplevel_decoration = decomgr_get,
};
static void bind_deco(struct wl_client *cl, void *data, uint32_t ver, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(cl, &zxdg_decoration_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(cl); return; }
    wl_resource_set_implementation(r, &decomgr_impl, data, NULL);
}

/* ── keymap ─────────────────────────────────────────────────────── */

/* The client does its own key translation from a keymap we hand it.
 * That indirection is the only reason an application we did not write
 * can be typed into in Greek, Dvorak or Hungarian: we never decide what
 * a key means, we only say which key moved. */
static int build_keymap(aurwl *c)
{
    c->keymap_fd = -1;
    c->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!c->xkb) return -1;

    struct xkb_rule_names names = {0};
    names.layout  = getenv("AUROS_KB_LAYOUT");
    names.variant = getenv("AUROS_KB_VARIANT");
    names.options = getenv("AUROS_KB_OPTIONS");
    c->keymap = xkb_keymap_new_from_names(c->xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!c->keymap) return -1;
    c->xkb_state = xkb_state_new(c->keymap);
    if (!c->xkb_state) return -1;

    char *str = xkb_keymap_get_as_string(c->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) return -1;
    size_t len = strlen(str) + 1;

    int fd = memfd_create("aurwl-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) { free(str); return -1; }
    if (write(fd, str, len) != (ssize_t)len) { close(fd); free(str); return -1; }
    free(str);
    /* Sealed so a client cannot rewrite the keymap every other client
     * is reading from the same descriptor. */
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    c->keymap_fd = fd;
    c->keymap_size = len;
    return 0;
}

/* ── construction ───────────────────────────────────────────────── */

aurwl *aurwl_create(int w, int h, int refresh_mhz)
{
    aurwl *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ow = w > 0 ? w : 1280;
    c->oh = h > 0 ? h : 720;
    c->refresh_mhz = refresh_mhz > 0 ? refresh_mhz : 60000;
    c->keymap_fd = -1;

    wl_list_init(&c->surfaces);  wl_list_init(&c->seats);
    wl_list_init(&c->pointers);  wl_list_init(&c->keyboards);
    wl_list_init(&c->touches);   wl_list_init(&c->outputs);
    wl_list_init(&c->devices);
    wl_list_init(&c->offers);
    wl_list_init(&c->xdg_outputs);

    c->display = wl_display_create();
    if (!c->display) { free(c); return NULL; }
    c->loop = wl_display_get_event_loop(c->display);

    /* libwayland's own wl_shm: the pool bookkeeping, the SIGBUS guard
     * and the format negotiation are exactly the parts where writing
     * our own would be original in no useful way. */
    if (wl_display_init_shm(c->display) < 0) { aurwl_destroy(c); return NULL; }

    c->g_compositor    = wl_global_create(c->display, &wl_compositor_interface, 5, c, bind_compositor);
    c->g_subcompositor = wl_global_create(c->display, &wl_subcompositor_interface, 1, c, bind_subcompositor);
    c->g_seat          = wl_global_create(c->display, &wl_seat_interface, 7, c, bind_seat);
    c->g_output        = wl_global_create(c->display, &wl_output_interface, 4, c, bind_output);
    c->g_xdg           = wl_global_create(c->display, &xdg_wm_base_interface, 5, c, bind_wm_base);
    c->g_ddm           = wl_global_create(c->display, &wl_data_device_manager_interface, 3, c, bind_ddm);
    c->g_deco          = wl_global_create(c->display, &zxdg_decoration_manager_v1_interface, 1, c, bind_deco);
    c->g_viewporter    = wl_global_create(c->display, &wp_viewporter_interface, 1, c, bind_viewporter);
    c->g_xdg_output    = wl_global_create(c->display, &zxdg_output_manager_v1_interface, 3, c, bind_xdg_output);

    if (!c->g_compositor || !c->g_subcompositor || !c->g_seat || !c->g_output ||
        !c->g_xdg || !c->g_ddm || !c->g_deco || !c->g_viewporter || !c->g_xdg_output) {
        fprintf(stderr, "aurwl: could not create globals\n");
        aurwl_destroy(c); return NULL;
    }

    if (build_keymap(c) < 0)
        fprintf(stderr, "aurwl: no xkb keymap; clients will get no key translation\n");

    if (c->keymap_fd < 0) {
        /* An empty sealed memfd rather than nothing: see the comment in
         * seat_get_keyboard about what -1 costs. */
        c->keymap_fd = memfd_create("aurwl-nokeymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (c->keymap_fd >= 0)
            fcntl(c->keymap_fd, F_ADD_SEALS,
                  F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
        c->keymap_size = 0;
    }

    c->socket = wl_display_add_socket_auto(c->display);
    if (!c->socket) {
        fprintf(stderr, "aurwl: no socket in XDG_RUNTIME_DIR (%s)\n",
                getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "unset");
        aurwl_destroy(c); return NULL;
    }
    /* A client that dies mid-write would otherwise take the compositor
     * with it, and with it every other application on the machine. */
    signal(SIGPIPE, SIG_IGN);
    return c;
}

void aurwl_destroy(aurwl *c)
{
    if (!c) return;
    if (c->xkb_state) xkb_state_unref(c->xkb_state);
    if (c->keymap)    xkb_keymap_unref(c->keymap);
    if (c->xkb)       xkb_context_unref(c->xkb);
    if (c->keymap_fd >= 0) close(c->keymap_fd);
    if (c->display) {
        /* wl_display_destroy() does not tear down connected clients, so
         * every surface, buffer and store they still hold is simply
         * abandoned. On process exit that is only untidy -- but it also
         * means the shutdown path never exercises the destructors, and
         * a destructor that is never run on a normal exit is one nobody
         * notices is broken. */
        wl_display_destroy_clients(c->display);
        wl_display_destroy(c->display);
    }
    free(c);
}

const char *aurwl_socket(const aurwl *c) { return c ? c->socket : NULL; }
uint32_t aurwl_damage_seq(const aurwl *c) { return c ? c->damage_seq : 0; }
int aurwl_fd(const aurwl *c) { return c ? wl_event_loop_get_fd(c->loop) : -1; }

void aurwl_dispatch(aurwl *c)
{
    if (!c) return;
    wl_event_loop_dispatch(c->loop, 0);
    wl_display_flush_clients(c->display);
}

void aurwl_resize_output(aurwl *c, int w, int h)
{
    if (!c || (w == c->ow && h == c->oh)) return;
    c->ow = w; c->oh = h;
    FOR_EACH_RES(r, &c->outputs) output_send(c, r);
    FOR_EACH_RES(x, &c->xdg_outputs) xdg_output_send(c, x);
}

/* ── the window list ────────────────────────────────────────────── */

static int win_visible(const aurwl_win *w)
{ return w->mapped && !w->dead && (w->role == ROLE_TOPLEVEL || w->role == ROLE_POPUP); }

int aurwl_window_count(const aurwl *c)
{
    if (!c) return 0;
    int n = 0; aurwl_win *w;
    wl_list_for_each(w, &c->surfaces, link) if (win_visible(w)) n++;
    return n;
}
aurwl_win *aurwl_window_at(const aurwl *c, int i)
{
    if (!c || i < 0) return NULL;
    aurwl_win *w;
    wl_list_for_each(w, &c->surfaces, link) if (win_visible(w) && i-- == 0) return w;
    return NULL;
}
uint32_t    aurwl_win_id(const aurwl_win *w)     { return w ? w->id : 0; }
const char *aurwl_win_title(const aurwl_win *w)  { return w ? w->title : ""; }
const char *aurwl_win_app_id(const aurwl_win *w) { return w ? w->app_id : ""; }
surface    *aurwl_win_content(aurwl_win *w)      { return w ? w->store : NULL; }
int         aurwl_win_is_popup(const aurwl_win *w) { return w && w->role == ROLE_POPUP; }
aurwl_win  *aurwl_win_parent(const aurwl_win *w) { return w ? w->parent : NULL; }

void aurwl_win_popup_offset(const aurwl_win *w, int *x, int *y)
{ if (x) *x = w ? w->px : 0; if (y) *y = w ? w->py : 0; }

void aurwl_win_pref_size(const aurwl_win *w, int *w_out, int *h_out)
{
    if (!w) { if (w_out) *w_out = 0; if (h_out) *h_out = 0; return; }
    /* Window geometry when the client gave one, because that is the
     * window without its shadow; the buffer otherwise. */
    if (w_out) *w_out = w->has_geo ? w->geo_w : w->cw;
    if (h_out) *h_out = w->has_geo ? w->geo_h : w->ch;
}

void aurwl_win_configure(aurwl_win *w, int width, int height,
                         int activated, int maximized, int fullscreen)
{
    if (!w || !w->xdg_toplevel) return;
    if (width  < 0) width  = 0;
    if (height < 0) height = 0;
    /* Re-sending an identical configure makes some toolkits redraw and
     * re-ack forever, which reads as a window that flickers and a CPU
     * that never idles. Dropping the duplicate is what makes calling
     * this every frame safe, which is what the shell wants to do. */
    if (w->want_w == width && w->want_h == height && w->want_act == activated &&
        w->want_max == maximized && w->want_full == fullscreen && w->acked)
        return;
    w->want_w = width; w->want_h = height;
    w->want_act = activated; w->want_max = maximized; w->want_full = fullscreen;
    configure_toplevel(w);
}

void aurwl_win_close(aurwl_win *w)
{
    if (!w) return;
    if (w->xdg_toplevel) xdg_toplevel_send_close(w->xdg_toplevel);
    else if (w->xdg_popup) xdg_popup_send_popup_done(w->xdg_popup);
}
void aurwl_win_kill(aurwl_win *w)
{
    if (!w || !w->res) return;
    /* Last resort, and it takes the whole client: a browser is one
     * process for many windows, so this is the "it is wedged and the
     * user is stuck" button, not the close button. */
    wl_client_destroy(wl_resource_get_client(w->res));
}

/* ── input ──────────────────────────────────────────────────────── */

static struct wl_client *win_client(aurwl_win *w)
{ return (w && w->res) ? wl_resource_get_client(w->res) : NULL; }

static void send_modifiers(aurwl *c)
{
    if (!c->xkb_state) return;
    uint32_t dep = xkb_state_serialize_mods(c->xkb_state, XKB_STATE_MODS_DEPRESSED);
    uint32_t lat = xkb_state_serialize_mods(c->xkb_state, XKB_STATE_MODS_LATCHED);
    uint32_t lck = xkb_state_serialize_mods(c->xkb_state, XKB_STATE_MODS_LOCKED);
    uint32_t grp = xkb_state_serialize_layout(c->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    uint32_t ser = serial_of(c);
    /* To the focused client only. With no focus this used to go to
     * everyone, telling every background process the live Shift, Ctrl
     * and Alt state of a keyboard none of them was reading. */
    struct wl_client *target = win_client(c->focus);
    if (!target) return;
    FOR_EACH_RES(k, &c->keyboards)
        if (wl_resource_get_client(k) == target)
            wl_keyboard_send_modifiers(k, ser, dep, lat, lck, grp);
}

void aurwl_update_modifiers(aurwl *c) { if (c) send_modifiers(c); }

void aurwl_set_focus(aurwl *c, aurwl_win *w)
{
    if (!c || c->focus == w) return;
    uint32_t ser = serial_of(c);

    if (c->focus) {
        struct wl_client *old = win_client(c->focus);
        FOR_EACH_RES(k, &c->keyboards)
            if (wl_resource_get_client(k) == old)
                wl_keyboard_send_leave(k, ser, c->focus->res);
    }
    c->focus = w;
    if (!w) return;

    struct wl_client *nw = win_client(w);
    struct wl_array keys; wl_array_init(&keys);          /* nothing held */
    FOR_EACH_RES(k, &c->keyboards)
        if (wl_resource_get_client(k) == nw)
            wl_keyboard_send_enter(k, ser, w->res, &keys);
    wl_array_release(&keys);
    send_modifiers(c);

    /* A newly focused client must learn what is on the clipboard before
     * its paste menu is drawn, not after the user tries to use it. */
    FOR_EACH_RES(dev, &c->devices)
        if (wl_resource_get_client(dev) == nw) send_selection_to(c, dev);
}

aurwl_win *aurwl_focus(const aurwl *c) { return c ? c->focus : NULL; }

void aurwl_pointer_motion(aurwl *c, aurwl_win *w, int sx, int sy, uint32_t t)
{
    if (!c) return;
    if (w && !win_visible(w)) w = NULL;

    if (c->ptr_focus != w) {
        if (c->ptr_focus) {
            struct wl_client *old = win_client(c->ptr_focus);
            uint32_t ser = serial_of(c);
            FOR_EACH_RES(p, &c->pointers)
                if (wl_resource_get_client(p) == old) {
                    wl_pointer_send_leave(p, ser, c->ptr_focus->res);
                    if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                        wl_pointer_send_frame(p);
                }
        }
        c->ptr_focus = w;
        if (w) {
            struct wl_client *nw = win_client(w);
            uint32_t ser = serial_of(c);
            FOR_EACH_RES(p, &c->pointers)
                if (wl_resource_get_client(p) == nw) {
                    wl_pointer_send_enter(p, ser, w->res,
                                          wl_fixed_from_int(sx), wl_fixed_from_int(sy));
                    if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                        wl_pointer_send_frame(p);
                }
        }
    }
    c->ptr_x = sx; c->ptr_y = sy;
    if (!w) return;
    struct wl_client *cl = win_client(w);
    FOR_EACH_RES(p, &c->pointers)
        if (wl_resource_get_client(p) == cl) {
            wl_pointer_send_motion(p, t, wl_fixed_from_int(sx), wl_fixed_from_int(sy));
            if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                wl_pointer_send_frame(p);
        }
}

void aurwl_pointer_button(aurwl *c, uint32_t button, int pressed, uint32_t t)
{
    if (!c || !c->ptr_focus) return;
    struct wl_client *cl = win_client(c->ptr_focus);
    uint32_t ser = serial_of(c);
    FOR_EACH_RES(p, &c->pointers)
        if (wl_resource_get_client(p) == cl) {
            wl_pointer_send_button(p, ser, t, button,
                pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                wl_pointer_send_frame(p);
        }
}

void aurwl_pointer_axis(aurwl *c, int horizontal, double step, uint32_t t)
{
    if (!c || !c->ptr_focus) return;
    struct wl_client *cl = win_client(c->ptr_focus);
    uint32_t axis = horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
                               : WL_POINTER_AXIS_VERTICAL_SCROLL;
    FOR_EACH_RES(p, &c->pointers)
        if (wl_resource_get_client(p) == cl) {
            int ver = wl_resource_get_version(p);
            if (ver >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
                wl_pointer_send_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
            /* A wheel notch is 15 degrees, which the protocol expresses
             * as 10 units of scroll; a toolkit that only reads discrete
             * steps ignores the fixed value entirely, so both go out. */
            if (ver >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                wl_pointer_send_axis_discrete(p, axis, step > 0 ? 1 : -1);
            wl_pointer_send_axis(p, t, axis, wl_fixed_from_double(step * 10.0));
            if (ver >= WL_POINTER_FRAME_SINCE_VERSION) wl_pointer_send_frame(p);
        }
}

int aurwl_key(aurwl *c, uint32_t code, int pressed, uint32_t t)
{
    if (!c) return 0;
    /* Modifier state is tracked whether or not anyone is listening: the
     * shell swallows some chords itself, and if those did not update
     * xkb the client's idea of Shift would drift from the user's hand. */
    if (c->xkb_state)
        xkb_state_update_key(c->xkb_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    if (!c->focus) return 0;
    struct wl_client *cl = win_client(c->focus);
    uint32_t ser = serial_of(c);
    int sent = 0;
    FOR_EACH_RES(k, &c->keyboards)
        if (wl_resource_get_client(k) == cl) {
            wl_keyboard_send_key(k, ser, t, code,
                pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
            sent = 1;
        }
    if (sent) send_modifiers(c);
    return sent;
}

/* ── frame ──────────────────────────────────────────────────────── */

void aurwl_frame_done(aurwl *c, uint32_t t)
{
    if (!c) return;
    aurwl_win *w;
    wl_list_for_each(w, &c->surfaces, link) {
        struct wl_resource *cb, *tmp;
        wl_resource_for_each_safe(cb, tmp, &w->frame_cbs) {
            wl_callback_send_done(cb, t);
            wl_resource_destroy(cb);
        }
    }
    wl_display_flush_clients(c->display);
}

/* ── launching ──────────────────────────────────────────────────── */

pid_t aurwl_spawn(aurwl *c, const char *cmdline)
{
    if (!c || !cmdline || !*cmdline) return -1;
    if (c->n_kids >= (int)(sizeof c->kids / sizeof c->kids[0])) aurwl_reap(c);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* A new session, so a crashing application cannot take the
         * shell's controlling terminal down with it, and so a Ctrl-C
         * meant for one program is not delivered to the desktop. */
        setsid();
        signal(SIGPIPE, SIG_DFL);
        setenv("WAYLAND_DISPLAY", c->socket, 1);
        unsetenv("DISPLAY");
        /* Toolkits pick a backend from the environment before they look
         * at what is actually available, and the guesses they make on a
         * machine with no X server are all wrong in different ways. */
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        setenv("CLUTTER_BACKEND", "wayland", 1);
        setenv("MOZ_ENABLE_WAYLAND", "1", 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);
        setenv("XDG_CURRENT_DESKTOP", "AurOS", 1);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    if (c->n_kids < (int)(sizeof c->kids / sizeof c->kids[0])) c->kids[c->n_kids++] = pid;
    return pid;
}

void aurwl_reap(aurwl *c)
{
    if (!c) return;
    pid_t p;
    while ((p = waitpid(-1, NULL, WNOHANG)) > 0) {
        for (int i = 0; i < c->n_kids; i++)
            if (c->kids[i] == p) { c->kids[i] = c->kids[--c->n_kids]; break; }
    }
}
