/* appwatch.c — see appwatch.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "appwatch.h"

/* The same places apps.c reads, in the same order. AUROS_APP_DIRS
 * replaces them, exactly as it does there, so a harness can watch a
 * folder it controls. */
static const char *SYS_DIRS[] = {
    "/usr/share/applications",
    "/usr/local/share/applications",
    "/var/lib/flatpak/exports/share/applications",
    NULL
};
static const char *USER_DIRS[] = {
    ".local/share/flatpak/exports/share/applications",
    ".local/share/applications",
    NULL
};

#define MAX_TARGETS 12
#define MAX_WATCH   32

static struct {
    int  fd;
    char target[MAX_TARGETS][PATH_MAX];
    int  n_target;
    /* What each watch descriptor is: a directory programs are in, or a
     * parent being watched until that directory appears. */
    int  wd[MAX_WATCH];
    int  is_target[MAX_WATCH];
    int  n_wd;
    int64_t first_ms, last_ms;     /* 0 = nothing pending */
} A = { -1, {{0}}, 0, {0}, {0}, 0, 0, 0 };

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void remember(int wd, int is_target)
{
    for (int i = 0; i < A.n_wd; i++)
        if (A.wd[i] == wd) { A.is_target[i] |= is_target; return; }
    if (A.n_wd < MAX_WATCH) { A.wd[A.n_wd] = wd; A.is_target[A.n_wd] = is_target; A.n_wd++; }
}

static void forget(int wd)
{
    for (int i = 0; i < A.n_wd; i++)
        if (A.wd[i] == wd) {
            A.wd[i] = A.wd[A.n_wd - 1];
            A.is_target[i] = A.is_target[A.n_wd - 1];
            A.n_wd--;
            return;
        }
}

static int wd_is_target(int wd)
{
    for (int i = 0; i < A.n_wd; i++) if (A.wd[i] == wd) return A.is_target[i];
    return 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Watch every target that exists; for every one that does not, watch
 * its nearest existing parent for the moment something is created in
 * it. Returns how many targets are now watched directly. Calling it
 * again is harmless: inotify returns the same descriptor for a path it
 * already watches, with the mask given. */
static int arm(void)
{
    int direct = 0;
    for (int t = 0; t < A.n_target; t++) {
        const char *dir = A.target[t];
        if (is_dir(dir)) {
            int wd = inotify_add_watch(A.fd, dir,
                         IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
                         IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF |
                         IN_MOVE_SELF | IN_ONLYDIR);
            if (wd >= 0) { remember(wd, 1); direct++; }
            continue;
        }
        char up[PATH_MAX];
        snprintf(up, sizeof up, "%s", dir);
        for (;;) {
            char *slash = strrchr(up, '/');
            if (!slash || slash == up) { snprintf(up, sizeof up, "/"); }
            else *slash = 0;
            if (is_dir(up)) {
                int wd = inotify_add_watch(A.fd, up,
                             IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
                if (wd >= 0) remember(wd, 0);
                break;
            }
            if (!strcmp(up, "/")) break;
        }
    }
    return direct;
}

int appwatch_open(void)
{
    if (A.fd >= 0) return A.fd;
    A.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (A.fd < 0) return -1;

    A.n_target = 0;
    const char *ov = getenv("AUROS_APP_DIRS");
    if (ov && *ov) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", ov);
        for (char *t = strtok(tmp, ":"); t && A.n_target < MAX_TARGETS; t = strtok(NULL, ":"))
            if (*t) snprintf(A.target[A.n_target++], PATH_MAX, "%s", t);
    } else {
        for (int i = 0; SYS_DIRS[i] && A.n_target < MAX_TARGETS; i++)
            snprintf(A.target[A.n_target++], PATH_MAX, "%s", SYS_DIRS[i]);
        const char *home = getenv("HOME");
        if (home && *home && strlen(home) < 200)
            for (int i = 0; USER_DIRS[i] && A.n_target < MAX_TARGETS; i++)
                snprintf(A.target[A.n_target++], PATH_MAX, "%s/%s", home, USER_DIRS[i]);
    }
    arm();
    return A.fd;
}

int appwatch_fd(void) { return A.fd; }

void appwatch_close(void)
{
    if (A.fd >= 0) close(A.fd);
    A.fd = -1;
    A.n_wd = 0;
    A.first_ms = A.last_ms = 0;
}

static void pending(void)
{
    int64_t t = now_ms();
    if (!A.first_ms) A.first_ms = t;
    A.last_ms = t;
}

void appwatch_pump(void)
{
    if (A.fd < 0) return;
    /* Aligned as inotify(7) asks. */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t got = read(A.fd, buf, sizeof buf);
        if (got <= 0) break;
        int rearm = 0;
        for (char *p = buf; p < buf + got; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) { pending(); rearm = 1; continue; }
            if (ev->mask & IN_IGNORED) {
                /* The directory went away (or was renamed over). Its
                 * descriptor is dead; arm() will find what replaced it,
                 * or the parent to wait in. */
                if (wd_is_target(ev->wd)) pending();
                forget(ev->wd);
                rearm = 1;
                continue;
            }
            if (wd_is_target(ev->wd)) {
                /* Anything at all in a directory programs live in:
                 * dpkg renames into place, flatpak swaps symlinks, and
                 * a rescan is cheap. */
                pending();
                if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) rearm = 1;
            } else {
                /* A parent: something was made in it, which may be the
                 * directory we are waiting for or the next step on the
                 * way to it. */
                rearm = 1;
            }
        }
        if (rearm) {
            int before = 0;
            for (int i = 0; i < A.n_wd; i++) before += A.is_target[i];
            arm();
            int after = 0;
            for (int i = 0; i < A.n_wd; i++) after += A.is_target[i];
            /* A directory that has just come into being may already
             * hold the program that made it: Flathub creates exports/
             * and puts the first program's entry in it at once. */
            if (after != before) pending();
        }
    }
}

int appwatch_due(void)
{
    if (!A.last_ms) return 0;
    int64_t t = now_ms();
    if (t - A.last_ms >= APPWATCH_SETTLE_MS || t - A.first_ms >= APPWATCH_MAX_MS) {
        A.first_ms = A.last_ms = 0;
        return 1;
    }
    return 0;
}

int appwatch_wait_ms(void)
{
    if (!A.last_ms) return -1;
    int64_t t = now_ms();
    int64_t a = APPWATCH_SETTLE_MS - (t - A.last_ms);
    int64_t b = APPWATCH_MAX_MS - (t - A.first_ms);
    int64_t w = a < b ? a : b;
    if (w < 0) w = 0;
    return (int)w + 1;
}
