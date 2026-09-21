/* ═══════════════════════════════════════════════════════════════════
 *  ferry-hivepeek — a read-only Windows registry hive reader.
 *
 *  Ferry needs four facts that only exist inside binary `regf` files:
 *  where the user redirected their Documents folder, which timezone the
 *  machine was in, which profiles exist and where, and the OneDrive
 *  root. Shell cannot parse a binary B-tree, and libhivex is a build
 *  dependency we cannot assume is on a first-boot system built from
 *  source — so `ferry-hive` prefers hivexget when it exists and falls
 *  back to this. Same output shape either way.
 *
 *  Deliberately NOT a general registry library:
 *    - read-only, mmap-free, never writes a byte anywhere;
 *    - ignores the header checksum and the sequence numbers, because a
 *      hive lifted off a machine that was not shut down cleanly has a
 *      stale sequence and is still perfectly readable. Ferry refuses
 *      dirty volumes elsewhere; refusing twice here would only turn a
 *      recoverable read into a mystery;
 *    - ignores transaction LOG replay for the same reason. Anything a
 *      LOG would add is a setting changed seconds before shutdown.
 *
 *  It parses hostile input as root, so every read goes through ok()/rd*
 *  bounds checks and every list walk has a visit budget. A corrupt hive
 *  must fail, never wander off the end of the buffer or loop forever.
 *
 *    ferry-hivepeek HIVE KEY            values as NAME<TAB>TYPE<TAB>VALUE
 *    ferry-hivepeek HIVE KEY NAME       one value, raw ('@' = default)
 *    ferry-hivepeek -t HIVE KEY NAME    TYPE<TAB>VALUE
 *    ferry-hivepeek -k HIVE KEY         subkey names, one per line
 *
 *  exit 0 found · 1 not found · 2 unusable hive or bad usage
 * ═══════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define HIVE_BASE      0x1000u      /* cell offsets are relative to this */
#define MAX_HIVE       (512u * 1024u * 1024u)
#define MAX_VISITS     200000       /* cycle budget for one list walk    */
#define MAX_DATA       (4u * 1024u * 1024u)

static unsigned char *img;
static size_t         imglen;

static int ok(size_t off, size_t len)
{
    return off <= imglen && len <= imglen - off;
}
static uint16_t rd16(size_t off)
{
    if (!ok(off, 2)) return 0;
    return (uint16_t)(img[off] | (img[off + 1] << 8));
}
static uint32_t rd32(size_t off)
{
    if (!ok(off, 4)) return 0;
    return (uint32_t)img[off] | ((uint32_t)img[off + 1] << 8) |
           ((uint32_t)img[off + 2] << 16) | ((uint32_t)img[off + 3] << 24);
}

/* A cell offset is relative to HIVE_BASE and points at a 4-byte size
 * field; the payload starts after it. Returns 0 (an impossible payload
 * position) when the offset is the 0xffffffff "none" marker or lands
 * outside the file. */
static size_t cell(uint32_t off, size_t need)
{
    size_t at, sz;
    if (off == 0xffffffffu) return 0;
    at = (size_t)HIVE_BASE + off;
    if (!ok(at, 4)) return 0;
    sz = (size_t)(int32_t)rd32(at);
    if ((int32_t)rd32(at) < 0) sz = (size_t)(-(int32_t)rd32(at)); /* allocated */
    if (sz < 4 || !ok(at, sz)) return 0;
    if (need && sz - 4 < need) return 0;
    return at + 4;
}
static size_t cell_size(uint32_t off)
{
    size_t at = (size_t)HIVE_BASE + off;
    int32_t s;
    if (!ok(at, 4)) return 0;
    s = (int32_t)rd32(at);
    if (s < 0) s = -s;
    if (s < 4 || !ok(at, (size_t)s)) return 0;
    return (size_t)s - 4;
}
static int sig_is(size_t p, const char *s)
{
    return p && ok(p, 2) && img[p] == (unsigned char)s[0] &&
           img[p + 1] == (unsigned char)s[1];
}

/* ── Text. Registry names and REG_SZ data are UTF-16LE unless a flag
 *    says the name is Latin-1. Emit UTF-8 so the shell can handle it. */
