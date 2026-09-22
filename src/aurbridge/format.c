/* format.c — see format.h. Pure functions over buffers; nothing here
 * opens anything. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "format.h"

/* ── CRC-32, the polynomial GPT and gzip both use ─────────────────── */

static uint32_t crc_tab[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

uint32_t fmt_crc32(const void *data, size_t n)
{
    if (!crc_ready) crc_init();
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── SHA-256 ─────────────────────────────────────────────────────── */

static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(fmt_sha *s, const unsigned char *b)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
               ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b2=s->h[1],c=s->h[2],d=s->h[3];
    uint32_t e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b2) ^ (a & c) ^ (b2 & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b2; b2=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b2; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f;  s->h[6]+=g; s->h[7]+=h;
}

void fmt_sha_start(fmt_sha *s)
{
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372;
    s->h[3]=0xa54ff53a; s->h[4]=0x510e527f; s->h[5]=0x9b05688c;
    s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->len = 0; s->n = 0;
}

void fmt_sha_feed(fmt_sha *s, const void *data, size_t n)
{
    const unsigned char *p = data;
    s->len += n;
    while (n) {
        size_t k = 64 - s->n;
        if (k > n) k = n;
        memcpy(s->buf + s->n, p, k);
        s->n += k; p += k; n -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

void fmt_sha_done(fmt_sha *s, unsigned char out[32])
{
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    fmt_sha_feed(s, &pad, 1);
    unsigned char z = 0;
    while (s->n != 56) fmt_sha_feed(s, &z, 1);
    unsigned char l[8];
    for (int i = 0; i < 8; i++) l[i] = (unsigned char)(bits >> (56 - 8*i));
    fmt_sha_feed(s, l, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(s->h[i] >> 24);
        out[i*4+1] = (unsigned char)(s->h[i] >> 16);
        out[i*4+2] = (unsigned char)(s->h[i] >> 8);
        out[i*4+3] = (unsigned char)(s->h[i]);
    }
}

void fmt_sha_hex(const unsigned char d[32], char *out, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    if (n < 65) { if (n) out[0] = 0; return; }
    for (int i = 0; i < 32; i++) {
        out[i*2]   = hx[d[i] >> 4];
        out[i*2+1] = hx[d[i] & 15];
    }
    out[64] = 0;
}

/* ── little-endian stores ────────────────────────────────────────── */

static void p16(uint8_t *p, uint16_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void p32(uint8_t *p, uint32_t v)
{ for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void p64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t g32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
         ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t g64(const uint8_t *p)
{ uint64_t v=0; for (int i=7;i>=0;i--) v=(v<<8)|p[i]; return v; }

/* ── the type GUIDs ──────────────────────────────────────────────── */

/* A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41 */
const uint8_t FMT_GUID_IMAGE[16] = {
    0x9C,0x5E,0x2A,0xA1, 0x6E,0xAB, 0x4D,0x4E,
    0x9F,0x35, 0x5B,0x1C,0x0A,0x2E,0x7D,0x41 };
/* 7E1C3B90-4D2A-4F16-8B77-2C6E5A9D0E33 */
const uint8_t FMT_GUID_RECORD[16] = {
    0x90,0x3B,0x1C,0x7E, 0x2A,0x4D, 0x16,0x4F,
    0x8B,0x77, 0x2C,0x6E,0x5A,0x9D,0x0E,0x33 };
/* 7E1C3B90-4D2A-4F16-8B77-2C6E5A9D0E34 */
const uint8_t FMT_GUID_SAVED[16] = {
    0x90,0x3B,0x1C,0x7E, 0x2A,0x4D, 0x16,0x4F,
    0x8B,0x77, 0x2C,0x6E,0x5A,0x9D,0x0E,0x34 };

/* ── the GPT ─────────────────────────────────────────────────────── */

#define ENTRIES   128u
#define ENTRY_SZ  128u
#define ARRAY_SZ  (ENTRIES * ENTRY_SZ)      /* 16384 */

static uint64_t array_blocks(uint32_t sector)
{ return (ARRAY_SZ + sector - 1) / sector; }

size_t fmt_gpt_head_bytes(uint32_t sector)
{ return (size_t)((2 + array_blocks(sector)) * sector); }

size_t fmt_gpt_tail_bytes(uint32_t sector)
{ return (size_t)((array_blocks(sector) + 1) * sector); }

uint64_t fmt_gpt_backup_lba(uint64_t disk_bytes, uint32_t sector)
{
    uint64_t last = disk_bytes / sector - 1;
    return last - array_blocks(sector);       /* where the tail starts */
}

uint64_t fmt_gpt_first_usable(uint32_t sector)
{ return 2 + array_blocks(sector); }

uint64_t fmt_gpt_last_usable(uint64_t disk_bytes, uint32_t sector)
{ return fmt_gpt_backup_lba(disk_bytes, sector) - 1; }

static void header(uint8_t *h, uint32_t sector, uint64_t my, uint64_t alt,
                   uint64_t first_usable, uint64_t last_usable,
                   uint64_t entry_lba, const uint8_t guid[16],
                   uint32_t array_crc)
{
    memset(h, 0, sector);
    memcpy(h, "EFI PART", 8);
    p32(h + 8, 0x00010000);          /* revision 1.0                  */
    p32(h + 12, 92);                 /* HeaderSize                    */
    p32(h + 16, 0);                  /* HeaderCRC, filled in below    */
    p64(h + 24, my);
    p64(h + 32, alt);
    p64(h + 40, first_usable);
    p64(h + 48, last_usable);
    memcpy(h + 56, guid, 16);
    p64(h + 72, entry_lba);
    p32(h + 80, ENTRIES);
    p32(h + 84, ENTRY_SZ);
    p32(h + 88, array_crc);
    p32(h + 16, fmt_crc32(h, 92));
}

int fmt_gpt_build(uint64_t disk_bytes, uint32_t sector,
                  const uint8_t disk_guid[16],
                  const fmt_part *parts, int n_parts,
                  uint8_t *head, uint8_t *tail, char *why, size_t wn)
{
    if (sector < 512 || sector > 4096 || (sector & (sector - 1))) {
        snprintf(why, wn, "a disk with %u-byte blocks is not one AurOS can "
                          "prepare", (unsigned)sector);
        return -1;
    }
    uint64_t blocks = disk_bytes / sector;
    if (blocks < 2 * (2 + array_blocks(sector)) + 8) {
        snprintf(why, wn, "this memory stick is far too small");
        return -1;
    }
    uint64_t fu = fmt_gpt_first_usable(sector);
    uint64_t lu = fmt_gpt_last_usable(disk_bytes, sector);
    uint64_t alt = blocks - 1;

    if (n_parts < 1 || (uint32_t)n_parts > ENTRIES) {
        snprintf(why, wn, "nothing to put on the memory stick");
        return -1;
    }
    for (int i = 0; i < n_parts; i++) {
        if (parts[i].first < fu || parts[i].last > lu ||
            parts[i].first > parts[i].last) {
            snprintf(why, wn,
                     "there is not enough room on this memory stick for "
                     "everything AurOS needs to put on it");
            return -1;
        }
        for (int k = 0; k < i; k++)
            if (!(parts[i].last < parts[k].first ||
                  parts[i].first > parts[k].last)) {
                snprintf(why, wn, "two parts of the memory stick would "
                                  "overlap");
                return -1;
            }
    }

    size_t ab = (size_t)(array_blocks(sector) * sector);
    uint8_t *arr = head + 2 * sector;        /* primary array in place */
    memset(head, 0, fmt_gpt_head_bytes(sector));
    memset(tail, 0, fmt_gpt_tail_bytes(sector));

    for (int i = 0; i < n_parts; i++) {
        uint8_t *e = arr + (size_t)i * ENTRY_SZ;
        memcpy(e, parts[i].type, 16);
        /* A unique GUID per partition, derived rather than random: the
         * disk GUID, the index and the extent, hashed. Random would
         * need an entropy source on Windows 7 as well as 11, and a
         * PARTUUID that is a function of what the partition IS is
         * easier to reason about when two sticks turn up with the
         * same one. */
        {
            fmt_sha s; unsigned char d[32];
            fmt_sha_start(&s);
            fmt_sha_feed(&s, disk_guid, 16);
            uint8_t nb[24];
            p64(nb, (uint64_t)i); p64(nb + 8, parts[i].first);
            p64(nb + 16, parts[i].last);
            fmt_sha_feed(&s, nb, sizeof nb);
            fmt_sha_done(&s, d);
            memcpy(e + 16, d, 16);
            /* RFC 4122 version 4 / variant bits, so that tools which
             * insist on a well-formed UUID do not call it corrupt. */
            e[16 + 7] = (uint8_t)((e[16 + 7] & 0x0F) | 0x40);
            e[16 + 8] = (uint8_t)((e[16 + 8] & 0x3F) | 0x80);
        }
        p64(e + 32, parts[i].first);
        p64(e + 40, parts[i].last);
        p64(e + 48, 0);
        for (int c = 0; c < 36 && parts[i].name[c]; c++)
            p16(e + 56 + c * 2, (uint16_t)(unsigned char)parts[i].name[c]);
    }
    uint32_t acrc = fmt_crc32(arr, ab);

    /* The protective MBR: one entry of type 0xEE covering the disk,
     * clamped at 0xFFFFFFFF the way the specification says. */
    head[446 + 0] = 0x00;
    head[446 + 1] = 0x00; head[446 + 2] = 0x02; head[446 + 3] = 0x00;
    head[446 + 4] = 0xEE;
    head[446 + 5] = 0xFF; head[446 + 6] = 0xFF; head[446 + 7] = 0xFF;
    p32(head + 446 + 8, 1);
    p32(head + 446 + 12,
        blocks - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(blocks - 1));
    head[510] = 0x55; head[511] = 0xAA;

    uint64_t barr_lba = fmt_gpt_backup_lba(disk_bytes, sector);
    header(head + sector, sector, 1, alt, fu, lu, 2, disk_guid, acrc);
    memcpy(tail, arr, ab);
    header(tail + ab, sector, alt, 1, fu, lu, barr_lba, disk_guid, acrc);
    return 0;
}

/* ── the manifest ────────────────────────────────────────────────── */

void fmt_manifest(uint8_t out[FMT_MANIFEST_BYTES],
                  uint64_t image_bytes, uint64_t root_off, uint64_t root_len,
                  uint32_t image_sector, const unsigned char root_sha[32],
                  const char *profile)
{
    memset(out, 0, FMT_MANIFEST_BYTES);
    memcpy(out, "AURIMG01", 8);
    p64(out + 8, image_bytes);
    p64(out + 16, root_off);
    p64(out + 24, root_len);
    p32(out + 32, image_sector);
    memcpy(out + 36, root_sha, 32);
    snprintf((char *)out + 68, 63, "%s", profile ? profile : "");
}

int fmt_image_root_extent(const uint8_t *gpt_head, size_t head_len,
                          uint32_t sector, uint64_t *off, uint64_t *len,
                          char *why, size_t wn)
{
    if (head_len < (size_t)(2 * sector) ||
        memcmp(gpt_head + sector, "EFI PART", 8) != 0) {
        snprintf(why, wn, "the AurOS image file is not what it should be");
        return -1;
    }
    const uint8_t *h = gpt_head + sector;
    uint64_t elba = g64(h + 72);
    uint32_t ne = g32(h + 80), es = g32(h + 84);
    if (!ne || ne > 4096 || es < 128 || es > 4096) {
        snprintf(why, wn, "the AurOS image file has an unreadable layout");
        return -1;
    }
    uint64_t need64 = elba * (uint64_t)sector + (uint64_t)ne * es;
    if (need64 > head_len) {
        /* uint64 ALL THE WAY. This was a size_t, which is 32 bits in
         * a mingw32 build, so a large PartitionEntryLBA truncated to
         * something small, passed this bound, and was then used to
         * index into the buffer. */
        snprintf(why, wn, "the AurOS image file has an unreadable layout");
        return -1;
    }
    /* The LAST partition in the image is its root: mkimage puts the
     * ESP first and the root second, and the root is the one that
     * extends to the end. Chosen by EXTENT rather than by index so
     * that an image built with a third partition one day does not
     * silently select the wrong one. */
    uint64_t best_first = 0, best_last = 0;
    int found = 0;
    for (uint32_t i = 0; i < ne; i++) {
        const uint8_t *e = gpt_head + elba * sector + (uint64_t)i * es;
        int used = 0;
        for (int q = 0; q < 16; q++) if (e[q]) { used = 1; break; }
        if (!used) continue;
        uint64_t f = g64(e + 32), l = g64(e + 40);
        /* AN ENTRY WHOSE END IS BEFORE ITS START IS NOT AN ENTRY.
         * Without this, first=2 last=1 won the `l > best_last`
         * comparison on an empty table, *len came out 0, and both
         * sides of the read-back hash were SHA-256 of nothing -- so
         * the stick was declared "ready and has been checked" with no
         * root filesystem on it at all. A failure treated as success. */
        if (l < f) continue;
        if (!found || l > best_last) { best_first = f; best_last = l; found = 1; }
    }
    if (!found || !best_last) {
        snprintf(why, wn, "the AurOS image file has no partitions in it");
        return -1;
    }
    *off = best_first * sector;
    *len = (best_last - best_first + 1) * sector;
    return 0;
}

/* ── the journal ─────────────────────────────────────────────────── */

/* ESCAPED, BECAUSE THIS IS JSON AND THE OTHER SIDE IS STRICT.
 *
 * disk_model is built from the drive's SCSI INQUIRY vendor and product
 * strings -- arbitrary vendor bytes -- and disk_serial likewise. A `"`
 * or a `\` in either used to go straight into the file, and
 * src/aurstage/journal.c's reader accepts only \\ \" \/ \n \t and
 * returns NULL on anything else. The result is `corrupt = 1`, which
 * journal.h is explicit is NOT the same as "no journal": the staging
 * environment refuses -- after the user has already sat through the
 * reboot, which is the one outcome the whole design is arranged to
 * prevent. Anything that is not printable ASCII is replaced rather
 * than escaped, because a control character in a disk model is not
 * information anybody needs and \u.... is not in the reader's
 * vocabulary either. */
static void esc(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 2 < n; p++) {
        if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = (char)*p; }
        else if (*p < 0x20 || *p > 0x7E) out[o++] = ' ';
        else out[o++] = (char)*p;
    }
    out[o] = 0;
}

size_t fmt_journal_json(const fmt_journal *j, char *out, size_t n)
{
    char e_serial[300], e_model[300], e_part[32], e_gpt[160];
    char e_stage[64], e_boot[64];
    esc(e_serial, sizeof e_serial, j->disk_serial);
    esc(e_model,  sizeof e_model,  j->disk_model);
    esc(e_part,   sizeof e_part,   j->win_part);
    esc(e_gpt,    sizeof e_gpt,    j->gpt_sha256);
    esc(e_stage,  sizeof e_stage,  j->stage);
    esc(e_boot,   sizeof e_boot,   j->boot_from);
    int k = snprintf(out, n,
        "{\"disk_serial\":\"%s\",\"disk_model\":\"%s\","
        "\"disk_bytes\":%llu,\"logical_sector\":%u,"
        "\"win_part\":\"%s\",\"win_start_lba\":%llu,\"win_sectors\":%llu,"
        "\"win_ntfs_serial\":%llu,\"gpt_sha256\":\"%s\",\"stage\":\"%s\","
        "\"boot_from\":\"%s\",\"run_id\":%llu,"
        "\"written_unix\":%llu}\n",
        e_serial, e_model,
        (unsigned long long)j->disk_bytes, (unsigned)j->logical_sector,
        e_part, (unsigned long long)j->win_start_lba,
        (unsigned long long)j->win_sectors,
        (unsigned long long)j->win_ntfs_serial,
        e_gpt, e_stage, e_boot,
        (unsigned long long)j->run_id,
        (unsigned long long)j->written_unix);
    if (k < 0 || (size_t)k >= n) return 0;
    return (size_t)k;
}

/* ── a cpio with one file in it ──────────────────────────────────── */

static size_t hex8(uint8_t *p, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)hx[(v >> (28 - 4*i)) & 15];
    return 8;
}

/* newc: a 110-byte ASCII header, the NUL-terminated name padded to a
 * 4-byte boundary, the data padded likewise, and a TRAILER!!! entry. */
static size_t cpio_entry(uint8_t *out, size_t n, size_t at,
                         const char *name, uint32_t mode, uint32_t ino,
                         const void *data, size_t len)
{
    size_t namelen = strlen(name) + 1;
    size_t need = 110 + namelen;
    need = (need + 3) & ~(size_t)3;
    size_t after = need + ((len + 3) & ~(size_t)3);
    if (at + after > n) return 0;
    uint8_t *p = out + at;
    memcpy(p, "070701", 6); p += 6;
    p += hex8(p, ino);          /* ino     */
    p += hex8(p, mode);         /* mode    */
    p += hex8(p, 0);            /* uid     */
    p += hex8(p, 0);            /* gid     */
    p += hex8(p, 1);            /* nlink   */
    p += hex8(p, 0);            /* mtime   */
    p += hex8(p, (uint32_t)len);
    p += hex8(p, 0); p += hex8(p, 0);   /* devmajor, devminor */
    p += hex8(p, 0); p += hex8(p, 0);   /* rdevmajor, rdevminor */
    p += hex8(p, (uint32_t)namelen);
    p += hex8(p, 0);            /* check   */
    memcpy(p, name, namelen);
    memset(out + at + 110 + namelen, 0, need - (110 + namelen));
    if (len) memcpy(out + at + need, data, len);
    memset(out + at + need + len, 0, ((len + 3) & ~(size_t)3) - len);
    return at + after;
}

size_t fmt_cpio_one(const char *path, const void *data, size_t data_len,
                    uint8_t *out, size_t n)
{
    /* The directory has to exist in the archive before the file in it
     * does, or the kernel's unpacker drops the file without a word. */
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    size_t at = 0;
    if (slash) {
        *slash = 0;
        at = cpio_entry(out, n, at, dir, 0040755, 1, NULL, 0);
        if (!at) return 0;
    }
    at = cpio_entry(out, n, at, path, 0100644, 2, data, data_len);
    if (!at) return 0;
    at = cpio_entry(out, n, at, "TRAILER!!!", 0, 0, NULL, 0);
    return at;
}

/* ── a gzip container with nothing compressed in it ──────────────── */

size_t fmt_gzip_store(const void *data, size_t len, uint8_t *out, size_t n)
{
    /* Each stored block carries at most 65535 bytes. */
    size_t blocks = len / 65535 + 1;
    size_t need = 10 + blocks * 5 + len + 8;
    if (need > n) return 0;
    uint8_t *p = out;
    *p++ = 0x1F; *p++ = 0x8B; *p++ = 8; *p++ = 0;      /* magic, deflate */
    p32(p, 0); p += 4;                                 /* mtime          */
    *p++ = 0; *p++ = 3;                                /* flags, unix    */
    const uint8_t *s = data;
    size_t left = len;
    do {
        uint16_t take = (uint16_t)(left > 65535 ? 65535 : left);
        *p++ = (uint8_t)((left <= 65535) ? 1 : 0);     /* BFINAL, stored */
        p16(p, take); p += 2;
        p16(p, (uint16_t)~take); p += 2;
        if (take) { memcpy(p, s, take); p += take; s += take; }
        left -= take;
    } while (left);
    p32(p, fmt_crc32(data, len)); p += 4;
    p32(p, (uint32_t)len); p += 4;
    return (size_t)(p - out);
}

/* ── the hash of a machine's partition table ─────────────────────── */

int fmt_gpt_sha256(int (*read_at)(void *ud, uint64_t off, void *buf, size_t n),
                   void *ud, uint32_t sector, char *hex, size_t n)
{
    if (n < 65 || sector < 512 || sector > 4096) return -1;
    uint8_t h[4096];
    if (read_at(ud, (uint64_t)sector, h, sector) != 0) return -1;
    if (memcmp(h, "EFI PART", 8) != 0) return -1;
    uint32_t hs = g32(h + 12);
    uint64_t elba = g64(h + 72);
    uint32_t ne = g32(h + 80), es = g32(h + 84);
    if (hs < 92 || hs > sector) return -1;
    if (es < 128 || es > 4096) return -1;
    if (!ne || ne > 4096) return -1;
    uint64_t ab = (uint64_t)ne * es;
    if (ab > 16ull * 1024 * 1024) return -1;

    fmt_sha s;
    fmt_sha_start(&s);
    fmt_sha_feed(&s, h, hs);
    uint8_t buf[65536];
    uint64_t at = 0;
    while (at < ab) {
        size_t chunk = (size_t)(ab - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (read_at(ud, elba * sector + at, buf, chunk) != 0) return -1;
        fmt_sha_feed(&s, buf, chunk);
        at += chunk;
    }
    unsigned char d[32];
    fmt_sha_done(&s, d);
    fmt_sha_hex(d, hex, n);
    return 0;
}

/* ── the selftest ────────────────────────────────────────────────── */
/*
 * Vectors, not round trips. A round trip proves this file agrees with
 * itself, which is what it would do if every constant in it were
 * wrong in the same direction. Where a published vector exists it is
 * used; where the thing under test is a layout rather than a function,
 * the bytes are checked at their offsets.
 */

/* memmem is a GNU extension and this file also builds with mingw. */
static int has_bytes(const void *hay, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    if (nn > hn) return 0;
    const unsigned char *h = hay;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(h + i, needle, nn) == 0) return 1;
    return 0;
}

static int nope(int *bad, const char *what)
{ (*bad)++; fprintf(stderr, "format: %s\n", what); return 0; }

int fmt_selftest(void)
{
    int bad = 0;

    /* CRC-32 of "123456789" is the check value in the CRC catalogue. */
    if (fmt_crc32("123456789", 9) != 0xCBF43926u)
        nope(&bad, "crc32 does not match the published check value");

    /* SHA-256 of "abc", from FIPS 180-4. */
    {
        fmt_sha s; unsigned char d[32]; char hex[65];
        fmt_sha_start(&s); fmt_sha_feed(&s, "abc", 3); fmt_sha_done(&s, d);
        fmt_sha_hex(d, hex, sizeof hex);
        if (strcmp(hex, "ba7816bf8f01cfea414140de5dae2223"
                        "b00361a396177a9cb410ff61f20015ad") != 0)
            nope(&bad, "sha256(\"abc\") is wrong");
    }
    /* And one longer than a block, because a wrong buffer boundary is
     * invisible on a three-byte message. */
    {
        fmt_sha s; unsigned char d[32]; char hex[65];
        fmt_sha_start(&s);
        fmt_sha_feed(&s, "abcdbcdecdefdefgefghfghighijhijkijkljklm"
                         "klmnlmnomnopnopq", 56);
        fmt_sha_done(&s, d);
        fmt_sha_hex(d, hex, sizeof hex);
        if (strcmp(hex, "248d6a61d20638b8e5c026930c3e6039"
                        "a33ce45964ff2167f6ecedd419db06c1") != 0)
            nope(&bad, "sha256 of a 56-byte message is wrong");
    }

    /* The GPT, checked at its offsets and then against its own CRCs
     * the way a reader would. */
    {
        uint32_t sector = 512;
        uint64_t bytes = 768ull * 1024 * 1024;
        uint8_t guid[16];
        for (int i = 0; i < 16; i++) guid[i] = (uint8_t)(i * 7 + 1);
        static uint8_t head[2 * 4096 + 16384], tail[16384 + 4096];
        uint64_t fu = fmt_gpt_first_usable(sector);
        uint64_t lu = fmt_gpt_last_usable(bytes, sector);
        fmt_part parts[3] = {
            { FMT_GUID_IMAGE,  "AUROS-IMAGE",  2048,   821247 },
            { FMT_GUID_RECORD, "AUROS-RECORD", 821248, 829439 },
            { FMT_GUID_SAVED,  "AUROS-SAVED",  829440, lu     },
        };
        char why[200];
        if (fmt_gpt_build(bytes, sector, guid, parts, 3, head, tail,
                          why, sizeof why) != 0)
            nope(&bad, "a perfectly ordinary stick layout was refused");
        if (head[510] != 0x55 || head[511] != 0xAA)
            nope(&bad, "the protective record has no signature");
        if (head[446 + 4] != 0xEE)
            nope(&bad, "the protective record is not type 0xEE");
        const uint8_t *h = head + sector;
        if (memcmp(h, "EFI PART", 8) != 0)
            nope(&bad, "the primary header is not a GPT header");
        if (g32(h + 12) != 92) nope(&bad, "HeaderSize is not 92");
        if (g64(h + 24) != 1)  nope(&bad, "MyLBA is not 1");
        if (g64(h + 32) != bytes / sector - 1)
            nope(&bad, "AlternateLBA is not the last block");
        if (g64(h + 40) != fu) nope(&bad, "FirstUsableLBA is wrong");
        if (g64(h + 48) != lu) nope(&bad, "LastUsableLBA is wrong");
        /* The header's own CRC, computed the way firmware does. */
        {
            uint8_t c[92];
            memcpy(c, h, 92); p32(c + 16, 0);
            if (fmt_crc32(c, 92) != g32(h + 16))
                nope(&bad, "the primary header's checksum does not check");
        }
        if (fmt_crc32(head + 2 * sector, 16384) != g32(h + 88))
            nope(&bad, "the entry array's checksum does not check");
        /* The backup must describe the same partitions and point the
         * other way. A backup that disagrees is how a disk becomes one
         * many tools refuse to touch. */
        const uint8_t *b = tail + 16384;
        if (memcmp(b, "EFI PART", 8) != 0)
            nope(&bad, "the backup header is not a GPT header");
        if (g64(b + 24) != bytes / sector - 1 || g64(b + 32) != 1)
            nope(&bad, "the backup header points the wrong way");
        if (g32(b + 88) != g32(h + 88))
            nope(&bad, "the two copies of the table disagree");
        if (g64(b + 72) != fmt_gpt_backup_lba(bytes, sector))
            nope(&bad, "the backup array is not where the backup says");
        if (memcmp(tail, head + 2 * sector, 16384) != 0)
            nope(&bad, "the two entry arrays are not the same bytes");
        /* The first entry, at its offsets. */
        const uint8_t *e = head + 2 * sector;
        if (memcmp(e, FMT_GUID_IMAGE, 16) != 0)
            nope(&bad, "the image partition has the wrong type");
        if (g64(e + 32) != 2048 || g64(e + 40) != 821247)
            nope(&bad, "the image partition is not where it was put");
        if (e[56] != 'A' || e[57] != 0 || e[58] != 'U')
            nope(&bad, "the partition name is not UTF-16");
        /* Overlap and out-of-range must be refused, not clamped. */
        fmt_part bad2[2] = {
            { FMT_GUID_IMAGE,  "A", 2048, 4095 },
            { FMT_GUID_RECORD, "B", 4000, 5000 },
        };
        if (fmt_gpt_build(bytes, sector, guid, bad2, 2, head, tail,
                          why, sizeof why) == 0)
            nope(&bad, "two overlapping partitions were accepted");
        fmt_part out1[1] = { { FMT_GUID_IMAGE, "A", 1, 4095 } };
        if (fmt_gpt_build(bytes, sector, guid, out1, 1, head, tail,
                          why, sizeof why) == 0)
            nope(&bad, "a partition over the table itself was accepted");
    }

    /* 4Kn: every LBA means four kilobytes, and the entry array is four
     * blocks rather than thirty-two. Getting this wrong is the eight-
     * times mistake the risk register names. */
    {
        uint32_t sector = 4096;
        uint64_t bytes = 768ull * 1024 * 1024;
        if (fmt_gpt_first_usable(sector) != 2 + 4)
            nope(&bad, "on 4Kn the first usable block is wrong");
        if (fmt_gpt_head_bytes(sector) != (size_t)(6 * 4096))
            nope(&bad, "on 4Kn the head of the table is the wrong size");
        if (fmt_gpt_backup_lba(bytes, sector) != bytes / 4096 - 1 - 4)
            nope(&bad, "on 4Kn the backup table is in the wrong place");
    }

    /* The manifest, at its offsets, because src/aurstage/image.c reads
     * it by number and not by name. */
    {
        uint8_t man[FMT_MANIFEST_BYTES];
        unsigned char sha[32];
        for (int i = 0; i < 32; i++) sha[i] = (unsigned char)(i + 1);
        fmt_manifest(man, 0x1122334455667788ull, 0x1000, 0x2000, 512,
                     sha, "desktop");
        if (memcmp(man, "AURIMG01", 8) != 0) nope(&bad, "manifest magic");
        if (g64(man + 8) != 0x1122334455667788ull)
            nope(&bad, "the manifest's image length is at the wrong offset");
        if (g64(man + 16) != 0x1000 || g64(man + 24) != 0x2000)
            nope(&bad, "the manifest's root extent is at the wrong offset");
        if (g32(man + 32) != 512)
            nope(&bad, "the manifest's sector size is at the wrong offset");
        if (memcmp(man + 36, sha, 32) != 0)
            nope(&bad, "the manifest's hash is at the wrong offset");
        if (strcmp((char *)man + 68, "desktop") != 0)
            nope(&bad, "the manifest's profile is at the wrong offset");
    }

    /* The journal. Checked for the exact field names aurstage's parser
     * looks for, since a rename on one side is an install that refuses
     * every machine. */
    {
        fmt_journal j;
        memset(&j, 0, sizeof j);
        snprintf(j.disk_serial, sizeof j.disk_serial, "AUROSTEST");
        snprintf(j.disk_model,  sizeof j.disk_model,  "QEMU");
        j.disk_bytes = 3221225472ull; j.logical_sector = 512;
        snprintf(j.win_part, sizeof j.win_part, "2");
        j.win_start_lba = 206848; j.win_sectors = 2097152;
        snprintf(j.gpt_sha256, sizeof j.gpt_sha256, "%064d", 0);
        snprintf(j.stage, sizeof j.stage, "armed");
        snprintf(j.boot_from, sizeof j.boot_from, "esp");
        j.run_id = 12345; j.written_unix = 1700000000;
        char out[1024];
        size_t k = fmt_journal_json(&j, out, sizeof out);
        if (!k) nope(&bad, "the journal did not fit in a kilobyte");
        static const char *want[] = {
            "\"disk_serial\":\"AUROSTEST\"", "\"disk_model\":\"QEMU\"",
            "\"disk_bytes\":3221225472", "\"logical_sector\":512",
            "\"win_part\":\"2\"", "\"win_start_lba\":206848",
            "\"win_sectors\":2097152", "\"win_ntfs_serial\":0",
            "\"gpt_sha256\":\"", "\"stage\":\"armed\"",
            "\"boot_from\":\"esp\"", "\"run_id\":12345",
            "\"written_unix\":1700000000", NULL };
        for (int i = 0; want[i]; i++)
            if (!strstr(out, want[i])) {
                fprintf(stderr, "format: the journal has no %s\n", want[i]);
                bad++;
            }
    }

    /* The cpio, at its offsets. */
    {
        static uint8_t buf[4096];
        const char *body = "{\"stage\":\"armed\"}\n";
        size_t k = fmt_cpio_one("aurbridge/journal.json", body, strlen(body),
                                buf, sizeof buf);
        if (!k) nope(&bad, "the cpio did not fit");
        if (memcmp(buf, "070701", 6) != 0) nope(&bad, "the cpio has no magic");
        if (k % 4) nope(&bad, "the cpio does not end on a four-byte boundary");
        if (!has_bytes(buf, k, "aurbridge/journal.json"))
            nope(&bad, "the cpio does not contain the file name");
        if (!has_bytes(buf, k, "TRAILER!!!"))
            nope(&bad, "the cpio has no trailer");
    }

    /* The gzip container. Its own header and footer, and the stored
     * block framing, because the kernel is what reads it and it will
     * not say why if this is wrong. */
    {
        static uint8_t in[70000], out[80000];
        for (size_t i = 0; i < sizeof in; i++) in[i] = (uint8_t)(i * 31);
        size_t k = fmt_gzip_store(in, sizeof in, out, sizeof out);
        if (!k) nope(&bad, "the gzip container did not fit");
        if (out[0] != 0x1F || out[1] != 0x8B || out[2] != 8)
            nope(&bad, "the gzip container has the wrong magic");
        if (g32(out + k - 4) != (uint32_t)sizeof in)
            nope(&bad, "the gzip container states the wrong length");
        if (g32(out + k - 8) != fmt_crc32(in, sizeof in))
            nope(&bad, "the gzip container states the wrong checksum");
        /* Two stored blocks, because 70000 is more than 65535: the
         * first must NOT be final and the second must be. */
        if ((out[10] & 1) != 0) nope(&bad, "the first block claims to be last");
        uint16_t len0 = (uint16_t)(out[11] | (out[12] << 8));
        if (len0 != 65535) nope(&bad, "the first stored block is not full");
        if ((out[10 + 5 + 65535] & 1) != 1)
            nope(&bad, "the last block does not say it is last");
    }

    if (!bad) fprintf(stderr, "format: every vector checks out\n");
    return bad;
}
