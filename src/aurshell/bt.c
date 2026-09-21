/* bt.c — see bt.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>

#include "bt.h"
#include "draw.h"
#include "run.h"

#define BT_MAX_DEV 24
#define OUT_MAX    16384

/* Which bluetoothctl run is in flight. */
enum { J_NONE, J_RADIO, J_SCAN, J_LIST, J_PAIR };

static struct {
    int page, trouble;
    bt_dev devs[BT_MAX_DEV];
    int n_devs;
    int have_radio;
    int scanning;
    int sel, first_row, hover_act;
    int last_x, last_y, moved_once;
    char pick[BT_NAME_MAX];
    char pick_addr[20];

    int job;
    pid_t pid;
    int fd;
    char out[OUT_MAX];
    int out_n;
} B = { .sel = -1, .hover_act = -1, .fd = -1, .pid = -1 };

/* ── running bluetoothctl ───────────────────────────────────────── */

#define NOEXEC_MARK "\x01" "btnoexec\n"

#define REAP_MAX 8
static pid_t reaping[REAP_MAX];
static int   n_reaping;

void bt_reap(void)
{
    for (int i = n_reaping - 1; i >= 0; i--) {
        pid_t r = waitpid(reaping[i], NULL, WNOHANG);
        if (r > 0 || (r < 0 && errno == ECHILD))
            reaping[i] = reaping[--n_reaping];
    }
}

static void remember(pid_t p)
{
    if (p <= 0) return;
    bt_reap();
    if (n_reaping < REAP_MAX) { reaping[n_reaping++] = p; return; }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

static void child_stop(void)
{
    if (B.fd >= 0) { close(B.fd); B.fd = -1; }
    if (B.pid > 0) { kill(B.pid, SIGTERM); remember(B.pid); }
    B.pid = -1;
    B.job = J_NONE;
    B.out_n = 0;
    B.scanning = 0;
}

static int child_start(int job, const char *const argv[])
{
    if (B.job != J_NONE) child_stop();
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1); dup2(p[1], 2);
        if (p[1] > 2) close(p[1]);
        setsid();
        signal(SIGPIPE, SIG_DFL);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], (char *const *)argv);
        ssize_t r = write(2, NOEXEC_MARK, sizeof NOEXEC_MARK - 1);
        (void)r;
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    B.fd = p[0]; B.pid = pid; B.job = job; B.out_n = 0;
    return 0;
}

/* ── reading it back ────────────────────────────────────────────────
 *
 * `bluetoothctl devices` prints one per line:
 *
 *     Device AA:BB:CC:DD:EE:FF Ann's Headphones
 *
 * A device that has never announced a name prints its address twice,
 * with the colons turned into dashes. That is not a name and must not
 * be shown as one: a list of six rows reading FC-58-FA-21-03-9C is a
 * list she cannot choose from.
 */
static int looks_like_address(const char *s)
{
    int digits = 0, seps = 0;
    for (const char *p = s; *p; p++) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') ||
            (*p >= 'a' && *p <= 'f')) digits++;
        else if (*p == '-' || *p == ':') seps++;
        else return 0;
    }
    return digits == 12 && seps == 5;
}

int bt_parse_devices(char *out, bt_dev *devs, int max)
{
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "Device ", 7)) continue;
        char *addr = line + 7;
        char *sp = strchr(addr, ' ');
        if (!sp) continue;
        *sp = 0;
        const char *name = sp + 1;
        if (strlen(addr) != 17) continue;
        if (n >= max) break;

        /* Same address twice, or an obvious duplicate: one row. */
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (!strcmp(devs[i].addr, addr)) dup = 1;
        if (dup) continue;

        memset(&devs[n], 0, sizeof devs[n]);
        snprintf(devs[n].addr, sizeof devs[n].addr, "%s", addr);
        if (looks_like_address(name) || !*name)
            devs[n].name[0] = 0;           /* nameless: said so below */
        else
            snprintf(devs[n].name, BT_NAME_MAX, "%s", name);
        n++;
    }
    return n;
}

