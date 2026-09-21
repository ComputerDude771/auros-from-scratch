/* wlhostile.c — the compositor, against clients that are trying to break it.
 *
 * A Wayland client is untrusted. It can send any request, in any order,
 * with any arguments, and the compositor it is talking to is the most
 * privileged process on the machine: it owns the display, it reads every
 * keystroke, and when it dies every application dies with it.
 *
 * The polite tests answer "does a browser work". wlstress.sh answers
 * "does a crashing application take the desktop with it". This answers
 * the one that decides whether the machine can be trusted with a
 * program the user downloaded: when a client deliberately misbehaves,
 * does the compositor refuse it and carry on?
 *
 * Every case here was a real defect, found by adversarial review and
 * reproduced before it was fixed. Two were four-request crashes. One
 * wrote 127 bytes of the client's choosing into freed memory.
 *
 * Passing means: the compositor stayed up, and afterwards it still
 * served a normal client. Refusing the hostile client -- disconnecting
 * it with a protocol error -- is the correct response, not a failure.
 *
 *   wlhostile            run every case
 *   wlhostile N          run case N only
 *   wlhostile --client N  (internal) be the hostile client
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>

#include <wayland-client.h>
#include "xdg-shell-client.h"

#include "../src/aurwl/aurwl.h"

/* ── the hostile client ─────────────────────────────────────────── */

struct cl {
    struct wl_display    *dpy;
    struct wl_registry   *reg;
    struct wl_compositor *comp;
    struct wl_subcompositor *subcomp;
    struct xdg_wm_base   *wm;
    struct wl_shm        *shm;
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
    struct cl *c = d;
    if (!strcmp(iface, "wl_compositor"))
        c->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, "wl_subcompositor"))
        c->subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, "wl_shm"))
        c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        c->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

/* Each case sends the shortest sequence that used to break something.
 * The client is expected to be disconnected; that is the point. */
static int be_hostile(int which)
{
    struct cl c = {0};
    c.dpy = wl_display_connect(NULL);
    if (!c.dpy) return 2;
    c.reg = wl_display_get_registry(c.dpy);
    wl_registry_add_listener(c.reg, &reg_l, &c);
    wl_display_roundtrip(c.dpy);
    if (!c.comp || !c.subcomp || !c.wm) return 2;
    xdg_wm_base_add_listener(c.wm, &wm_l, &c);

    struct wl_surface *A = wl_compositor_create_surface(c.comp);
    struct wl_surface *B = wl_compositor_create_surface(c.comp);

    switch (which) {
    case 1: {   /* its own surface as its own sibling, below */
        struct wl_subsurface *s = wl_subcompositor_get_subsurface(c.subcomp, A, B);
        wl_subsurface_place_below(s, A);
        break;
    }
    case 2: {   /* the same, above, then destroy the parent underneath it */
        struct wl_subsurface *s = wl_subcompositor_get_subsurface(c.subcomp, A, B);
        wl_subsurface_place_above(s, A);
        wl_display_roundtrip(c.dpy);
        wl_surface_destroy(B);
        wl_display_roundtrip(c.dpy);
        wl_subsurface_place_above(s, NULL);
        break;
    }
    case 3: {   /* two roles on one surface, then write through the stale one */
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, A);
        struct xdg_toplevel *t1 = xdg_surface_get_toplevel(xs);
        struct xdg_toplevel *t2 = xdg_surface_get_toplevel(xs);
        wl_display_roundtrip(c.dpy);
        wl_surface_destroy(A);
        wl_display_roundtrip(c.dpy);
        char big[200]; memset(big, 'A', sizeof big - 1); big[sizeof big - 1] = 0;
        xdg_toplevel_set_title(t1, big);
        xdg_toplevel_set_title(t2, big);
        break;
    }
    case 4: {   /* attach a buffer, destroy it, then commit */
        if (!c.shm) return 2;
        int fd = memfd_create("hostile", 0);
        if (fd < 0) return 2;
        size_t sz = 64 * 64 * 4;
        if (ftruncate(fd, (off_t)sz) < 0) { close(fd); return 2; }
        struct wl_shm_pool *pool = wl_shm_create_pool(c.shm, fd, (int32_t)sz);
        struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, 64, 64, 64 * 4,
                                                        WL_SHM_FORMAT_ARGB8888);
        wl_surface_attach(A, b, 0, 0);
        wl_surface_damage(A, 0, 0, 64, 64);
        wl_display_roundtrip(c.dpy);
        /* The buffer goes while the compositor still holds the pointer
         * from attach(). A synchronized subsurface makes an ordinary
         * toolkit do this without meaning any harm. */
        wl_buffer_destroy(b);
        wl_shm_pool_destroy(pool);
        close(fd);
        wl_display_roundtrip(c.dpy);
        wl_surface_commit(A);
        break;
    }
    case 5: {   /* destroy the xdg_surface, keep the toplevel, poke it */
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, A);
        struct xdg_toplevel *t = xdg_surface_get_toplevel(xs);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        xdg_surface_destroy(xs);
        xdg_toplevel_set_maximized(t);
        xdg_toplevel_set_title(t, "still here");
        break;
    }
    case 6: {   /* two popups naming each other, then a subsurface walk */
        struct xdg_surface *xa = xdg_wm_base_get_xdg_surface(c.wm, A);
        struct xdg_surface *xb = xdg_wm_base_get_xdg_surface(c.wm, B);
        struct xdg_positioner *p = xdg_wm_base_create_positioner(c.wm);
        xdg_positioner_set_size(p, 10, 10);
        xdg_positioner_set_anchor_rect(p, 0, 0, 10, 10);
        xdg_surface_get_popup(xa, xb, p);
        xdg_surface_get_popup(xb, xa, p);
        wl_display_roundtrip(c.dpy);
        struct wl_surface *C = wl_compositor_create_surface(c.comp);
        wl_subcompositor_get_subsurface(c.subcomp, C, A);
        break;
    }
    case 7: {   /* a very deep chain of synchronized subsurfaces */
        struct wl_surface *prev = A;
        for (int i = 0; i < 4000; i++) {
            struct wl_surface *s = wl_compositor_create_surface(c.comp);
            wl_subcompositor_get_subsurface(c.subcomp, s, prev);
            wl_surface_commit(s);
            prev = s;
            if ((i & 511) == 0 && wl_display_flush(c.dpy) < 0) break;
        }
        wl_surface_commit(A);
        break;
    }
    case 8: {   /* absurd damage rectangles */
        wl_surface_damage(A, -2147483647, -2147483647, 2147483647, 2147483647);
        wl_surface_damage_buffer(A, 2147483647, 2147483647, 2147483647, 2147483647);
        wl_surface_set_buffer_scale(A, 2147483647);
        wl_surface_commit(A);
        break;
    }
    default: return 2;
    }

    wl_display_roundtrip(c.dpy);
    wl_display_flush(c.dpy);
    wl_display_disconnect(c.dpy);
    return 0;
}