static void put_utf8(char **o, char *end, unsigned long c)
{
    char *p = *o;
    if (c < 0x80)          { if (end - p < 1) return; *p++ = (char)c; }
    else if (c < 0x800)    { if (end - p < 2) return;
                             *p++ = (char)(0xC0 | (c >> 6));
                             *p++ = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000)  { if (end - p < 3) return;
                             *p++ = (char)(0xE0 | (c >> 12));
                             *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
                             *p++ = (char)(0x80 | (c & 0x3F)); }
    else                   { if (end - p < 4) return;
                             *p++ = (char)(0xF0 | (c >> 18));
                             *p++ = (char)(0x80 | ((c >> 12) & 0x3F));
                             *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
                             *p++ = (char)(0x80 | (c & 0x3F)); }
    *o = p;
}
static void utf16le_to_utf8(size_t src, size_t bytes, char *out, size_t outsz)
{
    char *o = out, *end = out + outsz - 1;
    size_t i;
    for (i = 0; i + 1 < bytes && o < end; i += 2) {
        unsigned long c = rd16(src + i);
        if (c == 0) break;                       /* trailing NUL padding */
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes) {   /* surrogate */
            unsigned long lo = rd16(src + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        put_utf8(&o, end, c);
    }
    *o = '\0';
}
static void latin1_to_utf8(size_t src, size_t bytes, char *out, size_t outsz)
{
    char *o = out, *end = out + outsz - 1;
    size_t i;
    for (i = 0; i < bytes && o < end; i++) {
        if (!ok(src + i, 1) || img[src + i] == 0) break;
        put_utf8(&o, end, img[src + i]);
    }
    *o = '\0';
}

/* ── nk (key node). Offsets per the on-disk layout; named rather than
 *    magic-numbered inline because getting one wrong reads garbage. */
#define NK_FLAGS        0x02
#define NK_SUBKEY_COUNT 0x14
#define NK_SUBKEY_LIST  0x1C
#define NK_VALUE_COUNT  0x24
#define NK_VALUE_LIST   0x28
#define NK_NAME_LEN     0x48
#define NK_NAME         0x4C
#define NK_FLAG_ASCII   0x0020

static void nk_name(size_t nk, char *out, size_t outsz)
{
    size_t len = rd16(nk + NK_NAME_LEN);
    out[0] = '\0';
    if (!ok(nk + NK_NAME, len)) return;
    if (rd16(nk + NK_FLAGS) & NK_FLAG_ASCII)
        latin1_to_utf8(nk + NK_NAME, len, out, outsz);
    else
        utf16le_to_utf8(nk + NK_NAME, len, out, outsz);
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Walk a subkey list (lf/lh hash leaves, li index leaf, ri index root).
 * If `want` is NULL every subkey name is printed; otherwise the matching
 * subkey's cell offset is returned. `budget` bounds a malformed or
 * deliberately cyclic list. */
static size_t subkeys_walk(uint32_t listoff, const char *want, int *budget, int depth)
{
    size_t list = cell(listoff, 4);
    unsigned count, i;
    if (!list || depth > 8) return 0;
    count = rd16(list + 2);

    if (sig_is(list, "ri")) {
        for (i = 0; i < count; i++) {
            size_t sub;
            if (*budget <= 0) return 0;
            (*budget)--;
            sub = subkeys_walk(rd32(list + 4 + i * 4), want, budget, depth + 1);
            if (sub) return sub;
        }
        return 0;
    }
    if (!sig_is(list, "lf") && !sig_is(list, "lh") && !sig_is(list, "li"))
        return 0;
    {
        unsigned stride = sig_is(list, "li") ? 4u : 8u;
        for (i = 0; i < count; i++) {
            size_t nk;
            char nm[512];
            if (*budget <= 0) return 0;
            (*budget)--;
            if (!ok(list + 4 + i * stride, 4)) return 0;
            nk = cell(rd32(list + 4 + i * stride), NK_NAME);
            if (!nk || !sig_is(nk, "nk")) continue;
            nk_name(nk, nm, sizeof nm);
            if (!want) { printf("%s\n", nm); continue; }
            if (ieq(nm, want)) return nk;
        }
    }
    return 0;
}

/* Resolve a backslash- or slash-separated path from the root key. */
static size_t key_find(const char *path)
{
    size_t nk = cell(rd32(0x24), NK_NAME);
    char buf[2048];
    char *p;
    if (!nk || !sig_is(nk, "nk")) return 0;
    if (!path || !*path) return nk;
    snprintf(buf, sizeof buf, "%s", path);

    p = buf;
    while (*p) {
        char *seg;
        int budget = MAX_VISITS;
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        seg = p;
        while (*p && *p != '\\' && *p != '/') p++;
        if (*p) *p++ = '\0';
        if (!*seg) continue;
        nk = subkeys_walk(rd32(nk + NK_SUBKEY_LIST), seg, &budget, 0);
        if (!nk) return 0;
    }
    return nk;
}

/* ── vk (value). Data under 5 bytes lives inline in the offset field;
 *    data over ~16 KiB is split across a `db` segment list. */
#define VK_NAME_LEN 0x02
#define VK_DATA_LEN 0x04
#define VK_DATA_OFF 0x08
#define VK_TYPE     0x0C
#define VK_FLAGS    0x10
#define VK_NAME     0x14

static void vk_name(size_t vk, char *out, size_t outsz)
{
    size_t len = rd16(vk + VK_NAME_LEN);
    out[0] = '\0';
    if (len == 0) { snprintf(out, outsz, "@"); return; }   /* default value */
    if (!ok(vk + VK_NAME, len)) return;
    if (rd16(vk + VK_FLAGS) & 1)
        latin1_to_utf8(vk + VK_NAME, len, out, outsz);
    else
        utf16le_to_utf8(vk + VK_NAME, len, out, outsz);
}

/* Returns a malloc'd copy of the value data (caller frees) and its
 * length. A copy rather than a pointer because `db` data is scattered. */
static unsigned char *vk_data(size_t vk, size_t *outlen)
{
    uint32_t raw = rd32(vk + VK_DATA_LEN);
    size_t len = raw & 0x7fffffffu;
    unsigned char *buf;

    *outlen = 0;
    if (len > MAX_DATA) len = MAX_DATA;

    if (raw & 0x80000000u) {                     /* inline, <= 4 bytes */
        if (len > 4) len = 4;
        buf = malloc(len + 2);
        if (!buf) return NULL;
        memcpy(buf, img + vk + VK_DATA_OFF, len);
        buf[len] = buf[len + 1] = 0;
        *outlen = len;
        return buf;
    }
    {
        uint32_t off = rd32(vk + VK_DATA_OFF);
        size_t d = cell(off, 0);
        if (!d) return NULL;

        if (sig_is(d, "db")) {                   /* big data: segments  */
            unsigned segs = rd16(d + 2);
            size_t list = cell(rd32(d + 4), 4);
            size_t got = 0;
            unsigned i;
            if (!list) return NULL;
            buf = malloc(len + 2);
            if (!buf) return NULL;
            for (i = 0; i < segs && got < len; i++) {
                size_t seg = cell(rd32(list + i * 4), 0);
                size_t ssz = cell_size(rd32(list + i * 4));
                size_t take;
                if (!seg) break;
                if (ssz > 16344) ssz = 16344;    /* segment payload cap */
                take = len - got < ssz ? len - got : ssz;
                if (!ok(seg, take)) break;
                memcpy(buf + got, img + seg, take);
                got += take;
            }
            buf[got] = buf[got + 1] = 0;
            *outlen = got;
            return buf;
        }
        if (cell_size(off) < len) len = cell_size(off);
        if (!ok(d, len)) return NULL;
        buf = malloc(len + 2);
        if (!buf) return NULL;
        memcpy(buf, img + d, len);
        buf[len] = buf[len + 1] = 0;
        *outlen = len;
        return buf;
    }
}

static const char *type_name(uint32_t t)
{
    switch (t) {
        case 0:  return "none";
        case 1:  return "string";
        case 2:  return "expand";
        case 3:  return "binary";
        case 4:  return "dword";
        case 5:  return "dword-be";
        case 6:  return "link";
        case 7:  return "multi";
        case 11: return "qword";
        default: return "unknown";
    }
}

/* One line of output, so a control character inside a value can never
 * forge a second line for the shell parsing us. Backslash is left alone
 * on purpose: registry values are mostly paths, and escaping it would
 * make every caller un-escape a path it is about to use. Only control
 * characters are encoded, and nothing un-escapes them. */
static void print_escaped(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n')      fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') ;                    /* CRLF -> LF */
        else if (c < 0x20)  printf("\\x%02x", c);
        else                putchar(c);
    }
}

