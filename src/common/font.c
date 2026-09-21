#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Horizontal subpixel positions the cache keeps per codepoint.
 *
 * Snapping every glyph to a whole pixel is crisper per glyph but makes
 * inter-letter gaps wobble by up to a pixel, which at 12px reads as
 * uneven colour along a line of text. Four quarter-pixel phases costs
 * 4x the cache and removes the wobble; it is the same trade every
 * unhinted renderer makes. */
#define SUBPX      4
#define CACHE_BINS 512

/* Sanity ceilings. None of these are format limits -- they exist so a
 * corrupt or hostile font cannot talk us into a gigabyte allocation. */
#define MAX_FILE   (64u << 20)
#define MAX_GLYPHS 65536
#define MAX_POINTS 20000        /* per simple glyph */
#define MAX_PATH   200000       /* flattened points per glyph */
#define MAX_DIM    4096         /* glyph bitmap edge, px */
#define MAX_DEPTH  6            /* composite nesting */

/* ── bounds-checked big-endian reader ─────────────────────────────
 * Every table offset, glyph index and array length below comes out of
 * the file, so nothing may be trusted. Reads latch `bad` instead of
 * returning an error at each call site: the parsers stay readable, and
 * a single check after a run of reads catches any overrun. A failed
 * read yields 0, which for a length or a count means "empty" and so
 * fails safe on its own. */
typedef struct {
    const uint8_t *b;
    size_t         n;     /* total file size */
    size_t         p;     /* cursor, always <= n */
    int            bad;
} rd;

static rd rd_at(const uint8_t *b, size_t n, size_t off)
{
    rd r = { b, n, off, 0 };
    if (off > n) { r.p = n; r.bad = 1; }
    return r;
}
static int rd_have(rd *r, size_t k)
{
    if (r->bad || k > r->n - r->p) { r->bad = 1; return 0; }
    return 1;
}
static uint32_t ru8(rd *r)
{
    if (!rd_have(r, 1)) return 0;
    return r->b[r->p++];
}
static uint32_t ru16(rd *r)
{
    if (!rd_have(r, 2)) return 0;
    uint32_t v = ((uint32_t)r->b[r->p] << 8) | r->b[r->p + 1];
    r->p += 2; return v;
}
static int32_t rs16(rd *r) { return (int16_t)ru16(r); }
static uint32_t ru32(rd *r)
{
    if (!rd_have(r, 4)) return 0;
    uint32_t v = ((uint32_t)r->b[r->p] << 24) | ((uint32_t)r->b[r->p+1] << 16)
               | ((uint32_t)r->b[r->p+2] <<  8) |  (uint32_t)r->b[r->p+3];
    r->p += 4; return v;
}
static void rd_skip(rd *r, size_t k) { if (rd_have(r, k)) r->p += k; }
static void rd_to(rd *r, size_t off)
{
    if (off > r->n) { r->bad = 1; r->p = r->n; } else r->p = off;
}
/* F2Dot14: composite scale factors. 0x4000 == 1.0. */
static float rf2dot14(rd *r) { return (float)rs16(r) * (1.0f / 16384.0f); }

/* ── cached glyph ────────────────────────────────────────────────── */
typedef struct glyph {
    struct glyph *next;
    uint32_t key;          /* codepoint * SUBPX + phase */
    uint16_t gid;
    uint16_t bw, bh;       /* coverage bitmap size, 0 for blank glyphs */
    int16_t  x0, y0;       /* bitmap origin relative to (pen, baseline) */
    float    adv;          /* advance in px; independent of the phase */
    uint8_t *cov;          /* bw*bh, 0..255, or NULL */
} glyph;

struct font {
    uint8_t *data;
    size_t   size;

    size_t   glyf, glyf_len;
    size_t   loca, loca_len;
    size_t   hmtx, hmtx_len;
    size_t   cmap_sub;     /* absolute offset of the chosen subtable */
    size_t   kern_pairs;   /* absolute offset of the format-0 pair array */
    uint32_t kern_n;

    int      nglyphs, nhmetrics, loca_long;
    float    upem, scale, px;
    float    ascent, descent, line_gap;

    glyph   *bin[CACHE_BINS];
};

/* ── affine transform, row-vector convention ──────────────────────
 * (x,y) -> (a*x + c*y + e, b*x + d*y + f). Composites nest, so this
 * carries the accumulated scale/flip down the recursion instead of a
 * second pass over the point list. */
typedef struct { float a, b, c, d, e, f; } xform;

/* `k` applied first, then `p`. */
static xform xf_mul(xform k, xform p)
{
    xform o;
    o.a = k.a*p.a + k.b*p.c;  o.b = k.a*p.b + k.b*p.d;
    o.c = k.c*p.a + k.d*p.c;  o.d = k.c*p.b + k.d*p.d;
    o.e = k.e*p.a + k.f*p.c + p.e;
    o.f = k.e*p.b + k.f*p.d + p.f;
    return o;
}

/* ── flattened outline ───────────────────────────────────────────── */
typedef struct { float x, y; } pt;
typedef struct {
    pt   *p;   int np, pcap;
    int  *end; int ne, ecap;   /* one-past-last index of each contour */
    int   start;               /* first point of the contour in progress */
    int   oom;
} path;

