/* plan.h — where the new partitions go.
 *
 * THE MISTAKE THIS FILE IS SHAPED AROUND
 *
 * The first design for stage C computed the new partitions from the
 * END OF THE DISK: put the recovery partition at
 * `last_usable - 600 MiB`, and run the AurOS root from the shrunk
 * Windows volume up to it.
 *
 * On the layout docs/AURBRIDGE.md itself calls typical -- ESP, MSR,
 * C:, OEM recovery -- that is wrong on every machine. The OEM recovery
 * partition sits at the END of the disk. Shrinking C: creates a gap
 * BETWEEN C: AND WinRE; it creates nothing at the end. Both computed
 * extents land on top of WinRE, the overlap guard fires, and the
 * product refuses its entire target population while believing it is
 * being careful.
 *
 * So: everything here is computed inside the gap, and the gap's far
 * edge comes from gpt_gap_end() -- that is, from the PARTITION TABLE.
 * Never from the /sys survey, which is readdir() order, capped at
 * STAGE_MAX_PART, and holds only the partitions the kernel chose to
 * instantiate. A partition the block layer skipped is invisible to the
 * survey, and laying the AurOS root across one is not a mistake
 * anything later can catch.
 *
 * EVERY NUMBER HERE IS IN LOGICAL BLOCKS, the disk's own, which is
 * 4096 on a 4Kn drive. The survey's `start_lba` is in 512-byte units
 * whatever the drive is -- a kernel convention, not a fact about the
 * disk -- so the two are never mixed and the conversion happens once,
 * at the door.
 */
#ifndef AUROS_PLAN_H
#define AUROS_PLAN_H

#include <stddef.h>
#include <stdint.h>
#include "gpt.h"

/* One mebibyte, in logical blocks. Partitions are aligned to it for
 * the same reason every partitioner since 2010 has: a 4 KiB-physical
 * drive that is misaligned reads and writes twice. */
#define PLAN_ALIGN_BYTES  (1024u * 1024u)

typedef struct {
    uint32_t sector;            /* logical block size, bytes          */
    int      win_idx;           /* the Windows entry in the table     */

    uint64_t win_first;
    uint64_t win_last_old;      /* before                             */
    uint64_t win_last_new;      /* after the filesystem-only shrink   */

    uint64_t gap_first, gap_last;   /* the space the shrink creates   */

    uint64_t root_first, root_last;
    uint64_t rec_first,  rec_last;
    /* THE WAY BACK GETS ITS OWN PARTITION, and not a file in the one
     * above it. The recovery partition is an EFI System partition, so
     * it carries a FAT filesystem that firmware can launch out of;
     * putting the captured copy of somebody's Windows in a FILE on
     * that filesystem means mounting FAT read-write in an initramfs,
     * on the one path whose whole purpose is surviving a machine that
     * has gone wrong. A raw extent has no metadata to corrupt, is one
     * contiguous run by construction, and is found by its type GUID
     * whatever state the rest of the disk is in -- the same three
     * reasons record.h gives for the same decision. */
    uint64_t rsc_first,  rsc_last;
} stage_layout;

/* Work out the layout.
 *
 *   `win_new_bytes`  what the volume will actually be after the
 *                    shrink -- NOT what ntfsresize says the floor is.
 *   `root_src_bytes` the size of the root image that will be written.
 *   `rec_bytes`      the recovery partition.
 *   `min_root_bytes` the PRODUCT floor, not the image size. Setting it
 *                    to "the size at which the image merely fits"
 *                    silently deletes the documented 24 GB minimum and
 *                    hands a user an AurOS with no room in it, having
 *                    shrunk their Windows to get there.
 *
 * Returns 0 and fills `out`, or -1 with one sentence in `why`. */
int plan_compute(const gpt_table *t, int win_idx,
                 uint64_t win_new_bytes, uint64_t root_src_bytes,
                 uint64_t rec_bytes, uint64_t rsc_bytes,
                 uint64_t min_root_bytes,
                 stage_layout *out, char *why, size_t n);

/* Every bound, checked against the TABLE.
 *
 * Called again before every step that acts on the layout, not once at
 * the start: the thing it is guarding against is the plan being right
 * when it was made and wrong by the time it is used. */
int plan_check(const gpt_table *t, const stage_layout *L,
               uint64_t root_src_bytes, uint64_t rec_bytes,
               uint64_t rsc_bytes, uint64_t min_root_bytes,
               char *why, size_t n);

/* Human-readable, for the log and for a support engineer reading a
 * thousand of them. */
void plan_say(const stage_layout *L);

#endif
