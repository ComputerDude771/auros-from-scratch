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
#include <time.h>

#include "shrink.h"
#include "ntfs.h"
#include "aurstage.h"

/* WHERE ntfsresize IS, and why this is a #define rather than an
 * argument. The path has to be fixed for the same reason the argument
 * vector is fixed -- a caller that can choose the program can choose a
 * different program -- but the tests need to run against the copy in
 * the profile's rootfs rather than one installed on the build host.
 * A compile-time constant is still not something a caller can reach. */
#ifndef NTFSRESIZE_PATH
#define NTFSRESIZE_PATH "/sbin/ntfsresize"
#endif

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

    /* TWO WAYS THIS USED TO HANG, both of them PID 1 sitting silent on
     * somebody's laptop for ever, which is the worst failure this
     * program has.
     *
     * The first: it stopped reading once `out` was full and then
     * waited for the child. A child with more to say blocks writing
     * into a full pipe, and neither side ever moves again. So the
     * pipe is now drained to the end whatever happens -- past the
     * buffer the extra is read and thrown away, which is the only
     * thing that lets the child finish.
     *
     * The second: the timeout was per-poll, so every byte reset it.
     * `ntfsresize --info` prints a running percentage, which means it
     * dribbles, which means the bound was not 300 seconds but 300
     * seconds per chunk -- unbounded in practice. It is a deadline
     * measured from the start now. (run.c carries the same note about
     * the same mistake; this is the second time I have made it.) */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long budget = (long)timeout_s * 1000;
    size_t got = 0;
    int killed = 0;
    char tail[4096];
    uint64_t nseen = 0;          /* bytes ever put in the ring */
    for (;;) {
        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        long spent = (tn.tv_sec - t0.tv_sec) * 1000L
                   + (tn.tv_nsec - t0.tv_nsec) / 1000000L;
        long left = budget - spent;
        if (!killed && left <= 0) { kill(pid, SIGKILL); killed = 1; }

        struct pollfd pf = { p[0], POLLIN, 0 };
        int r = poll(&pf, 1, killed ? 200 : (int)(left > 1000 ? 1000 : left));
        if (r == 0) { if (killed) break; continue; }
        if (r < 0) { if (errno == EINTR) continue; break; }

        char bin[4096];
        char  *dst = bin;
        size_t room = sizeof bin;
        if (out && got + 1 < n) { dst = out + got; room = n - 1 - got; }
        ssize_t k = read(p[0], dst, room);
        if (k <= 0) break;                 /* it closed: it is finished */
        if (dst != bin) { got += (size_t)k; continue; }

        /* THE ANSWER IS AT THE END, AND THE BUFFER FILLS FROM THE
         * FRONT. Once `out` was full this threw the rest away, and
         * "You might resize at" -- the number the whole call exists to
         * get -- is printed AFTER ntfsresize's progress bars, each of
         * which emits a hundred carriage-returned percentage lines. On
         * a large fragmented volume, which is the population this
         * product is for, the head filled with progress and the answer
         * fell off the end, and the machine was refused with "could be
         * read but not measured" for a buffer size.
         *
         * So the overflow is kept too: the last TAIL bytes are held in
         * a ring and stitched on at the end. Head and tail together
         * hold the volume line, which comes first, and the resize
         * line, which comes last, whatever runs between them. */
        for (ssize_t i = 0; i < k; i++) {
            tail[nseen % sizeof tail] = bin[i];
            nseen++;
        }
    }
    if (out && n) {
        out[got] = 0;
        if (nseen) {
            /* Unroll the ring, then append as much as still fits --
             * from the END of the tail, which is where the answer is. */
            char flat[sizeof tail + 1];
            size_t have = nseen < sizeof tail ? (size_t)nseen : sizeof tail;
            for (size_t i = 0; i < have; i++)
                flat[i] = tail[(size_t)(nseen - have + i) % sizeof tail];
            flat[have] = 0;
            size_t room2 = n - 1 - got;
            size_t take = have < room2 ? have : room2;
            memcpy(out + got, flat + (have - take), take);
            out[got + take] = 0;
        }
    }
    close(p[0]);

    /* Unconditional, so waitpid cannot wait on something still alive.
     * A process that has already exited is a zombie and has not been
     * reaped yet, so its pid is still ours and the signal is simply
     * discarded. */
    kill(pid, SIGKILL);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    if (killed) return -1;                 /* out of time: no answer */
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
    const char *argv[] = { NTFSRESIZE_PATH, "--info", "--no-action",
                           dev, NULL };

    /* HALF AN HOUR, not five minutes. `--info` reads $Bitmap and walks
     * the MFT, and on the machines this product is for -- a ten-year-old
     * 5400 rpm terabyte drive with a large, fragmented MFT -- that is
     * minutes, not seconds. A deadline shorter than the honest worst
     * case turns a slow machine into a refused one. It is a real
     * deadline now (see capture), so it bounds the whole call. */
    char buf[8192];
    int rc = capture(argv, buf, sizeof buf, 1800);
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

