/* font_test.c — specimen sheet, self-checks and fuzz driver for font.c.
 *
 *   font_test [ttf] [out.png]     render the specimen + run assertions
 *   font_test --fuzz [ttf] [n]    truncate/corrupt a font n times
 *
 * The specimen is rendered in the real UI colours (Nocturne #0B0E14 with
 * #D4DCEA text) because anti-aliasing quality is a function of contrast
 * polarity: light-on-dark exaggerates blooming in a way that a black-on-
 * white test sheet would hide entirely.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -std=gnu11 font_test.c font.c png.c -lm -o font_test
 */
#include "font.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BG    0x0B0E14u    /* Nocturne background */
#define FG    0xD4DCEAu    /* Nocturne body text  */
#define DIM   0x5A6478u
#define ACC   0x7DD3C0u

static const char *DEFAULT_TTF =
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

static int failures = 0;
static void check(int ok, const char *what)
{
    if (!ok) { failures++; printf("  FAIL  %s\n", what); }
    else printf("  ok    %s\n", what);
}

/* ── canvas helpers ──────────────────────────────────────────────── */
typedef struct { uint32_t *px; int w, h; } canvas;

static canvas canvas_new(int w, int h, uint32_t bg)
{
    canvas c = { malloc((size_t)w * h * 4), w, h };
    if (c.px) for (int i = 0; i < w * h; i++) c.px[i] = bg;
    return c;
}

static void hline(canvas *c, int y, uint32_t col)
{
    if (y < 0 || y >= c->h) return;
    for (int x = 24; x < c->w - 24; x++) c->px[(size_t)y * c->w + x] = col;
}

/* Nearest-neighbour zoom of a scratch render into the sheet. Small text
 * cannot be judged at 1:1 in a screenshot, and the whole point of the
 * exercise is to see whether the coverage ramp is smooth. */
static void blit_zoom(canvas *dst, int dx, int dy, const canvas *src, int z)
{
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) {
            uint32_t v = src->px[(size_t)y * src->w + x];
            for (int j = 0; j < z; j++)
                for (int i = 0; i < z; i++) {
                    int X = dx + x*z + i, Y = dy + y*z + j;
                    if (X >= 0 && X < dst->w && Y >= 0 && Y < dst->h)
                        dst->px[(size_t)Y * dst->w + X] = v;
                }
        }
}

/* ── single-glyph probe: render one string on black and measure ink ── */
typedef struct {
    int x0, y0, x1, y1;      /* ink bbox, -1 if blank */
    int n_on;                /* pixels at full coverage */
    int n_any;               /* pixels with any coverage */
    int n_mid;               /* partial coverage: the AA ramp */
    canvas c;
} probe;

static probe probe_text(font *f, const char *s, float px)
{
    probe p = { -1, -1, -1, -1, 0, 0, 0, {NULL, 0, 0} };
    int w = (int)(font_text_width(f, s) + px * 2.0f) + 8;
    int h = (int)(px * 3.0f) + 8;
    p.c = canvas_new(w, h, 0x000000);
    if (!p.c.px) return p;
    font_draw(f, p.c.px, w, h, px * 0.5f, px * 2.0f, s, 0xFFFFFF, 1.0f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = p.c.px[(size_t)y * w + x] & 0xFF;
            if (!v) continue;
            p.n_any++;
            if (v == 0xFF) p.n_on++; else p.n_mid++;
            if (p.x0 < 0 || x < p.x0) p.x0 = x;
            if (p.y0 < 0 || y < p.y0) p.y0 = y;
            if (x > p.x1) p.x1 = x;
            if (y > p.y1) p.y1 = y;
        }
    return p;
}

/* Does this glyph have a counter — an enclosed region of background
 * inside the ink? Flood fill from the bbox border; anything unvisited
 * and unpainted is a hole. A renderer that gets the winding rule or the
 * implied on-curve midpoints wrong fills these in solid. */
