/* aurfirst.h — phases 9 and 10: the machine stops being on loan.
 *
 * WHERE THIS SITS
 *
 * docs/AURBRIDGE.md's phase model ends with two phases nothing
 * implemented:
 *
 *    9  FIRSTBOOT   desktop; user confirms "this works"
 *   10  IMPORT      Ferry imports files; optionally make AurOS default
 *
 * Everything before them is written and tested. The installer leaves a
 * machine that has AurOS on it, has a signed boot chain in a partition
 * of its own, has a Boot#### entry pointing at that partition, and has
 * BootNext armed at it -- and has BootOrder untouched, so switching it
 * on still reaches Windows. That is rule 3: nothing irreversible until
 * AurOS has booted and the user has said it works.
 *
 * This is the program that lets her say it.
 *
 * THE HOLD, AND WHY IT IS NOT A ONE-OFF
 *
 * BootNext is consumed by the firmware as it is used. The installer
 * arms it at the end of the install and then hands over to AurOS in the
 * SAME boot, so it is still armed when the machine is next switched
 * off -- but exactly once. If somebody uses AurOS, shuts down, starts
 * up (BootNext fires, AurOS), shuts down and starts up again, the
 * second start has nothing armed and reaches Windows.
 *
 * So `aurfirst hold` re-arms it on every boot until the question is
 * answered. The machine keeps coming back to AurOS while AurOS works,
 * and the moment it does not the firmware falls through to BootOrder,
 * which still says Windows, and nobody has to do anything. That
 * property is the whole of R4's answer and it is worth paying one
 * NVRAM write per boot for.
 *
 * THE ONLY PLACE THAT WRITES BootOrder
 *
 * src/aurstage/nvram.h says, at length, that the staging environment
 * may not write BootOrder and that there is deliberately no function
 * there that does -- because the way a rule like that gets broken is
 * somebody finding a function that already does the thing. The rule was
 * never "this product may not write BootOrder"; it was "not until the
 * user has seen AurOS work and said so". This program is what "and said
 * so" means, and `aurfirst confirm` is the one function in the tree
 * that writes it.
 *
 * It is a separate program with a separate copy of about a hundred
 * lines of efivarfs plumbing, and that duplication is deliberate rather
 * than an oversight. The alternative is one shared writer that both can
 * call, which is precisely the function the staging environment must
 * not have. Two programs with different rules about what they may write
 * do not share the thing the rules are about. tools/aurfirsttest.sh
 * pins the one thing they DO share -- the on-disk shape of an efivarfs
 * variable -- against the other implementation's bytes.
 *
 * NOTHING HERE IMPORTS ANYTHING. Ferry is a complete set of tools
 * already, installed at /usr/lib/ferry, and it mounts Windows
 * read-only and refuses a dirty volume out loud. This program decides
 * WHEN it is allowed to run -- which is after the answer, never before
 * -- and gets out of the way.
 */
#ifndef AUROS_AURFIRST_H
#define AUROS_AURFIRST_H

#include <stddef.h>
#include <stdint.h>

/* Where the answer is remembered. On the root filesystem, not in
 * NVRAM: NVRAM is what the firmware reads and this is what AurOS
 * reads, and a machine whose CMOS battery has died should not be asked
 * the question again on every boot for the rest of its life. */
/* Overridable at compile time, by the test and by nothing else --
 * there is no environment variable that moves where the answer is
 * remembered, because a machine in the field that can be told to
 * remember it somewhere else is a machine that can be told to forget
 * it. src/aurstage/nvram.c is arranged the same way and says so. */
#ifndef AF_DIR
#define AF_DIR        "/var/lib/auros"
#endif
#define AF_CONFIRMED  AF_DIR "/converted.confirmed"
#define AF_DECLINED   AF_DIR "/converted.declined"

/* WHERE THE DESKTOP READS THIS FROM.
 *
 * The shell runs as the person using the machine and has no business
 * reading efivarfs, parsing load options, or starting a program to ask
 * a question. So every run of aurfirst writes what it found here, as
 * key=value, and src/aurshell/welcome.c reads that one small file.
 *
 * On /run rather than in AF_DIR because it describes THIS BOOT: the
 * hold unit refreshes it before the desktop starts, and a stale copy
 * surviving a reboot would be a desktop asking a question that has
 * already been answered on a machine somebody has since changed in
 * the firmware setup screen. */
#ifndef AF_STATE_FILE
#define AF_STATE_FILE "/run/auros-first.state"
#endif

/* The description the installer gave its entry. One string, in one
 * place, because the two halves finding each other by it is the whole
 * mechanism. It matches src/aurstage/loader.h's LOADER_ENTRY_DESC and
 * tools/aurfirsttest.sh checks that it still does. */
#define AF_ENTRY_DESC "AurOS"

typedef struct {
    int      efivars;       /* there is an efivarfs to read           */
    int      writable;      /* and we could write to it               */
    int      have_entry;    /* a Boot#### whose description is ours   */
    uint16_t entry;
    int      in_order;      /* it appears in BootOrder                */
    int      is_default;    /* it is FIRST in BootOrder               */
    int      have_order;    /* BootOrder exists at all                */
    int      bootnext;      /* BootNext is armed at our entry         */
    int      confirmed;
    int      declined;
    /* The stamps could not be looked at -- /var not mounted yet, or a
     * stat that failed for a reason other than "not there". Neither
     * answered nor unanswered, and the hold does nothing in it. */
    int      unknown;
} af_state;

