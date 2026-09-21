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

#include <dbus/dbus.h>

#include "../src/aurshell/notify.h"
#include "../src/aurshell/foot.h"

/* The harness reads the cards the way the screen does: through the
 * geometry contract and the queue, not by counting. */
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
    if (pipe(p) < 0) { unlink(cfg); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); unlink(cfg); return -1; }
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
static const char *hints = "{}";

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
               app, rid, "", summary, body, "[]", hints, exp, (char *)NULL);
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

static const char *note_summary(int i) { return notify_card_summary(i); }
static const char *note_body(int i)    { return notify_card_body(i); }

/* Press every card away, the way she would. */
static void clear_all(shell_ctx *c)
{
    for (int guard = 0; guard < NOTIFY_MAX + 4 && notify_showing(); guard++) {
        notify_view v; notify_view_now(c, &v);
        notify_geom g; notify_layout(c->screen_w, c->screen_h + v.foot_h, &v, &g);
        if (!g.n) break;
        notify_click(c, g.card[0].x + g.card[0].w / 2,
                        g.card[0].y + g.card[0].h / 2);
    }
    notify_pump(c);
}

/* Joining the bus is a HANDSHAKE now, driven by the frame loop, so
 * that a socket which accepts and then says nothing cannot stop the
 * desktop. It therefore does not finish inside one call -- which is
 * the point -- and a harness has to pump for it the way the shell
 * does. */
