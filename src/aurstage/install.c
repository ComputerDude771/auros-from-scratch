/* install.c — see install.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <time.h>

#include "install.h"
#include "journal.h"
#include "ntfs.h"
#include "shrink.h"
#include "health.h"
#include "fde.h"
#include "gpt.h"
#include "plan.h"
#include "wr.h"
#include "image.h"
#include "probe.h"
#include "commit.h"
#include "record.h"
#include "rescue.h"
#include "loader.h"
#include "fault.h"
#include "winvol.h"

#define MIN_ROOT_DEFAULT (24ull * 1000 * 1000 * 1000)

/* THE SAME NUMBER THE DRY RUN USES. It was hardcoded here while the
 * dry run read aurstage.min_gb= from the command line, so a machine
 * the dry run called convertible was refused by the install -- the
 * two halves disagreeing about the same question, which makes every
 * row in a hardware matrix a guess. */
static uint64_t min_root_bytes(void)
{
    char v[32];
    if (stage_cmdline_value("aurstage.min_gb=", v, sizeof v) == 0) {
        unsigned long long g = strtoull(v, NULL, 10);
        if (g >= 1 && g <= 4096) return g * 1000ull * 1000 * 1000;
    }
    return MIN_ROOT_DEFAULT;
}

/* ── saying things ───────────────────────────────────────────────── */

static rec_target g_rec;
static int        g_rec_ok;
static int        g_from_usb;
/* THE NO-STICK MODE: the journal says the image is on the Windows
 * drive and there is no memory stick at all. See winvol.h for what
 * that changes and image.h for why it is not the design. */
static int        g_nostick;

/* Whatever else happens on the way out, the Windows drive is not left
 * mounted behind a refusal. */
static void let_go_of_windows(void)
{
    char w[160];
    if (winvol_mounted() && winvol_umount(w, sizeof w) != 0)
        stage_warn("%s", w);
}

/* WHAT TO DO NEXT, and it depends on how this machine booted.
 *
 * If the staging environment came from the ESP, restarting reaches
 * Windows, because BootNext was one-shot and has already been
 * consumed. If it came from the recovery stick, restarting reaches
 * the STICK again -- the firmware entry points at it -- and the person
 * loops. */
static void say_what_happens_next(void)
{
    stage_say("%s", "");
    if (g_from_usb) {
        stage_say("Take the AurOS memory stick out of this computer, then");
        stage_say("turn it off and on again. Windows will start as usual.");
    } else {
        stage_say("Turn this computer off and on again. Windows will start");
        stage_say("as usual -- nothing on it has been changed.");
    }
    stage_say("To try again, open AurBridge in Windows and press");
    stage_say("Start installing.");
}

/* A refusal ABOVE the line: nothing has been touched. */
static void refuse(const char *why, const char *remedy, const char *verdict)
{
    let_go_of_windows();
    stage_warn("%s", why);
    if (remedy && remedy[0]) stage_say("         %s", remedy);
    if (g_rec_ok) {
        char w[200];
        rec_write(&g_rec, REC_REFUSED, verdict, w, sizeof w);
    }
    stage_say("aurstage-report v1 verdict=%s record=refused -", verdict);
    say_what_happens_next();
    for (;;) pause();
}

/* A failure BELOW the line: something has been changed, and the
 * sentence has to be honest about that without frightening somebody
 * whose Windows is in fact still fine. */
static void give_up(const char *why, const char *remedy, const char *verdict,
                    int windows_still_boots)
{
    let_go_of_windows();
    stage_warn("%s", why);
    if (remedy && remedy[0]) stage_say("         %s", remedy);
    if (g_rec_ok) {
        char w[200];
        rec_write(&g_rec, REC_FAILED, verdict, w, sizeof w);
    }
    stage_say("aurstage-report v1 verdict=%s record=failed -", verdict);
    if (windows_still_boots) {
        stage_say("%s", "");
        stage_say("Windows itself has not been damaged and will still start.");
        say_what_happens_next();
    } else {
        stage_say("%s", "");
        stage_say("Start this computer from the AurOS memory stick and");
        stage_say("choose \"Put Windows back\".");
    }
    for (;;) pause();
}

static void step(rec_step s, const char *note)
{
    if (!g_rec_ok) return;
    char w[200];
    rec_write(&g_rec, s, note, w, sizeof w);
}

static void dots(int pct)
{
    static int last = -1;
    if (pct == last || pct % 5) return;
    last = pct;
    fprintf(stderr, " %d%%", pct);
    if (pct >= 100) fprintf(stderr, "\n");
}

/* The same bar, with a fault point in it.
 *
 * `dots` is used by four different long steps, so a single fault point
 * inside it would mean "halfway through whichever of them got there
 * first" -- which is the image check, before the line, on every run. A
 * named instant that cannot be aimed at is not one. This one belongs to
 * the image write and to nothing else: a machine cut here has a root
 * partition half written, into space that is not in the partition table
 * yet, which is the case the "first megabyte last" rule exists for. */
static void dots_write(int pct)
{
    dots(pct);
    /* TWENTY-FIVE, NOT FIFTY, and the number matters.
     *
     * image_write_root() reports 0..50 for the write and 50..100 for
     * the read-back, so `pct >= 50` was not the middle of the write --
     * it was the moment the write FINISHED, which is a disk state
     * `write-end` already covers one loop iteration later. The point
     * exists to cut the machine with the root extent half written into
     * space that is not in the partition table yet, and to prove the
     * "first megabyte last" rule leaves nothing that will mount. That
     * is a quarter of the way through the whole call. */
    if (pct == 25) fault_maybe("write-mid");
}

