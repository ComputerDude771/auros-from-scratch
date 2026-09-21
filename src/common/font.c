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
#define GID_BINS   256

/* Coverage transfer curve.
 *
 * Exact area coverage composited in device (sRGB) space renders unhinted
 * light-on-dark UI text noticeably lighter than a hinted renderer does: a
 * stem that falls between two pixel columns becomes two half-covered
 * pixels, and two 50% greys on a dark background read as much less ink
 * than one solid pixel, because sRGB is not linear in light. Hinting is
 * how every other renderer buys that weight back; we deliberately do not
 * hint, so a mild gamma on the coverage does it instead without moving a
 * single outline point. 1.25 was picked by eye against FreeType at 12-14px
 * on the Nocturne background: enough to match its apparent weight, small
 * enough that dark-on-light text is not visibly fattened. Override with
 * -DFONT_GAMMA=1.0 to get untouched analytic coverage. */
#ifndef FONT_GAMMA
#define FONT_GAMMA 1.25f
#endif

/* Sanity ceilings. None of these are format limits -- they exist so a
 * corrupt or hostile font cannot talk us into a gigabyte allocation. */
#define MAX_FILE   (64u << 20)
#define MAX_GLYPHS 65536
#define MAX_POINTS 20000        /* per simple glyph */
#define MAX_PATH   200000       /* flattened points per glyph */
#define MAX_DIM    4096         /* glyph bitmap edge, px */
#define MAX_DEPTH  6            /* composite nesting */
#define MAX_FD     256          /* CID-keyed CFF font dicts */

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

/* ── CFF INDEX ────────────────────────────────────────────────────
 * The Compact Font Format's universal array-of-blobs container: it
 * holds the font names, the Top DICTs, the strings, the subroutines and
 * the charstrings alike. On disk it is count(u16), offSize(u8), then
 * (count + 1) offsets of offSize bytes each, then the data.
 *
 * Two traps live in those few bytes. The offsets are 1-BASED and are
 * measured from the byte *preceding* the data area, so element i starts
 * at (data - 1) + offsets[i]; reading them as 0-based shifts every blob
 * by one byte, which turns charstrings into plausible noise rather than
 * into an obvious error. And count == 0 is legal, in which case the
 * INDEX is those two bytes and nothing else -- no offSize follows, so a
 * parser that reads the third byte anyway carries on decoding whatever
 * structure came next. */
typedef struct {
    size_t   offs;    /* absolute offset of offsets[0] */
    size_t   base;    /* absolute; element i begins at base + offsets[i] */
    size_t   end;     /* absolute offset one past the whole INDEX */
    uint32_t count;
    uint32_t osz;     /* 1..4 */
} cff_index;

/* ── cached glyph ─────────────────────────────────────────────────
 * Keyed by codepoint and subpixel phase, chained in a fixed bucket
 * array, and never evicted: a shell draws from a small, stable set of
 * strings, so the cache converges to a few hundred kilobytes and stays
 * there. Text from an unbounded source would want an LRU; the shell is
 * not that, and pretending otherwise would buy complexity for nothing. */
typedef struct glyph {
    struct glyph *next;
    uint32_t key;          /* codepoint * SUBPX + phase */
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

    /* cmap lookups are a binary search over a few thousand ranges, and
     * a line of text hits the same few dozen codepoints over and over,
     * so a direct-mapped cache in front of it earns its 2 KiB. Slots
     * store codepoint+1 so a zeroed table reads as empty. */
    struct { uint32_t cp1; int32_t gid; } gc[GID_BINS];

    int      nglyphs, nhmetrics, loca_long;
    float    scale;        /* px per font unit: everything else derives */
    float    ascent, descent, line_gap;

    /* CFF: OpenType/PostScript outlines. `is_cff` selects the outline
     * loader and nothing else -- cmap, hmtx, kern, the glyph cache and
     * the rasteriser are shared with the `glyf` path, and so is every
     * function in font.h. Callers never learn which format they got. */
    int        is_cff, is_cid;
    size_t     cff, cff_len;
    cff_index  charstrings;    /* indexed by glyph id, exactly like loca */
    cff_index  gsubrs, lsubrs;
    cff_index  fdarray;        /* CID-keyed only */
    cff_index *fdsubrs;        /* CID-keyed only: local subrs per font dict */
    int        nfd;
    size_t     fdselect;       /* absolute, 0 if absent */
    size_t     charset;        /* absolute, 0 = predefined ISOAdobe */
    float      fm[6];          /* FontMatrix: charstring units -> em */

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
} outline;

static int ol_add(outline *P, float x, float y)
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
static void ol_close(outline *P)
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
static void ol_free(outline *P) { free(P->p); free(P->end); }

/* Flatten a quadratic to line segments.
 *
 * A quadratic deviates from its chord by |P0 - 2P1 + P2| / 8, and that
 * error falls as 1/n^2 under uniform subdivision, so n = sqrt(d/(4*tol))
 * hits the tolerance with the fewest segments. At tol = 1/16 px the
 * facets are far below what 8-bit coverage can show. */
static void ol_quad(outline *P, float x0, float y0, float cx, float cy,
                      float x1, float y1)
{
    float ax = x0 - 2.0f*cx + x1, ay = y0 - 2.0f*cy + y1;
    float d  = sqrtf(ax*ax + ay*ay);
    int   n  = (int)ceilf(sqrtf(d * 4.0f));
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        ol_add(P, u*u*x0 + 2.0f*u*t*cx + t*t*x1,
                    u*u*y0 + 2.0f*u*t*cy + t*t*y1);
    }
}

/* Flatten a cubic to line segments.
 *
 * CFF outlines are cubic where TrueType's are quadratic, so they need
 * their own error bound. Writing B(t) - chord(t) as t(1-t)[(1-t)a + t b]
 * with a = 3P1 - 2P0 - P3 and b = 3P2 - P0 - 2P3 makes the deviation at
 * most max(|a|,|b|)/4, and like the quadratic case it falls as 1/n^2
 * under uniform subdivision -- so n = 2 * sqrt(|a| or |b|) lands on the
 * same 1/16 px tolerance ol_quad() uses. (da/db are squared lengths, so
 * the fourth root below is the square root of the length.)
 *
 * d is clamped before the cast because a corrupt charstring can pile up
 * enough deltas to reach an infinity, and converting that to int is
 * undefined. The 64-segment cap only bites above ~1000px type, where it
 * still leaves the facets a tenth of a pixel deep. */
