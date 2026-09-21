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

/* One pixel of source-over. This function is called several million
 * times per frame -- 2.5M to 6.3M, measured -- so what it does NOT do
 * matters more than what it does.
 *
 * The general form divides each channel by the composited alpha to
 * un-premultiply. Three float divisions per pixel. But the shell's
 * framebuffer is opaque from the wallpaper blit onward, so the
 * destination alpha is always 255, the output alpha is always 1, and
 * all three divisions are by exactly 1.0. That case gets an integer
 * lerp instead and is the one nearly every pixel takes.
 *
 * The general path stays for surfaces that really are translucent --
 * offscreen buffers, window content -- because silently producing the
 * wrong answer there would be far worse than the divisions. */
void draw_blend_px(surface *s, int x, int y, uint32_t rgb, float a)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    a = clampf(a, 0.f, 1.f);
    if (a <= 0.f) return;
    uint32_t *p = &s->px[(size_t)y * s->stride + x];
    uint32_t d = *p;
    if (a >= 1.f) { *p = 0xFF000000u | (rgb & 0xFFFFFFu); return; }

    if ((d >> 24) == 0xFFu) {
        /* 0..256 rather than 0..255: the multiply then folds into a
         * shift, and 256 is exactly reachable so full alpha is exact. */
        uint32_t sa = (uint32_t)(a * 256.f + 0.5f);
        if (sa == 0) return;
        if (sa > 256) sa = 256;
        uint32_t ia = 256u - sa;
        uint32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
        uint32_t dr = (d   >> 16) & 0xFF, dg = (d   >> 8) & 0xFF, db = d   & 0xFF;
        /* +128 before the shift rounds to nearest. Truncating instead
         * biases every blend downward by up to one level, and a frame
         * stacks five or six blends on the same pixel, so the bias
         * compounds into a visible darkening rather than cancelling. */
        *p = 0xFF000000u
           | ((((sr * sa + dr * ia + 128u) >> 8) & 0xFFu) << 16)
           | ((((sg * sa + dg * ia + 128u) >> 8) & 0xFFu) << 8)
           |   (((sb * sa + db * ia + 128u) >> 8) & 0xFFu);
        return;
    }

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

/* Walk the pixels a rounded box touches, handing each one its exact
 * coverage.
 *
 * The obvious implementation evaluates the distance field for every
 * pixel in the bounding box. That is enormously wasteful: the field
 * only says anything interesting within a pixel of the boundary, and
 * for a full-size card the interior is ~95% of the area. Profiling the
 * shell at 1366x768 found this was the single largest cost in a frame,
 * ahead of even the blur -- roughly half a million needless sqrtf calls
 * per card per frame.
 *
 * So each row is split: the span that is unambiguously inside gets
 * coverage 1 with no field evaluation at all, and only the edge bands
 * and the corner arcs are sampled. `exact` disables the shortcut for
 * callers (shadows) whose emit function ignores the coverage and
 * computes its own falloff from the field, where "inside" is not 1. */
