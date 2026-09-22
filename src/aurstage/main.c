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
#include "fde.h"
#include "health.h"
#include "install.h"
#include "rescue.h"
#include "fault.h"
/* For LOADER_BOOT_TYPICAL_MB: how much of the gap the part that starts
 * the computer takes. The dry run has to count it. */
#include "loader.h"

/* What the kernel was told to do with us, out of /proc/cmdline. */


/* WHAT AurOS ACTUALLY NEEDS, with somewhere to put things afterwards.
 * Decimal gigabytes, because that is the unit a disk is sold in and
 * the unit she will compare it against.
 *
 * `aurstage.min_gb=` on the kernel command line replaces it. That is
 * not a back door: this whole product is meant to be rebuilt for a
 * school or an office, and such a build can be a great deal smaller
 * than the desktop one -- a kiosk image with no office suite does not
 * need twenty-four gigabytes, and refusing a machine that would have
 * been fine is a real cost. Stage B only reports, so the worst a
 * wrong number does here is describe a machine optimistically; the
 * value is printed in the report line so a row can always be read
 * against the floor it was judged by. */
#define AUROS_NEEDS_GB_DEFAULT 24ull

static uint64_t needs_bytes(void)
{
    char v[32];
    if (stage_cmdline_value("aurstage.min_gb=", v, sizeof v) == 0) {
        unsigned long long g = strtoull(v, NULL, 10);
        if (g >= 1 && g <= 4096) return g * 1000ull * 1000 * 1000;
    }
    return AUROS_NEEDS_GB_DEFAULT * 1000ull * 1000 * 1000;
}

/* Did the firmware give us EFI variables? efivarfs is mounted
 * read-only by boot.c; its presence is the only thing on a running
 * machine that says this is a UEFI boot, and a UEFI boot is what
 * BootNext -- the whole one-restart design -- rests on. */
