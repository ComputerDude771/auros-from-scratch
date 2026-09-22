/* record.h — what step this install reached, written down.
 *
 * R6: "On-disk transactional journal so a restarted installer knows
 * which step it died in."
 *
 * WHERE IT LIVES, AND WHY NOT ON THE ESP
 *
 * The obvious place is a preallocated file on the machine's EFI
 * partition. Three separate problems, all found by review:
 *
 *   - Writing it means writing FAT metadata, on the filesystem that
 *     holds \EFI\Microsoft\Boot, through the one phase whose only
 *     answer to a power cut is the recovery USB. A FAT being written
 *     when the power goes is a FAT that may not mount.
 *   - Addressing a file's blocks means FIBMAP, which returns a block
 *     number relative to the START OF THE FILESYSTEM. The absolute
 *     offset needs the partition start added, and forgetting that is
 *     the one mistake in the whole design that writes over the primary
 *     GPT.
 *   - A 64 KiB file on an ESP that is commonly 85-95% full is
 *     frequently not contiguous, and round-robin addressing across a
 *     fragmented extent list is a second thing to get wrong.
 *
 * So it lives in a raw partition on the recovery stick, which R4 makes
 * mandatory anyway: no filesystem, no FIBMAP, no FAT metadata, and one
 * contiguous run by construction. AurBridge creates it in phase 2, the
 * same way it creates the image partition.
 *
 * SIXTEEN SLOTS, ROUND ROBIN, EACH WITH ITS OWN CHECKSUM. A slot
 * half-written when the power goes fails its checksum and is ignored;
 * the one before it is still there. Nothing is ever overwritten in
 * place.
 *
 * THE STEP NUMBERS ARE NOT A WIRE FORMAT, and it is worth saying why
 * inserting one in the middle of the enum below is safe. A record is
 * only ever read by the same build that wrote it, within one install:
 * rec_is_ours() compares the run id, and a record from another run is
 * history rather than progress. A stick carried between two builds of
 * this program therefore has its older records ignored rather than
 * misread -- which is the same answer it already gives to a stick that
 * has somebody else's install on it.
 *
 * AND EVERY RECORD CARRIES THE RUN IT BELONGS TO. Without that, a
 * refusal from last Tuesday is indistinguishable from this run's
 * progress, and the resume ladder reads somebody else's history as its
 * own.
 */
#ifndef AUROS_RECORD_H
#define AUROS_RECORD_H

#include <stddef.h>
#include <stdint.h>
#include "aurstage.h"

/* The type GUID AurBridge gives the record partition. */
extern const uint8_t RECORD_TYPE_GUID[16];

#define REC_SLOTS       16
#define REC_SLOT_BYTES  4096
#define REC_NOTE        160

typedef enum {
    REC_NONE = 0,
    REC_BEGIN,          /* the staging environment came up            */
    REC_GATE_OK,        /* everything checkable was checked and passed*/
    REC_SHRINK_BEGIN,   /* THE ONE IRREVERSIBLE STEP starts here      */
    REC_SHRINK_END,
    REC_WRITE_BEGIN,
    REC_WRITE_END,      /* written AND read back AND hashed           */
    /* The boot partition, written into the gap while the OLD table is
     * still in force. Two steps rather than one because the thing
     * between them is minutes long on a slow stick, and a support
     * engineer reading one of these wants to know whether the copy was
     * running when the power went.
     *
     * NOT because the resume ladder reasons about it -- it does not,
     * and an earlier version of this comment said it did. A machine
     * cut anywhere in here has the old table in force and garbage in
     * free space, which is the same state as a cut during the root
     * write, and starting over is the right answer to all of it. */
    REC_BOOT_BEGIN,
    REC_BOOT_END,
    REC_PROBE_END,
    REC_COMMIT_ARRAY,   /* primary array down; still the old layout   */
    REC_COMMIT_SECTOR,  /* LBA 1 down. The machine is committed.      */
    REC_COMMIT_BACKUP,
    REC_BOOT_ENTRY,     /* AurOS is in the firmware's menu            */
    REC_SETTLE_END,     /* checked and grown                          */
    REC_DONE,
    REC_REFUSED,        /* a designed refusal; nothing was changed    */
    REC_FAILED,         /* something went wrong mid-way               */
} rec_step;

typedef struct {
    uint64_t seq;
    uint64_t run_id;
    uint32_t step;
    uint64_t when;
    char     note[REC_NOTE];
} rec_entry;

typedef struct {
    char     dev[72];       /* the disk the stick is                  */
    uint64_t base;          /* byte offset of the record partition    */
    uint64_t seq;           /* the next sequence number to use        */
    uint64_t run_id;
    int      open;
} rec_target;

/* Find the record area and take the next sequence number.
 * Writes NOTHING: see rec_last. */
int  rec_open(rec_target *r, const stage_machine *m, uint64_t run_id,
              char *why, size_t n);

/* The newest valid slot, whatever run it belongs to.
 *
 * MUST BE CALLED BEFORE THE FIRST rec_write OF THIS BOOT. The first
 * design wrote "this run began" and then asked for the newest record,
 * which was the one it had just written -- so the resume ladder always
 * took its first branch and re-ran ntfsresize on a volume that might
 * be halfway through being resized. Returns 0 if there is one. */
int  rec_last(const rec_target *r, rec_entry *out);

/* Was that record from THIS run? A record from another run is
 * history, not progress, and resuming from it is resuming somebody
 * else's install. */
int  rec_is_ours(const rec_target *r, const rec_entry *e);

/* Append. Each slot is written whole, with its own checksum. */
int  rec_write(rec_target *r, rec_step step, const char *note,
               char *why, size_t n);

const char *rec_step_name(rec_step s);

#endif
