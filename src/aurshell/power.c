/* power.c — see power.h. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

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

/* How far one press of the key moves it. A PERCENTAGE OF THE RANGE,
 * not a percentage point, because those are the same thing only on a
 * panel whose range happens to be 100.
 *
 * The first version added 5 to the percentage and wrote it back. On a
 * panel that counts to 9 -- and plenty do -- 56%+5% rounds to the step
 * it started on, so the key was dead while the indicator counted
 * cheerfully upward. A control that shows a number going up while
 * nothing changes is worse than no control: it tells her the machine
 * is broken in a way she cannot describe. */
#define BRIGHT_STEP 10

/* The one place that opens the panel and reads its two numbers, so no
 * caller can hold a range and a value that came from different reads
 * (a screen can be hot-plugged between them). */
static int bright_read(char *dir, size_t n, long *max, long *now)
{
    if (backlight_dir(dir, n) < 0) return -1;
    *max = slurp_long(dir, "max_brightness", -1);
    *now = slurp_long(dir, "brightness", -1);
    /* A panel whose only settings are OFF and ON is not a brightness
     * control, and saying it is one is a lie with a number on it: the
     * floor forbids off, so the only reachable value is on, and both
     * keys used to report "100%" over and over while nothing moved.
     * "This computer has no brightness to change" is the true
     * sentence, and it is the one the rest of this file is built to
     * tell. */
    if (*max <= 1 || *now < 0) return -1;
    return 0;
}

static int bright_write(const char *dir, long max, long want)
{
    /* The floor in the panel's own units, so a coarse panel gets at
     * least one step of light rather than being rounded to black. */
    long lo = (max * BRIGHT_FLOOR + 50) / 100;
    if (lo < 1) lo = 1;
    if (want < lo)  want = lo;
    if (want > max) want = max;

    char path[576];
    snprintf(path, sizeof path, "%s/brightness", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        /* The file is root's unless a rule has been laid down for the
         * video group. rootfs ships one; say which, because the
         * symptom otherwise is a control that moves and does nothing. */
        static int moaned = 0;
        if (!moaned++)
            fprintf(stderr, "aurshell: cannot write %s — is "
                            "60-auros-backlight.rules installed and has "
                            "udev replayed it?\n", path);
        return -1;
    }
    fprintf(f, "%ld\n", want);
    if (fclose(f) != 0) return -1;
    /* What was ACHIEVED, read back out of the same arithmetic the
     * getter uses. Returning what was asked for is how the indicator
     * came to disagree with the screen. */
    return (int)((want * 100 + max / 2) / max);
}

int power_brightness_set(int percent)
{
    char dir[512]; long max, now;
    if (bright_read(dir, sizeof dir, &max, &now) < 0) return -1;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    return bright_write(dir, max, (max * percent + 50) / 100);
}

int power_brightness_step(int dir_sign)
{
    char dir[512]; long max, now;
    if (bright_read(dir, sizeof dir, &max, &now) < 0) return -1;
    long step = (max * BRIGHT_STEP + 50) / 100;
    if (step < 1) step = 1;            /* never nothing */
    return bright_write(dir, max, now + (dir_sign >= 0 ? step : -step));
}

/* ── the sound ──────────────────────────────────────────────────── */

/* What we believe, so that a key press can redraw immediately instead
 * of after a program has been started and answered. The machine is
 * asked at start-up and whenever a panel opens; in between, the only
 * thing changing it is us. */
static int vol_pct = -1;
static int vol_muted = 0;

/* "NOT YET" AND "THIS COMPUTER HAS NO SOUND" ARE NOT THE SAME SENTENCE.
 *
 * The first version collapsed them: one failed probe set a "we asked,
 * there is none" flag and nothing ever asked again, for the life of
 * the boot. The sound row vanished from Settings and both volume keys
 * went dead -- on a perfectly healthy machine, because the shell and
 * the sound server start at the same moment and nothing orders one
 * after the other, or because a cold `wpctl` on 2013 hardware took
 * longer than the deadline once.
 *
 * The fact is the socket, not the program. It is either in the
 * runtime directory or it is not, which costs a stat() rather than a
 * process, and which answers the stall the cache was invented to
 * avoid without answering it WRONGLY. A missing socket only becomes
 * "this machine has no sound" after it has stayed missing past the
 * grace period below -- long enough for a slow boot, short enough
 * that she is still looking at the same screen. */
#define VOL_GRACE_MS 25000
/* How often to look again while there is still no answer. */
#define VOL_RETRY_MS  1000

static int   vol_have = 0;     /* a real number has been read, once   */
static int   vol_gone = 0;     /* there is no sound server            */
static long  vol_t0   = 0;     /* when we first went looking, or 0    */
static long  vol_asked_ms = 0; /* when we last looked                 */

#define SINK "@DEFAULT_AUDIO_SINK@"

static long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int sound_socket(void)
{
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) return 0;
    char p[320];
    struct stat st;
    snprintf(p, sizeof p, "%s/pipewire-0", rd);
    if (stat(p, &st) == 0) return 1;
    /* A machine running plain PulseAudio instead. wpctl will not talk
     * to it, but pactl might, and either way this is not a machine
     * with no sound -- so do not latch. */
    snprintf(p, sizeof p, "%s/pulse/native", rd);
    return stat(p, &st) == 0;
}

