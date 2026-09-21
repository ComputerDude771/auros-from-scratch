/* power.h — the battery, the backlight, and how loud it is.
 *
 * The three things a laptop cannot be used without, and the three this
 * product had no answer for at all. Not a missing button: a missing
 * everything. There was no package, no code and no pixel between a
 * person and the brightness of the screen they were looking at.
 *
 * WHY THIS READS /sys DIRECTLY
 *
 * Because the kernel already publishes all of it as files. upower is
 * a daemon, a D-Bus connection and a dependency to learn the same two
 * numbers that are sitting in /sys/class/power_supply, and it can be
 * absent or not running. A file that is either there or not is a
 * failure mode with one case in it.
 *
 * Sound is the exception and goes through wpctl, because there is no
 * file for it: the volume a person means is the one the sound server
 * applies, not the hardware mixer underneath it, and only the sound
 * server knows that number.
 *
 * EVERYTHING HERE IS ALLOWED TO BE ABSENT
 *
 * A desktop computer has no battery and usually no backlight. A
 * machine with no sound card has no volume. Every reader below says so
 * plainly rather than returning a zero that reads like "empty" or
 * "silent", because "this computer has no battery" and "this battery
 * is flat" are not the same sentence and must never be shown as one.
 */
#ifndef AUROS_POWER_H
#define AUROS_POWER_H

/* ── the battery ────────────────────────────────────────────────── */

typedef struct {
    int present;     /* there is a battery in this computer at all   */
    int percent;     /* 0..100, or -1 if the kernel will not say     */
    int plugged;     /* a cable is connected                         */
    int charging;    /* connected AND filling, which is not the same:
                      * a full battery on the mains is plugged and
                      * not charging, and saying "charging" there is
                      * a small lie she will notice                  */
    int minutes;     /* what is left, or -1 when it cannot be known  */
} power_battery;

void power_battery_read(power_battery *out);

/* ── the backlight ──────────────────────────────────────────────── */

/* A percentage, never the kernel's raw number: one panel counts to 7
 * and another to 96000, and a control built on the raw value is a
 * control that behaves differently on every machine.
 *
 * -1 means this computer has no backlight to change, which is the
 * normal answer on a desktop and on most machines with an external
 * screen. */
int  power_brightness(void);
/* Clamped to a floor above zero. Zero is a black screen, and a person
 * who reaches it has no way to see the control that would undo it. */
int  power_brightness_set(int percent);

/* ── the sound ──────────────────────────────────────────────────── */

/* 0..100, or -1 when there is no sound server to ask. Reading costs a
 * program start, so it is done when something is about to be drawn
 * with it, not every frame. */
int  power_volume(void);
int  power_muted(void);
/* These do not wait. The number they were given is remembered, so the
 * screen can show the new value immediately rather than after the
 * sound server has been asked what it thinks. */
void power_volume_set(int percent);
void power_mute_set(int muted);

/* Ask the machine again, rather than trusting what we last set. Called
 * when a panel opens. */
void power_refresh(void);

#endif
