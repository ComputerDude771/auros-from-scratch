#include "wall.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU 6.28318530717958647692

/* ── small float helpers ─────────────────────────────────────────── */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerpf(float a, float b, float t)  { return a + (b - a) * t; }
static float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0 + 1e-9f), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

typedef struct { float r, g, b; } rgb;
static rgb unpack(uint32_t c) {
    rgb o = { ((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f };
    return o;
}
static rgb mixc(rgb a, rgb b, float t) {
    rgb o = { lerpf(a.r,b.r,t), lerpf(a.g,b.g,t), lerpf(a.b,b.b,t) }; return o;
}
/* Additive light blending drags everything toward white. Pre-saturating
 * the emitter keeps a teal ribbon reading as teal at full brightness. */
static rgb saturate(rgb c, float amt) {
    float l = 0.2126f*c.r + 0.7152f*c.g + 0.0722f*c.b;
    rgb o = { l + (c.r-l)*amt, l + (c.g-l)*amt, l + (c.b-l)*amt };
    o.r = clampf(o.r,0.f,1.f); o.g = clampf(o.g,0.f,1.f); o.b = clampf(o.b,0.f,1.f);
    return o;
}

/* ── deterministic value noise (no rand(), so a theme always renders
 *    byte-identical — handy for reproducible ISO builds) ─────────── */
static float hash1i(int x, int s) {
    uint32_t n = (uint32_t)x * 374761393u + (uint32_t)s * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return (float)((n ^ (n >> 16)) & 0xFFFFFF) / 16777215.f;
}
static float hash2i(int x, int y, int s) {
    uint32_t n = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)s * 83492791u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return (float)((n ^ (n >> 16)) & 0xFFFFFF) / 16777215.f;
}
static float noise1(float x, int s) {
    int i = (int)floorf(x); float f = x - i;
    f = f * f * (3.f - 2.f * f);
    return lerpf(hash1i(i, s), hash1i(i + 1, s), f);
}
static float noise2(float x, float y, int s) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx*fx*(3.f-2.f*fx); fy = fy*fy*(3.f-2.f*fy);
    float a = hash2i(xi,   yi,   s), b = hash2i(xi+1, yi,   s);
    float c = hash2i(xi,   yi+1, s), d = hash2i(xi+1, yi+1, s);
    return lerpf(lerpf(a,b,fx), lerpf(c,d,fx), fy);
}
static float fbm2(float x, float y, int oct, int s) {
    float v = 0.f, amp = 0.5f, fr = 1.f;
    for (int i = 0; i < oct; i++) { v += amp * noise2(x*fr, y*fr, s+i*31); fr *= 2.03f; amp *= 0.5f; }
    return v;
}

typedef struct {
    rgb c1, c2, c3, c4;
    float intensity, grain, vignette;
} wallcfg;

/* ═══════════════════════════════════════════════════════════════════
 *  aurora — the default. Layered sinusoidal ribbons with a gaussian
 *  cross-section and vertical curtain striation, additively blended
 *  over a cold vertical gradient.
 * ═══════════════════════════════════════════════════════════════════ */
