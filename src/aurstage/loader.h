/* loader.h — phase 8b: making the machine able to start AurOS by itself.
 *
 * WHAT WAS MISSING, SAID PLAINLY
 *
 * Up to this file the installer shrank Windows, wrote AurOS into a new
 * partition, verified it, committed the table and handed over to it in
 * the same boot -- and nothing anywhere wrote a bootloader. The one
 * restart ended inside a working AurOS, and the next time the machine
 * was switched off it lost it: no boot entry pointed at the new
 * partition and no EFI binary existed to point at. Everything else in
 * this directory was correct and the product did not work.
 *
 * WHERE THE BOOTLOADER GOES: OUR OWN PARTITION, NEVER THEIRS
 *
 * The obvious place is the machine's EFI System partition, and it is
 * the wrong one. R12: "OEM EFI partitions hold vendor boot files and
 * firmware capsules as well as \EFI\Microsoft\Boot. Never mounted -- a
 * read-write mount is a write, and mounting is unavailable in
 * precisely the case this exists for." Mounting it read-write would
 * put the kernel's vfat driver in charge of the one partition whose
 * loss means Windows never starts again, on a machine we have
 * promised can always go back. Writing FAT structures into it by hand,
 * through wr.c, means understanding somebody else's filesystem well
 * enough to extend it -- which is a far bigger thing to be wrong about
 * than anything else in this program.
 *
 * So AurOS gets a partition of its own. plan.c was already carving one
 * out of the gap -- `rec_first`/`rec_last`, typed as an EFI System
 * partition, and until now written by nothing. UEFI boots whatever a
 * Boot#### entry points at; it does not care which EFI System
 * partition that is, and the entry names ours by its own partition
 * GUID. The machine's ESP is not opened, not mounted, and not written,
 * in this file or anywhere near it.
 *
 * WHAT GOES IN IT: THE IMAGE'S OWN ESP, COPIED WHOLE
 *
 * Not assembled. build/mkimage already builds a complete signed boot
 * chain inside the image -- Canonical's dual-signed shim, their signed
 * grub, the BOOTX64.CSV that lets shim's fallback create a real NVRAM
 * entry, and grub.cfg in the three directories that need it -- and
 * that ESP is the one QEMU boots in the build. Copying its bytes means
 * the thing that was tested is the thing on the machine, which is
 * image.h's own argument for shipping a whole-disk image rather than a
 * bare root filesystem. Assembling a second ESP here, in C, at the
 * bottom of a destructive install, would mean the artifact the build
 * proves bootable and the artifact the user gets are two different
 * objects assembled by two different programs.
 *
 * It also means this file writes no FAT. There is no directory entry
 * code here, no cluster allocator, no long-name encoder and no place
 * for any of them to be subtly wrong on a machine nobody can reach.
 *
 * BEFORE THE COMMIT, NOT AFTER
 *
 * The copy goes down while the OLD partition table is still in force,
 * into the gap the shrink made, exactly like the root filesystem. Rule
 * 4 stays true -- the layout still changes at one sector write -- and
 * a machine that loses power here is the "5-7 write and verify" row of
 * the power-loss table: NTFS smaller, old table in force, garbage in
 * free space, Windows still the default. The boot entry is written
 * afterwards, because the partition GUID it names does not exist until
 * the table does.
 *
 * WHAT IT DOES NOT DO: MAKE AUROS THE DEFAULT
 *
 * BootOrder is untouched. Switching this machine on still reaches
 * Windows until the user has seen AurOS work and said so -- rule 3,
 * and phase 10's job, not this one's. What this file arms is BootNext,
 * which the firmware clears as it uses it: the next start reaches
 * AurOS once, and a machine that cannot start AurOS comes back to
 * Windows by itself with nobody doing anything.
 */
#ifndef AUROS_LOADER_H
#define AUROS_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include "gpt.h"
#include "image.h"
#include "plan.h"
#include "wr.h"

/* The description of the entry this installer writes, and of the one
 * the Windows half wrote to get here. They are different entries and
 * both of them are ours; the second is deleted once the first exists,
 * so that a person looking at their firmware's boot menu a year from
 * now does not find "AurOS Installer" sitting above "AurOS". */
#define LOADER_ENTRY_DESC   "AurOS"
#define LOADER_INSTALLER_DESC "AurOS Installer"

/* The loader shim launches, relative to the partition. Canonical's
 * signed grub carries the prefix /EFI/ubuntu baked into the signature,
 * which is why build/mkimage puts a grub.cfg there as well as in ours
 * -- changing it would mean re-signing a binary only Microsoft can
 * sign. */
#define LOADER_PATH         "\\EFI\\AurOS\\shimx64.efi"

/* How much room the boot partition needs, given the image that will be
 * copied into it. Called BEFORE the plan is computed, so that a
 * machine which cannot fit it is refused with the disk untouched
 * rather than discovered after the shrink. */
int loader_bytes_needed(const image_src *img, uint64_t *need,
                        char *why, size_t n);

/* Copy the image's ESP into the planned boot partition and read every
 * byte back. `t` must have nothing armed over that extent; this arms
 * WR_RECOVERY itself and disarms it before returning, success or not.
 *
 * Runs BEFORE the commit. A failure here is a give_up with the disk
 * still readable as it was: Windows is smaller and still starts. */
int loader_write_boot(wr_target *t, const image_src *img,
                      const stage_layout *L, uint32_t sector,
                      void (*progress)(int percent), char *why, size_t n);

/* Put AurOS in the firmware's menu and arm the one-shot.
 *
 * Returns 0 when both happened, 1 when the ENTRY landed and only the
 * one-shot did not -- a different thing to tell somebody, because the
 * machine's menu now has AurOS in it -- and -1 when neither did.
 *
 * Runs AFTER the commit, because the boot entry names the partition by
 * the unique GUID the commit has just given it. `nw` is the table that
 * was written, not the one that was read.
 *
 * A failure here is a WARNING, not a give_up: AurOS is installed and
 * verified by this point, the machine still starts Windows, and the
 * removable-media fallback -- /EFI/BOOT/BOOTX64.EFI and the
 * BOOTX64.CSV beside it, both copied from the image -- lets shim
 * create the entry itself on firmware that looks there. */
int loader_register(const gpt_table *nw, const stage_layout *L,
                    uint16_t *entry_out, char *why, size_t n);

#endif
