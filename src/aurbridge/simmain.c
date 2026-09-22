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

int main(int argc, char **argv)
{
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
    snprintf(c.stick_serial, sizeof c.stick_serial, "%s", argv[3]);
    snprintf(c.image_path, sizeof c.image_path, "%s", argv[4]);
    snprintf(c.kernel_path, sizeof c.kernel_path, "%s", argv[5]);
    snprintf(c.initrd_path, sizeof c.initrd_path, "%s", argv[6]);
    snprintf(c.shell_archetype, sizeof c.shell_archetype, "%s", "familiar");
    snprintf(c.language, sizeof c.language, "%s", "en");
    /* The simulated person agrees. On a real machine nothing but a
     * person sets this, and phase 1 refuses without it -- which
     * ab_selftest checks and this does not, because a test that has to
     * click a button is a test nobody runs. */
    c.consent_given = 1;

    pf_report r;
    pf_run(&r);

    ab_machine m;
    char why[400] = "";
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
