/* net.c — see net.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "net.h"
#include "draw.h"

/* ── what we know ───────────────────────────────────────────────── */

#define NET_MAX_AP   24
#define NET_MAX_SAVED 32
#define OUT_MAX      16384
#define PW_MAX       80

/* Which nmcli run is in flight. They run one at a time, in this order,
 * because each is cheap except the last and she should see her own
 * network before the slow scan finishes rather than after it. */
enum { J_NONE, J_DEVICES, J_RADIO, J_SAVED, J_LIST_FAST, J_LIST_SCAN, J_JOIN };

static struct {
    int   page;                  /* the panel's state, one of P_*     */
    int   trouble;               /* one of T_*, when page == P_TROUBLE*/

    net_ap aps[NET_MAX_AP];
    int   n_aps;
    char  saved[NET_MAX_SAVED][NET_NAME_MAX];
    int   n_saved;

    int   have_wifi;             /* 1 yes, 0 no, -1 no answer         */
    char  wifi_dev[32];          /* what the radio is called, so that
                                  * Stop can actually stop            */
    int   asked_devices;         /* we have looked at least once      */
    int   scanning;              /* the slow look is still running    */

    int   sel;                   /* the row that is lit, and the one
                                  * Enter acts on. ONE notion of it:
                                  * the keyboard moves it and so does
                                  * the pointer, and painting reads it.
                                  * Keeping a separate `hover` meant the
                                  * pointer's row was lit while the
                                  * keyboard's row was the one Enter
                                  * joined -- so pressing Down three
                                  * times changed nothing on screen and
                                  * Enter joined a network she had never
                                  * been shown she had chosen.         */
    char  pick[NET_NAME_MAX];       /* its name, kept across the scan    */
    int   pick_secure;
    char  pw[PW_MAX];
    int   pw_n;

    int   first_row;             /* paging: the row at the top        */
    int   hover_act;             /* action under the pointer, -1 none */
    /* Where the pointer was when we last looked. The host calls
     * net_motion() after every batch of input whether or not the mouse
     * moved, so without this a keystroke was immediately followed by
     * the pointer re-asserting its own row over the keyboard's. */
    int   last_x, last_y;
    int   moved_once;

    /* the child */
    int   job;
    pid_t pid;
    int   fd;
    char  out[OUT_MAX];
    int   out_n;
} N = { .fd = -1, .pid = -1, .sel = -1, .hover_act = -1 };

/* ── running nmcli ──────────────────────────────────────────────────
 *
 * execvp with a pipe, never a shell. A wifi name is text somebody else
 * chose -- it can contain a quote, a semicolon or a backtick, and on a
 * machine that handed it to `sh -c` those would be instructions running
 * as her. Here they are argument bytes and nothing else.
 */
/* Children we have finished with but that may not have exited yet.
 *
 * The compositor used to collect every child of this process with
 * waitpid(-1), so nothing here had to. It no longer does -- a
 * compositor reaping another subsystem's child means that subsystem
 * can neither learn how it ended nor safely signal it -- so this
 * subsystem collects its own, and net_reap() is swept beside
 * aurwl_reap() on every pass of the main loop.
 *
 * Only one of these runs at a time, so the array is never more than a
 * slot or two deep; the full case is unreachable in practice and takes
 * the blunt way out rather than leaving something behind. */
#define REAP_MAX 8
static pid_t reaping[REAP_MAX];
static int   n_reaping;

void net_reap(void)
{
    for (int i = n_reaping - 1; i >= 0; i--) {
        pid_t r = waitpid(reaping[i], NULL, WNOHANG);
        /* >0 it exited. <0 with ECHILD means it is not ours to wait
         * for any more, so the slot is stale either way. Any OTHER
         * error -- EINTR, today impossible because every handler this
         * program installs restarts, but one sigaction away from being
         * possible -- must NOT be read as "gone", or a live child is
         * forgotten and becomes a process nothing collects. */
        if (r > 0 || (r < 0 && errno == ECHILD))
            reaping[i] = reaping[--n_reaping];
    }
}

static void remember_to_reap(pid_t p)
{
    if (p <= 0) return;
    net_reap();
    if (n_reaping < REAP_MAX) { reaping[n_reaping++] = p; return; }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

static void child_stop(void)
{
    if (N.fd >= 0) { close(N.fd); N.fd = -1; }
    if (N.pid > 0) { kill(N.pid, SIGTERM); remember_to_reap(N.pid); }
    N.pid = -1;
    N.job = J_NONE;
    N.out_n = 0;
    /* Nothing is running, so nothing is looking. This was cleared in
     * the two places a scan FINISHED and not in the one place a scan
     * was stopped -- so pressing a network during the four seconds a
     * scan takes, then coming back, left the panel reading "Still
     * looking..." for the rest of the session with no child alive. */
    N.scanning = 0;
}

/* The one thing the parent cannot learn any other way. The compositor
 * reaps its own children and this one is not its, but a failed execvp
 * still has to be distinguishable from a command that ran and said
 * nothing -- so the child says so on the pipe the parent is already
 * reading. */
/* Split deliberately. "\x01aurnoexec" is NOT what it looks like: a C
 * hex escape is greedy and 'a' is a hex digit, so \x01a is the single
 * byte 0x1A and the string is "\x1Aurnoexec". It happened to work,
 * because the same macro is written and searched for -- but anyone who
 * retyped this literal by hand, or wrote it in a test, would silently
 * break every detection of a program that could not be started. */
#define NOEXEC_MARK "\x01" "aurnoexec\n"

static int child_start(int job, const char *const argv[])
{
    if (N.job != J_NONE) child_stop();

    int p[2];
    if (pipe(p) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1);
        dup2(p[1], 2);
        if (p[1] > 2) close(p[1]);
        /* Its own group, so a stuck child can be stopped without
         * touching the desktop, and so nothing it starts inherits the
         * shell's terminal. */
        setsid();
        signal(SIGPIPE, SIG_DFL);
        /* Plain output in a known language: this parses what comes
         * back, and a translated "Error:" is a different string. */
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], (char *const *)argv);
        ssize_t r = write(2, NOEXEC_MARK, sizeof NOEXEC_MARK - 1);
        (void)r;
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    N.fd = p[0];
    N.pid = pid;
    N.job = job;
    N.out_n = 0;
    return 0;
}

