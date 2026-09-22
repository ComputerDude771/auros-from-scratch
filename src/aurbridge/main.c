/* aurbridge — the Windows-side installer.
 *
 * This binary is the only part of AurOS that can destroy a stranger's
 * data, so its default mode is to look and refuse, never to act. The
 * destructive phases are separate subcommands that each re-run preflight
 * and abort unless it returns a clean report. */
#include "preflight.h"
#include <stdio.h>
#include <string.h>

static void usage(void)
{
    fputs(
    "aurbridge — install AurOS alongside Windows\n\n"
    "  aurbridge preflight            check this PC and report (read-only)\n"
    "  aurbridge preflight --json     same, machine-readable for the wizard\n"
    "  aurbridge selftest             check this binary, not this PC\n"
    "  aurbridge version\n\n"
    "Preflight never writes to a disk. Destructive phases refuse to start\n"
    "unless preflight returns no blocking issues.\n", stderr);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "preflight";
    int json = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--json")) json = 1;

    if (!strcmp(cmd, "version")) {
        puts("aurbridge 0.1.0 (AurOS Nocturne)");
        return 0;
    }
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage(); return 0;
    }
    /* WHAT THE BINARY CAN SAY ABOUT ITSELF, as against what it can say
     * about the machine. preflight needs real disks, a real BitLocker
     * state and real firmware variables, so it says nothing useful
     * under Wine or in CI on Linux. pf_selftest() checks the parts
     * that are pure -- the parsers, the thresholds, the report
     * plumbing -- and those are exactly the parts a build should
     * refuse to ship broken. */
    if (!strcmp(cmd, "selftest")) {
        int bad = pf_selftest();
        if (bad == 0) puts("aurbridge selftest: ok");
        else fprintf(stderr, "aurbridge selftest: %d check%s wrong\n",
                     bad, bad == 1 ? "" : "s");
        return bad ? 1 : 0;
    }

    if (strcmp(cmd, "preflight") != 0) { usage(); return 2; }

    pf_report r;
    pf_run(&r);
    if (json) pf_print_json(&r); else pf_print_human(&r);

    /* Exit code is the contract for the wizard and for CI:
     * 0 = go, 1 = blocked. */
    return pf_is_go(&r) ? 0 : 1;
}
