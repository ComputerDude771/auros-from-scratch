/* run.h — starting another program, without a shell.
 *
 * The shell has to ask other programs things: how loud the sound is,
 * which networks are in range, whether this computer has Bluetooth.
 * Every one of those is a fork and an exec, and every one of them is
 * handed strings that came from somewhere else -- a network's name, a
 * device's name -- so not one of them may go through a shell. A wifi
 * called `;rm -rf ~` is a name.
 *
 * Two shapes, and the difference matters:
 *
 *   run_detached   we do not want an answer, only the doing. Turning
 *                  the volume down. It returns the moment the fork
 *                  succeeds, so the screen never waits for it.
 *
 *   run_capture    we want the answer and we want it now, because
 *                  something is about to be drawn with it. It BLOCKS,
 *                  with a timeout, so it is only for the places where
 *                  the alternative is drawing a wrong number: start-up,
 *                  and opening a panel. Never in a frame loop, never on
 *                  a keystroke.
 *
 * The long-running asks -- a wifi scan takes seconds -- do not belong
 * here at all. src/aurshell/net.c polls those against the main loop so
 * the desktop keeps painting while it waits.
 */
#ifndef AUROS_RUN_H
#define AUROS_RUN_H

#include <stddef.h>

/* Returns 0 if it started, -1 if it could not. Nothing about whether
 * the program then worked, which by definition we are not waiting for. */
int  run_detached(const char *const argv[]);

/* Its output (stdout and stderr, in the order they came) into `out`,
 * NUL-terminated and truncated to fit. Returns the number of bytes, or
 * -1 if it could not be started. A program that outlives `timeout_ms`
 * is killed and whatever it had said by then is what you get: a
 * desktop that stops painting because a helper hung is worse than a
 * number that is briefly missing. */
int  run_capture(const char *const argv[], char *out, size_t n, int timeout_ms);

/* Start it, wait for it, and say how it ENDED.
 *
 *   >= 0    the program's exit status; 0 means it worked
 *   RUN_RUNNING   still going when the deadline passed, which for
 *                 `systemctl poweroff` is the ordinary case
 *   RUN_NOSTART   it never started: no fork, no exec, nothing
 *
 * THOSE LAST TWO ARE NOT THE SAME ANSWER. They were both -1, and
 * run.h said so in one sentence -- "could not be started or did not
 * finish" -- and main.c read the union as "it is doing it". So a
 * machine that could not fork, which is exactly the state a machine is
 * in when a person reaches for Turn off, got a button that did nothing
 * and said nothing. That is the bug this function was added to fix,
 * committed inside the fix for it.
 *
 * This exists because run_detached() returning 0 means only "a process
 * was created", and three buttons were reading that as "the machine is
 * turning off". A `systemctl suspend` that policy refuses forks
 * perfectly, exits 1, and nobody looks. Use run_status() where the
 * failure has to be SAID and the wait is not in a frame loop. */
#define RUN_RUNNING  (-1)
#define RUN_NOSTART  (-2)
int  run_status(const char *const argv[], int timeout_ms);

/* Collect the ones run_detached() started. Swept once per pass of the
 * main loop. Each subsystem in this shell waits for its own children;
 * one that reaps another's leaves that one unable to learn how its
 * child ended or to signal it safely. */
void run_reap(void);

/* Point everything this shell starts at the session bus logind made,
 * once it exists, instead of at the private one the unit's wrapper
 * made at boot. Cheap (one stat) and a no-op after it has succeeded.
 * Call it from the main loop; see run.c for why it cannot simply be
 * an ordering dependency in the unit file. Returns 1 the one time it
 * changes anything. */
int  run_adopt_user_bus(void);

#endif
