/* ═══════════════════════════════════════════════════════════════════
 *  aurinit — PID 1 for AurOS
 *
 *  Responsibilities, in the order they happen:
 *    1. Mount the pseudo-filesystems nothing else can work without.
 *    2. Read service units from the /etc/auros/services directory.
 *    3. Start them in dependency order, supervise them, restart what
 *       dies (with backoff, so a crash loop cannot melt the machine).
 *    4. Reap orphans — PID 1 inherits every orphaned process on the
 *       system, and a PID 1 that does not reap leaks zombies forever.
 *    5. Answer aurctl over a control socket.
 *    6. Shut down cleanly: TERM, grace, KILL, sync, unmount, reboot.
 *
 *  Design notes:
 *    - Signals are handled through signalfd rather than handlers, so
 *      the main loop is a single poll() over [signalfd, control socket]
 *      with no async-signal-safety minefield.
 *    - PID 1 must never exit. Every error path here either recovers or
 *      falls through to a rescue shell; returning from main() would
 *      panic the kernel.
 * ═══════════════════════════════════════════════════════════════════ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>


#define MAX_SERVICES 128
#define MAX_DEPS      16
#define MAX_ARGV      32
#define CTRL_SOCKET  "/run/auros/init.sock"
#define SERVICE_DIR  "/etc/auros/services"

/* Restart-storm guard: more than BURST restarts inside WINDOW seconds
 * and the unit is parked as failed rather than retried forever. */
#define RESTART_BURST   5
#define RESTART_WINDOW 60
#define STOP_GRACE_SEC  5

enum state { S_INACTIVE, S_RUNNING, S_EXITED, S_FAILED, S_STOPPED };
enum type  { T_SIMPLE, T_ONESHOT };
enum restart_policy { R_NEVER, R_ON_FAILURE, R_ALWAYS };

typedef struct {
    char name[64];
    char desc[128];
    char exec[512];
    char tty[64];
    char after[MAX_DEPS][64];
    int  n_after;
    enum type  type;
    enum restart_policy restart;
    enum state state;
    pid_t pid;
    int  exit_code;
    int  restart_count;
    time_t first_restart;
    int  enabled;
} service;

static service svc[MAX_SERVICES];
static int n_svc = 0;
static volatile int shutting_down = 0;
static int reboot_cmd = RB_AUTOBOOT;

/* ── logging ─────────────────────────────────────────────────────── */
static const char *C_OK   = "\033[38;2;125;211;192m";
static const char *C_ERR  = "\033[38;2;242;120;141m";
static const char *C_DIM  = "\033[38;2;110;120;134m";
static const char *C_RST  = "\033[0m";