/* `bluetoothctl info AA:..` prints "\tPaired: yes" and so on. */
int bt_parse_info(char *out, bt_dev *d)
{
    if (strstr(out, "Paired: yes"))    d->paired = 1;
    if (strstr(out, "Connected: yes")) d->connected = 1;
    if (strstr(out, "not available"))  return -1;
    return 0;
}

static int read_trouble(const char *out)
{
    if (strstr(out, NOEXEC_MARK))                   return BTT_NOTOOL;
    if (strstr(out, "No default controller") ||
        strstr(out, "No controller"))               return BTT_NORADIO;
    if (strstr(out, "blocked") || strstr(out, "Blocked")) return BTT_OFF;
    return BTT_FAILED;
}

/* ── the sequence ───────────────────────────────────────────────────
 *
 * IS THERE A RADIO AT ALL, asked of a directory and not of a program.
 *
 * bluetoothctl does not answer this question; it waits for it. On a
 * machine with no controller `bluetoothctl power on` blocks -- not
 * fails, blocks, indefinitely, waiting for one to appear. The panel
 * showed "Nothing found nearby yet" and the advice to hold the button
 * on her headphones, forever, on a computer that has no Bluetooth in
 * it. Found by opening the panel on a booted machine; it cannot be
 * seen from the code, because the code looks correct and the program
 * it calls simply never comes back.
 *
 * The kernel publishes the answer as a directory. An empty one is a
 * machine with no Bluetooth, known instantly, with nothing started.
 */
static int have_adapter(void)
{
    const char *v = getenv("AUROS_BLUETOOTH");
    const char *dir = (v && *v) ? v : "/sys/class/bluetooth";
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') { found = 1; break; }
    closedir(d);
    return found;
}

static void start_job(int job)
{
    switch (job) {
    case J_RADIO: {
        /* She pressed the button; wanting the radio on is not a
         * separate question. Idempotent.
         *
         * --timeout on EVERY invocation, not only the ones that
         * obviously wait: bluetoothctl's habit is to block rather than
         * to fail, and a helper that never returns is a panel that
         * never moves. */
        const char *a[] = { "bluetoothctl", "--timeout", "5",
                            "power", "on", NULL };
        if (child_start(J_RADIO, a) < 0) {
            B.page = BT_TROUBLE; B.trouble = BTT_NOTOOL;
        }
        break;
    }
    case J_SCAN: {
        /* Ten seconds of looking. Anything longer and she has decided
         * the machine is not working; anything shorter and a pair of
         * headphones that announces itself every few seconds is
         * missed. */
        const char *a[] = { "bluetoothctl", "--timeout", "10",
                            "scan", "on", NULL };
        B.scanning = 1;
        child_start(J_SCAN, a);
        break;
    }
    case J_LIST: {
        const char *a[] = { "bluetoothctl", "--timeout", "5",
                            "devices", NULL };
        child_start(J_LIST, a);
        break;
    }
    }
}

static void start_pair(void)
{
    /* connect, not pair: on a device this computer already knows,
     * connect is the whole job, and on one it does not, bluetoothctl
     * pairs first. One verb for what is one act to her. */
    const char *a[] = { "bluetoothctl", "--timeout", "25",
                        "connect", B.pick_addr, NULL };
    B.page = BT_JOINING;
    if (child_start(J_PAIR, a) < 0) { B.page = BT_TROUBLE; B.trouble = BTT_FAILED; }
}

void bt_opened(shell_ctx *c)
{
    (void)c;
    B.page = BT_LIST;
    B.sel = -1; B.first_row = 0; B.hover_act = -1;
    B.moved_once = 0; B.trouble = 0; B.scanning = 0;
    B.n_devs = 0;
    /* Asked of the kernel before anything is started. A machine with
     * no Bluetooth says so at once rather than after a helper that
     * would never have come back. */
    if (!have_adapter()) {
        B.have_radio = 0;
        B.page = BT_TROUBLE;
        B.trouble = BTT_NORADIO;
        return;
    }
    start_job(J_RADIO);
}

