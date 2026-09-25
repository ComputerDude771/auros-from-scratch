/* inflate.c — see inflate.h. RFC 1952 around RFC 1951, pulled.
 *
 * The Huffman decoder is the usual canonical one with a 9-bit lookup
 * table in front of it: codes of nine bits or fewer -- nearly every
 * literal and length in practice -- are one table read, and anything
 * longer walks the canonical code lengths. Five gigabytes go through
 * here on a ten-year-old laptop, so the fast path matters; the slow one
 * is the one that is obviously right, and both are checked by the
 * selftest and by hashing everything that comes out. */
#include <string.h>
#include <stdio.h>

#include "inflate.h"

/* ── CRC-32, the gzip one ────────────────────────────────────────── */

static uint32_t g_crc_tab[256];
static int      g_crc_ready;

static void crc_init(void)
{
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc_feed(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = g_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ── the state ───────────────────────────────────────────────────── */

#define FAST_BITS 9
#define FAST_MASK ((1 << FAST_BITS) - 1)
#define WIN       32768u              /* how far back a distance reaches */
#define OUTCHUNK  (1u << 20)          /* how much is handed on at once   */

typedef struct {
    uint16_t fast[1 << FAST_BITS];    /* (length << 9) | symbol, 0 = slow */
    uint16_t firstcode[16];
    int      maxcode[17];
    uint16_t firstsymbol[16];
    uint8_t  size[288];
    uint16_t value[288];
} huff;

typedef struct {
    gz_reader rd;  void *rud;
    gz_writer wr;  void *wud;
    char     *why; size_t wn;

    uint8_t   in[1u << 16];
    size_t    in_pos, in_len;
    int       in_end;                 /* the reader has said "no more"   */
    uint32_t  zeros_fed;              /* bytes invented past that point  */

    uint32_t  bits;                   /* the bit buffer, LSB first       */
    int       nbits;

    uint8_t   out[WIN + OUTCHUNK];
    size_t    pos, flushed;
    uint64_t  total;
    uint32_t  crc;
    int       failed;

    huff      lit, dist;
} gz;

static int fail(gz *z, const char *msg)
{
    if (!z->failed && z->why && z->wn) snprintf(z->why, z->wn, "%s", msg);
    z->failed = 1;
    return -1;
}

/* One byte of input, or 0 past the end -- counted, so that a stream that
 * was cut short is noticed when its trailer is read rather than decoded
 * into a plausible tail of zeros. */
static int next_byte(gz *z, uint8_t *b)
{
    if (z->in_pos == z->in_len) {
        if (!z->in_end) {
            size_t got = 0;
            if (z->rd(z->rud, z->in, sizeof z->in, &got) != 0) {
                z->failed = 1;          /* the reader wrote its own why */
                return -1;
            }
            z->in_pos = 0;
            z->in_len = got;
            if (!got) z->in_end = 1;
        }
        if (z->in_pos == z->in_len) {
            z->zeros_fed++;
            *b = 0;
            return 0;
        }
    }
    *b = z->in[z->in_pos++];
    return 0;
}

static int fill(gz *z)
{
    while (z->nbits <= 24) {
        uint8_t b;
        if (next_byte(z, &b) != 0) return -1;
        z->bits |= (uint32_t)b << z->nbits;
        z->nbits += 8;
        /* A stream that has run dry keeps asking for more; a small
         * number of invented bytes is how a correct stream's last code
         * gets decoded, and anything past that is a stream that ended
         * early. */
        if (z->zeros_fed > 16) return fail(z, "the copy of AurOS stopped "
                                              "partway through");
    }
    return 0;
}

static int getbits(gz *z, int n, uint32_t *v)
{
    if (z->nbits < n && fill(z) != 0) return -1;
    *v = z->bits & ((1u << n) - 1);
    z->bits >>= n;
    z->nbits -= n;
    return 0;
}

/* ── output, with 32 KiB of history always kept ──────────────────── */

static int flush_out(gz *z)
{
    if (z->pos > z->flushed) {
        size_t k = z->pos - z->flushed;
        z->crc = crc_feed(z->crc, z->out + z->flushed, k);
        z->total += k;
        if (z->wr(z->wud, z->out + z->flushed, k) != 0) {
            z->failed = 1;
            return -1;
        }
        z->flushed = z->pos;
    }
    return 0;
}

static int put(gz *z, uint8_t b)
{
    z->out[z->pos++] = b;
    if (z->pos == sizeof z->out) {
        if (flush_out(z) != 0) return -1;
        memmove(z->out, z->out + z->pos - WIN, WIN);
        z->pos = z->flushed = WIN;
    }
    return 0;
}

/* The distance is measured back from what has been written, including
 * what has already been handed on, which is why WIN bytes of history
 * stay in the buffer. The first 32 KiB of a stream have less history
 * than that, and a distance reaching before the start is damage. */
static int copy_back(gz *z, uint32_t dist, uint32_t len)
{
    if (dist == 0 || dist > z->pos)
        return fail(z, "the copy of AurOS is damaged (a reference to "
                       "before its start)");
    while (len--) {
        if (put(z, z->out[z->pos - dist]) != 0) return -1;
    }
    return 0;
}

/* ── canonical Huffman ───────────────────────────────────────────── */

static int rev16(int n)
{
    n = ((n & 0xAAAA) >> 1) | ((n & 0x5555) << 1);
    n = ((n & 0xCCCC) >> 2) | ((n & 0x3333) << 2);
    n = ((n & 0xF0F0) >> 4) | ((n & 0x0F0F) << 4);
    n = ((n & 0xFF00) >> 8) | ((n & 0x00FF) << 8);
    return n;
}
static int rev(int v, int bits) { return rev16(v) >> (16 - bits); }

static int build(gz *z, huff *h, const uint8_t *lens, int num)
{
    int count[17], next[16];
    memset(count, 0, sizeof count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < num; i++) {
        if (lens[i] > 15) return fail(z, "the copy of AurOS is damaged (a "
                                         "code longer than allowed)");
        count[lens[i]]++;
    }
    count[0] = 0;
    for (int i = 1; i < 16; i++)
        if (count[i] > (1 << i))
            return fail(z, "the copy of AurOS is damaged (an impossible "
                           "code table)");
    int code = 0, k = 0;
    for (int i = 1; i < 16; i++) {
        next[i] = code;
        h->firstcode[i] = (uint16_t)code;
        h->firstsymbol[i] = (uint16_t)k;
        code += count[i];
        if (count[i] && code - 1 >= (1 << i))
            return fail(z, "the copy of AurOS is damaged (an over-full "
                           "code table)");
        h->maxcode[i] = code << (16 - i);
        code <<= 1;
        k += count[i];
    }
    h->maxcode[16] = 0x10000;
    for (int i = 0; i < num; i++) {
        int s = lens[i];
        if (!s) continue;
        int c = next[s] - h->firstcode[s] + h->firstsymbol[s];
        h->size[c]  = (uint8_t)s;
        h->value[c] = (uint16_t)i;
        if (s <= FAST_BITS) {
            int j = rev(next[s], s);
            while (j < (1 << FAST_BITS)) {
                h->fast[j] = (uint16_t)((s << 9) | i);
                j += 1 << s;
            }
        }
        next[s]++;
    }
    return 0;
}

static int decode(gz *z, const huff *h)
{
    if (z->nbits < 16 && fill(z) != 0) return -1;
    int f = h->fast[z->bits & FAST_MASK];
    if (f) {
        int s = f >> 9;
        z->bits >>= s;
        z->nbits -= s;
        return f & 511;
    }
    int k = rev16((int)(z->bits & 0xFFFF)), s;
    for (s = FAST_BITS + 1; s < 16; s++)
        if (k < h->maxcode[s]) break;
    if (s >= 16) return fail(z, "the copy of AurOS is damaged (an unknown "
                                "code)");
    int b = (k >> (16 - s)) - h->firstcode[s] + h->firstsymbol[s];
    if (b < 0 || b >= 288 || h->size[b] != s)
        return fail(z, "the copy of AurOS is damaged (an unknown code)");
    z->bits >>= s;
    z->nbits -= s;
    return h->value[b];
}

/* ── the three kinds of block ────────────────────────────────────── */

static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,
    31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,
    4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,
    193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
    9,9,10,10,11,11,12,12,13,13 };

static int codes(gz *z)
{
    for (;;) {
        int sym = decode(z, &z->lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (put(z, (uint8_t)sym) != 0) return -1;
            continue;
        }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return fail(z, "the copy of AurOS is damaged (a bad "
                                      "length)");
        uint32_t e = 0;
        if (LEXT[sym] && getbits(z, LEXT[sym], &e) != 0) return -1;
        uint32_t len = LBASE[sym] + e;
        int ds = decode(z, &z->dist);
        if (ds < 0) return -1;
        if (ds >= 30) return fail(z, "the copy of AurOS is damaged (a bad "
                                     "distance)");
        e = 0;
        if (DEXT[ds] && getbits(z, DEXT[ds], &e) != 0) return -1;
        if (copy_back(z, DBASE[ds] + e, len) != 0) return -1;
    }
}

static int stored(gz *z)
{
    /* Back to a byte boundary, then LEN and its complement. */
    z->bits >>= z->nbits & 7;
    z->nbits -= z->nbits & 7;
    uint32_t len, nlen;
    if (getbits(z, 16, &len) != 0 || getbits(z, 16, &nlen) != 0) return -1;
    if ((len ^ 0xFFFF) != nlen)
        return fail(z, "the copy of AurOS is damaged (a block that "
                       "contradicts itself)");
    while (len--) {
        uint32_t b;
        if (getbits(z, 8, &b) != 0) return -1;
        if (put(z, (uint8_t)b) != 0) return -1;
    }
    return 0;
}

static int fixed(gz *z)
{
    uint8_t l[288], d[30];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    for (i = 0; i < 30; i++) d[i] = 5;
    if (build(z, &z->lit, l, 288) != 0 || build(z, &z->dist, d, 30) != 0)
        return -1;
    return codes(z);
}

static int dynamic(gz *z)
{
    static const uint8_t ORDER[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,
                                       2,14,1,15 };
    uint32_t hlit, hdist, hclen;
    if (getbits(z, 5, &hlit) != 0 || getbits(z, 5, &hdist) != 0 ||
        getbits(z, 4, &hclen) != 0) return -1;
    hlit += 257; hdist += 1; hclen += 4;
    if (hlit > 286 || hdist > 30)
        return fail(z, "the copy of AurOS is damaged (too many codes)");
    uint8_t cl[19];
    memset(cl, 0, sizeof cl);
    for (uint32_t i = 0; i < hclen; i++) {
        uint32_t v;
        if (getbits(z, 3, &v) != 0) return -1;
        cl[ORDER[i]] = (uint8_t)v;
    }
    huff clh;
    if (build(z, &clh, cl, 19) != 0) return -1;

    uint8_t lens[286 + 30];
    uint32_t n = 0, want = hlit + hdist;
    while (n < want) {
        int c = decode(z, &clh);
        if (c < 0) return -1;
        if (c < 16) { lens[n++] = (uint8_t)c; continue; }
        uint32_t rep = 0, v;
        uint8_t fillv = 0;
        if (c == 16) {
            if (n == 0) return fail(z, "the copy of AurOS is damaged (a "
                                       "repeat of nothing)");
            fillv = lens[n - 1];
            if (getbits(z, 2, &v) != 0) return -1;
            rep = 3 + v;
        } else if (c == 17) {
            if (getbits(z, 3, &v) != 0) return -1;
            rep = 3 + v;
        } else if (c == 18) {
            if (getbits(z, 7, &v) != 0) return -1;
            rep = 11 + v;
        } else {
            return fail(z, "the copy of AurOS is damaged (a bad length code)");
        }
        if (n + rep > want)
            return fail(z, "the copy of AurOS is damaged (too many lengths)");
        while (rep--) lens[n++] = fillv;
    }
    if (lens[256] == 0)
        return fail(z, "the copy of AurOS is damaged (a block with no end)");
    if (build(z, &z->lit, lens, (int)hlit) != 0 ||
        build(z, &z->dist, lens + hlit, (int)hdist) != 0) return -1;
    return codes(z);
}

/* ── the gzip wrapper ────────────────────────────────────────────── */

static int byte(gz *z, uint32_t *v) { return getbits(z, 8, v); }

static int header(gz *z)
{
    uint32_t id1, id2, cm, flg, v;
    if (byte(z, &id1) || byte(z, &id2) || byte(z, &cm) || byte(z, &flg))
        return -1;
    if (z->zeros_fed)
        return fail(z, "the copy of AurOS is empty");
    if (id1 != 0x1F || id2 != 0x8B)
        return fail(z, "the copy of AurOS is not in the form this installer "
                       "reads");
    if (cm != 8 || (flg & 0xE0))
        return fail(z, "the copy of AurOS was packed in a way this installer "
                       "does not read");
    for (int i = 0; i < 6; i++) if (byte(z, &v)) return -1;  /* MTIME XFL OS */
    if (flg & 4) {                                           /* FEXTRA */
        uint32_t lo, hi;
        if (byte(z, &lo) || byte(z, &hi)) return -1;
        for (uint32_t i = 0; i < (lo | (hi << 8)); i++)
            if (byte(z, &v)) return -1;
    }
    for (int f = 8; f <= 16; f <<= 1) {                      /* FNAME FCOMMENT */
        if (!(flg & (uint32_t)f)) continue;
        uint32_t guard = 0;
        do {
            if (byte(z, &v)) return -1;
            if (++guard > 65536)
                return fail(z, "the copy of AurOS has a damaged header");
        } while (v);
    }
    if (flg & 2) { if (byte(z, &v) || byte(z, &v)) return -1; }  /* FHCRC */
    if (z->zeros_fed)
        return fail(z, "the copy of AurOS stopped partway through");
    return 0;
}

int gz_inflate(gz_reader rd, void *rud, gz_writer wr, void *wud,
               uint64_t *out_bytes, char *why, size_t n)
{
    static gz zs;                       /* 1 MB of window; not the stack */
    gz *z = &zs;
    memset(z, 0, sizeof *z);
    z->rd = rd; z->rud = rud; z->wr = wr; z->wud = wud;
    z->why = why; z->wn = n;
    crc_init();

    if (header(z) != 0) return -1;
    for (;;) {
        uint32_t last, type;
        if (getbits(z, 1, &last) != 0 || getbits(z, 2, &type) != 0) return -1;
        int rc;
        if (type == 0)      rc = stored(z);
        else if (type == 1) rc = fixed(z);
        else if (type == 2) rc = dynamic(z);
        else rc = fail(z, "the copy of AurOS is damaged (an unknown block)");
        if (rc != 0) return -1;
        if (last) break;
    }
    if (flush_out(z) != 0) return -1;

    /* The trailer, from the next byte boundary: CRC-32 and the length
     * modulo 2^32, both over what came out. */
    z->bits >>= z->nbits & 7;
    z->nbits -= z->nbits & 7;
    uint32_t t[8];
    for (int i = 0; i < 8; i++) if (byte(z, &t[i]) != 0) return -1;
    /* Every invented byte must still be sitting unread in the buffer. If
     * one was consumed, the stream ended before its own trailer. */
    if ((uint32_t)z->nbits < z->zeros_fed * 8)
        return fail(z, "the copy of AurOS stopped partway through");
    uint32_t crc  = t[0] | (t[1] << 8) | (t[2] << 16) | (t[3] << 24);
    uint32_t size = t[4] | (t[5] << 8) | (t[6] << 16) | (t[7] << 24);
    if (crc != z->crc || size != (uint32_t)z->total)
        return fail(z, "the copy of AurOS is damaged (its own check does not "
                       "match)");
    if (out_bytes) *out_bytes = z->total;
    return 0;
}

/* ── the selftest ────────────────────────────────────────────────── */

typedef struct { const uint8_t *p; size_t n, at; size_t step; } mem_in;
typedef struct { uint8_t buf[4096]; size_t n; int over; } mem_out;

static int mem_read(void *ud, uint8_t *b, size_t n, size_t *got)
{
    mem_in *m = ud;
    size_t k = m->n - m->at;
    if (k > n) k = n;
    if (m->step && k > m->step) k = m->step;   /* awkward chunking */
    memcpy(b, m->p + m->at, k);
    m->at += k;
    *got = k;
    return 0;
}
static int mem_write(void *ud, const uint8_t *b, size_t n)
{
    mem_out *m = ud;
    if (m->n + n > sizeof m->buf) { m->over = 1; return -1; }
    memcpy(m->buf + m->n, b, n);
    m->n += n;
    return 0;
}

/* Made with Python's gzip module (mtime=0); the expected text is beside
 * each. The dynamic one is forty lines built by dyn_text() below. */
static const uint8_t T_STORED[] = {   /* level 0: "stored block\n" */
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01, 0x0d,
    0x00, 0xf2, 0xff, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x20, 0x62, 0x6c,
    0x6f, 0x63, 0x6b, 0x0a, 0x6d, 0x75, 0x88, 0xc5, 0x0d, 0x00, 0x00, 0x00 };
static const uint8_t T_FIXED[] = {   /* "hello hello hello hello\n" */
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xcb, 0x48,
    0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0xb9, 0x00, 0x00, 0x88, 0x59,
    0x0b, 0x18, 0x00, 0x00, 0x00 };
static const uint8_t T_DYNAMIC[] = {   /* dyn_text below, 40 lines */
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xed, 0xd1,
    0xbb, 0x0d, 0x80, 0x30, 0x0c, 0x05, 0xc0, 0x55, 0xde, 0x00, 0x14, 0xfc,
    0x42, 0x43, 0xc5, 0x04, 0x14, 0x4c, 0x40, 0xe1, 0x28, 0x96, 0xac, 0x04,
    0x25, 0x46, 0xac, 0x8f, 0xb2, 0x82, 0x6b, 0x0f, 0x70, 0xd5, 0x09, 0x67,
    0xc2, 0x88, 0x12, 0xa1, 0x89, 0x70, 0xbc, 0xf5, 0xbc, 0xc0, 0x39, 0xca,
    0xad, 0x84, 0x46, 0x12, 0x95, 0x9a, 0x0e, 0xf8, 0x58, 0x13, 0x2a, 0x3d,
    0xa4, 0xac, 0x5c, 0xf2, 0x0e, 0xe9, 0x6c, 0xb2, 0xb1, 0xd9, 0xc6, 0x16,
    0x1b, 0x5b, 0x6d, 0x2c, 0xd8, 0xd8, 0x66, 0x63, 0x1e, 0xe0, 0x01, 0x1e,
    0xe0, 0x01, 0x1e, 0xe0, 0x01, 0x1e, 0xd0, 0xd9, 0x0f, 0xad, 0x76, 0xa4,
    0x6c, 0x98, 0x08, 0x00, 0x00 };

static int one(const uint8_t *g, size_t gn, const char *want, size_t step,
               int expect_ok)
{
    mem_in  in  = { g, gn, 0, step };
    static mem_out out;
    memset(&out, 0, sizeof out);
    char why[200] = "";
    uint64_t got = 0;
    int rc = gz_inflate(mem_read, &in, mem_write, &out, &got, why, sizeof why);
    if (!expect_ok) return rc != 0 ? 0 : 1;
    if (rc != 0) { fprintf(stderr, "inflate: %s\n", why); return 1; }
    size_t wl = strlen(want);
    return (got == wl && out.n == wl && !memcmp(out.buf, want, wl)) ? 0 : 1;
}

int gz_selftest(void)
{
    int bad = 0;
    bad += one(T_STORED, sizeof T_STORED, "stored block\n", 0, 1);
    bad += one(T_FIXED, sizeof T_FIXED, "hello hello hello hello\n", 0, 1);
    /* The same, handed over one byte at a time. */
    bad += one(T_FIXED, sizeof T_FIXED, "hello hello hello hello\n", 1, 1);
    /* Dynamic Huffman codes, with back-references, whole and in
     * three-byte scraps. */
    {
        static char want[2400];
        size_t w = 0;
        for (int i = 0; i < 40; i++)
            w += (size_t)snprintf(want + w, sizeof want - w,
                                  "line %d of the AurOS inflate selftest, "
                                  "with repetition; ", i % 7);
        bad += one(T_DYNAMIC, sizeof T_DYNAMIC, want, 0, 1);
        bad += one(T_DYNAMIC, sizeof T_DYNAMIC, want, 3, 1);
    }
    /* A wrong CRC is a refusal. */
    {
        uint8_t b[sizeof T_FIXED];
        memcpy(b, T_FIXED, sizeof b);
        b[sizeof b - 8] ^= 1;
        bad += one(b, sizeof b, "", 0, 0);
    }
    /* So is a stream cut before its trailer, and one cut in the middle. */
    bad += one(T_FIXED, sizeof T_FIXED - 4, "", 0, 0);
    bad += one(T_FIXED, 14, "", 0, 0);
    /* And something that is not gzip at all. */
    {
        static const uint8_t junk[] = "this is not gzip";
        bad += one(junk, sizeof junk, "", 0, 0);
    }
    return bad;
}
