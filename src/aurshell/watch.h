/* watch.h — the things this computer notices without being asked.
 *
 * Reading the battery and warning about it are not the same feature.
 * Settings can show her 4%, but only if she goes and looks; a laptop
 * that dies in the middle of a sentence has not been unhelpful once,
 * it has lost her work.
 *
 * So: one thing, called once a second, that watches for the handful of
 * facts a computer should speak up about. Today that is the battery.
 * It is a separate file because the list will grow -- a disk filling
 * up, a fan that has stopped -- and because none of it belongs in the
 * middle of the frame loop.
 *
 * WHAT IT WILL NOT DO
 *
 * It will not nag. Each warning is said once as it is crossed and not
 * again until the machine has been plugged in and pulled out, because
 * a message that appears every thirty seconds is one she learns to
 * ignore, and the one that matters is the one she has stopped reading.
 */
#ifndef AUROS_WATCH_H
#define AUROS_WATCH_H

#include "shell.h"

/* Once per second is plenty; calling it more often is free but
 * pointless. Returns 1 if it said something, so the host repaints. */
int watch_tick(shell_ctx *c);

/* The levels, named so the harness and the code cannot drift.
 *
 * LOW is a nudge while there is still time to find the cable. VERY_LOW
 * is the last comfortable moment. CRITICAL is where the machine stops
 * asking and puts itself to sleep, because everything open is about to
 * be lost otherwise -- and sleeping with 3% left is the one action
 * that reliably preserves it. */
#define WATCH_LOW       20
#define WATCH_VERY_LOW  10
#define WATCH_CRITICAL   3

#endif