static int path_add(path *P, float x, float y)
{
    if (P->oom) return 0;
    if (P->np >= P->pcap) {
        int cap = P->pcap ? P->pcap * 2 : 128;
        if (cap > MAX_PATH) { P->oom = 1; return 0; }
        pt *q = realloc(P->p, (size_t)cap * sizeof *q);
        if (!q) { P->oom = 1; return 0; }
        P->p = q; P->pcap = cap;
    }
    /* Drop exact duplicates: they contribute nothing and only cost the
     * rasteriser a zero-height edge test. */
    if (P->np > P->start && P->p[P->np-1].x == x && P->p[P->np-1].y == y) return 1;
    P->p[P->np].x = x; P->p[P->np].y = y; P->np++;
    return 1;
}
static void path_close(path *P)
{
    if (P->oom) return;
    if (P->np - P->start < 3) { P->np = P->start; return; }   /* degenerate */
    if (P->ne >= P->ecap) {
        int cap = P->ecap ? P->ecap * 2 : 16;
        int *q = realloc(P->end, (size_t)cap * sizeof *q);
        if (!q) { P->oom = 1; return; }
        P->end = q; P->ecap = cap;
    }
    P->end[P->ne++] = P->np;
    P->start = P->np;
}
static void path_free(path *P) { free(P->p); free(P->end); }

/* Flatten a quadratic to line segments.
 *
 * A quadratic deviates from its chord by |P0 - 2P1 + P2| / 8, and that
 * error falls as 1/n^2 under uniform subdivision, so n = sqrt(d/(4*tol))
 * hits the tolerance with the fewest segments. At tol = 1/16 px the
 * facets are far below what 8-bit coverage can show. */
static void path_quad(path *P, float x0, float y0, float cx, float cy,
                      float x1, float y1)
{
    float ax = x0 - 2.0f*cx + x1, ay = y0 - 2.0f*cy + y1;
    float d  = sqrtf(ax*ax + ay*ay);
    int   n  = (int)ceilf(sqrtf(d * 4.0f));
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        path_add(P, u*u*x0 + 2.0f*u*t*cx + t*t*x1,
                    u*u*y0 + 2.0f*u*t*cy + t*t*y1);
    }
}

/* ── sfnt table directory ────────────────────────────────────────── */
static uint32_t tag4(const char *s)
{
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16)
         | ((uint32_t)(uint8_t)s[2] <<  8) |  (uint32_t)(uint8_t)s[3];
}

static int find_table(const uint8_t *b, size_t n, size_t dir, uint32_t want,
                      size_t *off, size_t *len)
{
    rd r = rd_at(b, n, dir + 4);
    uint32_t nt = ru16(&r);
    rd_skip(&r, 6);
    if (r.bad || nt > 512) return 0;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t tg = ru32(&r);
        rd_skip(&r, 4);                       /* checksum */
        uint32_t to = ru32(&r), tl = ru32(&r);
        if (r.bad) return 0;
        if (tg != want) continue;
        /* A table claiming to run past EOF is the single most common
         * shape of a truncated font. Refuse it rather than clamping:
         * half a `loca` is worse than none. */
        if (to > n || tl > n - to) return 0;
        *off = to; *len = tl;
        return 1;
    }
    return 0;
}

/* ── cmap ────────────────────────────────────────────────────────── */

/* Prefer a full-Unicode subtable, then BMP, and accept the deprecated
 * platform-0 encodings as a fallback -- some older free fonts ship
 * nothing else. Ranking numerically keeps the "best so far" loop short. */
static int cmap_rank(uint32_t plat, uint32_t enc, uint32_t fmt)
{
    if (fmt != 0 && fmt != 4 && fmt != 6 && fmt != 12) return 0;
    if (plat == 3 && enc == 10) return 5;                /* MS UCS-4 */
    if (plat == 0 && enc >= 4)  return 4;                /* Unicode >= 2.0 full */
    if (plat == 3 && enc == 1)  return 3;                /* MS BMP */
    if (plat == 0)              return 2;                /* Unicode BMP */
    if (plat == 3 && enc == 0)  return 1;                /* MS Symbol */
    return 0;
}

static void cmap_pick(font *f, size_t off, size_t len)
{
    rd r = rd_at(f->data, f->size, off);
    rd_skip(&r, 2);
    uint32_t n = ru16(&r);
    if (r.bad || n > 64) return;
    int best = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t plat = ru16(&r), enc = ru16(&r), sub = ru32(&r);
        if (r.bad) return;
        if (sub >= len) continue;
        rd s = rd_at(f->data, f->size, off + sub);
        uint32_t fmt = ru16(&s);
        if (s.bad) continue;
        int rank = cmap_rank(plat, enc, fmt);
        if (rank > best) { best = rank; f->cmap_sub = off + sub; }
    }
}

