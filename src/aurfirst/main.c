/* main.c — aurfirst, the command. See aurfirst.h.
 *
 * FIVE SUBCOMMANDS AND NO INTERFACE. The desktop asks the question;
 * this is what it calls when the person answers, and what the boot
 * calls to keep the machine coming back to AurOS while nobody has
 * answered yet. Splitting it that way means the thing that writes
 * NVRAM is a hundred lines somebody can read, rather than a branch
 * inside sixteen thousand lines of compositor.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "aurfirst.h"

static void usage(void)
{
    fputs(
      "aurfirst — the last two steps of a conversion from Windows\n"
      "\n"
      "  aurfirst state     what this machine is waiting for, as key=value\n"
      "  aurfirst hold      keep the next start coming back to AurOS\n"
      "  aurfirst confirm   \"this works\": make AurOS what it starts by default\n"
      "  aurfirst decline   \"it does not\": the next start reaches Windows\n"
      "  aurfirst ferry     whether importing from Windows may run yet\n"
      "\n"
      "Until confirm, switching this computer on still reaches Windows.\n"
      "That is deliberate: nothing about this machine is irreversible\n"
      "until somebody has seen AurOS work and said so.\n", stderr);
}

static const char *say_bool(int v) { return v ? "yes" : "no"; }

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    char why[400] = {0};
    af_state s;
    af_look(&s);

    if (!strcmp(cmd, "state")) {
        printf("efivars=%s\n",   say_bool(s.efivars));
        printf("writable=%s\n",  say_bool(s.writable));
        printf("converted=%s\n", say_bool(s.have_entry));
        if (s.have_entry) printf("entry=%04X\n", s.entry);
        printf("bootorder=%s\n",  say_bool(s.have_order));
        printf("in_order=%s\n",   say_bool(s.in_order));
        printf("is_default=%s\n", say_bool(s.is_default));
        printf("bootnext=%s\n",   say_bool(s.bootnext));
        printf("confirmed=%s\n",  say_bool(s.confirmed));
        printf("declined=%s\n",   say_bool(s.declined));
        /* One word for the shell to switch on, so the decision about
         * what the desktop should show lives here rather than in six
         * layouts that each reimplement it slightly differently. */
        const char *phase =
            !s.have_entry            ? "not-converted" :
            s.confirmed              ? "done"          :
            s.declined               ? "declined"      :
                                       "asking";
        printf("phase=%s\n", phase);
        return 0;
    }

    if (!strcmp(cmd, "hold")) {
        int rc = af_hold(&s, why, sizeof why);
        if (rc < 0) { fprintf(stderr, "aurfirst: %s\n", why); return 1; }
        if (rc == 1) return 0;
        fprintf(stderr, "aurfirst: the next start will reach AurOS once "
                        "(entry %04X); Windows is still the default\n", s.entry);
        return 0;
    }

    if (!strcmp(cmd, "confirm")) {
        int rc = af_confirm(&s, why, sizeof why);
        if (rc < 0) { fprintf(stderr, "aurfirst: %s\n", why); return 1; }
        if (rc == 1) {
            fprintf(stderr, "aurfirst: this computer was not converted from "
                            "Windows; nothing to change\n");
            return 0;
        }
        fprintf(stderr, "aurfirst: AurOS is now what this computer starts\n");
        return 0;
    }

    if (!strcmp(cmd, "decline")) {
        if (af_decline(&s, why, sizeof why) != 0) {
            fprintf(stderr, "aurfirst: %s\n", why);
            return 1;
        }
        fprintf(stderr, "aurfirst: the next start will reach Windows\n");
        return 0;
    }

    /* MAY FERRY RUN YET.
     *
     * Not a question about Ferry -- it answers its own, loudly, about
     * hibernation and dirty volumes. It is a question about consent and
     * order: importing somebody's documents is the first thing this
     * product does to a machine that the machine cannot undo by
     * restarting, so it waits until the person has said AurOS works.
     * Exit 0 means yes. */
    if (!strcmp(cmd, "ferry")) {
        if (!s.have_entry) {
            puts("not-converted");
            return 1;
        }
        if (!s.confirmed) {
            puts("not-yet-confirmed");
            return 1;
        }
        puts("yes");
        return 0;
    }

    usage();
    return 2;
}
