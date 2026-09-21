#include "draw.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

surface *surface_new(int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;
    surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->px = calloc((size_t)w * h, sizeof *s->px);
    if (!s->px) { free(s); return NULL; }
    s->w = w; s->h = h; s->stride = w;
    return s;
}
void surface_free(surface *s) { if (s) { free(s->px); free(s); } }

void surface_fill(surface *s, uint32_t argb)
{
    for (int i = 0; i < s->w * s->h; i++) s->px[i] = argb;
}

void draw_blend_px(surface *s, int x, int y, uint32_t rgb, float a)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    a = clampf(a, 0.f, 1.f);
    if (a <= 0.f) return;
    uint32_t *p = &s->px[(size_t)y * s->stride + x];
    uint32_t d = *p;
    if (a >= 1.f) { *p = 0xFF000000u | (rgb & 0xFFFFFFu); return; }

    float sr = (float)((rgb >> 16) & 0xFF), sg = (float)((rgb >> 8) & 0xFF), sb = (float)(rgb & 0xFF);
    float dr = (float)((d   >> 16) & 0xFF), dg = (float)((d   >> 8) & 0xFF), db = (float)(d   & 0xFF);
    float da = (float)((d >> 24) & 0xFF) / 255.f;

    float oa = a + da * (1.f - a);
    float r = (sr * a + dr * da * (1.f - a)) / (oa > 0.f ? oa : 1.f);
    float g = (sg * a + dg * da * (1.f - a)) / (oa > 0.f ? oa : 1.f);
    float b = (sb * a + db * da * (1.f - a)) / (oa > 0.f ? oa : 1.f);

    *p = ((uint32_t)(clampf(oa,0.f,1.f) * 255.f + 0.5f) << 24) |
         ((uint32_t)(clampf(r,0.f,255.f) + 0.5f) << 16) |
         ((uint32_t)(clampf(g,0.f,255.f) + 0.5f) << 8)  |
          (uint32_t)(clampf(b,0.f,255.f) + 0.5f);
}

void draw_rect(surface *s, rect r, uint32_t rgb, float a)
{
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w, y1 = r.y + r.h;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            draw_blend_px(s, x, y, rgb, a);
}

/* Signed distance to a rounded box centred at the origin. Negative
 * inside, positive outside, and its magnitude is a true distance in
 * pixels -- which is what lets a single clamp produce exact coverage. */
static float sdf_round_box(float px, float py, float hw, float hh, float r)
{
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float qx = fabsf(px) - (hw - r);
    float qy = fabsf(py) - (hh - r);
    float ax = qx > 0.f ? qx : 0.f;
    float ay = qy > 0.f ? qy : 0.f;
    float outside = sqrtf(ax*ax + ay*ay);
    float inside  = fminf(fmaxf(qx, qy), 0.f);
    return outside + inside - r;
}

/* Per-corner radius: pick the radius belonging to the quadrant the
 * sample falls in, then evaluate the symmetric SDF in that quadrant. */
static float corner_for(const corners *c, float px, float py)
{
    if (py < 0.f) return px < 0.f ? c->tl : c->tr;
    return px < 0.f ? c->bl : c->br;
}

static void round_rect_cov(surface *s, rect r, corners c, float expand,
                           void (*emit)(surface *, int, int, float, void *), void *ud)
{
    float hw = r.w * 0.5f + expand, hh = r.h * 0.5f + expand;
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    int pad = (int)ceilf(expand) + 2;

    int x0 = r.x - pad, y0 = r.y - pad;
    int x1 = r.x + r.w + pad, y1 = r.y + r.h + pad;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f - cx;
            float rad = corner_for(&c, px, py) + expand;
            float d = sdf_round_box(px, py, hw, hh, rad);
            /* d is a distance in pixels, so 0.5 - d is exactly the
             * fraction of this pixel covered by the shape's edge. */
            float cov = clampf(0.5f - d, 0.f, 1.f);
            if (cov > 0.f) emit(s, x, y, cov, ud);
        }
    }
}

typedef struct { uint32_t rgb; float a; } fill_ud;
static void emit_fill(surface *s, int x, int y, float cov, void *ud)
{
    fill_ud *f = ud;
    draw_blend_px(s, x, y, f->rgb, cov * f->a);
}

void draw_round_rect(surface *s, rect r, corners c, uint32_t rgb, float a)
{
    fill_ud ud = { rgb, a };
    round_rect_cov(s, r, c, 0.f, emit_fill, &ud);
}