static int cmap_fmt4(font *f, size_t sub, uint32_t cp)
{
    if (cp > 0xFFFF) return 0;
    rd r = rd_at(f->data, f->size, sub + 6);
    uint32_t segx2 = ru16(&r);
    if (r.bad || segx2 < 2 || (segx2 & 1)) return 0;
    uint32_t seg = segx2 / 2;

    size_t ends    = sub + 14;
    size_t starts  = ends   + segx2 + 2;      /* +2 skips reservedPad */
    size_t deltas  = starts + segx2;
    size_t ranges  = deltas + segx2;
    if (ranges + segx2 > f->size) return 0;

    /* endCode[] is sorted, so binary search for the first segment whose
     * end is >= cp. The spec's searchRange fields describe the same
     * search; recomputing it is one line and cannot be poisoned. */
    uint32_t lo = 0, hi = seg;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        rd e = rd_at(f->data, f->size, ends + mid*2);
        if (ru16(&e) < cp) lo = mid + 1; else hi = mid;
    }
    if (lo >= seg) return 0;

    rd s = rd_at(f->data, f->size, starts + lo*2);
    uint32_t start = ru16(&s);
    if (cp < start || s.bad) return 0;

    rd d = rd_at(f->data, f->size, deltas + lo*2);
    uint32_t delta = ru16(&d);
    rd o = rd_at(f->data, f->size, ranges + lo*2);
    uint32_t ro = ru16(&o);
    if (o.bad) return 0;

    if (ro == 0) return (int)((cp + delta) & 0xFFFF);

    /* idRangeOffset is a byte offset from its own slot, which is the
     * one genuinely hostile bit of format 4: the target can land
     * anywhere, so it gets its own range check. */
    size_t gi = ranges + lo*2 + ro + (size_t)(cp - start) * 2;
    rd g = rd_at(f->data, f->size, gi);
    uint32_t id = ru16(&g);
    if (g.bad || id == 0) return 0;
    return (int)((id + delta) & 0xFFFF);
}

static int cmap_fmt12(font *f, size_t sub, uint32_t cp)
{
    rd r = rd_at(f->data, f->size, sub + 12);
    uint32_t n = ru32(&r);
    if (r.bad || n > (f->size - r.p) / 12) return 0;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        rd g = rd_at(f->data, f->size, sub + 16 + (size_t)mid * 12);
        uint32_t s = ru32(&g), e = ru32(&g), gi = ru32(&g);
        if (g.bad) return 0;
        if (cp < s) hi = mid;
        else if (cp > e) lo = mid + 1;
        else {
            uint32_t id = gi + (cp - s);
            return id < (uint32_t)f->nglyphs ? (int)id : 0;
        }
    }
    return 0;
}

static int cmap_lookup(font *f, uint32_t cp)
{
    if (!f->cmap_sub) return 0;
    rd r = rd_at(f->data, f->size, f->cmap_sub);
    uint32_t fmt = ru16(&r);
    int gid = 0;
    switch (fmt) {
    case 0: {                                  /* byte encoding */
        if (cp > 0xFF) break;
        rd g = rd_at(f->data, f->size, f->cmap_sub + 6 + cp);
        gid = (int)ru8(&g);
        if (g.bad) gid = 0;
        break;
    }
    case 4:  gid = cmap_fmt4(f, f->cmap_sub, cp); break;
    case 6: {                                  /* trimmed table */
        rd g = rd_at(f->data, f->size, f->cmap_sub + 6);
        uint32_t first = ru16(&g), cnt = ru16(&g);
        if (g.bad || cp < first || cp - first >= cnt) break;
        rd h = rd_at(f->data, f->size, f->cmap_sub + 10 + (size_t)(cp - first) * 2);
        gid = (int)ru16(&h);
        if (h.bad) gid = 0;
        break;
    }
    case 12: gid = cmap_fmt12(f, f->cmap_sub, cp); break;
    default: break;
    }
    /* Symbol subtables map the ASCII range into the F0xx private use
     * area; retrying there costs one lookup and makes icon fonts work. */
    if (!gid && cp < 0x100 && fmt == 4)
        gid = cmap_fmt4(f, f->cmap_sub, 0xF000 + cp);
    return (gid >= 0 && gid < f->nglyphs) ? gid : 0;
}

/* ── hmtx / kern ─────────────────────────────────────────────────── */
static float advance_of(font *f, int gid)
{
    if (f->nhmetrics <= 0) return 0.0f;
    /* Monospaced tails: every glyph past numberOfHMetrics shares the
     * last entry's advance and only its left bearing is stored. */
    int i = gid < f->nhmetrics ? gid : f->nhmetrics - 1;
    rd r = rd_at(f->data, f->size, f->hmtx + (size_t)i * 4);
    uint32_t a = ru16(&r);
    return r.bad ? 0.0f : (float)a * f->scale;
}

static void kern_init(font *f, size_t off, size_t len)
{
    rd r = rd_at(f->data, f->size, off);
    uint32_t ver = ru16(&r), n = ru16(&r);
    /* Apple's kern is version 0x00010000 with a 32-bit header and a
     * different subtable layout. Only the Microsoft version 0 form is
     * handled; anything else just means no kerning. */
    if (r.bad || ver != 0 || n == 0 || n > 32) return;
    size_t p = off + 4;
    for (uint32_t i = 0; i < n; i++) {
        rd s = rd_at(f->data, f->size, p);
        rd_skip(&s, 2);                        /* subtable version */
        uint32_t slen = ru16(&s), cov = ru16(&s);
        if (s.bad || slen < 14 || slen > len) return;
        /* coverage: bit 0 horizontal, bit 1 minimum (not a kern value),
         * high byte is the format. */
        if ((cov >> 8) == 0 && (cov & 1) && !(cov & 2)) {
            uint32_t np = ru16(&s);
            rd_skip(&s, 6);                    /* searchRange trio */
            if (!s.bad && np && np <= (f->size - s.p) / 6) {
                f->kern_pairs = s.p;
                f->kern_n = np;
                return;
            }
        }
        if (p + slen <= off + len) p += slen; else return;
    }
}