/* And the same again for the boot partition.
 *
 * A SEPARATE FUNCTION FOR A SEPARATE PHASE, for the reason dots_write
 * gives: one fault point shared between two long steps means "halfway
 * through whichever of them ran first", which is not an instant
 * anybody can aim at. This one cuts the machine with the copy of the
 * image's EFI partition half written into space the table does not
 * mention yet -- the case the "first megabyte last" rule exists for on
 * this partition as well as on the root. */
static void dots_boot(int pct)
{
    dots(pct);
    if (pct == 25) fault_maybe("boot-mid");
}

static void commit_note(int n, void *ud)
{
    (void)ud;
    step(n == 1 ? REC_COMMIT_ARRAY : n == 2 ? REC_COMMIT_SECTOR
                                            : REC_COMMIT_BACKUP, NULL);
    /* The three most dangerous instants in the product, named so a
     * test can stop the machine at each of them. See fault.h. */
    fault_maybe(n == 1 ? "commit-array"
              : n == 2 ? "commit-sector" : "commit-backup");
}

/* The partition device the kernel will make for an entry index. */
static void part_dev(char *out, size_t n, const char *disk, int idx1)
{
    size_t l = strlen(disk);
    int digit = l && disk[l - 1] >= '0' && disk[l - 1] <= '9';
    snprintf(out, n, "/dev/%s%s%d", disk, digit ? "p" : "", idx1);
}

/* ── the run ─────────────────────────────────────────────────────── */

