/* run.c — see run.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#include "run.h"

/* Children started and not yet collected. Small on purpose: these are
 * one-shot helpers that live for milliseconds, and more than a handful
 * outstanding means something is wrong rather than busy. */
#define REAP_MAX 16
static pid_t reaping[REAP_MAX];
static int   n_reaping;

void run_reap(void)
{
    for (int i = n_reaping - 1; i >= 0; i--) {
        pid_t r = waitpid(reaping[i], NULL, WNOHANG);
        /* >0 it exited. <0 with ECHILD means it is not ours any more.
         * Any other error means a LIVE child, which must not be
         * forgotten -- a forgotten child is a process nothing will
         * ever collect. */
        if (r > 0 || (r < 0 && errno == ECHILD))
            reaping[i] = reaping[--n_reaping];
    }
}

static void remember(pid_t p)
{
    if (p <= 0) return;
    run_reap();
    if (n_reaping < REAP_MAX) { reaping[n_reaping++] = p; return; }
    /* Full. Make room by stopping the OLDEST, not the one we were just
     * handed: under pressure, killing the newest kills the thing the
     * person just asked for -- the volume change they are dragging --
     * and keeps the stuck one that caused the pressure. */
    pid_t oldest = reaping[0];
    kill(oldest, SIGKILL);
    waitpid(oldest, NULL, 0);
    for (int i = 1; i < n_reaping; i++) reaping[i - 1] = reaping[i];
    reaping[n_reaping - 1] = p;
}

/* The child half of both shapes. `pipe_w` is the fd to become stdout
 * and stderr, or -1 for "throw it away". */
static void child(const char *const argv[], int pipe_w)
{
    int devnull = -1;
    if (pipe_w < 0) {
        devnull = open("/dev/null", O_WRONLY);
        pipe_w = devnull;
    }
    if (pipe_w >= 0) { dup2(pipe_w, 1); dup2(pipe_w, 2); }
    if (pipe_w > 2) close(pipe_w);
    if (devnull > 2 && devnull != pipe_w) close(devnull);
    /* Its own session, so a helper that misbehaves cannot take the
     * desktop's terminal with it. */
    setsid();
    signal(SIGPIPE, SIG_DFL);
    /* Plain output in a known language: callers parse what comes back,
     * and a translated word is a different string. */
    setenv("LC_ALL", "C", 1);
    execvp(argv[0], (char *const *)argv);
    _exit(127);
}

int run_detached(const char *const argv[])
{
    if (!argv || !argv[0] || !argv[0][0]) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { child(argv, -1); }
    remember(pid);
    return 0;
}

/* ONE SESSION BUS, NOT TWO.
 *
 * aurshell.service runs under `dbus-run-session`, which makes a bus
 * and exports DBUS_SESSION_BUS_ADDRESS pointing at it. That is there
 * because a program with no bus and no X11 $DISPLAY cannot autolaunch
 * one, and every GTK application on this machine wants a session bus.
 *
 * But logind ALSO gives this user a bus, at $XDG_RUNTIME_DIR/bus, and
 * that is the one everything started by `systemd --user` is on --
 * wireplumber, pipewire-pulse, any portal. So the shell's own
 * applications sat on one bus and the rest of the desktop on another.
 *
 * The obvious fix -- order the unit after the user's bus -- is
 * impossible, and worth writing down: the user manager is started BY
 * the logind session, and the logind session is created by THIS
 * unit's PAM stack. Nothing can be ordered before the thing that
 * causes it.
 *
 * So the shell adopts the real bus when it turns up, which is a
 * second or so into the boot and long before she clicks anything. The
 * wrapper stays, because a machine where the user manager never
 * starts must still have a bus rather than none.
 *
 * Returns 1 the first time it switches, so the caller can say so.
 */
int run_adopt_user_bus(void)
{
    static int done = 0;
    if (done) return 0;
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) return 0;

    char path[320];
    snprintf(path, sizeof path, "%s/bus", rd);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) return 0;

    char addr[384];
    snprintf(addr, sizeof addr, "unix:path=%s", path);
    done = 1;
    const char *cur = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (cur && !strcmp(cur, addr)) return 0;      /* already the one */
    if (setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1) != 0) return 0;
    return 1;
}

int run_status(const char *const argv[], int timeout_ms)
{
    if (!argv || !argv[0] || !argv[0][0]) return RUN_NOSTART;
    pid_t pid = fork();
    if (pid < 0) return RUN_NOSTART;
    if (pid == 0) { child(argv, -1); }

    /* Poll rather than block, so a helper that never exits costs the
     * deadline and not the desktop. 10ms is far finer than a person
     * can see and far coarser than a spin. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int budget = timeout_ms < 0 ? 0 : timeout_ms;
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(st))   return WEXITSTATUS(st);
            if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
            return RUN_RUNNING;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            /* It is gone and somebody else collected it, which cannot
             * happen in this program (every reaper waits on its own
             * pid table) but is not worth asserting. Nothing is known
             * about how it ended, so nothing is claimed. */
            return RUN_RUNNING;
        }

        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        long spent = (tn.tv_sec - t0.tv_sec) * 1000L
                   + (tn.tv_nsec - t0.tv_nsec) / 1000000L;
        if (spent >= budget) {
            /* Left alive on purpose: the caller's deadline is about how
             * long the SCREEN waits, and `systemctl poweroff` taking
             * longer than that is normal. remember() collects it. */
            remember(pid);
            return RUN_RUNNING;
        }
        struct timespec nap = { 0, 10 * 1000 * 1000 };
        nanosleep(&nap, NULL);
    }
}

int run_capture(const char *const argv[], char *out, size_t n, int timeout_ms)
{
    if (out && n) out[0] = 0;
    if (!argv || !argv[0] || !argv[0][0] || !out || n < 2) return -1;

    int p[2];
    if (pipe(p) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) { close(p[0]); child(argv, p[1]); }

    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);

    size_t got = 0;
    /* A DEADLINE, not a per-poll timeout.
     *
     * The first version passed the same `timeout_ms` to every poll(),
     * so any child that dribbled its answer out reset the clock on
     * every byte. The real bound was (bytes - 1) x timeout: a 128-byte
     * buffer at 400ms is fifty seconds of a single-threaded shell that
     * is not painting and not reading input. Reachable from one press
     * of the Settings button. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int budget = timeout_ms < 0 ? 0 : timeout_ms;
    for (;;) {
        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        long spent = (tn.tv_sec - t0.tv_sec) * 1000L
                   + (tn.tv_nsec - t0.tv_nsec) / 1000000L;
        int left = budget - (int)spent;
        if (left < 0) left = 0;

        struct pollfd pf = { p[0], POLLIN, 0 };
        int r = poll(&pf, 1, left);
        if (r == 0) {                       /* out of time */
            kill(pid, SIGKILL);
            break;
        }
        if (r < 0) { if (errno == EINTR) continue; break; }

        ssize_t k = read(p[0], out + got, n - 1 - got);
        if (k > 0) {
            got += (size_t)k;
            if (got >= n - 1) { kill(pid, SIGKILL); break; }
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (k < 0 && errno == EINTR) continue;
        break;                              /* 0 = it is finished */
    }
    out[got] = 0;
    close(p[0]);

    /* It has closed the pipe or been killed, so this does not hang --
     * but WNOHANG first, and only then wait, so the common case costs
     * nothing. */
    if (waitpid(pid, NULL, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    return (int)got;
}
