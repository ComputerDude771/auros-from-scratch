/* rescue.h — the way back.
 *
 * Rule 2 of docs/AURBRIDGE.md: "the machine can always go back". This
 * file is what makes that sentence true rather than aspirational. It
 * captures everything a machine needs to be the machine it was, before
 * the first destructive byte, and it puts it back.
 *
 * WHY THIS IS C AND NOT THE SHELL SCRIPT IT USED TO BE
 *
 * src/recovery/mkrecovery was a good shell tool and it is still in the
 * tree as an independent second opinion for the tests. It could not be
 * the product, for one reason that has nothing to do with taste:
 *
 *   A restore rewrites the partition table of the disk it is running
 *   from. Doing that from inside AurOS means rewriting the table under
 *   a mounted root, and growing the Windows filesystem back means
 *   growing a volume inside a partition whose entry has just changed
 *   under a kernel that has not re-read it. The only environment where
 *   the restore is safe is the one where nothing on that disk is
 *   mounted -- the staging initramfs. That environment has no shell.
 *
 * So "Put Windows back" in the AurOS settings panel does not restore
 * anything itself. It arms a flag and restarts into the staging
 * environment on the recovery partition, and this code runs there, in
 * exactly the same place, with exactly the same code paths, as the
 * install it is undoing.
 *
 * AND TWO THINGS THE SHELL VERSION GOT WRONG, both found by review
 * before either could ship, both recorded here next to the thing they
 * killed:
 *
 *   IT RESTORED A FIXED 2048 SECTORS FROM LBA 0. The first partition
 *   on a 4Kn disk starts at LBA 256 (1 MiB); on plenty of 512e OEM
 *   layouts it starts at 34, 40 or 63. Writing a megabyte -- eight
 *   megabytes on 4Kn -- of capture-time bytes from LBA 0 puts the
 *   start of somebody's ESP back to what it was at capture time at
 *   best, and writes table bytes over live filesystem data at worst.
 *   Nothing here restores a sector the captured header's own fields do
 *   not describe.
 *
 *   IT WAS NOT ONE SECTOR. "Write the primary head, then the backup"
 *   changes the machine's layout at the array write, not at LBA 1, for
 *   the reason commit.h sets out at length. The restore commits the
 *   same way the install does -- array, then LBA 1, then the backup --
 *   because a restore interrupted by a power cut is the single most
 *   likely power cut in the product's life: it is already running on a
 *   machine something has gone wrong with.
 *
 * WHAT IS CAPTURED, AND WHY EACH THING IS HERE
 *
 *   the protective MBR   one sector. Never restored unless it differs,
 *                        because stage C never writes it; captured
 *                        because a machine that boots nothing at all
 *                        is usually missing exactly this.
 *   the primary header   one sector, LBA 1.
 *   the primary array    at the header's OWN PartitionEntryLBA, for
 *                        NumberOfPartitionEntries x SizeOfPartitionEntry
 *                        bytes, rounded up to whole blocks. Not 32
 *                        sectors, not 2048.
 *   the backup header    at the header's OWN AlternateLBA.
 *   the backup array     at the BACKUP header's own PartitionEntryLBA,
 *                        which is not derivable from the primary's.
 *   the ESP, whole       OEM EFI partitions hold vendor boot files and
 *                        firmware capsules as well as Microsoft's
 *                        loader, and Windows does not start without
 *                        \EFI\Microsoft\Boot. Captured raw, by offset,
 *                        never mounted: a read-write mount is a write,
 *                        and mounting is unavailable in precisely the
 *                        case this exists for.
 *   per NTFS volume:     $Boot, and the copy of the boot sector NTFS
 *                        keeps in the volume's last sector -- the one
 *                        that MOVES when a volume is resized and the
 *                        one chkdsk falls back to -- and the sector
 *                        count the filesystem itself claimed. The GPT
 *                        records none of that, and the moment a shrink
 *                        succeeds the partition entry and the
 *                        filesystem inside it stop agreeing.
 *
 * Every section carries its own SHA-256 and the payload carries one
 * over all of them. A backup nobody verified is a rumour.
 *
 * WHERE IT LIVES: a raw partition on the recovery stick, for the same
 * three reasons record.h gives -- no filesystem to corrupt, no FIBMAP
 * to get wrong, one contiguous run by construction -- and then a
 * byte-identical copy in the AurOS recovery partition once there is
 * one, so that losing the stick is survivable and losing the disk is
 * survivable, but not both at once, which is the honest bound.
 */