static int efivars_present(void)
{
    int fd = open("/sys/firmware/efi/efivars", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return 0;
    close(fd);
    return 1;
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

/* Is this partition a Windows volume?
 *
 * A BITLOCKER VOLUME COUNTS. It is Windows; it is simply Windows we
 * cannot read. Leaving it out is how the refusal came back as "no
 * Windows filesystem found on this computer", which sends a person
 * with an ordinary encrypted laptop looking for a problem she does
 * not have. Counting it lets ntfs_read_state say the word BitLocker
 * and hand her the five-minute remedy. */
static int looks_like_windows(const stage_part *p)
{ return !strcmp(p->fstype, "ntfs") || !strcmp(p->fstype, "bitlocker"); }

/* Which partition is Windows?
 *
 * THE JOURNAL DECIDES, AND WHEN IT CANNOT, NOBODY DOES.
 *
 * This used to return the first NTFS partition in readdir() order and
 * merely WARN when there was more than one. Two things are wrong with
 * that, and both of them are ordinary machines rather than corner
 * cases:
 *
 *   - Every OEM laptop made in the last decade has a WinRE recovery
 *     partition, which is NTFS. So "more than one" is not unusual; it
 *     is the norm, and the warning fired on almost every real disk
 *     while the run carried on with whichever one the kernel's
 *     directory hash happened to yield first. A 500 MB recovery
 *     partition with 300 MB free would be measured, surface-tested
 *     and reported as "this computer could be converted".
 *   - This design requires a recovery USB stick to be plugged in, and
 *     a stick formatted NTFS is another candidate -- on removable
 *     media, which is now skipped outright.
 *
 * readdir() order is not disk order either: it is hash order, so the
 * answer was not even consistently wrong.
 *
 * So: the journal's win_start_lba picks it when there is a journal,
 * and when there is not, exactly one candidate is required. More than
 * one is a refusal, which is what the old comment already claimed to
 * do and did not. */
static const stage_part *find_windows(const stage_machine *m,
                                      const journal *j, int have,
                                      const stage_disk **on, int *how_many)
{
    const stage_part *found = NULL;
    *how_many = 0;
    for (int i = 0; i < m->n_disks; i++) {
        const stage_disk *d = &m->disk[i];
        /* A USB stick is not this computer's Windows. */
        if (d->removable) continue;
        for (int k = 0; k < d->n_parts; k++) {
            if (!looks_like_windows(&d->part[k])) continue;
            (*how_many)++;
            /* Named by the record: take that one and stop looking. */
            if (have && j->win_start_lba &&
                d->part[k].start_lba == j->win_start_lba &&
                d->serial[0] && !strcmp(d->serial, j->disk_serial)) {
                *on = d;
                return &d->part[k];
            }
            if (!found) { found = &d->part[k]; *on = d; }
        }
    }
    if (*how_many > 1) return NULL;     /* nothing may choose but the record */
    return found;
}

/* ONE LINE A TOOL CAN READ, on every path out of the dry run.
 *
 * docs/AURBRIDGE.md says the dry run is worth shipping on its own "to
 * build a hardware matrix before anyone's disk is at risk". A hardware
 * matrix is a table, and a table cannot be built out of eleven lines
 * of English that change whenever somebody improves the wording. So
 * every run ends with this, in a fixed shape, whatever happened.
 *
 * WHERE IT GOES is an honest limitation of stage B and is written
 * down rather than worked around: nothing here writes, so it goes to
 * the console and nowhere else. Over a serial cable a technician
 * collects it; in front of a person it is something to photograph.
 * Stage C is the first stage that has anywhere to put it. */
static const char *g_record = "none";       /* set by the journal step */

static void verdict_line(const char *verdict, const char *detail)
{
    /* ONE line per run, not one per thing that went wrong: a table
     * with two rows for the same machine is a table nobody can count. */
    stage_say("aurstage-report v1 verdict=%s record=%s %s", verdict,
              g_record, detail && detail[0] ? detail : "-");
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
        /* The dry run goes ON, deliberately. A real run stops here and
         * must; but the whole point of shipping this is to find out
         * what a fleet of machines looks like, and a machine whose
         * record is stale is exactly one whose disk we still want
         * described. The report line carries which it was. */
    }
    g_record = have ? journal_verdict_name(jv) : "none";

    /* 2. SOMEBODY ELSE'S ENCRYPTION, BEFORE ANYTHING ELSE IS READ.
     *
     * docs/AURBRIDGE.md: third-party full-disk encryption is an
     * unconditional abort, because under a sector-level filter driver
     * we do not control there is no safe shrink -- the bytes we read
     * are not the bytes Windows sees, and the bytes we would write
     * would not be either.
     *
     * The front of every fixed disk and the EFI partition on it. Both
     * are small; this costs a second. */
    char who[64];
    int fde_unsure = 0;
    for (int i = 0; i < m->n_disks; i++) {
        const stage_disk *d = &m->disk[i];
        if (d->removable) continue;
        char dd[80], esp[80];
        snprintf(dd, sizeof dd, "/dev/%s", d->name);
        esp[0] = 0;
        for (int k = 0; k < d->n_parts; k++)
            if (d->part[k].is_esp)
                snprintf(esp, sizeof esp, "/dev/%s", d->part[k].name);
        fde_verdict fv = fde_scan_disk(dd, esp[0] ? esp : NULL,
                                       who, sizeof who);
        if (fv == FDE_NAMED) {
            stage_warn("this computer's drive is protected by %s.", who);
            stage_say("         That has to be turned off and the drive "
                      "decrypted first.");
            stage_say("a real run would stop here, having changed nothing");
            verdict_line("third-party-encryption", who);
            return;
        }
        /* "Could not look" is not "nothing there". Stage B goes on --
         * everything after this reads the volume itself and will say
         * what it finds -- but the row records that this check never
         * got to run, because stage C may not proceed on one that
         * did not. */
        if (fv == FDE_UNSURE) fde_unsure = 1;
    }

    /* 3. WHICH PARTITION IS WINDOWS. */
    const stage_disk *on = NULL;
    int n_ntfs = 0;
    const stage_part *win = find_windows(m, &j, have, &on, &n_ntfs);
    if (!win && n_ntfs > 1) {
        stage_warn("there are %d Windows drives on this computer and "
                   "nothing here says which one to use", n_ntfs);
        stage_say("         A real run is told which by the installer. "
                  "Nothing has been changed.");
        char c[32]; snprintf(c, sizeof c, "candidates=%d", n_ntfs);
        verdict_line("ambiguous-windows", c);
        return;
    }
    if (!win) {
        stage_warn("no Windows filesystem found on this computer");
        verdict_line("no-windows", "-");
        return;
    }
    char dev[80];
    snprintf(dev, sizeof dev, "/dev/%s", win->name);
    stage_say("windows  %s on %s", dev, on->name);

    /* 3b. IS THIS A UEFI MACHINE WITH A GPT DISK AT ALL?
     *
     * docs/AURBRIDGE.md makes BIOS/MBR an unconditional refusal: the
     * one-restart design is built on `BootNext`, which is an EFI
     * variable, and on a machine that has none there is no one-shot,
     * self-reverting boot and so no safe way to fail. Nothing checked
     * this, so a 2012 BIOS desktop with an MBR disk passed every other
     * test in stage B and was recorded as convertible. */
    if (!on->gpt) {
        stage_warn("this computer divides its disk up in an older way "
                   "that AurOS cannot install alongside.");
        stage_say("         Nothing has been changed.");
        verdict_line("not-gpt", "-");
        return;
    }
    if (!efivars_present()) {
        stage_warn("this computer starts up in an older way that AurOS "
                   "cannot install alongside safely.");
        stage_say("         Nothing has been changed.");
        verdict_line("not-uefi", "-");
        return;
    }
    if (!on->sector_known) {
        /* docs/AURBRIDGE.md: "Always read StorageAccessAlignmentProperty,
         * and block if it cannot be read." Assuming 512 on a disk that
         * would not say is the 4Kn mistake that makes a partition
         * eight times too small. */
        stage_warn("this computer will not say what size its disk's "
                   "sectors are.");
        stage_say("         Nothing has been changed.");
        verdict_line("sector-size-unknown", "-");
        return;
    }

    /* 3c. THE DRIVE'S OWN OPINION OF ITSELF, before anything reads it
     *     hard. R5: "read SMART first... Refuse on any
     *     pending/uncorrectable sectors, no override."
     *
     *     This is the one check whose output is worth more to her than
     *     the install. She came here to put Linux on an old laptop and
     *     she is leaving knowing her drive is dying, while her
     *     photographs are still readable. A user told "your hard drive
     *     is failing, back up now" is a user we saved. */
    smart_state sm;
    char smdev[80];
    snprintf(smdev, sizeof smdev, "/dev/%s", on->name);
    smart_read(smdev, &sm);
    if (sm.verdict == SMART_FAILING) {
        stage_warn("%s", sm.why);
        stage_say("         %s", sm.remedy);
        stage_say("a real run would stop here, having changed nothing");
        verdict_line("drive-failing", smart_verdict_name(sm.verdict));
        return;
    }
    stage_say("drive    %s", sm.why);
    if (sm.remedy[0]) stage_say("         %s", sm.remedy);

    /* 3d. AND WHETHER IT WOULD SURVIVE BEING OPERATED ON. R6: "Refuse
     *     on battery <50% or not on AC." A dry run does not refuse --
     *     nothing here can be interrupted into a bad state -- but the
     *     answer goes in the row, because a machine that would be
     *     refused for being on battery is not a machine the fleet
     *     numbers should count as ready. */
    power_state pw;
    char pwhy[200];
    power_read(NULL, &pw);
    int pw_ok = power_ok(&pw, pwhy, sizeof pwhy);
    stage_say("power    %s", pwhy);
    if (!pw_ok)
        stage_say("         A real run would wait until it was plugged in.");

    /* 4. THE SECTOR SIZE, said out loud even when it is the ordinary
     *    512. Shrink takes sectors, partition tables take bytes, and
     *    assuming 512 on a 4Kn disk makes the partition eight times
     *    too small -- a failure that is invisible right up until it
     *    is total. */
    stage_say("sectors  %d logical, %d physical%s",
              on->logical_sector, on->physical_sector,
              on->logical_sector == 4096 ? "   (4Kn -- sizes are in 4K units)" : "");

    /* 5. THE STATE OF THE FILESYSTEM, read off the disk rather than
     *    taken from what Windows claimed before the restart. */
    ntfs_state ns;
    ntfs_read_state(dev, &ns);
    if (ns.verdict != NTFS_OK) {
        stage_warn("%s", ns.why);
        stage_say("         %s", ns.remedy);
        stage_say("a real run would stop here, having changed nothing");
        verdict_line("ntfs-refused", ntfs_verdict_name(ns.verdict));
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

    /* 6. HOW SMALL IT CAN ACTUALLY GET. The real number from the tool
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
        verdict_line("cannot-measure", sp.refused ? "refused" : "no-answer");
        return;
    }
    double cur_g = (double)sp.current_bytes / 1073741824.0;
    double min_g = (double)sp.smallest_bytes / 1073741824.0;
    stage_say("size     %.1f GiB now, %.1f GiB at its smallest", cur_g, min_g);
    /* ROOM FOR WHAT, EXACTLY. "More than zero bytes could be
     * reclaimed" is not the question -- the question is whether AurOS
     * fits and leaves the person somewhere to put things. Without a
     * floor, a machine with 400 MB free was recorded as convertible,
     * and stage C would then run the one irreversible step in the
     * product to free space nothing fits in. */
    uint64_t freeable = sp.current_bytes > sp.smallest_bytes
                      ? sp.current_bytes - sp.smallest_bytes : 0;
    /* AND THE ROOT IS NOT THE WHOLE OF WHAT HAS TO FIT.
     *
     * This asked only whether the 24 GB root floor fitted, while the
     * install has to fit that PLUS the partition AurOS starts from
     * (the image's own ESP, hundreds of megabytes) PLUS the saved copy
     * of this machine's Windows startup (its ESP again, 100 MB on one
     * laptop and a gigabyte on the next) -- all carved out of the same
     * gap. So a machine a little over the floor was reported
     * convertible here and refused by the install, which is precisely
     * the two-halves-disagreeing failure install.c's own comment says
     * was eliminated. Nothing is damaged by it -- the install refuses
     * before the shrink -- but a hardware matrix built from the dry
     * run has rows in it that are not true.
     *
     * The allowance is measured where it can be and estimated where it
     * cannot: rescue_size_needed asks THIS disk about its own ESP, and
     * the boot area is the size build/mkimage actually makes one,
     * because the dry run has no stick in it to read an image from.
     *
     * IT USED TO REACH FOR loader.h's CEILING HERE, on the reasoning
     * that generous is safe. Generous in this direction is a machine
     * told there is not enough room when there is: two gigabytes where
     * the install needs half of one, which made the dry run demand
     * three gigabytes and refuse four of the ordinary machines in
     * tools/matrixtest.sh -- ordinary laptops, the people this product
     * is for. A false refusal is not the safe side of this question.
     * The ceiling is disbelief about a number read off a stick; it was
     * never an estimate of anything. */
    uint64_t extra = 0;
    {
        uint64_t rsc = 0;
        char rw[200];
        if (rescue_size_needed(on, &rsc, rw, sizeof rw) == 0) extra += rsc;
        else extra += 1024ull * 1024 * 1024;     /* the worst ESP seen  */
        extra += LOADER_BOOT_TYPICAL_MB * 1024ull * 1024;
    }
    uint64_t need = needs_bytes() + extra;
    if (freeable < need) {
        stage_warn("there is not enough room on this computer to install "
                   "alongside Windows.");
        stage_say("         AurOS needs %llu GB free -- itself, the part it "
                  "starts from and a way back to Windows -- and this "
                  "computer can spare %llu GB.",
                  (unsigned long long)(need / 1000000000ull),
                  (unsigned long long)(freeable / 1000000000ull));
        char c[64];
        snprintf(c, sizeof c, "free_mib=%llu need_gb=%llu",
                 (unsigned long long)(freeable / (1024 * 1024)),
                 (unsigned long long)(need / 1000000000ull));
        verdict_line("no-room", c);
        return;
    }
    stage_say("free     %.1f GiB could be reclaimed",
              (double)(sp.current_bytes - sp.smallest_bytes) / 1073741824.0);

    /* 7. AND WHETHER THE DISK CAN BE TRUSTED WITH IT. Every sector of
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
    if (surface_test(dev, from, to, (uint32_t)on->logical_sector,
                     &bad, progress_dots) != 0) {
        stage_warn("this disk could not read the space %llu MB into the "
                   "Windows drive.",
                   (unsigned long long)(bad / (1024 * 1024)));
        stage_say("         The drive is failing. A real run would stop "
                  "here, having changed nothing.");
        char at[48];
        snprintf(at, sizeof at, "first_bad_mib=%llu",
                 (unsigned long long)(bad / (1024 * 1024)));
        verdict_line("bad-sectors", at);
        return;
    }
    stage_say("surface  every sector of the reclaimed space reads back");

    stage_say("%s", "");   /* a blank line; the format attribute objects to "" */
    stage_say("This computer could be converted.");
    stage_say("Nothing has been changed.");
    /* WHAT WE COULD NOT ESTABLISH GOES IN THE ROW.
     *
     * ntfs.h says stage C may not run on a volume whose state we only
     * know because ntfsresize did not object. That rule is worth
     * nothing if the machine-readable row cannot tell the two apart,
     * so the three tri-states are in it -- and `sure=yes` is the only
     * value stage C will accept. */
    char how[200];
    snprintf(how, sizeof how,
             "disk=%s part=%s sector=%d/%d cluster=%u free_mib=%llu "
             "need_gb=%llu drive=%s power=%s sure=%s",
             on->name, win->name,
             on->logical_sector, on->physical_sector, ns.bytes_per_cluster,
             (unsigned long long)(freeable / (1024 * 1024)),
             (unsigned long long)(need / 1000000000ull),
             smart_verdict_name(sm.verdict), pw_ok ? "ok" : "battery",
             (!fde_unsure && ns.dirty == NTFS_NO &&
              ns.hibernated == NTFS_NO && ns.log_dirty == NTFS_NO)
                 ? "yes" : "no");
    verdict_line("could-convert", how);
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
    /* AND IF THIS IS A TEST IMAGE, IT SAYS SO BEFORE ANYTHING ELSE.
     * A fault-injection image can be told to stop dead in the middle
     * of rewriting a partition table. One must never be mistaken for a
     * real one, and the cheapest guard against that is that it
     * announces itself in the first three lines of every boot. */
    if (fault_build()) {
        stage_warn("THIS IS A FAULT-INJECTION BUILD. It can be told to stop");
        stage_warn("in the middle of writing to a disk. Never give it to");
        stage_warn("anybody, and never point it at a computer you care about.");
    }

    if (!stage_mount_pseudo())
        stage_warn("some of the kernel's own filesystems are missing; "
                   "going on, because what follows only reads");

    /* AFTER /proc IS MOUNTED, AND NOT BEFORE. The flag lives in
     * /proc/cmdline, so reading it any earlier reads nothing and every
     * dry run silently becomes a handover that was never asked for.
     * It is still read here, well before the first thing that can
     * fail, because the paths that fail are the ones a dry run most
     * needs to report on. */
    int dry = stage_cmdline_has("aurstage.dry");

    int n = stage_load_modules();
    stage_say("%d drivers loaded", n);

    if (!stage_wait_for_disks(20000)) {
        stage_warn("this computer's disk did not appear.");
        stage_report_controllers();
        /* THE MOST INTERESTING ROW IN THE WHOLE MATRIX, and it used to
         * produce none: the dry-run flag was read after this point, so
         * an Intel RST machine printed no report line and then sat in
         * stop_here()'s forever-pause. An unattended sweep of a
         * thousand laptops stalled on exactly the machines it was run
         * to find. */
        if (dry) {
            stage_say("aurstage-report v1 verdict=no-disk record=none -");
            stage_say("nothing on this computer has been changed");
            sync();
            reboot(RB_POWER_OFF);
        }
        stop_here("AurOS cannot see the drive on this computer yet.");
    }

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
    /* THE REAL RUN. Everything above this is the same in both modes:
     * the dry run and the install look at the machine identically,
     * and the only difference is whether anything is allowed to
     * happen afterwards. */
    /* PUT WINDOWS BACK. Checked before the installer, deliberately:
     * a machine arriving here with both flags set is a machine
     * somebody is trying to rescue, and the rescue wins. */
    if (stage_cmdline_has("aurstage.restore")) {
        restore_run(&m);
        stop_here("the restore stopped unexpectedly");
    }

    if (stage_cmdline_has("aurstage.install")) {
        install_run(&m);
        /* install_run never returns. */
        stop_here("the installer stopped unexpectedly");
    }

    if (dry) {
        dry_run(&m);
        stage_say("nothing on this computer has been changed");
        sync();
        reboot(RB_POWER_OFF);
        for (;;) pause();
    }

    char root[128];
    if (stage_cmdline_value("aurstage.root=", root, sizeof root) != 0) {
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