static float kern_pair(font *f, int left, int right)
{
    if (!f->kern_n) return 0.0f;
    uint32_t want = ((uint32_t)left << 16) | (uint32_t)right;
    uint32_t lo = 0, hi = f->kern_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        rd r = rd_at(f->data, f->size, f->kern_pairs + (size_t)mid * 6);
        uint32_t key = ((uint32_t)ru16(&r) << 16) | ru16(&r);
        int32_t  val = rs16(&r);
        if (r.bad) return 0.0f;
        if (key < want) lo = mid + 1;
        else if (key > want) hi = mid;
        else return (float)val * f->scale;
    }
    return 0.0f;
}

/* ── glyf ────────────────────────────────────────────────────────── */
static int loca_range(font *f, int gid, size_t *beg, size_t *end)
{
    if (gid < 0 || gid >= f->nglyphs) return 0;
    uint32_t a, b;
    if (f->loca_long) {
        rd r = rd_at(f->data, f->size, f->loca + (size_t)gid * 4);
        a = ru32(&r); b = ru32(&r);
        if (r.bad) return 0;
    } else {
        rd r = rd_at(f->data, f->size, f->loca + (size_t)gid * 2);
        /* The short format stores offsets halved, which is why a font
         * with an odd glyph length has to pad -- and why forgetting the
         * *2 yields glyphs that look like shredded confetti. */
        a = ru16(&r) * 2u; b = ru16(&r) * 2u;
        if (r.bad) return 0;
    }
    if (b <= a) return 0;                      /* empty glyph (space) */
    if (a > f->glyf_len || b > f->glyf_len) return 0;
    *beg = f->glyf + a; *end = f->glyf + b;
    return 1;
}

static void glyf_outline(font *f, int gid, path *P, xform t, int depth);

static void simple_glyph(font *f, rd *r, int ncont, path *P, xform t)
{
    if (ncont <= 0 || ncont > MAX_POINTS) return;

    /* endPtsOfContours is read twice: once to size the point arrays and
     * once during the contour walk. Reading it into a local array keeps
     * the second pass from re-deriving offsets. */
    uint16_t *ends = malloc((size_t)ncont * sizeof *ends);
    if (!ends) return;
    for (int i = 0; i < ncont; i++) ends[i] = (uint16_t)ru16(r);
    if (r->bad) { free(ends); return; }

    int npts = ends[ncont-1] + 1;
    if (npts <= 0 || npts > MAX_POINTS) { free(ends); return; }
    /* Contour ends must be strictly increasing; anything else makes the
     * walk below run backwards over the point array. */
    for (int i = 1; i < ncont; i++)
        if (ends[i] <= ends[i-1]) { free(ends); return; }

    rd_skip(r, ru16(r));                       /* hinting bytecode: ignored */

    uint8_t *flg = malloc((size_t)npts);
    pt      *pts = malloc((size_t)npts * sizeof *pts);
    if (!flg || !pts) { free(ends); free(flg); free(pts); return; }

    for (int i = 0; i < npts; ) {
        uint32_t fl = ru8(r);
        if (r->bad) goto done;
        flg[i++] = (uint8_t)fl;
        if (fl & 0x08) {                       /* REPEAT */
            uint32_t rep = ru8(r);
            while (rep-- && i < npts) flg[i++] = (uint8_t)fl;
        }
    }

    /* Coordinates are deltas, and the "short" and "same" bits overload
     * each other: a short coordinate uses the same-bit as its SIGN,
     * while a long one uses it to mean "delta is zero". Conflating the
     * two is the other classic way to get confetti. */
    {
        int32_t v = 0;
        for (int i = 0; i < npts; i++) {
            if (flg[i] & 0x02) { int32_t d = (int32_t)ru8(r); v += (flg[i] & 0x10) ? d : -d; }
            else if (!(flg[i] & 0x10)) v += rs16(r);
            pts[i].x = (float)v;
        }
        v = 0;
        for (int i = 0; i < npts; i++) {
            if (flg[i] & 0x04) { int32_t d = (int32_t)ru8(r); v += (flg[i] & 0x20) ? d : -d; }
            else if (!(flg[i] & 0x20)) v += rs16(r);
            pts[i].y = (float)v;
        }
    }
    if (r->bad) goto done;

    /* Transform once, here: midpoints and Bezier evaluation are affine,
     * so doing the whole contour walk in device space is equivalent and
     * lets the flattening tolerance be expressed in pixels. */
    for (int i = 0; i < npts; i++) {
        float x = pts[i].x, y = pts[i].y;
        pts[i].x = t.a*x + t.c*y + t.e;
        pts[i].y = t.b*x + t.d*y + t.f;
    }

    for (int c = 0, s = 0; c < ncont; s = ends[c] + 1, c++) {
        int e = ends[c], n = e - s + 1;
        if (n < 2) continue;

        /* Pick a starting on-curve point. TrueType allows a contour to
         * begin (and consist entirely) of off-curve control points; in
         * that case the true start is the implied midpoint between the
         * last and first controls. Getting this wrong rotates the whole
         * contour by one segment and rounds off one corner of every
         * rectangle in the font. */
        pt start;
        int first, count;
        if (flg[s] & 1)        { start = pts[s]; first = s + 1; count = n - 1; }
        else if (flg[e] & 1)   { start = pts[e]; first = s;     count = n - 1; }
        else {
            start.x = 0.5f * (pts[s].x + pts[e].x);
            start.y = 0.5f * (pts[s].y + pts[e].y);
            first = s; count = n;
        }

        path_add(P, start.x, start.y);
        pt cur = start, ctl = {0, 0};
        int have_ctl = 0;

        for (int k = 0; k < count; k++) {
            int i = s + ((first - s) + k) % n;
            pt  p = pts[i];
            if (flg[i] & 1) {
                if (have_ctl) path_quad(P, cur.x, cur.y, ctl.x, ctl.y, p.x, p.y);
                else          path_add(P, p.x, p.y);
                cur = p; have_ctl = 0;
            } else if (have_ctl) {
                /* Two controls in a row: the on-curve point between
                 * them is IMPLIED at their midpoint. */
                pt mid = { 0.5f*(ctl.x + p.x), 0.5f*(ctl.y + p.y) };
                path_quad(P, cur.x, cur.y, ctl.x, ctl.y, mid.x, mid.y);
                cur = mid; ctl = p;
            } else { ctl = p; have_ctl = 1; }
        }
        if (have_ctl) path_quad(P, cur.x, cur.y, ctl.x, ctl.y, start.x, start.y);
        path_close(P);
    }

done:
    free(ends); free(flg); free(pts);
}