static void logmsg(const char *tag, const char *color, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s[%s]%s ", color, tag, C_RST);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define ok(...)   logmsg(" ok ", C_OK,  __VA_ARGS__)
#define fail(...) logmsg("fail", C_ERR, __VA_ARGS__)
#define info(...) logmsg("    ", C_DIM, __VA_ARGS__)

/* ── early mounts ────────────────────────────────────────────────── */
struct mnt { const char *src, *dst, *fs; unsigned long flags; const char *opts; };

static const struct mnt early_mounts[] = {
    { "proc",     "/proc",          "proc",     MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL },
    { "sysfs",    "/sys",           "sysfs",    MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL },
    { "devtmpfs", "/dev",           "devtmpfs", MS_NOSUID,                    "mode=0755" },
    { "devpts",   "/dev/pts",       "devpts",   MS_NOSUID|MS_NOEXEC,          "gid=5,mode=620" },
    { "tmpfs",    "/dev/shm",       "tmpfs",    MS_NOSUID|MS_NODEV,           "mode=1777" },
    { "tmpfs",    "/run",           "tmpfs",    MS_NOSUID|MS_NODEV,           "mode=0755" },
    { "tmpfs",    "/tmp",           "tmpfs",    MS_NOSUID|MS_NODEV,           "mode=1777" },
    { "cgroup2",  "/sys/fs/cgroup", "cgroup2",  MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL },
};

static void do_early_mounts(void)
{
    for (size_t i = 0; i < sizeof early_mounts / sizeof early_mounts[0]; i++) {
        const struct mnt *m = &early_mounts[i];
        mkdir(m->dst, 0755);
        if (mount(m->src, m->dst, m->fs, m->flags, m->opts) != 0 && errno != EBUSY)
            /* cgroup2 is optional; the rest are not, but there is no
             * recovery from here, so log and keep going to the shell. */
            info("mount %s: %s", m->dst, strerror(errno));
    }
    mkdir("/run/auros", 0755);
    /* /dev/console exists from devtmpfs; these are conveniences. */
    symlink("/proc/self/fd",   "/dev/fd");
    symlink("/proc/self/fd/0", "/dev/stdin");
    symlink("/proc/self/fd/1", "/dev/stdout");
    symlink("/proc/self/fd/2", "/dev/stderr");
}

/* ── unit parsing ────────────────────────────────────────────────── */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int load_service(const char *path, const char *fname)
{
    if (n_svc >= MAX_SERVICES) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    service *s = &svc[n_svc];
    memset(s, 0, sizeof *s);
    s->type = T_SIMPLE;
    s->restart = R_ON_FAILURE;
    s->state = S_INACTIVE;
    s->enabled = 1;
    /* Default the unit name to the filename minus .service. */
    snprintf(s->name, sizeof s->name, "%.*s",
             (int)(strrchr(fname, '.') ? strrchr(fname, '.') - fname : (long)strlen(fname)),
             fname);

    char line[640];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p), *v = trim(eq + 1);

        if      (!strcmp(k, "name"))        snprintf(s->name, sizeof s->name, "%s", v);
        else if (!strcmp(k, "description")) snprintf(s->desc, sizeof s->desc, "%s", v);
        else if (!strcmp(k, "exec"))        snprintf(s->exec, sizeof s->exec, "%s", v);
        else if (!strcmp(k, "tty"))         snprintf(s->tty,  sizeof s->tty,  "%s", v);
        else if (!strcmp(k, "type"))        s->type = !strcmp(v, "oneshot") ? T_ONESHOT : T_SIMPLE;
        else if (!strcmp(k, "enabled"))     s->enabled = !(!strcmp(v, "no") || !strcmp(v, "0"));
        else if (!strcmp(k, "restart")) {
            if      (!strcmp(v, "always"))  s->restart = R_ALWAYS;
            else if (!strcmp(v, "never"))   s->restart = R_NEVER;
            else                            s->restart = R_ON_FAILURE;
        } else if (!strcmp(k, "after")) {
            char *tok = strtok(v, " \t,");
            while (tok && s->n_after < MAX_DEPS) {
                snprintf(s->after[s->n_after++], 64, "%s", tok);
                tok = strtok(NULL, " \t,");
            }
        }
    }
    fclose(f);
    if (!s->exec[0]) return -1;
    n_svc++;
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(((const service *)a)->name, ((const service *)b)->name);
}

static void load_services(void)
{
    DIR *d = opendir(SERVICE_DIR);
    if (!d) { fail("no %s", SERVICE_DIR); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 9 || strcmp(e->d_name + l - 8, ".service")) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", SERVICE_DIR, e->d_name);
        load_service(path, e->d_name);
    }
    closedir(d);
    /* Deterministic order makes boot logs comparable between runs. */
    qsort(svc, n_svc, sizeof svc[0], cmp_name);
}

static service *find_svc(const char *name)
{
    for (int i = 0; i < n_svc; i++)
        if (!strcmp(svc[i].name, name)) return &svc[i];
    return NULL;
}

/* ── starting ────────────────────────────────────────────────────── */
static int deps_ready(const service *s)
{
    for (int i = 0; i < s->n_after; i++) {
        service *d = find_svc(s->after[i]);
        if (!d) continue;                       /* absent dep: ignore */
        if (!d->enabled) continue;
        /* A simple unit satisfies dependents once it is running; a
         * oneshot must have run to completion successfully. */
        if (d->type == T_ONESHOT) {
            if (d->state != S_EXITED) return 0;
        } else {
            if (d->state != S_RUNNING) return 0;
        }
    }
    return 1;
}

static void setup_tty(const char *tty)
{
    if (!tty || !*tty) return;
    int fd = open(tty, O_RDWR | O_NOCTTY);
    if (fd < 0) return;
    /* New session, then claim the tty as controlling terminal, so job
     * control works for a login shell started here. */
    setsid();
    ioctl(fd, TIOCSCTTY, 1);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    if (fd > 2) close(fd);
}

static void start_service(service *s)
{
    if (!s->enabled) return;

    char buf[512];
    snprintf(buf, sizeof buf, "%s", s->exec);
    char *argv[MAX_ARGV];
    int argc = 0;
    char *tok = strtok(buf, " \t");
    while (tok && argc < MAX_ARGV - 1) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
    argv[argc] = NULL;
    if (!argc) { s->state = S_FAILED; return; }

    pid_t pid = fork();
    if (pid < 0) { fail("%s: fork: %s", s->name, strerror(errno)); s->state = S_FAILED; return; }

    if (pid == 0) {
        /* Child: restore default signal disposition — PID 1 blocks
         * everything, and a child inheriting that mask would ignore
         * signals its own supervisor sends it. */
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        for (int i = 1; i < NSIG; i++) signal(i, SIG_DFL);

        setup_tty(s->tty);
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin", 1);
        execvp(argv[0], argv);
        fprintf(stderr, "aurinit: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    s->pid = pid;
    s->state = S_RUNNING;
    if (s->type == T_SIMPLE)
        ok("%s", s->desc[0] ? s->desc : s->name);
}

/* Repeatedly sweep for units whose dependencies are now satisfied.
 * Oneshots block their dependents, so this is called again from the
 * SIGCHLD path as units complete. */
static void start_ready(void)
{
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < n_svc; i++) {
            service *s = &svc[i];
            if (s->state != S_INACTIVE || !s->enabled) continue;
            if (!deps_ready(s)) continue;
            start_service(s);
            progress = 1;
        }
    }
}

