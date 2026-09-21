/* power.c — see power.h. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "power.h"
#include "run.h"

/* Overridable so tools/powertest.c can hand this a tree of files that
 * looks like a laptop, a desktop, a machine on the mains and a machine
 * about to die -- none of which the build host is. Not a test hook
 * bolted on: it is also how a technician points the shell at a second
 * battery or an unusual backlight without rebuilding it. */
static const char *sysdir(const char *var, const char *def)
{
    const char *v = getenv(var);
    return (v && *v) ? v : def;
}
#define POWER_SUPPLY sysdir("AUROS_POWER_SUPPLY", "/sys/class/power_supply")
#define BACKLIGHT    sysdir("AUROS_BACKLIGHT",    "/sys/class/backlight")

/* ── reading one small file ─────────────────────────────────────── */

static int slurp(const char *dir, const char *leaf, char *out, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    size_t len = strlen(out);
    while (len && (out[len-1] == '\n' || out[len-1] == '\r' ||
                   out[len-1] == ' '  || out[len-1] == '\t')) out[--len] = 0;
    return (int)len;
}

static long slurp_long(const char *dir, const char *leaf, long fallback)
{
    char buf[64];
    if (slurp(dir, leaf, buf, sizeof buf) < 0) return fallback;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf) return fallback;
    return v;
}

/* ── the battery ────────────────────────────────────────────────── */

void power_battery_read(power_battery *out)
{
    memset(out, 0, sizeof *out);
    out->percent = -1;
    out->minutes = -1;

    const char *root = POWER_SUPPLY;
    DIR *d = opendir(root);
    if (!d) return;                      /* no power supplies at all */

    /* Charge and current across every battery, because a laptop with
     * two of them has two of everything and one number to show. */
    long charge_now = 0, charge_full = 0, rate = 0;
    int have_charge = 0;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[512];
        snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);

        char type[32];
        if (slurp(dir, "type", type, sizeof type) < 0) continue;

        if (!strcmp(type, "Mains") || !strcmp(type, "USB")) {
            if (slurp_long(dir, "online", 0)) out->plugged = 1;
            continue;
        }
        if (strcmp(type, "Battery")) continue;

        /* A battery the kernel knows about but that is not fitted
         * reports zero everywhere; treating that as a flat battery
         * would put an alarming number on the screen of a machine
         * that has none. */
        if (slurp_long(dir, "present", 1) == 0) continue;
        out->present = 1;

        long pct = slurp_long(dir, "capacity", -1);
        if (pct >= 0) {
            if (out->percent < 0) out->percent = 0;
            /* Several batteries: the honest single number is how full
             * they are together, which needs charge rather than
             * percentage. Percentage is the fallback. */
            out->percent = (int)pct;
        }

        char st[32];
        if (slurp(dir, "status", st, sizeof st) >= 0) {
            if (!strcmp(st, "Charging")) out->charging = 1;
            if (!strcmp(st, "Charging") || !strcmp(st, "Full")) out->plugged = 1;
        }

        /* charge_* is in uAh, energy_* in uWh. Either works for a
         * ratio and for a time estimate as long as both halves come
         * from the same pair. */
        long now  = slurp_long(dir, "charge_now",  -1);
        long full = slurp_long(dir, "charge_full", -1);
        long cur  = slurp_long(dir, "current_now", -1);
        if (now < 0 || full < 0) {
            now  = slurp_long(dir, "energy_now",  -1);
            full = slurp_long(dir, "energy_full", -1);
            cur  = slurp_long(dir, "power_now",   -1);
        }
        if (now >= 0 && full > 0) {
            charge_now += now; charge_full += full; have_charge = 1;
            if (cur > 0) rate += cur;
        }
    }
    closedir(d);

    if (have_charge && charge_full > 0)
        out->percent = (int)((charge_now * 100 + charge_full / 2) / charge_full);
    if (out->percent > 100) out->percent = 100;
    if (out->present && out->percent < 0) out->percent = -1;

    /* How long is left, only when the machine is actually discharging
     * and actually says how fast. A number invented from nothing is
     * worse than no number: she will plan around it. */
    if (have_charge && rate > 0 && !out->charging && !out->plugged)
        out->minutes = (int)((charge_now * 60) / rate);
}

/* ── the backlight ──────────────────────────────────────────────── */

/* Which of them is the screen.
 *
 * A laptop often exposes two or three: the real panel controller
 * (intel_backlight, amdgpu_bl0, nv_backlight) and acpi_video0, which
 * is the firmware's coarse seven-step version of the same thing and on
 * many machines does nothing at all. Preferring the wrong one is how a
 * brightness control ends up moving a slider and changing nothing. */
