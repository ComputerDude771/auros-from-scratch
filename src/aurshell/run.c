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
    /* Unreachable in practice. Taking the blunt way out beats leaving
     * something behind. */
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
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
    int left = timeout_ms < 0 ? 0 : timeout_ms;
    for (;;) {
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
