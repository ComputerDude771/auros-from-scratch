/* ntfs.h — what the Windows volume says about itself, read from Linux.
 *
 * WHY NOT ASK WINDOWS
 *
 * docs/AURBRIDGE.md: "The authoritative check is not 'did Windows say
 * it shut down cleanly'. It is the on-disk NTFS state read from Linux,
 * immediately before touching anything."
 *
 * Windows' own opinion is recorded minutes earlier, on the other side
 * of a restart, by a system that may have installed updates in the
 * meantime. The bytes on the disk are the only thing that is true at
 * the moment we are about to change them.
 *
 * NOTHING HERE MOUNTS ANYTHING. Every field below comes from reading
 * sectors. A mount would replay the journal, which is a write, on the
 * volume we have promised not to touch.
 *
 * THIS PARSES HOSTILE DATA. A corrupt or crafted NTFS volume is an
 * attacker-controlled input to a program running as root on somebody's
 * only copy of their photographs. Every length in the structures below
 * comes off the disk, so every one of them is bounds-checked before
 * use, and a structure that does not make sense is a refusal rather
 * than a guess.
 */
#ifndef AUROS_NTFS_H
#define AUROS_NTFS_H

#include <stdint.h>

typedef enum {
    NTFS_OK = 0,
    /* Each of these is a REFUSAL, and each has a remedy a person can
     * actually carry out. A refusal the user cannot clear by following
     * its own instructions is a user who re-runs for ever. */
    NTFS_NOT_NTFS,        /* no NTFS here at all                       */
    NTFS_BITLOCKER,       /* -FVE-FS-. See the MUST NOT in ntfs.c      */
    NTFS_DIRTY,           /* the volume wants chkdsk                   */
    NTFS_LOG_UNCLEAN,     /* $LogFile has un-replayed transactions     */
    NTFS_HIBERNATED,      /* hiberfil.sys: a session is still in there */
    NTFS_UNREADABLE,      /* the disk would not give us the sectors    */
    NTFS_STRANGE,         /* structurally impossible; we do not guess  */
} ntfs_verdict;

/* What this file established FOR ITSELF, as against what it is
 * leaving to ntfsresize.
 *
 * Stage B may go on with an UNKNOWN: ntfsresize refuses a hibernated
 * or unclean volume itself and is the program doing the work anyway,
 * so an UNKNOWN costs a worse error message and nothing else.
 *
 * STAGE C MUST NOT. By then the answer decides whether a partition
 * gets rewritten, ntfsresize has already been run, and "we could not
 * tell" is not a state in which anybody's disk gets modified. */
typedef enum { NTFS_NO = 0, NTFS_YES, NTFS_UNSURE } ntfs_tri;

typedef struct {
    ntfs_verdict verdict;
    char     why[200];        /* one sentence, in her words            */
    char     remedy[200];     /* what would actually clear it          */

    /* Geometry, as the volume states it. The journal AurBridge wrote
     * is checked against these: a machine whose NTFS has moved or
     * changed size since Windows last looked at it is a machine we
     * stop on. */
    uint32_t bytes_per_sector;
    uint32_t bytes_per_cluster;
    uint64_t total_sectors;   /* the volume's own count, not /sys's    */
    uint64_t mft_lcn;
    uint64_t serial;          /* the NTFS volume serial number         */
    uint16_t volume_flags;    /* the raw flags word, for the log       */

    /* Both are YES for the BAD case, so that neither reads backwards
     * at a call site. See ntfs_tri above for what UNSURE permits. */
    ntfs_tri hibernated;      /* hiberfil.sys holds a live session     */
    ntfs_tri log_dirty;       /* $LogFile has outstanding transactions */
} ntfs_state;

/* Read the state of the NTFS volume on `dev`. Opens it read-only,
 * mounts nothing, writes nothing. Always fills `out`. */
void ntfs_read_state(const char *dev, ntfs_state *out);

/* The one check that must happen at EVERY site that touches partition
 * geometry, not just here -- see the MUST NOT in ntfs.c. 1 if the
 * first sector carries the BitLocker signature. */
int  ntfs_is_bitlocker(const unsigned char *first_sector);

#endif
