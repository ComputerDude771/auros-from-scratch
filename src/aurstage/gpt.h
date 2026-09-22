/* gpt.h — the partition table, read and rewritten.
 *
 * THE SINGLE MOST DANGEROUS WRITE IN THE PRODUCT lives behind this
 * header, so it is worth saying what it is: one sector, LBA 1, written
 * after everything else is already on the disk and flushed. Rule 4 of
 * docs/AURBRIDGE.md -- "one atomic commit point per destructive phase"
 * -- is that sector and nothing else.
 *
 * WHY THIS FILE EXISTS AT ALL. The first design for stage C described
 * the commit as "write the backup, then the primary" and named no
 * file, no function, no CRC32 and no way of generating the new
 * partitions' unique identifiers. An adversarial review pointed out
 * that an implementer would then invent the most dangerous code in the
 * tree over an afternoon, from memory. This is that code, written
 * deliberately.
 *
 * THREE THINGS IT DOES NOT ASSUME
 *
 *   HeaderSize is 92. It is read from the header on the disk. An OEM
 *   that wrote 96 gets a primary header whose CRC covers four bytes
 *   too few; firmware that validates strictly then falls back to the
 *   backup, and the machine boots a layout we did not intend.
 *
 *   An LBA is 512 bytes. It is the disk's logical block size, which is
 *   4096 on a 4Kn drive -- while /sys reports partition starts in
 *   512-byte units whatever the drive is. Those two conventions live
 *   three lines apart in this codebase and confusing them is the 8x
 *   error the risk register names.
 *
 *   There is room for another partition. There may not be, and growing
 *   the entry array would move FirstUsableLBA on top of somebody's
 *   partition. A full table is a refusal.
 */
#ifndef AUROS_GPT_H
#define AUROS_GPT_H

#include <stddef.h>
#include <stdint.h>

/* Every real disk has 128. The specification allows more, and a table
 * bigger than this is refused rather than partly read: a partition we
 * cannot see is a partition we could write over. */
#define GPT_MAX_ENT   256
#define GPT_NAME_CH   36

typedef struct {
    uint8_t  type[16];        /* type GUID, mixed-endian as on disk   */
    uint8_t  uuid[16];        /* unique partition GUID                */
    uint64_t first, last;     /* inclusive, in LOGICAL BLOCKS         */
    uint64_t attrs;
    uint16_t name[GPT_NAME_CH];   /* UTF-16LE, not terminated         */
} gpt_entry;

typedef struct {
    int      valid;
    int      from_backup;     /* the primary was bad and we used the
                               * backup -- worth saying out loud      */
    uint32_t sector;          /* logical block size, bytes            */
    uint32_t header_size;     /* AS FOUND. See the header comment.    */
    uint32_t revision;
    uint64_t my_lba, alt_lba;
    uint64_t first_usable, last_usable;
    uint64_t entry_lba;
    uint32_t n_entries, entry_size;
    uint8_t  disk_guid[16];
    uint64_t disk_sectors;    /* the whole device, in logical blocks  */
    gpt_entry ent[GPT_MAX_ENT];
} gpt_table;

/* The type GUIDs this product cares about, already in the mixed-endian
 * order GPT stores them. */
extern const uint8_t GPT_TYPE_ESP[16];        /* EFI System           */
extern const uint8_t GPT_TYPE_MSDATA[16];     /* Microsoft basic data */
extern const uint8_t GPT_TYPE_LINUX_ROOT[16]; /* Linux root (x86-64)  */
extern const uint8_t GPT_TYPE_LINUX[16];      /* Linux filesystem     */

/* Is this entry slot in use? An all-zero type GUID means free. */
int  gpt_used(const gpt_entry *e);

/* Read the table off a device or an image file. `sector` is the
 * logical block size. Validates both CRCs; falls back to the backup
 * when the primary is unreadable or fails, and says so in
 * `from_backup`. Returns 0 on success. */
int  gpt_read(int fd, uint32_t sector, uint64_t disk_bytes, gpt_table *t);

/* The index of the entry whose `first` is exactly this block, or -1.
 * By start, never by position: "the third one" is how a machine with a
 * partition the kernel did not instantiate gets the wrong answer. */
int  gpt_find_start(const gpt_table *t, uint64_t first_lba);

/* Where the gap after `idx` ends: the start of the next populated
 * entry at or after `after_lba`, or last_usable + 1 when there is
 * none.
 *
 * THIS IS THE FUNCTION THE FIRST DESIGN DID NOT HAVE, and its absence
 * was the worst mistake in it: the layout was computed from the end of
 * the disk, and on the OEM layout docs/AURBRIDGE.md calls typical the
 * space a shrink creates is a gap between C: and the recovery
 * partition, not space at the end. */
uint64_t gpt_gap_end(const gpt_table *t, uint64_t after_lba);

/* Set an existing entry's last block. Refuses to grow it, and refuses
 * to shrink it below its start. */
int  gpt_resize_entry(gpt_table *t, int idx, uint64_t new_last);

/* Append a partition. Refuses when the table is full, when the extent
 * overlaps anything, or when it falls outside the usable range.
 * `uuid_out` gets the generated PARTUUID. `why` gets one sentence. */
int  gpt_add(gpt_table *t, const uint8_t type[16], const char *name_ascii,
             uint64_t first, uint64_t last, uint8_t uuid_out[16],
             char *why, size_t n);

/* Does anything in the table overlap [first,last], other than the
 * entry at `except` (-1 for none)? Returns the offending index or -1.
 *
 * Checked against the TABLE, never against the /sys survey: that list
 * is readdir() order, capped, and holds only partitions the kernel
 * chose to instantiate. */
int  gpt_overlaps(const gpt_table *t, uint64_t first, uint64_t last,
                  int except);

/* Serialise. `hdr` gets one logical block; `arr` gets
 * n_entries * entry_size bytes. `primary` picks which header.
 * Both buffers must be big enough; the sizes are gpt_array_bytes()
 * and t->sector. */
size_t gpt_array_bytes(const gpt_table *t);
void   gpt_serialize(const gpt_table *t, int primary,
                     uint8_t *hdr, uint8_t *arr);

/* The IEEE CRC-32 the specification uses -- the same polynomial zlib
 * uses. Exported because the tests check it against a known vector;
 * a wrong CRC produces a table that reads back as ours and is refused
 * by firmware, which is a failure with no error message. */
uint32_t gpt_crc32(const void *data, size_t n);

#endif