static int counter_pixels(const probe *p)
{
    if (p->x0 < 0) return 0;
    int w = p->c.w, h = p->c.h;
    unsigned char *seen = calloc((size_t)w * h, 1);
    int *stack = malloc((size_t)w * h * sizeof *stack);
    if (!seen || !stack) { free(seen); free(stack); return 0; }
    /* Seed from the whole frame border (the render is padded, so the
     * border is always background). */
    int sp = 0;
#define PUSH(X,Y) do { int _i=(Y)*w+(X); \
        if ((X)>=0&&(X)<w&&(Y)>=0&&(Y)<h&&!seen[_i]&&!(p->c.px[_i]&0xFF)) \
        { seen[_i]=1; stack[sp++]=_i; } } while (0)
    for (int x = 0; x < w; x++) { PUSH(x, 0); PUSH(x, h-1); }
    for (int y = 0; y < h; y++) { PUSH(0, y); PUSH(w-1, y); }
    while (sp) {
        int i = stack[--sp], x = i % w, y = i / w;
        PUSH(x-1, y); PUSH(x+1, y); PUSH(x, y-1); PUSH(x, y+1);
    }
#undef PUSH
    int holes = 0;
    for (int y = p->y0; y <= p->y1; y++)
        for (int x = p->x0; x <= p->x1; x++) {
            int i = y*w + x;
            if (!seen[i] && !(p->c.px[i] & 0xFF)) holes++;
        }
    free(seen); free(stack);
    return holes;
}

/* Is this codepoint actually in the font?
 *
 * There is no API for it on purpose -- callers should not care -- but a
 * test does: OpenSymbol has no accented Latin at all, and a subset font
 * has almost nothing, so every miss comes back as .notdef and a naive
 * assertion would call that a rendering bug. Comparing against a
 * codepoint no font maps identifies the .notdef bitmap. */
static int has_glyph(font *f, const char *utf8, float px)
{
    probe want = probe_text(f, utf8, px);
    probe nope = probe_text(f, "\xF4\x8F\xBF\xBD", px);    /* U+10FFFD */
    int same = 0;
    if (want.c.px && nope.c.px && want.c.w == nope.c.w && want.c.h == nope.c.h)
        same = memcmp(want.c.px, nope.c.px,
                      (size_t)want.c.w * want.c.h * 4) == 0;
    free(want.c.px); free(nope.c.px);
    return !same;
}

/* ── the specimen sheet ──────────────────────────────────────────── */
static const char *SAMPLES[] = {
    "The quick brown fox jumps over the lazy dog. 0123456789",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz",
    "Accents: e\xCC\x81 \xC3\xA9 \xC3\xA0 \xC3\xBC \xC3\xB1 \xC3\xA7 \xC3\xB6 "
        "\xC3\x85 \xC3\x98 \xC5\x93 \xC3\x9F \xC3\x86 \xC5\xA0 \xC5\xBD",
    "Punctuation: !@#$%^&*()[]{}<>/\\|~`'\"-_=+.,;:? \xE2\x80\x94 \xE2\x86\x92",
    "Kerning pairs: AVATAR Wavy To. Yo, LTa P, F. VA Ty \xE2\x80\x94 Typography",
    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBC\xD0\xB8\xD1\x80 "
        "\xE2\x80\xA2 \xCE\x93\xCE\xB5\xCE\xB9\xCE\xAC \xCF\x83\xCE\xBF\xCF\x85",
};
#define NSAMPLES ((int)(sizeof SAMPLES / sizeof *SAMPLES))

