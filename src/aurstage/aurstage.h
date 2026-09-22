/* aurstage.h — the staging environment.
 *
 * WHAT THIS IS
 *
 * The one restart in this product lands here, not in Windows and not
 * in the installed system. This is the AurOS initramfs, running as PID
 * 1, in the same boot that will end inside AurOS. It is where every
 * destructive step happens, because it is the only place on the
 * machine where recovery code can run while the disk is being
 * rearranged. docs/AURBRIDGE.md has the reasoning; the short form is
 * that Windows' online shrink cannot move pagefile.sys, so shrinking
 * from inside Windows costs a second restart -- the one thing the
 * whole design is spending its budget to avoid.
 *
 * WHAT IT IS NOT
 *
 * It is not systemd and it will not become systemd. A generator, a
 * unit with an Install section, or an automount that mounts the
 * Windows volume behind our back is the single thing this environment
 * must never do: ntfsresize refuses a mounted volume, and a volume
 * mounted read-write for even a moment is a volume whose journal we
 * have changed. Nothing here mounts anything it was not told to.
 *
 * THE STAGES, and this file only implements the first
 *
 *   A  it exists, it boots, it writes NOTHING, and it hands over to
 *      the installed system in the same boot.          <- this file
 *   B  read-only verification: the journal AurBridge left, the NTFS
 *      state, `ntfsresize --no-action`, a surface test. Then a
 *      --dry-run that reboots back to Windows having changed nothing.
 *      docs/AURBRIDGE.md calls that the first shippable artifact.
 *   C  the destructive steps, one at a time, each with its own
 *      kill-the-power test.
 *   D  the way back: recovery partition, "put Windows back".
 *
 * "Nothing from a later stage before an earlier one" is the build
 * order that document sets, and the reason is that every stage after
 * this one is judged by whether an abort still leaves a machine that
 * boots Windows. That cannot be judged until the thing doing the
 * aborting is known to work.
 *
 * READ-ONLY IS STRUCTURAL HERE, NOT A HABIT
 *
 * Every block device this file opens, it opens O_RDONLY, through one
 * function, which is the only place in the program that opens one. A
 * stage that must not write is not made safe by everyone remembering;
 * it is made safe by there being nothing to remember with.
 */
#ifndef AUROS_AURSTAGE_H
#define AUROS_AURSTAGE_H

#include <stdint.h>

/* ── what the machine looks like ─────────────────────────────────── */

#define STAGE_MAX_DISK   8
#define STAGE_MAX_PART   32
#define STAGE_NAME       32

typedef struct {
    char     name[STAGE_NAME];      /* "sda2", "nvme0n1p3"            */
    uint64_t start_lba, sectors;    /* as the kernel reports it       */
    uint64_t bytes;
    char     fstype[16];            /* "ntfs", "ext4", "vfat", ""     */
    char     label[40];
    char     uuid[40];
    int      is_esp;
} stage_part;

typedef struct {
    char        name[STAGE_NAME];   /* "sda", "nvme0n1"               */
    char        model[48];
    uint64_t    bytes;
    int         logical_sector;     /* 512 or 4096 -- see the note in
                                     * disks.c: shrink takes sectors,
                                     * partition tables take bytes, and
                                     * assuming 512 on a 4Kn disk makes
                                     * the partition eight times too
                                     * small. */
    int         physical_sector;
    int         removable;
    int         n_parts;
    stage_part  part[STAGE_MAX_PART];
} stage_disk;

typedef struct {
    int         n_disks;
    stage_disk  disk[STAGE_MAX_DISK];
} stage_machine;

/* Look at every block device the kernel knows about. Opens nothing
 * writable and mounts nothing. Returns the number of disks found. */
int  stage_survey(stage_machine *m);
/* Print it the way a support engineer would want to read it. */
void stage_report(const stage_machine *m);

/* ── the pieces of coming up ─────────────────────────────────────── */

/* /proc, /sys, /dev, /run. Returns 0 if all of them are there. */
int  stage_mount_pseudo(void);
/* Ask the kernel to load a driver for every device it has told us
 * about, the way udev would, without udev. Returns how many it
 * loaded. */
int  stage_load_modules(void);
/* Wait until at least one whole disk with at least one partition has
 * appeared, or the deadline passes. Returns 1 if something turned up. */
int  stage_wait_for_disks(int timeout_ms);

/* Hand over to the installed system, in this same boot. `root_dev` is
 * a block device path. Never returns on success. */
int  stage_switch_root(const char *root_dev);

/* ── saying things ───────────────────────────────────────────────── */

/* Everything this environment says goes through here: to the console
 * and, once there is somewhere to put it, to a log that survives into
 * the installed system. A staging environment that fails silently is
 * a machine that has to be posted back. */
void stage_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void stage_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
