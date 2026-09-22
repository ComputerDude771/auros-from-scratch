/* nvram.h — the only place in this directory that writes an EFI variable.
 *
 * WHY THIS IS A SECOND GATE AND NOT A HOLE IN THE FIRST
 *
 * wr.h opens with "the only place in this directory that writes to a
 * disk", and build/staging enforces it by refusing to build an image
 * in which any other file opens something writably. Writing a boot
 * entry needs O_WRONLY|O_CREAT on a file under
 * /sys/firmware/efi/efivars, so the honest choices were to weaken that
 * gate with a pattern, to move NVRAM into wr.c, or to add a second
 * named gate.
 *
 * A pattern exception is an exception nobody reviewed -- build/staging
 * says exactly that about its own two -- and NVRAM is not a disk, so
 * folding it into the file whose entire value is "all disk writes are
 * here" would make that sentence false. So: a second file, named in
 * the build the way wr.c is, with its own rule. It may write nothing
 * but EFI variables, it may write them nowhere but under the efivarfs
 * mount point, and the build checks both.
 *
 * NVRAM DESERVES A GATE MORE THAN THE DISK DOES, not less. A wrong
 * byte on a disk costs a partition. A wrong Boot#### costs a machine
 * that will not start at all, with no error message, on hardware whose
 * firmware setup screen the owner has never seen -- and plat_win.c
 * already carries the scar: a 16-byte probe buffer declared Boot0000
 * free on every UEFI machine on earth and overwrote the Windows Boot
 * Manager entry.
 *
 * WHAT THIS FILE WILL NOT DO
 *
 * It will not touch BootOrder. docs/AURBRIDGE.md: "BootOrder is only
 * rewritten in phase 10, after the user confirms." Until then Windows
 * stays this machine's default, and the only thing pointing at AurOS
 * is BootNext -- one-shot, cleared by the firmware as it is used, so a
 * machine that cannot start AurOS comes back to Windows by itself with
 * nobody doing anything. There is deliberately no function here that
 * writes BootOrder, because the way that rule gets broken is somebody
 * finding a function that already does it.
 */
#ifndef AUROS_NVRAM_H
#define AUROS_NVRAM_H

#include <stddef.h>
#include <stdint.h>

/* The partition a boot entry points into, as the short-form Hard Drive
 * device path node needs it. Every field comes from the partition
 * table this installer has just written, never from /sys: a UEFI LBA
 * is the disk's logical block and /sys reports 512-byte units on every
 * disk including a 4Kn one. */
typedef struct {
    uint32_t number;        /* 1-based partition number                 */
    uint64_t first_lba;     /* in the disk's LOGICAL blocks             */
    uint64_t blocks;
    uint8_t  guid[16];      /* the unique partition GUID, as on disk    */
} nvram_hd;

/* Is there an efivarfs to write to at all? 0 means this machine is
 * BIOS-only or the kernel has no efivarfs, which is a refusal
 * somewhere above, not a thing to work around here. */
int nvram_present(void);

/* Build an EFI_LOAD_OPTION. Exported so the test can compare it byte
 * for byte against the one AurBridge builds on the Windows side: two
 * implementations of one wire format in two languages is exactly the
 * pair that drifts, and the symptom of drift is a machine that boots
 * nothing. Returns the length, or 0 if it would not fit. */
size_t nvram_load_option(uint8_t *out, size_t n, const char *desc,
                         const nvram_hd *on, const char *loader,
                         const char *cmdline);

/* Create or replace the persistent boot entry whose description is
 * `desc`.
 *
 * REPLACE, BY DESCRIPTION. An entry we wrote on a previous attempt is
 * ours to reuse; anything else is not, and a free slot is one with no
 * variable in it rather than one we do not recognise -- "the entries
 * we did not write" also selects the Fedora somebody installed last
 * month and the vendor diagnostics entry that was never in BootOrder.
 *
 * Does NOT touch BootOrder. See the header note. */
int nvram_boot_set(const char *desc, const nvram_hd *on, const char *loader,
                   const char *cmdline, uint16_t *num_out,
                   char *why, size_t n);

/* Delete every entry whose description is exactly `desc`. Returns how
 * many went, or -1 -- NEVER a partial count, because a caller testing
 * `rc < 0` would read one as success. */

/* Arm the one-shot. */
int nvram_boot_next(uint16_t num, char *why, size_t n);

/* Used for one thing: the "AurOS Installer" entry the Windows half
 * created, once the install it existed for is finished. Never matches
 * a prefix -- "AurOS Installer" and "AurOS" are different entries and
 * one of them is the one that boots. */
int nvram_boot_forget(const char *desc, char *why, size_t n);

#endif
