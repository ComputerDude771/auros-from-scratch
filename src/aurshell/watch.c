/* watch.c — see watch.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "watch.h"
#include "power.h"
#include "osd.h"
#include "notify.h"

/* Which warnings have already been given on this discharge. Cleared
 * the moment a cable goes in, so unplugging again gets the full
 * sequence rather than silence. */
static struct {
    uint32_t next_ms;
    int said_low, said_very, said_critical;
    int was_plugged;
} W;

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

int watch_tick(shell_ctx *c)
{
    if ((int32_t)(now_ms() - W.next_ms) < 0) return 0;
    W.next_ms = now_ms() + 1000;

    power_battery b;
    power_battery_read(&b);
    if (!b.present || b.percent < 0) return 0;

    if (b.plugged || b.charging) {
        /* A cable went in. Everything is forgiven and the sequence
         * starts again next time it comes out. */
        if (!W.was_plugged) {
            W.said_low = W.said_very = W.said_critical = 0;
            W.was_plugged = 1;
        }
        return 0;
    }
    W.was_plugged = 0;

    /* Most urgent first, so a machine that wakes up at 2% does not
     * work its way down through three messages before acting. */
    if (b.percent <= WATCH_CRITICAL) {
        if (W.said_critical) return 0;
        W.said_critical = W.said_very = W.said_low = 1;
        /* BOTH, on purpose. The indicator because it is in the middle
         * of the screen and the machine is about to stop; the card
         * because the machine is about to stop, and what she needs to
         * read is the sentence explaining why it did -- which she will
         * be reading AFTER she plugs it in and wakes it up. A message
         * that lasts three seconds and then the computer sleeps is a
         * computer that turned itself off for no reason she saw. */
        osd_say("The battery is empty. Saving what you have and going to sleep.");
        notify_local("This computer went to sleep",
                     "The battery was empty. It is safe to plug it in and "
                     "press a key; everything you had open is still here.");
        /* Sleep, not shut down. Everything she has open comes back
         * when the cable goes in; a shutdown at this point is the
         * machine deciding to close her work for her. */
        c->want_power_off = 3;
        return 1;
    }
    if (b.percent <= WATCH_VERY_LOW) {
        if (W.said_very) return 0;
        W.said_very = W.said_low = 1;
        /* A card, not a flash. This is a thing she has to ACT on, and
         * docs/EASY.md rule 5 is about exactly that: the indicator
         * goes away by itself after three seconds, and a person who
         * was looking at the keyboard would never have known. This
         * one stays until she presses it. */
        notify_local("The battery is very low",
                     "Please plug this computer in. It will go to sleep on "
                     "its own if the battery runs out.");
        return 1;
    }
    if (b.percent <= WATCH_LOW) {
        if (W.said_low) return 0;
        W.said_low = 1;
        notify_local("The battery is getting low",
                     "There is still plenty of time. Plug this computer in "
                     "when it suits you.");
        return 1;
    }
    /* Back above the line -- a battery that recovers a little, or a
     * reading that was briefly wrong -- so the warnings re-arm. */
    if (b.percent > WATCH_LOW + 5) {
        W.said_low = W.said_very = W.said_critical = 0;
    }
    return 0;
}
