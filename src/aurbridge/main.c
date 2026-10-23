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
#include "fix.h"
#include "gpt.h"
#include "plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

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


/* ── gapselftest: the fast path's Windows half, on a disk made for it ──
 *
 * The fast path (docs/AURBRIDGE.md, "Fast path") shrinks C: with
 * Windows' own online shrink and writes AurOS raw into the gap that
 * frees. Those are the Windows APIs no simulation can stand in for, so
 * this runs the ENGINE'S OWN code -- plat_shrink_system, the shared
 * planner (src/aurstage/plan.c), the gap fence, plat_read -- against a
 * throwaway virtual disk on a real Windows machine
 * (.github/workflows/windows.yml makes one with diskpart):
 *
 *   gapselftest DISK PART DISK-GUID write FREE-MB ROOT-MB
 *       shrink partition PART by FREE-MB, plan the gap the way the
 *       install does, write ROOT-MB and an 8 MB boot area of a known
 *       pattern into it, read both back, and prove that a write onto
 *       the NTFS partition is refused by BOTH fences. Leaves the disk
 *       shrunk and written, so the caller can check it independently.
 *   gapselftest DISK PART DISK-GUID undo ORIG-BYTES
 *       grow PART back to ORIG-BYTES with plat_grow_system.
 *
 * It refuses the disk Windows is running from, and any disk whose GPT
 * disk GUID is not the one named: a disk number typed wrong is the one
 * mistake this must not be able to make. */
typedef struct { int disk; } st_rd;
static int st_read(void *ud, uint64_t off, void *buf, size_t n)
{
    char why[PLAT_WHY];
    return plat_read(((st_rd *)ud)->disk, off, buf, n, why, sizeof why);
}