static void composite_glyph(font *f, rd *r, path *P, xform t, int depth)
{
    uint32_t flags;
    int guard = 0;
    do {
        if (++guard > 64) return;              /* runaway component list */
        flags = ru16(r);
        uint32_t idx = ru16(r);
        int32_t  a1, a2;
        if (flags & 0x0001) { a1 = rs16(r); a2 = rs16(r); }     /* words */
        else { a1 = (int8_t)ru8(r); a2 = (int8_t)ru8(r); }
        if (r->bad) return;

        xform c = { 1, 0, 0, 1, 0, 0 };
        if (flags & 0x0008) { c.a = c.d = rf2dot14(r); }                 /* scale */
        else if (flags & 0x0040) { c.a = rf2dot14(r); c.d = rf2dot14(r); }/* x,y scale */
        else if (flags & 0x0080) {                                       /* 2x2 */
            c.a = rf2dot14(r); c.b = rf2dot14(r);
            c.c = rf2dot14(r); c.d = rf2dot14(r);
        }
        if (r->bad) return;

        if (flags & 0x0002) {                  /* ARGS_ARE_XY_VALUES */
            /* The default (MS) reading is that the offset is NOT run
             * through the component's own scale -- it is already in the
             * parent's units. Composing it as the child's translation
             * gives exactly that; SCALED_COMPONENT_OFFSET asks for the
             * other behaviour and is vanishingly rare. */
            float dx = (float)a1, dy = (float)a2;
            if (flags & 0x0800) {
                float sx = dx, sy = dy;
                dx = c.a*sx + c.c*sy;
                dy = c.b*sx + c.d*sy;
            }
            c.e = dx; c.f = dy;
        }
        /* Else the args are point indices to be matched against the
         * already-placed components. No shipping Latin font does this,
         * and supporting it means keeping every component's untransformed
         * points alive, so the component is placed at the origin. */

        if (depth < MAX_DEPTH)
            glyf_outline(f, (int)idx, P, xf_mul(c, t), depth + 1);
    } while (flags & 0x0020);                  /* MORE_COMPONENTS */
}

static void glyf_outline(font *f, int gid, path *P, xform t, int depth)
{
    size_t beg, end;
    if (P->oom || !loca_range(f, gid, &beg, &end)) return;
    rd r = rd_at(f->data, f->size, beg);
    /* Clamp the reader to this glyph's slice so a lying length cannot
     * walk the parser into the next glyph's data. */
    if (end <= f->size) r.n = end;

    int nc = rs16(&r);
    rd_skip(&r, 8);                            /* xMin yMin xMax yMax */
    if (r.bad) return;
    if (nc >= 0) simple_glyph(f, &r, nc, P, t);
    else         composite_glyph(f, &r, P, t, depth);
}

/* ── rasteriser ───────────────────────────────────────────────────
 * Signed-area accumulation (the font-rs / FreeType-smooth family).
 *
 * For every edge we add, into one float cell per pixel, the derivative
 * of the winding-weighted coverage along the scanline: the cell the
 * edge crosses gets the exact partial area it cuts, and the cell after
 * it gets the remainder, so a running sum across the row reproduces the
 * signed winding number with exact analytic coverage at the boundary.
 * abs() then clamp implements the nonzero rule -- a counter wound the
 * other way cancels back to 0, and two overlapping contours saturate at
 * 1 instead of punching a hole the way even-odd would.
 *
 * The row stride is width+2: an edge landing on the last column writes
 * one cell past it, and that pad is never summed. Every x is clamped
 * into [0,w] first, so a wild coordinate from a corrupt glyph distorts
 * only the edge column rather than scribbling over the heap. */