static int sheet(const char *ttf, const char *out)
{
    const float sizes[] = { 12.0f, 14.0f, 18.0f, 32.0f, 64.0f };
    const int   nsizes  = 5;

    canvas cv = canvas_new(1280, 1500, BG);
    if (!cv.px) return 1;

    font *hdr = font_load(ttf, 20.0f);
    if (!hdr) { fprintf(stderr, "font_load failed: %s\n", ttf); free(cv.px); return 1; }

    float y = 46.0f;
    font_draw(hdr, cv.px, cv.w, cv.h, 32.0f, y, "AurOS font.c \xE2\x80\x94 specimen",
              ACC, 1.0f);
    font_draw(hdr, cv.px, cv.w, cv.h, 360.0f, y, ttf, DIM, 1.0f);
    y += 18.0f;
    hline(&cv, (int)y, 0x1A2030);
    y += 24.0f;

    for (int s = 0; s < nsizes; s++) {
        float px = sizes[s];
        font *f = font_load(ttf, px);
        if (!f) { fprintf(stderr, "font_load(%g) failed\n", (double)px); continue; }

        char lab[128];
        snprintf(lab, sizeof lab, "%gpx   ascent %.1f  descent %.1f  line %.1f",
                 (double)px, (double)font_ascent(f), (double)font_descent(f),
                 (double)font_line_height(f));
        font_draw(hdr, cv.px, cv.w, cv.h, 32.0f, y, lab, ACC, 0.85f);
        y += 22.0f;

        /* Big sizes get the short samples so the sheet stays inside the
         * canvas; the clipping path is exercised by the self-test. */
        int lines = px >= 32.0f ? 3 : NSAMPLES;
        for (int i = 0; i < lines; i++) {
            const char *txt = SAMPLES[i];
            if (px >= 64.0f && i == 0) txt = "Hamburgefonstiv 1982";
            if (px >= 64.0f && i == 1) txt = "AVATAR Wavy To. Yo,";
            if (px >= 32.0f && i == 2) txt = "\xC3\xA9 \xC3\xA0 \xC3\xBC \xC3\xB1 "
                "\xC3\xA7 \xC3\xB6 \xC3\x85 \xC3\x98 \xC5\x93 \xC3\x9F \xC3\x86 "
                "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
            y += font_ascent(f);
            font_draw(f, cv.px, cv.w, cv.h, 32.0f, y, txt, FG, 1.0f);
            y += font_line_height(f) - font_ascent(f);
        }
        y += 26.0f;
        font_free(f);
    }

    hline(&cv, (int)y, 0x1A2030);
    y += 24.0f;
    font_draw(hdr, cv.px, cv.w, cv.h, 32.0f, y,
              "6x zoom \xE2\x80\x94 13px UI text, and 48px counters", ACC, 0.85f);
    y += 20.0f;

    /* Zoomed 13px strip: this is the size the panel and menus use, so
     * it is the one that has to survive inspection. */
    {
        font *f = font_load(ttf, 13.0f);
        if (f) {
            canvas s = canvas_new(200, 22, BG);
            if (s.px) {
                font_draw(f, s.px, s.w, s.h, 2.0f, 15.0f,
                          "Settings \xE2\x80\xA2 Files \xE2\x80\xA2 Ho\xCC\x88he 42%",
                          FG, 1.0f);
                blit_zoom(&cv, 32, (int)y, &s, 6);
                free(s.px);
            }
            font_free(f);
        }
        y += 22 * 6 + 16;
    }

    /* Counter close-ups at 48px, zoomed 3x. */
    {
        font *f = font_load(ttf, 48.0f);
        if (f) {
            canvas s = canvas_new(390, 62, BG);
            if (s.px) {
                font_draw(f, s.px, s.w, s.h, 4.0f, 48.0f, "oeaBg@8%\xC3\xB6\xC3\xA9",
                          FG, 1.0f);
                blit_zoom(&cv, 32, (int)y, &s, 3);
                free(s.px);
            }
            font_free(f);
        }
        y += 62 * 3 + 20;
    }

    font_free(hdr);
    int rc = png_write_rgb(out, cv.px, cv.w, cv.h);
    free(cv.px);
    printf("wrote %s (%dx%d) rc=%d\n", out, 1280, 1500, rc);
    return rc;
}