/* Render one value into a UTF-8 string. MULTI_SZ joins with ';' — the
 * callers that read multi-sz (nothing yet) split on it; the alternative,
 * newlines, would break line-oriented shell parsing. */
static void render(uint32_t type, unsigned char *d, size_t len, char *out, size_t outsz)
{
    size_t save_len = imglen;
    unsigned char *save_img = img;
    out[0] = '\0';
    if (!d) return;

    /* utf16le_to_utf8 reads through the bounds-checked accessors, so
     * point them at the value buffer for the duration. */
    img = d; imglen = len;

    switch (type) {
        case 1: case 2: case 6:
            utf16le_to_utf8(0, len, out, outsz);
            break;
        case 7: {
            size_t i = 0, o = 0;
            while (i + 1 < len) {
                char part[1024];
                size_t j = i;
                while (j + 1 < len && rd16(j)) j += 2;
                utf16le_to_utf8(i, j - i, part, sizeof part);
                if (!part[0]) break;
                o += (size_t)snprintf(out + o, o < outsz ? outsz - o : 0,
                                      "%s%s", o ? ";" : "", part);
                if (o >= outsz) break;
                i = j + 2;
            }
            break;
        }
        case 4:
            snprintf(out, outsz, "%lu",
                     (unsigned long)(len >= 4 ? (uint32_t)(d[0] | (d[1] << 8) |
                     ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24)) : 0));
            break;
        case 5:
            snprintf(out, outsz, "%lu",
                     (unsigned long)(len >= 4 ? (uint32_t)(d[3] | (d[2] << 8) |
                     ((uint32_t)d[1] << 16) | ((uint32_t)d[0] << 24)) : 0));
            break;
        case 11: {
            unsigned long long v = 0;
            int i;
            for (i = (len >= 8 ? 7 : (int)len - 1); i >= 0; i--)
                v = (v << 8) | d[i];
            snprintf(out, outsz, "%llu", v);
            break;
        }
        default: {                                /* binary as lower hex */
            size_t i, o = 0;
            for (i = 0; i < len && o + 3 < outsz; i++)
                o += (size_t)snprintf(out + o, outsz - o, "%02x", d[i]);
            break;
        }
    }
    img = save_img; imglen = save_len;
}