/* ── child reaping ───────────────────────────────────────────────── */
static void reap_children(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        service *s = NULL;
        for (int i = 0; i < n_svc; i++)
            if (svc[i].pid == pid) { s = &svc[i]; break; }
        /* Not one of ours: an orphan the kernel reparented to PID 1.
         * Reaping it is the whole reason init must call waitpid. */
        if (!s) continue;

        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        s->exit_code = code;
        s->pid = 0;

        if (shutting_down) { s->state = S_STOPPED; continue; }

        if (s->type == T_ONESHOT) {
            if (code == 0) { s->state = S_EXITED; ok("%s", s->desc[0] ? s->desc : s->name); }
            else           { s->state = S_FAILED; fail("%s (exit %d)", s->name, code); }
            start_ready();                       /* may unblock dependents */
            continue;
        }

        int want_restart =
            s->restart == R_ALWAYS ||
            (s->restart == R_ON_FAILURE && code != 0);

        if (!want_restart) { s->state = S_EXITED; continue; }

        time_t now = time(NULL);
        if (s->first_restart == 0 || now - s->first_restart > RESTART_WINDOW) {
            s->first_restart = now;
            s->restart_count = 0;
        }
        if (++s->restart_count > RESTART_BURST) {
            s->state = S_FAILED;
            fail("%s restarting too fast (%d in %ds) — parked",
                 s->name, s->restart_count, RESTART_WINDOW);
            continue;
        }
        if (code != 0)
            fail("%s exited %d, restarting (%d/%d)",
                 s->name, code, s->restart_count, RESTART_BURST);
        s->state = S_INACTIVE;
        start_service(s);
    }
}

/* ── shutdown ────────────────────────────────────────────────────── */
static void do_shutdown(int cmd)
{
    shutting_down = 1;
    reboot_cmd = cmd;
    info("stopping services");

    /* Reverse start order approximates reverse dependency order. */
    for (int i = n_svc - 1; i >= 0; i--)
        if (svc[i].pid > 0) kill(svc[i].pid, SIGTERM);

    /* Anything still alive after the grace period gets SIGKILL. */
    for (int waited = 0; waited < STOP_GRACE_SEC * 10; waited++) {
        int alive = 0;
        int status; pid_t p;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0)
            for (int i = 0; i < n_svc; i++)
                if (svc[i].pid == p) { svc[i].pid = 0; svc[i].state = S_STOPPED; }
        for (int i = 0; i < n_svc; i++) if (svc[i].pid > 0) alive++;
        if (!alive) break;
        usleep(100000);
    }
    kill(-1, SIGTERM);
    usleep(300000);
    kill(-1, SIGKILL);

    info("flushing disks");
    sync();
    /* Remount read-only rather than unmount: the root filesystem is in
     * use by definition, and a clean RO remount is what fsck wants. */
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    sync();

    reboot(cmd);
    /* reboot() only returns on failure. There is nothing left to do. */
    fail("reboot syscall failed: %s", strerror(errno));
    for (;;) pause();
}

/* ── control socket (aurctl) ─────────────────────────────────────── */
static int make_ctrl_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTRL_SOCKET);
    unlink(CTRL_SOCKET);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    chmod(CTRL_SOCKET, 0600);
    return fd;
}

static const char *state_name(enum state s)
{
    switch (s) {
        case S_RUNNING:  return "running";
        case S_EXITED:   return "exited";
        case S_FAILED:   return "failed";
        case S_STOPPED:  return "stopped";
        default:         return "inactive";
    }
}

