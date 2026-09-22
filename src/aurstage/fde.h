/* fde.h — is somebody else's encryption in the way?
 *
 * docs/AURBRIDGE.md: "Third-party full-disk encryption (VeraCrypt
 * system encryption, Sophos, Trellix, Symantec) -> abort
 * unconditionally. There is no safe shrink underneath a sector-level
 * encryption filter we do not control."
 *
 * BitLocker is handled elsewhere, in ntfs.c, because it announces
 * itself in the volume's first sector and because getting it wrong is
 * the single most destructive mistake in this codebase. Everything
 * else hides better: a sector-level filter driver makes the disk look
 * ordinary to Windows and makes it look like noise to us.
 *
 * So there are two nets here, and the second one is the one that
 * actually catches things:
 *
 *   1. The names, where the product wrote them -- in the boot code at
 *      the front of the disk, or in the ESP for a UEFI machine.
 *   2. A volume that should be a filesystem and is not, and whose
 *      first sector is statistically random. That is what ciphertext
 *      looks like, and it does not depend on knowing whose ciphertext
 *      it is.
 *
 * Net 2 is deliberately reported as "we do not recognise this" rather
 * than "this is encrypted". We cannot tell an unknown encryption
 * product from a corrupt volume, and both are a refusal, so there is
 * nothing to gain by guessing which.
 */
#ifndef AUROS_FDE_H
#define AUROS_FDE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    FDE_NONE = 0,       /* we looked, and nothing is in the way      */
    FDE_NAMED,          /* a product we recognise by name            */
    /* WE COULD NOT LOOK. Not the same as FDE_NONE, and they used to
     * be: every error path -- the device would not open, a read
     * failed halfway -- returned "nothing in the way", so a check the
     * doc calls an unconditional abort failed open. "I did not look"
     * and "I looked and it was clean" must not be one value in a
     * function whose whole job is to stop a shrink. */
    FDE_UNSURE
} fde_verdict;

/* Look for encryption software at the front of a whole disk and in
 * its EFI partition. `found` gets the product's name when there is
 * one. Opens read-only; reads at most a few hundred megabytes. */
fde_verdict fde_scan_disk(const char *disk_dev, const char *esp_dev,
                          char *found, size_t n);

/* Does this sector look like ciphertext rather than the start of a
 * filesystem? Used on a partition that should have had one. */
int fde_looks_random(const unsigned char *sector, size_t n);

#endif