static void style_aurora(rgb *fb, int w, int h, const wallcfg *c)
{
    const float horizon = h * 0.80f;

    /* Sky: deepest overhead, a faint wash of c2 at the horizon. */
    for (int y = 0; y < h; y++) {
        float t = clampf((float)y / horizon, 0.f, 1.f);
        rgb base = mixc(c->c1, mixc(c->c1, c->c2, 0.62f), powf(t, 1.5f));
        for (int x = 0; x < w; x++) fb[(size_t)y*w + x] = base;
    }

    /* Stars, thinning out as they approach the glow near the horizon. */
    for (int y = 0; y < (int)(horizon); y++) {
        float fade = 1.f - powf((float)y / horizon, 2.0f);
        for (int x = 0; x < w; x++) {
            float r = hash2i(x, y, 991);
            if (r > 0.99950f) {
                float b = (r - 0.99950f) / 0.00050f;
                rgb *p = &fb[(size_t)y*w + x];
                float i = b * 0.85f * fade;
                p->r = clampf(p->r + i*0.88f, 0.f, 1.f);
                p->g = clampf(p->g + i*0.92f, 0.f, 1.f);
                p->b = clampf(p->b + i*1.00f, 0.f, 1.f);
            }
        }
    }

    struct { float ybase, amp, freq, phase, thick, bright; rgb col; } rib[5] = {
        { 0.36f, 0.070f, 1.7f, 0.00f, 0.030f, 1.00f, {0,0,0} },
        { 0.45f, 0.095f, 1.1f, 2.10f, 0.055f, 0.62f, {0,0,0} },
        { 0.29f, 0.055f, 2.6f, 4.30f, 0.020f, 0.78f, {0,0,0} },
        { 0.52f, 0.105f, 0.8f, 1.15f, 0.080f, 0.26f, {0,0,0} },
        { 0.40f, 0.045f, 3.4f, 5.60f, 0.014f, 0.55f, {0,0,0} },
    };
    rib[0].col = saturate(c->c3, 1.45f);
    rib[1].col = saturate(mixc(c->c3, c->c4, 0.55f), 1.40f);
    rib[2].col = saturate(c->c4, 1.45f);
    rib[3].col = saturate(mixc(c->c2, c->c3, 0.75f), 1.30f);
    rib[4].col = saturate(mixc(c->c4, c->c3, 0.30f), 1.40f);

    for (int r = 0; r < 5; r++) {
        for (int x = 0; x < w; x++) {
            float t = (float)x / w;
            float cy = rib[r].ybase
                     + rib[r].amp * (0.60f*sinf(t*TAU*rib[r].freq       + rib[r].phase)
                                   + 0.28f*sinf(t*TAU*rib[r].freq*2.31f + rib[r].phase*1.7f)
                                   + 0.12f*sinf(t*TAU*rib[r].freq*4.07f + rib[r].phase*2.9f));
            cy *= h;
            float th = rib[r].thick * h * (0.62f + 0.38f*noise1(t*3.1f + r*11.3f, 17));
            if (th < 1.f) th = 1.f;

            float env = smoothstep(0.f, 0.16f, t) * smoothstep(1.f, 0.84f, t);
            env *= 0.40f + 0.60f * noise1(t*2.4f + r*5.7f, 43);
            float striae = 0.55f + 0.45f * noise1(t*w*0.020f + r*7.1f, 71);
            float amt = rib[r].bright * env * striae * c->intensity;
            if (amt <= 0.001f) continue;

            int y0 = (int)(cy - th*4.0f), y1 = (int)(cy + th*4.0f);
            if (y0 < 0) y0 = 0;
            if (y1 >= h) y1 = h - 1;
            for (int y = y0; y <= y1; y++) {
                float d = ((float)y - cy) / th;
                /* Gaussian core plus a wide, weak skirt: real curtains
                 * have a bright filament inside a soft halo. */
                float i = (expf(-d*d) * 0.80f + expf(-d*d*0.10f) * 0.20f) * amt;
                if (d < 0) i *= 1.f + 0.30f*(-d);        /* brighter upper edge */
                rgb *p = &fb[(size_t)y*w + x];
                p->r = clampf(p->r + rib[r].col.r * i, 0.f, 1.f);
                p->g = clampf(p->g + rib[r].col.g * i, 0.f, 1.f);
                p->b = clampf(p->b + rib[r].col.b * i, 0.f, 1.f);
            }
        }
    }

    /* Layered ridge silhouettes.
     *
     * Ridged noise -- 1 - |2n-1| -- turns smooth fbm into sharp crests,
     * which is what makes this read as mountains rather than a sine
     * wave. Two layers at different scales and darknesses give depth;
     * the near layer is almost black so the eye reads it as closest. */
    rgb far_ridge  = mixc(c->c1, (rgb){0,0,0}, 0.30f);
    rgb near_ridge = mixc(c->c1, (rgb){0,0,0}, 0.68f);

    for (int x = 0; x < w; x++) {
        float t = (float)x / w;

        float nf = fbm2(t*2.6f, 0.5f, 5, 301);
        float rf = 1.f - fabsf(2.f*nf - 1.f);
        rf = powf(clampf(rf, 0.f, 1.f), 0.75f);
        float far_y = h*0.815f - h*0.150f*rf;

        float nn = fbm2(t*1.5f + 11.f, 2.5f, 5, 401);
        float rn = 1.f - fabsf(2.f*nn - 1.f);
        rn = powf(clampf(rn, 0.f, 1.f), 0.85f);
        float near_y = h*0.935f - h*0.115f*rn;

        /* Rim light: only where a crest sits under a bright patch of
         * sky, and only a couple of pixels deep. */
        int sample = (int)clampf(far_y - 6.f, 0.f, (float)h-1);
        rgb above = fb[(size_t)sample*w + x];
        float glow = clampf((above.r + above.g + above.b) / 1.4f, 0.f, 1.f);

        for (int y = (int)far_y; y < h; y++) {
            if (y < 0) continue;
            float rim = smoothstep(far_y + 3.0f, far_y - 0.5f, (float)y);
            fb[(size_t)y*w + x] = mixc(far_ridge, saturate(c->c3, 1.2f),
                                       rim * 0.22f * glow);
        }
        for (int y = (int)near_y; y < h; y++) {
            if (y < 0) continue;
            fb[(size_t)y*w + x] = near_ridge;
        }
    }
}