static void ol_cubic(outline *P, float x0, float y0, float x1, float y1,
                     float x2, float y2, float x3, float y3)
{
    float ax = 3.0f*x1 - 2.0f*x0 - x3, ay = 3.0f*y1 - 2.0f*y0 - y3;
    float bx = 3.0f*x2 - x0 - 2.0f*x3, by = 3.0f*y2 - y0 - 2.0f*y3;
    float da = ax*ax + ay*ay, db = bx*bx + by*by;
    float d  = da > db ? da : db;
    if (!(d >= 0.0f)) d = 0.0f;                /* a NaN lands here */
    if (d > 1e12f)    d = 1e12f;
    int n = (int)ceilf(sqrtf(sqrtf(d)) * 2.0f);
    if (n < 1)  n = 1;
    if (n > 64) n = 64;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float uu = u*u, tt = t*t;
        float c0 = uu*u, c1 = 3.0f*uu*t, c2 = 3.0f*u*tt, c3 = tt*t;
        ol_add(P, c0*x0 + c1*x1 + c2*x2 + c3*x3,
                  c0*y0 + c1*y1 + c2*y2 + c3*y3);
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

static int map_gid(font *f, uint32_t cp)
{
    uint32_t h = ((cp + 1u) * 2654435761u) >> 22;
    h &= GID_BINS - 1;
    if (f->gc[h].cp1 == cp + 1u) return f->gc[h].gid;
    int gid = cmap_lookup(f, cp);
    f->gc[h].cp1 = cp + 1u;
    f->gc[h].gid = gid;
    return gid;
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

static void glyf_outline(font *f, int gid, outline *P, xform t, int depth);

static void simple_glyph(rd *r, int ncont, outline *P, xform t)
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

        ol_add(P, start.x, start.y);
        pt cur = start, ctl = {0, 0};
        int have_ctl = 0;

        for (int k = 0; k < count; k++) {
            int i = s + ((first - s) + k) % n;
            pt  p = pts[i];
            if (flg[i] & 1) {
                if (have_ctl) ol_quad(P, cur.x, cur.y, ctl.x, ctl.y, p.x, p.y);
                else          ol_add(P, p.x, p.y);
                cur = p; have_ctl = 0;
            } else if (have_ctl) {
                /* Two controls in a row: the on-curve point between
                 * them is IMPLIED at their midpoint. */
                pt mid = { 0.5f*(ctl.x + p.x), 0.5f*(ctl.y + p.y) };
                ol_quad(P, cur.x, cur.y, ctl.x, ctl.y, mid.x, mid.y);
                cur = mid; ctl = p;
            } else { ctl = p; have_ctl = 1; }
        }
        if (have_ctl) ol_quad(P, cur.x, cur.y, ctl.x, ctl.y, start.x, start.y);
        ol_close(P);
    }

done:
    free(ends); free(flg); free(pts);
}

static void composite_glyph(font *f, rd *r, outline *P, xform t, int depth)
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

static void glyf_outline(font *f, int gid, outline *P, xform t, int depth)
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
    if (nc >= 0) simple_glyph(&r, nc, P, t);
    else         composite_glyph(f, &r, P, t, depth);
}

/* ── CFF: containers ──────────────────────────────────────────────
 *
 * An OpenType/PostScript font keeps its outlines in a `CFF ` table: a
 * complete Compact Font Format font-set embedded whole inside the sfnt.
 * Almost everything in it is reached by an offset from the start of
 * that table, and every one of those offsets comes from the file, so
 * the CFF table's own [base, base+len) window is the bound that every
 * read below is checked against.
 *
 * Layout, in the order the loader walks it:
 *
 *   header (hdrSize says where it ends -- it is NOT always 4)
 *   Name INDEX        -- font names; skipped, but its length is needed
 *   Top DICT INDEX    -- one DICT per font; entry 0 is the only one
 *   String INDEX      -- glyph/custom names; skipped, length needed
 *   Global Subr INDEX -- shared subroutines, referenced by callgsubr
 *   ... then wherever the Top DICT points: CharStrings, Private DICT,
 *       charset, and for CID fonts FDArray + FDSelect.
 */

/* Read an INDEX header at `pos`. See the cff_index comment for the two
 * traps (1-based offsets, the count == 0 short form). */
static int cff_index_read(font *f, size_t pos, cff_index *ix)
{
    size_t lim = f->cff + f->cff_len;
    memset(ix, 0, sizeof *ix);

    rd r = rd_at(f->data, lim, pos);
    uint32_t count = ru16(&r);
    if (r.bad) return 0;
    if (count == 0) { ix->end = r.p; return 1; }     /* two bytes and done */

    uint32_t osz = ru8(&r);
    if (r.bad || osz < 1 || osz > 4) return 0;
    size_t narr = (size_t)(count + 1) * osz;
    if (narr > lim - r.p) return 0;

    ix->count = count;
    ix->osz   = osz;
    ix->offs  = r.p;
    ix->base  = r.p + narr - 1;                      /* offsets are 1-based */

    /* offsets[count] is the total data size, and is what says where the
     * INDEX ends -- there is no length field anywhere else. */
    rd t = rd_at(f->data, lim, r.p + (size_t)count * osz);
    uint32_t last = 0;
    for (uint32_t k = 0; k < osz; k++) last = (last << 8) | ru8(&t);
    if (t.bad || last < 1 || last > lim - ix->base) return 0;
    ix->end = ix->base + last;
    return 1;
}

/* Byte range of element `i`. Reading offsets[i] and offsets[i+1] in one
 * pass works because they are adjacent; the reader is capped at the end
 * of the offset array so a huge `i` cannot walk into the data. */
static int cff_index_get(const font *f, const cff_index *ix, uint32_t i,
                         size_t *beg, size_t *end)
{
    if (i >= ix->count) return 0;
    rd r = rd_at(f->data, ix->base + 1, ix->offs + (size_t)i * ix->osz);
    uint32_t a = 0, b = 0;
    for (uint32_t k = 0; k < ix->osz; k++) a = (a << 8) | ru8(&r);
    for (uint32_t k = 0; k < ix->osz; k++) b = (b << 8) | ru8(&r);
    if (r.bad || a < 1 || b < a) return 0;
    if (b > ix->end - ix->base) return 0;
    *beg = ix->base + a;
    *end = ix->base + b;
    return 1;                                  /* beg == end: empty glyph */
}

/* Subroutine numbers are stored biased so that small negative numbers
 * reach the front of the array. The bias depends on the array's own
 * size, which means a font that lies about its subr count also shifts
 * every call in every charstring. */
static int cff_bias(uint32_t n)
{
    return n < 1240 ? 107 : n < 33900 ? 1131 : 32768;
}

/* ── CFF DICTs ────────────────────────────────────────────────────
 * A DICT is postfix: operands, then the operator they belong to. Number
 * encoding is *almost* the charstring encoding -- but b0 == 255 is a
 * reserved byte here and a 16.16 fixed-point number there, and b0 == 30
 * is a nibble-packed real here and nothing there. Sharing one number
 * reader between the two would be a bug, so there are two. */

static void dict_put(char *buf, size_t cap, size_t *n, char ch)
{
    if (*n + 1 < cap) buf[(*n)++] = ch;
}

/* Real number: BCD nibbles, 0-9 literal, a '.', b 'E', c 'E-', e '-',
 * f terminator. The loop always runs to the terminator even once the
 * text buffer is full, because stopping early would leave the DICT
 * cursor in the middle of a number. */