static void handle_ctrl(int fd)
{
    char buf[256];
    struct sockaddr_un from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) return;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';

    char *cmd = strtok(buf, " ");
    char *arg = strtok(NULL, " ");
    if (!cmd) return;

    char reply[4096]; reply[0] = '\0';

    if (!strcmp(cmd, "status")) {
        for (int i = 0; i < n_svc; i++) {
            char line[256];
            snprintf(line, sizeof line, "%-18.18s %-9s pid=%-6d %.120s\n",
                     svc[i].name, state_name(svc[i].state),
                     svc[i].pid, svc[i].desc);
            if (strlen(reply) + strlen(line) < sizeof reply) strcat(reply, line);
        }
    } else if (!strcmp(cmd, "start") && arg) {
        service *s = find_svc(arg);
        if (!s) snprintf(reply, sizeof reply, "no such service: %s\n", arg);
        else if (s->state == S_RUNNING) snprintf(reply, sizeof reply, "already running\n");
        else { s->state = S_INACTIVE; s->restart_count = 0; start_service(s);
               snprintf(reply, sizeof reply, "started %s\n", arg); }
    } else if (!strcmp(cmd, "stop") && arg) {
        service *s = find_svc(arg);
        if (!s || s->pid <= 0) snprintf(reply, sizeof reply, "not running\n");
        else { s->restart = R_NEVER; kill(s->pid, SIGTERM);
               snprintf(reply, sizeof reply, "stopping %s\n", arg); }
    } else if (!strcmp(cmd, "restart") && arg) {
        service *s = find_svc(arg);
        if (!s) snprintf(reply, sizeof reply, "no such service: %s\n", arg);
        else { if (s->pid > 0) kill(s->pid, SIGTERM);
               snprintf(reply, sizeof reply, "restarting %s\n", arg); }
    } else if (!strcmp(cmd, "reboot")) {
        do_shutdown(RB_AUTOBOOT);
    } else if (!strcmp(cmd, "poweroff")) {
        do_shutdown(RB_POWER_OFF);
    } else {
        snprintf(reply, sizeof reply, "unknown command: %s\n", cmd);
    }

    if (reply[0] && fl > sizeof(sa_family_t) && from.sun_path[0])
        sendto(fd, reply, strlen(reply), MSG_DONTWAIT, (struct sockaddr *)&from, fl);
}

/* ── rescue ──────────────────────────────────────────────────────── */
static void rescue_shell(const char *why)
{
    fail("%s", why);
    info("starting rescue shell on /dev/console");
    pid_t p = fork();
    if (p == 0) {
        sigset_t all; sigfillset(&all); sigprocmask(SIG_UNBLOCK, &all, NULL);
        setup_tty("/dev/console");
        execl("/bin/sh", "sh", NULL);
        _exit(127);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (getpid() != 1) {
        fprintf(stderr, "aurinit: must run as PID 1 (use aurctl to talk to init)\n");
        return 1;
    }

    umask(022);
    setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin", 1);

    do_early_mounts();

    /* Repaint the console to the active theme before anything prints,
     * so the boot is themed from the first line. */
    if (access("/etc/auros/console.sh", X_OK) == 0) {
        pid_t p = fork();
        if (p == 0) { execl("/bin/sh", "sh", "/etc/auros/console.sh", NULL); _exit(0); }
        if (p > 0) waitpid(p, NULL, 0);
    }

    fprintf(stderr, "\n%s  AurOS%s %s— booting%s\n\n", C_OK, C_RST, C_DIM, C_RST);

    /* Block everything, then take delivery through signalfd. */
    sigset_t mask;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    sigset_t want;
    sigemptyset(&want);
    sigaddset(&want, SIGCHLD);   /* a child died */
    sigaddset(&want, SIGTERM);   /* reboot  */
    sigaddset(&want, SIGUSR1);   /* halt    */
    sigaddset(&want, SIGUSR2);   /* poweroff*/
    sigaddset(&want, SIGINT);    /* ctrl-alt-del */
    int sfd = signalfd(-1, &want, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) rescue_shell("signalfd failed");

    /* Route Ctrl-Alt-Del to us as SIGINT instead of an instant reset. */
    reboot(RB_DISABLE_CAD);

    load_services();
    info("%d service unit%s", n_svc, n_svc == 1 ? "" : "s");
    if (n_svc == 0) rescue_shell("no service units found");

    int cfd = make_ctrl_socket();
    start_ready();

    struct pollfd pfd[2];
    pfd[0].fd = sfd; pfd[0].events = POLLIN;
    pfd[1].fd = cfd; pfd[1].events = POLLIN;

    for (;;) {
        int n = poll(pfd, cfd >= 0 ? 2 : 1, 1000);
        if (n < 0 && errno != EINTR) { usleep(100000); continue; }

        if (pfd[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == sizeof si) {
                switch (si.ssi_signo) {
                    case SIGCHLD:  reap_children(); break;
                    case SIGTERM:
                    case SIGINT:   do_shutdown(RB_AUTOBOOT); break;
                    case SIGUSR1:  do_shutdown(RB_HALT_SYSTEM); break;
                    case SIGUSR2:  do_shutdown(RB_POWER_OFF); break;
                }
            }
        }
        if (cfd >= 0 && (pfd[1].revents & POLLIN)) handle_ctrl(cfd);

        /* Belt and braces: signalfd can coalesce SIGCHLD, so sweep on
         * every timeout too. A missed reap is a permanent zombie. */
        reap_children();
    }
    /* unreachable — PID 1 must never return */
}
