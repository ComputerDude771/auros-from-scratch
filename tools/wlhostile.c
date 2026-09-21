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
#include <fcntl.h>
#include <time.h>

#include <wayland-client.h>
#include "xdg-shell-client.h"
#include "viewporter-client.h"

#include "../src/aurwl/aurwl.h"

/* ── the hostile client ─────────────────────────────────────────── */

struct cl {
    struct wl_display    *dpy;
    struct wl_registry   *reg;
    struct wl_compositor *comp;
    struct wl_subcompositor *subcomp;
    struct xdg_wm_base   *wm;
    struct wl_shm        *shm;
    struct wp_viewporter *vper;
    struct wl_data_device_manager *ddm;
    struct wl_seat       *seat;
};

/* A one-pixel buffer. Enough to make a surface real, which several of
 * the cases below need before the thing they are testing means
 * anything -- the viewport resample runs on a committed buffer, and a
 * surface with no buffer is not mapped and cannot be a cursor. */
static struct wl_buffer *one_pixel(struct cl *c)
{
    if (!c->shm) return NULL;
    int fd = memfd_create("px", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, 4) < 0) { close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, 4);
    close(fd);
    if (!pool) return NULL;
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4,
                                                    WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return b;
}
static void attach_1x1(struct cl *c, struct wl_surface *s)
{
    struct wl_buffer *b = one_pixel(c);
    if (b) wl_surface_attach(s, b, 0, 0);
}

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
    else if (!strcmp(iface, "wp_viewporter"))
        c->vper = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    else if (!strcmp(iface, "wl_data_device_manager"))
        c->ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface,
                                  ver < 3 ? ver : 3);
    else if (!strcmp(iface, "wl_seat"))
        c->seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 7 ? ver : 7);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

/* Cases where the point is that the CLIENT lives. The marker is left
 * in XDG_RUNTIME_DIR, where both halves of this program can see it. */
static void marker_path(char *out, size_t n)
{
    const char *rd = getenv("XDG_RUNTIME_DIR");
    snprintf(out, n, "%s/wlhostile-client-survived", rd && *rd ? rd : "/tmp");
}
static void survived_marker(void)
{
    char p[512]; marker_path(p, sizeof p);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
}

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
    case 9: {
        /* THE ONE THAT USED TO FREEZE THE MACHINE. Six requests: a
         * viewport destination of 16384x16384 made the compositor
         * allocate a gibibyte and spend 9.4 seconds in one dispatch
         * doing bilinear maths over 268 million pixels -- with no
         * repaint, no cursor and no keystroke anywhere, for as long as
         * the client cared to keep committing. */
        if (!c.vper) return 2;
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, A);
        xdg_surface_get_toplevel(xs);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        struct wp_viewport *vp = wp_viewporter_get_viewport(c.vper, A);
        wp_viewport_set_destination(vp, 16384, 16384);
        attach_1x1(&c, A);
        wl_surface_damage(A, 0, 0, 1, 1);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        for (int i = 0; i < 40; i++) {
            attach_1x1(&c, A);
            wl_surface_damage(A, 0, 0, 1, 1);
            wl_surface_commit(A);
        }
        wl_display_flush(c.dpy);
        break;
    }
    case 10: {
        /* THE CLIENT MUST SURVIVE THIS ONE.
         *
         * Every other case here is a program doing something it should
         * be stopped for, so "the client was disconnected" is the pass.
         * This one is two ordinary copies from one window -- the most
         * unremarkable thing a person does all day -- and the bug was
         * that the COMPOSITOR killed the application. A harness that
         * only asks whether the compositor survived scores that as a
         * pass, which is how it went unnoticed. So this case leaves a
         * marker behind only if it reached the end with a live
         * connection, and run_case looks for it.
         *
         * It also has to be FOCUSED. An offer is only ever sent to the
         * client at the keyboard -- deliberately, so that a background
         * process cannot read what she copied -- so a hostile client
         * that never gets focus never gets an offer and the bug cannot
         * happen to it. The first version of this case proved nothing
         * for exactly that reason. */
        if (!c.ddm || !c.seat) return 2;
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, A);
        xdg_surface_get_toplevel(xs);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        attach_1x1(&c, A);
        wl_surface_damage(A, 0, 0, 1, 1);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);

        struct wl_data_device *dd =
            wl_data_device_manager_get_data_device(c.ddm, c.seat);
        int copies = 0;
        for (int i = 0; i < 60 && copies < 2; i++) {
            if (wl_display_roundtrip(c.dpy) < 0) break;
            /* Give the compositor time to notice the window and hand
             * it the keyboard before copying. */
            if (i == 20 || i == 40) {
                struct wl_data_source *src =
                    wl_data_device_manager_create_data_source(c.ddm);
                wl_data_source_offer(src, "text/plain");
                wl_data_device_set_selection(dd, src, 1);
                copies++;
            }
            struct timespec ts = { 0, 30 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        /* One more round trip before asking. The error from the second
         * copy is delivered on the NEXT exchange, and the loop above
         * ends on the copy itself -- so asking straight away reads
         * "no error" from a connection that is already dead, and the
         * check passes the very bug it is here to catch. */
        for (int i = 0; i < 5; i++) {
            if (wl_display_roundtrip(c.dpy) < 0) break;
            struct timespec ts = { 0, 30 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (copies == 2 && wl_display_get_error(c.dpy) == 0) survived_marker();
        break;
    }
    case 11:
        /* One mouse movement used to cost twelve milliseconds with a
         * hundred thousand of these on the list -- and the pointer
         * that stops working is hers, not this program's. */
        if (!c.seat) return 2;
        for (int i = 0; i < 100000; i++) wl_seat_get_pointer(c.seat);
        wl_display_roundtrip(c.dpy);
        break;
    case 12:
        for (int i = 0; i < 100000; i++) wl_compositor_create_surface(c.comp);
        wl_display_roundtrip(c.dpy);
        break;
    case 13: {
        /* A role is permanent. It was not: every role-object
         * destructor put it back to none, and the xdg checks looked at
         * resource pointers rather than at the role, so a subsurface
         * could become a window. */
        wl_subcompositor_get_subsurface(c.subcomp, A, B);
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, A);
        xdg_surface_get_toplevel(xs);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        break;
    }
    case 14: {
        /* ...and a mapped, focused window could be turned into a
         * cursor, which drops it out of the window list with no unmap
         * and no leave event, leaving the shell's slot for it standing
         * and the client believing its keys are still held. */
        if (!c.seat) return 2;
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, A);
        xdg_surface_get_toplevel(xs);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        attach_1x1(&c, A);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        struct wl_pointer *ptr = wl_seat_get_pointer(c.seat);
        wl_pointer_set_cursor(ptr, 1, A, 0, 0);
        wl_display_roundtrip(c.dpy);
        break;
    }
    case 15: {
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, A);
        xdg_surface_get_toplevel(xs);
        wl_surface_commit(A);
        wl_display_roundtrip(c.dpy);
        xdg_surface_ack_configure(xs, 0xdeadbeef);
        wl_display_roundtrip(c.dpy);
        break;
    }
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
    "a viewport scaled to sixteen thousand pixels",
    "copying to the clipboard twice",
    "a hundred thousand pointers",
    "a hundred thousand surfaces",
    "a subsurface that then asks to be a window",
    "a mapped window that then asks to be the cursor",
    "acking a configure that was never sent",
};
#define N_CASES ((int)(sizeof CASES / sizeof CASES[0]) - 1)

