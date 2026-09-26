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

/* ── the image, in pieces ───────────────────────────────────────────
 *
 * Where the image is published for the no-stick build: gzip, cut into
 * numbered pieces small enough for a git host, each with its own
 * SHA-256, all relative to one base address. The list is text, baked
 * into the installer when it is built (build/aurbridge), and read by
 * ab_pieces_parse:
 *
 *   base https://example.org/auros/desktop/
 *   piece auros-desktop.img.gz.000 94371840 <64 hex>
 *   piece auros-desktop.img.gz.001 94371840 <64 hex>
 *   ...
 *
 * What comes out of joining and unpacking them is checked against
 * image_sha256 and image_expect below, so the pieces' own hashes are
 * there to name WHICH piece went wrong, not to decide whether the
 * image is right. */
#define AB_MAX_PIECES 128
typedef struct {
    char     name[96];
    uint64_t bytes;
    char     sha256[65];
} ab_piece;

typedef struct {
    char     base[320];
    int      n;
    uint64_t total;
    ab_piece p[AB_MAX_PIECES];
} ab_pieces;

int ab_pieces_parse(const char *text, ab_pieces *out, char *why, size_t n);

/* What the person chose, in the wizard. */
typedef struct {
    char profile[64];          /* "desktop", "school-kiosk"           */
    char shell_archetype[32];  /* shells/<id>.shell                   */
    /* WHAT SHE CHOSE ON THE PERSONALIZE PAGE, as values the installed
     * system can act on rather than the words on the chips. Written to
     * \EFI\AurOS\choices.conf in phase 3 and applied at first boot
     * (rootfs/usr/lib/auros/choices.sh). Any of them may be empty.
     *   language   a glibc locale:         "es_ES.UTF-8"
     *   keyboard   "xkb:<layout>[:<var>]"  or "klid:<8 hex>" (Windows')
     *   timezone   "iana:<Area/City>"      or "windows:<TimeZoneKeyName>"
     *   theme      themes/<id>.theme       "moss"                        */
    char language[32];
    char keyboard[64];
    char timezone[96];
    char theme[32];
    char stick_serial[64];     /* the disk she nominated (R4/R11)     */

    /* Where the image is, or will be. The wizard fills this in from
     * where it is running: the download lands beside the installer,
     * which is where somebody would look for it. */
    char image_path[512];      /* auros-<profile>.img                 */

    /* WHERE TO GET IT, AND WHAT IT MUST BE.
     *
     * The product is one file somebody downloads. The image is five
     * gigabytes and does not fit inside one, so the installer fetches
     * it -- resumably, because the people this is for are on the
     * connections that drop -- and checks it against a hash that was
     * baked in when the installer was built. A downloaded image whose
     * hash is not this one is not installed, whatever else is true
     * about it.
     *
     * Both empty means the old arrangement: the image must already be
     * sitting beside the installer. That is still what a developer
     * build does. */
    char image_url[512];
    char image_sha256[65];     /* 64 hex characters, or empty         */
    /* How big it will be once it is here. Phase 0 asks whether the
     * memory stick is large enough, and on a machine where the image
     * has not been downloaded yet there is no file to measure. */
    uint64_t image_expect;

    /* The staging environment. Empty means "out of the installer
     * itself", which is what a shipped one does -- see plat.h. A path
     * is for a developer running against a build tree. */
    char kernel_path[512];     /* out/auros-staging-vmlinuz           */
    char initrd_path[512];     /* out/auros-staging.img               */

    /* R1. Empty when the drive is not encrypted. When it is, this is
     * what she typed back from the recovery key, and phase 1 refuses
     * to go on without it. */
    char key_typed_back[64];

    /* She has read what phase 1 says and agreed to it. Nothing sets
     * this but a person. */
    int  consent_given;

    /* THE NO-STICK MODE. No memory stick is written or needed: the
     * image stays at image_path, which must then be on the Windows
     * drive (the wizard puts it in \AurOS\ there), with its manifest
     * beside it as image_path + ".manifest", and the journal says
     * image_on=windows so the staging environment reads it through a
     * read-only mount. What this costs is written down in
     * docs/AURBRIDGE.md, "Installing without a memory stick". */
    int  no_stick;

    /* Where to get the image from when it is not already at
     * image_path: the piece list above (text, or NULL), and failing
     * that image_url. image_alt_path is a copy somebody has already
     * put somewhere else -- beside the installer -- which is moved into
     * place rather than downloaded again. */
    const char *pieces_text;
    char image_alt_path[512];

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

/* Get the image to c->image_path and check it: already there, moved
 * from image_alt_path, downloaded in pieces, or downloaded whole --
 * in that order. Exposed so the console tool can exercise the download
 * on its own, which is how it is tested under Wine. */
int ab_fetch_image(const ab_choice *c, ab_say say, ab_progress prog,
                   void *ud, char *why, size_t n);

int ab_selftest(void);

#endif