/* ── mesh: inverse-square weighted colour blobs, very soft ───────── */
static void style_mesh(rgb *fb, int w, int h, const wallcfg *c)
{
    struct { float x, y, r; rgb col; } pt[6] = {
        { 0.12f, 0.16f, 0.55f, {0,0,0} }, { 0.86f, 0.10f, 0.48f, {0,0,0} },
        { 0.72f, 0.78f, 0.62f, {0,0,0} }, { 0.22f, 0.88f, 0.52f, {0,0,0} },
        { 0.52f, 0.42f, 0.70f, {0,0,0} }, { 0.95f, 0.50f, 0.40f, {0,0,0} },
    };
    /* The two base colours anchor the field; c3/c4 are accents whose
     * presence is scaled by wall_intensity, so a restrained light theme
     * and a loud dark one can share this style. */
    float k = clampf(c->intensity, 0.f, 1.f);
    pt[0].col = c->c1;
    pt[1].col = mixc(c->c1, mixc(c->c2, c->c3, 0.35f), 0.55f + 0.45f*k);
    pt[2].col = mixc(c->c2, c->c3, k);
    pt[3].col = c->c2;
    pt[4].col = mixc(c->c1, c->c2, 0.5f);
    pt[5].col = mixc(c->c2, c->c4, k);
    pt[2].r *= 0.55f + 0.45f*k;
    pt[5].r *= 0.55f + 0.45f*k;

    for (int y = 0; y < h; y++) {
        float fy = (float)y / h;
        for (int x = 0; x < w; x++) {
            float fx = (float)x / w;
            /* Warp the sample point with fbm so the blobs are organic
             * rather than obviously elliptical. */
            float wx = fx + 0.10f * (fbm2(fx*2.4f, fy*2.4f, 4, 7)  - 0.5f);
            float wy = fy + 0.10f * (fbm2(fx*2.4f + 9.f, fy*2.4f, 4, 23) - 0.5f);
            float tw = 0.f; rgb acc = {0,0,0};
            for (int i = 0; i < 6; i++) {
                float dx = (wx - pt[i].x), dy = (wy - pt[i].y) * 0.85f;
                float d2 = dx*dx + dy*dy;
                float wgt = pt[i].r / (d2 * 6.5f + 0.045f);
                acc.r += pt[i].col.r * wgt; acc.g += pt[i].col.g * wgt; acc.b += pt[i].col.b * wgt;
                tw += wgt;
            }
            acc.r /= tw; acc.g /= tw; acc.b /= tw;
            fb[(size_t)y*w + x] = acc;
        }
    }
}