static void acc_edge(float *acc, int stride, int w, int h,
                     float ax, float ay, float bx, float by)
{
    if (ay == by) return;                      /* horizontal: no winding */
    float dir = 1.0f;
    if (ay > by) {
        dir = -1.0f;
        float t;
        t = ax; ax = bx; bx = t;
        t = ay; ay = by; by = t;
    }
    if (by <= 0.0f || ay >= (float)h) return;

    float dxdy = (bx - ax) / (by - ay);
    float x = ax;
    int   y = 0;
    if (ay < 0.0f) x -= ay * dxdy;             /* walk down to y = 0 */
    else           y = (int)ay;

    /* Clamp before the cast: converting a float larger than INT_MAX is
     * undefined, and a corrupt glyph can easily produce one. */
    float bc = ceilf(by);
    int ylast = bc >= (float)h ? h : (int)bc;

    for (; y < ylast; y++) {
        float ytop = (float)y     > ay ? (float)y     : ay;
        float ybot = (float)(y+1) < by ? (float)(y+1) : by;
        float dy   = ybot - ytop;
        if (dy <= 0.0f) { continue; }
        float xn = x + dxdy * dy;
        float d  = dy * dir;

        float x0 = x < xn ? x : xn;
        float x1 = x < xn ? xn : x;
        /* Negated comparisons so a NaN -- which a degenerate slope can
         * produce -- lands on the clamp instead of slipping through to
         * an undefined float-to-int conversion below. */
        if (!(x0 >= 0.0f))       x0 = 0.0f;
        if (!(x1 >= 0.0f))       x1 = 0.0f;
        if (!(x0 <= (float)w))   x0 = (float)w;
        if (!(x1 <= (float)w))   x1 = (float)w;
        if (x1 < x0) x1 = x0;

        float *row = acc + (size_t)y * stride;
        float fl = floorf(x0), ce = ceilf(x1);
        int   i0 = (int)fl,    i1 = (int)ce;

        if (i1 <= i0 + 1) {
            /* Wholly inside one column: split by the midpoint's offset. */
            float xm = 0.5f * (x0 + x1) - fl;
            row[i0]     += d * (1.0f - xm);
            row[i0 + 1] += d * xm;
        } else {
            float s   = 1.0f / (x1 - x0);      /* dy per unit x */
            float f0  = x0 - fl;
            float A0  = 0.5f * s * (1.0f - f0) * (1.0f - f0);
            float f1  = x1 - ce + 1.0f;
            float AM  = 0.5f * s * f1 * f1;
            row[i0] += d * A0;
            if (i1 == i0 + 2) {
                row[i0 + 1] += d * (1.0f - A0 - AM);
            } else {
                float A1 = s * (1.5f - f0);
                row[i0 + 1] += d * (A1 - A0);
                for (int xi = i0 + 2; xi < i1 - 1; xi++) row[xi] += d * s;
                float A2 = A1 + (float)(i1 - i0 - 3) * s;
                row[i1 - 1] += d * (1.0f - A2 - AM);
            }
            row[i1] += d * AM;
        }
        x = xn;
    }
}

