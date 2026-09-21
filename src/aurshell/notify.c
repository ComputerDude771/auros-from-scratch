/* notify.c — see notify.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <dbus/dbus.h>

#include "notify.h"
#include "draw.h"
#include "foot.h"

/* ── what is on screen ──────────────────────────────────────────── */

typedef struct {
    uint32_t id;
    char     app[NOTIFY_APP];
    char     summary[NOTIFY_SUMMARY];
    char     body[NOTIFY_BODY];
    /* TWO DIFFERENT THINGS, and they were one.
     *
     * `urgent` is how it is DRAWN, and a sender may ask for it: at
     * worst a program makes its own card look important, which is
     * rude and not dangerous.
     *
     * `sticky` is whether it ever goes away by itself, and a sender
     * may NOT ask for that. It was the same flag, read straight out
     * of the hints dictionary, so any program could put a card on her
     * screen that never expired -- four of them and the queue was
     * wedged, and re-sendable the instant she pressed them away. That
     * is precisely "no program gets to decide how much of her screen
     * it takes", broken by the field that enforces it. Only the shell
     * sets sticky, and the shell only ever says things she must act
     * on. */
    int      urgent;
    int      sticky;
    int32_t  due_ms;          /* monotonic ms; ignored when sticky    */
} note;

#define BUS_NAME  "org.freedesktop.Notifications"
#define BUS_PATH  "/org/freedesktop/Notifications"

/* Eight seconds, not two. Long enough to look up, read it and look
 * back down, for a person who does not read quickly. */
#define LIFE_MS      8000
#define LIFE_MIN_MS  4000
#define LIFE_MAX_MS  30000
/* Trying again costs a connection attempt, so not every frame. */
#define RETRY_MS     1000

static struct {
    DBusConnection *conn;
    int      owned;
    /* The bus address this connection was made on. Compared against
     * the environment every time, because the shell MOVES: it starts
     * on the private bus its unit makes and adopts logind's a second
     * or two later, and that move can be triggered from anywhere --
     * the frame loop, or a spawn (see session.c). A server that is
     * still on the old bus is a server nothing can find, so this is
     * checked here rather than relying on whoever moved to say so. */
    char     addr[512];
    int32_t  next_try;
    int      state;                /* NS_* */
    DBusPendingCall *pending;
    int32_t  deadline;
    note     n[NOTIFY_MAX];
    int      count;
    uint32_t next_id;
} N = { NULL, 0, {0}, 0, 0, NULL, 0, {{0}}, 0, 1 };

static int32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* Copy, and make it printable.
 *
 * Every string here came from another program, so none of it may be
 * trusted to be short, to be one line, or to be free of the control
 * characters that would otherwise be drawn as boxes or would run the
 * text off the card. The body of a notification is also allowed by the
 * specification to contain a little markup -- <b>, <i>, <a href> --
 * which this shell does not render and must not show raw. */
/* The tags the specification actually permits in a body: <b>, <i>,
 * <u>, <a href=...> and <img ...>, and their closers. Nothing else in
 * a notification is markup. */
static int markup_tag(const char *p, size_t *len)
{
    static const char *TAG[] = { "b", "i", "u", "a", "img" };
    const char *q = p + 1;                       /* past the '<' */
    if (*q == '/') q++;
    size_t nlen = 0;
    while (q[nlen] && (q[nlen] == '-' ||
                       (q[nlen] >= 'a' && q[nlen] <= 'z') ||
                       (q[nlen] >= 'A' && q[nlen] <= 'Z'))) nlen++;
    if (!nlen || nlen > 3) return 0;
    int known = 0;
    for (size_t i = 0; i < sizeof TAG / sizeof TAG[0]; i++)
        if (strlen(TAG[i]) == nlen && !strncasecmp(q, TAG[i], nlen)) known = 1;
    if (!known) return 0;
    /* It only counts as a tag if it is CLOSED. An unterminated one is
     * a less-than sign somebody typed. */
    const char *close = strchr(q + nlen, '>');
    if (!close) return 0;
    *len = (size_t)(close - p) + 1;
    return 1;
}

/* Copy, and make it printable.
 *
 * Every string here came from another program, so none of it may be
 * trusted to be short, to be one line, or to be free of the control
 * characters that would otherwise be drawn as boxes or would run the
 * text off the card.
 *
 * ONLY REAL MARKUP IS REMOVED. The first version dropped everything
 * between a '<' and the next '>', and never reset that state at the
 * end of the string -- so a '<' with no '>' after it ate the whole
 * remainder. "Saved <report 2024>.pdf" arrived as "Saved .pdf", which
 * deletes the one word the notification exists to say, and "Happy
 * birthday <3" arrived as "Happy birthday". The server does not even
 * advertise "body-markup", so senders are required to send plain text
 * and angle brackets in it are hers, not a toolkit's.
 */