/* ── the harness ────────────────────────────────────────────────── */

static const char *CASES[] = {
    "",
    "a subsurface placed below itself",
    "a subsurface placed above itself, then orphaned",
    "two roles on one surface, written through after free",
    "a buffer destroyed between attach and commit",
    "an xdg_surface destroyed under a live toplevel",
    "two popups naming each other as parent",
    "four thousand nested synchronized subsurfaces",
    "damage rectangles at the limits of int32",
};
#define N_CASES ((int)(sizeof CASES / sizeof CASES[0]) - 1)

static uint32_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Run one case in its own compositor, then prove the compositor still
 * works by serving an ordinary client. */
static int run_case(const char *self, int which)
{
    aurwl *c = aurwl_create(800, 600, 60000);
    if (!c) { printf("  could not start a compositor\n"); return 1; }

    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s --client %d", self, which);
    if (aurwl_spawn(c, cmd) < 0) { aurwl_destroy(c); return 1; }

    uint32_t t0 = now_ms();
    while (now_ms() - t0 < 6000) {
        struct pollfd p = { aurwl_fd(c), POLLIN, 0 };
        poll(&p, 1, 16);
        aurwl_dispatch(c);
        aurwl_reap(c);
        aurwl_frame_done(c, now_ms());
    }

    /* The real test: does a well-behaved client still get a window? */
    if (aurwl_spawn(c, "weston-simple-shm") < 0) { aurwl_destroy(c); return 1; }
    int good = 0;
    t0 = now_ms();
    while (now_ms() - t0 < 8000 && !good) {
        struct pollfd p = { aurwl_fd(c), POLLIN, 0 };
        poll(&p, 1, 16);
        aurwl_dispatch(c);
        aurwl_reap(c);
        aurwl_frame_done(c, now_ms());
        for (int i = 0; i < aurwl_window_count(c); i++)
            if (aurwl_win_content(aurwl_window_at(c, i))) good = 1;
    }
    aurwl_destroy(c);
    return good ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--client")) return be_hostile(atoi(argv[2]));

    int only = (argc == 2) ? atoi(argv[1]) : 0;
    int bad = 0, ran = 0;
    for (int i = 1; i <= N_CASES; i++) {
        if (only && i != only) continue;
        printf("%d. %-52s ", i, CASES[i]);
        fflush(stdout);
        /* Each case gets its own process: a crash here must be reported
         * as a failed case, not end the run. */
        pid_t pid = fork();
        if (pid == 0) _exit(run_case(argv[0], i));
        int st = 0;
        waitpid(pid, &st, 0);
        ran++;
        if (WIFSIGNALED(st)) { printf("CRASH (signal %d)\n", WTERMSIG(st)); bad++; }
        else if (WEXITSTATUS(st) != 0) { printf("compositor stopped serving clients\n"); bad++; }
        else printf("refused, still serving\n");
    }
    printf("\n");
    if (bad) { printf("%d of %d hostile clients got through\n", bad, ran); return 1; }
    printf("%d/%d — the compositor refused every one and kept working\n", ran, ran);
    return 0;
}
