/* draw.h — the AurOS 2D rasterizer.
 *
 * Everything the shell paints goes through here: the bar, panels, the
 * command palette, focus rings. The whole visual language is rounded
 * rectangles, soft shadows and translucency over a procedural
 * wallpaper, so those three primitives are the ones that had to be
 * genuinely good rather than merely correct.
 *
 * Surfaces are 32-bit 0xAARRGGBB in native byte order. The shell's own
 * framebuffer is opaque, but intermediate layers carry alpha so panels
 * can be composited with blur behind them.
 */
#ifndef AUROS_DRAW_H
#define AUROS_DRAW_H

#include <stdint.h>

typedef struct {
    uint32_t *px;
    int w, h;
    int stride;          /* in pixels, not bytes */
} surface;

typedef struct { int x, y, w, h; } rect;

/* Corner radii, clockwise from top-left. Equal values give a uniform
 * rounded box; differing values give the asymmetric shapes the bar and
 * the palette use where they meet a screen edge. */
typedef struct { float tl, tr, br, bl; } corners;

static inline corners corners_all(float r) { corners c = {r,r,r,r}; return c; }

surface *surface_new(int w, int h);
void     surface_free(surface *s);
void     surface_fill(surface *s, uint32_t argb);

/* Alpha compositing: src over dst, both premultiplied on the fly. */
void draw_blend_px(surface *s, int x, int y, uint32_t rgb, float a);

void draw_rect(surface *s, rect r, uint32_t rgb, float a);

/* Anti-aliased via a signed distance field rather than by drawing
 * quarter-circles: one formula covers every radius, every size, and
 * sub-pixel positions, and the coverage it produces is exact rather
 * than stair-stepped. */
void draw_round_rect(surface *s, rect r, corners c, uint32_t rgb, float a);
void draw_round_rect_border(surface *s, rect r, corners c, float width,
                            uint32_t rgb, float a);

/* Vertical or horizontal two-stop gradient, clipped to a rounded box. */
void draw_round_rect_gradient(surface *s, rect r, corners c,
                              uint32_t top, uint32_t bottom, float a, int vertical);

void draw_circle(surface *s, float cx, float cy, float radius, uint32_t rgb, float a);
void draw_line(surface *s, float x0, float y0, float x1, float y1,
               float width, uint32_t rgb, float a);

/* Three-pass box blur approximates a Gaussian closely enough that the
 * difference is invisible at panel scale, and it is O(1) per pixel in
 * the radius instead of O(r^2). */
void draw_blur_region(surface *s, rect r, int radius);

/* Soft shadow cast by a rounded box, drawn beneath it. Uses the same
 * distance field, so the falloff follows the corner curvature instead
 * of ringing at the corners the way a blurred rectangle does. */
void draw_round_rect_shadow(surface *s, rect r, corners c,
                            float spread, uint32_t rgb, float a, int dy);

void draw_copy(surface *dst, const surface *src, int dx, int dy);

#endif