/* ── assertions ──────────────────────────────────────────────────── */
static void selftest(const char *ttf)
{
    printf("self-test: %s\n", ttf);
    font *f = font_load(ttf, 48.0f);
    if (!f) { printf("  FAIL  font_load\n"); failures++; return; }

    check(font_ascent(f) > 0 && font_descent(f) > 0, "metrics positive");
    check(font_line_height(f) >= font_ascent(f) + font_descent(f), "line height sane");

    /* Exact-coverage check: FULL BLOCK is a plain rectangle, so its
     * interior must be saturated and its area must be close to the
     * advance times the block height. */
    probe blk = probe_text(f, "\xE2\x96\x88", 48.0f);
    if (blk.x0 >= 0 && has_glyph(f, "\xE2\x96\x88", 48.0f)) {
        check(blk.n_on > 0, "solid block has saturated interior");
        int iw = blk.x1 - blk.x0 + 1, ih = blk.y1 - blk.y0 + 1;
        check(blk.n_on >= (iw - 2) * (ih - 2),
              "solid block interior fully covered (no pinholes)");
        check(blk.n_mid * 4 < blk.n_on, "solid block edge ramp is thin");
    } else printf("  skip  no FULL BLOCK glyph in this font\n");
    free(blk.c.px);

    struct { const char *s; int want; } holes[] = {
        { "o", 1 }, { "e", 1 }, { "a", 1 }, { "B", 2 }, { "d", 1 }, { "8", 2 },
        { "\xC3\xB6", 1 },       /* o-diaeresis: composite */
        { "\xC3\xA9", 1 },       /* e-acute: composite */
    };
    for (int i = 0; i < (int)(sizeof holes / sizeof *holes); i++) {
        if (!has_glyph(f, holes[i].s, 48.0f)) {
            printf("  skip  '%s' not in this font\n", holes[i].s); continue;
        }
        probe p = probe_text(f, holes[i].s, 48.0f);
        int hp = counter_pixels(&p);
        char msg[96];
        snprintf(msg, sizeof msg, "'%s' has a counter (%d bg px enclosed)",
                 holes[i].s, hp);
        check(p.x0 >= 0 && hp >= 8, msg);
        free(p.c.px);
    }

    /* Composite glyphs must actually carry the accent: the accented
     * form has to be taller than the base letter and carry more ink. */
    if (has_glyph(f, "\xC3\xA9", 48.0f)) {
        probe pe = probe_text(f, "e", 48.0f), pea = probe_text(f, "\xC3\xA9", 48.0f);
        check(pe.y0 >= 0 && pea.y0 >= 0 && pea.y0 < pe.y0,
              "composite 'e-acute' is taller than 'e'");
        check(pea.n_any > pe.n_any, "composite 'e-acute' has more ink than 'e'");
        free(pe.c.px); free(pea.c.px);
    } else printf("  skip  no 'e-acute' in this font\n");

    if (has_glyph(f, "\xC3\xB1", 48.0f)) {
        probe pn = probe_text(f, "n", 48.0f), pnt = probe_text(f, "\xC3\xB1", 48.0f);
        check(pn.y0 >= 0 && pnt.y0 >= 0 && pnt.y0 < pn.y0,
              "composite 'n-tilde' is taller than 'n'");
        free(pn.c.px); free(pnt.c.px);
    } else printf("  skip  no 'n-tilde' in this font\n");

    /* Widths: proportional fonts must not be measuring a fixed advance,
     * and the measured width must match where the ink actually stops. */
    float wi = font_text_width(f, "iiiiiiiiii");
    float wm = font_text_width(f, "mmmmmmmmmm");
    if (fabsf(wi - wm) < 0.01f)
        check(wi > 0.0f, "monospaced font: i and m share one advance");
    else
        check(wm > wi * 1.5f, "proportional advances (m wider than i)");

    probe ptxt = probe_text(f, "Hamburgefonstiv", 48.0f);
    float want = font_text_width(f, "Hamburgefonstiv");
    check(ptxt.x1 > 0 && fabsf((float)(ptxt.x1 - 24) - want) < want * 0.10f,
          "drawn ink ends near the measured advance");
    free(ptxt.c.px);

    /* Kerning, if the font has a `kern` table at all. */
    float av = font_text_width(f, "AV");
    float a1 = font_text_width(f, "A") + font_text_width(f, "V");
    printf("  info  AV=%.2f  A+V=%.2f  delta=%.2f %s\n",
           (double)av, (double)a1, (double)(av - a1),
           av < a1 - 0.01f ? "(kern table in use)" : "(no kern pairs)");

    /* Unmapped codepoints and junk UTF-8 must not blow up. */
    font_text_width(f, "\xFF\xFE\x80\x80 \xE0\x80 \xF7\xBF\xBF\xBF");
    canvas c = canvas_new(64, 64, BG);
    if (c.px) {
        font_draw(f, c.px, 64, 64, -500.0f, 20.0f, "clip left", FG, 1.0f);
        font_draw(f, c.px, 64, 64, 900.0f, 20.0f, "clip right", FG, 1.0f);
        font_draw(f, c.px, 64, 64, 4.0f, -900.0f, "clip up", FG, 1.0f);
        font_draw(f, c.px, 64, 64, 4.0f, 900.0f, "clip down", FG, 1.0f);
        font_draw(f, c.px, 64, 64, 4.0f, 30.0f, "", FG, 1.0f);
        font_draw(f, c.px, 64, 64, 4.0f, 30.0f, "x", FG, 0.0f);
        free(c.px);
    }
    check(1, "clipping and degenerate draws survive");

    font_free(f);
    font_free(NULL);
}

