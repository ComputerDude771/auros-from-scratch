/* osd.h — the thing that appears when she presses a key on the side.
 *
 * Volume and brightness are the two controls on a laptop that are
 * already physical: they are printed on the keyboard and people press
 * them without being taught. What they have never worked without is
 * feedback. A brightness key that changes the screen tells you what it
 * did; a volume key in a silent room tells you nothing at all, and the
 * person presses it eleven more times.
 *
 * So: a bar, in the middle, for a moment. It is not a panel and it is
 * not a control -- it cannot be pressed, it goes away on its own, and
 * nothing is lost if she never sees it.
 *
 * That is the one place in this product where something disappears by
 * itself, and docs/EASY.md rule 5 says nothing may be time-limited. It
 * does not break that rule, it is the reason for it: the rule exists
 * so that nothing she NEEDS is on a timer. This is an echo of a key she
 * just pressed, and the key is always there to press again.
 */
#ifndef AUROS_OSD_H
#define AUROS_OSD_H

#include "shell.h"

/* What it is showing. */
typedef enum { OSD_NONE, OSD_VOLUME, OSD_BRIGHTNESS, OSD_MUTED,
               /* One line, no bar: where a picture was saved, what
                * could not be done. Same shape, same timing, same
                * reason -- it is the echo of something she just did. */
               OSD_SAID } osd_kind;

/* Show one, from now. `value` is 0..100. */
void osd_show(osd_kind kind, int value);
void osd_say(const char *line);

/* True while one is on screen; the host repaints while this holds. */
int  osd_visible(void);

/* Painted last, over everything including the band: it is the answer
 * to a key that was just pressed, and an answer behind a window is not
 * one. */
void osd_paint(shell_ctx *c, surface *s, shell_fonts *f);

#endif
