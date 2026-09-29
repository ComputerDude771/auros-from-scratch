/* fix.h — what the installer does about a problem, instead of describing it.
 *
 * The check (preflight.c) finds problems and explains them. This decides,
 * for each one, which of four things is true:
 *
 *   FIX_AUTO     the installer can fix it now, with one press, and does
 *   FIX_RESTART  it needs Windows restarted; the installer restarts it and
 *                opens itself again afterwards, where it left off
 *   FIX_WAIT     only the person can do it (plug in the charger, take out
 *                a USB stick); the installer notices by itself and carries on
 *   FIX_CANNOT   nothing on this PC can fix it; one plain sentence says why
 *
 * and gives the problem and the fix in one short line each, because the
 * page that shows them is read by somebody who wanted AurOS, not a report.
 * A problem this table does not know falls back to the check's own title
 * and remedy, so a new check is never invisible here.
 */
#ifndef AURBRIDGE_FIX_H
#define AURBRIDGE_FIX_H

#include <stddef.h>
#include <wchar.h>

typedef enum { FIX_UNKNOWN = 0, FIX_AUTO, FIX_RESTART, FIX_WAIT, FIX_CANNOT } fix_kind;

typedef struct {
    const char    *id;        /* the preflight finding id              */
    fix_kind       kind;
    const wchar_t *problem;   /* one short line: what is wrong          */
    const wchar_t *fix;       /* one short line: what happens about it  */
    const wchar_t *after;     /* FIX_AUTO only: said when the fix has run
                                 and the check still finds the problem   */
} fix_info;

/* The entry for a finding id, or NULL if there is none. */
const fix_info *fix_lookup(const char *id);
/* The i-th entry, or NULL past the end (for the wizard's own test). */
const fix_info *fix_at(int i);

/* Carry out a FIX_AUTO fix. 0 when it ran (the check that follows says
 * whether it was enough), -1 with a sentence in `why`. Blocking; the
 * wizard calls it from a worker thread. */
int fix_run(const char *id, char *why, size_t n);

/* For FIX_RESTART: arrange for this installer to open again, elevated,
 * the next time this person signs in, then restart Windows. The task it
 * leaves is removed by fix_resume_done(), which the reopened installer
 * calls first. */
int fix_restart_and_continue(char *why, size_t n);
int fix_resume_arm(char *why, size_t n);        /* the first half alone */
void fix_resume_done(void);

#endif