static double cff_real(rd *r)
{
    char   buf[64];
    size_t n = 0;
    int    done = 0, guard = 0;

    while (!done && ++guard <= 64) {
        uint32_t b = ru8(r);
        if (r->bad) break;
        for (int half = 0; half < 2 && !done; half++) {
            uint32_t v = half ? (b & 0x0F) : (b >> 4);
            switch (v) {
            case 0xA: dict_put(buf, sizeof buf, &n, '.'); break;
            case 0xB: dict_put(buf, sizeof buf, &n, 'E'); break;
            case 0xC: dict_put(buf, sizeof buf, &n, 'E');
                      dict_put(buf, sizeof buf, &n, '-'); break;
            case 0xD: break;                             /* reserved */
            case 0xE: dict_put(buf, sizeof buf, &n, '-'); break;
            case 0xF: done = 1; break;
            default:  dict_put(buf, sizeof buf, &n, (char)('0' + v)); break;
            }
        }
    }
    buf[n] = '\0';
    return strtod(buf, NULL);
}

/* Operand -> offset. Anything outside int32 (or a NaN, which a malformed
 * real can produce) is junk; -1 is the "absent" sentinel every field
 * below is initialised to, and every use is guarded by a > 0 test. */
static long cff_long(double v)
{
    if (!(v > -2147483649.0 && v < 2147483648.0)) return -1;
    return (long)v;
}

/* Only the operators this rasteriser acts on are kept. Everything else
 * -- BlueValues, StdHW, FontBBox, the name/copyright SIDs -- is hinting
 * or metadata we do not use, and is skipped by clearing the stack. */
typedef struct {
    long  charstrings, charset, fdarray, fdselect, cstype;
    long  priv_off, priv_sz, subrs;
    int   is_cid, have_fm;
    float fm[6];
} cff_dict;

static void cff_dict_init(cff_dict *d)
{
    memset(d, 0, sizeof *d);
    d->charstrings = d->fdarray = d->fdselect = d->priv_off = -1;
    d->priv_sz = d->subrs = d->cstype = -1;
    d->charset = 0;                            /* 0 = predefined ISOAdobe */
}

static int cff_dict_parse(font *f, size_t beg, size_t end, cff_dict *d)
{
    double st[48];
    int    n = 0;
    long   guard = 0;
    rd     r = rd_at(f->data, end, beg);

    while (r.p < end && !r.bad) {
        if (++guard > 100000) return 0;
        uint32_t b0 = ru8(&r);

        if (b0 == 28) { double v = (double)(int16_t)ru16(&r); if (n < 48) st[n++] = v; }
        else if (b0 == 29) { double v = (double)(int32_t)ru32(&r); if (n < 48) st[n++] = v; }
        else if (b0 == 30) { double v = cff_real(&r); if (n < 48) st[n++] = v; }
        else if (b0 >= 32 && b0 <= 246) { if (n < 48) st[n++] = (double)b0 - 139.0; }
        else if (b0 >= 247 && b0 <= 250) {
            double b1 = (double)ru8(&r);
            if (n < 48) st[n++] = ((double)b0 - 247.0) * 256.0 + b1 + 108.0;
        } else if (b0 >= 251 && b0 <= 254) {
            double b1 = (double)ru8(&r);
            if (n < 48) st[n++] = -(((double)b0 - 251.0) * 256.0) - b1 - 108.0;
        } else {
            /* Operator. 12 is an escape byte: the real opcode is the one
             * after it, and it shares numbers with the one-byte set, so
             * the two spaces are kept apart by the +1200. */
            uint32_t op = b0;
            if (b0 == 12) op = 1200 + ru8(&r);
            if (r.bad) break;
            switch (op) {
            case   15: if (n > 0) d->charset     = cff_long(st[n-1]); break;
            case   17: if (n > 0) d->charstrings = cff_long(st[n-1]); break;
            case   18: if (n > 1) { d->priv_sz  = cff_long(st[n-2]);
                                    d->priv_off = cff_long(st[n-1]); } break;
            case   19: if (n > 0) d->subrs      = cff_long(st[n-1]); break;
            case 1206: if (n > 0) d->cstype     = cff_long(st[n-1]); break;
            case 1207: if (n >= 6) {
                           for (int i = 0; i < 6; i++) d->fm[i] = (float)st[i];
                           d->have_fm = 1;
                       } break;
            case 1230: d->is_cid = 1; break;                   /* ROS */
            case 1236: if (n > 0) d->fdarray    = cff_long(st[n-1]); break;
            case 1237: if (n > 0) d->fdselect   = cff_long(st[n-1]); break;
            default: break;      /* reserved or uninteresting: just clear */
            }
            n = 0;
        }
    }
    return !r.bad;
}

/* ── charset: GID -> SID ──────────────────────────────────────────
 * Glyph ids come from `cmap`, so the charset is not needed to draw
 * text. The one thing that does need it is `seac` (below), which names
 * its two components by StandardEncoding code rather than by GID. That
 * is rare enough that the reverse lookup is left as a linear scan
 * instead of being unpacked into a table at load. */
static int cff_gid_for_sid(font *f, uint32_t sid)
{
    if (sid == 0 || f->nglyphs <= 0) return 0;

    /* Predefined charsets (0 ISOAdobe, 1 Expert, 2 ExpertSubset) are not
     * stored in the file. For ISOAdobe -- the only one worth humouring --
     * SID and GID coincide over the standard-strings range. */
    if (!f->charset)
        return sid < (uint32_t)f->nglyphs ? (int)sid : 0;

    rd r = rd_at(f->data, f->cff + f->cff_len, f->charset);
    uint32_t fmt = ru8(&r);
    if (r.bad) return 0;

    if (fmt == 0) {
        /* .notdef is GID 0 and is never listed, so the array starts at
         * GID 1 -- an off-by-one here mis-maps every accent. */
        for (int gid = 1; gid < f->nglyphs; gid++) {
            uint32_t s = ru16(&r);
            if (r.bad) break;
            if (s == sid) return gid;
        }
    } else if (fmt == 1 || fmt == 2) {
        int gid = 1;
        while (gid < f->nglyphs) {
            uint32_t first = ru16(&r);
            uint32_t nleft = (fmt == 1) ? ru8(&r) : ru16(&r);
            if (r.bad) break;
            if (sid >= first && sid - first <= nleft) {
                long hit = (long)gid + (long)(sid - first);
                return hit < f->nglyphs ? (int)hit : 0;
            }
            gid += (int)nleft + 1;
        }
    }
    return 0;
}

/* StandardEncoding code -> SID.
 *
 * Codes 32..126 map onto SIDs 1..95 in order, which is the whole of
 * ASCII and covers every base letter seac ever uses. Above that the map
 * is sparse: the codes below take SIDs 96..149 consecutively, so the
 * table only has to list the codes. */
static const uint8_t STD_HI[] = {
    161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
    177,178,179,180,
    182,183,184,185,186,187,188,189,
    191,
    193,194,195,196,197,198,199,200,
    202,203,
    205,206,207,208,
    225,
    227,
    232,233,234,235,
    241,
    245,
    248,249,250,251
};