typedef struct { uint32_t top, bot; float a; int vertical; rect r; } grad_ud;
static void emit_grad(surface *s, int x, int y, float cov, void *ud)
{
    grad_ud *g = ud;
    float t = g->vertical ? ((float)y - g->r.y) / (float)(g->r.h ? g->r.h : 1)
                          : ((float)x - g->r.x) / (float)(g->r.w ? g->r.w : 1);
    t = clampf(t, 0.f, 1.f);
    float r0 = (float)((g->top >> 16) & 0xFF), r1 = (float)((g->bot >> 16) & 0xFF);
    float g0 = (float)((g->top >> 8)  & 0xFF), g1 = (float)((g->bot >> 8)  & 0xFF);
    float b0 = (float)( g->top        & 0xFF), b1 = (float)( g->bot        & 0xFF);
    uint32_t col = ((uint32_t)(r0 + (r1-r0)*t) << 16) |
                   ((uint32_t)(g0 + (g1-g0)*t) << 8)  |
                    (uint32_t)(b0 + (b1-b0)*t);
    draw_blend_px(s, x, y, col, cov * g->a);
}

void draw_round_rect_gradient(surface *s, rect r, corners c,
                              uint32_t top, uint32_t bottom, float a, int vertical)
{
    grad_ud ud = { top, bottom, a, vertical, r };
    round_rect_cov(s, r, c, 0.f, emit_grad, &ud);
}

void draw_round_rect_border(surface *s, rect r, corners c, float width,
                            uint32_t rgb, float a)
{
    /* Ring coverage = outer coverage minus inner coverage, evaluated
     * from the same field so the two edges stay concentric and the
     * stroke has even weight all the way round the corners. */
    float hw = r.w * 0.5f, hh = r.h * 0.5f;
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    int pad = 2;
    int x0 = r.x - pad < 0 ? 0 : r.x - pad, y0 = r.y - pad < 0 ? 0 : r.y - pad;
    int x1 = r.x + r.w + pad, y1 = r.y + r.h + pad;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f - cx;
            float rad = corner_for(&c, px, py);
            float dout = sdf_round_box(px, py, hw, hh, rad);
            float din  = sdf_round_box(px, py, hw - width, hh - width,
                                       rad - width > 0.f ? rad - width : 0.f);
            float cov = clampf(0.5f - dout, 0.f, 1.f) - clampf(0.5f - din, 0.f, 1.f);
            if (cov > 0.f) draw_blend_px(s, x, y, rgb, cov * a);
        }
    }
}

typedef struct { uint32_t rgb; float a; float spread; rect r; corners c; } shadow_ud;
static void emit_shadow(surface *s, int x, int y, float cov, void *ud)
{
    (void)cov;
    shadow_ud *sh = ud;
    float cx = sh->r.x + sh->r.w * 0.5f, cy = sh->r.y + sh->r.h * 0.5f;
    float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
    float rad = corner_for(&sh->c, px, py);
    float d = sdf_round_box(px, py, sh->r.w * 0.5f, sh->r.h * 0.5f, rad);
    /* Smooth falloff over the spread distance. Squaring it gives the
     * soft shoulder a real shadow has rather than a linear ramp. */
    float t = clampf(1.f - d / sh->spread, 0.f, 1.f);
    float f = t * t;
    if (f > 0.f) draw_blend_px(s, x, y, sh->rgb, f * sh->a);
}

void draw_round_rect_shadow(surface *s, rect r, corners c,
                            float spread, uint32_t rgb, float a, int dy)
{
    rect sr = r; sr.y += dy;
    shadow_ud ud = { rgb, a, spread <= 0.f ? 1.f : spread, sr, c };
    round_rect_cov(s, sr, c, spread, emit_shadow, &ud);
}

void draw_circle(surface *s, float cx, float cy, float radius, uint32_t rgb, float a)
{
    int x0 = (int)floorf(cx - radius) - 1, y0 = (int)floorf(cy - radius) - 1;
    int x1 = (int)ceilf(cx + radius) + 1,  y1 = (int)ceilf(cy + radius) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float cov = clampf(0.5f - (sqrtf(dx*dx + dy*dy) - radius), 0.f, 1.f);
            if (cov > 0.f) draw_blend_px(s, x, y, rgb, cov * a);
        }
}

void draw_line(surface *s, float x0, float y0, float x1, float y1,
               float width, uint32_t rgb, float a)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 1e-6f) return;
    float hw = width * 0.5f;
    int bx0 = (int)floorf(fminf(x0,x1) - hw) - 1, by0 = (int)floorf(fminf(y0,y1) - hw) - 1;
    int bx1 = (int)ceilf (fmaxf(x0,x1) + hw) + 1, by1 = (int)ceilf (fmaxf(y0,y1) + hw) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > s->w) bx1 = s->w;
    if (by1 > s->h) by1 = s->h;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float t = clampf((px*dx + py*dy) / (len*len), 0.f, 1.f);
            float qx = px - dx*t, qy = py - dy*t;
            float cov = clampf(0.5f - (sqrtf(qx*qx + qy*qy) - hw), 0.f, 1.f);
            if (cov > 0.f) draw_blend_px(s, x, y, rgb, cov * a);
        }
}