static uint8_t *rasterise(const path *P, int w, int h)
{
    size_t stride = (size_t)w + 2;
    float *acc = calloc(stride * (size_t)h, sizeof *acc);
    uint8_t *cov = calloc((size_t)w * (size_t)h, 1);
    if (!acc || !cov) { free(acc); free(cov); return NULL; }

    for (int c = 0, s = 0; c < P->ne; s = P->end[c], c++) {
        int e = P->end[c];
        for (int i = s; i < e - 1; i++)
            acc_edge(acc, (int)stride, w, h,
                     P->p[i].x, P->p[i].y, P->p[i+1].x, P->p[i+1].y);
        if (e - s >= 2)                        /* implicit close */
            acc_edge(acc, (int)stride, w, h,
                     P->p[e-1].x, P->p[e-1].y, P->p[s].x, P->p[s].y);
    }

    for (int y = 0; y < h; y++) {
        const float *row = acc + (size_t)y * stride;
        uint8_t *out = cov + (size_t)y * w;
        float sum = 0.0f;
        for (int x = 0; x < w; x++) {
            sum += row[x];
            float a = sum < 0.0f ? -sum : sum;
            if (a > 1.0f) a = 1.0f;
            out[x] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
    free(acc);
    return cov;
}

/* ── glyph cache ─────────────────────────────────────────────────── */
static glyph *glyph_build(font *f, uint32_t cp, int phase)
{
    glyph *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->key = cp * SUBPX + (uint32_t)phase;
    g->gid = (uint16_t)cmap_lookup(f, cp);
    g->adv = advance_of(f, g->gid);

    path P = {0};
    xform t = { f->scale, 0.0f, 0.0f, -f->scale,   /* y grows down on screen */
                (float)phase / (float)SUBPX, 0.0f };
    glyf_outline(f, g->gid, &P, t, 0);

    if (P.oom || P.ne == 0 || P.np == 0) { path_free(&P); return g; }

    float x0 = P.p[0].x, x1 = x0, y0 = P.p[0].y, y1 = y0;
    for (int i = 1; i < P.np; i++) {
        if (P.p[i].x < x0) x0 = P.p[i].x;
        if (P.p[i].x > x1) x1 = P.p[i].x;
        if (P.p[i].y < y0) y0 = P.p[i].y;
        if (P.p[i].y > y1) y1 = P.p[i].y;
    }
    /* NaN or an absurd bbox means the outline is junk; a blank glyph is
     * the correct rendering of junk. The magnitude test doubles as the
     * guard that makes the float-to-int casts below well defined. */
    if (!(x1 >= x0) || !(y1 >= y0) ||
        !(x0 > -1e6f) || !(x1 < 1e6f) || !(y0 > -1e6f) || !(y1 < 1e6f)) {
        path_free(&P); return g;
    }

    int bx = (int)floorf(x0) - 1, by = (int)floorf(y0) - 1;
    int bw = (int)ceilf(x1) + 1 - bx, bh = (int)ceilf(y1) + 1 - by;
    if (bw <= 0 || bh <= 0 || bw > MAX_DIM || bh > MAX_DIM) { path_free(&P); return g; }

    for (int i = 0; i < P.np; i++) { P.p[i].x -= (float)bx; P.p[i].y -= (float)by; }

    g->cov = rasterise(&P, bw, bh);
    if (g->cov) { g->bw = (uint16_t)bw; g->bh = (uint16_t)bh;
                  g->x0 = (int16_t)bx;  g->y0 = (int16_t)by; }
    path_free(&P);
    return g;
}

static glyph *glyph_get(font *f, uint32_t cp, int phase)
{
    uint32_t key = cp * SUBPX + (uint32_t)phase;
    uint32_t h = (key * 2654435761u) >> 20;
    h &= CACHE_BINS - 1;
    for (glyph *g = f->bin[h]; g; g = g->next)
        if (g->key == key) return g;
    glyph *g = glyph_build(f, cp, phase);
    if (!g) return NULL;
    g->next = f->bin[h];
    f->bin[h] = g;
    return g;
}

/* ── UTF-8 ───────────────────────────────────────────────────────── */
static uint32_t utf8_next(const char **sp)
{
    const uint8_t *p = (const uint8_t *)*sp;
    uint32_t c = p[0];
    int n;
    if (c < 0x80)            { *sp = (const char *)(p + 1); return c; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *sp = (const char *)(p + 1); return 0xFFFD; }

    /* A NUL fails the continuation test, so this never reads past the
     * end of the string even for a truncated sequence. */
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *sp = (const char *)(p + 1); return 0xFFFD; }
        c = (c << 6) | (p[i] & 0x3Fu);
    }
    *sp = (const char *)(p + n + 1);

    static const uint32_t least[4] = { 0, 0x80, 0x800, 0x10000 };
    if (c < least[n] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0xFFFD;
    return c;
}

/* ── load / free ─────────────────────────────────────────────────── */
font *font_load(const char *path, float px)
{
    if (!path || !(px >= 1.0f) || px > 2000.0f) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END)) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 12 || (unsigned long)sz > MAX_FILE) { fclose(fp); return NULL; }
    rewind(fp);

    font *f = calloc(1, sizeof *f);
    if (!f) { fclose(fp); return NULL; }
    f->data = malloc((size_t)sz);
    if (!f->data || fread(f->data, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp); free(f->data); free(f); return NULL;
    }
    fclose(fp);
    f->size = (size_t)sz;
    f->px   = px;

    /* A .ttc is a directory of table directories sharing one blob; the
     * shell only ever wants the first face. */
    size_t dir = 0;
    rd r = rd_at(f->data, f->size, 0);
    uint32_t ver = ru32(&r);
    if (ver == tag4("ttcf")) {
        rd_skip(&r, 4);
        uint32_t nf = ru32(&r);
        uint32_t off = ru32(&r);
        if (r.bad || nf == 0 || off >= f->size) goto fail;
        dir = off;
        rd d = rd_at(f->data, f->size, dir);
        ver = ru32(&d);
        if (d.bad) goto fail;
    }
    /* `OTTO` is a valid sfnt whose outlines live in a CFF table: real
     * fonts, but a completely different curve format. Rejecting here is
     * honest; silently producing blank text is not. */
    if (ver != 0x00010000u && ver != tag4("true")) goto fail;

    size_t off, len;
    if (!find_table(f->data, f->size, dir, tag4("head"), &off, &len) || len < 54) goto fail;
    {
        /* Seek to each field rather than counting skips: `head` has two
         * 8-byte LONGDATETIMEs and a bbox in the middle, and an
         * off-by-two here silently yields a nonsense indexToLocFormat. */
        rd h = rd_at(f->data, f->size, off + 12);
        if (ru32(&h) != 0x5F0F3CF5u) goto fail;          /* magicNumber */
        rd_to(&h, off + 18);
        uint32_t upem = ru16(&h);
        rd_to(&h, off + 50);
        int32_t fmt = rs16(&h);                          /* indexToLocFormat */
        if (h.bad || upem < 16 || upem > 16384 || (fmt != 0 && fmt != 1)) goto fail;
        f->upem = (float)upem;
        f->loca_long = fmt;
    }
    f->scale = px / f->upem;

    if (!find_table(f->data, f->size, dir, tag4("hhea"), &off, &len) || len < 36) goto fail;
    {
        rd h = rd_at(f->data, f->size, off + 4);
        f->ascent   =  (float)rs16(&h) * f->scale;
        f->descent  = -(float)rs16(&h) * f->scale;   /* stored negative */
        f->line_gap =  (float)rs16(&h) * f->scale;
        rd_to(&h, off + 34);
        f->nhmetrics = (int)ru16(&h);
        if (h.bad) goto fail;
    }
    /* Some fonts ship a zeroed hhea. Fall back to the classic 80/20
     * split of the em so text still lays out instead of collapsing. */
    if (!(f->ascent > 0.0f)) { f->ascent = px * 0.8f; f->descent = px * 0.2f; }
    if (!(f->descent >= 0.0f)) f->descent = 0.0f;

    if (!find_table(f->data, f->size, dir, tag4("maxp"), &off, &len) || len < 6) goto fail;
    {
        rd h = rd_at(f->data, f->size, off + 4);
        f->nglyphs = (int)ru16(&h);
        if (h.bad || f->nglyphs <= 0 || f->nglyphs > MAX_GLYPHS) goto fail;
    }

    if (!find_table(f->data, f->size, dir, tag4("hmtx"), &f->hmtx, &f->hmtx_len)) goto fail;
    if (f->nhmetrics <= 0) f->nhmetrics = 1;
    /* numberOfHMetrics is only believable if hmtx is big enough to hold
     * that many 4-byte records; clamp rather than reject, since a short
     * hmtx still renders correctly for the glyphs it does cover. */
    if ((size_t)f->nhmetrics * 4 > f->hmtx_len) f->nhmetrics = (int)(f->hmtx_len / 4);

    if (!find_table(f->data, f->size, dir, tag4("loca"), &f->loca, &f->loca_len)) goto fail;
    if (!find_table(f->data, f->size, dir, tag4("glyf"), &f->glyf, &f->glyf_len)) goto fail;
    {
        size_t need = (size_t)(f->nglyphs + 1) * (f->loca_long ? 4u : 2u);
        if (need > f->loca_len) {
            int fit = (int)(f->loca_len / (f->loca_long ? 4u : 2u)) - 1;
            if (fit <= 0) goto fail;
            f->nglyphs = fit;
        }
    }

    if (find_table(f->data, f->size, dir, tag4("cmap"), &off, &len) && len >= 4)
        cmap_pick(f, off, len);
    if (!f->cmap_sub) goto fail;               /* no way to map text to glyphs */

    /* Kerning is optional in every sense: absent, Apple-format, or
     * GPOS-only fonts all just render unkerned. */
    if (find_table(f->data, f->size, dir, tag4("kern"), &off, &len) && len >= 6)
        kern_init(f, off, len);

    return f;