static int cff_seac_gid(font *f, int code)
{
    if (code < 32 || code > 255) return 0;
    if (code <= 126) return cff_gid_for_sid(f, (uint32_t)(code - 31));
    for (unsigned i = 0; i < sizeof STD_HI / sizeof *STD_HI; i++)
        if (STD_HI[i] == code) return cff_gid_for_sid(f, 96 + i);
    return 0;
}

/* ── CID keying ───────────────────────────────────────────────────
 * A CID-keyed CFF has no single Private DICT: each glyph belongs to one
 * of several "font dicts" chosen by FDSelect, and the local subroutine
 * array lives in that dict's Private DICT. Using the wrong one turns
 * every callsubr into a jump to an unrelated blob of bytes. */
static int cff_fd_of(font *f, int gid)
{
    if (!f->fdselect || gid < 0) return 0;
    rd r = rd_at(f->data, f->cff + f->cff_len, f->fdselect);
    uint32_t fmt = ru8(&r);
    if (r.bad) return 0;

    if (fmt == 0) {                            /* one byte per glyph */
        rd g = rd_at(f->data, f->cff + f->cff_len, f->fdselect + 1 + (size_t)gid);
        uint32_t fd = ru8(&g);
        return g.bad ? 0 : (int)fd;
    }
    if (fmt == 3) {                            /* sorted ranges */
        uint32_t nr = ru16(&r);
        if (r.bad || nr == 0) return 0;
        size_t arr = r.p;
        uint32_t lo = 0, hi = nr;
        /* Each record is first(u16) + fd(u8); the range runs up to the
         * next record's `first`, with a sentinel u16 after the last. */
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            rd m = rd_at(f->data, f->cff + f->cff_len, arr + (size_t)mid * 3);
            uint32_t first = ru16(&m);
            if (m.bad) return 0;
            if (first > (uint32_t)gid) hi = mid; else lo = mid + 1;
        }
        if (lo == 0) return 0;                 /* below the first range */
        rd m = rd_at(f->data, f->cff + f->cff_len, arr + (size_t)(lo - 1) * 3 + 2);
        uint32_t fd = ru8(&m);
        return m.bad ? 0 : (int)fd;
    }
    return 0;
}

static const cff_index *cff_subrs_for(font *f, int gid)
{
    if (f->is_cid && f->fdsubrs) {
        int fd = cff_fd_of(f, gid);
        if (fd >= 0 && fd < f->nfd) return &f->fdsubrs[fd];
    }
    return &f->lsubrs;
}

/* ── Type 2 charstring interpreter ────────────────────────────────
 *
 * A charstring is a stack machine with no branches, so the only way to
 * lose the plot is to consume the wrong number of bytes for an
 * operator. Two places make that easy:
 *
 *   - hintmask/cntrmask are followed by a raw bitmask whose LENGTH is
 *     (number of stem hints declared so far + 7) / 8. Nothing delimits
 *     it. Miscount the stems by one and, eight hints later, the mask
 *     length changes and every byte after it decodes as a different
 *     operator.
 *   - the first stack-clearing operator may carry an extra LEADING
 *     operand, the advance width. Leave it on the stack and the glyph's
 *     first move is read off by one argument.
 *
 * Both are handled once, here, rather than at each operator.
 */
#define MAX_T2_DEPTH  10        /* spec's own subroutine nesting limit */
#define MAX_T2_OPS    100000    /* runaway or mutually recursive subrs */
#define MAX_T2_STACK  48        /* spec's operand stack depth */
#define MAX_SEAC      2         /* accent composition is never nested */

typedef struct {
    font            *f;
    outline         *P;
    xform            t;         /* charstring units -> device pixels */
    const cff_index *lsub;      /* local subrs for this glyph's FD */
    float            st[MAX_T2_STACK];
    float            x, y;      /* current point, charstring units */
    long             ops;
    int              nst, nstems, open, width_done, done, seac;
} t2;

static void cff_glyph_path(font *f, int gid, outline *P, xform t, int seac);

static pt t2_dev(const t2 *c, float x, float y)
{
    pt p = { c->t.a * x + c->t.c * y + c->t.e,
             c->t.b * x + c->t.d * y + c->t.f };
    return p;
}

static void t2_pt(t2 *c, float x, float y)
{
    pt p = t2_dev(c, x, y);
    ol_add(c->P, p.x, p.y);
}

/* CFF has no closepath operator: a contour is closed implicitly by the
 * next rmoveto or by endchar, and its last point is joined back to its
 * first. The rasteriser already closes every contour it is handed, so
 * "close" here is only the bookkeeping that ends one and starts the
 * next. */
static void t2_move(t2 *c, float dx, float dy)
{
    if (c->open) ol_close(c->P);
    c->x += dx; c->y += dy;
    t2_pt(c, c->x, c->y);
    c->open = 1;
}

/* A draw before any moveto is malformed. Starting the contour at the
 * current point loses nothing and keeps whatever geometry follows. */
static void t2_begin(t2 *c)
{
    if (!c->open) { t2_pt(c, c->x, c->y); c->open = 1; }
}

static void t2_line(t2 *c, float dx, float dy)
{
    t2_begin(c);
    c->x += dx; c->y += dy;
    t2_pt(c, c->x, c->y);
}

/* Every Type 2 curve operator is a cubic; the specialised ones (hh, hv,
 * vv, vh, the flexes) differ only in which deltas they imply to be
 * zero, so they all funnel through here as six explicit deltas. */
static void t2_curve(t2 *c, float dx1, float dy1, float dx2, float dy2,
                     float dx3, float dy3)
{
    t2_begin(c);
    float x1 = c->x + dx1, y1 = c->y + dy1;
    float x2 = x1    + dx2, y2 = y1    + dy2;
    float x3 = x2    + dx3, y3 = y2    + dy3;
    pt a = t2_dev(c, c->x, c->y), b = t2_dev(c, x1, y1);
    pt d = t2_dev(c, x2, y2),     e = t2_dev(c, x3, y3);
    ol_cubic(c->P, a.x, a.y, b.x, b.y, d.x, d.y, e.x, e.y);
    c->x = x3; c->y = y3;
}

/* Drop the leading width operand, if this is the first stack-clearing
 * operator and one is present.
 *
 * The advance itself is taken from `hmtx` -- OpenType requires the two
 * to agree, and hmtx is what the layout code already reads -- so the
 * value is discarded. It still has to be *removed*: it is a LEADING
 * operand, so leaving it shifts every real argument by one.
 *
 * `pairs` means the operator's own argument count is even (the stem
 * operators, rmoveto, endchar): an odd count then betrays the width.
 * hmoveto and vmoveto take exactly one argument, so for them it is a
 * count of two that betrays it. */
static void t2_width(t2 *c, int pairs)
{
    if (c->width_done) return;
    c->width_done = 1;
    int extra = pairs ? (c->nst & 1) : (c->nst > 1);
    if (extra && c->nst > 0) {
        memmove(c->st, c->st + 1, (size_t)(c->nst - 1) * sizeof c->st[0]);
        c->nst--;
    }
}

