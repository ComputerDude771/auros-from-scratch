/* main.c — PID 1 in the staging environment. See aurstage.h.
 *
 * STAGE A. This program boots, looks at the machine, writes nothing to
 * any disk, and hands over to the installed system in the same boot.
 * That is the whole of it, on purpose: docs/AURBRIDGE.md sets the
 * build order and the reason for it is that every later stage is
 * judged by whether an abort still leaves a machine that boots
 * Windows -- which cannot be judged until the thing doing the aborting
 * is known to work.
 *
 * PID 1 MUST NEVER RETURN. Returning from main() here panics the
 * kernel, which on a user's machine is a screen of hexadecimal where
 * an explanation should be. Every path below ends in either a
 * successful handover or a sentence and a controlled stop.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <time.h>

#include "aurstage.h"
#include "ntfs.h"
#include "shrink.h"
#include "journal.h"

/* What the kernel was told to do with us, out of /proc/cmdline. */
static int cmdline_has(const char *word)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return 0;
    buf[k] = 0;
    return strstr(buf, word) != NULL;
}

static int cmdline_value(const char *key, char *out, size_t n)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return -1;
    buf[k] = 0;
    char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\n' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return i ? 0 : -1;
}

/* ── the dry run: everything stage B can ask, and no writes ──────── */

static void progress_dots(uint64_t done, uint64_t total)
{
    /* One line that grows, not a thousand lines. This runs for
     * minutes on a ten-year-old disk and somebody is watching it. */
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct == last || pct % 10) return;
    last = pct;
    fprintf(stderr, " %d%%", pct);
    if (pct == 100) fprintf(stderr, "\n");
}

/* Which partition is Windows? Not "the biggest", not "the second one".
 * The one with an NTFS filesystem on it -- and if there is more than
 * one, we say so and stop, because choosing between them is exactly
 * the decision that must come from the journal rather than from a
 * guess made on the machine. */
static const stage_part *find_windows(const stage_machine *m,
                                      const stage_disk **on, int *how_many)
{
    const stage_part *found = NULL;
    *how_many = 0;
    for (int i = 0; i < m->n_disks; i++)
        for (int k = 0; k < m->disk[i].n_parts; k++)
            if (!strcmp(m->disk[i].part[k].fstype, "ntfs")) {
                (*how_many)++;
                if (!found) { found = &m->disk[i].part[k]; *on = &m->disk[i]; }
            }
    return found;
}

static void dry_run(const stage_machine *m)
{
    stage_say("%s", "");   /* a blank line; the format attribute objects to "" */
    stage_say("── what this computer would allow ──────────────────────");

    /* 1. THE JOURNAL. What asked for this, and is this still that
     *    machine? An armed one-shot boot consumed days later, after
     *    Windows has updated and defragmented, is a trap; this is what
     *    catches it. */
    journal j;
    char why[256];
    int have = journal_read("/aurbridge/journal.json", &j) ||
               journal_read("/run/aurbridge/journal.json", &j);
    journal_verdict jv = have ? journal_check(&j, m, why, sizeof why)
                              : (snprintf(why, sizeof why,
                                    "No installer record was found. This is a "
                                    "look-only run."), JOURNAL_NONE);
    stage_say("record   %s", why);
    if (have && jv != JOURNAL_MATCH) {
        stage_warn("this machine does not match what was prepared; "
                   "a real run would stop here");
    }

    /* 2. WHICH PARTITION IS WINDOWS. */
    const stage_disk *on = NULL;
    int n_ntfs = 0;
    const stage_part *win = find_windows(m, &on, &n_ntfs);
    if (!win) {
        stage_warn("no Windows filesystem found on this computer");
        return;
    }
    if (n_ntfs > 1)
        stage_warn("%d Windows filesystems found; a real run needs the "
                   "installer record to say which", n_ntfs);
    char dev[80];
    snprintf(dev, sizeof dev, "/dev/%s", win->name);
    stage_say("windows  %s on %s", dev, on->name);

    /* 3. THE SECTOR SIZE, said out loud even when it is the ordinary
     *    512. Shrink takes sectors, partition tables take bytes, and
     *    assuming 512 on a 4Kn disk makes the partition eight times
     *    too small -- a failure that is invisible right up until it
     *    is total. */
    stage_say("sectors  %d logical, %d physical%s",
              on->logical_sector, on->physical_sector,
              on->logical_sector == 4096 ? "   (4Kn -- sizes are in 4K units)" : "");

    /* 4. THE STATE OF THE FILESYSTEM, read off the disk rather than
     *    taken from what Windows claimed before the restart. */
    ntfs_state ns;
    ntfs_read_state(dev, &ns);
    if (ns.verdict != NTFS_OK) {
        stage_warn("%s", ns.why);
        stage_say("         %s", ns.remedy);
        stage_say("a real run would stop here, having changed nothing");
        return;
    }
    stage_say("state    %s", ns.why);
    stage_say("         serial %016llX, %u-byte clusters",
              (unsigned long long)ns.serial, ns.bytes_per_cluster);
    /* Said out loud rather than left implied. This is the line that
     * tells a support engineer reading a thousand dry-run logs which
     * machines we read for ourselves and which ones we only know
     * about because ntfsresize did not object -- and stage C is not
     * allowed to run on the second kind. */
    stage_say("         asleep %s, unfinished work %s",
              ns.hibernated == NTFS_NO ? "no"
                : ns.hibernated == NTFS_YES ? "YES" : "could not tell",
              ns.log_dirty == NTFS_NO ? "no"
                : ns.log_dirty == NTFS_YES ? "YES" : "could not tell");

    /* 5. HOW SMALL IT CAN ACTUALLY GET. The real number from the tool
     *    that will do the work, not an estimate from free space --
     *    those differ by a great deal on a volume whose data has
     *    drifted to the far end, which after five years of Windows is
     *    every volume. */
    stage_say("measuring how small the Windows drive can get; "
              "this takes a few minutes");
    shrink_plan sp;
    shrink_ask(dev, &sp);
    if (!sp.ok) {
        stage_warn("%s", sp.why);
        stage_say("a real run would stop here, having changed nothing");
        return;
    }
    double cur_g = (double)sp.current_bytes / 1073741824.0;
    double min_g = (double)sp.smallest_bytes / 1073741824.0;
    stage_say("size     %.1f GiB now, %.1f GiB at its smallest", cur_g, min_g);
    if (sp.smallest_bytes >= sp.current_bytes) {
        stage_warn("there is no room on this computer to install alongside "
                   "Windows");
        return;
    }
    stage_say("free     %.1f GiB could be reclaimed",
              (double)(sp.current_bytes - sp.smallest_bytes) / 1073741824.0);

    /* 6. AND WHETHER THE DISK CAN BE TRUSTED WITH IT. Every sector of
     *    the region that would be reclaimed, read. R5, and cheap
     *    against the alternative: a bad sector found halfway through a
     *    shrink is the one failure in this product with no way back. */
    /* Offsets are relative to the PARTITION device, not the disk. Both
     * would work -- /sys gives start_lba in 512-byte units whatever the
     * drive's logical sector size, so an absolute offset is one
     * multiply away -- but the partition device cannot read past its
     * own end, and a region computed one partition too far along is a
     * mistake that reports a neighbour's bad sector as Windows'. The
     * kernel's bounds are better than our arithmetic.
     *
     * What is NOT covered yet: R5 also asks for the region NTFS will
     * relocate INTO. That is free space scattered below the new size
     * and we cannot name its extents without reading $Bitmap, which
     * stage B does not do. Written down rather than quietly skipped. */
    uint64_t from = sp.smallest_bytes;
    uint64_t to   = sp.current_bytes;
    stage_say("reading every sector of the space that would be reclaimed");
    uint64_t bad = 0;
    fprintf(stderr, "aurstage: ");
    if (surface_test(dev, from, to, &bad, progress_dots) != 0) {
        stage_warn("this disk could not read the space %llu MB into the "
                   "Windows drive.",
                   (unsigned long long)(bad / (1024 * 1024)));
        stage_say("         The drive is failing. A real run would stop "
                  "here, having changed nothing.");
        return;
    }
    stage_say("surface  every sector of the reclaimed space reads back");

    stage_say("%s", "");   /* a blank line; the format attribute objects to "" */
    stage_say("This computer could be converted.");
    stage_say("Nothing has been changed.");
}