/* ── reading nmcli back ─────────────────────────────────────────────
 *
 * `-t` gives colon-separated fields with a literal colon written as
 * "\:" and a literal backslash as "\\". A wifi name with a colon in it
 * is not exotic -- phones produce them -- and splitting on every colon
 * would truncate exactly those names.
 */
static int split_t(char *line, char *f[], int maxf)
{
    int n = 0;
    char *w = line;
    f[n++] = w;
    for (char *r = line; *r; r++) {
        if (*r == '\\' && r[1]) { *w++ = *++r; continue; }
        if (*r == ':') {
            *w++ = 0;
            if (n >= maxf) return n;
            f[n++] = w;
            continue;
        }
        *w++ = *r;
    }
    *w = 0;
    return n;
}

static int named_in(const char *name, const char saved[][NET_NAME_MAX],
                    int n_saved)
{
    for (int i = 0; i < n_saved; i++)
        if (!strcmp(saved[i], name)) return 1;
    return 0;
}

/* 1 there is wifi, 0 there is none, -1 the machine did not answer.
 *
 * The third case is the one that matters. `nmcli device status` with
 * NetworkManager not yet running prints "Error: NetworkManager is not
 * running." and exits non-zero -- and the first version of this read
 * that as "no line says wifi" and therefore "this computer has no wifi
 * of its own". On first boot, in the house the machine was carried
 * into, opening the panel a few seconds early told a laptop with a
 * wifi card that it had none, and offered nothing but Close. */
int net_parse_devices(char *terse, char *dev, size_t devn)
{
    int have_wifi = 0, lines = 0;
    if (dev && devn) dev[0] = 0;
    char *save = NULL;
    for (char *line = strtok_r(terse, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "Error:", 6) || !strncmp(line, "error:", 6))
            return -1;
        char *f[4];
        if (split_t(line, f, 4) < 2) continue;
        lines++;
        if (!strcmp(f[1], "wifi")) {
            have_wifi = 1;
            /* The first one. A machine with two radios is rare and the
             * first is the one nmcli would have used anyway. */
            if (dev && devn && !dev[0]) snprintf(dev, devn, "%s", f[0]);
        }
    }
    /* Not one device at all, not even loopback: nmcli said nothing we
     * can read, which is not the same as "no wifi". */
    if (!lines) return -1;
    return have_wifi;
}

int net_parse_saved(char *terse, char out[][NET_NAME_MAX], int max)
{
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(terse, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *f[2];
        if (split_t(line, f, 2) < 2) continue;
        if (strcmp(f[1], "802-11-wireless") && strcmp(f[1], "wifi")) continue;
        if (!f[0][0] || n >= max) continue;
        snprintf(out[n], NET_NAME_MAX, "%s", f[0]);
        n++;
    }
    return n;
}

int net_parse_list(char *terse, net_ap *out, int max,
                   const char saved[][NET_NAME_MAX], int n_saved)
{
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(terse, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *f[4];
        if (split_t(line, f, 4) < 4) continue;
        const char *name = f[1];
        /* A network with no name is one that does not announce itself.
         * She cannot pick what is not there to read, and an empty row
         * would be showing her nothing and calling it something. */
        if (!name[0]) continue;

        int sig = atoi(f[2]);
        if (sig < 0) sig = 0;
        if (sig > 100) sig = 100;
        int secure = f[3][0] != 0;
        int in_use = f[0][0] == '*';

        /* One row per name. A house with two access points answering
         * for one network is one network to her, and the stronger
         * reading is the true one. */
        int at = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].name, name)) { at = i; break; }
        if (at < 0) {
            if (n >= max) continue;
            at = n++;
            memset(&out[at], 0, sizeof out[at]);
            snprintf(out[at].name, NET_NAME_MAX, "%s", name);
        }
        if (sig > out[at].signal) out[at].signal = sig;
        out[at].secure |= secure;
        out[at].in_use |= in_use;
        out[at].known = named_in(out[at].name, saved, n_saved);
    }

    /* Strongest first, and the one we are already on above everything.
     * A list in the order the radio happened to hear them is a list she
     * has to read all of. */
    for (int i = 1; i < n; i++) {
        net_ap k = out[i];
        int j = i - 1;
        while (j >= 0 && ((out[j].in_use < k.in_use) ||
                          (out[j].in_use == k.in_use &&
                           out[j].signal < k.signal))) {
            out[j + 1] = out[j]; j--;
        }
        out[j + 1] = k;
    }
    return n;
}

/* The state-carrying wrappers the panel itself uses. */
static int is_saved(const char *name)
{
    return named_in(name, (const char (*)[NET_NAME_MAX])N.saved, N.n_saved);
}

static void parse_devices(void)
{
    N.have_wifi = net_parse_devices(N.out, N.wifi_dev, sizeof N.wifi_dev);
    N.asked_devices = 1;
}


static void parse_saved(void)
{
    N.n_saved = net_parse_saved(N.out, N.saved, NET_MAX_SAVED);
}

static void parse_list(void)
{
    N.n_aps = net_parse_list(N.out, N.aps, NET_MAX_AP,
                             (const char (*)[NET_NAME_MAX])N.saved, N.n_saved);
    if (N.first_row >= N.n_aps) N.first_row = 0;
}

/* nmcli says why it failed in a sentence meant for a person who
 * already knows what a network is. This is the only place in the
 * product that reads those sentences, and nothing downstream of it
 * ever sees one. */
int net_parse_trouble(const char *out)
{
    if (strstr(out, NOEXEC_MARK))                     return T_NOTOOL;
    if (strstr(out, "Secrets were required")   ||
        strstr(out, "secrets were required")   ||
        strstr(out, "psk: property is invalid")||
        strstr(out, "Passwords or encryption keys"))  return T_PASSWORD;
    if (strstr(out, "No Wi-Fi device found") ||
        strstr(out, "No Wi-Fi device"))               return T_NOWIFI;
    if (strstr(out, "NetworkManager is not running")) return T_NOANSWER;
    if (strstr(out, "No network with SSID")    ||
        strstr(out, "not found"))                     return T_GONE;
    if (strstr(out, "Not authorized")          ||
        strstr(out, "not authorized")          ||
        strstr(out, "insufficient privileges") ||
        strstr(out, "access denied"))                 return T_NOTALLOWED;
    return T_OTHER;
}

