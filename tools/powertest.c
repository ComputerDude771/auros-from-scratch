/* powertest.c — does this computer know its own battery and screen?
 *
 * Every laptop this product exists to rescue has a battery and a
 * backlight. No build host does. So the machines are made out of
 * directories: the kernel publishes all of it as small files, and a
 * tree of small files is a laptop as far as this code is concerned.
 *
 * The cases are the ones that are wrong on real hardware:
 *
 *   two batteries        a laptop with a second cell reports two of
 *                        everything, and one number has to come out
 *   full on the mains    "plugged in" and "charging" are not the same
 *                        sentence, and a full battery that says it is
 *                        charging is a small lie she will notice
 *   a battery bay with   present=0 reports zero everywhere, which read
 *   nothing in it        as a flat battery would put 0% on the screen
 *                        of a machine that has none
 *   acpi_video0          laptops expose a real panel control AND the
 *                        firmware's seven-step version of it, and on
 *                        many machines the firmware one does nothing.
 *                        Picking it is how a brightness slider moves
 *                        and changes nothing.
 *   a panel that counts  one counts to 7 and another to 96000, so the
 *   to 7                 control cannot be built on the raw number
 *
 *   cc -O2 -std=gnu11 -o /tmp/powertest tools/powertest.c \
 *      src/aurshell/power.c src/aurshell/run.c
 *   /tmp/powertest
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/aurshell/power.h"

static int fail = 0, checked = 0;
static char root[256];

static void ok(const char *what, int cond)
{
    checked++;
    printf("    %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fail++;
}

static void put(const char *dir, const char *leaf, const char *val)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, leaf);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fprintf(f, "%s\n", val);
    fclose(f);
}

/* Build one device directory and return its path. */
static const char *dev(const char *kind, const char *name)
{
    static char p[512];
    snprintf(p, sizeof p, "%s/%s/%s", root, kind, name);
    char cmd[640];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", p);
    if (system(cmd) != 0) exit(2);
    return p;
}

static void clean(const char *kind)
{
    char cmd[640];
    snprintf(cmd, sizeof cmd, "rm -rf '%s/%s' && mkdir -p '%s/%s'",
             root, kind, root, kind);
    if (system(cmd) != 0) exit(2);
}

