/* simmain.c — run the phase engine against a computer made of files.
 *
 * NOT SHIPPED. It is built only by ./build/aurbridge sim, into a host
 * binary that the end-to-end tests use, and it exists so that the
 * memory stick and the journal the installer is tested with are the
 * ones AurBridge actually produces.
 *
 *   aurbridge-sim <machine-dir> <profile> <stick-serial> \
 *                 <image> <kernel> <initrd>
 *
 * The machine directory is described at the top of plat_sim.c. On
 * success it prints one machine-readable line, which is what the test
 * reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phases.h"
#include "plat.h"
#include "inflate.h"
#include "sbdb.h"
#include "aurbridge-baked.h"

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

/* One thing at a time, for the parts of the engine that do not need a
 * whole machine. The download is the obvious one: it needs a URL and a
 * file and nothing else, and what is worth testing about it -- does a
 * resume resume, does a server that ignores Range get noticed -- is
 * not reachable from a run that also wants a disk and a stick. */
static int one_fetch(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: aurbridge-sim fetch URL DEST\n");
        return 2;
    }
    char why[1200] = "";
    if (plat_fetch(argv[2], argv[3], NULL, NULL, why, sizeof why) != 0) {
        fprintf(stderr, "aurbridge: %s\n", why);
        printf("fetch verdict=failed why=\"%s\"\n", why);
        return 1;
    }
    printf("fetch verdict=ok\n");
    return 0;
}

/* The whole-image path on its own: `getimage PIECES-FILE DEST SHA256
 * BYTES`. The same code the no-stick install runs in phase 2, against a
 * server the test starts. */
static char g_pieces[65536];

static int load_pieces(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t k = fread(g_pieces, 1, sizeof g_pieces - 1, f);
    fclose(f);
    g_pieces[k] = 0;
    return 0;
}