#ifndef AUROS_RESCUE_H
#define AUROS_RESCUE_H

#include <stddef.h>
#include <stdint.h>
#include "aurstage.h"
#include "wr.h"

/* The type GUID AurBridge gives the raw rescue partition on the stick,
 * generated once for this product. Found by what it is, never by the
 * `removable` flag -- see image.h for why that flag is a trap. */
extern const uint8_t RESCUE_TYPE_GUID[16];

#define RESCUE_MAGIC      "AURRSC01"
#define RESCUE_HDR_BYTES  4096u
#define RESCUE_MAX_SEC    40
#define RESCUE_SEC_BYTES  96u
#define RESCUE_MAX_NTFS   8

/* THE HEADER, ON DISK. Written down because a second implementation --
 * a support tool, a future AurBridge, somebody's script five years from
 * now -- has to be able to read a stick without reading this file's
 * .c. Every number is little-endian; a "block" is the machine disk's
 * own logical block size, which is the `sector` field and which is
 * 4096 on a 4Kn drive.
 *
 *   0    magic, the eight bytes "AURRSC01"
 *   8    payload_bytes, header included
 *   16   disk_bytes, of the machine's disk
 *   24   sector, the machine disk's logical block size
 *   28   n_sections
 *   32   captured_unix
 *   40   run_id
 *   48   serial, 80 bytes, NUL-padded
 *   128  model, 64 bytes, NUL-padded
 *   192  esp_lba
 *   200  esp_blocks
 *   208  sha-256 of bytes [4096, payload_bytes)
 *   240  crc-32 of this whole 4096-byte header with these four
 *        bytes zero, the same polynomial GPT uses
 *   256  the section table: n_sections entries of 96 bytes
 *
 * and each section entry:
 *
 *   0  kind      4      8   disk_lba   8      24  off   8
 *   4  index     4      16  lba_count  8      32  len   8
 *   40 aux       8      48  aux2       8      56  sha-256, 32 bytes
 *
 * The header is written LAST, after everything it describes is on the
 * stick and flushed, so a capture interrupted by a power cut has no
 * magic at its start and is refused rather than half-believed. */

/* Section kinds. On-disk numbers: never renumber, only append. */
typedef enum {
    RS_MBR         = 1,
    RS_GPT_HDR     = 2,
    RS_GPT_ARR     = 3,
    RS_GPT_ALT_HDR = 4,
    RS_GPT_ALT_ARR = 5,
    RS_ESP         = 6,
    RS_NTFS_BOOT   = 7,   /* aux = the volume's own total_sectors      */
    RS_NTFS_BAKBOOT= 8,   /* aux2 = the partition's last LBA           */
} rescue_kind;

typedef struct {
    uint32_t kind;
    uint32_t index;        /* GPT partition number, 1-based, or 0      */
    uint64_t disk_lba;     /* where on the machine's disk it came from */
    uint64_t lba_count;
    uint64_t off;          /* where in the payload the bytes are       */
    uint64_t len;
    uint64_t aux, aux2;
    unsigned char sha[32];
} rescue_section;

typedef struct {
    uint64_t payload_bytes;       /* header included                   */
    uint64_t disk_bytes;
    uint32_t sector;
    uint32_t n_sections;
    uint64_t captured_unix;
    uint64_t run_id;
    char     serial[80];
    char     model[64];
    uint64_t esp_lba, esp_blocks;
    unsigned char body_sha[32];
    rescue_section sec[RESCUE_MAX_SEC];
} rescue_payload;