int main(void)
{
    char tmpl[] = "/tmp/powertest-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(root, sizeof root, "%s", tmpl);

    char ps[300], bl[300];
    snprintf(ps, sizeof ps, "%s/power_supply", root);
    snprintf(bl, sizeof bl, "%s/backlight", root);
    setenv("AUROS_POWER_SUPPLY", ps, 1);
    setenv("AUROS_BACKLIGHT", bl, 1);

    power_battery b;
    printf("does this computer know its own battery and screen?\n\n");

    /* ── a desktop: no battery, no backlight ─────────────────────── */
    printf("  a desktop computer\n");
    clean("power_supply"); clean("backlight");
    power_battery_read(&b);
    ok("has no battery, and says so rather than 0%", !b.present);
    ok("its percentage is 'unknown', not zero", b.percent == -1);
    ok("it has no screen brightness to change", power_brightness() == -1);
    ok("and setting it fails rather than pretending",
       power_brightness_set(50) == -1);

    /* ── an ordinary laptop, unplugged ───────────────────────────── */
    printf("\n  a laptop running on its battery\n");
    clean("power_supply");
    {
        const char *d = dev("power_supply", "BAT0");
        put(d, "type", "Battery"); put(d, "present", "1");
        put(d, "capacity", "62");  put(d, "status", "Discharging");
        put(d, "charge_now", "3100000"); put(d, "charge_full", "5000000");
        put(d, "current_now", "1550000");
        const char *a = dev("power_supply", "AC");
        put(a, "type", "Mains"); put(a, "online", "0");
    }
    power_battery_read(&b);
    ok("there is a battery", b.present);
    ok("it is 62% full", b.percent == 62);
    ok("nothing is plugged in", !b.plugged && !b.charging);
    ok("about two hours left", b.minutes >= 115 && b.minutes <= 125);

    /* ── the same laptop, full, on the mains ─────────────────────── */
    printf("\n  the same laptop, full, plugged in\n");
    clean("power_supply");
    {
        const char *d = dev("power_supply", "BAT0");
        put(d, "type", "Battery"); put(d, "present", "1");
        put(d, "capacity", "100"); put(d, "status", "Full");
        put(d, "charge_now", "5000000"); put(d, "charge_full", "5000000");
        const char *a = dev("power_supply", "AC");
        put(a, "type", "Mains"); put(a, "online", "1");
    }
    power_battery_read(&b);
    ok("it is plugged in", b.plugged);
    ok("and NOT described as charging", !b.charging);
    ok("no time estimate is invented", b.minutes == -1);

    /* ── two batteries ───────────────────────────────────────────── */
    printf("\n  a laptop with two batteries\n");
    clean("power_supply");
    {
        const char *d0 = dev("power_supply", "BAT0");
        put(d0, "type", "Battery"); put(d0, "present", "1");
        put(d0, "capacity", "100"); put(d0, "status", "Discharging");
        put(d0, "charge_now", "4000000"); put(d0, "charge_full", "4000000");
        const char *d1 = dev("power_supply", "BAT1");
        put(d1, "type", "Battery"); put(d1, "present", "1");
        put(d1, "capacity", "0");   put(d1, "status", "Discharging");
        put(d1, "charge_now", "0"); put(d1, "charge_full", "4000000");
    }
    power_battery_read(&b);
    ok("one full and one empty reads as half", b.percent == 50);

    /* ── an empty battery bay ────────────────────────────────────── */
    printf("\n  a laptop with the battery taken out\n");
    clean("power_supply");
    {
        const char *d = dev("power_supply", "BAT0");
        put(d, "type", "Battery"); put(d, "present", "0");
        put(d, "capacity", "0");   put(d, "status", "Unknown");
        const char *a = dev("power_supply", "AC");
        put(a, "type", "Mains"); put(a, "online", "1");
    }
    power_battery_read(&b);
    ok("it is not reported as a flat battery", !b.present);
    ok("but the machine still knows it is plugged in", b.plugged);

    /* ── the backlight ───────────────────────────────────────────── */
    printf("\n  the screen\n");
    clean("backlight");
    {
        const char *raw = dev("backlight", "intel_backlight");
        put(raw, "type", "raw");
        put(raw, "max_brightness", "96000");
        put(raw, "brightness", "48000");
        const char *fw = dev("backlight", "acpi_video0");
        put(fw, "type", "firmware");
        put(fw, "max_brightness", "7");
        put(fw, "brightness", "7");
    }
    ok("the real panel control is used, not acpi_video0",
       power_brightness() == 50);
    ok("setting 80% reports 80%", power_brightness_set(80) == 80);
    ok("and reading it back agrees", power_brightness() == 80);
    ok("it will not go all the way dark", power_brightness_set(0) > 0);
    ok("a dark screen is still readable back", power_brightness() > 0);

    /* A panel whose whole range is seven steps. */
    printf("\n  a screen with only seven steps\n");
    clean("backlight");
    {
        const char *d = dev("backlight", "acpi_video0");
        put(d, "type", "firmware");
        put(d, "max_brightness", "7");
        put(d, "brightness", "4");
    }
    ok("it is still a percentage", power_brightness() == 57);
    ok("setting 100% works", power_brightness_set(100) == 100);
    ok("and lands on the top step", power_brightness() == 100);

    char cmd[320];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) { /* a leftover temp directory is not a failure */ }

    printf("\n");
    if (fail) {
        printf("%d of %d wrong. The machine is telling her something\n",
               fail, checked);
        printf("about her own laptop that is not true.\n");
        return 1;
    }
    printf("%d checks: the battery and the screen read correctly on\n", checked);
    printf("machines this build host is not.\n");
    return 0;
}
