/* format.h — the bytes AurBridge and the staging environment agree on.
 *
 * Everything in this file exists twice: once here, in the program that
 * writes it on Windows, and once in src/aurstage, in the program that
 * reads it after the restart. They are in different languages'
 * toolchains, in different trees, written months apart, and the second
 * one refuses the install when they disagree. So each is written
 * against a SPECIFICATION rather than against the other's source, and
 * the specification is in the comments here and in aurstage.h.
 *
 * It is all pure functions over buffers, deliberately: it is the part
 * of AurBridge that can be tested on a machine that is not Windows,
 * and `aurbridge selftest` does exactly that.
 */
#ifndef AURBRIDGE_FORMAT_H
#define AURBRIDGE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* ── the two checksums ───────────────────────────────────────────── */

/* The IEEE CRC-32 the GPT specification uses. */
uint32_t fmt_crc32(const void *data, size_t n);

typedef struct {
    uint32_t h[8];
    uint64_t len;
    size_t   n;
    unsigned char buf[64];
} fmt_sha;

void fmt_sha_start(fmt_sha *s);
void fmt_sha_feed(fmt_sha *s, const void *data, size_t n);
void fmt_sha_done(fmt_sha *s, unsigned char out[32]);
void fmt_sha_hex(const unsigned char d[32], char *out, size_t n);

/* ── the partition table AurBridge writes on the memory stick ─────── */

/* The three type GUIDs, in GPT's mixed-endian order. They are the same
 * sixteen bytes src/aurstage/image.c, record.c and rescue.c look for,
 * and a stick is found by them and by nothing else -- never by the
 * removable flag, which is a SCSI bit that USB SSDs do not set. */
extern const uint8_t FMT_GUID_IMAGE[16];
extern const uint8_t FMT_GUID_RECORD[16];
extern const uint8_t FMT_GUID_SAVED[16];

typedef struct {
    const uint8_t *type;
    const char    *name;      /* ASCII; stored as UTF-16LE            */
    uint64_t       first, last;
} fmt_part;

/* Build a whole GPT for a disk of `disk_bytes` with `sector`-byte
 * blocks: the protective MBR, the primary header and array, and the
 * backup array and header. `out` must be at least
 * fmt_gpt_bytes(sector) and is laid out as the disk is, from LBA 0.
 * `tail` gets the backup region, which starts at fmt_gpt_backup_lba().
 *
 * Two buffers rather than one because the disk between them is the
 * whole stick, and building that in memory to write five kilobytes at
 * each end is not a thing to do on a machine with two gigabytes. */
size_t   fmt_gpt_head_bytes(uint32_t sector);
size_t   fmt_gpt_tail_bytes(uint32_t sector);
uint64_t fmt_gpt_backup_lba(uint64_t disk_bytes, uint32_t sector);
uint64_t fmt_gpt_first_usable(uint32_t sector);
uint64_t fmt_gpt_last_usable(uint64_t disk_bytes, uint32_t sector);

/* Returns 0, or -1 with a sentence in `why` when the partitions do not
 * fit, overlap, or fall outside the usable range. */
int fmt_gpt_build(uint64_t disk_bytes, uint32_t sector,
                  const uint8_t disk_guid[16],
                  const fmt_part *parts, int n_parts,
                  uint8_t *head, uint8_t *tail, char *why, size_t wn);

/* ── the manifest at the start of the image partition ────────────── */

#define FMT_MANIFEST_BYTES 4096

/* "AURIMG01", then the image's length, the root extent inside it, the
 * image's own logical block size, the SHA-256 of that extent, and the
 * profile id. src/aurstage/image.c reads exactly this. */
void fmt_manifest(uint8_t out[FMT_MANIFEST_BYTES],
                  uint64_t image_bytes, uint64_t root_off, uint64_t root_len,
                  uint32_t image_sector, const unsigned char root_sha[32],
                  const char *profile);

/* Where the root partition is inside a whole-disk AurOS image, read out
 * of the image's OWN GPT. This is what makes the manifest describe the
 * image beside it rather than what somebody typed. */
int fmt_image_root_extent(const uint8_t *gpt_head, size_t head_len,
                          uint32_t sector, uint64_t *off, uint64_t *len,
                          char *why, size_t wn);

/* ── the journal the staging environment reads after the restart ─── */

typedef struct {
    char     disk_serial[80];
    char     disk_model[64];
    uint64_t disk_bytes;
    uint32_t logical_sector;
    char     win_part[8];
    uint64_t win_start_lba;      /* in 512-byte units, as /sys reports */
    uint64_t win_sectors;
    uint64_t win_ntfs_serial;
    char     gpt_sha256[65];
    char     stage[16];
    char     boot_from[16];
    uint64_t run_id;
    uint64_t written_unix;
} fmt_journal;

/* Exactly the shape src/aurstage/journal.c's parser accepts. It is
 * strict on purpose, so this is written by hand rather than by a
 * general JSON writer: a field renamed here and not there is an
 * install that refuses every machine. */
size_t fmt_journal_json(const fmt_journal *j, char *out, size_t n);

/* ── getting it into the initramfs ───────────────────────────────── */

/* A newc cpio holding one file at `path`, and nothing else.
 * Returns the number of bytes written, or 0 if it did not fit. */
size_t fmt_cpio_one(const char *path, const void *data, size_t data_len,
                    uint8_t *out, size_t n);

/* Wrap a buffer in a gzip container using STORED deflate blocks.
 *
 * Not compression: a container. The kernel's initramfs unpacker
 * accepts several segments concatenated, and the safest way to append
 * one to an image that is already gzip is to append another gzip --
 * the alternative, a raw cpio, depends on the unpacker's alignment
 * rules holding at the end of a compressed segment, which is a
 * property of a kernel version rather than of a format. Stored blocks
 * need no compressor at all and are forty lines. */
size_t fmt_gzip_store(const void *data, size_t len, uint8_t *out, size_t n);

/* ── the hash of a machine's partition table ─────────────────────── */

/* Computed exactly as src/aurstage/aurstage.h defines it, which is
 * quoted here because the definition is the contract:
 *
 *   SHA-256 over, in order, the first HeaderSize bytes at byte offset
 *   S (LBA 1, the primary GPT header), HeaderSize being the
 *   little-endian uint32 at offset 12 of that header, and
 *   NumberOfPartitionEntries x SizeOfPartitionEntry bytes starting at
 *   byte offset PartitionEntryLBA x S -- where S is the disk's LOGICAL
 *   BLOCK SIZE, 4096 on a 4Kn drive.
 *
 * `read_at` is how this gets at the disk, so the same code serves the
 * real machine and the simulated one. Bounds are part of the
 * definition: HeaderSize in [92, S], SizeOfPartitionEntry in
 * [128, 4096], NumberOfPartitionEntries in [1, 4096], the array at
 * most 16 MiB. Outside them is "cannot be read", which is a refusal. */
int fmt_gpt_sha256(int (*read_at)(void *ud, uint64_t off, void *buf, size_t n),
                   void *ud, uint32_t sector, char *hex, size_t n);

/* Every pure function above, checked against vectors. Returns the
 * number that failed. */
int fmt_selftest(void);

#endif
