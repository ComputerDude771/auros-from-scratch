/* wall.h — procedural wallpaper renderer.
 * AurOS ships no wallpaper images. The background is generated from
 * the active theme's colours, so a reskin never leaves a stale photo
 * behind and the ISO carries no megabytes of JPEG. */
#ifndef AUROS_WALL_H
#define AUROS_WALL_H
#include <stdint.h>
#include "theme.h"
/* Renders w*h packed 0x00RRGGBB into px (caller-allocated). */
void wall_render(uint32_t *px, int w, int h, const theme_t *t);
#endif