/* A staging environment that fails has to stop in a way a person can
 * describe over the telephone. Not a panic, not a reboot loop. */
static void stop_here(const char *why)
{
    stage_warn("%s", why);
    stage_say("Nothing on this computer has been changed.");
    stage_say("Turn it off with the power button and start it again;");
    stage_say("it will come back to Windows.");
    for (;;) pause();
}

int main(void)
{
    /* stdout unbuffered: everything this says may be the last thing
     * anybody sees, and a line sitting in a buffer when the kernel
     * stops is a line that was never said. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    stage_say("AurOS staging environment");
    stage_say("this is the one restart; nothing has been changed yet");

    if (!stage_mount_pseudo())
        stage_warn("some of the kernel's own filesystems are missing; "
                   "going on, because what follows only reads");

    int n = stage_load_modules();
    stage_say("%d drivers loaded", n);

    if (!stage_wait_for_disks(20000))
        stop_here("this computer's disk did not appear. "
                  "It may use a storage mode AurOS cannot see yet.");

    stage_machine m;
    stage_survey(&m);
    stage_report(&m);

    /* THE DRY RUN.
     *
     * Everything stage B can establish, run in order, and then stop --
     * changing nothing, and handing over to nothing.
     *
     * docs/AURBRIDGE.md calls this the first shippable artifact and
     * says it is "worth shipping on its own to build a hardware matrix
     * before anyone's disk is at risk". That is the whole point of it:
     * the questions below are the ones that decide whether this
     * machine can be converted, and every one of them can be answered
     * without writing a byte. Ship it, run it on a thousand laptops,
     * and find out what the fleet looks like while the worst possible
     * outcome is a wasted restart. */
    if (cmdline_has("aurstage.dry")) {
        dry_run(&m);
        stage_say("nothing on this computer has been changed");
        sync();
        reboot(RB_POWER_OFF);
        for (;;) pause();
    }

    char root[128];
    if (cmdline_value("aurstage.root=", root, sizeof root) != 0) {
        /* Nobody said which. Stage A does not guess: choosing a root
         * by looking for "the ext4 one that has an init in it" is
         * exactly the sort of helpfulness that picks the wrong disk on
         * a machine with two. Stage B gets this from the journal
         * AurBridge wrote, which names it. */
        stop_here("nobody said which system to start "
                  "(no aurstage.root= on the kernel command line)");
    }

    stage_say("asked to hand over to %s", root);
    if (stage_switch_root(root) != 0)
        stop_here("that system could not be started");

    /* switch_root does not return. If we are here, exec failed after
     * the old root was already gone, and there is nothing left to try. */
    stop_here("the handover did not complete");
    return 0;
}