static void guid_str(const uint8_t g[16], char *out, size_t n)
{
    snprintf(out, n, "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
                     "%02X%02X%02X%02X%02X%02X}",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* A pattern nobody could mistake for anything: xorshift64 seeded by the
 * megabyte it lands in, so any one megabyte can be regenerated alone. */
static void st_pattern(uint8_t *b, size_t n, uint64_t seed)
{
    uint64_t x = seed * 0x9E3779B97F4A7C15ull | 1;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        memcpy(b + i, &x, 8);
    }
}

static double st_now(void) { return (double)GetTickCount64() / 1000.0; }

static int st_fail(const char *what, const char *why)
{
    fprintf(stderr, "aurbridge: gapselftest: %s%s%s\n", what,
            why && why[0] ? ": " : "", why ? why : "");
    printf("gapselftest verdict=failed step=\"%s\"\n", what);
    return 1;
}

/* Write `len` bytes of pattern at disk offset `at` through the fence,
 * hashing what was written; then read it back with plat_read and hash
 * that. Both hashes come back as hex. */
static int st_extent(int disk, uint64_t at, uint64_t len, uint64_t seed0,
                     char *wrote_hex, char *read_hex, char *why, size_t wn)
{
    static uint8_t buf[1 << 20];
    fmt_sha h; fmt_sha_start(&h);
    unsigned char d[32];
    for (uint64_t o = 0; o < len; o += sizeof buf) {
        size_t k = len - o < sizeof buf ? (size_t)(len - o) : sizeof buf;
        st_pattern(buf, k, seed0 + o / sizeof buf);
        if (plat_gap_write(disk, at + o, buf, k, why, wn) != 0) return -1;
        fmt_sha_feed(&h, buf, k);
    }
    fmt_sha_done(&h, d); fmt_sha_hex(d, wrote_hex, 65);
    if (plat_gap_flush(disk, why, wn) != 0) return -1;
    fmt_sha_start(&h);
    for (uint64_t o = 0; o < len; o += sizeof buf) {
        size_t k = len - o < sizeof buf ? (size_t)(len - o) : sizeof buf;
        if (plat_read(disk, at + o, buf, k, why, wn) != 0) return -1;
        fmt_sha_feed(&h, buf, k);
    }
    fmt_sha_done(&h, d); fmt_sha_hex(d, read_hex, 65);
    return 0;
}

static int gapselftest(int argc, char **argv)
{
    if (argc < 7) {
        fputs("usage: aurbridge gapselftest DISK PART DISK-GUID write FREE-MB ROOT-MB\n"
              "       aurbridge gapselftest DISK PART DISK-GUID undo ORIG-BYTES\n", stderr);
        return 2;
    }
    int disk = atoi(argv[2]), part = atoi(argv[3]);
    const char *want_guid = argv[4], *phase = argv[5];
    char why[PLAT_WHY] = "";

    /* NOT THE COMPUTER'S OWN DISK, whatever number was typed. */
    static pf_report r;
    pf_run(&r);
    if (r.system_disk >= 0 && r.system_disk < r.n_disks &&
        r.disks[r.system_disk].index == disk)
        return st_fail("refusing: that is the disk Windows is running from", NULL);

    plat_disk dd[16];
    int nd = plat_disks(dd, 16), di = -1;
    for (int i = 0; i < nd; i++) if (dd[i].index == disk) di = i;
    if (di < 0) return st_fail("no such disk", NULL);
    uint32_t ss = dd[di].logical_sector;
    if (!ss) return st_fail("the disk will not say its sector size", NULL);

    static gpt_table t;
    st_rd rd = { disk };
    if (gpt_read_with(st_read, &rd, ss, dd[di].size_bytes, &t) != 0 || !t.valid)
        return st_fail("the disk's partition table could not be read", NULL);
    char g[48];
    guid_str(t.disk_guid, g, sizeof g);
    if (_stricmp(g, want_guid) != 0) {
        snprintf(why, sizeof why, "disk %d is %s, not %s", disk, g, want_guid);
        return st_fail("refusing: not the disk that was named", why);
    }
    int idx = part - 1;
    if (idx < 0 || (uint32_t)idx >= t.n_entries || !gpt_used(&t.ent[idx]) ||
        memcmp(t.ent[idx].type, GPT_TYPE_MSDATA, 16) != 0)
        return st_fail("that partition is not a basic data partition", NULL);
    uint64_t before = (t.ent[idx].last - t.ent[idx].first + 1) * ss;
    printf("gapselftest disk=%d sector=%u guid=%s part=%d first_lba=%llu bytes=%llu\n",
           disk, (unsigned)ss, g, part, (unsigned long long)t.ent[idx].first,
           (unsigned long long)before);

    if (!strcmp(phase, "undo")) {
        uint64_t orig = strtoull(argv[6], NULL, 10), got = 0;
        double t0 = st_now();
        if (plat_grow_system(disk, part, orig, &got, why, sizeof why) != 0)
            return st_fail("plat_grow_system", why);
        double t1 = st_now();
        if (gpt_read_with(st_read, &rd, ss, dd[di].size_bytes, &t) != 0)
            return st_fail("the table could not be read after growing", NULL);
        uint64_t table = (t.ent[idx].last - t.ent[idx].first + 1) * ss;
        printf("gapselftest grow seconds=%.1f reported=%llu table=%llu\n",
               t1 - t0, (unsigned long long)got, (unsigned long long)table);
        if (table != orig || got != orig)
            return st_fail("the partition is not its original size again", NULL);
        printf("gapselftest verdict=ok phase=undo bytes=%llu\n",
               (unsigned long long)table);
        return 0;
    }
    if (strcmp(phase, "write") != 0) return st_fail("no such phase", phase);

    uint64_t free_b = strtoull(argv[6], NULL, 10) << 20;
    uint64_t root_b = (argc > 7 ? strtoull(argv[7], NULL, 10) : 64) << 20;
    const uint64_t boot_b = 8u << 20, rsc_b = 8u << 20;

    uint64_t now = 0, smin = 0, smax = 0;
    double t0 = st_now();
    if (plat_shrink_limits(disk, part, &now, &smin, &smax, why, sizeof why) != 0)
        return st_fail("plat_shrink_limits", why);
    double t1 = st_now();
    printf("gapselftest limits seconds=%.1f now=%llu min=%llu max=%llu\n", t1 - t0,
           (unsigned long long)now, (unsigned long long)smin, (unsigned long long)smax);
    if (now != before) return st_fail("Windows and the table disagree about its size", NULL);

    uint64_t achieved = 0;
    if (plat_shrink_system(disk, part, before - free_b, &achieved, why, sizeof why) != 0)
        return st_fail("plat_shrink_system", why);
    double t2 = st_now();
    if (gpt_read_with(st_read, &rd, ss, dd[di].size_bytes, &t) != 0)
        return st_fail("the table could not be read after the shrink", NULL);
    uint64_t table = (t.ent[idx].last - t.ent[idx].first + 1) * ss;
    printf("gapselftest shrink seconds=%.1f achieved=%llu table=%llu\n", t2 - t1,
           (unsigned long long)achieved, (unsigned long long)table);
    if (table != achieved) return st_fail("the shrink's report and the table disagree", NULL);
    if (achieved >= before) return st_fail("the partition is not smaller", NULL);

    /* THE PLAN, BY THE FUNCTION THE INSTALL USES. */
    stage_layout L;
    if (plan_compute_prepared(&t, idx, root_b, boot_b, rsc_b, root_b, &L,
                              why, sizeof why) != 0)
        return st_fail("plan_compute_prepared", why);
    printf("gapselftest plan win=%llu..%llu root=%llu..%llu saved=%llu..%llu "
           "boot=%llu..%llu\n",
           (unsigned long long)L.win_first, (unsigned long long)L.win_last_new,
           (unsigned long long)L.root_first, (unsigned long long)L.root_last,
           (unsigned long long)L.rsc_first, (unsigned long long)L.rsc_last,
           (unsigned long long)L.rec_first, (unsigned long long)L.rec_last);

    uint64_t lo = L.root_first * ss, hi = (L.rec_last + 1) * ss;
    if (plat_gap_allow(disk, lo, hi, why, sizeof why) != 0)
        return st_fail("plat_gap_allow", why);
    char w1[65], r1[65], w2[65], r2[65];
    if (st_extent(disk, L.root_first * ss, root_b, 1, w1, r1, why, sizeof why) != 0)
        return st_fail("writing the root extent", why);
    if (st_extent(disk, L.rec_first * ss, boot_b, 1u << 20, w2, r2, why, sizeof why) != 0)
        return st_fail("writing the boot extent", why);
    double t3 = st_now();
    printf("gapselftest write seconds=%.1f root_off=%llu root_len=%llu root_sha=%s "
           "boot_off=%llu boot_len=%llu boot_sha=%s\n", t3 - t2,
           (unsigned long long)(L.root_first * ss), (unsigned long long)root_b, w1,
           (unsigned long long)(L.rec_first * ss), (unsigned long long)boot_b, w2);
    if (strcmp(w1, r1) || strcmp(w2, r2))
        return st_fail("what was read back is not what was written", NULL);
    printf("gapselftest readback=ok\n");

    /* FENCE ONE: the engine's own. Each of these must be refused. */
    static uint8_t z[8192];
    uint64_t ntfs_end = (L.win_last_new + 1) * ss;
    const struct { const char *what; uint64_t off; size_t n; } F[] = {
        { "straddling the end of the NTFS partition", ntfs_end - 4096, 8192 },
        { "inside the NTFS partition", L.win_first * ss, 4096 },
        { "just before the gap", lo - 4096, 4096 },
        { "running past the gap", hi - 4096, 8192 },
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        char w[PLAT_WHY] = "";
        if (plat_gap_write(disk, F[i].off, z, F[i].n, w, sizeof w) == 0)
            return st_fail("fence one let a write through", F[i].what);
        printf("gapselftest fence1 refused=\"%s\" why=\"%s\"\n", F[i].what, w);
    }
    plat_gap_forbid();
    {
        char w[PLAT_WHY] = "";
        if (plat_gap_write(disk, lo, z, 4096, w, sizeof w) == 0)
            return st_fail("a write with the gap forbidden was let through", NULL);
        printf("gapselftest fence1 refused=\"with nothing armed\"\n");
    }

    /* FENCE TWO: Windows' own, bypassing fence one. The first sector of
     * the NTFS volume, written back with its own bytes: refused. And a
     * sector in the gap, the same way, as the control that shows the
     * probe can tell the difference. */
    {
        char w[PLAT_WHY] = "";
        int a = plat_gap_os_fence_probe(disk, L.win_first * ss, w, sizeof w);
        printf("gapselftest fence2 ntfs=%d why=\"%s\"\n", a, w);
        if (a != 1) return st_fail("Windows did not refuse a raw write inside NTFS", w);
        int b = plat_gap_os_fence_probe(disk, L.rsc_first * ss, w, sizeof w);
        printf("gapselftest fence2 gap=%d why=\"%s\"\n", b, w);
        if (b != 0) return st_fail("the probe's control (a write into the gap) failed", w);
    }
    printf("gapselftest verdict=ok phase=write orig_bytes=%llu now_bytes=%llu\n",
           (unsigned long long)before, (unsigned long long)achieved);
    return 0;
}

static void usage(void)
{
    fputs(
    "aurbridge — install AurOS alongside Windows\n\n"
    "  aurbridge preflight            check this PC and report (read-only)\n"
    "  aurbridge preflight --json     same, machine-readable for the wizard\n"
    "  aurbridge selftest             check this binary, not this PC\n"
    "  aurbridge fast-startup-off     what an install does first: Fast Startup off\n"
    "  aurbridge fix ID               what the wizard's Fix button does for ID\n"
    "  aurbridge resume-arm|resume-done\n"
    "                                 set up / take away opening again after a restart\n"
    "  aurbridge getimage LIST DEST SHA256 BYTES\n"
    "                                 download AurOS in pieces (files only)\n"
    "  aurbridge gapselftest DISK PART GUID write|undo ...\n"
    "                                 the fast path on a throwaway disk (CI)\n"
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
    if (!strcmp(cmd, "gapselftest")) return gapselftest(argc, argv);
    /* The install's first step, alone, so that a real Windows machine
     * can be asked whether it works (.github/workflows/windows.yml). */
    if (!strcmp(cmd, "fast-startup-off")) {
        char why[400];
        if (plat_fast_startup_off(why, sizeof why) != 0) {
            fprintf(stderr, "aurbridge: %s\n", why);
            return 1;
        }
        puts("aurbridge: Fast Startup and hibernation are off");
        return 0;
    }
    /* The wizard's fixes, one at a time, for the same reason. */
    if (!strcmp(cmd, "fix")) {
        if (argc < 3) { usage(); return 2; }
        const fix_info *f = fix_lookup(argv[2]);
        if (!f || f->kind != FIX_AUTO) {
            fprintf(stderr, "aurbridge: \"%s\" is not something a program fixes\n", argv[2]);
            return 2;
        }
        char why[400] = "";
        if (fix_run(argv[2], why, sizeof why) != 0) {
            fprintf(stderr, "aurbridge: %s\n", why);
            return 1;
        }
        printf("aurbridge: ran the fix for %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(cmd, "resume-arm")) {
        char why[400] = "";
        if (fix_resume_arm(why, sizeof why) != 0) {
            fprintf(stderr, "aurbridge: %s\n", why);
            return 1;
        }
        puts("aurbridge: the installer opens again at the next sign-in");
        return 0;
    }
    if (!strcmp(cmd, "resume-done")) {
        fix_resume_done();
        puts("aurbridge: it will not open again by itself");
        return 0;
    }
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