static void sane(char *dst, size_t n, const char *src)
{
    if (!n) return;
    dst[0] = 0;
    if (!src) return;
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < n; ) {
        if (*p == '<') {
            size_t taglen = 0;
            if (markup_tag(p, &taglen)) { p += taglen; continue; }
        }
        unsigned char ch = (unsigned char)*p;
        /* Newlines and tabs become spaces: a card is a fixed shape and
         * a program that sends ten lines must not be able to decide
         * how much of the screen it gets. */
        if (ch == '\n' || ch == '\t' || ch == '\r') { ch = ' '; p++; }
        else if (ch < 0x20 || ch == 0x7F) { p++; continue; }
        else if (ch >= 0x80) {
            /* A MULTI-BYTE CHARACTER GOES IN WHOLE OR NOT AT ALL.
             * Cutting one in half leaves the font engine two bytes it
             * cannot decode and draws a pair of empty boxes at the end
             * of every long summary in a language that is not English
             * -- on a product whose last commit was about being able
             * to draw those languages at all. */
            size_t len = (ch >= 0xF0) ? 4 : (ch >= 0xE0) ? 3 :
                         (ch >= 0xC0) ? 2 : 1;
            if (len == 1) { p++; continue; }        /* a stray trail byte */
            size_t have = 0;
            while (have < len && p[have]) have++;
            if (have < len) break;                  /* truncated at source */
            if (o + len >= n) break;                /* no room: stop clean */
            for (size_t i = 0; i < len; i++) dst[o++] = p[i];
            p += len;
            continue;
        }
        else p++;
        /* Runs of space collapse, so padding cannot be used to push
         * the readable part off the end. */
        if (ch == ' ' && (o == 0 || dst[o - 1] == ' ')) continue;
        dst[o++] = (char)ch;
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

/* ── the queue ──────────────────────────────────────────────────── */

static int find_id(uint32_t id)
{
    for (int i = 0; i < N.count; i++) if (N.n[i].id == id) return i;
    return -1;
}

static void drop_at(int i)
{
    if (i < 0 || i >= N.count) return;
    for (int k = i; k + 1 < N.count; k++) N.n[k] = N.n[k + 1];
    N.count--;
}

static void closed_signal(uint32_t id, uint32_t reason)
{
    if (!N.conn) return;
    DBusMessage *sig = dbus_message_new_signal(BUS_PATH, BUS_NAME,
                                               "NotificationClosed");
    if (!sig) return;
    dbus_message_append_args(sig, DBUS_TYPE_UINT32, &id,
                                  DBUS_TYPE_UINT32, &reason,
                                  DBUS_TYPE_INVALID);
    dbus_connection_send(N.conn, sig, NULL);
    dbus_message_unref(sig);
}

/* How many the screen can actually hold, which is not always
 * NOTIFY_MAX: at 1024x600 with her largest text only three cards fit.
 * A fourth used to be accepted, counted, and never drawn -- so it
 * could not be pressed away either, and if it was one that never
 * expires it sat in a slot for the life of the session, invisible.
 *
 * Set from the real screen once per pass of the main loop. The
 * fallback is NOTIFY_MAX, which is right before the first frame. */
static int vis_cap = NOTIFY_MAX;

static uint32_t mint_id(void)
{
    for (int tries = 0; tries < NOTIFY_MAX + 2; tries++) {
        uint32_t id = N.next_id++;
        if (N.next_id == 0) N.next_id = 1;      /* 0 means "new" */
        if (id && find_id(id) < 0) return id;
    }
    return N.next_id++;                         /* cannot happen */
}

static uint32_t put(const char *app, uint32_t replaces, const char *summary,
                    const char *body, int urgent, int sticky, int32_t life_ms)
{
    int slot;
    uint32_t id;

    if (replaces && (slot = find_id(replaces)) >= 0) {
        /* The same notification saying something new -- a download's
         * percentage. It keeps its place in the stack rather than
         * jumping to the front, or a progress bar would make the
         * others dance. */
        id = replaces;
    } else {
        /* A replaces_id naming nothing is a NEW notification, and it
         * gets a new id. It used to be given the id it asked for,
         * without advancing the counter -- so `replaces_id=1` from any
         * program collided with the first id this shell hands out, two
         * live cards shared it, find_id() returned the wrong one, and
         * CloseNotification closed somebody else's. One program taking
         * over another's notification, reachable by accident. */
        id = mint_id();
        int cap = vis_cap > 0 && vis_cap < NOTIFY_MAX ? vis_cap : NOTIFY_MAX;
        if (N.count >= cap) {
            /* Full. The oldest goes -- but the oldest one SHE STILL
             * HAS TO ACT ON does not. The battery warning was being
             * evicted by four downloads finishing, on the commit that
             * said it "stays until she presses it". */
            int victim = -1;
            for (int i = 0; i < N.count; i++)
                if (!N.n[i].sticky) { victim = i; break; }
            if (victim < 0 && sticky) victim = 0;   /* shell over shell */
            if (victim < 0) {
                /* Every slot is something she must act on, and this is
                 * not. Tell the sender at once rather than silently
                 * dropping it or pushing a warning off the screen. */
                closed_signal(id, 4);
                return id;
            }
            closed_signal(N.n[victim].id, 4);
            drop_at(victim);
        }
        slot = N.count++;
        memset(&N.n[slot], 0, sizeof N.n[slot]);
    }

    note *t = &N.n[slot];
    t->id = id;
    sane(t->app,     sizeof t->app,     app);
    sane(t->summary, sizeof t->summary, summary);
    sane(t->body,    sizeof t->body,    body);
    t->urgent = urgent;
    t->sticky = sticky;
    if (life_ms < LIFE_MIN_MS) life_ms = LIFE_MIN_MS;
    if (life_ms > LIFE_MAX_MS) life_ms = LIFE_MAX_MS;
    t->due_ms = now_ms() + life_ms;

    /* Something with no words at all is not a notification; it is a
     * card she cannot read and cannot tell apart from the next one. */
    if (!t->summary[0] && !t->body[0])
        snprintf(t->summary, sizeof t->summary, "%s",
                 t->app[0] ? t->app : "A program has a message");
    return id;
}

void notify_local(const char *summary, const char *body)
{
    /* Sticky: it stays until she presses it. Only the shell may do
     * this, and the shell only says things she has to act on -- a
     * battery that is about to run out. */
    put("This computer", 0, summary, body, 1, 1, LIFE_MAX_MS);
}

void notify_fit(const shell_ctx *c)
{
    if (!c || c->screen_w <= 0) return;
    notify_view v = { NOTIFY_MAX,
                      (c->text_scale > 0.1f) ? c->text_scale : 1.f,
                      foot_height(c) };
    notify_geom g;
    notify_layout(c->screen_w, c->screen_h + v.foot_h, &v, &g);
    vis_cap = g.n > 0 ? g.n : 1;
}

/* ── the bus ────────────────────────────────────────────────────── */

static const char *INTROSPECT =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "  <method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.Notifications\">\n"
    "  <method name=\"Notify\">\n"
    "   <arg type=\"s\" name=\"app_name\" direction=\"in\"/>\n"
    "   <arg type=\"u\" name=\"replaces_id\" direction=\"in\"/>\n"
    "   <arg type=\"s\" name=\"app_icon\" direction=\"in\"/>\n"
    "   <arg type=\"s\" name=\"summary\" direction=\"in\"/>\n"
    "   <arg type=\"s\" name=\"body\" direction=\"in\"/>\n"
    "   <arg type=\"as\" name=\"actions\" direction=\"in\"/>\n"
    "   <arg type=\"a{sv}\" name=\"hints\" direction=\"in\"/>\n"
    "   <arg type=\"i\" name=\"expire_timeout\" direction=\"in\"/>\n"
    "   <arg type=\"u\" name=\"id\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"CloseNotification\">\n"
    "   <arg type=\"u\" name=\"id\" direction=\"in\"/>\n"
    "  </method>\n"
    "  <method name=\"GetCapabilities\">\n"
    "   <arg type=\"as\" name=\"caps\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <method name=\"GetServerInformation\">\n"
    "   <arg type=\"s\" name=\"name\" direction=\"out\"/>\n"
    "   <arg type=\"s\" name=\"vendor\" direction=\"out\"/>\n"
    "   <arg type=\"s\" name=\"version\" direction=\"out\"/>\n"
    "   <arg type=\"s\" name=\"spec_version\" direction=\"out\"/>\n"
    "  </method>\n"
    "  <signal name=\"NotificationClosed\">\n"
    "   <arg type=\"u\"/><arg type=\"u\"/>\n"
    "  </signal>\n"
    "  <signal name=\"ActionInvoked\">\n"
    "   <arg type=\"u\"/><arg type=\"s\"/>\n"
    "  </signal>\n"
    " </interface>\n"
    "</node>\n";

static void reply_str(DBusConnection *c, DBusMessage *m, const char *s)
{
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r) return;
    dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
    dbus_connection_send(c, r, NULL);
    dbus_message_unref(r);
}