/* Separable box blur, run three times. Three box passes converge on a
 * Gaussian (central limit theorem) and each pass is a running sum, so
 * cost is independent of the radius. */
static void box_blur_pass(uint32_t *src, uint32_t *dst, int w, int h, int stride,
                          int radius, int horizontal)
{
    int outer = horizontal ? h : w;
    int inner = horizontal ? w : h;
    int step  = horizontal ? 1 : stride;
    int jump  = horizontal ? stride : 1;
    int win   = radius * 2 + 1;

    /* Integer accumulators, not float. A running sum of bytes is exact
     * in int, whereas float add/subtract drifts over a long row and can
     * push a channel a hair above 255 -- which, shifted into place
     * unclamped, spills into the NEXT channel and paints coloured
     * streaks across every blurred panel. That was a real bug here. */
    for (int o = 0; o < outer; o++) {
        uint32_t *line_s = src + (size_t)o * jump;
        uint32_t *line_d = dst + (size_t)o * jump;
        int32_t ra = 0, rr = 0, rg = 0, rb = 0;

        for (int i = -radius; i <= radius; i++) {
            int k = i < 0 ? 0 : (i >= inner ? inner - 1 : i);
            uint32_t p = line_s[(size_t)k * step];
            ra += (int32_t)((p >> 24) & 0xFF); rr += (int32_t)((p >> 16) & 0xFF);
            rg += (int32_t)((p >> 8)  & 0xFF); rb += (int32_t)( p        & 0xFF);
        }
        for (int i = 0; i < inner; i++) {
            /* Rounded integer division, then clamp defensively. */
            int32_t oa = (ra + win/2) / win, orr = (rr + win/2) / win;
            int32_t og = (rg + win/2) / win, ob  = (rb + win/2) / win;
            if (oa < 0) oa = 0; if (oa > 255) oa = 255;
            if (orr < 0) orr = 0; if (orr > 255) orr = 255;
            if (og < 0) og = 0; if (og > 255) og = 255;
            if (ob < 0) ob = 0; if (ob > 255) ob = 255;
            line_d[(size_t)i * step] = ((uint32_t)oa << 24) | ((uint32_t)orr << 16) |
                                       ((uint32_t)og  << 8) |  (uint32_t)ob;

            int add = i + radius + 1; if (add >= inner) add = inner - 1;
            int sub = i - radius;     if (sub < 0)      sub = 0;
            uint32_t pa = line_s[(size_t)add * step], ps = line_s[(size_t)sub * step];
            ra += (int32_t)((pa >> 24) & 0xFF) - (int32_t)((ps >> 24) & 0xFF);
            rr += (int32_t)((pa >> 16) & 0xFF) - (int32_t)((ps >> 16) & 0xFF);
            rg += (int32_t)((pa >> 8)  & 0xFF) - (int32_t)((ps >> 8)  & 0xFF);
            rb += (int32_t)( pa        & 0xFF) - (int32_t)( ps        & 0xFF);
        }
    }
    (void)w; (void)h;
}

void draw_blur_region(surface *s, rect r, int radius)
{
    if (radius < 1) return;
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > s->w ? s->w : r.x + r.w;
    int y1 = r.y + r.h > s->h ? s->h : r.y + r.h;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;

    uint32_t *a = malloc((size_t)w * h * sizeof *a);
    uint32_t *b = malloc((size_t)w * h * sizeof *b);
    if (!a || !b) { free(a); free(b); return; }

    for (int y = 0; y < h; y++)
        memcpy(a + (size_t)y * w, s->px + (size_t)(y0 + y) * s->stride + x0,
               (size_t)w * sizeof *a);

    for (int pass = 0; pass < 3; pass++) {
        box_blur_pass(a, b, w, h, w, radius, 1);
        box_blur_pass(b, a, w, h, w, radius, 0);
    }
    for (int y = 0; y < h; y++)
        memcpy(s->px + (size_t)(y0 + y) * s->stride + x0, a + (size_t)y * w,
               (size_t)w * sizeof *a);
    free(a); free(b);
}

void draw_copy(surface *dst, const surface *src, int dx, int dy)
{
    for (int y = 0; y < src->h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < src->w; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            uint32_t p = src->px[(size_t)y * src->stride + x];
            draw_blend_px(dst, tx, ty, p & 0xFFFFFFu, (float)((p >> 24) & 0xFF) / 255.f);
        }
    }
}