static int backlight_dir(char *out, size_t n)
{
    const char *root = BACKLIGHT;
    DIR *d = opendir(root);
    if (!d) return -1;
    char best[256] = {0};
    int best_rank = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        /* raw panel control > platform/firmware > the ACPI fallback */
        int rank = 1;
        char dir[512];
        snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
        char type[32];
        if (slurp(dir, "type", type, sizeof type) >= 0) {
            if (!strcmp(type, "raw"))      rank = 3;
            else if (!strcmp(type, "platform")) rank = 2;
            else if (!strcmp(type, "firmware")) rank = 1;
        }
        if (!strncmp(e->d_name, "acpi_video", 10)) rank = 0;
        if (rank > best_rank) {
            best_rank = rank;
            snprintf(best, sizeof best, "%s", e->d_name);
        }
    }
    closedir(d);
    if (best_rank < 0) return -1;
    snprintf(out, n, "%s/%s", root, best);
    return 0;
}

/* Never all the way off. Zero is a black screen, and a person who
 * reaches it cannot see the control that would undo it -- which makes
 * it a state she can enter and not leave. */
#define BRIGHT_FLOOR 5

int power_brightness(void)
{
    char dir[512];
    if (backlight_dir(dir, sizeof dir) < 0) return -1;
    long max = slurp_long(dir, "max_brightness", -1);
    long now = slurp_long(dir, "brightness", -1);
    if (max <= 0 || now < 0) return -1;
    return (int)((now * 100 + max / 2) / max);
}

int power_brightness_set(int percent)
{
    char dir[512];
    if (backlight_dir(dir, sizeof dir) < 0) return -1;
    long max = slurp_long(dir, "max_brightness", -1);
    if (max <= 0) return -1;

    if (percent < BRIGHT_FLOOR) percent = BRIGHT_FLOOR;
    if (percent > 100) percent = 100;
    long want = (max * percent + 50) / 100;
    if (want < 1) want = 1;

    char path[576];
    snprintf(path, sizeof path, "%s/brightness", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        /* The file is root's unless a rule has been laid down for the
         * video group. build/forge writes one; say which, because the
         * symptom otherwise is a control that moves and does nothing. */
        static int moaned = 0;
        if (!moaned++)
            fprintf(stderr, "aurshell: cannot write %s — is the udev rule "
                            "for the video group installed?\n", path);
        return -1;
    }
    fprintf(f, "%ld\n", want);
    fclose(f);
    return percent;
}

/* ── the sound ──────────────────────────────────────────────────── */

/* What we believe, so that a key press can redraw immediately instead
 * of after a program has been started and answered. The machine is
 * asked at start-up and whenever a panel opens; in between, the only
 * thing changing it is us. */
static int vol_pct = -1;
static int vol_muted = 0;

#define SINK "@DEFAULT_AUDIO_SINK@"

static void volume_ask(void)
{
    char out[128];
    const char *argv[] = { "wpctl", "get-volume", SINK, NULL };
    if (run_capture(argv, out, sizeof out, 700) <= 0) { vol_pct = -1; return; }
    /* "Volume: 0.43" or "Volume: 0.43 [MUTED]" */
    const char *p = strstr(out, "Volume:");
    if (!p) { vol_pct = -1; return; }
    double v = atof(p + 7);
    if (v < 0) v = 0;
    if (v > 1.5) v = 1.5;
    vol_pct = (int)(v * 100.0 + 0.5);
    if (vol_pct > 100) vol_pct = 100;
    vol_muted = strstr(out, "MUTED") != NULL;
}

void power_refresh(void) { volume_ask(); }

int power_volume(void)
{
    if (vol_pct < 0) volume_ask();
    return vol_pct;
}

int power_muted(void) { return vol_muted; }

void power_volume_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char arg[32];
    snprintf(arg, sizeof arg, "%d%%", percent);
    const char *argv[] = { "wpctl", "set-volume", SINK, arg, NULL };
    if (run_detached(argv) == 0) {
        vol_pct = percent;
        /* Turning it up past nothing is how a person unmutes, whatever
         * the mute flag says. Leaving it muted here means she presses
         * the loud key four times in silence. */
        if (percent > 0 && vol_muted) power_mute_set(0);
    }
}

void power_mute_set(int muted)
{
    const char *argv[] = { "wpctl", "set-mute", SINK, muted ? "1" : "0", NULL };
    if (run_detached(argv) == 0) vol_muted = muted ? 1 : 0;
}