void install_run(const stage_machine *m)
{
    char why[280];

    stage_say("%s", "");
    stage_say("── installing AurOS ────────────────────────────────────");

    /* ── THE RECORD, READ BEFORE ANYTHING IS WRITTEN ────────────────
     *
     * The order here is the whole point. Asking what the last record
     * was AFTER writing "this run began" gets back the record just
     * written, so the resume ladder always decides "start fresh" --
     * and starting fresh after an interrupted shrink means running
     * ntfsresize again on a volume that may be halfway through one. */
    /* A FILE THAT IS THERE AND WILL NOT PARSE IS NOT THE SAME AS NO
     * FILE, which journal.h has said at length since the day it was
     * written -- and nothing, anywhere, read the flag that says which.
     *
     * `a() || b()` was the whole of it, and journal_read() begins with
     * memset, so the second call erased the `corrupt` the first had
     * set. A half-written journal -- which is exactly what a power cut
     * during the Windows phase leaves -- came out of this as
     * have=0, corrupt=0 and the person was told "This looks like a
     * memory stick that was left plugged in." about her own computer,
     * after the restart, with the installer's own damaged note sitting
     * on the EFI partition.
     *
     * The two are different machines and they get different sentences.
     * Neither touches the disk: a record that will not parse cannot be
     * checked against this machine, and an install that cannot be
     * checked does not happen. */
    journal j;
    int corrupt = 0;
    int have = journal_read("/aurbridge/journal.json", &j);
    if (!have) {
        corrupt = j.corrupt;
        have = journal_read("/run/aurbridge/journal.json", &j);
        if (!have && j.corrupt) corrupt = 1;
    }
    if (!have && corrupt) {
        refuse("The note the installer left on this computer is damaged, so "
               "AurOS cannot tell whether this is the computer it was "
               "prepared for.",
               "Nothing has been changed. Start the computer again and it "
               "will come back to Windows; then run the installer once more.",
               "record-damaged");
    }
    if (!have) {
        refuse("Nothing on this computer asked for AurOS to be installed.",
               "This looks like a memory stick that was left plugged in.",
               "no-record");
    }
    g_from_usb = !strcmp(j.boot_from, "usb");
    g_nostick  = !strcmp(j.image_on, "windows");
    if (j.image_on[0] && !g_nostick && strcmp(j.image_on, "stick") != 0)
        refuse("The note the installer left says the copy of AurOS is "
               "somewhere this version of AurOS does not know how to look.",
               "Run the installer in Windows again.", "image-on-unknown");
    if (g_nostick && !j.profile[0])
        refuse("The note the installer left does not say which AurOS to "
               "install.", "Run the installer in Windows again.",
               "no-profile");

    /* NO STICK, NO RECORD. The record lives in a raw partition on the
     * stick (record.h says why not on the ESP), so the no-stick mode has
     * none and does not go looking: rec_open would otherwise write this
     * run's notes onto whatever old AurOS stick happens to be plugged
     * in. The cost is the one record.h names -- an interrupted install
     * starts over -- and starting over is safe here because every step
     * up to the commit leaves the old table in force, and NTFS itself
     * says when a shrink was interrupted (ntfsresize marks the volume
     * dirty, and the gate below refuses a dirty volume). */
    g_rec_ok = g_nostick ? 0
             : (rec_open(&g_rec, m, j.run_id, why, sizeof why) == 0);
    rec_entry last;
    int had_last = g_rec_ok && rec_last(&g_rec, &last) == 0;
    int resume_from = REC_NONE;
    if (had_last && rec_is_ours(&g_rec, &last)) {
        resume_from = (int)last.step;
        stage_say("record   this install already reached: %s",
                  rec_step_name((rec_step)last.step));
    } else if (had_last) {
        /* SOMEBODY ELSE'S HISTORY. A refusal from a previous attempt
         * is not this attempt's progress. */
        stage_say("record   an older attempt is on the stick; ignoring it");
    }
    if (g_nostick)
        stage_say("record   no memory stick: an interrupted install will "
                  "start again from the beginning");
    else if (!g_rec_ok)
        stage_warn("AurOS cannot write down what it is doing; going on, "
                   "but an interrupted install will not be resumable");

    /* Anything past the commit means the disk is already the new
     * shape, and this program has no business starting over on it. */
    if (resume_from >= REC_COMMIT_SECTOR && resume_from != REC_REFUSED &&
        resume_from != REC_FAILED) {
        give_up("AurOS has already been installed on this computer.",
                "Restart it and choose AurOS from the menu.",
                "already-installed", 1);
    }
    /* An interrupted shrink is the one row of the power-loss table
     * with no automatic answer. */
    if (resume_from == REC_SHRINK_BEGIN) {
        give_up("The last attempt was interrupted while it was resizing the "
                "Windows drive.",
                "Windows may need to check itself. Start it and let it.",
                "shrink-interrupted", 1);
    }

    step(REC_BEGIN, NULL);

    /* ── THE GATE ──────────────────────────────────────────────────
     * Everything below is answerable with the disk untouched. */

    journal_verdict jv = journal_check(&j, m, why, sizeof why);
    if (jv != JOURNAL_MATCH)
        refuse(why, "The installer will check this computer again.",
               journal_verdict_name(jv));
    stage_say("record   %s", why);

    /* Which disk, and which partition, named by the record. */
    /* THE SAME DISK journal_check just matched, found the same way --
     * not a second, stricter search that could disagree with it. */
    const stage_disk *disk = journal_disk(&j, m, NULL);
    if (!disk)
        refuse("This is not the computer the installer was prepared for.",
               NULL, "wrong-disk");
    if (!disk->sector_known)
        refuse("This computer will not say what size its disk's sectors are.",
               NULL, "sector-size-unknown");
    if (!disk->gpt)
        refuse("This computer divides its disk up in an older way that "
               "AurOS cannot install alongside.", NULL, "not-gpt");

    char diskdev[80];
    snprintf(diskdev, sizeof diskdev, "/dev/%s", disk->name);

    /* The Windows partition, and the encryption check at the site that
     * is about to touch its geometry -- not inherited from a tri-state
     * some other subsystem computed earlier in the run. */
    const stage_part *win = NULL;
    for (int k = 0; k < disk->n_parts; k++)
        if (disk->part[k].start_lba == j.win_start_lba) win = &disk->part[k];
    if (!win)
        refuse("The Windows part of this disk is not where it was.",
               NULL, "moved");
    char windev[80];
    snprintf(windev, sizeof windev, "/dev/%s", win->name);

    ntfs_state ns;
    ntfs_read_state(windev, &ns);
    if (ns.verdict != NTFS_OK)
        refuse(ns.why, ns.remedy, ntfs_verdict_name(ns.verdict));
    /* STAGE C MAY NOT PROCEED ON AN UNSURE. ntfs.h says so: stage B
     * may go on because ntfsresize is the backstop, but by here the
     * answer decides whether a partition gets rewritten. */
    if (ns.dirty == NTFS_UNSURE || ns.hibernated == NTFS_UNSURE ||
        ns.log_dirty == NTFS_UNSURE)
        refuse("AurOS cannot tell whether this Windows drive was closed down "
               "properly.",
               "Start Windows, shut it down from the Start menu, and try "
               "again.", "ntfs-unsure");

    /* Somebody else's encryption. */
    char who[64];
    char espdev[80]; espdev[0] = 0;
    for (int k = 0; k < disk->n_parts; k++)
        if (disk->part[k].is_esp)
            snprintf(espdev, sizeof espdev, "/dev/%s", disk->part[k].name);
    fde_verdict fv = fde_scan_disk(diskdev, espdev[0] ? espdev : NULL,
                                   who, sizeof who);
    if (fv == FDE_NAMED) {
        snprintf(why, sizeof why,
                 "This computer's drive is protected by %s.", who);
        refuse(why, "That has to be turned off and the drive decrypted first.",
               "third-party-encryption");
    }
    if (fv == FDE_UNSURE)
        refuse("AurOS could not check whether this drive is encrypted by "
               "other software.", NULL, "encryption-unsure");

    /* The drive's own opinion of itself. */
    smart_state sm;
    smart_read(diskdev, &sm);
    if (sm.verdict == SMART_FAILING)
        refuse(sm.why, sm.remedy, "drive-failing");
    stage_say("drive    %s", sm.why);

    /* And whether it would survive being operated on. */
    power_state pw; char pwhy[200];
    power_read(NULL, &pw);
    if (!power_ok(&pw, pwhy, sizeof pwhy))
        refuse(pwhy, "Plug it in and try again.", "on-battery");
    stage_say("power    %s", pwhy);

    /* The image, found and hashed BEFORE anything is touched. */
    image_src img;
    /* THE PROFILE THE JOURNAL NAMES, not the first image it comes to.
     *
     * This argument was `j.stage[0] ? NULL : NULL` -- NULL either way,
     * written to look like a decision. image_find() has been able to
     * insist on a profile since it was written; there was nothing on
     * this side of the restart that knew which one, so it never was.
     * Now the journal carries it, and image_find walks every disk in
     * the machine, so a second AurOS stick left plugged in no longer
     * decides this by being reached first.
     *
     * A journal from before that has an empty profile, and an empty
     * profile asks for no check, which is exactly what happened
     * before. */
    char winroot[64] = "";
    if (g_nostick) {
        /* READ-ONLY, and for as short a time as possible: mounted to
         * find and hash the image, unmounted again before the shrink. */
        if (winvol_mount(windev, winroot, sizeof winroot, why, sizeof why) != 0)
            refuse(why, NULL, "windows-unreadable");
        char ip[160], mp[176];
        snprintf(ip, sizeof ip, "%s/AurOS/auros-%s.img", winroot, j.profile);
        snprintf(mp, sizeof mp, "%s.manifest", ip);
        if (image_find_file(ip, mp, j.profile, &img, why, sizeof why) != 0)
            refuse(why, "Run the installer in Windows again.", "no-image");
    } else if (image_find(m, j.profile, &img, why, sizeof why) != 0) {
        refuse(why, NULL, "no-image");
    }
    stage_say("image    %s, %.1f GiB", img.profile,
              (double)img.root_len / (1024.0*1024.0*1024.0));
    stage_say(g_nostick ? "checking the copy of AurOS on the Windows drive"
                        : "checking the copy of AurOS on the memory stick");
    fprintf(stderr, "aurstage: ");
    if (image_verify(&img, dots, why, sizeof why) != 0)
        refuse(why, NULL, "image-damaged");

    /* HOW MUCH ROOM THE THING THAT STARTS THE COMPUTER NEEDS, asked
     * here, before the plan, so that an image whose boot partition
     * will not fit is refused with the disk untouched.
     *
     * AND IT COMES FROM THE IMAGE, NOT FROM A CONSTANT. It was 600 MiB
     * in a #define, chosen when that partition was going to hold a
     * rescue kernel and a copy of this machine's Windows startup as
     * well; both of those moved elsewhere, and what is left is exactly
     * one thing -- the image's own EFI partition, copied whole, see
     * loader.h. A constant that disagrees with the thing being copied
     * is either gigabytes of somebody's Windows taken for nothing or
     * an install that fails after the shrink. */
    uint64_t boot_need = 0;
    if (loader_bytes_needed(&img, &boot_need, why, sizeof why) != 0)
        refuse(why, NULL, "no-boot-area");
    /* Everything this run needs from the Windows drive's FILES has been
     * read and checked; from here to the copy, it is closed. */
    if (g_nostick && winvol_umount(why, sizeof why) != 0)
        refuse(why, NULL, "windows-still-open");

    /* ── THE WAY BACK, AND IT IS NOT OPTIONAL ──────────────────────
     *
     * R4: a single-PC household whose install goes wrong has no second
     * computer to read instructions on and no way to make a stick. So
     * the stick is mandatory. It is FOUND and MEASURED here, because
     * the layout below has to leave room for the copy and the size of
     * that copy is dominated by this machine's EFI partition -- 100 MB
     * on one laptop and a gigabyte on the next. The copy itself is
     * made last, further down. */
    rescue_area rsc;
    memset(&rsc, 0, sizeof rsc);
    uint64_t rsc_need = 0;
    if (g_nostick) {
        /* NO STICK: THE COPY IS HELD IN MEMORY until the shrink has made
         * room for it on this disk, and written there before anything
         * else is. It is taken HERE, before the shrink, because the
         * shrink rewrites the Windows volume's own boot sector -- a copy
         * made afterwards would remember the smaller size, and "put
         * Windows back" would put back a smaller Windows. */
        if (rescue_size_needed(disk, &rsc_need, why, sizeof why) != 0)
            refuse(why, NULL, "cannot-size-rescue");
        uint64_t avail = stage_mem_available();
        if (!avail || rsc_need + (256ull << 20) > avail)
            refuse("This computer does not have enough memory to hold a copy "
                   "of its Windows startup while AurOS is installed without "
                   "a memory stick.",
                   "Install with a memory stick instead.", "no-memory");
        snprintf(rsc.dev, sizeof rsc.dev, "/run/aurstage/saved.bin");
        rsc.part_off = 0;
        rsc.part_bytes = rsc_need;
        if (wr_scratch(rsc.dev, rsc_need, why, sizeof why) != 0)
            refuse(why, "Install with a memory stick instead.", "no-memory");
    } else {
        if (rescue_find(m, disk->name, &rsc, why, sizeof why) != 0)
            refuse(why, "Make the AurOS memory stick again and start over.",
                   "no-rescue-area");
        if (rescue_size_needed(disk, &rsc_need, why, sizeof why) != 0)
            refuse(why, NULL, "cannot-size-rescue");
        if (rsc_need > rsc.part_bytes)
            refuse("The AurOS memory stick does not have room to save this "
                   "computer's Windows startup.",
                   "Make the stick again on a larger drive.", "rescue-too-small");
    }

    /* The table, and the plan. */
    int dfd = open(diskdev, O_RDONLY | O_CLOEXEC);
    gpt_table old;
    int gok = dfd >= 0 && gpt_read(dfd, (uint32_t)disk->logical_sector,
                                   disk->bytes, &old) == 0;
    if (dfd >= 0) close(dfd);
    if (!gok)
        refuse("This computer's partition table could not be read.",
               NULL, "no-table");

    uint32_t ss = old.sector;
    int wi = gpt_find_start(&old, win->start_lba * 512ull / ss);
    if (wi < 0)
        refuse("The Windows partition is not in this disk's table where it "
               "was expected.", NULL, "moved");

    /* How small it can actually get. */
    stage_say("measuring how small the Windows drive can get");
    shrink_plan sp;
    shrink_ask(windev, &sp);
    if (!sp.ok)
        refuse(sp.why, NULL, "cannot-measure");

    /* The surface of the space that would be reclaimed. R5. */
    stage_say("reading every sector of the space that would be reclaimed");
    fprintf(stderr, "aurstage: ");
    uint64_t bad = 0;
    if (surface_test(windev, sp.smallest_bytes, sp.current_bytes,
                     (uint32_t)disk->logical_sector, &bad, NULL) != 0) {
        snprintf(why, sizeof why,
                 "This disk could not read the space %llu MB into the Windows "
                 "drive. The drive is failing.",
                 (unsigned long long)(bad / (1024 * 1024)));
        refuse(why, "Copy your files onto a USB stick today and have the "
                    "drive replaced.", "bad-sectors");
    }
    fprintf(stderr, "\n");

    /* A layout that fits, checked against the table. */
    stage_layout L;
    if (plan_compute(&old, wi, sp.smallest_bytes, img.root_len,
                     boot_need, rsc_need, min_root_bytes(), &L,
                     why, sizeof why) != 0)
        refuse(why, NULL, "no-room");
    plan_say(&L);

    /* ── THE WAY BACK, AND IT IS NOT OPTIONAL ──────────────────────
     *
     * R4: a single-PC household whose install goes wrong has no second
     * computer to read instructions on and no way to make a stick. So
     * the stick is mandatory, it is checked here, and the copy of this
     * machine's startup is made here -- LAST in the gate, because it
     * is the only gate item that takes minutes and writes half a
     * gigabyte, and spending that before the cheap refusals would be
     * rude to everybody it is going to refuse anyway. */
    stage_say("%s", "");
    stage_say("Saving this computer's Windows startup, so it can be put back.");
    fprintf(stderr, "aurstage: ");
    if (rescue_capture(disk, &rsc, j.run_id, dots, why, sizeof why) != 0)
        refuse(why, NULL, "rescue-capture-failed");
    fprintf(stderr, "\n");
    stage_say("saved    %llu MB, read back and checked",
              (unsigned long long)(rsc_need / (1024 * 1024)));

    step(REC_GATE_OK, NULL);
    fault_maybe("gate");
    stage_say("%s", "");
    stage_say("Everything AurOS can check has been checked.");

    /* ════ BELOW THIS LINE THE DISK CHANGES ════════════════════════ */

    stage_say("%s", "");
    stage_say("Making room on the Windows drive. This is the only part");
    stage_say("that cannot be undone. Do not turn the computer off.");
    step(REC_SHRINK_BEGIN, NULL);
    fault_maybe("shrink-begin");
    fprintf(stderr, "aurstage: ");
    shrink_result sr;
    shrink_do(windev, sp.smallest_bytes, dots, &sr);
    if (!sr.ok) {
        if (!sr.started)
            give_up(sr.why, "Nothing on the Windows drive was touched.",
                    "shrink-could-not-start", 1);
        give_up("The Windows drive could not be resized.",
                "Start Windows and let it check the drive.",
                "shrink-failed", 1);
    }
    step(REC_SHRINK_END, sr.why);
    fault_maybe("shrink-end");
    stage_say("windows  %s", sr.why);

    /* RE-PLAN FROM WHAT THE FILESYSTEM ACTUALLY CAME OUT AT, not from
     * what was asked for: ntfsresize rounds to its own cluster
     * boundary, and a partition entry that ends below its filesystem
     * is a filesystem whose last blocks are outside its partition. */
    if (plan_compute(&old, wi, sr.achieved_bytes, img.root_len,
                     boot_need, rsc_need, min_root_bytes(), &L,
                     why, sizeof why) != 0)
        give_up(why, "The Windows drive is smaller but nothing else has "
                     "changed.", "no-room-after-shrink", 1);

    /* ── phases 5 and 6 ──────────────────────────────────────────── */
    wr_target t;
    if (wr_open(&t, diskdev, why, sizeof why) != 0)
        give_up(why, NULL, "disk-not-writable", 1);
    uint64_t root_off = L.root_first * (uint64_t)ss;
    uint64_t root_end = (L.root_last + 1) * (uint64_t)ss;
    /* NO STICK: THE WAY BACK GOES ONTO THIS DISK FIRST, before a single
     * byte of AurOS, into the space the shrink has just freed and the
     * commit will make a partition of. Until it is there and read back
     * there is no copy of this machine's startup anywhere but in memory
     * -- which is why a failure here stops the install rather than
     * warning about it, as the stick mode's second copy does. */
    if (g_nostick) {
        stage_say("%s", "");
        stage_say("Keeping the way back on this computer, before anything "
                  "else is written.");
        rescue_payload rp0;
        if (wr_arm(&t, WR_MIRROR, L.rsc_first * (uint64_t)ss,
                   (L.rsc_last + 1) * (uint64_t)ss, why, sizeof why) != 0 ||
            rescue_open(&rsc, &rp0, why, sizeof why) != 0 ||
            rescue_mirror(&t, &rsc, &rp0, L.rsc_first * (uint64_t)ss,
                          why, sizeof why) != 0)
            give_up(why, "The Windows drive is smaller but still works.",
                    "saved-copy-failed", 1);
        wr_disarm(&t, WR_MIRROR);
        stage_say("saved    the way back is on this computer, read back and "
                  "checked");
        fault_maybe("mirror-end");
        /* And the Windows drive again, read-only, to copy AurOS out of
         * it into space outside it. */
        if (winvol_mount(windev, winroot, sizeof winroot, why, sizeof why) != 0)
            give_up(why, "The Windows drive is smaller but still works.",
                    "windows-unreadable", 1);
    }
    if (wr_arm(&t, WR_ROOT, root_off, root_end, why, sizeof why) != 0)
        give_up(why, NULL, "cannot-arm", 1);

    stage_say("%s", "");
    stage_say("Copying AurOS onto this computer.");
    step(REC_WRITE_BEGIN, NULL);
    fprintf(stderr, "aurstage: ");
    if (image_write_root(&t, &img, root_off, dots_write, why, sizeof why) != 0)
        give_up(why, "The Windows drive is smaller but still works.",
                "write-failed", 1);
    step(REC_WRITE_END, NULL);
    fault_maybe("write-end");
    wr_disarm(&t, WR_ROOT);

    /* ── phase 8a: the thing that starts the computer ──────────────
     *
     * Into the gap, while the OLD table is still in force, exactly
     * like the root filesystem above it. Rule 4 is untouched: the
     * layout still changes at one sector, further down. A machine
     * that loses power in here is the "5-7 write and verify" row --
     * Windows smaller, old table, garbage in free space, and
     * BootOrder still pointing at Windows Boot Manager. */
    stage_say("%s", "");
    stage_say("Making this computer able to start AurOS.");
    step(REC_BOOT_BEGIN, NULL);
    fprintf(stderr, "aurstage: ");
    if (loader_write_boot(&t, &img, &L, ss, dots_boot, why, sizeof why) != 0)
        give_up(why, "The Windows drive is smaller but still works.",
                "boot-write-failed", 1);
    fprintf(stderr, "\n");
    step(REC_BOOT_END, NULL);
    fault_maybe("boot-end");
    if (g_nostick && winvol_umount(why, sizeof why) != 0)
        give_up(why, "The Windows drive is smaller but still works.",
                "windows-still-open", 1);

    /* ── phase 7 ─────────────────────────────────────────────────── */
    stage_say("%s", "");
    stage_say("Checking that this computer works under AurOS.");
    /* The new root is not a partition yet -- the table has not been
     * committed -- so it is probed through a loop of its own extent.
     * There is no loop device here either, so the probe is given the
     * whole disk and the offset it starts at. */
    char rootdev[80];
    probe_result pr;
    probe_run_at(diskdev, root_off, &pr);
    if (pr.verdict != PROBE_OK)
        give_up(pr.why, pr.remedy, probe_verdict_name(pr.verdict), 1);
    stage_say("checks   %s", pr.why);
    step(REC_PROBE_END, pr.why);

    /* ── phase 8 ─────────────────────────────────────────────────── */
    gpt_table nw;
    if (commit_build(&old, &L, &nw, why, sizeof why) != 0)
        give_up(why, NULL, "cannot-build-table", 1);

    size_t ab = gpt_array_bytes(&nw);
    uint64_t alt = nw.disk_sectors - 1;
    uint64_t barr = alt - (ab + ss - 1) / ss;
    if (wr_arm(&t, WR_GPT_PRIMARY, 1ull * ss, nw.entry_lba * ss + ab,
               why, sizeof why) != 0 ||
        wr_arm(&t, WR_GPT_BACKUP, barr * ss, (alt + 1) * ss,
               why, sizeof why) != 0)
        give_up(why, NULL, "cannot-arm-table", 1);

    /* THE PLAN, CHECKED AGAINST THE TABLE ONE LAST TIME. plan.h says
     * this happens "before every step that acts on the layout"; it
     * happened once, inside plan_compute, and then five steps acted on
     * it. This is the cheapest and most valuable of the five: after it
     * the table is committed and nothing can be taken back. */
    if (plan_check(&old, &L, img.root_len, boot_need, rsc_need,
                   min_root_bytes(), why, sizeof why) != 0)
        give_up(why, "The Windows drive is smaller but still works.",
                "plan-changed", 1);

    stage_say("%s", "");
    stage_say("Writing the new layout.");
    if (commit_table(&t, &nw, commit_note, NULL, why, sizeof why) != 0)
        give_up(why, NULL, "commit-failed", 1);
    /* AND THE TWO MOST DANGEROUS WINDOWS IN THE PRODUCT ARE SHUT.
     *
     * wr.h: "Disarm one window, so that a phase which is finished
     * cannot write again. Called as each phase completes." These two
     * were armed and never disarmed, so the window containing LBA 1
     * stayed open through the boot entry, through half a gigabyte of
     * rescue mirror, until wr_close. Nothing reaches it today; the
     * point of the file is that nothing can. */
    wr_disarm(&t, WR_GPT_PRIMARY);
    wr_disarm(&t, WR_GPT_BACKUP);

    /* ── phase 8c: the firmware's menu ─────────────────────────────
     *
     * The boot entry names the partition the commit has just made, by
     * the GUID the commit generated, so it cannot be written before
     * this point. BootOrder is NOT touched -- switching this machine
     * on still reaches Windows until the user has seen AurOS work.
     * What is armed is BootNext, which the firmware clears as it uses
     * it: the next start reaches AurOS once, and a machine that
     * cannot start AurOS comes back to Windows by itself.
     *
     * A failure is a warning. AurOS is on the disk and verified; what
     * is missing is a line in a menu, and the sentence says how to
     * get there without it. */
    uint16_t boot_entry = 0;
    int reg = loader_register(&nw, &L, &boot_entry, why, sizeof why);
    if (reg > 0) {
        /* THE ENTRY IS THERE AND ONLY THE ONE-SHOT FAILED, which is a
         * different sentence. Saying "could not add itself to the
         * start-up menu" about a machine whose menu now has AurOS in
         * it sends a person to do the whole install again. */
        stage_warn("AurOS is in this computer's start-up menu, but this "
                   "computer would not be asked to start it next time (%s).",
                   why);
        stage_say("         Hold the key your computer shows at start-up for "
                  "a boot menu");
        stage_say("         and choose AurOS from it.");
    } else if (reg < 0) {
        stage_warn("AurOS is installed but could not add itself to this "
                   "computer's start-up menu (%s).", why);
        stage_say("         Hold the key your computer shows at start-up for "
                  "a boot menu");
        stage_say("         and choose AurOS from it.");
    } else {
        stage_say("startup  AurOS is in this computer's start-up menu "
                  "(entry %04X); Windows is still what it starts by "
                  "default", boot_entry);
    }
    step(REC_BOOT_ENTRY, NULL);
    fault_maybe("boot-entry");

    /* A SECOND COPY OF THE WAY BACK, on the machine itself.
     *
     * The stick already holds one and is mandatory; this is for the
     * person who, eighteen months from now, has reused the stick for
     * holiday photographs and wants Windows back. It is written after
     * the commit because the partition it goes in does not exist until
     * then, and a failure here is a WARNING and not a give_up: AurOS
     * is installed and working at this point, and the copy that
     * matters -- the one that survives this disk failing -- is on the
     * stick either way. */
    /* In the no-stick mode it is already there: it went down before
     * the image did, and was the only copy until then. */
    if (!g_nostick) {
        stage_say("Keeping a copy of the way back on this computer too.");
        rescue_payload rp;
        if (wr_arm(&t, WR_MIRROR, L.rsc_first * (uint64_t)ss,
                   (L.rsc_last + 1) * (uint64_t)ss, why, sizeof why) != 0 ||
            rescue_open(&rsc, &rp, why, sizeof why) != 0 ||
            rescue_mirror(&t, &rsc, &rp, L.rsc_first * (uint64_t)ss,
                          why, sizeof why) != 0)
            stage_warn("the copy of the way back could not be kept on this "
                       "computer (%s). The one on the memory stick is fine; "
                       "keep the stick.", why);
        else
            stage_say("saved    a second copy is on this computer");
        wr_disarm(&t, WR_MIRROR);
    }
    wr_close(&t);

    /* From here Windows is still bootable -- its partition entry
     * matches its filesystem -- but the disk is the new shape. */
    int rootidx = -1;
    for (uint32_t k = 0; k < nw.n_entries; k++)
        if (gpt_used(&nw.ent[k]) && nw.ent[k].first == L.root_first)
            rootidx = (int)k;
    part_dev(rootdev, sizeof rootdev, disk->name, rootidx + 1);

    stage_say("Making AurOS fill the space it was given.");
    int st = commit_settle(diskdev, rootdev, why, sizeof why);
    if (st < 0)
        give_up(why, NULL, "settle-failed", 1);
    if (st > 0)
        stage_warn("%s", why);
    step(REC_SETTLE_END, NULL);
    fault_maybe("settle-end");
    step(REC_DONE, NULL);

    stage_say("%s", "");
    stage_say("aurstage-report v1 verdict=installed record=done disk=%s",
              disk->name);
    stage_say("AurOS is installed. Starting it now.");
    sync();
    if (stage_switch_root(rootdev) != 0)
        give_up("AurOS is installed but did not start.",
                "Turn this computer off and on again.", "handover-failed", 1);
    for (;;) pause();
}

