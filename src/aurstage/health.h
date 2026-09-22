/* health.h — is this machine well enough to be operated on?
 *
 * Two questions, asked before the one irreversible step and not after.
 *
 * THE POWER. docs/research/red-team.md, R6: "Refuse on battery <50% or
 * not on AC." The shrink is the only non-restartable window in the
 * product and it can run for forty minutes on a 5400 rpm disk. A
 * laptop that dies in the middle of it is the one row of the
 * power-loss table with no good answer, so the answer has to be to
 * not start.
 *
 * THE DRIVE. R5: "read SMART first (reallocated, pending,
 * uncorrectable, power-on hours). Refuse on any pending/uncorrectable
 * sectors, no override." We are selecting for exactly the population
 * this matters to -- the pitch is "your old PC" -- and a shrink is
 * the most aggressive workload that drive has ever seen: it reads
 * sectors untouched for years and then writes several gigabytes.
 *
 *   A user told "your hard drive is failing, back up now" is a user
 *   we saved. A user whose drive dies during our install is one we
 *   killed.
 *
 * NO OVERRIDE ON EITHER, and that is deliberate. An override exists to
 * be used by the person least able to judge the risk, at the moment
 * they most want to ignore it.
 *
 * NOTHING HERE WRITES. The power state comes out of sysfs; SMART comes
 * out of a read-only pass-through command on a read-only fd.
 */
#ifndef AUROS_HEALTH_H
#define AUROS_HEALTH_H

#include <stddef.h>
#include <stdint.h>

/* ── the power ───────────────────────────────────────────────────── */

typedef struct {
    int found;            /* we could see a power supply at all       */
    int on_mains;         /* a mains adapter is present AND online    */
    int has_battery;
    int percent;          /* -1 if it will not say                    */
    int charging;
} power_state;

/* Read the machine's power state. `root` is the sysfs directory to
 * read, or NULL for the real one -- a parameter only so the tests can
 * build a machine that this container does not have. */
void power_read(const char *root, power_state *out);

/* Is it safe to begin the one irreversible step? `why` gets one
 * sentence naming what is wrong, in her words. */
int  power_ok(const power_state *p, char *why, size_t n);

/* ── the drive ───────────────────────────────────────────────────── */

typedef enum {
    SMART_GOOD = 0,
    SMART_FAILING,      /* pending or uncorrectable sectors: refuse   */
    SMART_WORN,         /* reallocations, but nothing pending: warn   */
    SMART_UNKNOWN,      /* the drive would not answer                 */
} smart_verdict;

typedef struct {
    smart_verdict verdict;
    int      answered;          /* the drive returned SMART data      */
    uint64_t reallocated;       /* sectors already moved              */
    uint64_t pending;           /* waiting to be moved: THE bad one   */
    uint64_t uncorrectable;     /* could not be read at all           */
    uint64_t power_on_hours;
    char     why[200];
    char     remedy[200];
} smart_state;

/* Ask the drive about itself. `dev` is a whole-disk device path.
 * Always fills `out`; a drive that will not answer is SMART_UNKNOWN,
 * which is not a refusal on its own -- plenty of USB bridges and
 * virtual disks do not pass SMART through, and refusing every one of
 * them refuses machines that are fine. */
void smart_read(const char *dev, smart_state *out);

/* Form a verdict from a raw ATA SMART data page.
 *
 * Exposed, rather than buried in smart_read(), so the tests can hand
 * it pages from drives nobody here owns. A failing drive is not
 * something you can arrange to have on the day you write the code
 * that refuses one, and a refusal that has never once fired is a
 * refusal nobody should trust. */
void smart_parse_ata(const unsigned char page[512], smart_state *out);

/* The short, stable name for a verdict, for the report line. */
const char *smart_verdict_name(smart_verdict v);

#endif
