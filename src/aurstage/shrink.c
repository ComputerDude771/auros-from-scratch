/* shrink.c — see shrink.h. Stage B: asks and reads, never writes. */
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

#include "shrink.h"
#include "aurstage.h"

/* ── running ntfsresize and reading what it said ─────────────────── */

/* We do not write our own NTFS resizer, and we do not write our own
 * parser for its numbers either: both of its size lines are printed in
 * bytes, in a fixed form, and that is what is matched. A tool whose
 * output we cannot parse is a tool whose answer we do not have, and
 * that is a refusal rather than a guess. */
static int capture(const char *const argv[], char *out, size_t n, int timeout_s)
{
    if (out && n) out[0] = 0;
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1); dup2(p[1], 2);
        if (p[1] > 2) close(p[1]);
        setenv("LC_ALL", "C", 1);          /* its numbers, not a locale's */
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(p[1]);
    size_t got = 0;
    int deadline = timeout_s * 1000;
    while (got + 1 < n) {
        struct pollfd pf = { p[0], POLLIN, 0 };
        int r = poll(&pf, 1, deadline > 0 ? 1000 : 0);
        if (r == 0) { if ((deadline -= 1000) <= 0) { kill(pid, SIGKILL); break; } continue; }
        if (r < 0) { if (errno == EINTR) continue; break; }
        ssize_t k = read(p[0], out + got, n - 1 - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    out[got] = 0;
    close(p[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static uint64_t after(const char *hay, const char *needle)
{
    const char *p = strstr(hay, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    return strtoull(p, NULL, 10);
}

void shrink_ask(const char *dev, shrink_plan *out)
{
    memset(out, 0, sizeof *out);

    /* THE ARGUMENT VECTOR IS FIXED HERE AND TAKES NOTHING FROM A
     * CALLER. That is what makes --force unreachable by construction
     * rather than merely unpassed, which docs/AURBRIDGE.md requires:
     * --force authorises resizing a filesystem whose own metadata
     * Windows has declared untrustworthy, which is R9 verbatim, and it
     * is one word away at all times.
     *
     * --info implies --no-action. Both are given anyway, because the
     * whole safety of stage B rests on this process not writing, and a
     * reader checking that should not have to know which flag implies
     * which. */
    const char *argv[] = { "/sbin/ntfsresize", "--info", "--no-action",
                           dev, NULL };

    char buf[8192];
    int rc = capture(argv, buf, sizeof buf, 300);
    if (rc < 0) {
        snprintf(out->why, sizeof out->why,
                 "The tool that measures the Windows drive could not be run.");
        return;
    }

    /* ntfsresize prints, in this shape:
     *     Current volume size: 4293910528 bytes (4294 MB)
     *     You might resize at 1234567890 bytes or 1235 MB
     * and on a volume it will not touch, a reason instead. */
    out->current_bytes  = after(buf, "Current volume size:");
    out->smallest_bytes = after(buf, "You might resize at");

    if (rc != 0 || !out->current_bytes) {
        out->refused = 1;
        /* Its own words, trimmed to one line, because they name things
         * a support engineer can act on and inventing a paraphrase
         * loses that. */
        const char *msg = strstr(buf, "ERROR");
        if (!msg) msg = strstr(buf, "Error");
        if (!msg) msg = buf;
        char one[240];
        size_t i = 0;
        while (msg[i] && msg[i] != '\n' && i + 1 < sizeof one) { one[i] = msg[i]; i++; }
        one[i] = 0;
        snprintf(out->why, sizeof out->why, "%s", one[0] ? one :
                 "The Windows drive cannot be resized.");
        return;
    }
    if (!out->smallest_bytes) {
        /* It measured the volume and would not say how small it can
         * go. That is not a number we may invent. */
        out->refused = 1;
        snprintf(out->why, sizeof out->why,
                 "The Windows drive could be read but not measured.");
        return;
    }
    out->ok = 1;
}

/* ── the surface test ────────────────────────────────────────────── */

int surface_test(const char *dev, uint64_t from, uint64_t to,
                 uint64_t *first_bad,
                 void (*progress)(uint64_t done, uint64_t total))
{
    if (first_bad) *first_bad = 0;
    if (to <= from) return 0;

    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (first_bad) *first_bad = from; return -1; }

    /* A megabyte at a time: big enough that the read rate is the
     * disk's rather than the syscall's, small enough that one bad
     * sector is localised to somewhere a person can be told about. */
    enum { CHUNK = 1024 * 1024 };
    static unsigned char buf[CHUNK];
    uint64_t total = to - from, done = 0;

    for (uint64_t off = from; off < to; ) {
        size_t want = (size_t)((to - off) < CHUNK ? (to - off) : CHUNK);
        ssize_t k = pread(fd, buf, want, (off_t)off);
        if (k <= 0) {
            /* Narrow it to the sector, so the report names a place and
             * not a megabyte. A disk that fails a whole chunk still
             * fails one sector first, and which one matters to whoever
             * looks at the drive afterwards. */
            for (uint64_t s = off; s < off + want; s += 512) {
                unsigned char one[512];
                if (pread(fd, one, sizeof one, (off_t)s) == (ssize_t)sizeof one)
                    continue;
                if (first_bad) *first_bad = s;
                close(fd);
                return -1;
            }
            if (first_bad) *first_bad = off;
            close(fd);
            return -1;
        }
        off  += (uint64_t)k;
        done += (uint64_t)k;
        if (progress) progress(done, total);
    }
    close(fd);
    return 0;
}
