/* commit.h — phase 8: the one sector everything else is arranged around.
 *
 * Rule 4 of docs/AURBRIDGE.md: "one atomic commit point per
 * destructive phase. All data movement happens with the old layout
 * still in force; the new partition table is a single sector write."
 *
 * That sector is LBA 1, the primary GPT header. Everything else the
 * commit needs -- the backup header, both copies of the entry array --
 * is written and flushed BEFORE it, while the old primary header is
 * still in force and still describes the old layout. Up to the moment
 * LBA 1 lands, every reader on earth sees the disk exactly as it was.
 *
 * WHY THE BACKUP GOES LAST, WHICH IS NOT WHAT THE DESIGN SAID
 *
 * Both docs/AURBRIDGE.md ("the new GPT: backup header, then primary,
 * one flush") and the review that went over stage C said to write the
 * backup first. tools/committest.sh took a snapshot of the disk after
 * each flush and asked an independent tool what it saw, and the
 * backup-first order does not do what everyone assumed:
 *
 *   Writing the primary ENTRY ARRAY invalidates the primary header,
 *   because the header carries a CRC of the array. Every reader then
 *   falls back to the backup -- which, backup-first, already
 *   describes the NEW layout. So the machine's partitions change at
 *   the array write, not at LBA 1, and rule 4's "the new partition
 *   table is a single sector write" is simply not true of it.
 *
 * Writing the primary array first and the backup last gives the
 * property the rule states. During the window where the primary is
 * invalid, readers fall back to a backup that still describes the OLD
 * layout, so the disk continues to read exactly as it did. Then LBA 1
 * lands -- one sector, and sector writes are atomic on every real
 * device -- and the layout changes in that instant. The backup is
 * brought up to date immediately afterwards.
 *
 * The cost is a brief window where the backup is stale while the
 * primary is correct. Readers prefer the primary, so nothing is
 * misled; only a machine that lost its primary in that same second
 * would see the old table, and that machine is running the recovery
 * tool anyway.
 *
 * LBA 0 IS NEVER WRITTEN. The protective MBR describes the whole disk
 * with one entry, and the disk has not changed size, so there is
 * nothing in it to update. Rewriting it would be a second write in the
 * commit window for no reason, and it is the sector a machine that
 * will not boot at all is usually missing.
 */
#ifndef AUROS_COMMIT_H
#define AUROS_COMMIT_H

#include <stddef.h>
#include "gpt.h"
#include "plan.h"
#include "wr.h"

/* Build the new table from the old one and the layout: shrink the
 * Windows entry, add the AurOS root and the recovery partition.
 * Refuses on anything that does not fit or overlaps. */
int commit_build(const gpt_table *old, const stage_layout *L,
                 gpt_table *out, char *why, size_t n);

/* Write it, in the order above, and read every byte back.
 *
 * `t` must have WR_GPT_BACKUP and WR_GPT_PRIMARY armed. The primary
 * window deliberately includes LBA 1, and this is the only function
 * that writes it. */
/* `after` is called once after each flush, with the step number
 * (1 = the backup is down, 2 = the primary array is down, 3 = LBA 1
 * has landed and the disk is committed). The record uses it to write
 * down which step it reached, and the tests use it to stop the world
 * and ask what a reader would see at that instant -- which is the
 * only way to check the claim the ordering makes. */
int commit_table(wr_target *t, const gpt_table *nw,
                 void (*after)(int step, void *ud), void *ud,
                 char *why, size_t n);

/* After the commit: make the kernel notice, then check and grow the
 * root filesystem into the partition it now has.
 *
 * e2fsck RUNS FIRST, AND NOT AS A PRECAUTION. resize2fs refuses with
 * "Please run 'e2fsck -f' first" whenever s_lastcheck < s_mtime, and
 * that is true of every image build/mkimage produces -- it makes the
 * filesystem and then mounts it to fill it. Without this the root
 * never grows, silently, on every single install, and the user who
 * gave AurOS two hundred gigabytes gets seven. */
int commit_settle(const char *disk_dev, const char *root_dev,
                  char *why, size_t n);

#endif