/* A restore that cannot go on stops the same way the installer does:
 * a sentence, a power-off, and no reboot loop. Separate from the
 * installer's give_up() because there is no install record to write to
 * here and nothing below the line to warn about. */
static void stop_restore(void)
{
    stage_say("%s", "");
    stage_say("Turn this computer off with the power button, and start it");
    stage_say("again with the AurOS memory stick plugged in.");
    sync();
    for (;;) pause();
}

/* ── "Put Windows back" ──────────────────────────────────────────────
 *
 * The other half of rule 2. This runs in the same staging environment
 * as the install, started by `aurstage.restore` on the kernel command
 * line, which is what the button in the AurOS settings panel arms
 * before it restarts the machine. It does NOT run inside AurOS: see
 * the header of rescue.h for why an in-place restore cannot be safe.
 *
 * WHICH SAVED COPY, when there are normally two. The stick's and the
 * machine's own are byte-identical when both are healthy, so the
 * choice only matters when one of them is not -- and the one most
 * likely not to be is the copy sitting on the disk that has gone
 * wrong. So the stick is preferred, every candidate is opened and
 * fully verified before any of them is used, and a machine with a
 * damaged stick and a good on-disk copy still gets its Windows back.
 */
static void restore_say(const char *line, void *ud)
{ (void)ud; stage_say("%s", line); }

