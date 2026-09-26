/* aurbridge — the Windows-side installer.
 *
 * This binary is the only part of AurOS that can destroy a stranger's
 * data, so its default mode is to look and refuse, never to act. The
 * destructive phases are separate subcommands that each re-run preflight
 * and abort unless it returns a clean report. */
#include "preflight.h"
#include "phases.h"
#include "inflate.h"
#include "sbdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void say(const char *line, void *ud)
{ (void)ud; fprintf(stderr, "aurbridge: %s\n", line); }

static void prog(int pct, void *ud)
{
    static int last = -1;
    (void)ud;
    if (pct == last || pct % 10) return;
    last = pct;
    fprintf(stderr, " %d%%", pct);
    if (pct >= 100) fprintf(stderr, "\n");
}

/* getimage PIECES-FILE DEST SHA256 BYTES -- the no-stick download on
 * its own: fetch the pieces with WinHTTP, check them, unpack them and
 * check the result. It writes only files, which is why it can be run
 * under Wine against a server a test starts, and why it is the way the
 * WinHTTP half of the product is exercised before a real machine. */
static int getimage(int argc, char **argv)
{
    if (argc < 6) {
        fputs("usage: aurbridge getimage PIECES-FILE DEST SHA256 BYTES\n", stderr);
        return 2;
    }
    static char text[65536];
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "aurbridge: no %s\n", argv[2]); return 2; }
    size_t k = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[k] = 0;
    ab_choice c;
    memset(&c, 0, sizeof c);
    c.no_stick = 1;
    c.pieces_text = text;
    snprintf(c.image_path, sizeof c.image_path, "%s", argv[3]);
    snprintf(c.image_sha256, sizeof c.image_sha256, "%s", argv[4]);
    c.image_expect = strtoull(argv[5], NULL, 10);
    char why[400] = "";
    if (ab_fetch_image(&c, say, prog, NULL, why, sizeof why) != 0) {
        fprintf(stderr, "aurbridge: %s\n", why);
        printf("getimage verdict=failed\n");
        return 1;
    }
    printf("getimage verdict=ok\n");
    return 0;
}

static void usage(void)
{
    fputs(
    "aurbridge — install AurOS alongside Windows\n\n"
    "  aurbridge preflight            check this PC and report (read-only)\n"
    "  aurbridge preflight --json     same, machine-readable for the wizard\n"
    "  aurbridge selftest             check this binary, not this PC\n"
    "  aurbridge getimage LIST DEST SHA256 BYTES\n"
    "                                 download AurOS in pieces (files only)\n"
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
    if (!strcmp(cmd, "getimage")) return getimage(argc, argv);
    if (!strcmp(cmd, "selftest")) {
        int bad = pf_selftest() + gz_selftest() + sbdb_selftest();
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
