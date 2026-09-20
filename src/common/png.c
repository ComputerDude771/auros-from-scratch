#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── bit sink ─────────────────────────────────────────────────────
 * DEFLATE packs its own header bits LSB-first, but Huffman codes are
 * emitted MSB-first. Keeping the two operations separate (put_bits vs
 * put_code) is the difference between a working encoder and an hour
 * of staring at a corrupt stream. */
typedef struct {
    uint8_t *buf; size_t len, cap;
    uint32_t acc; int nbits;
} bitw;

static int bw_grow(bitw *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 1 << 16;
    while (cap < b->len + need) cap *= 2;
    uint8_t *p = realloc(b->buf, cap);
    if (!p) return -1;
    b->buf = p; b->cap = cap; return 0;
}
static void bw_byte(bitw *b, uint8_t v) { if (!bw_grow(b, 1)) b->buf[b->len++] = v; }

static void put_bits(bitw *b, uint32_t val, int n) {
    b->acc |= (val & ((1u << n) - 1)) << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) { bw_byte(b, (uint8_t)(b->acc & 0xFF)); b->acc >>= 8; b->nbits -= 8; }
}
static void put_code(bitw *b, uint32_t code, int n) {   /* MSB-first */
    for (int i = n - 1; i >= 0; i--) put_bits(b, (code >> i) & 1, 1);
}
static void bw_flush(bitw *b) { if (b->nbits) put_bits(b, 0, 8 - b->nbits); }

/* ── fixed Huffman tables (RFC 1951 §3.2.6) ─────────────────────── */
static void put_litlen(bitw *b, int sym) {
    if (sym <= 143)      put_code(b, 0x030 + sym,         8);
    else if (sym <= 255) put_code(b, 0x190 + sym - 144,   9);
    else if (sym <= 279) put_code(b, 0x000 + sym - 256,   7);
    else                 put_code(b, 0x0C0 + sym - 280,   8);
}

static const uint16_t LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,
                                      51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                       1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void put_match(bitw *b, int len, int dist) {
    int lc = 28;
    while (lc > 0 && len < LEN_BASE[lc]) lc--;
    put_litlen(b, 257 + lc);
    if (LEN_EXTRA[lc]) put_bits(b, (uint32_t)(len - LEN_BASE[lc]), LEN_EXTRA[lc]);
    int dc = 29;
    while (dc > 0 && dist < DIST_BASE[dc]) dc--;
    put_code(b, (uint32_t)dc, 5);
    if (DIST_EXTRA[dc]) put_bits(b, (uint32_t)(dist - DIST_BASE[dc]), DIST_EXTRA[dc]);
}

/* ── LZ77: hash-chain matcher over a 32 KiB window ───────────────── */
#define WSIZE   32768
#define HSIZE   32768
#define MAXCHAIN 48
#define MINMATCH 3
#define MAXMATCH 258

static uint32_t hash3(const uint8_t *p) {
    return (uint32_t)(((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (HSIZE - 1));
}

static int deflate_fixed(const uint8_t *src, size_t n, bitw *b) {
    int32_t *head = malloc(HSIZE * sizeof *head);
    int32_t *prev = malloc(n ? n * sizeof *prev : sizeof *prev);
    if (!head || !prev) { free(head); free(prev); return -1; }
    for (int i = 0; i < HSIZE; i++) head[i] = -1;

    put_bits(b, 1, 1);      /* BFINAL */
    put_bits(b, 1, 2);      /* BTYPE = fixed Huffman */

    size_t i = 0;
    while (i < n) {
        int best = 0, bestd = 0;
        if (i + MINMATCH <= n) {
            uint32_t h = hash3(src + i);
            int32_t c = head[h];
            int chain = MAXCHAIN;
            while (c >= 0 && chain-- > 0) {
                size_t d = i - (size_t)c;
                if (d == 0 || d > WSIZE) break;
                size_t maxl = n - i; if (maxl > MAXMATCH) maxl = MAXMATCH;
                size_t l = 0;
                while (l < maxl && src[c + l] == src[i + l]) l++;
                if ((int)l > best) { best = (int)l; bestd = (int)d; if (l >= MAXMATCH) break; }
                c = prev[c];
            }
        }
        if (best >= MINMATCH) {
            put_match(b, best, bestd);
            for (int k = 0; k < best; k++) {
                if (i + MINMATCH <= n) {
                    uint32_t h = hash3(src + i);
                    prev[i] = head[h]; head[h] = (int32_t)i;
                }
                i++;
            }
        } else {
            if (i + MINMATCH <= n) {
                uint32_t h = hash3(src + i);
                prev[i] = head[h]; head[h] = (int32_t)i;
            }
            put_litlen(b, src[i]); i++;
        }
    }
    put_litlen(b, 256);     /* end of block */
    bw_flush(b);
    free(head); free(prev);
    return 0;
}

/* ── checksums ───────────────────────────────────────────────────── */
static uint32_t crc_tab[256]; static int crc_ready;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    crc_ready = 1;
}
/* Resumable: pass 0 to start, then feed the previous return value to
 * continue a running CRC across two buffers. */
static uint32_t crc32b(const uint8_t *d, size_t n, uint32_t c) {
    if (!crc_ready) crc_init();
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static uint32_t adler32(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static int chunk(FILE *f, const char *tag, const uint8_t *data, size_t n) {
    uint8_t hdr[4]; be32(hdr, (uint32_t)n);
    if (fwrite(hdr, 1, 4, f) != 4) return -1;
    if (fwrite(tag, 1, 4, f) != 4) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;
    /* CRC covers the type tag and the data, but not the length field. */
    uint32_t c = crc32b((const uint8_t *)tag, 4, 0);
    if (n) c = crc32b(data, n, c);
    uint8_t cb[4]; be32(cb, c);
    return fwrite(cb, 1, 4, f) == 4 ? 0 : -1;
}

int png_write_rgb(const char *path, const uint32_t *px, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    size_t stride = (size_t)w * 3 + 1;
    size_t raw_n  = stride * (size_t)h;
    uint8_t *raw  = malloc(raw_n);
    if (!raw) return -1;

    /* Filter type 1 (Sub) on every row: cheap, and it turns smooth
     * horizontal gradients into runs of near-zero bytes that LZ77 eats. */
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        row[0] = 1;
        uint8_t pr = 0, pg = 0, pb = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * w + x];
            uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            row[1 + x*3 + 0] = (uint8_t)(r - pr);
            row[1 + x*3 + 1] = (uint8_t)(g - pg);
            row[1 + x*3 + 2] = (uint8_t)(b - pb);
            pr = r; pg = g; pb = b;
        }
    }

    bitw b = {0};
    /* zlib wrapper: CMF=0x78 (deflate, 32K window), FLG chosen so the
     * 16-bit header is a multiple of 31. */
    bw_byte(&b, 0x78); bw_byte(&b, 0x01);
    if (deflate_fixed(raw, raw_n, &b)) { free(raw); free(b.buf); return -1; }
    uint32_t ad = adler32(raw, raw_n);
    bw_byte(&b, ad >> 24); bw_byte(&b, ad >> 16); bw_byte(&b, ad >> 8); bw_byte(&b, ad);
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(b.buf); return -1; }
    static const uint8_t sig[8] = {137,'P','N','G','\r','\n',26,'\n'};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    int rc = chunk(f, "IHDR", ihdr, sizeof ihdr)
           | chunk(f, "IDAT", b.buf, b.len)
           | chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(b.buf);
    return rc;
}