static int one_getimage(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: aurbridge-sim getimage PIECES DEST SHA256 BYTES\n");
        return 2;
    }
    if (load_pieces(argv[2]) != 0) { fprintf(stderr, "no %s\n", argv[2]); return 2; }
    ab_choice c;
    memset(&c, 0, sizeof c);
    c.no_stick = 1;
    c.pieces_text = g_pieces;
    snprintf(c.image_path, sizeof c.image_path, "%s", argv[3]);
    snprintf(c.image_sha256, sizeof c.image_sha256, "%s", argv[4]);
    c.image_expect = strtoull(argv[5], NULL, 10);
    char why[1200] = "";
    if (ab_fetch_image(&c, say, prog, NULL, why, sizeof why) != 0) {
        fprintf(stderr, "aurbridge: %s\n", why);
        printf("getimage verdict=failed why=\"%s\"\n", why);
        return 1;
    }
    printf("getimage verdict=ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "fetch")) return one_fetch(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "getimage")) return one_getimage(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "gzselftest")) {
        int bad = gz_selftest();
        printf("gzselftest %s\n", bad ? "FAILED" : "ok");
        return bad ? 1 : 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "sbselftest")) {
        int bad = sbdb_selftest();
        printf("sbselftest %s\n", bad ? "FAILED" : "ok");
        return bad ? 1 : 0;
    }
    /* `sbdb DB-FILE`: what a real db says, as preflight would judge it,
     * against the key names this build baked in off its shim. */
    if (argc == 3 && !strcmp(argv[1], "sbdb")) {
        static unsigned char db[65536];
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 2; }
        size_t n = fread(db, 1, sizeof db, f);
        fclose(f);
        char seen[1024];
        int t = sbdb_trusts(db, n, AUROS_SHIM_CAS, seen, sizeof seen);
        printf("sbdb trusts=%d wants=\"%s\" seen=\"%s\"\n", t,
               AUROS_SHIM_CAS, seen);
        return t == 1 ? 0 : 1;
    }
    if (argc < 7) {
        fprintf(stderr,
            "usage: aurbridge-sim MACHINE-DIR PROFILE STICK-SERIAL "
            "IMAGE KERNEL INITRD\n");
        return 2;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    setenv("AURBRIDGE_SIM", argv[1], 1);

    ab_choice c;
    memset(&c, 0, sizeof c);
    snprintf(c.profile, sizeof c.profile, "%s", argv[2]);
    /* "none" is the no-stick mode: the image stays where it is (it has
     * to be on the simulated Windows drive for the install to find it,
     * which the test arranges), and its manifest is written beside it. */
    if (!strcmp(argv[3], "none")) c.no_stick = 1;
    else snprintf(c.stick_serial, sizeof c.stick_serial, "%s", argv[3]);
    if (getenv("AURBRIDGE_PIECES") && load_pieces(getenv("AURBRIDGE_PIECES")) == 0)
        c.pieces_text = g_pieces;
    if (getenv("AURBRIDGE_IMAGE_SHA256"))
        snprintf(c.image_sha256, sizeof c.image_sha256, "%s",
                 getenv("AURBRIDGE_IMAGE_SHA256"));
    if (getenv("AURBRIDGE_IMAGE_BYTES"))
        c.image_expect = strtoull(getenv("AURBRIDGE_IMAGE_BYTES"), NULL, 10);
    snprintf(c.image_path, sizeof c.image_path, "%s", argv[4]);
    snprintf(c.kernel_path, sizeof c.kernel_path, "%s", argv[5]);
    snprintf(c.initrd_path, sizeof c.initrd_path, "%s", argv[6]);
    /* The personalize page's answers, from the environment when a test
     * wants to see them arrive: AURBRIDGE_LANGUAGE, _KEYBOARD, _TIMEZONE,
     * _THEME, _SHELL. Empty otherwise, which is a choices.conf with
     * nothing in it -- the image's defaults. */
    {
        const struct { const char *env; char *dst; size_t n; } E[] = {
            { "AURBRIDGE_LANGUAGE", c.language, sizeof c.language },
            { "AURBRIDGE_KEYBOARD", c.keyboard, sizeof c.keyboard },
            { "AURBRIDGE_TIMEZONE", c.timezone, sizeof c.timezone },
            { "AURBRIDGE_THEME",    c.theme,    sizeof c.theme },
            { "AURBRIDGE_SHELL",    c.shell_archetype, sizeof c.shell_archetype },
        };
        for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
            if (getenv(E[i].env))
                snprintf(E[i].dst, E[i].n, "%s", getenv(E[i].env));
    }
    /* The simulated person agrees. On a real machine nothing but a
     * person sets this, and phase 1 refuses without it -- which
     * ab_selftest checks and this does not, because a test that has to
     * click a button is a test nobody runs. */
    c.consent_given = 1;

    pf_report r;
    pf_run(&r);

    ab_machine m;
    char why[1200] = "";
    int rc = ab_run(AB_HANDOFF, &c, &r, &m, say, prog, NULL, why, sizeof why);
    if (rc != 0) {
        fprintf(stderr, "aurbridge: REFUSED: %s\n", why);
        printf("aurbridge-report v1 verdict=refused why=\"%s\"\n", why);
        return 1;
    }
    printf("aurbridge-report v1 verdict=armed disk=%d serial=%s "
           "win_start_lba=%llu win_sectors=%llu sector=%u gpt=%s "
           "stick=%d image_off=%llu record_off=%llu saved_off=%llu "
           "boot=%04X run_id=%llu\n",
           m.disk_index, m.disk_serial,
           (unsigned long long)m.win_start_lba,
           (unsigned long long)m.win_sectors,
           (unsigned)m.logical_sector, m.gpt_sha256,
           m.stick_index,
           (unsigned long long)m.image_part_off,
           (unsigned long long)m.record_part_off,
           (unsigned long long)m.saved_part_off,
           m.boot_entry, (unsigned long long)m.run_id);
    return 0;
}
