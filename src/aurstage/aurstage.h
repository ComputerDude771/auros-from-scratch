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
 * READ-ONLY IS CHECKED HERE, NOT REMEMBERED
 *
 * Every block device this directory opens, it opens O_RDONLY. That
 * used to be true because disks.c was the only file that opened one;
 * it is not any more -- ntfs.c, fde.c and shrink.c read devices too,
 * which is what stage B is -- so the property is enforced instead of
 * arranged. build/staging greps for a writable open anywhere in this
 * directory and refuses to build the image if it finds one.
 *
 * A stage that must not write is not made safe by everyone
 * remembering. It is made safe by something that fails loudly when
 * somebody forgets.
 */
#ifndef AUROS_AURSTAGE_H
#define AUROS_AURSTAGE_H

#include <stddef.h>
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
    int      is_esp;                /* GPT type GUID says EFI System   */
    int      is_gpt;                /* the disk it is on has a GPT     */
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
    /* 0 if the kernel would not say. docs/AURBRIDGE.md: "Always read
     * StorageAccessAlignmentProperty, and block if it cannot be read."
     * Defaulting to 512 is how a 4Kn disk gets a partition eight times
     * too small, so the unknown is carried rather than papered over. */
    int         sector_known;
    int         removable;
    int         gpt;                /* a valid primary GPT was found   */
    char        serial[80];         /* "" if the disk will not say     */
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

/* Say what storage hardware this machine actually has, when none of
 * it produced a disk. Not a diagnosis and deliberately not a BIOS
 * instruction -- see the comment on the definition. */
void stage_report_controllers(void);

/* The hash of this disk's partition table, as 64 lowercase hex
 * characters. `hex` needs 65. Returns 0 if it could be computed.
 *
 * WHAT EXACTLY IS HASHED, because AurBridge has to compute the same
 * thing on the other side of a restart, in another language, and "a
 * hash of the GPT" is not a specification:
 *
 *   Let S be the disk's LOGICAL BLOCK SIZE in bytes, as the disk
 *   reports it -- 512 on an ordinary drive, 4096 on a 4Kn one. An LBA
 *   below means a multiple of S, per UEFI.
 *
 *   SHA-256 over, in order,
 *     1. the first HeaderSize bytes at byte offset S (LBA 1, the
 *        primary GPT header), HeaderSize being the little-endian
 *        uint32 at offset 12 of that header, and
 *     2. NumberOfPartitionEntries x SizeOfPartitionEntry bytes
 *        starting at byte offset PartitionEntryLBA x S.
 *
 * S IS SPELLED OUT BECAUSE IT IS THE WHOLE TRAP. Linux reports a
 * partition's start and size in /sys in 512-byte units on every disk
 * including a 4Kn one, so "LBA" means one thing three lines above
 * this in stage_part and another thing here. An implementation that
 * assumed 512 would hash the wrong bytes on every 4Kn machine and
 * refuse all of them with "the way this disk is divided up has
 * changed", on disks nobody had touched.
 *
 * Not the whole of LBA 1, because the bytes past HeaderSize are
 * padding no firmware promises anything about. Not the backup table,
 * which is a separate question.
 *
 * THE BOUNDS ARE PART OF THE DEFINITION, not an implementation
 * detail: a table outside them is answered "cannot be read", which is
 * a refusal, so a second implementation that accepts more than this
 * disagrees about which machines are usable. HeaderSize in [92, S];
 * SizeOfPartitionEntry in [128, 4096]; NumberOfPartitionEntries in
 * [1, 4096]; the array at most 16 MiB and wholly inside the disk. */
int  stage_gpt_sha256(const stage_disk *d, char *hex, size_t n);

/* Hand over to the installed system, in this same boot. `root_dev` is
 * a block device path. Never returns on success. */
int  stage_switch_root(const char *root_dev);

/* What the kernel was told to do with us, out of /proc/cmdline.
 * Shared, because the dry run and the real run must agree about every
 * one of these -- a flag that changes the dry run's answer and not
 * the install's makes the hardware matrix a lie. */
int  stage_cmdline_has(const char *word);
int  stage_cmdline_value(const char *key, char *out, size_t n);

/* ── saying things ───────────────────────────────────────────────── */

/* Everything this environment says goes through here: to the console
 * and, once there is somewhere to put it, to a log that survives into
 * the installed system. A staging environment that fails silently is
 * a machine that has to be posted back. */
void stage_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void stage_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
