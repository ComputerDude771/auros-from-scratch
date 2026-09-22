/* image.h — finding the AurOS image, and putting it on the disk.
 *
 * WHAT THE BUILD PUBLISHES, AND WHY IT IS NOT WHAT GETS WRITTEN
 *
 * build/mkimage emits a whole-disk GPT image: protective MBR, an ESP,
 * and a root partition. docs/AURBRIDGE.md has flagged for some time
 * that writing THAT into a partition embeds a nested GPT and boots
 * nothing, and offers two ways out. This is the second one: the
 * staging environment translates it, and only the root partition's
 * byte extent goes into the new root partition.
 *
 * That keeps one artifact. The thing the build boots and tests in
 * QEMU is the thing the installer copies, which is mkimage's own
 * stated reason for existing -- "a verified copy rather than an
 * unverified assembly on a stranger's machine". Publishing a bare
 * root filesystem instead would make the tested object and the
 * shipped object two different things.
 *
 * WHERE IT LIVES: A RAW PARTITION ON THE RECOVERY STICK
 *
 * The desktop image is over five gigabytes. FAT32 cannot hold a file
 * of 4 GiB or more, so the stick's FAT partition is out; exFAT means
 * carrying a driver in an initramfs budgeted at about 80 MB, for one
 * file; splitting it into chunks means the published artifact is
 * something you must reassemble before you can check its hash. A raw
 * partition has none of those problems and is read with one pread.
 *
 * It cannot live on C:. This environment never mounts NTFS -- there is
 * no ntfs3 in the image and boot.c says why -- and a hand-written NTFS
 * reader streaming five gigabytes is not something to put on the path
 * that decides what lands in the root partition, on a volume we have
 * just resized.
 *
 * HOW THE STICK IS FOUND: BY WHAT IS ON IT
 *
 * NOT by /sys's `removable` flag. That is the SCSI RMB bit: thumb
 * drives usually set it, but USB SSDs and anything in a USB-to-SATA
 * enclosure do not -- and a five-gigabyte payload is exactly what a
 * person puts on the big fast drive they already own. An adversarial
 * review found that the refusal this produced told the user to plug in
 * a stick that was already plugged in.
 *
 * So: every disk's partition table is scanned for a partition whose
 * type GUID is ours, and the manifest on it must name the profile the
 * journal asked for and a hash that matches.
 */
#ifndef AUROS_IMAGE_H
#define AUROS_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include "aurstage.h"
#include "wr.h"

/* The type GUID AurBridge gives the raw image partition on the stick.
 * Generated once, for this product, so that finding it is an answer
 * and not a guess. */
extern const uint8_t IMAGE_TYPE_GUID[16];

typedef struct {
    /* THE WHOLE DISK, AND A BYTE OFFSET -- not a partition device.
     *
     * Partition nodes are made by the kernel and published by
     * devtmpfs, and this environment comes up before any of the usual
     * machinery that waits for them. Reading the whole disk at an
     * offset needs none of that, works identically on a machine whose
     * loop driver was built with max_part=0 (which is what the test
     * container has), and removes a whole class of "the file is not
     * there yet" from the destructive path. */
    char     dev[72];           /* the DISK holding it                */
    uint64_t part_off;          /* where its partition starts, bytes  */
    char     profile[64];
    uint64_t image_bytes;
    uint64_t root_off;          /* the root extent, INSIDE the image  */
    uint64_t root_len;
    uint32_t image_sector;      /* the image's own logical block size */
    unsigned char root_sha[32]; /* of the root extent, from the build */
    int      have_sha;
    /* THE PART THAT STARTS THE COMPUTER, and its own hash.
     *
     * This was missing, and the gap was the whole point of the file:
     * loader.c copies this extent onto the machine and reads it back
     * AGAINST THE STICK -- the same bytes it just wrote -- so a
     * bit-flip in the shim or in grub was copied faithfully, verified
     * faithfully, and reported as a successful install. The machine
     * then failed to start AurOS with no message, or a Secure Boot
     * violation. Nothing anywhere had a number to compare it against.
     *
     * Zero on a stick written by an older build, which is why every
     * use is guarded rather than assumed. */
    uint64_t esp_off;
    uint64_t esp_len;
    unsigned char esp_sha[32];
    int      have_esp_sha;
} image_src;

/* Find it. Scans every disk's table for our type GUID, reads the
 * manifest that must sit at the start of the partition, and
 * cross-checks the offsets against the image's OWN GPT.
 *
 * The cross-check is not redundancy for its own sake: it is the only
 * thing that catches OUR OWN build publishing a manifest that does
 * not describe the image beside it, a bug whose consequence is
 * writing the image's ESP bytes into the root partition and
 * discovering it after the irreversible shrink. */
int image_find(const stage_machine *m, const char *want_profile,
               image_src *out, char *why, size_t n);

/* Hash the root extent where it lies and compare against the build's.
 * Done BEFORE the shrink, always: every question this answers is
 * answerable with the disk untouched, and discovering the stick is
 * corrupt afterwards turns a free refusal into an expensive one. */
int image_verify(const image_src *s, void (*progress)(int percent),
                 char *why, size_t n);

/* Write the root extent to `dst_off` on the target, then read it back.
 *
 * THE FIRST MEGABYTE IS WRITTEN LAST. Borrowed from eos-installer and
 * worth keeping: zero it, write everything else, verify, and only then
 * lay it down. A partially written install is then never a
 * bootable-looking install -- it has no superblock, so nothing will
 * mount it, and nothing will mistake it for a filesystem that merely
 * has something wrong with it. */
int image_write_root(wr_target *t, const image_src *s, uint64_t dst_off,
                     void (*progress)(int percent), char *why, size_t n);

/* Where the image's OWN EFI System partition lies, as a byte offset
 * and length INSIDE the image -- the same frame `root_off` uses.
 *
 * The installer copies that partition WHOLE onto the machine, into a
 * partition of its own, and loader.h says at length why it is copied
 * rather than assembled. All this function does is find it, out of
 * the image's own table, for the same reason the root extent is taken
 * from there: the manifest describes one partition and the thing that
 * has to be copied is another, and inventing an offset for it in the
 * manifest would mean a wire format change across both halves of the
 * product to carry a number the image already states. */
/* Where the EFI partition is. The manifest's numbers when it has
 * them, cross-checked against the image's own table -- the same
 * arrangement the root extent has, and for the same reason: the
 * manifest is what the build says and the table is what the image is,
 * and it is our own build publishing a disagreement that this
 * catches. */
int image_esp_extent(const image_src *s, uint64_t *off, uint64_t *len,
                     char *why, size_t n);

/* Where the image itself starts on the stick. Every offset "inside the
 * image" -- root_off, and what image_esp_extent returns -- is measured
 * from here, and the manifest sits in front of it. */
uint64_t image_base_off(const image_src *s);

#endif