void restore_run(const stage_machine *m)
{
    char why[400];
    stage_say("%s", "");
    stage_say("── putting Windows back ────────────────────────────────");

    rescue_area areas[STAGE_MAX_DISK * 2];
    int n = rescue_find_n(m, areas, (int)(sizeof areas / sizeof areas[0]));
    if (n == 0) {
        stage_warn("there is no saved copy of this computer's Windows "
                   "startup, on this computer or on any drive plugged "
                   "into it.");
        stage_say("         Plug in the AurOS memory stick and try again.");
        stage_say("aurstage-report v1 verdict=restore-nothing-saved "
                  "record=none -");
        stop_restore();
    }

    /* Open every candidate, and work out which disk each describes.
     * Nothing is written until one of them has been read end to end
     * and every hash in it has matched. */
    int best = -1, best_disk = -1, best_on_target = 1;
    for (int i = 0; i < n; i++) {
        rescue_payload p;
        if (rescue_open(&areas[i], &p, why, sizeof why) != 0) {
            stage_warn("a saved copy on %s could not be read: %s",
                       areas[i].dev, why);
            continue;
        }
        /* EVERY CANDIDATE IS READ END TO END HERE, not later.
         *
         * This loop used to check only the 4 KiB header, which is what
         * rescue_open reads, and rescue_restore did the full check on
         * whichever one was chosen. So a stick with an intact header
         * and bit-rot anywhere in its 900 MB body was PREFERRED (being
         * off-target), failed its hashes inside rescue_restore, and the
         * whole run stopped -- with a perfectly good copy sitting on
         * the machine's own disk that was never tried. The fallback
         * this file's own comment promises did not exist. */
        stage_say("checking the saved copy on %s", areas[i].dev);
        if (rescue_verify(&areas[i], &p, NULL, why, sizeof why) != 0) {
            stage_warn("the saved copy on %s is damaged: %s",
                       areas[i].dev, why);
            continue;
        }
        /* WHICH DISK IT IS OF, and a serial in the capture is an answer
         * rather than a hint. The first version fell through to
         * matching on size whenever THIS disk would not state a serial,
         * even when the capture named one -- and it had no `break`, so
         * on a desktop with two identical drives the LAST one won. */
        int di = -1;
        for (int k = 0; k < m->n_disks && di < 0; k++) {
            const stage_disk *d = &m->disk[k];
            if (p.serial[0]) {
                if (d->serial[0] && !strcmp(p.serial, d->serial)) di = k;
            } else if (d->bytes == p.disk_bytes && !d->removable) {
                di = k;
            }
        }
        if (di < 0 && p.serial[0]) {
            /* The capture names a disk and no disk here says that name.
             * Size alone then decides, and only when exactly one disk
             * is that size -- two are a refusal, not a coin toss. */
            int hits = 0;
            for (int k = 0; k < m->n_disks; k++)
                if (m->disk[k].bytes == p.disk_bytes && !m->disk[k].removable)
                    { hits++; di = k; }
            if (hits != 1) {
                di = -1;
                stage_warn("a saved copy on %s names a disk this computer "
                           "does not report, and %d disks here are its size; "
                           "AurOS will not guess", areas[i].dev, hits);
            }
        }
        if (di < 0) {
            stage_warn("a saved copy on %s is of a different computer's "
                       "disk; ignoring it", areas[i].dev);
            continue;
        }
        char target[80];
        snprintf(target, sizeof target, "/dev/%s", m->disk[di].name);
        int on_target = strcmp(target, areas[i].dev) == 0;
        /* Prefer a copy that is NOT on the disk being rescued. */
        if (best < 0 || (best_on_target && !on_target)) {
            best = i; best_disk = di; best_on_target = on_target;
        }
    }
    if (best < 0) {
        stage_warn("no readable saved copy of this computer's Windows "
                   "startup could be found.");
        stage_say("aurstage-report v1 verdict=restore-no-usable-copy "
                  "record=none -");
        stop_restore();
    }

    char diskdev[80];
    snprintf(diskdev, sizeof diskdev, "/dev/%s", m->disk[best_disk].name);
    stage_say("using the saved copy on %s%s", areas[best].dev,
              best_on_target ? " (this computer's own disk)"
                             : " (the memory stick)");
    stage_say("restoring %s", diskdev);

    rescue_outcome oc;
    if (rescue_restore(diskdev, &areas[best], restore_say, NULL, &oc,
                       why, sizeof why) != 0) {
        stage_warn("%s", why);
        stage_say("aurstage-report v1 verdict=restore-failed record=none "
                  "table=%d esp=%d", oc.table_restored, oc.esp_restored);
        stop_restore();
    }
    stage_say("%s", "");
    stage_say("aurstage-report v1 verdict=restored record=done "
              "disk=%s table=%d esp=%d grown=%d small=%d mbr=%d",
              m->disk[best_disk].name, oc.table_restored, oc.esp_restored,
              oc.volumes_grown, oc.volumes_left_small, oc.mbr_restored);
    stage_say("%s", "");
    stage_say("Windows is back. This computer will switch itself off;");
    stage_say("take the memory stick out and start it again.");
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