/* ── the one irreversible step ───────────────────────────────────── */

/* Run ntfsresize for real, feeding it the confirmation it asks for and
 * reading its progress back out.
 *
 * Unlike capture() above this streams: the caller gets a percentage
 * while it happens, because on a 5400 rpm disk with five years of
 * Windows on it this runs for forty minutes and the person is
 * watching a screen that says the one thing that cannot be undone is
 * happening. A progress bar is not decoration there. */
void shrink_do(const char *dev, uint64_t target_bytes,
               void (*progress)(int percent), shrink_result *out)
{
    memset(out, 0, sizeof *out);

    char size[32];
    snprintf(size, sizeof size, "%llu", (unsigned long long)target_bytes);

    /* FIXED, AS ABOVE, AND WITHOUT --force. See shrink.h. */
    const char *argv[] = { NTFSRESIZE_PATH, "--size", size, dev, NULL };

    int in[2], outp[2];
    if (pipe(in) < 0) {
        snprintf(out->why, sizeof out->why,
                 "the tool that resizes the Windows drive could not be started");
        return;
    }
    if (pipe(outp) < 0) {
        close(in[0]); close(in[1]);
        snprintf(out->why, sizeof out->why,
                 "the tool that resizes the Windows drive could not be started");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(outp[0]); close(outp[1]);
        snprintf(out->why, sizeof out->why,
                 "this computer would not start the resizing tool");
        return;
    }
    if (pid == 0) {
        close(in[1]); close(outp[0]);
        dup2(in[0], 0);
        dup2(outp[1], 1); dup2(outp[1], 2);
        if (in[0] > 2) close(in[0]);
        if (outp[1] > 2) close(outp[1]);
        setenv("LC_ALL", "C", 1);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(in[0]); close(outp[1]);

    /* It asks once, near the beginning. Answering immediately is safe:
     * the question is always the same one and the answer is always the
     * same, and we have already decided -- twice, in stage B and again
     * at the gate -- that this volume may be resized. */
    ssize_t ign = write(in[1], "y\n", 2); (void)ign;
    close(in[1]);

    char line[512];
    size_t ln = 0;
    int last_pct = -1;
    char tail[1024]; size_t tn = 0;
    for (;;) {
        char c;
        ssize_t k = read(outp[0], &c, 1);
        if (k <= 0) break;
        /* Keep the last kilobyte for the message on failure, and
         * assemble lines for the percentage. */
        tail[tn % sizeof tail] = c; tn++;
        if (c == '\r' || c == '\n') {
            line[ln] = 0;
            /* "  12.34 percent completed" */
            const char *pc = strstr(line, "percent completed");
            if (pc) {
                out->started = 1;
                int pct = (int)strtod(line, NULL);
                if (pct != last_pct && progress) { progress(pct); last_pct = pct; }
            } else if (strstr(line, "Relocating") || strstr(line, "Shrinking") ||
                       strstr(line, "Updating")) {
                out->started = 1;
            }
            ln = 0;
        } else if (ln + 1 < sizeof line) {
            line[ln++] = c;
        }
    }
    close(outp[0]);

    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        snprintf(out->why, sizeof out->why,
                 "the resizing tool disappeared");
        return;
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

    if (code != 0) {
        /* THE DISTINCTION THAT MATTERS. A child that never started
         * moving data did not touch the volume, and telling that user
         * their drive is damaged is a lie -- the likeliest cause is
         * our own image missing a library, which has happened here
         * before and was found only inside QEMU. */
        if (!out->started)
            snprintf(out->why, sizeof out->why,
                     "AurOS could not run the tool that resizes the Windows "
                     "drive. Nothing on this computer has been changed.");
        else
            snprintf(out->why, sizeof out->why,
                     "The Windows drive could not be resized.");
        return;
    }

    /* WHAT IT ACTUALLY CAME OUT AT, read from the volume's own boot
     * sector rather than assumed to be what we asked for. ntfsresize
     * rounds to its own cluster boundary, and the partition entry
     * must never end below the filesystem inside it. */
    if (ntfs_volume_bytes(dev, &out->achieved_bytes) != 0 ||
        out->achieved_bytes == 0) {
        snprintf(out->why, sizeof out->why,
                 "The Windows drive was resized but will not say how big it "
                 "now is.");
        return;
    }
    out->ok = 1;
    snprintf(out->why, sizeof out->why, "the Windows drive is now %.1f GiB",
             (double)out->achieved_bytes / (1024.0*1024.0*1024.0));
}

/* ── the surface test ────────────────────────────────────────────── */

int surface_test(const char *dev, uint64_t from, uint64_t to,
                 uint32_t sector,
                 uint64_t *first_bad,
                 void (*progress)(uint64_t done, uint64_t total))
{
    if (first_bad) *first_bad = 0;
    if (to <= from) return 0;

    /* A DEADLINE, because this is the one long step that had none.
     * A drive that is failing does not usually return an error -- it
     * retries, resets its link and returns the sector thirty seconds
     * later. Reading a 400 GB region on such a drive can take days,
     * with a progress dot every ten per cent, and there is nothing a
     * person can do but hold the power button. Four hours is far
     * beyond any healthy disk (a 5400 rpm drive reads 400 GB in about
     * two) and far short of forever. Running out of time is reported
     * as the drive being too slow to trust, which is what it is. */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const long BUDGET_S = 4 * 60 * 60;

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
        if (k == 0) {
            /* END OF THE DEVICE, not a bad sector. The region came
             * from ntfsresize's idea of the volume size and the device
             * is the partition, so these should agree -- and when they
             * do not, "the drive is failing" is the wrong thing to say
             * about a drive that is fine. */
            if (first_bad) *first_bad = off;
            close(fd);
            return -1;
        }
        if (k < 0) {
            /* Narrow it to the sector, so the report names a place and
             * not a megabyte. A disk that fails a whole chunk still
             * fails one sector first, and which one matters to whoever
             * looks at the drive afterwards. */
            /* THE DRIVE'S OWN SECTOR, not 512. On a 4Kn disk a
             * 512-byte read is not a thing the device can do, so the
             * narrowing loop reported the start of the chunk and the
             * message named a place a megabyte away from the fault. */
            uint32_t ssz = sector >= 512 && sector <= 4096 &&
                           !(sector & (sector - 1)) ? sector : 512;
            for (uint64_t s = off; s < off + want; s += ssz) {
                unsigned char one[4096];
                if (pread(fd, one, ssz, (off_t)s) == (ssize_t)ssz)
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

        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        if (tn.tv_sec - t0.tv_sec > BUDGET_S) {
            if (first_bad) *first_bad = off;
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}
