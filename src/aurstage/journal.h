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
    int      corrupt;             /* one was found and would not      */

    char     disk_serial[JOURNAL_STR];
    char     disk_model[JOURNAL_STR];
    uint64_t disk_bytes;
    uint32_t logical_sector;      /* 512 or 4096; see the 512e note   */

    char     win_part[JOURNAL_STR];   /* "2", the partition number    */
    /* IN 512-BYTE UNITS, ALWAYS, WHATEVER THE DISK'S SECTOR SIZE IS.
     *
     * This is the one field in the file with a real chance of being
     * written wrong by the other side, so it is written down here
     * rather than left to be inferred. Linux reports a partition's
     * start and length in /sys in 512-byte units on every disk,
     * including a 4Kn one, and that is what these are compared
     * against. AurBridge computes them from Windows' byte offsets as
     * offset / 512 -- NOT offset / logical_sector, which on a 4Kn
     * disk is eight times too small and makes every such machine
     * report that Windows has moved. */
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
    JOURNAL_TABLE_CHANGED, /* the partition table is not the same one */
    JOURNAL_CORRUPT,       /* a record is there and does not parse     */
    JOURNAL_CLOCK,         /* this machine's clock is before it        */
    JOURNAL_SECTORS,       /* the disk reports a different sector size */
} journal_verdict;

/* Read a journal from `path`. Returns 1 if it parsed.
 *
 * A FILE THAT IS THERE AND WILL NOT PARSE IS NOT THE SAME AS NO FILE,
 * and `out->corrupt` is how the two are told apart. They used to be
 * one answer, so a half-written record -- which is exactly what a
 * power cut during the Windows phase leaves -- reported as "nothing
 * on this computer asked for this" and the run carried on as an
 * innocent look-only. Stage C inherits this gate, and there it is the
 * difference between a refusal and a resize. */
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

/* A short, stable name for a verdict. Never translated. */
const char *journal_verdict_name(journal_verdict v);

#endif