/* ── waves: a synthwave horizon — sun, scanlines, perspective grid ─ */
static void style_waves(rgb *fb, int w, int h, const wallcfg *c)
{
    float hor = h * 0.62f;
    rgb sky_top = c->c1, sky_bot = mixc(c->c2, c->c3, 0.30f);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            rgb v;
            if (y < hor) {
                float t = (float)y / hor;
                v = mixc(sky_top, sky_bot, powf(t, 1.6f));
            } else {
                float t = ((float)y - hor) / (h - hor);
                v = mixc(mixc(c->c1, c->c2, 0.55f), c->c1, powf(t, 0.6f));
            }
            fb[(size_t)y*w + x] = v;
        }
    }

    /* Sun: vertical c4 -> c3 gradient, sliced by widening bands. */
    float sx = w * 0.5f, sy = hor - h*0.055f, sr = h * 0.20f;
    for (int y = (int)(sy - sr); y <= (int)(sy + sr); y++) {
        if (y < 0 || y >= h) continue;
        for (int x = (int)(sx - sr); x <= (int)(sx + sr); x++) {
            if (x < 0 || x >= w) continue;
            float dx = (x - sx) / sr, dy = (y - sy) / sr;
            float d = sqrtf(dx*dx + dy*dy);
            if (d > 1.f) continue;
            float g = clampf((y - (sy - sr)) / (2*sr), 0.f, 1.f);
            rgb col = mixc(c->c4, c->c3, g);
            /* Horizontal cut bands, thicker toward the bottom. */
            float band = (y - (sy - sr)) / (h * 0.030f);
            float bf = band - floorf(band);
            float cut = smoothstep(0.f, 0.45f, g) * 0.92f;
            if (bf < cut * 0.75f && g > 0.34f) continue;
            float edge = smoothstep(1.f, 0.90f, d);
            rgb *p = &fb[(size_t)y*w + x];
            *p = mixc(*p, col, edge);
        }
    }

    /* Horizon glow. */
    for (int y = (int)(hor - h*0.05f); y < (int)(hor + h*0.05f); y++) {
        if (y < 0 || y >= h) continue;
        float d = fabsf((float)y - hor) / (h*0.05f);
        float i = (1.f - d) * 0.55f * c->intensity;
        for (int x = 0; x < w; x++) {
            rgb *p = &fb[(size_t)y*w + x];
            *p = mixc(*p, c->c3, i);
        }
    }

    /* Perspective grid below the horizon.
     *
     * Screen row -> ground depth z = 1/t. Lines sit at integer world
     * coordinates; the distance to the nearest one is compared against
     * that coordinate's per-pixel derivative, which keeps every line a
     * constant ~1.5px wide instead of aliasing into moire near the
     * horizon. Distance is measured with roundf, which is symmetric
     * about zero -- fmodf is not, and left a visible seam down x = w/2. */
    const float ZK = 4.0f, XK = 7.0f;   /* world units across the frame */
    for (int y = (int)hor + 1; y < h; y++) {
        float t = ((float)y - hor) / (h - hor);
        float z = 1.f / (t + 0.035f);
        float fade = smoothstep(0.f, 0.10f, t) * (1.f - t*0.18f);
        if (fade <= 0.f) continue;

        /* d(z*ZK)/dy, for the rail line width. */
        float dzdy = z * z * ZK / (h - hor);
        float zr   = z * ZK;
        float drail = fabsf(zr - roundf(zr));
        /* Past Nyquist the lines are closer than a pixel; fading them
         * out there is what stops the near-horizon moire. */
        float rl = smoothstep(dzdy * 1.5f, 0.f, drail) * fade
                 * (1.f - smoothstep(0.07f, 0.19f, dzdy));

        float dpdx = z * XK * (2.f / w);              /* d(px)/dx */
        for (int x = 0; x < w; x++) {
            float fx = ((float)x - w*0.5f) / (w*0.5f);
            float px = fx * z * XK;
            float dvert = fabsf(px - roundf(px));
            float vl = smoothstep(dpdx * 1.5f, 0.f, dvert) * fade
                     * (1.f - smoothstep(0.07f, 0.19f, dpdx));

            float i = clampf(rl + vl, 0.f, 1.f) * 0.95f * c->intensity;
            if (i <= 0.002f) continue;
            rgb *p = &fb[(size_t)y*w + x];
            *p = mixc(*p, saturate(c->c3, 1.25f), i);
        }
    }
}