/* ── the sequence ───────────────────────────────────────────────── */

static void start_job(int job)
{
    switch (job) {
    case J_DEVICES: {
        const char *a[] = { "nmcli", "-t", "-f", "DEVICE,TYPE,STATE",
                            "device", "status", NULL };
        /* If we cannot even start looking, say so now. The alternative
         * is a panel that reads "Looking..." for the rest of her life,
         * which is the worst of the three things it could do. */
        if (child_start(J_DEVICES, a) < 0) {
            N.page = P_TROUBLE; N.trouble = T_NOTOOL;
        }
        break;
    }
    /* Switch the radio on. She pressed Internet; wanting the wifi on
     * is not a separate question.
     *
     * A laptop wifi key, or an `nmcli radio wifi off` from months ago,
     * leaves the card present but soft-blocked, and the panel would
     * then show an empty list and say no wifi was found near this
     * computer -- true of the radio and false of the house, with no way
     * out of it that does not involve a terminal. It is idempotent, so
     * it costs a fork on a machine whose radio is already on. */
    case J_RADIO: {
        const char *a[] = { "nmcli", "radio", "wifi", "on", NULL };
        child_start(J_RADIO, a);
        break;
    }
    case J_SAVED: {
        const char *a[] = { "nmcli", "-t", "-f", "NAME,TYPE",
                            "connection", "show", NULL };
        child_start(J_SAVED, a);
        break;
    }
    /* The cached answer first, so the list is on the screen at once,
     * then a real look. Waiting for the radio before showing her
     * anything is four to eight seconds of a blank panel, which reads
     * as a machine that has stopped. */
    case J_LIST_FAST: {
        const char *a[] = { "nmcli", "-t", "-f",
                            "IN-USE,SSID,SIGNAL,SECURITY",
                            "device", "wifi", "list", "--rescan", "no", NULL };
        child_start(J_LIST_FAST, a);
        break;
    }
    case J_LIST_SCAN: {
        const char *a[] = { "nmcli", "-t", "-f",
                            "IN-USE,SSID,SIGNAL,SECURITY",
                            "device", "wifi", "list", "--rescan", "yes", NULL };
        N.scanning = 1;
        child_start(J_LIST_SCAN, a);
        break;
    }
    }
}

static void start_join(void)
{
    /* -w bounds the wait. nmcli's own default is ninety seconds, which
     * is long past the point where she has decided the machine is
     * broken and pressed something else. */
    const char *with_pw[] = { "nmcli", "-w", "25", "device", "wifi",
                              "connect", N.pick, "password", N.pw, NULL };
    const char *no_pw[]   = { "nmcli", "-w", "25", "device", "wifi",
                              "connect", N.pick, NULL };
    N.page = P_JOINING;
    if (child_start(J_JOIN, (N.pw_n > 0) ? with_pw : no_pw) < 0) {
        N.page = P_TROUBLE; N.trouble = T_OTHER;
    }
}

void net_opened(shell_ctx *c)
{
    (void)c;
    N.page = P_LIST;
    N.sel = -1;
    N.first_row = 0;
    N.hover_act = -1;
    N.moved_once = 0;
    /* A re-open must not inherit the last one's "still looking" or its
     * trouble screen. */
    N.scanning = 0;
    N.trouble = 0;
    N.pw[0] = 0; N.pw_n = 0;
    start_job(J_DEVICES);
}

void net_closed(shell_ctx *c)
{
    (void)c;
    child_stop();
    N.scanning = 0;
    /* The password is not kept one frame longer than the screen that
     * asked for it. */
    memset(N.pw, 0, sizeof N.pw);
    N.pw_n = 0;
}

void net_fini(void)
{
    child_stop();
    /* Give whatever was stopped a moment to go, then insist. Nothing
     * of ours outlives the desktop. */
    for (int i = 0; i < n_reaping; i++) {
        if (waitpid(reaping[i], NULL, WNOHANG) != 0) continue;
        kill(reaping[i], SIGKILL);
        waitpid(reaping[i], NULL, 0);
    }
    n_reaping = 0;
    memset(&N, 0, sizeof N);
    /* -1 is "nothing", and 0 is "the first one". Zeroing the struct and
     * restoring only two of the four sentinels would leave a
     * re-initialised panel with its first button drawn as if the
     * pointer were on it. */
    N.fd = -1; N.pid = -1; N.sel = -1; N.hover_act = -1;
}

int net_fd(void) { return N.fd; }

