/* phases.h — what AurBridge does to a computer before the restart.
 *
 * docs/AURBRIDGE.md numbers eleven phases. Four of them happen on
 * Windows, and this is those four:
 *
 *   0 INSPECT   look, and refuse. Nothing is opened for writing.
 *   1 CONSENT   the user agrees, in words, to a thing named plainly --
 *               and if the drive is BitLocker-encrypted, proves she
 *               has the key before anything can make her need it.
 *   2 PREPARE   the memory stick: the AurOS image, a place for the
 *               installer's notes, and room for a copy of this
 *               machine's Windows startup. Consumes a memory stick;
 *               changes nothing on the computer.
 *   3 HANDOFF   the staging environment into the EFI partition, and a
 *               ONE-SHOT boot entry. This is the last thing that
 *               happens before the one restart.
 *
 * NOTHING HERE WRITES TO THE COMPUTER'S OWN DISK. Not one byte. The
 * shrink, the partition table and the image all happen after the
 * restart, in src/aurstage, where nothing is mounted. The complete
 * list of what phases 0-3 change is: the memory stick, two files in a
 * directory on the EFI partition that AurOS created, and two firmware
 * variables. plat.h is what makes that checkable rather than claimed:
 * plat_write() refuses every disk but the stick.
 *
 * AND EVERY PHASE IS UNDOABLE UNTIL THE RESTART. ab_abort() clears
 * BootNext, and a machine that has had phases 0 to 3 done to it and
 * then aborted is a machine with some files on its EFI partition and
 * a memory stick it did not have before. Nothing else.
 */
#ifndef AURBRIDGE_PHASES_H
#define AURBRIDGE_PHASES_H

#include <stddef.h>
#include <stdint.h>
#include "preflight.h"
#include "plat.h"

typedef enum {
    AB_INSPECT = 0,
    AB_CONSENT,
    AB_PREPARE,
    AB_HANDOFF,
    AB_N
} ab_phase;

const char *ab_phase_name(ab_phase p);

/* What the person chose, in the wizard. */
typedef struct {
    char profile[64];          /* "desktop", "school-kiosk"           */
    char shell_archetype[32];  /* shells/<id>.shell                   */
    char language[16];
    char stick_serial[64];     /* the disk she nominated (R4/R11)     */

    /* Where the build put things. Absolute paths; the wizard fills
     * them in from where it is running. */
    char image_path[512];      /* out/auros-<profile>.img             */
    char kernel_path[512];     /* out/auros-staging-vmlinuz           */
    char initrd_path[512];     /* out/auros-staging.img               */

    /* R1. Empty when the drive is not encrypted. When it is, this is
     * what she typed back from the recovery key, and phase 1 refuses
     * to go on without it. */
    char key_typed_back[64];

    /* She has read what phase 1 says and agreed to it. Nothing sets
     * this but a person. */
    int  consent_given;

    /* Minutes of the user's own time this is allowed to take before
     * saying so -- reserved; the phases report progress instead. */
    int  reserved;
} ab_choice;

/* What phases 0 and 2 learned, carried to 3 and written into the
 * journal. */
typedef struct {
    int      disk_index;
    char     disk_serial[128];
    char     disk_model[128];
    uint64_t disk_bytes;
    uint32_t logical_sector;

    char     win_part[8];
    uint64_t win_start_lba;        /* 512-byte units. See journal.h.  */
    uint64_t win_sectors;
    uint64_t win_ntfs_serial;

    uint64_t esp_offset, esp_length;
    /* The EFI partition, as the boot entry has to name it. */
    plat_partition esp;
    char     gpt_sha256[65];
    uint64_t run_id;

    int      stick_index;
    uint64_t stick_bytes;
    uint32_t stick_sector;
    uint64_t image_part_off, image_part_len;
    uint64_t record_part_off, saved_part_off, saved_part_len;

    uint16_t boot_entry;           /* the Boot#### we made            */
    int      bootnext_set;
    /* Phase 1 suspended BitLocker and nothing else has turned it back
     * on. ab_abort() is what turns it back on, and this is how it
     * knows it has to. */
    int      bitlocker_suspended;
} ab_machine;

/* Every phase says what it is doing, because a person is watching a
 * progress bar for twenty minutes and "please wait" is not a sentence
 * anybody should have to read for that long. */
typedef void (*ab_say)(const char *line, void *ud);
typedef void (*ab_progress)(int percent, void *ud);

/* Run phases 0 through `upto`, in order, stopping at the first
 * refusal. Returns 0 when every one of them finished.
 *
 * `r` is a preflight report that has already been run, or NULL to run
 * one. `m` is filled in as it goes and is what ab_abort() needs. */
int ab_run(ab_phase upto, const ab_choice *c, pf_report *r, ab_machine *m,
           ab_say say, ab_progress prog, void *ud, char *why, size_t n);

/* Undo everything that can be undone, which before the restart is all
 * of it. Safe to call at any point, including twice. */
void ab_abort(ab_machine *m, ab_say say, void *ud);

/* Pure enough to test: what size the three partitions on the stick
 * should be for this machine and this image. Exposed because the
 * "your memory stick is too small" refusal has to be able to say a
 * number before anything is written, and because the saved-copy size
 * has to match what src/aurstage/rescue.c will ask for. */
int ab_stick_layout(uint64_t stick_bytes, uint32_t sector,
                    uint64_t image_bytes, uint64_t esp_bytes,
                    uint64_t *image_first, uint64_t *image_last,
                    uint64_t *record_first, uint64_t *record_last,
                    uint64_t *saved_first, uint64_t *saved_last,
                    char *why, size_t n);

/* How big the saved copy of this machine's Windows startup will be.
 * src/aurstage/rescue.c computes the same number from the same facts;
 * if this one is smaller the install refuses AFTER the restart, which
 * is a refusal the user has already waited through a reboot for. */
uint64_t ab_saved_bytes(uint64_t esp_bytes, uint32_t sector, int n_volumes);

int ab_selftest(void);

#endif