/* Look, and change nothing. */
int  af_look(af_state *s);

/* Write what was found where the desktop can read it. Called after
 * every subcommand, including the ones that changed something, so the
 * file is never a description of the state before the change. Failure
 * is not an error worth stopping for: the answer is in NVRAM and on
 * the root filesystem either way, and the worst case is a desktop that
 * does not ask this boot. */
void af_publish(const af_state *s);

/* Arm BootNext at our entry, so the next start reaches AurOS once.
 * Does nothing at all once the question has been answered either way.
 * Returns 0 armed, 1 nothing to do, -1 with a sentence. */
int  af_hold(const af_state *s, char *why, size_t n);

/* "This works." Put our entry at the head of BootOrder, clear
 * BootNext, and write the stamp. The one function in this tree that
 * writes BootOrder. */
int  af_confirm(const af_state *s, char *why, size_t n);

/* "It does not." Clear BootNext so the next start reaches whatever
 * BootOrder says -- which is still Windows, because nothing here has
 * touched it -- and write the stamp so the hold stops re-arming. */
int  af_decline(const af_state *s, char *why, size_t n);

/* ── the one word the desktop is allowed to ask for ───────────────── */

/* Take the request out of the desktop's runtime directory and remove
 * it. `out` gets one of "confirm", "decline", "import", or the literal
 * "unknown". Returns 0 for one of ours, 1 for unknown, -1 for nothing
 * there. See request.c for why this is not three lines of shell. */
int af_request_take(char *out, size_t n);

/* ── the EFI variables, read and written from inside AurOS ────────── */

#define AF_GLOBAL_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"

/* As much of one Boot#### as anything here needs to look at. The
 * firmware's own limit is far higher; a load option this side cannot
 * read whole is one it declines to reason about. */
#define AF_OPT_MAX 4096

/* Read one global variable's data, attributes stripped. Returns the
 * length, -1 if there is nothing to read, or -2 if it is LONGER than
 * `n` -- which is a refusal and not a shorter answer, because a
 * silently clipped BootOrder is a boot menu with entries missing. */
int  af_var_get(const char *name, uint8_t *out, size_t n);
/* Write one, attributes and data in a single write(2). */
int  af_var_put(const char *name, const void *data, size_t len,
                char *why, size_t n);
/* Delete one. A zero-length write is not a delete on every kernel;
 * unlink is. */
int  af_var_del(const char *name, char *why, size_t n);

int  af_efivars_present(void);
int  af_efivars_writable(void);

/* Every global Boot#### that exists, ascending. Returns how many. */
int  af_boot_numbers(uint16_t *out, int max);
/* The ASCII description of a load option. */
void af_desc_of(const uint8_t *opt, int len, char *out, size_t n);

/* IS THIS ONE MEANT TO BE STARTED BY ITSELF?
 *   1  yes -- read, and its attributes say it is a boot option
 *   0  no  -- read, and its attributes say it is not
 *  -1  cannot tell: unreadable, truncated, or too short to be one
 *
 * A load option's first four bytes are its attributes, and the UEFI
 * spec is explicit about two of them: an entry without
 * LOAD_OPTION_ACTIVE is not to be booted, and one with
 * LOAD_OPTION_HIDDEN is not even to be shown. A third, the category
 * field, separates a boot option from an APPLICATION -- a firmware
 * setup entry, a vendor's diagnostics -- which the boot manager is not
 * supposed to run in the ordinary sequence at all.
 *
 * This matters in exactly one place: a machine whose firmware ships
 * with no BootOrder and enumerates for itself. af_confirm() builds one
 * there, and building it in plain numeric order would put the
 * manufacturer's diagnostics partition ahead of Windows -- something
 * nobody asked for, which she would then have to undo in a firmware
 * menu she was told she would never have to open.
 *
 * THREE ANSWERS AND NOT TWO, and that distinction is the whole safety
 * of this. The first version returned 0 for "no" and for "I could not
 * read it" alike, and af_confirm DROPPED everything that answered 0.
 * A review built the machine that breaks: firmware with no BootOrder
 * -- which is firmware that has already shown it does not keep the
 * boot variables tidy -- and a Windows entry with LOAD_OPTION_ACTIVE
 * clear, which is how several vendors record "the user switched this
 * off in the boot menu" rather than deleting the variable. Windows
 * was filtered out, a BootOrder containing only AurOS was written,
 * confirm printed success and stamped the answer permanently, and
 * af_decline then refused to undo it because ours was the only entry
 * left. The way back was gone, and the program had removed it while
 * saying the opposite.
 *
 * So nothing is dropped any more; see af_confirm. This answer only
 * decides ORDER, and -1 is ordered with the yeses, because "I could
 * not read it" is not evidence against an entry. */
int  af_boot_bootable(uint16_t num);

/* How af_look() chose our entry: "partition" when it names the boot
 * partition this install starts from, "description" when that could
 * not be worked out and the first entry called AurOS was taken. */
const char *af_entry_by(void);

#endif
