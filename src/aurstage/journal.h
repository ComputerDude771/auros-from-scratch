/* journal.h — what AurBridge recorded about this machine, and whether
 * the machine still matches it.
 *
 * WHY THIS EXISTS AT ALL
 *
 * docs/AURBRIDGE.md, phase 3: `BootNext` is one-shot and
 * self-reverting, which is the property that makes a failed first boot
 * a non-event. But a user can cancel a restart, and an application can
 * block one. An armed `BootNext` that is consumed THREE DAYS LATER,
 * after Windows has updated, defragmented, hibernated and grown its
 * pagefile, is a trap: the staging environment would come up believing
 * facts about a disk that has moved underneath it.
 *
 * So the staging environment re-verifies the machine against what was
 * recorded -- disk serial, GPT hash, NTFS start LBA and sector count --
 * and aborts on any mismatch. That is not a nicety; it is the thing
 * that makes the one-shot boot safe to arm at all.
 *
 * THE FORMAT IS JSON, and the reader below is deliberately strict and
 * small: a flat object of strings and integers, nothing nested, no
 * arrays, no escapes beyond the ones a path needs. It is written by a
 * Windows program in another language and read here by a program
 * running as root on somebody's only copy of their photographs, so
 * anything it does not understand exactly is a refusal.
 */
#ifndef AUROS_JOURNAL_H
#define AUROS_JOURNAL_H

#include <stdint.h>

#define JOURNAL_STR 128

typedef struct {
    int      present;             /* a journal was found and parsed   */

    char     disk_serial[JOURNAL_STR];
    char     disk_model[JOURNAL_STR];
    uint64_t disk_bytes;
    uint32_t logical_sector;      /* 512 or 4096; see the 512e note   */

    char     win_part[JOURNAL_STR];   /* "2", the partition number    */
    uint64_t win_start_lba;
    uint64_t win_sectors;
    uint64_t win_ntfs_serial;     /* the NTFS volume serial           */

    char     gpt_sha256[80];      /* of the primary GPT as found      */
    char     stage[JOURNAL_STR];  /* which phase it last completed    */
    uint64_t written_unix;        /* when Windows wrote this          */
} journal;

typedef enum {
    JOURNAL_MATCH = 0,
    JOURNAL_NONE,          /* no journal: nothing armed this          */
    JOURNAL_UNREADABLE,    /* there is one and we do not trust it     */
    JOURNAL_WRONG_DISK,    /* serial does not match                   */
    JOURNAL_MOVED,         /* NTFS is not where it was                */
    JOURNAL_RESIZED,       /* NTFS is not the size it was             */
    JOURNAL_STALE,         /* written far enough back to be a trap    */
} journal_verdict;

/* Read a journal from `path`. Fills `out` and returns 1 if it parsed. */
int journal_read(const char *path, journal *out);

/* Does this machine still match what was recorded? Checks the disk's
 * serial, the age of the record, and -- the part that actually catches
 * a disk which moved underneath us -- the Windows partition's start
 * and length, against the machine the survey found.
 *
 * `why` gets one sentence naming the difference. */
#include "aurstage.h"
journal_verdict journal_check(const journal *j, const stage_machine *m,
                              char *why, size_t n);

#endif