/* ── fuzz ────────────────────────────────────────────────────────── */
static uint32_t rnd_state = 0x1234567u;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;  return rnd_state;
}

static void exercise(font *f)
{
    if (!f) return;
    canvas c = canvas_new(128, 64, BG);
    if (!c.px) return;
    font_ascent(f); font_descent(f); font_line_height(f);
    font_text_width(f, "The quick brown fox \xC3\xA9\xC3\xB1\xC3\xBC 0123");
    font_draw(f, c.px, c.w, c.h, 3.0f, 40.0f,
              "The quick brown fox \xC3\xA9\xC3\xB1\xC3\xBC 0123", FG, 1.0f);
    /* Sweep codepoints so cmap segment maths, composite recursion and
     * loca bounds all get walked on the mangled tables. */
    char buf[8];
    for (uint32_t cp = 0x20; cp < 0x2600; cp += 7) {
        int n = 0;
        if (cp < 0x80) buf[n++] = (char)cp;
        else if (cp < 0x800) { buf[n++] = (char)(0xC0|(cp>>6)); buf[n++] = (char)(0x80|(cp&63)); }
        else { buf[n++] = (char)(0xE0|(cp>>12)); buf[n++] = (char)(0x80|((cp>>6)&63));
               buf[n++] = (char)(0x80|(cp&63)); }
        buf[n] = 0;
        font_draw(f, c.px, c.w, c.h, 3.0f, 40.0f, buf, FG, 1.0f);
    }
    free(c.px);
}

static int fuzz(const char *ttf, int iters)
{
    FILE *fp = fopen(ttf, "rb");
    if (!fp) { perror(ttf); return 1; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    rewind(fp);
    uint8_t *orig = malloc((size_t)n);
    if (!orig || fread(orig, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return 1; }
    fclose(fp);

    const char *tmp = "/tmp/auros_font_fuzz.ttf";
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { free(orig); return 1; }

    int loaded = 0;
    for (int it = 0; it < iters; it++) {
        size_t len = (size_t)n;
        memcpy(buf, orig, len);
        int mode = it % 4;
        if (mode == 0) {                       /* truncation */
            len = (size_t)(rnd() % (uint32_t)n);
        } else if (mode == 1) {                /* byte corruption */
            int k = 1 + (int)(rnd() % 64);
            for (int i = 0; i < k; i++) buf[rnd() % len] = (uint8_t)rnd();
        } else if (mode == 2) {                /* both */
            len = (size_t)(rnd() % (uint32_t)n);
            if (len > 16) {
                int k = 1 + (int)(rnd() % 32);
                for (int i = 0; i < k; i++) buf[rnd() % len] = (uint8_t)rnd();
            }
        } else {
            /* Leave the sfnt header and table directory intact so the
             * font still loads: corruption that never gets past
             * font_load() never reaches the glyph and cmap parsers,
             * which are where the interesting arithmetic lives. */
            size_t keep = 12 + 16 * 64;
            if (len > keep + 64) {
                int k = 1 + (int)(rnd() % 512);
                for (int i = 0; i < k; i++)
                    buf[keep + rnd() % (uint32_t)(len - keep)] = (uint8_t)rnd();
            }
        }
        FILE *o = fopen(tmp, "wb");
        if (!o) break;
        fwrite(buf, 1, len, o);
        fclose(o);

        font *f = font_load(tmp, 13.0f + (float)(it % 5) * 9.0f);
        if (f) { loaded++; exercise(f); font_free(f); }
        if (((it + 1) % 50) == 0) { printf("  fuzz %d/%d (%d loaded)\n", it+1, iters, loaded); fflush(stdout); }
    }
    remove(tmp);
    free(buf); free(orig);
    printf("fuzz done: %d iterations, %d mangled fonts still loaded\n", iters, loaded);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--fuzz")) {
        const char *ttf = argc > 2 ? argv[2] : DEFAULT_TTF;
        int n = argc > 3 ? atoi(argv[3]) : 300;
        return fuzz(ttf, n);
    }
    const char *ttf = argc > 1 ? argv[1] : DEFAULT_TTF;
    const char *out = argc > 2 ? argv[2] : "font_specimen.png";

    selftest(ttf);
    int rc = sheet(ttf, out);
    printf("%d check(s) failed\n", failures);
    return rc || failures ? 1 : 0;
}
