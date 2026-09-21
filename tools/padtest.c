/* Does the shell understand real pointing devices?
 *
 * There is no way to plug a laptop touchpad into a build machine, and
 * this is the code every laptop user touches constantly: a mistake here
 * is not a degradation, it is a desktop that does not work. So the
 * decisions are pure functions (src/aurshell/pad.c) and this drives
 * them with the capability bits real hardware actually advertises.
 *
 * Each profile below is what the kernel reports for that class of
 * device. The comments say what goes wrong if the classification is
 * mistaken, because every one of these has been a shipped bug in some
 * desktop at some point -- two of them in this one.
 */
#include <stdio.h>
#include <string.h>
#include "../src/aurshell/pad.h"

static int fails = 0;

static void expect(const char *what, int got, int want)
{
    if (got == want) { printf("  ok    %-46s\n", what); return; }
    printf("  FAIL  %-46s got %d, want %d\n", what, got, want);
    fails++;
}

static const char *kindstr(int k)
{
    static char b[64];
    b[0] = 0;
    if (k & DEV_KBD)    strcat(b, "KBD ");
    if (k & DEV_REL)    strcat(b, "REL ");
    if (k & DEV_ABS)    strcat(b, "ABS ");
    if (k & DEV_DIRECT) strcat(b, "DIRECT ");
    if (k & DEV_PAD)    strcat(b, "PAD ");
    if (!b[0])          strcat(b, "(ignored)");
    return b;
}

struct profile {
    const char *name;
    dev_caps   caps;
    int        want;
    const char *why;
};

static const struct profile PROFILES[] = {
{ "USB mouse",
  { .has_rel_xy = 1, .letter_keys = 0 },
  DEV_REL,
  "the easy one" },

{ "USB keyboard",
  { .letter_keys = 10 },
  DEV_KBD,
  "no axes of any kind" },

{ "Synaptics touchpad (PS/2)",
  { .has_abs_xy = 1, .has_btn_finger = 1, .letter_keys = 0 },
  DEV_PAD,
  "as ABS the pointer teleports to where the finger lands" },

{ "Elan clickpad (i2c-hid)",
  { .has_abs_xy = 1, .has_btn_finger = 1, .letter_keys = 0 },
  DEV_PAD,
  "same, and every finger lift would be a click" },

{ "touchscreen",
  { .has_abs_xy = 1, .prop_direct = 1, .letter_keys = 0 },
  DEV_ABS | DEV_DIRECT,
  "as PAD a tap would move the pointer instead of pressing" },

{ "VM absolute tablet (virtio/usb-tablet)",
  { .has_abs_xy = 1, .letter_keys = 0 },
  DEV_ABS,
  "no DIRECT property and no finger tool -- absolute anyway, or the "
  "pointer never moves at all" },

{ "graphics tablet (pen)",
  { .has_abs_xy = 1, .letter_keys = 0 },
  DEV_ABS,
  "absolute, and BTN_TOUCH must not click" },

{ "keyboard with built-in trackpad (K400 class)",
  { .has_rel_xy = 1, .letter_keys = 10 },
  DEV_REL | DEV_KBD,
  "as an enum this classifies as a pointer and drops every keystroke" },

{ "touchpad that also reports relative (some HID pads)",
  { .has_rel_xy = 1, .has_abs_xy = 1, .has_btn_finger = 1 },
  DEV_REL | DEV_PAD,
  "both paths are live; the pad path must not be lost" },

{ "power button / lid switch",
  { .letter_keys = 0 },
  0,
  "reports EV_KEY but no letters -- must be ignored, not treated as a "
  "keyboard" },

{ "media-key keyboard (few letters)",
  { .letter_keys = 3 },
  0,
  "three letter keys is a remote, not a keyboard" },
};

/* A pad reports 0..1200 across and 0..800 down, which is typical. */
#define PX 0
#define PY 1