int net_pump(shell_ctx *c)
{
    (void)c;
    if (N.fd < 0) return 0;

    for (;;) {
        if (N.out_n >= OUT_MAX - 1) {
            /* More than sixteen kilobytes of network names is not a
             * house, it is a fault or an attack. Take what we have --
             * and STOP the thing producing it. Closing the pipe and
             * walking away leaves it alive until it next writes and
             * takes a SIGPIPE, which for a program that has stopped
             * writing is never. */
            if (N.pid > 0) kill(N.pid, SIGTERM);
            break;
        }
        ssize_t r = read(N.fd, N.out + N.out_n, (size_t)(OUT_MAX - 1 - N.out_n));
        if (r > 0) { N.out_n += (int)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (r < 0 && errno == EINTR) continue;
        break;                       /* 0 = the child is finished */
    }

    N.out[N.out_n] = 0;
    int job = N.job;
    char *text = N.out;

    close(N.fd); N.fd = -1;
    remember_to_reap(N.pid);
    N.pid = -1;
    N.job = J_NONE;

    switch (job) {
    case J_DEVICES:
        if (strstr(text, NOEXEC_MARK)) {
            N.page = P_TROUBLE; N.trouble = T_NOTOOL;
            N.out_n = 0; return 1;
        }
        parse_devices();
        N.out_n = 0;
        if (N.have_wifi < 0) {
            N.page = P_TROUBLE; N.trouble = T_NOANSWER; return 1;
        }
        if (!N.have_wifi) { N.page = P_TROUBLE; N.trouble = T_NOWIFI; return 1; }
        start_job(J_RADIO);
        return 1;

    case J_RADIO:
        /* Whether it worked or not, carry on: an already-on radio
         * prints nothing, and a refusal here is better discovered as an
         * empty list than as a stop. */
        N.out_n = 0;
        start_job(J_SAVED);
        return 1;

    case J_SAVED:
        parse_saved();
        N.out_n = 0;
        start_job(J_LIST_FAST);
        return 1;

    case J_LIST_FAST:
        parse_list();
        N.out_n = 0;
        start_job(J_LIST_SCAN);
        return 1;

    case J_LIST_SCAN:
        parse_list();
        N.out_n = 0;
        N.scanning = 0;
        return 1;

    case J_JOIN: {
        int ok = strstr(text, "successfully activated") != NULL;
        int why = net_parse_trouble(text);
        /* A join that says nothing at all did not happen. */
        if (ok) {
            N.page = P_JOINED;
            memset(N.pw, 0, sizeof N.pw); N.pw_n = 0;
            /* Remembered from here on, so the next time is one press. */
            if (N.n_saved < NET_MAX_SAVED && !is_saved(N.pick))
                snprintf(N.saved[N.n_saved++], NET_NAME_MAX, "%s", N.pick);
            for (int i = 0; i < N.n_aps; i++) {
                N.aps[i].in_use = !strcmp(N.aps[i].name, N.pick);
                if (N.aps[i].in_use) N.aps[i].known = 1;
            }
        } else {
            N.page = P_TROUBLE;
            N.trouble = why;
        }
        N.out_n = 0;
        return 1;
    }
    }
    N.out_n = 0;
    return 0;
}

/* ── is this machine on a network ───────────────────────────────────
 *
 * The kernel's routing table, read as a file. No child, no daemon, no
 * library: a default route is exactly the thing that makes the
 * difference between a machine that can reach the world and one that
 * cannot, and asking costs a few microseconds, so the band can ask
 * every frame.
 */
int net_online(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (f) {
        char line[512];
        if (fgets(line, sizeof line, f)) {                 /* header */
            while (fgets(line, sizeof line, f)) {
                char iface[64]; unsigned long dest = 1, gw = 0;
                if (sscanf(line, "%63s %lx %lx", iface, &dest, &gw) != 3)
                    continue;
                if (dest == 0 && strcmp(iface, "lo")) { fclose(f); return 1; }
            }
        }
        fclose(f);
    }
    /* A machine on a network that has no IPv4 at all is not exotic any
     * more, and telling such a person they are offline would be a lie
     * they cannot argue with. */
    f = fopen("/proc/net/ipv6_route", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char dest[64], plen[8], dev[64];
            if (sscanf(line, "%63s %7s %*s %*s %*s %*s %*s %*s %*s %63s",
                       dest, plen, dev) != 3) continue;
            if (!strcmp(dest, "00000000000000000000000000000000") &&
                !strcmp(plen, "00") && strcmp(dev, "lo")) { fclose(f); return 1; }
        }
        fclose(f);
    }
    return 0;
}

/* ── where everything is ────────────────────────────────────────────
 *
 * ONE function, called by painting AND by hit-testing, per
 * docs/SHELLS.md. It takes no fonts on purpose: a layout derived from
 * font metrics is a layout the click handler cannot reproduce without
 * loading the same fonts, and that gap is exactly how a desktop ends up
 * with buttons that are half a line away from where they look.
 */
enum { A_CLOSE, A_JOIN, A_BACK, A_MORE, A_AGAIN, A_STOP, A_N };

static const char *ACT_LABEL[A_N] = {
    "Close", "Connect", "Go back", "Show more", "Try again", "Stop"
};

typedef struct {
    int  gx, cw;                 /* the left measure and content width */
    int  head_y, sub_y;          /* baselines                          */
    int  rule_y;
    int  list_y, row_h, n_vis;
    rect rows[NET_MAX_AP]; int n_rows;
    rect field;
    rect acts[A_N]; int act[A_N]; int n_acts;
} net_geom;