static void t2_run(t2 *c, size_t beg, size_t end, int depth)
{
    rd     r = rd_at(c->f->data, end, beg);
    float *s = c->st;

    while (r.p < end && !r.bad && !c->done) {
        /* One budget for the whole glyph, shared across subroutine
         * calls: a pair of subrs that call each other would otherwise
         * spin forever inside the depth limit. */
        if (++c->ops > MAX_T2_OPS) { c->done = 1; return; }

        uint32_t b0 = ru8(&r);

        /* Operands. Note 255: a 16.16 FIXED number here, a reserved byte
         * in a DICT. 28 is a 16-bit integer in both. 29 is NOT a 32-bit
         * integer here -- it is the callgsubr operator. */
        if (b0 >= 32 || b0 == 28) {
            float v;
            if      (b0 == 28)  v = (float)(int16_t)ru16(&r);
            else if (b0 <= 246) v = (float)b0 - 139.0f;
            else if (b0 <= 250) v =  (float)(((int)b0 - 247) * 256 + (int)ru8(&r) + 108);
            else if (b0 <= 254) v = -(float)(((int)b0 - 251) * 256 + (int)ru8(&r) + 108);
            else                v = (float)(int32_t)ru32(&r) * (1.0f / 65536.0f);
            if (r.bad) return;
            if (c->nst >= MAX_T2_STACK) { c->done = 1; return; }
            s[c->nst++] = v;
            continue;
        }

        switch (b0) {

        case 1: case 3: case 18: case 23:      /* hstem vstem hstemhm vstemhm */
            t2_width(c, 1);
            c->nstems += c->nst / 2;
            c->nst = 0;
            break;

        case 19: case 20: {                    /* hintmask cntrmask */
            /* Operands still on the stack are an implicit vstemhm: the
             * spec lets the operator itself be elided when a stem list
             * runs straight into a mask. They must be counted, because
             * they lengthen the mask that follows. */
            t2_width(c, 1);
            c->nstems += c->nst / 2;
            c->nst = 0;
            rd_skip(&r, (size_t)(c->nstems + 7) / 8);
            if (r.bad) return;
            break;
        }

        case 21:                               /* rmoveto */
            t2_width(c, 1);
            if (c->nst >= 2) t2_move(c, s[c->nst-2], s[c->nst-1]);
            c->nst = 0;
            break;

        case 22:                               /* hmoveto */
            t2_width(c, 0);
            if (c->nst >= 1) t2_move(c, s[c->nst-1], 0.0f);
            c->nst = 0;
            break;

        case 4:                                /* vmoveto */
            t2_width(c, 0);
            if (c->nst >= 1) t2_move(c, 0.0f, s[c->nst-1]);
            c->nst = 0;
            break;

        case 5:                                /* rlineto */
            for (int i = 0; i + 1 < c->nst; i += 2) t2_line(c, s[i], s[i+1]);
            c->nst = 0;
            break;

        case 6: case 7: {                      /* hlineto vlineto */
            /* One argument per segment, alternating axis. The operator
             * only picks which axis the FIRST segment uses. */
            int horiz = (b0 == 6);
            for (int i = 0; i < c->nst; i++, horiz = !horiz) {
                if (horiz) t2_line(c, s[i], 0.0f);
                else       t2_line(c, 0.0f, s[i]);
            }
            c->nst = 0;
            break;
        }

        case 8:                                /* rrcurveto */
            for (int i = 0; i + 5 < c->nst; i += 6)
                t2_curve(c, s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
            c->nst = 0;
            break;

        case 24: {                             /* rcurveline */
            int i = 0;
            while (c->nst - i >= 8) {          /* keep 2 back for the line */
                t2_curve(c, s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
                i += 6;
            }
            if (c->nst - i >= 2) t2_line(c, s[i], s[i+1]);
            c->nst = 0;
            break;
        }

        case 25: {                             /* rlinecurve */
            int i = 0;
            while (c->nst - i >= 8) {          /* keep 6 back for the curve */
                t2_line(c, s[i], s[i+1]);
                i += 2;
            }
            if (c->nst - i >= 6)
                t2_curve(c, s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
            c->nst = 0;
            break;
        }

        case 26: {                             /* vvcurveto */
            /* Vertical start and end tangents: only the first curve of
             * the run may have a horizontal nudge, and it is the odd
             * leading operand. */
            int i = 0; float dx = 0.0f;
            if (c->nst & 1) { dx = s[0]; i = 1; }
            for (; i + 3 < c->nst; i += 4, dx = 0.0f)
                t2_curve(c, dx, s[i], s[i+1], s[i+2], 0.0f, s[i+3]);
            c->nst = 0;
            break;
        }

        case 27: {                             /* hhcurveto */
            int i = 0; float dy = 0.0f;
            if (c->nst & 1) { dy = s[0]; i = 1; }
            for (; i + 3 < c->nst; i += 4, dy = 0.0f)
                t2_curve(c, s[i], dy, s[i+1], s[i+2], s[i+3], 0.0f);
            c->nst = 0;
            break;
        }

        case 30: case 31: {                    /* vhcurveto hvcurveto */
            /* Four arguments per curve, alternating between "starts
             * horizontal, ends vertical" and the reverse. A trailing
             * FIFTH argument on the last group is the one free
             * coordinate of the final point, which would otherwise be
             * pinned to the axis. */
            int horiz = (b0 == 31), i = 0;
            while (c->nst - i >= 4) {
                float last = (c->nst - i == 5) ? s[i+4] : 0.0f;
                if (horiz) t2_curve(c, s[i], 0.0f, s[i+1], s[i+2], last, s[i+3]);
                else       t2_curve(c, 0.0f, s[i], s[i+1], s[i+2], s[i+3], last);
                i += 4;
                horiz = !horiz;
            }
            c->nst = 0;
            break;
        }

        case 10: case 29: {                    /* callsubr callgsubr */
            const cff_index *ix = (b0 == 10) ? c->lsub : &c->f->gsubrs;
            if (c->nst < 1) break;
            float v = s[--c->nst];
            if (!(v > -70000.0f && v < 70000.0f)) break;
            long idx = (long)v + cff_bias(ix->count);
            size_t sb, se;
            if (depth + 1 >= MAX_T2_DEPTH) break;
            if (idx < 0 || !cff_index_get(c->f, ix, (uint32_t)idx, &sb, &se)) break;
            t2_run(c, sb, se, depth + 1);
            if (c->done) return;
            break;
        }

        case 11:                               /* return */
            return;

        case 14: {                             /* endchar */
            t2_width(c, 1);
            /* Four remaining arguments mean `seac`: the Type 1 accented-
             * character shortcut, still emitted by Type1-to-OTF
             * conversions. bchar and achar are StandardEncoding CODES,
             * not glyph ids, so they go through the charset. */
            if (c->nst >= 4 && c->seac < MAX_SEAC) {
                float adx = s[c->nst-4], ady = s[c->nst-3];
                float bc  = s[c->nst-2], ac  = s[c->nst-1];
                int bchar = (bc >= 0.0f && bc <= 255.0f) ? (int)bc : -1;
                int achar = (ac >= 0.0f && ac <= 255.0f) ? (int)ac : -1;
                if (c->open) ol_close(c->P);
                c->open = 0;
                int bg = cff_seac_gid(c->f, bchar);
                int ag = cff_seac_gid(c->f, achar);
                if (bg > 0) cff_glyph_path(c->f, bg, c->P, c->t, c->seac + 1);
                if (ag > 0 && adx > -1e6f && adx < 1e6f &&
                              ady > -1e6f && ady < 1e6f) {
                    xform o = { 1.0f, 0.0f, 0.0f, 1.0f, adx, ady };
                    cff_glyph_path(c->f, ag, c->P, xf_mul(o, c->t), c->seac + 1);
                }
            }
            if (c->open) ol_close(c->P);
            c->open = 0;
            c->done = 1;
            return;
        }

        case 12: {                             /* escape */
            uint32_t b1 = ru8(&r);
            if (r.bad) return;
            switch (b1) {
            case 34:                           /* hflex */
                /* A flex is two cubics that together replace what would
                 * be a nearly-flat curve; the variants exist only to
                 * save bytes by implying the zero deltas. hflex pins
                 * both ends and both outer controls to one y, and the
                 * second curve mirrors the first's dy2 so the pen ends
                 * back on the starting line. */
                if (c->nst >= 7) {
                    t2_curve(c, s[0], 0.0f, s[1],  s[2], s[3], 0.0f);
                    t2_curve(c, s[4], 0.0f, s[5], -s[2], s[6], 0.0f);
                }
                break;
            case 35:                           /* flex (the 13th arg is a
                                                * depth threshold: unused,
                                                * we always draw curves) */
                if (c->nst >= 12) {
                    t2_curve(c, s[0], s[1], s[2],  s[3], s[4],  s[5]);
                    t2_curve(c, s[6], s[7], s[8],  s[9], s[10], s[11]);
                }
                break;
            case 36:                           /* hflex1 */
                if (c->nst >= 9) {
                    t2_curve(c, s[0], s[1], s[2], s[3], s[4], 0.0f);
                    t2_curve(c, s[5], 0.0f, s[6], s[7], s[8],
                             -(s[1] + s[3] + s[7]));
                }
                break;
            case 37:                           /* flex1 */
                if (c->nst >= 11) {
                    /* The 11th operand is the single free coordinate of
                     * the endpoint; the other one snaps back to wherever
                     * the flex started, and which is which is decided by
                     * whichever axis the flex travelled further along. */
                    float sx = c->x, sy = c->y;
                    float dx = s[0] + s[2] + s[4] + s[6] + s[8];
                    float dy = s[1] + s[3] + s[5] + s[7] + s[9];
                    float ex, ey;
                    if (fabsf(dx) > fabsf(dy)) { ex = sx + dx + s[10]; ey = sy; }
                    else                       { ex = sx; ey = sy + dy + s[10]; }
                    t2_curve(c, s[0], s[1], s[2], s[3], s[4], s[5]);
                    float x2 = c->x + s[6] + s[8], y2 = c->y + s[7] + s[9];
                    t2_curve(c, s[6], s[7], s[8], s[9], ex - x2, ey - y2);
                }
                break;
            default:
                /* The arithmetic and storage operators (add, ifelse,
                 * random, put/get, ...) exist but no shipping font uses
                 * them. Guessing at their arity would desynchronise the
                 * rest of the charstring, so the glyph stops here. */
                c->done = 1;
                return;
            }
            c->nst = 0;
            break;
        }

        default:
            /* Reserved opcode: whatever this charstring is, it is not a
             * Type 2 one. Stop rather than resynchronise on luck. */
            c->done = 1;
            return;
        }
    }
}

static void cff_glyph_path(font *f, int gid, outline *P, xform t, int seac)
{
    size_t beg, end;
    if (P->oom || gid < 0 || gid >= f->nglyphs) return;
    if (!cff_index_get(f, &f->charstrings, (uint32_t)gid, &beg, &end)) return;

    t2 c;
    memset(&c, 0, sizeof c);
    c.f = f; c.P = P; c.t = t; c.seac = seac;
    c.lsub = cff_subrs_for(f, gid);
    t2_run(&c, beg, end, 0);
    /* A charstring that ran off its end without an endchar still owes
     * us its last contour. */
    if (c.open) ol_close(P);
}

/* ── CFF load ─────────────────────────────────────────────────────
 * Walks the container once and records where the pieces are. Nothing is
 * copied out of the file: charstrings are interpreted in place. */
static int cff_load(font *f, size_t off, size_t len, float upem)
{
    f->cff = off;
    f->cff_len = len;
    size_t lim = off + len;

    /* Header: major, minor, hdrSize, offSize. hdrSize is what says where
     * the Name INDEX begins; it is 4 in every font anyone ships, but the
     * field exists precisely so it need not be, and hard-coding 4 would
     * break on a padded header. */
    rd r = rd_at(f->data, lim, off);
    uint32_t major = ru8(&r);
    rd_skip(&r, 1);                            /* minor */
    uint32_t hdrsz = ru8(&r);
    if (r.bad || major != 1 || hdrsz < 4 || hdrsz > len) return 0;

    cff_index names, top, strings;
    if (!cff_index_read(f, off + hdrsz, &names))        return 0;
    if (!cff_index_read(f, names.end,   &top))          return 0;
    if (!cff_index_read(f, top.end,     &strings))      return 0;
    if (!cff_index_read(f, strings.end, &f->gsubrs))    return 0;

    /* Only the first font of the set is used: an sfnt-embedded CFF is
     * required to hold exactly one, and a FontSet with several inside a
     * `CFF ` table is malformed. */
    size_t tb, te;
    if (!cff_index_get(f, &top, 0, &tb, &te)) return 0;

    cff_dict d;
    cff_dict_init(&d);
    if (!cff_dict_parse(f, tb, te, &d)) return 0;

    /* CharstringType 1 means Type 1 charstrings in a CFF wrapper: a
     * different, encrypted-lineage format. Refuse instead of feeding it
     * to a Type 2 interpreter. */
    if (d.cstype != -1 && d.cstype != 2) return 0;
    if (d.charstrings <= 0 || (size_t)d.charstrings >= len) return 0;
    if (!cff_index_read(f, off + (size_t)d.charstrings, &f->charstrings)) return 0;
    if (f->charstrings.count == 0) return 0;

    /* The FontMatrix maps charstring units onto the EM SQUARE, whereas
     * everything else in this file works in head's font units and
     * multiplies by f->scale. Folding unitsPerEm in here once converts
     * the matrix into font units, so the CFF glyph transform ends up
     * being the same `* f->scale` the glyf path uses.
     *
     * The matrix is normally exactly 1/unitsPerEm. Its DEFAULT, though,
     * is 1/1000 -- so a 2048-upem font that omits it would be scaled by
     * 1/1000 and come out at half size. An absent matrix therefore
     * falls back to the identity (charstring units ARE font units),
     * which is head's upem, rather than to the CFF default. */
    if (d.have_fm &&
        fabsf(d.fm[0]) > 1e-7f && fabsf(d.fm[0]) < 1.0f &&
        fabsf(d.fm[3]) > 1e-7f && fabsf(d.fm[3]) < 1.0f &&
        fabsf(d.fm[1]) < 4.0f  && fabsf(d.fm[2]) < 4.0f &&
        fabsf(d.fm[4]) < 16.0f && fabsf(d.fm[5]) < 16.0f) {
        for (int i = 0; i < 6; i++) f->fm[i] = d.fm[i] * upem;
    } else {
        f->fm[0] = f->fm[3] = 1.0f;
        f->fm[1] = f->fm[2] = f->fm[4] = f->fm[5] = 0.0f;
    }

    /* Private DICT. Its own Subrs offset is relative to the Private
     * DICT's start, not to the table -- the one offset in CFF that is
     * not measured from the table base. */
    if (d.priv_off > 0 && d.priv_sz > 0 &&
        (size_t)d.priv_off < len && (size_t)d.priv_sz <= len - (size_t)d.priv_off) {
        size_t pb = off + (size_t)d.priv_off;
        cff_dict pd;
        cff_dict_init(&pd);
        if (cff_dict_parse(f, pb, pb + (size_t)d.priv_sz, &pd) && pd.subrs > 0)
            cff_index_read(f, pb + (size_t)pd.subrs, &f->lsubrs);
    }

    f->is_cid = d.is_cid;
    if (d.is_cid) {
        if (d.fdselect > 0 && (size_t)d.fdselect < len)
            f->fdselect = off + (size_t)d.fdselect;
        if (d.fdarray > 0 && (size_t)d.fdarray < len &&
            cff_index_read(f, off + (size_t)d.fdarray, &f->fdarray) &&
            f->fdarray.count > 0 && f->fdarray.count <= MAX_FD) {
            f->fdsubrs = calloc(f->fdarray.count, sizeof *f->fdsubrs);
            if (!f->fdsubrs) return 0;
            f->nfd = (int)f->fdarray.count;
            for (int i = 0; i < f->nfd; i++) {
                size_t fb, fe;
                cff_dict fd, qd;
                if (!cff_index_get(f, &f->fdarray, (uint32_t)i, &fb, &fe)) continue;
                cff_dict_init(&fd);
                if (!cff_dict_parse(f, fb, fe, &fd)) continue;
                if (fd.priv_off <= 0 || fd.priv_sz <= 0) continue;
                if ((size_t)fd.priv_off >= len ||
                    (size_t)fd.priv_sz > len - (size_t)fd.priv_off) continue;
                size_t qb = off + (size_t)fd.priv_off;
                cff_dict_init(&qd);
                if (cff_dict_parse(f, qb, qb + (size_t)fd.priv_sz, &qd) && qd.subrs > 0)
                    cff_index_read(f, qb + (size_t)qd.subrs, &f->fdsubrs[i]);
            }
        }
    }

    /* charset is only consulted by seac; 0/1/2 are the predefined ones,
     * which are not in the file at all. */
    if (d.charset > 2 && (size_t)d.charset < len) f->charset = off + (size_t)d.charset;

    /* CharStrings is the authority on how many glyphs there are. maxp
     * should agree, but if it claims more, every extra id would index
     * past the end of the INDEX. */
    if ((int)f->charstrings.count < f->nglyphs) f->nglyphs = (int)f->charstrings.count;
    if (f->nglyphs <= 0) return 0;

    f->is_cff = 1;
    return 1;
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
    /* Valid arithmetic on int16 coordinates cannot produce these, but a
     * single NaN slipping through would make the min/max bbox in
     * glyph_build() silently ignore it and then turn into an undefined
     * float-to-int cast down here, so the invariant is enforced where
     * it is relied on. */
    if (!(ax > -1e9f && ax < 1e9f) || !(ay > -1e9f && ay < 1e9f) ||
        !(bx > -1e9f && bx < 1e9f) || !(by > -1e9f && by < 1e9f)) return;

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

static uint8_t *rasterise(const outline *P, int w, int h)
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

    /* One 256-entry table beats a powf() per pixel, and 0 and 255 are
     * fixed points of any gamma, so fully-in and fully-out pixels stay
     * exact whatever FONT_GAMMA is set to. */
    static uint8_t ramp[256];
    static int ramp_ready;
    if (!ramp_ready) {
        for (int i = 0; i < 256; i++)
            ramp[i] = (uint8_t)(powf((float)i / 255.0f, 1.0f / FONT_GAMMA) * 255.0f + 0.5f);
        ramp_ready = 1;
    }

    for (int y = 0; y < h; y++) {
        const float *row = acc + (size_t)y * stride;
        uint8_t *out = cov + (size_t)y * w;
        float sum = 0.0f;
        for (int x = 0; x < w; x++) {
            sum += row[x];
            float a = sum < 0.0f ? -sum : sum;
            if (a > 1.0f) a = 1.0f;
            out[x] = ramp[(int)(a * 255.0f + 0.5f)];
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
    int gid = map_gid(f, cp);
    g->adv = advance_of(f, gid);

    outline P = {0};
    if (f->is_cff) {
        /* f->fm holds the FontMatrix already folded into font units, so
         * this is the glyf transform with a (usually identity) 2x2 in
         * front of it -- and the interpreter can work in device space
         * throughout instead of transforming a point list afterwards.
         * Row-vector convention, as everywhere else here:
         *     x' = a*x + c*y + e,   y' = b*x + d*y + f. */
        xform t;
        t.a =  f->fm[0] * f->scale;  t.b = -f->fm[1] * f->scale;
        t.c =  f->fm[2] * f->scale;  t.d = -f->fm[3] * f->scale;
        t.e =  f->fm[4] * f->scale + (float)phase / (float)SUBPX;
        t.f = -f->fm[5] * f->scale;
        cff_glyph_path(f, gid, &P, t, 0);
    } else {
        xform t = { f->scale, 0.0f, 0.0f, -f->scale, /* y grows down on screen */
                    (float)phase / (float)SUBPX, 0.0f };
        glyf_outline(f, gid, &P, t, 0);
    }

    if (P.oom || P.ne == 0 || P.np == 0) { ol_free(&P); return g; }

    float x0 = P.p[0].x, x1 = x0, y0 = P.p[0].y, y1 = y0;
    for (int i = 1; i < P.np; i++) {
        if (P.p[i].x < x0) x0 = P.p[i].x;
        if (P.p[i].x > x1) x1 = P.p[i].x;
        if (P.p[i].y < y0) y0 = P.p[i].y;
        if (P.p[i].y > y1) y1 = P.p[i].y;
    }
    /* NaN or an absurd bbox means the outline is junk, and a blank glyph
     * is the correct rendering of junk. The magnitude bound does double
     * duty: it makes the float-to-int casts below defined, and it keeps
     * the origin inside the int16 fields of the cache entry. No real
     * glyph is 30000px across at the 2000px size ceiling. */
    if (!(x1 >= x0) || !(y1 >= y0) ||
        !(x0 > -30000.0f) || !(x1 < 30000.0f) ||
        !(y0 > -30000.0f) || !(y1 < 30000.0f)) {
        ol_free(&P); return g;
    }

    int bx = (int)floorf(x0) - 1, by = (int)floorf(y0) - 1;
    int bw = (int)ceilf(x1) + 1 - bx, bh = (int)ceilf(y1) + 1 - by;
    if (bw <= 0 || bh <= 0 || bw > MAX_DIM || bh > MAX_DIM) { ol_free(&P); return g; }

    for (int i = 0; i < P.np; i++) { P.p[i].x -= (float)bx; P.p[i].y -= (float)by; }

    g->cov = rasterise(&P, bw, bh);
    if (g->cov) { g->bw = (uint16_t)bw; g->bh = (uint16_t)bh;
                  g->x0 = (int16_t)bx;  g->y0 = (int16_t)by; }
    ol_free(&P);
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
    /* Three sfnt flavours get this far. 0x00010000 and `true` carry
     * `glyf` outlines; `OTTO` carries a `CFF ` table instead. Which
     * outline loader actually runs is decided below by which table is
     * present rather than by this tag, because the two do occasionally
     * disagree and the tables are the ones that have to be read. */
    if (ver != 0x00010000u && ver != tag4("true") && ver != tag4("OTTO"))
        goto fail;

    size_t off, len;
    int32_t  loca_fmt = 0;
    uint32_t upem = 0;
    if (!find_table(f->data, f->size, dir, tag4("head"), &off, &len) || len < 54) goto fail;
    {
        /* Seek to each field rather than counting skips: `head` has two
         * 8-byte LONGDATETIMEs and a bbox in the middle, and an
         * off-by-two here silently yields a nonsense indexToLocFormat. */
        rd h = rd_at(f->data, f->size, off + 12);
        if (ru32(&h) != 0x5F0F3CF5u) goto fail;          /* magicNumber */
        rd_to(&h, off + 18);
        upem = ru16(&h);
        rd_to(&h, off + 50);
        loca_fmt = rs16(&h);                             /* indexToLocFormat */
        if (h.bad || upem < 16 || upem > 16384) goto fail;
        /* Range-checked in the `glyf` branch instead of here: a CFF font
         * has no `loca`, and refusing one over a field nothing reads
         * would turn a perfectly good OTF into a desktop with no text. */
        f->loca_long = (loca_fmt == 1);
    }
    f->scale = px / (float)upem;

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

    /* Outline format. `glyf` + `loca` is TrueType, `CFF ` is
     * OpenType/PostScript, and a well-formed file has exactly one of the
     * two. If both somehow appear, `glyf` wins: whatever the sfnt tag
     * claims, a file with real `loca` and `glyf` tables is a TrueType
     * font and the CFF is the afterthought.
     *
     * `CFF2` -- the variable-font successor -- is deliberately NOT
     * accepted. It reuses CFF's INDEX containers, which makes it look
     * parseable, but its charstrings are a different dialect: blend and
     * vsindex operators driven by an item variation store, and no width
     * operand at all. Feeding one to the Type 2 interpreter below would
     * not fail loudly; it would draw confident nonsense. A CFF2-only
     * font matches neither branch here and is refused. */
    {
        size_t co = 0, cl = 0;
        int have_glyf = find_table(f->data, f->size, dir, tag4("glyf"),
                                   &f->glyf, &f->glyf_len)
                     && find_table(f->data, f->size, dir, tag4("loca"),
                                   &f->loca, &f->loca_len);
        int have_cff  = find_table(f->data, f->size, dir, tag4("CFF "), &co, &cl);

        if (have_glyf) {
            if (loca_fmt != 0 && loca_fmt != 1) goto fail;
            size_t need = (size_t)(f->nglyphs + 1) * (f->loca_long ? 4u : 2u);
            if (need > f->loca_len) {
                int fit = (int)(f->loca_len / (f->loca_long ? 4u : 2u)) - 1;
                if (fit <= 0) goto fail;
                f->nglyphs = fit;
            }
        } else if (have_cff) {
            if (cl < 8 || !cff_load(f, co, cl, (float)upem)) goto fail;
        } else {
            goto fail;
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
    /* cff_load() can allocate the per-font-dict subroutine table before
     * hitting a later check, so the failure path has to release it too. */
    free(f->fdsubrs);
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
    free(f->fdsubrs);
    free(f->data);
    free(f);
}

float font_ascent(const font *f)      { return f ? f->ascent : 0.0f; }
float font_descent(const font *f)     { return f ? f->descent : 0.0f; }
float font_line_height(const font *f) { return f ? f->ascent + f->descent + f->line_gap : 0.0f; }

/* ── layout ──────────────────────────────────────────────────────── */
/* Measuring is pure metric work -- cmap, hmtx and kern -- and never
 * touches the rasteriser. Laying out a paragraph to find where it wraps
 * must not cost a bitmap for every glyph in it. */
float font_text_width(font *f, const char *utf8)
{
    if (!f || !utf8) return 0.0f;
    float pen = 0.0f;
    int prev = -1;
    while (*utf8) {
        int gid = map_gid(f, utf8_next(&utf8));
        if (prev >= 0) pen += kern_pair(f, prev, gid);
        pen += advance_of(f, gid);
        prev = gid;
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
        /* A long enough string can walk the pen out of float-to-int
         * range; stop rather than let the cast go undefined. */
        if (!(pen > -1e6f && pen < 1e6f)) break;

        /* The glyph id has to be known before the phase, because kerning
         * moves the pen and so decides which phase is wanted. Looking it
         * up through cmap rather than through a phase-0 cache entry is
         * what keeps a misaligned run from rasterising every glyph
         * twice. */
        int gid = map_gid(f, cp);
        if (prev >= 0) pen += kern_pair(f, prev, gid);
        prev = gid;

        int ix = (int)floorf(pen);
        int ph = (int)((pen - (float)ix) * SUBPX + 0.5f);
        if (ph >= SUBPX) { ph = 0; ix++; }
        glyph *g = glyph_get(f, cp, ph);
        if (!g) { pen += advance_of(f, gid); continue; }

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
                    uint32_t da = (d >> 24) & 0xFF;
                    uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                    /* +127 before the divide rounds to nearest, which
                     * keeps a full-coverage pixel exactly the source
                     * colour instead of one LSB short of it. */
                    uint32_t nr = (dr * (255 - a) + sr * a + 127) / 255;
                    uint32_t ng = (dg * (255 - a) + sg * a + 127) / 255;
                    uint32_t nb = (db * (255 - a) + sb * a + 127) / 255;
                    /* Carry the alpha byte. This used to be dropped, and
                     * the surface compositor reads that byte as coverage:
                     * every glyph left a hole of alpha 0 behind it, so
                     * anything translucent drawn over text afterwards --
                     * a window shadow crossing the title of the window
                     * below, say -- blended against nothing and collapsed
                     * the text to pure black. Two layouts hit it
                     * independently and each patched around it locally.
                     * Standard src-over: on an opaque destination this is
                     * exactly 255, so nothing else changes. */
                    uint32_t na = da + (a * (255 - da) + 127) / 255;
                    dst[c] = (na << 24) | (nr << 16) | (ng << 8) | nb;
                }
            }
        }
        pen += g->adv;
    }
}