static int values(size_t nk, const char *want, int show_type)
{
    uint32_t n = rd32(nk + NK_VALUE_COUNT);
    size_t list;
    uint32_t i;
    int found = 0;
    char text[65536];

    if (n == 0 || n == 0xffffffffu) return 0;
    if (n > 65535) n = 65535;
    list = cell(rd32(nk + NK_VALUE_LIST), 4);
    if (!list) return 0;

    for (i = 0; i < n; i++) {
        size_t vk;
        char nm[512];
        uint32_t type;
        unsigned char *d;
        size_t dlen;

        if (!ok(list + i * 4, 4)) break;
        vk = cell(rd32(list + i * 4), VK_NAME);
        if (!vk || !sig_is(vk, "vk")) continue;
        vk_name(vk, nm, sizeof nm);
        if (want && !ieq(nm, want)) continue;

        type = rd32(vk + VK_TYPE);
        d = vk_data(vk, &dlen);
        render(type, d, dlen, text, sizeof text);
        free(d);

        if (want) {
            if (show_type) printf("%s\t", type_name(type));
            print_escaped(text);
            putchar('\n');
            return 1;
        }
        printf("%s\t%s\t", nm, type_name(type));
        print_escaped(text);
        putchar('\n');
        found = 1;
    }
    return found;
}

int main(int argc, char **argv)
{
    const char *hive, *path, *want = NULL;
    int list_keys = 0, show_type = 0, a = 1;
    FILE *f;
    long sz;
    size_t nk;

    while (a < argc && argv[a][0] == '-' && argv[a][1]) {
        if (!strcmp(argv[a], "-k")) list_keys = 1;
        else if (!strcmp(argv[a], "-t")) show_type = 1;
        else { fprintf(stderr, "ferry-hivepeek: unknown option %s\n", argv[a]); return 2; }
        a++;
    }
    if (argc - a < 2) {
        fprintf(stderr,
            "usage: ferry-hivepeek [-k] [-t] HIVE KEYPATH [VALUENAME]\n");
        return 2;
    }
    hive = argv[a]; path = argv[a + 1];
    if (argc - a >= 3) want = argv[a + 2];

    f = fopen(hive, "rb");
    if (!f) { fprintf(stderr, "ferry-hivepeek: cannot open %s\n", hive); return 2; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= (long)HIVE_BASE || (unsigned long)sz > MAX_HIVE) {
        fprintf(stderr, "ferry-hivepeek: %s is not a usable hive (%ld bytes)\n",
                hive, sz);
        fclose(f); return 2;
    }
    img = malloc((size_t)sz);
    if (!img) { fclose(f); return 2; }
    imglen = fread(img, 1, (size_t)sz, f);
    fclose(f);

    if (imglen < HIVE_BASE || memcmp(img, "regf", 4) != 0) {
        fprintf(stderr, "ferry-hivepeek: %s has no regf header\n", hive);
        return 2;
    }

    nk = key_find(path);
    if (!nk) return 1;
    if (list_keys) {
        int budget = MAX_VISITS;
        subkeys_walk(rd32(nk + NK_SUBKEY_LIST), NULL, &budget, 0);
        return 0;
    }
    return values(nk, want, show_type) ? 0 : 1;
}