void bt_closed(shell_ctx *c) { (void)c; child_stop(); }

void bt_fini(void)
{
    child_stop();
    for (int i = 0; i < n_reaping; i++) {
        if (waitpid(reaping[i], NULL, WNOHANG) != 0) continue;
        kill(reaping[i], SIGKILL);
        waitpid(reaping[i], NULL, 0);
    }
    n_reaping = 0;
    memset(&B, 0, sizeof B);
    B.fd = -1; B.pid = -1; B.sel = -1; B.hover_act = -1;
}

int bt_fd(void) { return B.fd; }

int bt_pump(shell_ctx *c)
{
    (void)c;
    if (B.fd < 0) return 0;
    for (;;) {
        if (B.out_n >= OUT_MAX - 1) { if (B.pid > 0) kill(B.pid, SIGTERM); break; }
        ssize_t r = read(B.fd, B.out + B.out_n, (size_t)(OUT_MAX - 1 - B.out_n));
        if (r > 0) { B.out_n += (int)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    B.out[B.out_n] = 0;
    int job = B.job;
    char *text = B.out;
    close(B.fd); B.fd = -1;
    remember(B.pid); B.pid = -1;
    B.job = J_NONE;

    switch (job) {
    case J_RADIO:
        if (strstr(text, NOEXEC_MARK)) {
            B.page = BT_TROUBLE; B.trouble = BTT_NOTOOL; B.out_n = 0; return 1;
        }
        if (strstr(text, "No default controller")) {
            B.page = BT_TROUBLE; B.trouble = BTT_NORADIO; B.out_n = 0; return 1;
        }
        B.have_radio = 1;
        B.out_n = 0;
        start_job(J_LIST);        /* what it already knows, at once */
        return 1;
    case J_LIST:
        B.n_devs = bt_parse_devices(text, B.devs, BT_MAX_DEV);
        B.out_n = 0;
        if (!B.scanning) start_job(J_SCAN);
        return 1;
    case J_SCAN:
        B.scanning = 0;
        B.out_n = 0;
        start_job(J_LIST);        /* and again, now that it has looked */
        return 1;
    case J_PAIR: {
        int ok = strstr(text, "Connection successful") != NULL ||
                 strstr(text, "Connected: yes") != NULL;
        if (ok) B.page = BT_JOINED;
        else { B.page = BT_TROUBLE; B.trouble = read_trouble(text); }
        B.out_n = 0;
        return 1;
    }
    }
    B.out_n = 0;
    return 0;
}

/* ── where everything is ────────────────────────────────────────────
 *
 * The same contract as the wifi panel's: a pure function of the view
 * and the panel size, so tools/targets.c can ask about a screen this
 * machine is not showing and a radio it does not have.
 */
enum { A_CLOSE, A_AGAIN, A_BACK, A_MORE, A_N };
static const char *ACT_LABEL[A_N] = { "Close", "Look again", "Go back",
                                      "Show more" };

typedef struct {
    int  gx, cw;
    int  head_y, rule_y, sub_y;
    int  list_y, row_h, n_vis;
    rect rows[BT_MAX_DEV]; int n_rows;
    rect acts[A_N]; int act[A_N]; int n_acts;
} bt_geom;

static int clampi_(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static bt_view view_now(void)
{
    bt_view v = { B.page, B.trouble, B.n_devs, B.first_row };
    return v;
}

static void bt_layout(const shell_ctx *c, int sw, int sh,
                      const bt_view *v, bt_geom *g)
{
    memset(g, 0, sizeof *g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    g->gx = clampi_(sw / 14, 20, 110);
    g->cw = sw - 2 * g->gx;
    if (g->cw < 120) { g->gx = 8; g->cw = sw - 16; }

    g->head_y = (int)(52.f * k);
    g->rule_y = g->head_y + (int)(14.f * k);
    g->sub_y  = g->rule_y + (int)(26.f * k);

    int act_h = (int)(34.f + 16.f * k); if (act_h < 44) act_h = 44;
    int act_w = (int)(100.f + 50.f * k);
    int gap   = (int)(10.f * k); if (gap < 8) gap = 8;
    int last_y = sh - (int)(14.f * k) - act_h;
    if (last_y < 0) last_y = 0;

    g->row_h = (int)(30.f + 26.f * k); if (g->row_h < 48) g->row_h = 48;
    g->list_y = g->sub_y + (int)(18.f * k);
    int space = last_y - (int)(14.f * k) - g->list_y;
    g->n_vis = space / g->row_h;
    if (g->n_vis < 1) g->n_vis = 1;
    if (g->n_vis > BT_MAX_DEV) g->n_vis = BT_MAX_DEV;

    int acts_y;
    if (v->page == BT_LIST) {
        int from = clampi_(v->first_row, 0, v->n_devs > 0 ? v->n_devs - 1 : 0);
        for (int i = 0; i < g->n_vis && from + i < v->n_devs; i++) {
            g->rows[g->n_rows] = (rect){ g->gx, g->list_y + i * g->row_h,
                                         g->cw, g->row_h };
            g->n_rows++;
        }
        acts_y = g->list_y + g->n_rows * g->row_h + (int)(16.f * k);
    } else {
        acts_y = g->sub_y + (int)(52.f * k);
    }
    if (acts_y > last_y) acts_y = last_y;
    if (acts_y < g->sub_y + (int)(20.f * k)) acts_y = g->sub_y + (int)(20.f * k);

    int which[A_N], na = 0;
    switch (v->page) {
    case BT_LIST:    which[na++] = A_AGAIN;
                     if (v->n_devs > g->n_vis) which[na++] = A_MORE;
                     which[na++] = A_CLOSE; break;
    case BT_JOINING: which[na++] = A_BACK; break;
    case BT_JOINED:  which[na++] = A_CLOSE; break;
    case BT_TROUBLE: which[na++] = A_AGAIN; which[na++] = A_CLOSE; break;
    }
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

static void paint_action(surface *s, shell_fonts *f, const shell_ctx *c,
                         rect r, const char *label, int hot, int primary)
{
    draw_rect(s, r, primary ? c->fg : (hot ? c->surface_hi : c->surface_c), 1.f);
    if (!primary) draw_frame(s, r, 1, c->subtle, hot ? 0.9f : 0.5f);
    font *ft = f->mid ? f->mid : f->small;
    if (ft)
        shell_text_centred(s, ft, (float)r.x + r.w / 2.f,
                           shell_baseline(ft, (float)r.y, (float)r.h),
                           label, primary ? c->bg : c->fg, 1.f);
}

void bt_paint(shell_ctx *c, surface *s, shell_fonts *f)
{
    if (!c->bt_open) return;
    int sw = s->w, sh = c->screen_h;
    if (sh <= 0 || sh > s->h) sh = s->h;

    bt_view v = view_now();
    bt_geom g;
    bt_layout(c, sw, sh, &v, &g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    draw_rect(s, (rect){ 0, 0, sw, sh }, c->bg, 0.97f);

    font *head = f->huge ? f->huge : (f->big ? f->big : f->mid);
    font *name = f->mid ? f->mid : f->small;
    font *bodyf = f->small ? f->small : f->mid;
    font *tiny = f->tiny ? f->tiny : bodyf;

    if (head)
        shell_text(s, head, (float)g.gx, (float)g.head_y,
                   "Headphones and mice", c->fg, 1.f);
    draw_hrule(s, g.gx, g.rule_y, g.cw, 1, c->fg, 0.28f);

    char sub[160]; sub[0] = 0;
    const char *subtext = sub;
    switch (B.page) {
    case BT_LIST:
        if (B.scanning && B.n_devs == 0) subtext = "Looking for things nearby...";
        else if (B.n_devs == 0)          subtext = "Nothing found nearby yet.";
        else if (B.scanning)             subtext = "Nearby. Still looking...";
        else                             subtext = "Press the one you want to use.";
        break;
    case BT_JOINING:
        snprintf(sub, sizeof sub, "Connecting to %s. This can take a moment.",
                 B.pick);
        break;
    case BT_JOINED:
        snprintf(sub, sizeof sub, "%s is connected.", B.pick);
        break;
    case BT_TROUBLE:
        switch (B.trouble) {
        case BTT_NORADIO:
            subtext = "This computer has no way to connect to these."; break;
        case BTT_NOTOOL:
            subtext = "This computer cannot look for them."; break;
        case BTT_OFF:
            subtext = "This computer's connection to these is switched off."; break;
        default:
            snprintf(sub, sizeof sub, "%s would not connect. Try again?",
                     B.pick[0] ? B.pick : "It");
            break;
        }
        break;
    }
    if (bodyf && subtext && *subtext)
        shell_text_elided(s, bodyf, (float)g.gx, (float)g.sub_y, (float)g.cw,
                          subtext, c->fg, 0.85f);

    const char *advice = NULL;
    if (B.page == BT_LIST && B.n_devs == 0 && !B.scanning)
        advice = "Hold the button on your headphones until the light flashes.";
    else if (B.page == BT_TROUBLE && B.trouble == BTT_NORADIO)
        advice = "Headphones and mice with a cable or a small plug still work.";
    else if (B.page == BT_TROUBLE && B.trouble == BTT_OFF)
        advice = "There is often a switch or a key on the side of the computer.";
    if (advice && tiny)
        shell_text_elided(s, tiny, (float)g.gx,
                          (float)g.sub_y + (int)(26.f * k), (float)g.cw,
                          advice, c->fg, 0.6f);

    if (B.page == BT_LIST) {
        int from = clampi_(B.first_row, 0, B.n_devs > 0 ? B.n_devs - 1 : 0);
        for (int i = 0; i < g.n_rows; i++) {
            const bt_dev *d = &B.devs[from + i];
            rect r = g.rows[i];
            if (B.sel == from + i) draw_rect(s, r, c->surface_hi, 1.f);
            draw_hrule(s, r.x, r.y + r.h - 1, r.w, 1, c->fg, 0.12f);
            float by = shell_baseline(name, (float)r.y, (float)r.h);

            const char *tag = d->connected ? "Connected"
                            : d->paired    ? "Used before"
                                           : NULL;
            float tw = (tag && tiny) ? shell_text_w(tiny, tag) : 0.f;
            if (tag && tiny)
                shell_text(s, tiny, (float)(r.x + r.w) - tw, by, tag,
                           c->fg, d->connected ? 0.9f : 0.55f);
            /* A thing that never said what it is called gets a
             * sentence rather than its address. Six rows of
             * FC-58-FA-21-03-9C is a list she cannot choose from. */
            const char *label = d->name[0] ? d->name
                                           : "Something without a name";
            if (name)
                shell_text_elided(s, name, (float)r.x + 4.f, by,
                                  (float)r.w - tw - 24.f, label,
                                  c->fg, d->name[0] ? 1.f : 0.6f);
        }
        if (B.n_devs > g.n_rows && tiny) {
            char more[48];
            snprintf(more, sizeof more, "%d of %d", from + g.n_rows, B.n_devs);
            float w = shell_text_w(tiny, more);
            shell_text(s, tiny, (float)(g.gx + g.cw) - w, (float)g.sub_y,
                       more, c->fg, 0.5f);
        }
    }

    for (int i = 0; i < g.n_acts; i++)
        paint_action(s, f, c, g.acts[i], ACT_LABEL[g.act[i]],
                     B.hover_act == g.act[i],
                     g.act[i] == A_CLOSE && B.page == BT_JOINED);
}

/* ── input ──────────────────────────────────────────────────────── */

static int in_rect(rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void bt_motion(shell_ctx *c, int x, int y)
{
    if (!c->bt_open) { B.hover_act = -1; return; }
    if (B.moved_once && x == B.last_x && y == B.last_y) return;
    B.last_x = x; B.last_y = y; B.moved_once = 1;

    B.hover_act = -1;
    bt_view v = view_now();
    bt_geom g;
    bt_layout(c, c->screen_w, c->screen_h, &v, &g);
    for (int i = 0; i < g.n_acts; i++)
        if (in_rect(g.acts[i], x, y)) { B.hover_act = g.act[i]; B.sel = -1; return; }
    int from = clampi_(B.first_row, 0, B.n_devs > 0 ? B.n_devs - 1 : 0);
    B.sel = -1;
    for (int i = 0; i < g.n_rows; i++)
        if (in_rect(g.rows[i], x, y)) { B.sel = from + i; return; }
}

static void pick(int idx)
{
    if (idx < 0 || idx >= B.n_devs) return;
    snprintf(B.pick, sizeof B.pick, "%s",
             B.devs[idx].name[0] ? B.devs[idx].name : "That one");
    snprintf(B.pick_addr, sizeof B.pick_addr, "%s", B.devs[idx].addr);
    start_pair();
}

static void do_action(shell_ctx *c, int a)
{
    switch (a) {
    case A_CLOSE: c->bt_open = 0; break;
    case A_BACK:  child_stop(); B.page = BT_LIST; break;
    case A_AGAIN:
        B.page = BT_LIST; B.first_row = 0; B.sel = -1; B.n_devs = 0;
        if (!have_adapter()) { B.page = BT_TROUBLE; B.trouble = BTT_NORADIO; }
        else start_job(J_RADIO);
        break;
    case A_MORE: {
        bt_view v = view_now();
        bt_geom g;
        bt_layout(c, c->screen_w, c->screen_h, &v, &g);
        B.first_row += g.n_vis;
        if (B.first_row >= B.n_devs) B.first_row = 0;
        break;
    }
    }
}

int bt_click(shell_ctx *c, int x, int y)
{
    if (!c->bt_open) return 0;
    if (y >= c->screen_h) return 0;

    bt_view v = view_now();
    bt_geom g;
    bt_layout(c, c->screen_w, c->screen_h, &v, &g);
    for (int i = 0; i < g.n_acts; i++)
        if (in_rect(g.acts[i], x, y)) { do_action(c, g.act[i]); return 1; }
    if (B.page == BT_LIST) {
        int from = clampi_(B.first_row, 0, B.n_devs > 0 ? B.n_devs - 1 : 0);
        for (int i = 0; i < g.n_rows; i++)
            if (in_rect(g.rows[i], x, y)) { B.sel = from + i; pick(B.sel); return 1; }
    }
    return 1;
}

int bt_key(shell_ctx *c, int k)
{
    if (!c->bt_open) return 0;
    if (k == 1) {
        if (B.page == BT_LIST || B.page == BT_JOINED) c->bt_open = 0;
        else if (B.page == BT_JOINING) do_action(c, A_BACK);
        else c->bt_open = 0;
        return 1;
    }
    if (B.page == BT_LIST && B.n_devs > 0) {
        if (k == 103 || k == 108) {
            if (B.sel < 0) B.sel = B.first_row;
            else B.sel += (k == 108) ? 1 : -1;
            B.sel = clampi_(B.sel, 0, B.n_devs - 1);
            bt_view v = view_now();
            bt_geom g;
            bt_layout(c, c->screen_w, c->screen_h, &v, &g);
            if (B.sel < B.first_row) B.first_row = B.sel;
            else if (g.n_vis > 0 && B.sel >= B.first_row + g.n_vis)
                B.first_row = B.sel - g.n_vis + 1;
            return 1;
        }
        if (k == 28 && B.sel >= 0) { pick(B.sel); return 1; }
    }
    return 1;
}

int bt_targets(const shell_ctx *c, int sw, int sh, const bt_view *v,
               rect *out, int max)
{
    bt_geom g;
    bt_layout(c, sw, sh, v, &g);
    int n = 0;
    for (int i = 0; i < g.n_rows && n < max; i++) out[n++] = g.rows[i];
    for (int i = 0; i < g.n_acts && n < max; i++) out[n++] = g.acts[i];
    return n;
}
