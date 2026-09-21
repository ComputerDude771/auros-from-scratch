/* notifytest.c — when a program has something to tell her, does she see it?
 *
 * Nothing in this image implemented org.freedesktop.Notifications,
 * which is the one way a Linux application has of saying anything
 * outside its own window. Every such call came back ServiceUnknown and
 * the application shrugged: the USB stick said nothing when it was
 * safe to remove, the browser said nothing when a download finished,
 * the store said nothing when it was done.
 *
 * This drives the real thing over a REAL SESSION BUS, started here,
 * with a real client (gdbus) -- not by calling the handlers
 * directly, because half of what can go wrong is in the marshalling
 * and in what happens when a message is the wrong shape.
 *
 * And most of it is about not trusting the sender. Every string in a
 * notification comes from another program: it may be enormous, may be
 * twelve lines, may be full of control characters, may be the markup
 * the specification allows and this shell does not draw. A card is a
 * fixed shape on HER screen and no program gets to decide how much of
 * it to take.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>

#include "../src/aurshell/notify.h"
#include "../src/aurshell/foot.h"

static int fail = 0, checked = 0;
static void ok(const char *what, int good)
{
    checked++;
    printf("    %-58s %s\n", what, good ? "ok" : "FAIL");
    if (!good) fail++;
}

/* ── a bus of our own ───────────────────────────────────────────── */

static char bus_addr[512];
static pid_t bus_pid = -1;

static int start_bus(void)
{
    char cfg[] = "/tmp/auros-notifytest-XXXXXX";
    int d = mkstemp(cfg);
    if (d < 0) return -1;
    dprintf(d,
        "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
        "<busconfig><type>session</type>"
        "<listen>unix:tmpdir=/tmp</listen>"
        "<policy context=\"default\">"
        "<allow send_destination=\"*\" eavesdrop=\"true\"/>"
        "<allow eavesdrop=\"true\"/><allow own=\"*\"/>"
        "</policy></busconfig>\n");
    close(d);

    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1);
        execlp("dbus-daemon", "dbus-daemon", "--config-file", cfg,
               "--print-address", "--nofork", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    while (n + 1 < sizeof bus_addr) {
        ssize_t r = read(p[0], bus_addr + n, 1);
        if (r <= 0) break;
        if (bus_addr[n] == '\n') break;
        n++;
    }
    bus_addr[n] = 0;
    close(p[0]);
    unlink(cfg);
    if (!n) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
    bus_pid = pid;
    setenv("DBUS_SESSION_BUS_ADDRESS", bus_addr, 1);
    return 0;
}

static void stop_bus(void)
{
    if (bus_pid > 0) { kill(bus_pid, SIGTERM); waitpid(bus_pid, NULL, 0); }
}

/* A real client, over the real bus.
 *
 * Forked and NOT waited for here. dbus-send --print-reply blocks until
 * the server replies, and the server only replies when the shell pumps
 * -- so waiting for the client before pumping is a deadlock, and the
 * first version of this file wrote one. The caller pumps while the
 * client is in flight, which is what the real main loop does. */
