/* appwatch.h — a program was installed; show it now.
 *
 * The app table was read once, when the desktop started. So a program
 * installed from "Get more programs" did not exist anywhere on her
 * screen until the machine was restarted: the store said Installed,
 * she pressed Home, and the thing she had just installed was not
 * there. The store's own Open button was the only way to reach it,
 * once, and after that it was lost.
 *
 * This watches every directory programs announce themselves in --
 * Ubuntu's, the local ones, Flathub's for the whole machine and
 * Flathub's for her -- with inotify, and asks for a rescan a moment
 * after they stop changing (an install touches the directory several
 * times in a second; one rescan, not five). A directory that does not
 * exist yet -- Flathub's, before the first Flathub program -- is
 * waited for through the nearest parent that does.
 */
#ifndef AUROS_APPWATCH_H
#define AUROS_APPWATCH_H

/* Start watching. Idempotent. Returns the fd for the host's poll(),
 * or -1 if inotify is not available (the desktop then simply does not
 * see new programs until it is restarted, which is how it was). */
int  appwatch_open(void);
int  appwatch_fd(void);
void appwatch_close(void);

/* Read whatever is waiting, without blocking. Call every pass. */
void appwatch_pump(void);

/* 1 once, when a change has settled and the host should rescan. */
int  appwatch_due(void);

/* How long the host may sleep before a pending rescan is due, or -1. */
int  appwatch_wait_ms(void);

/* The quiet time after the last change before a rescan, and the most a
 * rescan can be put off by a directory that never stops changing. */
#define APPWATCH_SETTLE_MS  700
#define APPWATCH_MAX_MS    4000

#endif
