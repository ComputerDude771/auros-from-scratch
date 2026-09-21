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

/* Collect the ones run_detached() started. Swept once per pass of the
 * main loop. Each subsystem in this shell waits for its own children;
 * one that reaps another's leaves that one unable to learn how its
 * child ended or to signal it safely. */
void run_reap(void);

#endif
