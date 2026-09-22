/* fault.c — see fault.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

#include "fault.h"
#include "aurstage.h"

#ifdef AURSTAGE_FAULT

int fault_build(void) { return 1; }

void fault_maybe(const char *name)
{
    char want[64];
    /* The flag is read on EVERY call rather than cached, because the
     * cost is one small file read and the alternative is a static that
     * a future refactor initialises before /proc is mounted -- which
     * is exactly how the dry-run flag was once read as absent on every
     * machine. */
    if (stage_cmdline_value("aurstage.die_at=", want, sizeof want) != 0)
        return;
    if (strcmp(want, name) != 0) return;

    /* Say it before stopping, and say it on stderr, which is
     * unbuffered: this line is how the harness knows the machine died
     * where it was told to and not somewhere else. */
    fprintf(stderr, "\nAURSTAGE-FAULT %s\n", name);
    fflush(NULL);

    /* NO sync(). The whole point is to be the moment the power went,
     * and a tidy shutdown here would flush exactly the writes whose
     * absence is what makes the test interesting. RB_AUTOBOOT with
     * QEMU's -no-reboot makes the machine stop instantly. */
    reboot(RB_AUTOBOOT);
    /* If the kernel would not do it, spin: carrying on past a fault
     * point would produce a run that looks like a survived power cut
     * and is not one. */
    for (;;) pause();
}

#else

int  fault_build(void) { return 0; }
void fault_maybe(const char *name) { (void)name; }

#endif