static void volume_ask(void)
{
    /* THE CLOCK HERE IS MONOTONIC, NOT time().
     *
     * The grace period below is measured from the earliest moment in
     * the boot, which is exactly when the wall clock is least
     * trustworthy: an image built on a fixed date, a laptop with a
     * dead RTC, and timesyncd stepping the clock forward by years a
     * second or two in. One forward step consumed the whole grace in
     * an instant and latched "this computer has no sound" on a machine
     * whose sound was about to come up.
     *
     * And the latch is no longer permanent. A socket that turns up
     * after we gave up is a sound server that started late, which is
     * an ordinary thing on slow hardware, not a reason to stay silent
     * until the next reboot. */
    long t = mono_ms();
    if (!vol_t0) vol_t0 = t;
    vol_asked_ms = t;

    if (!sound_socket()) {
        if (!vol_have && t - vol_t0 > VOL_GRACE_MS) {
            vol_gone = 1;
            vol_pct  = -1;
        }
        return;                       /* no exec, no stall, no lie */
    }
    if (vol_gone) {
        /* It came back. */
        vol_gone = 0;
        fprintf(stderr, "aurshell: the sound server is here after all\n");
    }

    char out[128];
    const char *argv[] = { "wpctl", "get-volume", SINK, NULL };
    /* Short. This is on the path of opening a panel, and a person who
     * pressed Settings is watching the screen. A miss is not an
     * answer, so nothing is recorded and the next refresh asks again. */
    if (run_capture(argv, out, sizeof out, 400) <= 0) return;
    /* "Volume: 0.43" or "Volume: 0.43 [MUTED]" */
    const char *p = strstr(out, "Volume:");
    if (!p) return;
    double v = atof(p + 7);
    if (v < 0) v = 0;
    if (v > 1.5) v = 1.5;
    vol_pct = (int)(v * 100.0 + 0.5);
    if (vol_pct > 100) vol_pct = 100;
    vol_muted = strstr(out, "MUTED") != NULL;
    vol_have  = 1;
}

/* ONE press of a key is one change. ONE DRAG is not two hundred.
 *
 * Dragging the volume slider called set() on every pointer motion --
 * about 125 times a second on a normal touchpad, each one a fork and
 * an exec of wpctl. That is the whole reap table filled in an eighth
 * of a second, then a kill on every further step, on the hardware
 * this product exists to rescue.
 *
 * So: what she asked for is believed and drawn AT ONCE, and the sound
 * server is told at most this often, plus once more when she lets go.
 * The number she stops on is always the number that gets sent. */
#define VOL_SEND_MS 60

static int  vol_want = -1;        /* asked for, not yet sent          */
static long vol_sent_ms = 0;

static void volume_send(int percent)
{
    char arg[32];
    snprintf(arg, sizeof arg, "%d%%", percent);
    const char *argv[] = { "wpctl", "set-volume", SINK, arg, NULL };
    if (run_detached(argv) == 0) vol_sent_ms = mono_ms();
}

/* Called once per pass of the main loop. Two jobs.
 *
 * ONE: keep asking until there is an answer. power_refresh() runs at
 * start-up -- BEFORE the compositor exists, so before the logind
 * session exists, so before the sound server has been started by it --
 * and then only when a panel opens. Removing the "asked once, never
 * again" flag was not enough on its own: with nothing in the frame
 * loop asking, the first probe was still the only probe, and the
 * volume keys stayed dead for the life of the boot on a machine whose
 * sound came up two seconds later. The fix for a stuck answer is to
 * ask again, which means something has to be doing the asking.
 *
 * TWO: flush whatever volume she landed on, so letting go of the
 * slider is always heard even if the last motion arrived inside the
 * throttle window. */
void power_step(void)
{
    if (!vol_have && !vol_gone) {
        long t = mono_ms();
        if (!vol_asked_ms || t - vol_asked_ms >= VOL_RETRY_MS) volume_ask();
    }
    if (vol_want < 0) return;
    if (mono_ms() - vol_sent_ms < VOL_SEND_MS) return;
    int v = vol_want;
    vol_want = -1;
    volume_send(v);
}

/* Ask the machine again. Cheap on a machine with no sound (a stat),
 * one short program start on a machine with sound. */
void power_refresh(void)
{
    volume_ask();
}

int power_volume(void)
{
    /* A pure reader. power_step() does the asking, every pass, until
     * there is an answer -- this used to try to be clever about asking
     * lazily and the condition it tested (`!vol_t0`) was false from
     * the first start-up probe onwards, so it never fired again. */
    return vol_pct;
}

int power_muted(void) { return vol_muted; }

void power_volume_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    vol_pct  = percent;               /* believed at once, drawn at once */
    vol_want = percent;
    if (mono_ms() - vol_sent_ms >= VOL_SEND_MS) {
        vol_want = -1;
        volume_send(percent);
    }
    /* Turning it up past nothing is how a person unmutes, whatever
     * the mute flag says. Leaving it muted here means she presses
     * the loud key four times in silence. */
    if (percent > 0 && vol_muted) power_mute_set(0);
}

void power_mute_set(int muted)
{
    const char *argv[] = { "wpctl", "set-mute", SINK, muted ? "1" : "0", NULL };
    if (run_detached(argv) == 0) vol_muted = muted ? 1 : 0;
}