/* Where the payload lives. */
typedef struct {
    char     dev[72];      /* the DISK holding it, not a partition node*/
    uint64_t part_off;     /* byte offset of the rescue partition      */
    uint64_t part_bytes;
} rescue_area;

/* Find the rescue partition on any disk that is not `exclude_disk`.
 * Excluding the target is not politeness: a capture written onto the
 * disk being captured is gone at exactly the moment it is needed. */
int rescue_find(const stage_machine *m, const char *exclude_disk,
                rescue_area *out, char *why, size_t n);

/* Every rescue area on the machine, in disk order. The restore needs
 * this rather than the first match: there are normally two copies --
 * the stick's and the one on the machine's own disk -- and which of
 * them is the right one to read cannot be decided until each has been
 * opened and its recorded disk identified. Returns how many were
 * found, up to `max`. */
int rescue_find_n(const stage_machine *m, rescue_area *out, int max);

/* How many bytes a capture of this disk needs. Asked AT THE GATE, so
 * that "the rescue area on the stick is too small for this machine's
 * EFI partition" is a refusal with the disk untouched, instead of a
 * discovery made after the shrink. */
int rescue_size_needed(const stage_disk *d, uint64_t *need,
                       char *why, size_t n);

/* Capture, and read every byte of it back. Returns 0 only when the
 * payload on the stick verifies. Writes nothing to `d`. */
int rescue_capture(const stage_disk *d, const rescue_area *area,
                   uint64_t run_id,
                   void (*progress)(int percent), char *why, size_t n);

/* Read the header and section table out of an area. Checks the magic,
 * the header CRC and the geometry; does not read the body. */
int rescue_open(const rescue_area *area, rescue_payload *out,
                char *why, size_t n);

/* Read the whole body and check every section hash and the payload
 * hash. This is what makes the capture a backup rather than a rumour,
 * and it runs before the restore writes its first byte. */
int rescue_verify(const rescue_area *area, const rescue_payload *p,
                  void (*progress)(int percent), char *why, size_t n);

/* Copy the payload, byte for byte, into the AurOS recovery partition
 * at `dst_off`, then read it back and compare against the stick.
 * `t` must have WR_MIRROR armed over the destination -- its own
 * window, not the boot partition's, because they are two extents and
 * the sentence a refusal prints comes from the name. This line said
 * WR_RECOVERY while the caller armed WR_MIRROR, and between them the
 * second copy was written to no machine at all. */
int rescue_mirror(wr_target *t, const rescue_area *area,
                  const rescue_payload *p, uint64_t dst_off,
                  char *why, size_t n);

/* ── putting it back ─────────────────────────────────────────────── */

/* The restore talks, because it is the one operation a frightened
 * person watches every line of. */
typedef void (*rescue_say)(const char *line, void *ud);

typedef struct {
    int table_restored;
    int esp_restored;
    int volumes_grown;
    int volumes_left_small;   /* grew back short, or would not grow    */
    int mbr_restored;
} rescue_outcome;

/* Put the captured boot state back on `disk_dev`.
 *
 * Order, and every step of it is idempotent so that the whole thing
 * can simply be run again:
 *
 *   1. the partition table, committed in one sector
 *   2. the EFI partition, whole
 *   3. each NTFS volume grown back to the size it claimed
 *
 * The grow is last because it is the only step that is both long and
 * documented-restartable, and because a machine interrupted after step
 * 2 boots Windows with a smaller C: -- which is a machine somebody can
 * use, back up from, and finish the restore on -- while a machine
 * interrupted between 1 and 2 boots nothing at all. That window is
 * unavoidable: the restore's whole job is to delete the partition
 * AurOS is on, and there is no ordering in which both systems are
 * bootable throughout.
 *
 * Returns 0 if Windows should now start. */
int rescue_restore(const char *disk_dev, const rescue_area *area,
                   rescue_say say, void *ud, rescue_outcome *out,
                   char *why, size_t n);

#endif