static void round_rect_cov_ex(surface *s, rect r, corners c, float expand, int exact,
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

    /* Conservative radius: the largest corner, so the interior bound
     * holds whichever quadrant a row happens to cross. */
    float rmax = c.tl;
    if (c.tr > rmax) rmax = c.tr;
    if (c.br > rmax) rmax = c.br;
    if (c.bl > rmax) rmax = c.bl;
    rmax += expand;
    if (rmax > hw) rmax = hw;
    if (rmax > hh) rmax = hh;

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f - cy;

        int lo = x0, hi = x1;            /* span to evaluate exactly */
        int ilo = 0, ihi = -1;           /* span known to be interior */

        /* The row must also be clear of the horizontal edges. Constraining
         * only x was wrong: a row inside a corner band is still within a
         * pixel of the top or bottom edge, where coverage is partial
         * whatever x is, and claiming it as interior drew those rows at
         * full alpha. */
        if (!exact && fabsf(py) <= hh - 1.f) {
            /* Half-width of the guaranteed-inside span on this row: the
             * full width away from the corner bands, and the straight
             * section between them inside one. */
            float inner_hw = (fabsf(py) <= hh - rmax) ? hw - 1.f : hw - rmax - 1.f;
            if (inner_hw > 0.f) {
                ilo = (int)ceilf(cx - inner_hw);
                ihi = (int)floorf(cx + inner_hw);
                if (ilo < x0) ilo = x0;
                if (ihi >= x1) ihi = x1 - 1;
                if (ihi >= ilo) { lo = x0; hi = x1; }
                else { ihi = -1; }
            }
        }

        for (int x = lo; x < hi; x++) {
            if (ihi >= ilo && x >= ilo && x <= ihi) {
                /* Emit EVERY pixel of the interior run, then jump past
                 * it. Emitting once and skipping to the end silently
                 * drew one pixel per row and left the card hollow. */
                for (int xi = x; xi <= ihi; xi++) emit(s, xi, y, 1.f, ud);
                x = ihi;
                continue;
            }
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

static void round_rect_cov(surface *s, rect r, corners c, float expand,
                           void (*emit)(surface *, int, int, float, void *), void *ud)
{
    round_rect_cov_ex(s, r, c, expand, 0, emit, ud);
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

/* A gradient's two stops are constants, so unpacking them per pixel --
 * six shifts, six masks, six int-to-float conversions of the same two
 * numbers, two thirds of a million times a frame -- is pure waste. They
 * are unpacked once here. A vertical gradient is also constant along a
 * row, so the colour is computed once per row and reused. */
typedef struct {
    float r0, g0, b0, dr, dg, db;
    float a;
    int   vertical;
    rect  r;
    int   row;            /* the row `cached` belongs to, -1 for none */
    uint32_t cached;
} grad_ud;

static uint32_t grad_at(grad_ud *g, float t)
{
    t = clampf(t, 0.f, 1.f);
    return ((uint32_t)(g->r0 + g->dr * t) << 16) |
           ((uint32_t)(g->g0 + g->dg * t) << 8)  |
            (uint32_t)(g->b0 + g->db * t);
}

static void emit_grad(surface *s, int x, int y, float cov, void *ud)
{
    grad_ud *g = ud;
    uint32_t col;
    if (g->vertical) {
        if (y != g->row) {
            g->row = y;
            g->cached = grad_at(g, ((float)y - g->r.y) /
                                   (float)(g->r.h ? g->r.h : 1));
        }
        col = g->cached;
    } else {
        col = grad_at(g, ((float)x - g->r.x) / (float)(g->r.w ? g->r.w : 1));
    }
    draw_blend_px(s, x, y, col, cov * g->a);
}

void draw_round_rect_gradient(surface *s, rect r, corners c,
                              uint32_t top, uint32_t bottom, float a, int vertical)
{
    grad_ud ud;
    ud.r0 = (float)((top >> 16) & 0xFF);
    ud.g0 = (float)((top >> 8)  & 0xFF);
    ud.b0 = (float)( top        & 0xFF);
    ud.dr = (float)((bottom >> 16) & 0xFF) - ud.r0;
    ud.dg = (float)((bottom >> 8)  & 0xFF) - ud.g0;
    ud.db = (float)( bottom        & 0xFF) - ud.b0;
    ud.a = a; ud.vertical = vertical; ud.r = r;
    ud.row = -1; ud.cached = 0;
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

    /* Skip the interior, exactly as draw_round_rect does.
     *
     * A border is a one- or two-pixel stroke, and this loop was
     * evaluating the distance field -- two sqrtf each -- across the
     * whole bounding box to find it. Measured on the rail archetype:
     * 1,101,385 pixels scanned per frame from twelve calls, of which
     * about 51,000 carry any ink. Over 95% of the work produced a
     * coverage of zero.
     *
     * More than `width` inside the inner edge, both fields saturate and
     * the ring coverage is exactly 1 - 1 = 0, so skipping is not an
     * approximation. The conservative bound is the largest corner
     * radius, so it holds whichever quadrant a row crosses. */
    float rmax = c.tl;
    if (c.tr > rmax) rmax = c.tr;
    if (c.br > rmax) rmax = c.br;
    if (c.bl > rmax) rmax = c.bl;

    float ihw = hw - width - 1.f;        /* half-width of the hole    */
    float ihh = hh - width - 1.f;
    float irm = rmax - width;            /* its corner radius         */
    if (irm < 0.f) irm = 0.f;

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f - cy;

        int hlo = 0, hhi = -1;           /* the span to skip on this row */
        if (ihw > 0.f && ihh > 0.f && fabsf(py) <= ihh) {
            float span = (fabsf(py) <= ihh - irm) ? ihw : ihw - irm;
            if (span > 0.f) {
                hlo = (int)ceilf(cx - span);
                hhi = (int)floorf(cx + span);
                if (hlo < x0) hlo = x0;
                if (hhi >= x1) hhi = x1 - 1;
                if (hhi < hlo) hhi = -1;
            }
        }

        for (int x = x0; x < x1; x++) {
            if (hhi >= hlo && x >= hlo && x <= hhi) { x = hhi; continue; }
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

typedef struct { uint32_t rgb; float a; float spread; rect r; corners c;
                 float hw, hh, cx, cy, rmax; } shadow_ud;
static void emit_shadow(surface *s, int x, int y, float cov, void *ud)
{
    (void)cov;
    shadow_ud *sh = ud;
    float px = (float)x + 0.5f - sh->cx, py = (float)y + 0.5f - sh->cy;

    /* Well inside the box the distance is negative, so the falloff
     * saturates at exactly 1 and the shadow is a flat wash. Detecting
     * that with two comparisons instead of computing it with a sqrtf is
     * exact, not an approximation, and it covers roughly 83% of the
     * 820,000 to 1,110,000 pixels a shadow writes per frame -- which
     * made it the single most expensive thing in the frame. */
    float ax = fabsf(px), ay = fabsf(py);
    if (ax <= sh->hw - sh->rmax - 1.f && ay <= sh->hh - 1.f) {
        draw_blend_px(s, x, y, sh->rgb, sh->a);
        return;
    }
    if (ay <= sh->hh - sh->rmax - 1.f && ax <= sh->hw - 1.f) {
        draw_blend_px(s, x, y, sh->rgb, sh->a);
        return;
    }

    float rad = corner_for(&sh->c, px, py);
    float d = sdf_round_box(px, py, sh->hw, sh->hh, rad);
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
    float rmax = c.tl;
    if (c.tr > rmax) rmax = c.tr;
    if (c.br > rmax) rmax = c.br;
    if (c.bl > rmax) rmax = c.bl;
    shadow_ud ud = { rgb, a, spread <= 0.f ? 1.f : spread, sr, c,
                     sr.w * 0.5f, sr.h * 0.5f,
                     sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f, rmax };
    round_rect_cov_ex(s, sr, c, spread, 1, emit_shadow, &ud);
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

/* ── box blur ──────────────────────────────────────────────────────
 *
 * Separable box blur run three times. Three box passes converge on a
 * Gaussian (central limit theorem) and each pass is a running sum, so
 * the cost is independent of the radius.
 *
 * Two things here are not obvious and both were measured, not guessed:
 *
 * 1. NO DIVISION. The naive pass divides four channels by the window
 *    width for every pixel of every pass -- 24 integer divisions per
 *    pixel per blur. Integer division is 20-40 cycles on the hardware
 *    we target, and at 1366x768 that alone is most of a frame. It is
 *    replaced by a multiply-and-shift by a precomputed reciprocal,
 *    which is EXACT here, not approximate: tools/recip_proof.c checks
 *    every (window, sum) pair a blur can produce, all 4.2 million of
 *    them, for radius 1..128.
 *
 * 2. NO STRIDED WALK. The vertical pass of the naive version steps a
 *    whole row between reads, so on a wide region every single pixel
 *    access is a cache miss. Instead the buffer is transposed once and
 *    the vertical passes run as horizontal ones. Horizontal and
 *    vertical box filters commute, so doing H,H,H,V,V,V instead of
 *    H,V,H,V,H,V is the same filter; the intermediate rounding differs
 *    by at most a hair and it is a blurred backdrop.
 * ───────────────────────────────────────────────────────────────── */

#define BLUR_MAX_RADIUS 128   /* beyond this a blur is a flat fill */

/* One pass along rows. src and dst are w x h, tightly packed. */
static void box_blur_rows(const uint32_t *src, uint32_t *dst,
                          int w, int h, int radius)
{
    int win = radius * 2 + 1;
    uint32_t recip = (0xFFFFFFFFu / (uint32_t)win) + 1u;
    int half = win / 2;

    for (int y = 0; y < h; y++) {
        const uint32_t *ls = src + (size_t)y * w;
        uint32_t       *ld = dst + (size_t)y * w;
        /* Integer accumulators, not float: a running sum of bytes is
         * exact in int, whereas float add/subtract drifts over a long
         * row and can push a channel a hair above 255 -- which, shifted
         * into place unclamped, spills into the NEXT channel and paints
         * coloured streaks across every blurred panel. Real bug, once. */
        int32_t ra = 0, rr = 0, rg = 0, rb = 0;

        uint32_t first = ls[0], last = ls[w - 1];
        /* Seed the window, clamping the taps that hang off either end
         * to the edge pixel -- the same edge rule the running update
         * below uses, so the two never disagree. */
        for (int i = -radius; i <= radius; i++) {
            uint32_t p = i < 0 ? first : (i >= w ? last : ls[i]);
            ra += (int32_t)((p >> 24) & 0xFF); rr += (int32_t)((p >> 16) & 0xFF);
            rg += (int32_t)((p >> 8)  & 0xFF); rb += (int32_t)( p        & 0xFF);
        }

        for (int x = 0; x < w; x++) {
            uint32_t oa = (uint32_t)(((uint64_t)(uint32_t)(ra + half) * recip) >> 32);
            uint32_t orr= (uint32_t)(((uint64_t)(uint32_t)(rr + half) * recip) >> 32);
            uint32_t og = (uint32_t)(((uint64_t)(uint32_t)(rg + half) * recip) >> 32);
            uint32_t ob = (uint32_t)(((uint64_t)(uint32_t)(rb + half) * recip) >> 32);
            ld[x] = (oa << 24) | (orr << 16) | (og << 8) | ob;

            int add = x + radius + 1, sub = x - radius;
            uint32_t pa = add >= w ? last  : ls[add];
            uint32_t ps = sub < 0  ? first : ls[sub];
            ra += (int32_t)((pa >> 24) & 0xFF) - (int32_t)((ps >> 24) & 0xFF);
            rr += (int32_t)((pa >> 16) & 0xFF) - (int32_t)((ps >> 16) & 0xFF);
            rg += (int32_t)((pa >> 8)  & 0xFF) - (int32_t)((ps >> 8)  & 0xFF);
            rb += (int32_t)( pa        & 0xFF) - (int32_t)( ps        & 0xFF);
        }
    }
}

/* Blocked transpose. The block keeps both the read and the write side
 * inside a handful of cache lines; a naive transpose misses on one side
 * or the other for every pixel, which is the cost we came here to
 * avoid. */
#define TBLK 32
static void transpose(const uint32_t *src, uint32_t *dst, int w, int h)
{
    for (int by = 0; by < h; by += TBLK)
        for (int bx = 0; bx < w; bx += TBLK) {
            int ye = by + TBLK < h ? by + TBLK : h;
            int xe = bx + TBLK < w ? bx + TBLK : w;
            for (int y = by; y < ye; y++)
                for (int x = bx; x < xe; x++)
                    dst[(size_t)x * h + y] = src[(size_t)y * w + x];
        }
}

void draw_blur_region(surface *s, rect r, int radius)
{
    if (radius < 1) return;
    if (radius > BLUR_MAX_RADIUS) radius = BLUR_MAX_RADIUS;
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

    box_blur_rows(a, b, w, h, radius);      /* horizontal x3 */
    box_blur_rows(b, a, w, h, radius);
    box_blur_rows(a, b, w, h, radius);

    transpose(b, a, w, h);                  /* now h x w */

    box_blur_rows(a, b, h, w, radius);      /* "vertical" x3 */
    box_blur_rows(b, a, h, w, radius);
    box_blur_rows(a, b, h, w, radius);

    transpose(b, a, h, w);                  /* back to w x h */

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
