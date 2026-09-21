#include "anim.h"
#include <math.h>
#include <stddef.h>

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

float ease(ease_kind k, float t)
{
    t = clampf(t, 0.f, 1.f);
    switch (k) {
        case EASE_LINEAR:  return t;
        case EASE_OUT_CUBIC: { float u = 1.f - t; return 1.f - u*u*u; }
        case EASE_IN_OUT_CUBIC:
            return t < 0.5f ? 4.f*t*t*t : 1.f - powf(-2.f*t + 2.f, 3.f) / 2.f;
        case EASE_OUT_BACK: {
            const float c1 = 1.70158f, c3 = c1 + 1.f;
            float u = t - 1.f;
            return 1.f + c3*u*u*u + c1*u*u;
        }
        case EASE_SPRING: {
            /* Critically-ish damped: settles without a visible bounce at
             * the end, which reads as responsive rather than bouncy. A
             * real spring integrator would be frame-rate dependent. */
            if (t >= 1.f) return 1.f;
            float w = 12.0f, z = 0.78f;
            float wd = w * sqrtf(1.f - z*z);
            return 1.f - expf(-z * w * t) * (cosf(wd * t) + (z * w / wd) * sinf(wd * t));
        }
    }
    return t;
}

void tween_to(tween *t, float target, float duration_s, ease_kind k)
{
    if (duration_s <= 0.f) { t->value = t->from = t->to = target; t->active = 0; return; }
    /* Retarget mid-flight from the CURRENT value, not the old start, so
     * an interrupted animation continues smoothly instead of snapping
     * back. Users interrupt animations constantly. */
    t->from = t->value;
    t->to = target;
    t->elapsed = 0.f;
    t->duration = duration_s;
    t->kind = k;
    t->active = 1;
}

void tween_set(tween *t, float value)
{
    t->from = t->to = t->value = value;
    t->elapsed = t->duration = 0.f;
    t->active = 0;
}

int tween_step(tween *t, float dt)
{
    if (!t->active) return 0;
    t->elapsed += dt;
    float p = t->duration > 0.f ? t->elapsed / t->duration : 1.f;
    if (p >= 1.f) { t->value = t->to; t->active = 0; return 0; }
    t->value = t->from + (t->to - t->from) * ease(t->kind, p);
    return 1;
}

/* Sample src bilinearly at (fx, fy) in source pixel coordinates. */
static uint32_t sample_bilinear(const surface *s, float fx, float fy)
{
    if (fx < 0.f) fx = 0.f;
    if (fy < 0.f) fy = 0.f;
    if (fx > (float)(s->w - 1)) fx = (float)(s->w - 1);
    if (fy > (float)(s->h - 1)) fy = (float)(s->h - 1);

    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < s->w ? x0 + 1 : x0;
    int y1 = y0 + 1 < s->h ? y0 + 1 : y0;
    float tx = fx - (float)x0, ty = fy - (float)y0;

    const uint32_t *p = s->px;
    uint32_t a = p[(size_t)y0 * s->stride + x0], b = p[(size_t)y0 * s->stride + x1];
    uint32_t c = p[(size_t)y1 * s->stride + x0], d = p[(size_t)y1 * s->stride + x1];

    float out[4];
    for (int ch = 0; ch < 4; ch++) {
        int sh = ch * 8;
        float va = (float)((a >> sh) & 0xFF), vb = (float)((b >> sh) & 0xFF);
        float vc = (float)((c >> sh) & 0xFF), vd = (float)((d >> sh) & 0xFF);
        float top = va + (vb - va) * tx;
        float bot = vc + (vd - vc) * tx;
        out[ch] = top + (bot - top) * ty;
    }
    return ((uint32_t)(out[3] + 0.5f) << 24) | ((uint32_t)(out[2] + 0.5f) << 16) |
           ((uint32_t)(out[1] + 0.5f) << 8)  |  (uint32_t)(out[0] + 0.5f);
}

void draw_scaled(surface *dst, const surface *src, rect r, float alpha)
{
    if (!src || src->w <= 0 || src->h <= 0 || r.w <= 0 || r.h <= 0) return;
    float sx = (float)src->w / (float)r.w;
    float sy = (float)src->h / (float)r.h;

    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > dst->w ? dst->w : r.x + r.w;
    int y1 = r.y + r.h > dst->h ? dst->h : r.y + r.h;

    for (int y = y0; y < y1; y++) {
        float fy = ((float)y + 0.5f - (float)r.y) * sy - 0.5f;
        for (int x = x0; x < x1; x++) {
            float fx = ((float)x + 0.5f - (float)r.x) * sx - 0.5f;
            uint32_t s = sample_bilinear(src, fx, fy);
            float a = (float)((s >> 24) & 0xFF) / 255.f * alpha;
            if (a > 0.f) draw_blend_px(dst, x, y, s & 0xFFFFFFu, a);
        }
    }
}

/* Reuses the same signed distance field the rest of the rasterizer uses,
 * so a scaled view's corners match a drawn card's corners exactly --
 * mismatched radii between a card and its contents is the sort of thing
 * nobody can name but everybody sees. */
static float sdf_round_box_local(float px, float py, float hw, float hh, float r)
{
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float qx = fabsf(px) - (hw - r);
    float qy = fabsf(py) - (hh - r);
    float ax = qx > 0.f ? qx : 0.f, ay = qy > 0.f ? qy : 0.f;
    return sqrtf(ax*ax + ay*ay) + fminf(fmaxf(qx, qy), 0.f) - r;
}

void draw_scaled_rounded(surface *dst, const surface *src, rect r,
                         corners c, float alpha)
{
    if (!src || src->w <= 0 || src->h <= 0 || r.w <= 0 || r.h <= 0) return;
    float sx = (float)src->w / (float)r.w;
    float sy = (float)src->h / (float)r.h;
    float hw = r.w * 0.5f, hh = r.h * 0.5f;
    float cx = (float)r.x + hw, cy = (float)r.y + hh;

    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > dst->w ? dst->w : r.x + r.w;
    int y1 = r.y + r.h > dst->h ? dst->h : r.y + r.h;

    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f - cy;
        float fy = ((float)y + 0.5f - (float)r.y) * sy - 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f - cx;
            float rad = (py < 0.f) ? (px < 0.f ? c.tl : c.tr)
                                   : (px < 0.f ? c.bl : c.br);
            float cov = sdf_round_box_local(px, py, hw, hh, rad);
            cov = clampf(0.5f - cov, 0.f, 1.f);
            if (cov <= 0.f) continue;
            float fx = ((float)x + 0.5f - (float)r.x) * sx - 0.5f;
            uint32_t s = sample_bilinear(src, fx, fy);
            float a = (float)((s >> 24) & 0xFF) / 255.f * alpha * cov;
            if (a > 0.f) draw_blend_px(dst, x, y, s & 0xFFFFFFu, a);
        }
    }
}
