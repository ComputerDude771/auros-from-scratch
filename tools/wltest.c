/* wltest.c — does a real application's window actually arrive?
 *
 * The shell could always paint something that looked like a window. The
 * only question that matters about aurwl is whether the pixels in that
 * window came from another process. So this harness does the smallest
 * thing that can answer it: start the compositor, launch a real client,
 * pump the event loop, and write whatever each mapped window contains
 * to a PNG. If the PNG is a browser, the compositor works. If it is
 * blank, no amount of protocol conformance matters.
 *
 *   wltest [-s SECONDS] [-o PREFIX] [-W w] [-H h] -- <command>
 *
 * Exit status is 0 only if at least one window mapped AND arrived with
 * pixels that are not all one colour -- a window full of one colour is
 * what a client draws when it has given up, and it must not pass.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>

#include "../src/aurwl/aurwl.h"
#include "../src/common/png.h"

static uint32_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* A window that arrived as one flat colour has not really arrived. */
static int distinct_colours(const surface *s, int cap)
{
    uint32_t seen[64]; int n = 0;
    for (int y = 0; y < s->h && n < cap; y += 1 + s->h / 64)
        for (int x = 0; x < s->w && n < cap; x += 1 + s->w / 64) {
            uint32_t p = s->px[(size_t)y * s->stride + x];
            int dup = 0;
            for (int i = 0; i < n; i++) if (seen[i] == p) { dup = 1; break; }
            if (!dup && n < 64) seen[n++] = p;
        }
    return n;
}

int main(int argc, char **argv)
{
    int secs = 12, ow = 1280, oh = 720;
    const char *prefix = "wltest";
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) ow = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) oh = atoi(argv[++i]);
        else break;
    }
    if (i >= argc) { fprintf(stderr, "usage: wltest [-s N] [-o PFX] -- <command>\n"); return 2; }

    /* Join the remaining arguments back into one shell command, because
     * that is the shape a .desktop Exec= line has and the shape aurwl
     * spawns. */
    char cmd[4096] = {0};
    for (int j = i; j < argc; j++) {
        strncat(cmd, argv[j], sizeof cmd - strlen(cmd) - 2);
        if (j + 1 < argc) strncat(cmd, " ", sizeof cmd - strlen(cmd) - 2);
    }

    aurwl *c = aurwl_create(ow, oh, 60000);
    if (!c) { fprintf(stderr, "wltest: compositor did not start\n"); return 1; }
    printf("socket   %s\n", aurwl_socket(c));
    printf("command  %s\n", cmd);

/* A development harness runs whatever command the person at the
 * keyboard typed, so /bin/sh is the right thing here and is chosen
 * explicitly. The product path has no shell: an Exec= line from a file
 * on disk is parsed into an argv and handed to execvp. */
    const char *argv_sh[] = { "/bin/sh", "-c", cmd, NULL };
    if (aurwl_spawn(c, argv_sh) < 0) { fprintf(stderr, "wltest: spawn failed\n"); return 1; }

    uint32_t t0 = now_ms(), last_frame = 0;
    int peak = 0;
    while (now_ms() - t0 < (uint32_t)secs * 1000u) {
        struct pollfd p = { .fd = aurwl_fd(c), .events = POLLIN };
        poll(&p, 1, 16);
        aurwl_dispatch(c);
        aurwl_reap(c);

        uint32_t t = now_ms();
        /* Clients throttle to frame callbacks. Without firing them, a
         * toolkit draws exactly one frame and then waits forever -- and
         * the harness would report an empty window for a client that is
         * working perfectly. */
        if (t - last_frame >= 16) { aurwl_frame_done(c, t); last_frame = t; }

        int n = aurwl_window_count(c);
        if (n > peak) peak = n;

        /* Give the first window keyboard focus as soon as it exists:
         * some toolkits do not paint their real content until they
         * believe they are focused. */
        if (n > 0 && !aurwl_focus(c)) aurwl_set_focus(c, aurwl_window_at(c, 0));
    }

    int n = aurwl_window_count(c), good = 0;
    printf("windows  %d mapped now, %d peak\n", n, peak);
    for (int k = 0; k < n; k++) {
        aurwl_win *w = aurwl_window_at(c, k);
        surface *s = aurwl_win_content(w);
        int pw, ph; aurwl_win_pref_size(w, &pw, &ph);
        printf("  [%u] %-28s %-26s %s %dx%d (geom %dx%d)",
               aurwl_win_id(w), aurwl_win_app_id(w), aurwl_win_title(w),
               aurwl_win_is_popup(w) ? "popup" : "toplevel",
               s ? s->w : 0, s ? s->h : 0, pw, ph);
        if (!s) { printf("  NO CONTENT\n"); continue; }
        int nc = distinct_colours(s, 64);
        char path[512];
        snprintf(path, sizeof path, "%s-%u.png", prefix, aurwl_win_id(w));
        png_write_rgb(path, s->px, s->w, s->h);
        printf("  %2d colours -> %s\n", nc, path);
        if (nc >= 3) good++;
    }
    aurwl_destroy(c);

    if (!peak)  { printf("\nFAIL: no window ever mapped\n"); return 1; }
    if (!good)  { printf("\nFAIL: %d window(s) mapped but none had real content\n", peak); return 1; }
    printf("\nPASS: %d window(s) arrived with content from another process\n", good);
    return 0;
}
