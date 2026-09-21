/* pad.h — pointing-device classification and touchpad tracking.
 *
 * Split out of main.c for one reason: it is the code every laptop user
 * touches constantly, it cannot be tested by plugging a device into a
 * build machine, and getting it wrong is not a degradation but a
 * desktop that does not work. Everything here is pure -- no ioctls, no
 * file descriptors -- so tools/padtest.c can drive it with the
 * capability bits of real hardware and assert the answers.
 */
#ifndef AUROS_PAD_H
#define AUROS_PAD_H

#include <stdint.h>

enum {
    DEV_KBD    = 1,   /* a spread of letter keys                        */
    DEV_REL    = 2,   /* a mouse: REL_X/REL_Y deltas                    */
    DEV_ABS    = 4,   /* an absolute pointer: position maps to a pixel  */
    DEV_DIRECT = 8,   /* a touchscreen: the finger IS the pointer       */
    DEV_PAD    = 16   /* a touchpad: absolute axes, relative intent     */
};

/* What the kernel told us about a device, as five yes/no answers. The
 * caller reads these with EVIOCGBIT/EVIOCGPROP; this function makes the
 * decision, and only the decision. */
typedef struct {
    int has_rel_xy;      /* REL_X and REL_Y                             */
    int has_abs_xy;      /* ABS_X/ABS_Y or ABS_MT_POSITION_X/Y          */
    int has_btn_finger;  /* BTN_TOOL_FINGER                             */
    int prop_direct;     /* INPUT_PROP_DIRECT                           */
    int letter_keys;     /* how many of KEY_Q..KEY_P are present        */
} dev_caps;

int pad_classify(const dev_caps *d);

/* ── touchpad state ─────────────────────────────────────────────── */
typedef struct {
    int x, y;            /* last reported finger position               */
    int have;            /* is there a previous sample to subtract?     */
    int fingers;
    int travel_x, travel_y;
    int64_t down_ms;     /* when the finger landed                      */
    int lo_x, hi_x, lo_y, hi_y;
} pad_state;

void pad_reset(pad_state *p, int lo_x, int hi_x, int lo_y, int hi_y);

/* An absolute axis sample. Returns the pointer movement in pixels, 0 if
 * this sample is not a movement (first after a finger lands, a gesture,
 * or an implausible jump). `axis` is 0 for x, 1 for y. */
int  pad_delta(pad_state *p, int axis, int value);

/* Call once per SYN_REPORT: the next sample is now a movement. */
void pad_synced(pad_state *p);

/* A button or tool event. Returns 1 if it completes a tap, i.e. click.
 * `now_ms` is a monotonic millisecond clock. */
enum { PAD_FINGER, PAD_DOUBLETAP, PAD_TRIPLETAP, PAD_QUADTAP, PAD_TOUCH };
int  pad_button(pad_state *p, int what, int pressed, int64_t now_ms);

#endif
