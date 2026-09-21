/* anim.h — easing and scaled compositing.
 *
 * Motion is not decoration here. For someone who has never used this
 * system, a thing that MOVES from where they clicked to where it ends up
 * teaches the relationship between the two; a thing that teleports has
 * to be explained. Every transition in the shell exists to answer
 * "where did it go?" before the user has to ask.
 *
 * Everything is float-timed and frame-rate independent, because the
 * hardware this targets does not hold a steady frame rate.
 */
#ifndef AUROS_ANIM_H
#define AUROS_ANIM_H

#include <stdint.h>
#include "draw.h"

typedef enum {
    EASE_LINEAR,
    EASE_OUT_CUBIC,     /* default for things arriving: fast, then settles */
    EASE_IN_OUT_CUBIC,  /* for things moving between two known places     */
    EASE_OUT_BACK,      /* slight overshoot; use sparingly, reads as play  */
    EASE_SPRING         /* damped oscillation; the theme's default curve   */
} ease_kind;

float ease(ease_kind k, float t);          /* t in [0,1] -> eased [0,~1] */

/* A value animating toward a target. Update once per frame with the
 * real elapsed time; ask whether it is still moving to decide if the
 * shell needs another frame or can go back to sleep. */
typedef struct {
    float from, to, value;
    float elapsed, duration;
    ease_kind kind;
    int active;
} tween;

void  tween_to(tween *t, float target, float duration_s, ease_kind k);
void  tween_set(tween *t, float value);     /* jump, no animation */
int   tween_step(tween *t, float dt);       /* returns 1 while still moving */

/* Bilinear scaled blit. Card-based UIs live or die on being able to
 * shrink a live view into a thumbnail and grow it back without the
 * result looking like a screenshot of a screenshot. Nearest-neighbour
 * shimmers horribly while animating, which is exactly when it is seen. */
void draw_scaled(surface *dst, const surface *src, rect dst_rect, float alpha);

/* Same, but clipped to a rounded rectangle, so a scaled view can sit in
 * a rounded card without a separate masking pass. */
/* ── where live content actually landed ─────────────────────────
 *
 * Once windows contain another process's pixels, a click inside one has
 * to reach that process, which means knowing where on screen the window
 * was drawn. Six archetypes compute that rectangle six different ways,
 * and asking each of them to also describe it would be six chances for
 * the description to drift from the drawing -- which is the exact bug
 * docs/DESIGN.md already records in the rail archetype.
 *
 * So nothing describes it. The scaled blits record the rectangle they
 * blit into, and input routing reads that. The record is a byproduct of
 * painting, so it cannot disagree with painting. Entries are in paint
 * order, which is back to front.
 */
void draw_track_reset(void);
int  draw_track_count(void);
int  draw_track_at(int i, const surface **src, rect *out);
/* The topmost rectangle this content was drawn into this frame. */
int  draw_track_find(const surface *src, rect *out);
/* The space the archetype set aside for it, which is usually larger.
 * This is the size a client should be asked to draw at. */
int  draw_track_slot(const surface *src, rect *out);

/* Fit content into the slot an archetype chose, without ever enlarging
 * it. See draw_fit_rect() in anim.c for why never enlarging matters. */
rect draw_fit_rect(rect slot, const surface *src);
void draw_content_fit(surface *dst, const surface *src, rect slot,
                      corners c, float alpha);

void draw_scaled_rounded(surface *dst, const surface *src, rect dst_rect,
                         corners c, float alpha);

#endif
