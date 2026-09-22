/* main.c — PID 1 in the staging environment. See aurstage.h.
 *
 * STAGE A. This program boots, looks at the machine, writes nothing to
 * any disk, and hands over to the installed system in the same boot.
 * That is the whole of it, on purpose: docs/AURBRIDGE.md sets the
 * build order and the reason for it is that every later stage is
 * judged by whether an abort still leaves a machine that boots
 * Windows -- which cannot be judged until the thing doing the aborting
 * is known to work.
 *
 * PID 1 MUST NEVER RETURN. Returning from main() here panics the
 * kernel, which on a user's machine is a screen of hexadecimal where
 * an explanation should be. Every path below ends in either a
 * successful handover or a sentence and a controlled stop.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <time.h>

#include "aurstage.h"

/* What the kernel was told to do with us, out of /proc/cmdline. */
static int cmdline_has(const char *word)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return 0;
    buf[k] = 0;
    return strstr(buf, word) != NULL;
}

static int cmdline_value(const char *key, char *out, size_t n)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return -1;
    buf[k] = 0;
    char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\n' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return i ? 0 : -1;
}

/* A staging environment that fails has to stop in a way a person can
 * describe over the telephone. Not a panic, not a reboot loop. */
static void stop_here(const char *why)
{
    stage_warn("%s", why);
    stage_say("Nothing on this computer has been changed.");
    stage_say("Turn it off with the power button and start it again;");
    stage_say("it will come back to Windows.");
    for (;;) pause();
}

int main(void)
{
    /* stdout unbuffered: everything this says may be the last thing
     * anybody sees, and a line sitting in a buffer when the kernel
     * stops is a line that was never said. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    stage_say("AurOS staging environment");
    stage_say("this is the one restart; nothing has been changed yet");

    if (!stage_mount_pseudo())
        stage_warn("some of the kernel's own filesystems are missing; "
                   "going on, because what follows only reads");

    int n = stage_load_modules();
    stage_say("%d drivers loaded", n);

    if (!stage_wait_for_disks(20000))
        stop_here("this computer's disk did not appear. "
                  "It may use a storage mode AurOS cannot see yet.");

    stage_machine m;
    stage_survey(&m);
    stage_report(&m);

    /* THE ONLY THING STAGE A DOES BESIDES LOOK.
     *
     * `aurstage.dry` means: say what you found and stop, changing
     * nothing and handing over to nothing. It is how the hardware
     * matrix gets built before any disk is at risk, and it is what
     * docs/AURBRIDGE.md calls the first shippable artifact once stage
     * B's readers are behind it. */
    if (cmdline_has("aurstage.dry")) {
        stage_say("asked to look and not touch; that is all of it");
        stage_say("nothing on this computer has been changed");
        sync();
        reboot(RB_POWER_OFF);
        for (;;) pause();
    }

    char root[128];
    if (cmdline_value("aurstage.root=", root, sizeof root) != 0) {
        /* Nobody said which. Stage A does not guess: choosing a root
         * by looking for "the ext4 one that has an init in it" is
         * exactly the sort of helpfulness that picks the wrong disk on
         * a machine with two. Stage B gets this from the journal
         * AurBridge wrote, which names it. */
        stop_here("nobody said which system to start "
                  "(no aurstage.root= on the kernel command line)");
    }

    stage_say("asked to hand over to %s", root);
    if (stage_switch_root(root) != 0)
        stop_here("that system could not be started");

    /* switch_root does not return. If we are here, exec failed after
     * the old root was already gone, and there is nothing left to try. */
    stop_here("the handover did not complete");
    return 0;
}