static void style_gradient(rgb *fb, int w, int h, const wallcfg *c)
{
    for (int y = 0; y < h; y++) {
        float fy = (float)y / h;
        for (int x = 0; x < w; x++) {
            float fx = (float)x / w;
            float t = clampf((fx * 0.62f + fy * 0.38f), 0.f, 1.f);
            rgb v = mixc(c->c1, c->c2, smoothstep(0.f, 1.f, t));
            float d1 = hypotf((fx-0.22f)*1.0f, (fy-0.18f)*1.3f);
            float d2 = hypotf((fx-0.82f)*1.0f, (fy-0.86f)*1.3f);
            v = mixc(v, c->c3, expf(-d1*d1*5.5f) * 0.42f * c->intensity);
            v = mixc(v, c->c4, expf(-d2*d2*6.5f) * 0.32f * c->intensity);
            fb[(size_t)y*w + x] = v;
        }
    }
}

static void style_noise(rgb *fb, int w, int h, const wallcfg *c)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float fx = (float)x / w * 3.2f, fy = (float)y / h * 3.2f;
            float n = fbm2(fx, fy, 6, 5);
            n = clampf((n - 0.25f) / 0.5f, 0.f, 1.f);
            rgb v = n < 0.5f ? mixc(c->c1, c->c2, n * 2.f)
                             : mixc(c->c2, c->c3, (n - 0.5f) * 2.f * c->intensity);
            fb[(size_t)y*w + x] = v;
        }
    }
}

static void style_solid(rgb *fb, int w, int h, const wallcfg *c)
{
    for (int i = 0; i < w*h; i++) fb[i] = c->c1;
}

/* ── shared post-processing ──────────────────────────────────────── */
static void post(rgb *fb, int w, int h, const wallcfg *c)
{
    for (int y = 0; y < h; y++) {
        float fy = ((float)y / h - 0.5f) * 2.f;
        for (int x = 0; x < w; x++) {
            float fx = ((float)x / w - 0.5f) * 2.f;
            rgb *p = &fb[(size_t)y*w + x];

            if (c->vignette > 0.f) {
                float d = sqrtf(fx*fx*0.78f + fy*fy);
                float v = 1.f - c->vignette * smoothstep(0.35f, 1.45f, d);
                p->r *= v; p->g *= v; p->b *= v;
            }
            if (c->grain > 0.f) {
                /* Ordered-ish dither: kills the banding that 8-bit
                 * gradients otherwise show on a large display. */
                float n = (hash2i(x, y, 1337) - 0.5f) * c->grain;
                p->r += n; p->g += n; p->b += n;
            }
            p->r = clampf(p->r, 0.f, 1.f);
            p->g = clampf(p->g, 0.f, 1.f);
            p->b = clampf(p->b, 0.f, 1.f);
        }
    }
}

void wall_render(uint32_t *px, int w, int h, const theme_t *t)
{
    wallcfg c;
    c.c1 = unpack(theme_color(t, "wall_c1", 0x0B0E14));
    c.c2 = unpack(theme_color(t, "wall_c2", 0x14303A));
    c.c3 = unpack(theme_color(t, "wall_c3", 0x7DD3C0));
    c.c4 = unpack(theme_color(t, "wall_c4", 0xA78BFA));
    c.intensity = (float)theme_num(t, "wall_intensity", 0.55);
    c.grain     = (float)theme_num(t, "wall_grain",     0.05);
    c.vignette  = (float)theme_num(t, "wall_vignette",  0.45);

    const char *style = theme_str(t, "wall_style", "aurora");
    rgb *fb = malloc((size_t)w * h * sizeof *fb);
    if (!fb) return;

    if      (!strcmp(style, "mesh"))     style_mesh(fb, w, h, &c);
    else if (!strcmp(style, "waves"))    style_waves(fb, w, h, &c);
    else if (!strcmp(style, "gradient")) style_gradient(fb, w, h, &c);
    else if (!strcmp(style, "noise"))    style_noise(fb, w, h, &c);
    else if (!strcmp(style, "solid"))    style_solid(fb, w, h, &c);
    else                                 style_aurora(fb, w, h, &c);

    post(fb, w, h, &c);

    for (int i = 0; i < w*h; i++) {
        uint32_t r = (uint32_t)(fb[i].r * 255.f + 0.5f);
        uint32_t g = (uint32_t)(fb[i].g * 255.f + 0.5f);
        uint32_t b = (uint32_t)(fb[i].b * 255.f + 0.5f);
        px[i] = (r << 16) | (g << 8) | b;
    }
    free(fb);
}