fail:
    free(f->data);
    free(f);
    return NULL;
}

void font_free(font *f)
{
    if (!f) return;
    for (int i = 0; i < CACHE_BINS; i++) {
        glyph *g = f->bin[i];
        while (g) { glyph *n = g->next; free(g->cov); free(g); g = n; }
    }
    free(f->data);
    free(f);
}

float font_ascent(const font *f)      { return f ? f->ascent : 0.0f; }
float font_descent(const font *f)     { return f ? f->descent : 0.0f; }
float font_line_height(const font *f) { return f ? f->ascent + f->descent + f->line_gap : 0.0f; }

/* ── layout ──────────────────────────────────────────────────────── */
float font_text_width(font *f, const char *utf8)
{
    if (!f || !utf8) return 0.0f;
    float pen = 0.0f;
    int prev = -1;
    while (*utf8) {
        uint32_t cp = utf8_next(&utf8);
        /* Phase 0 is enough for measuring: the advance does not depend
         * on where in the pixel the glyph was rasterised. */
        glyph *g = glyph_get(f, cp, 0);
        if (!g) continue;
        if (prev >= 0) pen += kern_pair(f, prev, g->gid);
        pen += g->adv;
        prev = g->gid;
    }
    return pen;
}

void font_draw(font *f, uint32_t *px, int w, int h,
               float x, float y, const char *utf8, uint32_t color, float alpha)
{
    if (!f || !px || !utf8 || w <= 0 || h <= 0) return;
    if (!(alpha > 0.0f)) return;
    if (alpha > 1.0f) alpha = 1.0f;
    /* A NaN or wildly out-of-range pen position would make the casts
     * below undefined, and nothing it could draw would be on screen. */
    if (!(x > -1e6f && x < 1e6f) || !(y > -1e6f && y < 1e6f)) return;

    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    /* The baseline snaps to a whole pixel. Horizontal stems are what
     * the eye tracks along a line of text, and letting them straddle
     * two rows blurs every x-height and cap-height edge at once. */
    int base = (int)floorf(y + 0.5f);
    float pen = x;
    int prev = -1;

    while (*utf8) {
        uint32_t cp = utf8_next(&utf8);
        glyph *probe = glyph_get(f, cp, 0);
        if (!probe) continue;
        if (prev >= 0) pen += kern_pair(f, prev, probe->gid);
        prev = probe->gid;

        int ix = (int)floorf(pen);
        int ph = (int)((pen - (float)ix) * SUBPX + 0.5f);
        if (ph >= SUBPX) { ph = 0; ix++; }
        glyph *g = ph ? glyph_get(f, cp, ph) : probe;
        if (!g) { pen += probe->adv; continue; }

        if (g->cov) {
            int gx = ix + g->x0, gy = base + g->y0;
            int cy0 = gy < 0 ? -gy : 0, cy1 = g->bh;
            if (gy + cy1 > h) cy1 = h - gy;
            int cx0 = gx < 0 ? -gx : 0, cx1 = g->bw;
            if (gx + cx1 > w) cx1 = w - gx;

            for (int r = cy0; r < cy1; r++) {
                const uint8_t *src = g->cov + (size_t)r * g->bw;
                uint32_t *dst = px + (size_t)(gy + r) * w + gx;
                for (int c = cx0; c < cx1; c++) {
                    uint32_t a = src[c];
                    if (!a) continue;
                    a = (uint32_t)((float)a * alpha + 0.5f);
                    if (!a) continue;
                    uint32_t d = dst[c];
                    uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                    /* +127 before the divide rounds to nearest, which
                     * keeps a full-coverage pixel exactly the source
                     * colour instead of one LSB short of it. */
                    uint32_t nr = (dr * (255 - a) + sr * a + 127) / 255;
                    uint32_t ng = (dg * (255 - a) + sg * a + 127) / 255;
                    uint32_t nb = (db * (255 - a) + sb * a + 127) / 255;
                    dst[c] = (nr << 16) | (ng << 8) | nb;
                }
            }
        }
        pen += g->adv;
    }
}