static int clampi_(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* The layout reads a VIEW -- which screen, how many networks -- rather
 * than the live state, so that it is a pure function of its arguments.
 * That is what lets tools/targets.c measure every screen of this panel
 * at every text size without a radio, a daemon or a running desktop;
 * a geometry function that can only be asked about the state the
 * machine happens to be in is a geometry function nothing can check. */
static net_view view_now(void)
{
    net_view v = { N.page, N.trouble, N.n_aps, N.first_row };
    return v;
}

static void net_layout(const shell_ctx *c, int sw, int sh,
                       const net_view *v, net_geom *g)
{
    memset(g, 0, sizeof *g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    g->gx = clampi_(sw / 14, 20, 110);
    g->cw = sw - 2 * g->gx;
    if (g->cw < 120) { g->gx = 8; g->cw = sw - 16; }

    g->head_y = (int)(52.f * k);
    g->rule_y = g->head_y + (int)(14.f * k);
    g->sub_y  = g->rule_y + (int)(26.f * k);

    /* Furniture grows with the type, but not IN PROPORTION to it. A
     * button scaled to twice its size is a hundred-pixel button, which
     * is not easier to press than a sixty-pixel one and costs the list
     * above it two whole rows. What has to grow at her chosen rate is
     * the words; what has to grow around them is the room they need.
     * At 200% on a 1024x600 panel the proportional version showed her
     * exactly one network out of six. */
    int act_h = (int)(34.f + 16.f * k); if (act_h < 44) act_h = 44;
    int act_w = (int)(100.f + 50.f * k);
    int gap   = (int)(10.f * k); if (gap < 8) gap = 8;

    /* The lowest the buttons may sit and still be on the panel. It is
     * a LIMIT, not a position: the buttons go directly under whatever
     * this screen has to say, not at the bottom of the screen.
     *
     * Pinning them to the bottom is the obvious thing and it is wrong
     * here. On a screen carrying one sentence it leaves four hundred
     * empty pixels between the sentence and the thing to press, and
     * she has to go looking for the second half of a two-part
     * instruction. Under the words is where her eye already is. */
    int last_y = sh - (int)(14.f * k) - act_h;
    if (last_y < 0) last_y = 0;

    g->row_h = (int)(30.f + 26.f * k); if (g->row_h < 48) g->row_h = 48;
    g->list_y = g->sub_y + (int)(18.f * k);
    int space = last_y - (int)(14.f * k) - g->list_y;
    g->n_vis = space / g->row_h;
    if (g->n_vis < 1) g->n_vis = 1;
    if (g->n_vis > NET_MAX_AP) g->n_vis = NET_MAX_AP;

    int acts_y;
    switch (v->page) {
    case P_LIST: {
        int from = clampi_(v->first_row, 0, v->n_aps > 0 ? v->n_aps - 1 : 0);
        for (int i = 0; i < g->n_vis && from + i < v->n_aps; i++) {
            g->rows[g->n_rows] = (rect){ g->gx, g->list_y + i * g->row_h,
                                         g->cw, g->row_h };
            g->n_rows++;
        }
        acts_y = g->list_y + g->n_rows * g->row_h + (int)(16.f * k);
        break;
    }
    case P_PASSWORD: {
        int fh = (int)(56.f * k); if (fh < 52) fh = 52;
        int floor_y = g->sub_y + (int)(18.f * k);
        int fy = g->sub_y + (int)(48.f * k);
        /* Keep the whole block -- box, gap, buttons -- on the panel.
         * At the largest text on the smallest panel, which is exactly
         * the machine and exactly the person this product is for, the
         * naive version put the box THROUGH the Connect button.
         * tools/targets.c is what said so. */
        int highest = last_y - (int)(16.f * k) - fh;
        if (fy > highest) fy = highest;
        if (fy < floor_y) {
            fy = floor_y;
            fh = last_y - (int)(16.f * k) - fy;
            if (fh < 44) fh = 44;
        }
        g->field = (rect){ g->gx, fy, g->cw, fh };
        acts_y = fy + fh + (int)(16.f * k);
        break;
    }
    default:
        /* One sentence, sometimes two. */
        acts_y = g->sub_y + (int)(52.f * k);
        break;
    }
    if (acts_y > last_y) acts_y = last_y;
    if (acts_y < g->sub_y + (int)(20.f * k)) acts_y = g->sub_y + (int)(20.f * k);

    /* The way out, always in the same corner, always the same size.
     * Rule 6: every state she can reach, she can leave -- and the
     * leaving is not a different shape on every screen. */
    int which[A_N], na = 0;
    switch (v->page) {
    case P_LIST:
        which[na++] = A_AGAIN;
        if (v->n_aps > g->n_vis) which[na++] = A_MORE;
        which[na++] = A_CLOSE;
        break;
    case P_PASSWORD:  which[na++] = A_JOIN; which[na++] = A_BACK; break;
    case P_JOINING:   which[na++] = A_STOP; break;
    case P_JOINED:    which[na++] = A_CLOSE; break;
    case P_TROUBLE:
        /* Try again on every one of them, including the two that look
         * final. "This computer has no wifi" is a conclusion drawn from
         * one answer from one program, and being wrong about it -- the
         * network service was still starting, the radio was switched
         * off -- used to leave her on a screen whose only button was
         * Close. A retry costs one second and nothing else; being
         * unable to retry costs her the machine. */
        which[na++] = A_AGAIN;
        if (v->trouble == T_NOWIFI || v->trouble == T_NOTOOL ||
            v->trouble == T_NOANSWER || v->trouble == T_NOTALLOWED)
            which[na++] = A_CLOSE;
        else
            which[na++] = A_BACK;
        break;
    }
    /* If they will not fit side by side, they get narrower rather than
     * overlapping; the 44px floor still wins. */
    if (na > 0) {
        int room = g->cw - (na - 1) * gap;
        if (act_w * na > room) act_w = room / na;
        if (act_w < 44) act_w = 44;
    }
    int ax = g->gx;
    for (int i = 0; i < na; i++) {
        g->acts[i] = (rect){ ax, acts_y, act_w, act_h };
        g->act[i]  = which[i];
        ax += act_w + gap;
    }
    g->n_acts = na;
}

/* ── painting ───────────────────────────────────────────────────── */

/* How strong the signal is, as four bars. Not a percentage and not a
 * number in dBm: she is deciding "is this the one in this house", and
 * four bars is the shape every phone she has ever held used to answer
 * exactly that question. */
static void draw_bars(surface *s, int x, int cy, float k, int signal,
                      uint32_t on, uint32_t off)
{
    int bw = (int)(5.f * k); if (bw < 4) bw = 4;
    int gap = (int)(3.f * k); if (gap < 2) gap = 2;
    int filled = signal >= 75 ? 4 : signal >= 50 ? 3 : signal >= 25 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int h = (int)((7 + i * 5) * k); if (h < 6) h = 6;
        rect b = { x + i * (bw + gap), cy - h / 2 + (int)(6.f * k), bw, h };
        draw_rect(s, b, i < filled ? on : off, i < filled ? 1.f : 0.35f);
    }
}

/* Connect does nothing until there is something to send. A button that
 * presses cleanly and has no effect is the failure this product keeps
 * finding in itself, so it is drawn as unavailable and does not light
 * up under the pointer -- tools/hittest.c's rule is that nothing
 * highlights that cannot be clicked, and the converse is what makes a
 * dimmed control honest rather than broken. */
static int act_enabled(int a)
{
    if (a != A_JOIN) return 1;
    return N.pw_n > 0 || !N.pick_secure;
}

static void paint_action(surface *s, shell_fonts *f, const shell_ctx *c,
                         rect r, const char *label, int hot, int primary,
                         int enabled)
{
    uint32_t fill = primary ? c->fg : c->surface_c;
    uint32_t ink  = primary ? c->bg : c->fg;
    if (hot && !primary) fill = c->surface_hi;
    draw_rect(s, r, fill, enabled ? 1.f : 0.45f);
    if (!primary) draw_frame(s, r, 1, c->subtle, hot ? 0.9f : 0.5f);
    font *ft = f->mid ? f->mid : f->small;
    if (!ft) return;
    shell_text_centred(s, ft, (float)r.x + r.w / 2.f,
                       shell_baseline(ft, (float)r.y, (float)r.h),
                       label, ink, enabled ? 1.f : 0.5f);
}

void net_paint(shell_ctx *c, surface *s, shell_fonts *f)
{
    if (!c->net_open) return;
    int sw = s->w, sh = c->screen_h;
    if (sh <= 0 || sh > s->h) sh = s->h;

    net_geom g;
    net_view v = view_now();
    net_layout(c, sw, sh, &v, &g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    rect body = { 0, 0, sw, sh };
    draw_rect(s, body, c->bg, 0.97f);

    font *head = f->huge ? f->huge : (f->big ? f->big : f->mid);
    font *name = f->mid ? f->mid : f->small;
    font *bodyf = f->small ? f->small : f->mid;
    font *tiny = f->tiny ? f->tiny : bodyf;

    if (head)
        shell_text(s, head, (float)g.gx, (float)g.head_y, "Internet",
                   c->fg, 1.f);
    draw_hrule(s, g.gx, g.rule_y, g.cw, 1, c->fg, 0.28f);

    /* One line under the rule saying where she is. It is the only
     * running commentary in the panel: everything else is a thing she
     * can press. */
    char sub[160];
    const char *subtext = sub;
    sub[0] = 0;
    switch (N.page) {
    case P_LIST:
        if (!N.asked_devices)      subtext = "Looking...";
        else if (N.n_aps == 0)     subtext = N.scanning
                                      ? "Looking for wifi near you..."
                                      : "No wifi found near this computer.";
        else if (N.scanning)       subtext = "Nearby wifi. Still looking...";
        else                       subtext = "Press the name of your wifi.";
        break;
    case P_PASSWORD:
        snprintf(sub, sizeof sub, "Type the password for %s", N.pick);
        break;
    case P_JOINING:
        snprintf(sub, sizeof sub, "Joining %s. This can take a moment.",
                 N.pick);
        break;
    case P_JOINED:
        snprintf(sub, sizeof sub, "You are connected to %s.", N.pick);
        break;
    case P_TROUBLE:
        switch (N.trouble) {
        case T_PASSWORD:
            subtext = "That password did not work. Try typing it again?";
            break;
        case T_GONE:
            snprintf(sub, sizeof sub,
                     "%s is not there any more. Try another one?", N.pick);
            break;
        case T_NOTALLOWED:
            subtext = "This computer will not let the wifi be changed here.";
            break;
        case T_NOWIFI:
            subtext = "This computer has no wifi of its own.";
            break;
        case T_NOANSWER:
            subtext = "This computer could not look for wifi just now.";
            break;
        case T_NOTOOL:
            subtext = "This computer cannot search for wifi.";
            break;
        default:
            subtext = "The wifi did not join. You can try again.";
            break;
        }
        break;
    }
    if (bodyf && subtext && *subtext)
        shell_text_elided(s, bodyf, (float)g.gx, (float)g.sub_y,
                          (float)g.cw, subtext, c->fg, 0.85f);

    /* A second line for the cases where "what happened" is not enough
     * and she needs "and here is what you do" -- rule 8. */
    const char *advice = NULL;
    if (N.page == P_PASSWORD)
        advice = "It is usually printed on the bottom of your internet box.";
    else if (N.page == P_TROUBLE && N.trouble == T_NOWIFI)
        advice = "A cable from your internet box to this computer will work.";
    else if (N.page == P_TROUBLE && N.trouble == T_NOANSWER)
        advice = "It may still be starting up. Wait a moment and try again.";
    else if (N.page == P_TROUBLE && N.trouble == T_NOTALLOWED)
        advice = "The person who set this computer up can change it.";
    else if (N.page == P_TROUBLE && N.trouble == T_NOTOOL)
        advice = "A cable from your internet box to this computer will work.";
    if (advice && tiny) {
        /* On the password screen it sits just above the box, wherever
         * the box ended up; anywhere else it follows the line above
         * it. Either way it is derived from something else's position
         * rather than from a constant, so it cannot be painted through
         * whatever moved. */
        int ay = (N.page == P_PASSWORD) ? g.field.y - (int)(14.f * k)
                                        : g.sub_y + (int)(26.f * k);
        shell_text_elided(s, tiny, (float)g.gx, (float)ay, (float)g.cw,
                          advice, c->fg, 0.6f);
    }

    /* ── the list ─────────────────────────────────────────────── */
    if (N.page == P_LIST) {
        int from = clampi_(N.first_row, 0, N.n_aps > 0 ? N.n_aps - 1 : 0);
        for (int i = 0; i < g.n_rows; i++) {
            const net_ap *a = &N.aps[from + i];
            rect r = g.rows[i];
            if (N.sel == from + i) draw_rect(s, r, c->surface_hi, 1.f);
            draw_hrule(s, r.x, r.y + r.h - 1, r.w, 1, c->fg, 0.12f);

            float by = shell_baseline(name, (float)r.y, (float)r.h);
            int right = r.x + r.w;
            int bars_x = right - (int)(46.f * k);

            /* What this row says on its right, when it has something
             * to say. A wifi that wants a password is every wifi, and
             * six rows each carrying the words "Password needed" is
             * six repetitions of a thing she already assumed -- noise
             * in the one place she is trying to read one name. The
             * news is the opposite: this is the one you are on, this
             * is one you have joined before, this one will let you
             * straight in. */
            const char *tag = a->in_use ? "Connected"
                            : a->known  ? "Saved"
                            : !a->secure ? "No password"
                                         : NULL;
            float tagw = (tag && tiny) ? shell_text_w(tiny, tag) : 0.f;
            if (tag && tiny)
                shell_text(s, tiny, (float)bars_x - tagw - 12.f,
                           by, tag, c->fg, a->in_use ? 0.9f : 0.55f);

            draw_bars(s, bars_x, r.y + r.h / 2, k, a->signal, c->fg, c->fg);

            float room = (float)(bars_x - r.x) - tagw - 28.f;
            if (room < 40.f) room = 40.f;   /* a name squeezed to
                                             * nothing draws nothing,
                                             * and a row with no name
                                             * is a row she cannot use */
            if (name)
                shell_text_elided(s, name, (float)r.x + 4.f, by, room,
                                  a->name, c->fg, 1.f);
        }
        /* How far down the list she is, on the line that already says
         * what to do -- right-aligned, so it sits opposite the
         * instruction rather than under the last row. Under the last
         * row is where the buttons are, and at the largest text that
         * is precisely where this used to be painted. */
        if (N.n_aps > g.n_rows && tiny) {
            char more[64];
            snprintf(more, sizeof more, "%d of %d",
                     from + g.n_rows, N.n_aps);
            float w = shell_text_w(tiny, more);
            shell_text(s, tiny, (float)(g.gx + g.cw) - w, (float)g.sub_y,
                       more, c->fg, 0.5f);
        }
    }

    /* ── the password, shown as she types it ──────────────────────
     *
     * Not dots. She is at her own machine, typing a string off the
     * underside of a box, and a field she cannot read back is a field
     * she cannot correct -- which is the single commonest reason an
     * unpractised typist fails to join a network they have the password
     * for. A kiosk never reaches this screen at all; the band has no
     * Internet button in a kiosk build.
     */
    if (N.page == P_PASSWORD) {
        rect fr = g.field;
        draw_rect(s, fr, c->surface_c, 1.f);
        draw_frame(s, fr, 2, c->accent, 0.9f);
        font *big = f->big ? f->big : name;
        if (big) {
            const char *shown = N.pw_n ? N.pw : "";
            shell_text_elided(s, big, (float)fr.x + 16.f,
                              shell_baseline(big, (float)fr.y, (float)fr.h),
                              (float)fr.w - 32.f, shown, c->fg, 1.f);
            /* A bar where the next letter will go, so an empty box
             * reads as "type here" rather than as a broken one. */
            float w = N.pw_n ? shell_text_w(big, N.pw) : 0.f;
            if (w > (float)fr.w - 40.f) w = (float)fr.w - 40.f;
            rect car = { fr.x + 16 + (int)w + 2, fr.y + 10, 2, fr.h - 20 };
            draw_rect(s, car, c->accent, 0.9f);
        }
    }

    /* ── the buttons ──────────────────────────────────────────── */
    for (int i = 0; i < g.n_acts; i++) {
        int a = g.act[i];
        int on = act_enabled(a);
        int primary = on && ((a == A_JOIN) ||
                             (a == A_CLOSE && N.page == P_JOINED));
        paint_action(s, f, c, g.acts[i], ACT_LABEL[a],
                     N.hover_act == a, primary, on);
    }
}

/* ── input ──────────────────────────────────────────────────────── */

static int in_rect(rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void net_motion(shell_ctx *c, int x, int y)
{
    if (!c->net_open) { N.hover_act = -1; return; }
    /* The pointer speaks only when it has actually moved. */
    if (N.moved_once && x == N.last_x && y == N.last_y) return;
    N.last_x = x; N.last_y = y; N.moved_once = 1;

    N.hover_act = -1;
    net_geom g;
    net_view v = view_now();
    net_layout(c, c->screen_w, c->screen_h, &v, &g);
    for (int i = 0; i < g.n_acts; i++)
        if (in_rect(g.acts[i], x, y)) {
            if (act_enabled(g.act[i])) N.hover_act = g.act[i];
            N.sel = -1;                  /* she is reaching for a button */
            return;
        }
    int from = clampi_(N.first_row, 0, N.n_aps > 0 ? N.n_aps - 1 : 0);
    N.sel = -1;
    for (int i = 0; i < g.n_rows; i++)
        if (in_rect(g.rows[i], x, y)) { N.sel = from + i; return; }
}

/* She picked a network. If we have joined it before, or it wants no
 * password at all, there is nothing to ask her: join it. Asking for a
 * password she has already given once is the machine forgetting, and
 * she will assume she got it wrong. */
static void pick_ap(const net_ap *a)
{
    snprintf(N.pick, NET_NAME_MAX, "%s", a->name);
    N.pick_secure = a->secure;
    N.pw[0] = 0; N.pw_n = 0;
    if (!a->secure || a->known) start_join();
    else N.page = P_PASSWORD;
}

static void do_action(shell_ctx *c, int a)
{
    switch (a) {
    case A_CLOSE:
        /* Only the flag. The host notices it change and does the rest
         * -- stopping whatever nmcli is running, forgetting the
         * password, handing the keyboard back -- in the one place that
         * also handles the band's button and Escape. Three doors, one
         * piece of bookkeeping. */
        c->net_open = 0;
        break;
    case A_JOIN:
        if (N.pw_n > 0 || !N.pick_secure) start_join();
        break;
    case A_BACK:
        child_stop();
        memset(N.pw, 0, sizeof N.pw); N.pw_n = 0;
        N.page = P_LIST;
        break;
    case A_MORE: {
        net_geom g;
        net_view v = view_now();
        net_layout(c, c->screen_w, c->screen_h, &v, &g);
        N.first_row += g.n_vis;
        if (N.first_row >= N.n_aps) N.first_row = 0;   /* wraps, so the
                                                        * last page is
                                                        * never a dead
                                                        * end */
        break;
    }
    case A_AGAIN:
        if (N.page == P_TROUBLE && N.trouble == T_PASSWORD) {
            N.pw[0] = 0; N.pw_n = 0;
            N.page = P_PASSWORD;
            break;
        }
        /* Start the whole sequence again, from asking the machine what
         * it has. Restarting only the scan was enough when the scan was
         * the thing that failed, and wrong every other time: pressing
         * Try again during the opening handshake -- which is exactly
         * when an impatient person presses it, because the panel is
         * still blank -- cut the chain that asks which networks this
         * computer has joined before. Every one of them then showed as
         * new and asked for a password she had already given, which is
         * the failure this panel exists to avoid. */
        N.page = P_LIST;
        N.first_row = 0;
        N.sel = -1;
        start_job(J_DEVICES);
        break;
    case A_STOP:
        child_stop();
        /* Killing nmcli does not cancel the join: NetworkManager was
         * asked and goes on trying, so she would press Stop, be put
         * back on the list, and find the machine connected anyway. A
         * button called Stop has to stop the thing it is under. */
        if (N.wifi_dev[0]) {
            const char *off[] = { "nmcli", "device", "disconnect",
                                  N.wifi_dev, NULL };
            child_start(J_RADIO, off);   /* its result is not read; the
                                          * next list says the truth */
        }
        N.page = P_LIST;
        break;
    }
}

int net_click(shell_ctx *c, int x, int y)
{
    if (!c->net_open) return 0;
    if (y >= c->screen_h) return 0;         /* the band's, not ours */

    net_geom g;
    net_view v = view_now();
    net_layout(c, c->screen_w, c->screen_h, &v, &g);

    for (int i = 0; i < g.n_acts; i++)
        if (in_rect(g.acts[i], x, y)) {
            if (act_enabled(g.act[i])) do_action(c, g.act[i]);
            return 1;                  /* taken either way; a press that
                                        * fell through to the desktop
                                        * would be worse than one that
                                        * did nothing */
        }

    if (N.page == P_LIST) {
        int from = clampi_(N.first_row, 0, N.n_aps > 0 ? N.n_aps - 1 : 0);
        for (int i = 0; i < g.n_rows; i++)
            if (in_rect(g.rows[i], x, y)) {
                N.sel = from + i;
                pick_ap(&N.aps[N.sel]);
                return 1;
            }
    }
    /* Everything else inside the panel is swallowed. A press that fell
     * between two rows must not reach the desktop underneath and start
     * a program she cannot see. */
    return 1;
}

int net_key(shell_ctx *c, int k)
{
    if (!c->net_open) return 0;

    /* evdev: 1 Escape, 28 Return, 14 Backspace, 103/108 up/down. */
    if (k == 1) {
        /* Escape does whatever this screen's second button does, so
         * that the keyboard and the pointer agree. It used to always
         * mean "back to the list", which on the screens that say "this
         * computer has no wifi" put her in front of "No wifi found near
         * this computer" -- a different claim, and a false one. */
        if (N.page == P_LIST || N.page == P_JOINED) { c->net_open = 0; return 1; }
        if (N.page == P_JOINING) { do_action(c, A_STOP); return 1; }
        if (N.page == P_TROUBLE &&
            (N.trouble == T_NOWIFI || N.trouble == T_NOTOOL ||
             N.trouble == T_NOANSWER || N.trouble == T_NOTALLOWED)) {
            c->net_open = 0; return 1;
        }
        do_action(c, A_BACK);
        return 1;
    }

    if (N.page == P_PASSWORD) {
        if (k == 28) { do_action(c, A_JOIN); return 1; }
        if (k == 14) {
            while (N.pw_n > 0) {
                unsigned char b = (unsigned char)N.pw[--N.pw_n];
                N.pw[N.pw_n] = 0;
                if ((b & 0xC0) != 0x80) break;   /* whole character */
            }
            return 1;
        }
        /* The keymap's answer, or the plain table when there is no
         * keymap to ask. A machine whose compositor failed to start
         * can run no applications at all, and the one thing still
         * worth doing on it is getting it onto the network so somebody
         * can repair it -- which needs a box that accepts characters. */
        char one[2];
        const char *t = c->key_text;
        if (!*t) {
            char ch = shell_key_char(k);
            one[0] = ch; one[1] = 0;
            t = ch ? one : "";
        }
        size_t len = strlen(t);
        if (len && N.pw_n + (int)len < PW_MAX - 1) {
            memcpy(N.pw + N.pw_n, t, len);
            N.pw_n += (int)len;
            N.pw[N.pw_n] = 0;
        }
        return 1;
    }

    if (N.page == P_LIST && N.n_aps > 0) {
        if (k == 103 || k == 108) {
            if (N.sel < 0) N.sel = N.first_row;
            else N.sel += (k == 108) ? 1 : -1;
            N.sel = clampi_(N.sel, 0, N.n_aps - 1);
            /* And the list follows her, or the selection walks off the
             * page and she is choosing something she cannot see. */
            net_geom g;
            net_view v = view_now();
            net_layout(c, c->screen_w, c->screen_h, &v, &g);
            if (N.sel < N.first_row) N.first_row = N.sel;
            else if (g.n_vis > 0 && N.sel >= N.first_row + g.n_vis)
                N.first_row = N.sel - g.n_vis + 1;
            return 1;
        }
        if (k == 28 && N.sel >= 0 && N.sel < N.n_aps) {
            pick_ap(&N.aps[N.sel]); return 1;
        }
    }
    /* While the panel is up it owns the keyboard. A keystroke that
     * slipped past it would land in whatever the desktop does with
     * typing, which in one archetype is "open the finder". */
    return 1;
}

/* ── what a harness can measure ─────────────────────────────────────
 *
 * Every rectangle this panel puts under her finger, for a view it is
 * handed rather than the one it happens to be in. Not quite the same
 * as "every rectangle she can press": the password box is here because
 * how big it is decides whether she can READ back what she typed, and
 * pressing it does nothing -- there is no on-screen keyboard in this
 * product for it to summon. tools/targets.c
 * walks all five screens at four text sizes and three panel sizes
 * through this, which is the only way docs/EASY.md rule 4 -- 44 pixels
 * at 1024x600 -- is a check rather than an intention.
 */
int net_targets(const shell_ctx *c, int sw, int sh, const net_view *v,
                rect *out, int max)
{
    net_geom g;
    net_layout(c, sw, sh, v, &g);
    int n = 0;
    for (int i = 0; i < g.n_rows && n < max; i++) out[n++] = g.rows[i];
    if (v->page == P_PASSWORD && n < max) out[n++] = g.field;
    for (int i = 0; i < g.n_acts && n < max; i++) out[n++] = g.acts[i];
    return n;
}
