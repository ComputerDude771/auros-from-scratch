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
#include "fault.h"

/* 600 MiB: enough for a rescue kernel, an initramfs, the captured ESP
 * and this machine's payload, with room for the payload to grow. */
#define RECOVERY_BYTES   (600ull * 1024 * 1024)
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
    if (pct >= 50 && pct < 55) fault_maybe("write-mid");
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
    journal j;
    int have = journal_read("/aurbridge/journal.json", &j) ||
               journal_read("/run/aurbridge/journal.json", &j);
    if (!have) {
        refuse("Nothing on this computer asked for AurOS to be installed.",
               "This looks like a memory stick that was left plugged in.",
               "no-record");
    }
    g_from_usb = !strcmp(j.boot_from, "usb");

    g_rec_ok = (rec_open(&g_rec, m, j.run_id, why, sizeof why) == 0);
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
    if (!g_rec_ok)
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
    const stage_disk *disk = NULL;
    for (int i = 0; i < m->n_disks && !disk; i++)
        if (m->disk[i].serial[0] && !strcmp(m->disk[i].serial, j.disk_serial))
            disk = &m->disk[i];
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
    if (image_find(m, j.stage[0] ? NULL : NULL, &img, why, sizeof why) != 0)
        refuse(why, NULL, "no-image");
    stage_say("image    %s, %.1f GiB", img.profile,
              (double)img.root_len / (1024.0*1024.0*1024.0));
    stage_say("checking the copy of AurOS on the memory stick");
    fprintf(stderr, "aurstage: ");
    if (image_verify(&img, dots, why, sizeof why) != 0)
        refuse(why, NULL, "image-damaged");

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
    if (rescue_find(m, disk->name, &rsc, why, sizeof why) != 0)
        refuse(why, "Make the AurOS memory stick again and start over.",
               "no-rescue-area");
    uint64_t rsc_need = 0;
    if (rescue_size_needed(disk, &rsc_need, why, sizeof why) != 0)
        refuse(why, NULL, "cannot-size-rescue");
    if (rsc_need > rsc.part_bytes)
        refuse("The AurOS memory stick does not have room to save this "
               "computer's Windows startup.",
               "Make the stick again on a larger drive.", "rescue-too-small");

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
                     RECOVERY_BYTES, rsc_need, min_root_bytes(), &L,
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
                     RECOVERY_BYTES, rsc_need, min_root_bytes(), &L,
                     why, sizeof why) != 0)
        give_up(why, "The Windows drive is smaller but nothing else has "
                     "changed.", "no-room-after-shrink", 1);

    /* ── phases 5 and 6 ──────────────────────────────────────────── */
    wr_target t;
    if (wr_open(&t, diskdev, why, sizeof why) != 0)
        give_up(why, NULL, "disk-not-writable", 1);
    uint64_t root_off = L.root_first * (uint64_t)ss;
    uint64_t root_end = (L.root_last + 1) * (uint64_t)ss;
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

    stage_say("%s", "");
    stage_say("Writing the new layout.");
    if (commit_table(&t, &nw, commit_note, NULL, why, sizeof why) != 0)
        give_up(why, NULL, "commit-failed", 1);

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
    stage_say("Keeping a copy of the way back on this computer too.");
    rescue_payload rp;
    if (wr_arm(&t, WR_RECOVERY, L.rsc_first * (uint64_t)ss,
               (L.rsc_last + 1) * (uint64_t)ss, why, sizeof why) != 0 ||
        rescue_open(&rsc, &rp, why, sizeof why) != 0 ||
        rescue_mirror(&t, &rsc, &rp, L.rsc_first * (uint64_t)ss,
                      why, sizeof why) != 0)
        stage_warn("the copy of the way back could not be kept on this "
                   "computer (%s). The one on the memory stick is fine; "
                   "keep the stick.", why);
    else
        stage_say("saved    a second copy is on this computer");
    wr_disarm(&t, WR_RECOVERY);
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
        int di = -1;
        for (int k = 0; k < m->n_disks; k++) {
            const stage_disk *d = &m->disk[k];
            if (p.serial[0] && d->serial[0]) {
                if (!strcmp(p.serial, d->serial)) { di = k; break; }
            } else if (d->bytes == p.disk_bytes && !d->removable) {
                di = k;
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