static void tap_and_drag(void)
{
    pad_state p;
    int moved;

    printf("\ntouchpad behaviour\n");

    /* A finger landing must not move the pointer. This is the fling:
     * every time the user lifts and puts their finger down somewhere
     * else, a naive subtraction throws the pointer across the screen. */
    pad_reset(&p, 0, 1200, 0, 800);
    pad_button(&p, PAD_FINGER, 1, 0);
    pad_button(&p, PAD_TOUCH, 1, 0);
    moved = pad_delta(&p, PX, 900);
    pad_delta(&p, PY, 600);
    pad_synced(&p);
    expect("finger landing does not move the pointer", moved, 0);

    /* Now it moves. */
    moved = pad_delta(&p, PX, 920);
    expect("20 units of finger -> 30 px of pointer", moved, 30);

    /* A slow, deliberate drag must not round to nothing. */
    moved = pad_delta(&p, PX, 921);
    expect("1 unit still moves the pointer", moved, 1);

    /* A second finger is a scroll gesture, not motion. */
    pad_button(&p, PAD_DOUBLETAP, 1, 0);
    moved = pad_delta(&p, PX, 1000);
    expect("two fingers do not move the pointer", moved, 0);
    pad_button(&p, PAD_TOUCH, 0, 0);

    /* A finger re-seating jumps; that is not a hand moving that fast. */
    pad_reset(&p, 0, 1200, 0, 800);
    pad_button(&p, PAD_FINGER, 1, 0);
    pad_button(&p, PAD_TOUCH, 1, 0);
    pad_delta(&p, PX, 100); pad_synced(&p);
    moved = pad_delta(&p, PX, 900);
    expect("a jump across the pad is ignored", moved, 0);

    /* A tap: down and up, quickly, having barely moved. */
    pad_reset(&p, 0, 1200, 0, 800);
    pad_button(&p, PAD_FINGER, 1, 100);
    pad_button(&p, PAD_TOUCH, 1, 100);
    pad_delta(&p, PX, 400); pad_delta(&p, PY, 400); pad_synced(&p);
    pad_delta(&p, PX, 402); pad_synced(&p);
    expect("quick, still tap clicks", pad_button(&p, PAD_TOUCH, 0, 180), 1);

    /* Too slow to be a tap: the user was resting a finger. */
    pad_reset(&p, 0, 1200, 0, 800);
    pad_button(&p, PAD_FINGER, 1, 100);
    pad_button(&p, PAD_TOUCH, 1, 100);
    expect("a long press is not a tap", pad_button(&p, PAD_TOUCH, 0, 900), 0);

    /* Moved too far: that was a drag, not a tap. Clicking here is the
     * bug where the pointer lands somewhere and opens whatever is
     * underneath it. */
    pad_reset(&p, 0, 1200, 0, 800);
    pad_button(&p, PAD_FINGER, 1, 100);
    pad_button(&p, PAD_TOUCH, 1, 100);
    pad_delta(&p, PX, 400); pad_synced(&p);
    pad_delta(&p, PX, 460); pad_synced(&p);
    expect("a drag is not a tap", pad_button(&p, PAD_TOUCH, 0, 180), 0);

    /* Two-finger tap is a right-click elsewhere; we have no context
     * menu, so it must do nothing rather than left-click. */
    pad_reset(&p, 0, 1200, 0, 800);
    pad_button(&p, PAD_FINGER, 1, 100);
    pad_button(&p, PAD_TOUCH, 1, 100);
    pad_button(&p, PAD_DOUBLETAP, 1, 110);
    expect("a two-finger tap does not left-click",
           pad_button(&p, PAD_TOUCH, 0, 170), 0);

    /* After a tap the next finger-down must not move the pointer
     * either -- the state has to reset, or the second tap flings. */
    pad_reset(&p, 0, 1200, 0, 800);
    pad_button(&p, PAD_FINGER, 1, 0);
    pad_button(&p, PAD_TOUCH, 1, 0);
    pad_delta(&p, PX, 300); pad_synced(&p);
    pad_button(&p, PAD_TOUCH, 0, 100);
    pad_button(&p, PAD_FINGER, 1, 500);
    pad_button(&p, PAD_TOUCH, 1, 500);
    moved = pad_delta(&p, PX, 800);
    expect("the finger-down after a tap does not fling", moved, 0);
}

int main(void)
{
    printf("device classification\n");
    for (unsigned i = 0; i < sizeof PROFILES / sizeof *PROFILES; i++) {
        int got = pad_classify(&PROFILES[i].caps);
        if (got == PROFILES[i].want)
            printf("  ok    %-46s %s\n", PROFILES[i].name, kindstr(got));
        else {
            printf("  FAIL  %-46s got [%s]", PROFILES[i].name, kindstr(got));
            printf(" want [%s]\n        %s\n", kindstr(PROFILES[i].want),
                   PROFILES[i].why);
            fails++;
        }
    }
    tap_and_drag();
    printf("\n%s\n", fails ? "FAIL" : "every device profile classifies correctly");
    return fails != 0;
}
