/* foot.h — the band that is always there.
 *
 * docs/EASY.md rules 3, 6 and 7: she can always make the text bigger,
 * every state she can reach she can leave, and help is one thing in one
 * place. All three want the same thing -- something that does not
 * belong to any archetype and cannot be navigated away from -- so all
 * three live here.
 *
 * It carries exactly what she had no way to do at all:
 *
 *   Help          one thing, same place, in all six archetypes
 *   Smaller / Bigger   the type scale, which is the single highest
 *                 value control in the product for a 68-year-old
 *   Turn off      there was no way to shut the computer down except
 *                 holding the power button in
 *
 * HOW IT REACHES SIX ARCHETYPES WITHOUT SIX IMPLEMENTATIONS
 *
 * It does not draw into them and they cannot draw into it. The host
 * hands the archetype a surface that is shorter than the screen --
 * same pixels, same stride, fewer rows -- and reduces c->screen_h to
 * match. Every primitive in draw.c and font.c already clips to s->h, so
 * the band is unreachable from a layout by construction rather than by
 * agreement, and not one archetype needed editing to gain it.
 *
 * THE ONE PLACE A THEME DOES NOT REACH
 *
 * Its colours are fixed, and that is a deliberate exception to the rule
 * in docs/DESIGN.md that nothing hardcodes a colour. A theme may
 * restyle every pixel in this product except the one that leads out of
 * it: the control she uses when the screen has become unreadable must
 * not be styled by the thing that made it unreadable.
 *
 * Its type has a floor for the same reason. The floor is a floor and
 * not a ceiling -- the band grows with her chosen size, because the
 * person who picks the largest text is the person who needs the way out
 * in the largest text.
 */
#ifndef AUROS_FOOT_H
#define AUROS_FOOT_H

#include "shell.h"

/* Where the buttons are. ONE function, called by painting, by
 * hit-testing and by the harness that measures them -- exposed so that
 * tools/targets.c can check rule 4 against the real geometry rather
 * than against a number copied out of this file. FOOT_MAX is the most
 * it ever returns. */
#define FOOT_MAX 4
int  foot_buttons(const shell_ctx *c, int sw, int sh, rect *out, int *which);

/* Rows to reserve at the bottom of the screen. 0 when the profile has
 * turned the band off entirely, which a kiosk does. */
int  foot_height(const shell_ctx *c);

/* Paint into the FULL-height surface, after the archetype has painted
 * into its shorter one. */
void foot_paint(shell_ctx *c, surface *full, shell_fonts *f);

/* Screen coordinates, full height. Returns 1 if the band took it, in
 * which case the archetype must not also act on it. */
int  foot_click(shell_ctx *c, int x, int y);
void foot_motion(shell_ctx *c, int x, int y);

/* True while the help panel is showing; the host dims input to the
 * archetype underneath it. */
int  foot_help_open(const shell_ctx *c);

/* Her chosen text size, as a multiplier over the theme's sizes. Read at
 * start-up from her own settings and written back when she changes it,
 * so it survives a restart without needing anything privileged. */
float foot_load_text_scale(void);
void  foot_save_text_scale(float scale);

#endif