/* The urgency hint, out of a{sv}. Anything that is not a byte or is
 * not there at all is "normal", which is what the specification says
 * and also the only safe reading of a program we do not control. */
static int read_urgency(DBusMessageIter *hints)
{
    int urgency = 1;
    if (dbus_message_iter_get_arg_type(hints) != DBUS_TYPE_ARRAY) return urgency;
    DBusMessageIter arr;
    dbus_message_iter_recurse(hints, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e;
        dbus_message_iter_recurse(&arr, &e);
        const char *key = NULL;
        if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&e, &key);
        if (key && !strcmp(key, "urgency")) {
            dbus_message_iter_next(&e);
            if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_VARIANT) {
                DBusMessageIter v;
                dbus_message_iter_recurse(&e, &v);
                if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BYTE) {
                    unsigned char u = 1;
                    dbus_message_iter_get_basic(&v, &u);
                    urgency = (int)u;
                }
            }
        }
        dbus_message_iter_next(&arr);
    }
    if (urgency < 0 || urgency > 2) urgency = 1;
    return urgency;
}

static DBusHandlerResult on_notify(DBusConnection *c, DBusMessage *m)
{
    DBusMessageIter it;
    /* FALSE means the message has NO arguments -- and returning
     * HANDLED here claimed it, so libdbus generated no error reply
     * and nothing was ever sent. The caller sat on its pending call
     * for the libdbus default of twenty-five seconds; a toolkit that
     * notifies synchronously froze for all of it. Every other wrong
     * shape was answered; only the empty one was silent. */
    if (!dbus_message_iter_init(m, &it)) goto bad;

    const char *app = "", *icon = "", *summary = "", *body = "";
    dbus_uint32_t replaces = 0;
    dbus_int32_t  expire = -1;

    /* EVERY argument is checked before it is read. A message whose
     * signature does not match is a message from a program that got it
     * wrong, or from one that is trying something; neither may be
     * allowed to make this read a string out of an integer. */
    #define TAKE(t, p) do { \
        if (dbus_message_iter_get_arg_type(&it) != (t)) goto bad; \
        dbus_message_iter_get_basic(&it, (p)); \
        dbus_message_iter_next(&it); \
    } while (0)
    TAKE(DBUS_TYPE_STRING, &app);
    TAKE(DBUS_TYPE_UINT32, &replaces);
    TAKE(DBUS_TYPE_STRING, &icon);
    TAKE(DBUS_TYPE_STRING, &summary);
    TAKE(DBUS_TYPE_STRING, &body);
    #undef TAKE
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) goto bad;
    dbus_message_iter_next(&it);                  /* actions: not shown */
    int urgency = read_urgency(&it);
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32)
        dbus_message_iter_get_basic(&it, &expire);

    /* -1 is "you decide". 0 is "never" -- which is granted only to the
     * urgent ones, because a program that says "never" about a message
     * she does not need is a card that stays on her screen forever. */
    int32_t life = (expire > 0) ? (int32_t)expire : LIFE_MS;
    if (expire == 0) life = (urgency >= 2) ? LIFE_MAX_MS : LIFE_MS;

    /* urgency decides how it LOOKS. It does not decide whether it
     * ever goes away; see the note on the two flags above. */
    dbus_uint32_t id = put(app, replaces, summary, body,
                           urgency >= 2, 0, life);

    /* A fire-and-forget caller is not answered. The specification says
     * so, and every reply we send it is a message it will throw away. */
    if (!dbus_message_get_no_reply(m)) {
        DBusMessage *r = dbus_message_new_method_return(m);
        if (r) {
            dbus_message_append_args(r, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
            dbus_connection_send(c, r, NULL);
            dbus_message_unref(r);
        }
    }
    (void)icon;
    return DBUS_HANDLER_RESULT_HANDLED;

bad: {
        DBusMessage *e = dbus_message_new_error(m,
            "org.freedesktop.DBus.Error.InvalidArgs",
            "Notify takes (susssasa{sv}i)");
        if (e) { dbus_connection_send(c, e, NULL); dbus_message_unref(e); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
}

static DBusHandlerResult on_msg(DBusConnection *c, DBusMessage *m, void *ud)
{
    (void)ud;
    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable",
                                    "Introspect")) {
        reply_str(c, m, INTROSPECT);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, BUS_NAME, "Notify"))
        return on_notify(c, m);

    if (dbus_message_is_method_call(m, BUS_NAME, "CloseNotification")) {
        dbus_uint32_t id = 0;
        DBusError err; dbus_error_init(&err);
        if (dbus_message_get_args(m, &err, DBUS_TYPE_UINT32, &id,
                                  DBUS_TYPE_INVALID)) {
            int i = find_id(id);
            if (i >= 0) { drop_at(i); closed_signal(id, 3); }
        }
        dbus_error_free(&err);
        DBusMessage *r = dbus_message_new_method_return(m);
        if (r) { dbus_connection_send(c, r, NULL); dbus_message_unref(r); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(m, BUS_NAME, "GetCapabilities")) {
        /* Only what is true. Claiming "actions" would put buttons in
         * every notification that this shell would then not draw, and
         * a button that is not there is worse than one that was never
         * offered. "body" and "persistence" are true: the body is
         * shown, and nothing is lost when she is not looking. */
        const char *caps[] = { "body", "persistence" };
        DBusMessage *r = dbus_message_new_method_return(m);
        if (r) {
            DBusMessageIter it, arr;
            dbus_message_iter_init_append(r, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
            for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
                dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &caps[i]);
            dbus_message_iter_close_container(&it, &arr);
            dbus_connection_send(c, r, NULL);
            dbus_message_unref(r);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(m, BUS_NAME, "GetServerInformation")) {
        const char *name = "aurshell", *vendor = "AurOS";
        const char *ver = "1", *spec = "1.2";
        DBusMessage *r = dbus_message_new_method_return(m);
        if (r) {
            dbus_message_append_args(r, DBUS_TYPE_STRING, &name,
                                        DBUS_TYPE_STRING, &vendor,
                                        DBUS_TYPE_STRING, &ver,
                                        DBUS_TYPE_STRING, &spec,
                                        DBUS_TYPE_INVALID);
            dbus_connection_send(c, r, NULL);
            dbus_message_unref(r);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable VTABLE = { NULL, on_msg, NULL, NULL, NULL, NULL };

/* ── joining the bus, WITHOUT EVER BLOCKING THE SCREEN ───────────────
 *
 * dbus_bus_register() and dbus_bus_request_name() are synchronous
 * round trips, and libdbus makes them with an INFINITE poll -- not
 * even its own 25-second default. A gdb backtrace of this shell, on a
 * socket that accepted and then said nothing, sat in
 *
 *     __GI___poll (nfds=1, timeout=-1)
 *       dbus_pending_call_block -> dbus_bus_register -> notify_open
 *
 * for as long as it was left. Nothing painted, no key was read, no
 * click was answered, and no VT switch was acknowledged.
 *
 * That is not a contrived state. $XDG_RUNTIME_DIR/bus is a systemd
 * SOCKET unit: it exists and accepts connections before dbus.service
 * is answering on it, which is exactly the boot this whole feature is
 * about. The shell would have found the socket, connected, and stopped
 * being a desktop until the daemon got round to it.
 *
 * So the handshake is a state machine driven by the frame loop, with a
 * deadline this program owns. Hello and RequestName are sent with
 * send_with_reply() and collected in the pump; a reply that does not
 * come in time closes the connection and tries again later. Nothing
 * here waits for anything.
 */
static const char *bus_addr_now(void)
{
    const char *a = getenv("DBUS_SESSION_BUS_ADDRESS");
    return (a && *a) ? a : "";
}

enum { NS_OFF = 0, NS_HELLO, NS_NAME, NS_OWNED };

/* Long enough for any bus that is actually answering, short enough
 * that a broken one is retried rather than believed. */
#define HANDSHAKE_MS 4000

static int32_t mono32(void) { return now_ms(); }

static void drop_pending(void)
{
    if (!N.pending) return;
    dbus_pending_call_cancel(N.pending);
    dbus_pending_call_unref(N.pending);
    N.pending = NULL;
}

static int send_call(const char *member, DBusMessage **built)
{
    DBusMessage *m = dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", member);
    if (!m) return 0;
    if (built) *built = m;
    return 1;
}

static int begin_hello(void)
{
    DBusMessage *m = NULL;
    if (!send_call("Hello", &m)) return 0;
    int ok = dbus_connection_send_with_reply(N.conn, m, &N.pending,
                                             HANDSHAKE_MS) && N.pending;
    dbus_message_unref(m);
    if (!ok) { N.pending = NULL; return 0; }
    N.state = NS_HELLO;
    N.deadline = mono32() + HANDSHAKE_MS;
    return 1;
}

static int begin_request_name(void)
{
    DBusMessage *m = NULL;
    if (!send_call("RequestName", &m)) return 0;
    const char *nm = BUS_NAME;
    dbus_uint32_t flags = DBUS_NAME_FLAG_DO_NOT_QUEUE;
    if (!dbus_message_append_args(m, DBUS_TYPE_STRING, &nm,
                                     DBUS_TYPE_UINT32, &flags,
                                     DBUS_TYPE_INVALID)) {
        dbus_message_unref(m); return 0;
    }
    int ok = dbus_connection_send_with_reply(N.conn, m, &N.pending,
                                             HANDSHAKE_MS) && N.pending;
    dbus_message_unref(m);
    if (!ok) { N.pending = NULL; return 0; }
    N.state = NS_NAME;
    N.deadline = mono32() + HANDSHAKE_MS;
    return 1;
}

/* One step of the handshake, if one is owed. Returns 1 if anything
 * changed that the caller might want to say out loud. */
static int handshake_step(void)
{
    if (N.state == NS_OFF || N.state == NS_OWNED) return 0;
    if (!N.pending) { notify_close(); return 1; }

    if (!dbus_pending_call_get_completed(N.pending)) {
        if ((int32_t)(mono32() - N.deadline) < 0) return 0;
        fprintf(stderr, "aurshell: the session bus accepted a connection and "
                        "then did not answer; will try again\n");
        notify_close();
        N.next_try = mono32() + 2000;
        return 1;
    }

    DBusMessage *r = dbus_pending_call_steal_reply(N.pending);
    dbus_pending_call_unref(N.pending);
    N.pending = NULL;
    if (!r) { notify_close(); return 1; }

    int err = dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR;
    if (N.state == NS_HELLO) {
        const char *unique = NULL;
        if (err || !dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &unique,
                                          DBUS_TYPE_INVALID) || !unique) {
            fprintf(stderr, "aurshell: the session bus would not have us (%s)\n",
                    err ? dbus_message_get_error_name(r) : "no name");
            dbus_message_unref(r);
            notify_close();
            return 1;
        }
        dbus_bus_set_unique_name(N.conn, unique);
        dbus_message_unref(r);
        if (!begin_request_name()) { notify_close(); return 1; }
        return 0;
    }

    /* NS_NAME */
    dbus_uint32_t res = 0;
    if (err || !dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &res,
                                      DBUS_TYPE_INVALID)) {
        fprintf(stderr, "aurshell: could not claim %s (%s)\n", BUS_NAME,
                err ? dbus_message_get_error_name(r) : "no answer");
        dbus_message_unref(r);
        notify_close();
        return 1;
    }
    dbus_message_unref(r);
    if (res != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        /* Somebody else is the notification server. That is a fine
         * answer -- an organisation may ship its own -- and the right
         * thing is to stand down rather than fight over the name. */
        fprintf(stderr, "aurshell: %s is already served by another "
                        "program; not taking it over\n", BUS_NAME);
        notify_close();
        N.next_try = mono32() + 60000;
        return 1;
    }
    if (!dbus_connection_register_object_path(N.conn, BUS_PATH, &VTABLE, NULL)) {
        fprintf(stderr, "aurshell: could not serve %s\n", BUS_PATH);
        notify_close();
        return 1;
    }
    N.state = NS_OWNED;
    N.owned = 1;
    snprintf(N.addr, sizeof N.addr, "%s", bus_addr_now());
    fprintf(stderr, "aurshell: serving %s — programs can say things now\n",
            BUS_NAME);
    return 1;
}

int notify_open(void)
{
    if (N.owned && N.conn && dbus_connection_get_is_connected(N.conn)) {
        if (!strcmp(N.addr, bus_addr_now())) return 1;
        /* The shell moved. Give the name up here and take it again
         * over there; the cards on screen are kept, because they are
         * hers and have nothing to do with which socket they arrived
         * on. */
        fprintf(stderr, "aurshell: the session bus changed — announcing "
                        "notifications on the new one\n");
        notify_close();
        N.next_try = 0;                    /* at once, not in a second */
    }
    if (N.conn && !dbus_connection_get_is_connected(N.conn)) notify_close();

    /* A handshake already in flight: give it its step and nothing else. */
    if (N.state == NS_HELLO || N.state == NS_NAME) {
        handshake_step();
        return N.state == NS_OWNED;
    }

    int32_t t = mono32();
    if (N.next_try && (int32_t)(t - N.next_try) < 0) return 0;
    N.next_try = t + RETRY_MS;

    const char *want = bus_addr_now();
    if (!*want) return 0;                  /* no bus to be had yet */

    DBusError err; dbus_error_init(&err);
    /* OPENED BY ADDRESS, and privately.
     *
     * Privately because the shared connection is a process-wide
     * singleton that cannot be closed, and this one has to be: the
     * shell moves from the bus its unit made to the bus logind made.
     *
     * By ADDRESS because dbus_bus_get_private() does not re-read the
     * environment. libdbus caches the session bus address globally on
     * first use, so after the move it handed back a connection to the
     * bus we had just left -- and then request_name SUCCEEDED on it,
     * because we had only just given the name up there. The shell
     * logged "serving org.freedesktop.Notifications" on the wrong bus
     * and every application on the right one still found nothing.
     * Caught by a harness that started a second bus and sent to it.
     *
     * This is the one call here that can still wait on the kernel: a
     * connect() to a unix socket whose listen backlog is full. That is
     * bounded by the kernel and by a socket we have just stat()ed, and
     * it is not the unbounded wait the handshake used to be. */
    N.conn = dbus_connection_open_private(want, &err);
    if (!N.conn) {
        static int moaned = 0;
        if (!moaned++)
            fprintf(stderr, "aurshell: no session bus yet for notifications "
                            "(%s)\n", err.message ? err.message : "?");
        dbus_error_free(&err);
        return 0;
    }
    dbus_error_free(&err);
    /* Losing the bus must not kill the desktop. */
    dbus_connection_set_exit_on_disconnect(N.conn, FALSE);

    if (!begin_hello()) { notify_close(); return 0; }
    return 0;                              /* not ours yet; keep pumping */
}

void notify_close(void)
{
    drop_pending();
    N.state = NS_OFF;
    if (!N.conn) { N.owned = 0; return; }
    /* No release_name: that is another blocking round trip, and
     * closing the connection makes the bus drop every name we hold
     * anyway. */
    if (N.owned) dbus_connection_unregister_object_path(N.conn, BUS_PATH);
    dbus_connection_close(N.conn);
    dbus_connection_unref(N.conn);
    N.conn = NULL;
    N.owned = 0;
    N.addr[0] = 0;
}

void notify_fini(void)
{
    notify_close();
    N.count = 0;
}

int notify_fd(void)
{
    int fd = -1;
    if (!N.conn || !N.owned) return -1;
    if (!dbus_connection_get_unix_fd(N.conn, &fd)) return -1;
    return fd;
}

int notify_pump(shell_ctx *c)
{
    (void)c;
    if (!N.conn) return 0;
    if (!N.owned) {
        /* Still joining. read_write(0) is non-blocking and is what
         * lets the pending Hello or RequestName complete. */
        dbus_connection_read_write(N.conn, 0);
        while (dbus_connection_dispatch(N.conn) == DBUS_DISPATCH_DATA_REMAINS) { }
        return handshake_step();
    }
    int before = N.count;
    uint32_t sig = 0;
    for (int i = 0; i < N.count; i++) sig = sig * 31u + N.n[i].id + (uint32_t)N.n[i].due_ms;

    /* read_write(0) writes what is queued without waiting, which is
     * what dbus_connection_flush() was doing -- except that flush
     * BLOCKS until the queue is empty, with no timeout, from inside
     * the frame loop. */
    dbus_connection_read_write(N.conn, 0);
    while (dbus_connection_dispatch(N.conn) == DBUS_DISPATCH_DATA_REMAINS) { }

    if (!dbus_connection_get_is_connected(N.conn)) { notify_close(); return 1; }

    uint32_t after = 0;
    for (int i = 0; i < N.count; i++) after = after * 31u + N.n[i].id + (uint32_t)N.n[i].due_ms;
    return before != N.count || sig != after;
}

int notify_step(shell_ctx *c)
{
    (void)c;
    int changed = 0;
    int32_t t = now_ms();
    for (int i = N.count - 1; i >= 0; i--) {
        if (N.n[i].sticky) continue;            /* stays until pressed */
        if ((int32_t)(t - N.n[i].due_ms) < 0) continue;
        closed_signal(N.n[i].id, 1);            /* 1 = it expired */
        drop_at(i);
        changed = 1;
    }
    return changed;
}

int notify_showing(void) { return N.count; }

static const char *card_field(int i, size_t off)
{
    if (i < 0 || i >= N.count) return "";
    return (const char *)&N.n[i] + off;
}
const char *notify_card_app(int i)
{ return card_field(i, offsetof(note, app)); }
const char *notify_card_summary(int i)
{ return card_field(i, offsetof(note, summary)); }
const char *notify_card_body(int i)
{ return card_field(i, offsetof(note, body)); }
unsigned notify_card_id(int i)
{ return (i >= 0 && i < N.count) ? (unsigned)N.n[i].id : 0u; }
int notify_card_sticky(int i)
{ return (i >= 0 && i < N.count) ? N.n[i].sticky : 0; }

/* ── where they go ──────────────────────────────────────────────── */

void notify_view_now(const shell_ctx *c, notify_view *v)
{
    v->n = N.count;
    v->text_scale = (c && c->text_scale > 0.1f) ? c->text_scale : 1.f;
    v->foot_h = c ? foot_height(c) : 0;
}

void notify_layout(int sw, int sh, const notify_view *v, notify_geom *g)
{
    g->n = 0;
    if (!v || v->n <= 0) return;
    float k = (v->text_scale > 0.1f) ? v->text_scale : 1.f;

    int margin = (int)(18.f * k); if (margin < 14) margin = 14;
    int w = (int)(420.f * k);
    if (w > sw - 2 * margin) w = sw - 2 * margin;
    if (w < 200) w = 200;

    /* Title line, then two body lines, then padding. A fixed shape:
     * a program that sends ten lines of text must not get to decide
     * how much of her screen it takes. */
    int h = (int)(34.f + 52.f * k);
    if (h < NOTIFY_TARGET) h = NOTIFY_TARGET;
    int gap = (int)(10.f * k); if (gap < 8) gap = 8;

    /* Top right, and DOWN from the top: the band is at the bottom and
     * the way out must never be under a card. */
    int x = sw - w - margin;
    if (x < margin) x = margin;
    int y = margin;

    int body = sh - v->foot_h;
    int n = v->n > NOTIFY_MAX ? NOTIFY_MAX : v->n;
    for (int i = 0; i < n; i++) {
        int cy = y + i * (h + gap);
        if (cy + h > body) break;        /* a short screen holds fewer */
        g->card[g->n++] = (rect){ x, cy, w, h };
    }
}

int notify_targets(const shell_ctx *c, int sw, int sh,
                   const notify_view *v, rect *out, int max)
{
    (void)c;
    notify_geom g;
    notify_layout(sw, sh, v, &g);
    int n = 0;
    for (int i = 0; i < g.n && n < max; i++) out[n++] = g.card[i];
    return n;
}

void notify_paint(shell_ctx *c, surface *s, shell_fonts *f)
{
    if (!N.count) return;
    notify_view v; notify_view_now(c, &v);
    notify_geom g; notify_layout(s->w, s->h, &v, &g);
    if (!g.n) return;

    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;
    int pad = (int)(14.f * k); if (pad < 12) pad = 12;
    font *nm = f->mid   ? f->mid   : f->small;
    font *sm = f->small ? f->small : f->mid;

    for (int i = 0; i < g.n; i++) {
        rect r = g.card[i];
        /* Urgent ones carry the one signal colour on their edge. The
         * rest are the same surface as every other card in the system:
         * a notification is not more important than what she is doing,
         * it is only newer. */
        draw_rect(s, r, c->bg_alt, 0.97f);
        draw_frame(s, r, N.n[i].urgent ? 2 : 1,
                   N.n[i].urgent ? c->accent : c->overlay,
                   N.n[i].urgent ? 1.f : 0.75f);

        float tx = (float)(r.x + pad);
        float tw = (float)(r.w - 2 * pad);
        if (nm) {
            shell_text_elided(s, nm, tx,
                              (float)r.y + pad + font_ascent(nm),
                              tw, N.n[i].summary, c->fg_hi, 1.f);
        }
        if (sm && N.n[i].body[0]) {
            shell_text_elided(s, sm, tx,
                              (float)r.y + r.h - pad - font_descent(sm),
                              tw, N.n[i].body, c->fg, 0.88f);
        }
        /* Who it is from, quietly, at the top right. She needs to know
         * whether the browser or the computer is talking to her. */
        if (sm && N.n[i].app[0]) {
            float aw = shell_text_w(sm, N.n[i].app);
            if (aw < tw * 0.45f)
                shell_text(s, sm, (float)(r.x + r.w - pad) - aw,
                           (float)r.y + pad + font_ascent(sm),
                           N.n[i].app, c->subtle, 0.7f);
        }
    }
}

int notify_click(shell_ctx *c, int x, int y)
{
    if (!N.count) return 0;
    notify_view v; notify_view_now(c, &v);
    notify_geom g; notify_layout(c->screen_w, c->screen_h + v.foot_h, &v, &g);
    for (int i = 0; i < g.n; i++) {
        rect r = g.card[i];
        if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) continue;
        /* 2 = she dismissed it. The program that sent it is told, so
         * one that is waiting to know can stop waiting. */
        closed_signal(N.n[i].id, 2);
        drop_at(i);
        return 1;
    }
    return 0;
}
