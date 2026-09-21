/* settings.h — the things about this computer she is allowed to change.
 *
 * There was no settings screen in this product. Not a thin one: none.
 * The wallpaper, the theme, the clock, the keyboard, and which of the
 * six desktops the machine uses were all decided when the image was
 * built, by somebody else, for everybody. docs/SHELLS.md calls the
 * choice of desktop "the single most consequential line" in a profile
 * and then offered her no way to reach it.
 *
 * WHAT IS IN HERE AND WHAT IS NOT
 *
 * Only things she would go looking for, and only things that are hers
 * to decide. Not the network -- that has its own button, because
 * getting online is not a setting, it is a thing you do. Not the
 * permissions or the app lock-down, which belong to whoever set the
 * machine up. This is the brightness, the sound, the size of the
 * words, what the time is, how the desktop works and what it looks
 * like.
 *
 * A build can forbid all of it. allow_settings_change="no" takes the
 * button away entirely rather than showing controls that refuse, which
 * is the rule this product keeps relearning: a control that looks like
 * a control and is not is worse than no control.
 *
 * THE SLIDERS
 *
 * Every one of them has a minus and a plus as well as a track, and the
 * track answers a single press anywhere along it. Dragging works and
 * is never required -- docs/EASY.md rule 5. A slider that can only be
 * dragged is a control for steady hands.
 */
#ifndef AUROS_SETTINGS_H
#define AUROS_SETTINGS_H

#include "shell.h"

void settings_opened(shell_ctx *c);
void settings_closed(shell_ctx *c);

void settings_paint(shell_ctx *c, surface *s, shell_fonts *f);
int  settings_click(shell_ctx *c, int x, int y);
void settings_motion(shell_ctx *c, int x, int y);
int  settings_key(shell_ctx *c, int k);   /* 1 if this panel took it */

/* Once per pass of the main loop. 1 if the screen has something new.
 * Where a change that could not take effect immediately -- a theme
 * still being written to disk -- becomes a reload. */
int  settings_step(shell_ctx *c);

/* True while a slider is being dragged, so the host keeps the frames
 * coming and does not let the archetype see the motion. */
int  settings_dragging(void);

/* ── measuring it ───────────────────────────────────────────────────
 *
 * The same contract as the wifi panel's: the layout is a pure function
 * of the view and the panel size, so tools/targets.c can walk every
 * screen at every text size without a battery, a backlight or a sound
 * server. */
#define SET_PAGE_MAIN   0
#define SET_PAGE_TIME   1
#define SET_PAGE_SHELL  2
#define SET_PAGE_LOOK   3
#define SET_PAGE_N      4

typedef struct {
    int page;        /* one of SET_PAGE_*                      */
    int n_rows;      /* how many rows this page has            */
    int first_row;   /* paging: the row at the top             */
    int battery;     /* 1 if this machine shows a battery row  */
    int backlight;   /* 1 if it shows a brightness row         */
    int sound;       /* 1 if it shows a sound row              */
} set_view;

int  settings_targets(const shell_ctx *c, int sw, int sh, const set_view *v,
                      rect *out, int max);
/* How many rows a page has, for a machine described by the view. */
int  settings_rows_for(const set_view *v);

#endif