static int wait_owned(shell_ctx *c, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (notify_open()) return 1;
        notify_pump(c);
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    return notify_open();
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
                    notify_view v;
                    notify_view_now(&c, &v);
                    v.n = n; v.text_scale = K[k];
                    notify_geom g;
                    notify_layout(RES[r].w, RES[r].h, &v, &g);
                    for (int i = 0; i < g.n; i++) {
                        rect a = g.card[i];
                        measured++;
                        int shorter = a.w < a.h ? a.w : a.h;
                        if (shorter < NOTIFY_TARGET) small++;
                        if (a.x < 0 || a.y < v.bar_h ||
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
        ok("no card is off the screen, over the band, or over the clock",
           offscreen == 0);
    }

    /* ── the real thing, over a real bus ────────────────────────── */
    printf("\n  a real program, on a real bus\n");
    if (start_bus() < 0) {
        printf("    (no dbus-daemon here to test against)              SKIP\n");
        goto done;
    }
    shell_ctx c; fresh(&c);
    ok("the shell claims org.freedesktop.Notifications",
       wait_owned(&c, 5000) == 1);
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
        ok("...cut to what a card can hold",
           (int)strlen(note_summary(0)) < NOTIFY_SUMMARY &&
           (int)strlen(note_body(0)) < NOTIFY_BODY);
    }

    /* ── WHAT ACTUALLY ENDS UP ON THE CARD ──────────────────────
     *
     * Counting cards proves nothing about the text in them, and the
     * text is the whole notification. Every case below is a string a
     * real program sends. */
    clear_all(&c);
    {
        static const struct { const char *sent, *want, *why; } T[] = {
        { "Saved <report 2024>.pdf", "Saved <report 2024>.pdf",
          "angle brackets in a filename are hers, not markup" },
        { "Happy birthday <3", "Happy birthday <3",
          "an unclosed < does not eat the rest of the line" },
        { "Download <b>finished</b>", "Download finished",
          "real markup is removed, and only the markup" },
        { "A <a href=\"http://x\">link</a> here", "A link here",
          "a link becomes its words" },
        { "Two\nlines\there", "Two lines here",
          "newlines and tabs become one space" },
        { "Lots      of      space", "Lots of space",
          "padding cannot push the words off the end" },
        { "bell\x07 and null-ish\x01", "bell and null-ish",
          "control characters are not drawn as boxes" },
        };
        for (size_t k = 0; k < sizeof T / sizeof T[0]; k++) {
            clear_all(&c);
            send_notify(&c, "A program", 0, T[k].sent, "", 8000);
            pump_until(&c, 1, 2000);
            char w[128];
            snprintf(w, sizeof w, "%s", T[k].why);
            if (strcmp(note_summary(0), T[k].want) != 0) {
                printf("    %-58s FAIL\n", w);
                printf("      sent: %s\n      want: %s\n      got : %s\n",
                       T[k].sent, T[k].want, note_summary(0));
                fail++; checked++;
            } else ok(w, 1);
        }
        /* A word in a language that is not English, cut short. It must
         * not be cut in the middle of a character: the font engine
         * would draw the halves as empty boxes. */
        clear_all(&c);
        char wide[1200];
        size_t o = 0;
        while (o + 3 < sizeof wide - 1) { memcpy(wide + o, "\xe6\x97\xa5", 3); o += 3; }
        wide[o] = 0;
        send_notify(&c, "A program", 0, wide, "", 8000);
        pump_until(&c, 1, 2000);
        const char *got = note_summary(0);
        int whole = 1;
        for (size_t i = 0; got[i]; ) {
            unsigned char ch = (unsigned char)got[i];
            size_t len = (ch >= 0xF0) ? 4 : (ch >= 0xE0) ? 3 : (ch >= 0xC0) ? 2 : 1;
            for (size_t j = 1; j < len; j++)
                if (!got[i + j]) { whole = 0; break; }
            i += len;
        }
        ok("a long line in her language is never cut through a letter",
           whole && got[0]);
    }
    /* ── MALFORMED CALLS, SENT WITH libdbus ─────────────────────
     *
     * Not with gdbus. gdbus introspects the server first, learns the
     * signature, and refuses to send a badly-typed call AT ALL -- so
     * the check that used to be here never reached the server's type
     * checks and would have passed identically if the server had
     * none. It was proving that gdbus validates its own arguments.
     *
     * A hostile or broken program does not introspect first. These
     * are built by hand and put on the wire. */
    {
        DBusError e; dbus_error_init(&e);
        DBusConnection *raw = dbus_connection_open_private(bus_addr, &e);
        ok("(a raw client can be opened)", raw != NULL);
        if (raw) {
            dbus_bus_register(raw, &e);
            struct { const char *what; int nargs; } BAD[] = {
                { "no arguments at all",        0 },
                { "the wrong type in place 2",  1 },
                { "a truncated argument list",  2 },
            };
            for (size_t k = 0; k < sizeof BAD / sizeof BAD[0]; k++) {
                DBusMessage *m = dbus_message_new_method_call(
                    "org.freedesktop.Notifications",
                    "/org/freedesktop/Notifications",
                    "org.freedesktop.Notifications", "Notify");
                const char *sv = "app";
                if (BAD[k].nargs >= 1)
                    dbus_message_append_args(m, DBUS_TYPE_STRING, &sv,
                                             DBUS_TYPE_INVALID);
                if (BAD[k].nargs >= 2) {
                    const char *nope = "this-should-be-a-number";
                    dbus_message_append_args(m, DBUS_TYPE_STRING, &nope,
                                             DBUS_TYPE_INVALID);
                }
                DBusPendingCall *pc = NULL;
                dbus_connection_send_with_reply(raw, m, &pc, 5000);
                dbus_connection_flush(raw);
                dbus_message_unref(m);

                int had = notify_showing();
                int got = 0;
                for (int i = 0; i < 400 && pc; i++) {
                    notify_pump(&c);
                    /* dispatch, not just read_write: a pending call is
                     * only marked complete when its reply is
                     * dispatched. */
                    dbus_connection_read_write_dispatch(raw, 0);
                    if (dbus_pending_call_get_completed(pc)) { got = 1; break; }
                    struct timespec t = { 0, 10 * 1000 * 1000 };
                    nanosleep(&t, NULL);
                }
                DBusMessage *r = (pc && got) ? dbus_pending_call_steal_reply(pc)
                                             : NULL;
                char w[110];
                snprintf(w, sizeof w, "%s is ANSWERED, not left hanging",
                         BAD[k].what);
                ok(w, r != NULL);
                snprintf(w, sizeof w, "...with an error, and no card appears");
                ok(w, r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR
                        && notify_showing() == had);
                if (r) dbus_message_unref(r);
                if (pc) dbus_pending_call_unref(pc);
            }
            dbus_connection_close(raw);
            dbus_connection_unref(raw);
        }
        dbus_error_free(&e);
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
           wait_owned(&c, 5000) == 1);
        int had = notify_showing();
        ok("...and a program on the NEW bus can reach it",
           send_notify(&c, "Your files", 0, "On the new bus", "", 8000) == 0);
        ok("...and the message arrives", notify_showing() == had + 1);

        if (old > 0) { kill(old, SIGTERM); waitpid(old, NULL, 0); }
    }

    /* ── WHAT A SENDING PROGRAM MAY AND MAY NOT DECIDE ──────────
     *
     * A card that never goes away is the one thing a program must not
     * be able to put on her screen, and the flag that made it never
     * go away used to be read straight out of the sender's hints. */
    printf("\n  what a sending program may and may not decide\n");
    clear_all(&c);
    {
        hints = "{'urgency': <byte 2>}";
        send_notify(&c, "A program", 0, "I am very important", "", 1);
        pump_until(&c, 1, 2000);
        ok("a program may say its message is urgent", notify_showing() == 1);
        ok("...but may NOT make it stay for ever", !notify_card_sticky(0));
        /* Its life is clamped up to the floor, so it is still on
         * screen -- then it goes, on its own, which is the point. */
        int went = 0;
        for (int i = 0; i < 700 && !went; i++) {   /* > LIFE_MIN, 4s */
            notify_step(&c);
            if (!notify_showing()) went = 1;
            struct timespec t = { 0, 10 * 1000 * 1000 }; nanosleep(&t, NULL);
        }
        ok("...and it does go, by itself", went);
        hints = "{}";
    }
    {
        /* The shell's own. This one does stay. */
        clear_all(&c);
        notify_local("The battery is very low", "Please plug this computer in.");
        ok("the shell can say something that stays", notify_showing() == 1 &&
                                                     notify_card_sticky(0));
        for (int i = 0; i < 60; i++) notify_step(&c);
        ok("...and retiring does not retire it", notify_showing() == 1);

        /* And four programs talking at once must not push it off. */
        for (int i = 0; i < 6; i++) {
            char sm[48]; snprintf(sm, sizeof sm, "Download %d finished", i);
            send_notify(&c, "Firefox", 0, sm, "", 8000);
        }
        int survived = 0;
        for (int i = 0; i < notify_showing(); i++)
            if (notify_card_sticky(i)) survived = 1;
        ok("six downloads finishing do not remove the battery warning",
           survived);
        ok("...and the screen still holds no more than it can draw",
           notify_showing() <= NOTIFY_MAX);
        clear_all(&c);
    }
    {
        /* A replaces_id naming nothing must not take an id that is
         * already somebody's. */
        /* Learn what the server is about to hand out, then ask to
         * replace THAT -- an id that does not exist yet. With the
         * defect the impostor is given it verbatim and the counter is
         * not advanced, so the very next message mints the same id
         * and two live cards share it. */
        clear_all(&c);
        send_notify(&c, "First", 0, "one", "", 8000);
        pump_until(&c, 1, 2000);
        unsigned next = notify_card_id(0) + 1;
        send_notify(&c, "Impostor", next, "I claim the next id", "", 8000);
        pump_until(&c, 2, 2000);
        send_notify(&c, "Third", 0, "three", "", 8000);
        pump_until(&c, 3, 2000);
        ok("a message replacing nothing becomes a new one",
           notify_showing() == 3 &&
           strcmp(notify_card_summary(0), notify_card_summary(1)) != 0);
        /* And it must not be given an id that is already somebody's.
         * A twin means CloseNotification closes the wrong card and
         * the real owner can never replace or close its own -- one
         * program quietly taking over another's notification. */
        int twins = 0;
        for (int i = 0; i < notify_showing(); i++)
            for (int j = 0; j < i; j++)
                if (notify_card_id(i) == notify_card_id(j)) twins++;
        ok("...with an id that is not already somebody else's", twins == 0);
        clear_all(&c);
    }
    {
        /* Never accept more cards than the screen can draw. A card
         * that is not drawn cannot be pressed away either. */
        shell_ctx small; fresh(&small);
        small.text_scale = 2.0f;
        small.screen_w = 1024;
        small.screen_h = 600 - foot_height(&small);
        notify_fit(&small);
        for (int i = 0; i < NOTIFY_MAX + 3; i++) {
            char sm[32]; snprintf(sm, sizeof sm, "Message %d", i);
            send_notify(&small, "A program", 0, sm, "", 8000);
        }
        notify_view v; notify_view_now(&small, &v);
        notify_geom g;
        notify_layout(small.screen_w, small.screen_h + v.foot_h, &v, &g);
        ok("on a small screen at her largest text, every card is drawn",
           g.n == notify_showing() && g.n > 0);
        clear_all(&small);
        notify_fit(&c);
    }

    printf("\n  the rest of what a program may ask\n");
    {
        clear_all(&c);
        send_notify(&c, "A program", 0, "Close me", "", 8000);
        pump_until(&c, 1, 2000);
        /* CloseNotification, by the id the server handed back. The id
         * is 1-based and monotonic, so the live one is findable by
         * closing each in turn until the card goes. */
        int closed = 0;
        for (unsigned id = 1; id < 64 && !closed; id++) {
            pid_t pid = fork();
            if (pid == 0) {
                int nul = open("/dev/null", O_WRONLY);
                if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
                char a[32]; snprintf(a, sizeof a, "%u", id);
                execlp("gdbus", "gdbus", "call", "--session",
                       "--dest", "org.freedesktop.Notifications",
                       "--object-path", "/org/freedesktop/Notifications",
                       "--method", "org.freedesktop.Notifications.CloseNotification",
                       a, (char *)NULL);
                _exit(127);
            }
            for (int i = 0; i < 200; i++) {
                notify_pump(&c);
                if (waitpid(pid, NULL, WNOHANG) == pid) break;
                struct timespec t = { 0, 10 * 1000 * 1000 }; nanosleep(&t, NULL);
            }
            if (!notify_showing()) closed = 1;
        }
        ok("a program can take its own message back off the screen", closed);
    }
    {
        /* GetCapabilities must claim only what is true: "actions"
         * would put buttons in every notification that this shell
         * then would not draw. */
        char out[512] = {0};
        FILE *f = popen("gdbus call --session "
                        "--dest org.freedesktop.Notifications "
                        "--object-path /org/freedesktop/Notifications "
                        "--method org.freedesktop.Notifications.GetCapabilities "
                        "2>/dev/null", "r");
        pid_t pump = fork();
        if (pump == 0) { _exit(0); }
        for (int i = 0; i < 50; i++) { notify_pump(&c);
            struct timespec t = { 0, 10 * 1000 * 1000 }; nanosleep(&t, NULL); }
        if (f) { if (!fgets(out, sizeof out, f)) out[0] = 0; pclose(f); }
        if (pump > 0) waitpid(pump, NULL, 0);
        ok("it says it can show a body", strstr(out, "body") != NULL);
        ok("...and does NOT claim buttons it will not draw",
           strstr(out, "actions") == NULL);
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
