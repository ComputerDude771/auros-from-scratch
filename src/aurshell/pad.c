#include "pad.h"
#include <string.h>

/* ── classification ──────────────────────────────────────────────────
 *
 * Three tests, in this order, each earning its place:
 *
 *   BTN_TOOL_FINGER  -> a touchpad. This is the test, not the
 *       INPUT_PROP_POINTER property: plenty of pads predate the
 *       property bits, and plenty of absolute pointing devices that are
 *       NOT pads (a VM's tablet, a graphics tablet) also set POINTER.
 *       Classifying a pad as an absolute device makes the pointer
 *       teleport to wherever on the pad the finger landed, and makes
 *       every finger lift a click.
 *   INPUT_PROP_DIRECT -> a touchscreen. The finger is the pointer, so
 *       absolute mapping is right and BTN_TOUCH really is a click.
 *   anything else with absolute axes -> an absolute pointer. Map it
 *       straight to the screen; BTN_TOUCH is not a click.
 *
 * The result is a bitmask, not an enum, because a device is not one
 * thing: a wireless keyboard with a built-in trackpad is a single event
 * node that is both, and as an enum it classifies as a pointer and
 * drops every keystroke. */
int pad_classify(const dev_caps *d)
{
    int kind = 0;
    if (d->has_rel_xy) kind |= DEV_REL;
    if (d->has_abs_xy) {
        if (d->has_btn_finger)   kind |= DEV_PAD;
        else if (d->prop_direct) kind |= DEV_ABS | DEV_DIRECT;
        else                     kind |= DEV_ABS;
    }
    if (d->letter_keys > 5) kind |= DEV_KBD;
    return kind;
}

/* ── touchpad tracking ───────────────────────────────────────────────
 *
 * A pad reports where the finger IS. The user means how far it MOVED.
 * Differentiating the two is the whole job, and three things make it
 * more than a subtraction:
 *
 *   - The first sample after a finger lands is not a movement. Using it
 *     as one flings the pointer across the screen every time the user
 *     puts their finger down somewhere new.
 *   - A second finger landing makes ABS_X jump to it. That is a scroll
 *     gesture, not motion. We do not handle scrolling yet, and doing
 *     nothing is much better than doing something wrong.
 *   - A tap is a click. Every laptop user expects it, and a pad whose
 *     only click is the physical hinge feels broken. The test is the
 *     conventional one: down and up inside 180 ms having travelled
 *     almost nowhere.
 */
#define PAD_TAP_MS     180
#define PAD_TAP_SLOP    14     /* pad units, not pixels                */
#define PAD_SPEED_NUM    3     /* pointer travel per unit of finger    */
#define PAD_SPEED_DEN    2

void pad_reset(pad_state *p, int lo_x, int hi_x, int lo_y, int hi_y)
{
    memset(p, 0, sizeof *p);
    p->lo_x = lo_x; p->hi_x = hi_x;
    p->lo_y = lo_y; p->hi_y = hi_y;
}

int pad_delta(pad_state *p, int axis, int value)
{
    if (p->fingers > 1) return 0;            /* a gesture, not motion  */

    int prev = axis ? p->y : p->x;
    if (axis) p->y = value; else p->x = value;
    if (!p->have) return 0;                  /* a place, not a movement */

    int d = value - prev;

    /* A jump of more than a quarter of the pad is a finger re-seating
     * or a second finger being picked up, not a hand moving that fast. */
    int lo = axis ? p->lo_y : p->lo_x;
    int hi = axis ? p->hi_y : p->hi_x;
    if (hi > lo && (d > (hi - lo) / 4 || d < -(hi - lo) / 4)) return 0;

    if (axis) p->travel_y += d < 0 ? -d : d;
    else      p->travel_x += d < 0 ? -d : d;

    int step = d * PAD_SPEED_NUM / PAD_SPEED_DEN;
    /* Integer division would round a slow, deliberate drag to zero and
     * leave the pointer stuck -- the one thing a pad must never do. */
    if (step == 0 && d != 0) step = d > 0 ? 1 : -1;
    return step;
}

void pad_synced(pad_state *p) { p->have = 1; }

int pad_button(pad_state *p, int what, int pressed, int64_t now_ms)
{
    switch (what) {
    case PAD_FINGER:    if (pressed) p->fingers = 1; break;
    case PAD_DOUBLETAP: if (pressed) p->fingers = 2; break;
    case PAD_TRIPLETAP: if (pressed) p->fingers = 3; break;
    case PAD_QUADTAP:   if (pressed) p->fingers = 4; break;

    case PAD_TOUCH:
        if (pressed) {
            /* Finger down. The position it lands on is NOT a movement;
             * treating it as one is the fling. */
            p->have = 0;
            p->travel_x = p->travel_y = 0;
            p->down_ms = now_ms;
        } else {
            int quick = (now_ms - p->down_ms) < PAD_TAP_MS;
            int still = p->travel_x < PAD_TAP_SLOP &&
                        p->travel_y < PAD_TAP_SLOP;
            int one   = p->fingers <= 1;
            p->have = 0;
            p->fingers = 0;
            /* One finger, brief, barely moved: a tap. Two fingers is a
             * right-click on every other desktop; we have no context
             * menu yet, so it does nothing rather than something
             * surprising. */
            return quick && still && one;
        }
        break;
    default: break;
    }
    return 0;
}