static uint32_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Peak resident size of this process, in kilobytes. A compositor that
 * survives a case having allocated a gibibyte has not survived it on
 * the machines this product exists for; it has been killed by the
 * kernel on all of them. */
static long peak_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256]; long kb = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "VmHWM:", 6)) { kb = atol(line + 6); break; }
    fclose(f);
    return kb;
}

/* WHAT "SURVIVED" HAS TO MEAN.
 *
 * This used to run the hostile client for six seconds and then ask
 * whether a well-behaved one could still get a window. A compositor
 * frozen solid for nine seconds PASSED that -- the freeze was over by
 * the time the question was asked. So did one that had allocated a
 * gibibyte. Both of those were real, and both were found by somebody
 * measuring rather than by this file.
 *
 * A desktop that stops answering for a third of a second has already
 * failed the person using it, whatever it does afterwards. So the case
 * fails if ONE dispatch takes longer than that, or if the compositor's
 * peak memory grows by more than a modest amount over the run. */
#define MAX_DISPATCH_MS  300
#define MAX_GROWTH_KB    (64 * 1024)

/* Run one case in its own compositor, then prove the compositor still
 * works by serving an ordinary client. */
static int run_case(const char *self, int which)
{
    aurwl *c = aurwl_create(800, 600, 60000);
    if (!c) { printf("  could not start a compositor\n"); return 1; }

    long rss0 = peak_rss_kb();
    uint32_t worst = 0;
    /* Cases whose whole point is that an ordinary client is NOT killed
     * by what it did. */
    const int must_live = (which == 10);
    int focused = 0;
    char mark[512]; marker_path(mark, sizeof mark);
    unlink(mark);

    char num[16];
    snprintf(num, sizeof num, "%d", which);
    const char *argv_h[] = { self, "--client", num, NULL };
    if (aurwl_spawn(c, argv_h) < 0) { aurwl_destroy(c); return 1; }

    uint32_t t0 = now_ms();
    while (now_ms() - t0 < 6000) {
        struct pollfd p = { aurwl_fd(c), POLLIN, 0 };
        poll(&p, 1, 16);
        uint32_t d0 = now_ms();
        aurwl_dispatch(c);
        uint32_t took = now_ms() - d0;
        if (took > worst) worst = took;
        aurwl_reap(c);
        aurwl_frame_done(c, now_ms());
        /* The shell gives the keyboard to a window when it appears.
         * Without that the clipboard cases are testing nothing: an
         * offer only ever goes to the focused client. */
        if (must_live && !focused && aurwl_window_count(c) > 0) {
            aurwl_set_focus(c, aurwl_window_at(c, 0));
            focused = 1;
        }
    }
    long grew = peak_rss_kb() - rss0;
    if (worst > MAX_DISPATCH_MS) {
        printf("\n      FROZE for %u ms in one dispatch -- nothing painted,\n"
               "      no key or click answered, for that long\n", worst);
        aurwl_destroy(c);
        return 1;
    }
    if (grew > MAX_GROWTH_KB) {
        printf("\n      GREW by %ld MB. On a 2 GB machine that is the\n"
               "      compositor being killed, which is every window\n",
               grew / 1024);
        aurwl_destroy(c);
        return 1;
    }

    if (must_live && access(mark, F_OK) != 0) {
        printf("\n      THE CLIENT DIED. What it did was copy to the\n"
               "      clipboard twice, which is not an attack.\n");
        aurwl_destroy(c);
        return 1;
    }

    /* The real test: does a well-behaved client still get a window? */
    const char *argv_ok[] = { "weston-simple-shm", NULL };
    if (aurwl_spawn(c, argv_ok) < 0) { aurwl_destroy(c); return 1; }
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