static pid_t send_notify_async(const char *app, unsigned replaces,
                               const char *summary, const char *body,
                               int expire)
{
    /* gdbus, not dbus-send. dbus-send cannot build an a{sv} at all --
     * "Unknown type variant" -- so it cannot make a well-formed Notify
     * call, and a harness whose client is broken proves nothing about
     * the server. Found by running the two side by side against a
     * server that was working the whole time. */
    char rid[32], exp[32];
    snprintf(rid, sizeof rid, "%u", replaces);
    snprintf(exp, sizeof exp, "%d", expire);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execlp("gdbus", "gdbus", "call", "--session",
               "--dest", "org.freedesktop.Notifications",
               "--object-path", "/org/freedesktop/Notifications",
               "--method", "org.freedesktop.Notifications.Notify",
               app, rid, "", summary, body, "[]", "{}", exp, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Pump the shell while the client talks to it, then collect it. */
static int send_notify(shell_ctx *c, const char *app, unsigned replaces,
                       const char *summary, const char *body, int expire)
{
    pid_t pid = send_notify_async(app, replaces, summary, body, expire);
    if (pid < 0) return -1;
    for (int i = 0; i < 500; i++) {
        notify_pump(c);
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    return -1;                                   /* it never answered */
}

/* Pump until the count changes, or we give up. The client is another
 * process, so this is a real round trip. */
static int pump_until(shell_ctx *c, int want, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        notify_pump(c);
        if (notify_showing() == want) return 1;
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    return notify_showing() == want;
}

static void fresh(shell_ctx *c)
{
    memset(c, 0, sizeof *c);
    theme_t t = {0};
    shell_theme_load(c, &t);
    c->text_scale = 1.f;
    c->foot_hover = -1;
    c->screen_w = 1024;
    c->screen_h = 600 - foot_height(c);
}

int main(void)
{
    printf("\nWhen a program has something to tell her, does she see it?\n\n");

    /* ── the geometry, which needs no bus at all ────────────────── */
    printf("  where the cards go, on every screen and every text size\n");
    {
        static const struct { int w, h; } RES[] = {
            { 1024, 600 }, { 1366, 768 }, { 1920, 1080 },
        };
        static const float K[] = { 1.0f, 1.25f, 1.6f, 2.0f };
        int measured = 0, small = 0, overlap = 0, offscreen = 0;
        for (size_t r = 0; r < sizeof RES / sizeof RES[0]; r++)
            for (size_t k = 0; k < sizeof K / sizeof K[0]; k++)
                for (int n = 1; n <= NOTIFY_MAX; n++) {
                    shell_ctx c; fresh(&c);
                    c.text_scale = K[k];
                    notify_view v = { n, K[k], foot_height(&c) };
                    notify_geom g;
                    notify_layout(RES[r].w, RES[r].h, &v, &g);
                    for (int i = 0; i < g.n; i++) {
                        rect a = g.card[i];
                        measured++;
                        int shorter = a.w < a.h ? a.w : a.h;
                        if (shorter < NOTIFY_TARGET) small++;
                        if (a.x < 0 || a.y < 0 ||
                            a.x + a.w > RES[r].w ||
                            a.y + a.h > RES[r].h - v.foot_h) offscreen++;
                        for (int j = 0; j < i; j++) {
                            rect b = g.card[j];
                            if (a.x < b.x + b.w && b.x < a.x + a.w &&
                                a.y < b.y + b.h && b.y < a.y + a.h) overlap++;
                        }
                    }
                }
        char w[80];
        snprintf(w, sizeof w, "%d cards measured, none under %dpx",
                 measured, NOTIFY_TARGET);
        ok(w, measured > 0 && small == 0);
        ok("no card is drawn over another", overlap == 0);
        ok("no card is off the screen, or over the band", offscreen == 0);
    }

    /* ── the real thing, over a real bus ────────────────────────── */
    printf("\n  a real program, on a real bus\n");
    if (start_bus() < 0) {
        printf("    (no dbus-daemon here to test against)              SKIP\n");
        goto done;
    }
    shell_ctx c; fresh(&c);
    ok("the shell claims org.freedesktop.Notifications", notify_open() == 1);
    ok("...and offers a file descriptor to wait on", notify_fd() >= 0);
    ok("nothing is showing before anything is sent", notify_showing() == 0);

    ok("a program's message is accepted",
       send_notify(&c, "Your files", 0, "The USB stick can be taken out",
                   "Everything has been written to it.", 8000) == 0);
    ok("...and appears on her screen", pump_until(&c, 1, 3000));

    /* Replacing: a download's percentage must not stack up four deep. */
    send_notify(&c, "Firefox", 1, "Downloading  50%", "report.pdf", 8000);
    pump_until(&c, 1, 1000);
    ok("the same message saying something new replaces itself",
       notify_showing() == 1);

    for (int i = 0; i < 6; i++) {
        char sm[64]; snprintf(sm, sizeof sm, "Message %d", i);
        send_notify(&c, "A program", 0, sm, "", 8000);
    }
    for (int i = 0; i < 60 && notify_showing() < NOTIFY_MAX; i++) {
        notify_pump(&c);
        struct timespec t = { 0, 10 * 1000 * 1000 }; nanosleep(&t, NULL);
    }
    ok("six at once do not become six cards", notify_showing() == NOTIFY_MAX);

    /* Pressing one takes it away. It is the only control a card has,
     * so if this does not work the card cannot be got rid of. */
    {
        notify_view v; notify_view_now(&c, &v);
        notify_geom g;
        notify_layout(c.screen_w, c.screen_h + v.foot_h, &v, &g);
        int before = notify_showing();
        ok("there is something to press", g.n > 0);
        if (g.n > 0) {
            ok("pressing a card takes it away",
               notify_click(&c, g.card[0].x + g.card[0].w / 2,
                                g.card[0].y + g.card[0].h / 2) == 1 &&
               notify_showing() == before - 1);
        }
        ok("pressing where there is no card does nothing",
           notify_click(&c, 2, c.screen_h - 2) == 0);
    }

    /* ── what a program is not allowed to do to her screen ──────── */
    printf("\n  and what a program is NOT allowed to do to her screen\n");
    while (notify_showing()) {
        notify_view v; notify_view_now(&c, &v);
        notify_geom g; notify_layout(c.screen_w, c.screen_h + v.foot_h, &v, &g);
        if (!g.n) break;
        notify_click(&c, g.card[0].x + 1, g.card[0].y + 1);
    }

    {
        char huge[2000];
        memset(huge, 'x', sizeof huge - 1); huge[sizeof huge - 1] = 0;
        ok("a two-thousand-character summary is accepted without crashing",
           send_notify(&c, "A program", 0, huge, huge, 8000) == 0);
        ok("...and becomes one card", pump_until(&c, 1, 3000));
    }
    /* A wrong-shaped call must be refused, not guessed at. dbus-send
     * with too few arguments is exactly what a broken client sends. */
    {
        pid_t pid = fork();
        if (pid == 0) {
            int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
            execlp("gdbus", "gdbus", "call", "--session",
                   "--dest", "org.freedesktop.Notifications",
                   "--object-path", "/org/freedesktop/Notifications",
                   "--method", "org.freedesktop.Notifications.Notify",
                   "app", "not-a-number", (char *)NULL);
            _exit(127);
        }
        int had = notify_showing();
        int st = 0;
        for (int i = 0; i < 500; i++) {
            notify_pump(&c);
            if (waitpid(pid, &st, WNOHANG) == pid) break;
            struct timespec t = { 0, 10 * 1000 * 1000 }; nanosleep(&t, NULL);
        }
        ok("a message of the wrong shape is refused, not guessed at",
           WIFEXITED(st) && WEXITSTATUS(st) != 0 && notify_showing() == had);
    }

    /* The server must still be alive and serving after all of that. */
    ok("the shell is still serving after a malformed call",
       notify_fd() >= 0);
    ok("...and still accepts a good one",
       send_notify(&c, "Your files", 0, "Still here", "", 8000) == 0);

    /* ── THE SHELL MOVES BUS ────────────────────────────────────
     *
     * It starts on the private bus its unit makes and adopts logind's
     * a second or two later. A server left on the old bus is a server
     * nothing can find, and the move can be triggered from more than
     * one place -- the frame loop, or a spawn -- so notify_open() has
     * to notice for itself rather than trusting whoever moved to say
     * so. */
    printf("\n  and when the shell moves to logind's bus\n");
    {
        char first[512];
        snprintf(first, sizeof first, "%s", bus_addr);
        pid_t old = bus_pid;
        bus_pid = -1;
        ok("(a second bus can be started)", start_bus() == 0);
        ok("...and it is a different one", strcmp(first, bus_addr) != 0);

        /* start_bus() has already pointed the environment at it. */
        ok("the shell notices and re-announces itself there",
           notify_open() == 1);
        int had = notify_showing();
        ok("...and a program on the NEW bus can reach it",
           send_notify(&c, "Your files", 0, "On the new bus", "", 8000) == 0);
        ok("...and the message arrives", notify_showing() == had + 1 ||
                                         notify_showing() == NOTIFY_MAX);

        if (old > 0) { kill(old, SIGTERM); waitpid(old, NULL, 0); }
    }

    notify_fini();
    ok("it gives the name up when it is done", notify_fd() < 0);
    stop_bus();

done:
    printf("\n");
    if (fail) {
        printf("%d of %d wrong. A program tried to tell her something and\n",
               fail, checked);
        printf("she did not find out.\n");
        return 1;
    }
    printf("%d checks: programs can say things, she can get rid of them,\n", checked);
    printf("and none of them can take over her screen.\n");
    return 0;
}
