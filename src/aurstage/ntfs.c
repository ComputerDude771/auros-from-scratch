/* ntfs.c — see ntfs.h.
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ MUST NOT                                                         │
 * │                                                                  │
 * │ Never move, truncate or resize a partition entry whose first     │
 * │ sector carries "-FVE-FS-" at offset 3.                           │
 * │                                                                  │
 * │ Shrinking the partition without shrinking the volume destroys    │
 * │ the trailing FVE metadata copy and the ciphertext behind it:     │
 * │ instant, total, unrecoverable loss of an encrypted volume. It is │
 * │ the most destructive single mistake available anywhere in this   │
 * │ codebase, and it is a two-line mistake to make.                  │
 * │                                                                  │
 * │ ntfs_is_bitlocker() is that check, exported, so that every site  │
 * │ which touches partition geometry calls the same one.             │
 * └──────────────────────────────────────────────────────────────────┘
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntfs.h"
#include "fde.h"
#include "aurstage.h"

/* ── little-endian readers, because the disk is and we may not be ── */
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const unsigned char *p)
{ return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

static int read_at(int fd, void *buf, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}

int ntfs_is_bitlocker(const unsigned char *s)
{
    /* The BitLocker FVE header puts its signature where NTFS puts its
     * OEM id. That is the whole of the check and it is why it is
     * cheap enough to do at every site. */
    return s && memcmp(s + 3, "-FVE-FS-", 8) == 0;
}

static void refuse(ntfs_state *st, ntfs_verdict v, const char *why,
                   const char *remedy)
{
    st->verdict = v;
    snprintf(st->why, sizeof st->why, "%s", why);
    snprintf(st->remedy, sizeof st->remedy, "%s", remedy);
}

/* ── MFT records ─────────────────────────────────────────────────────
 *
 * Every record and index block on an NTFS volume carries an update
 * sequence array: the last two bytes of each sector are replaced by a
 * sequence number, and the originals live in the array. Nothing in a
 * record is valid until they are put back -- and the point of the
 * scheme is exactly the case we care about, a record half-written when
 * the power went. A parser that skips this reads two plausible bytes
 * per sector that are not the volume's data.
 */
static int apply_fixups(unsigned char *rec, size_t len, uint32_t sector_size)
{
    if (len < 48) return 0;
    uint16_t usa_off = le16(rec + 0x04);
    uint16_t usa_cnt = le16(rec + 0x06);
    if (usa_cnt == 0) return 0;
    /* usa_cnt counts the sequence number itself plus one entry per
     * fixup block. WHAT A FIXUP BLOCK IS is the awkward part: the
     * specification says "sector", and libntfs-3g hardcodes 512
     * whatever the drive reports. On an ordinary drive those are the
     * same number and the question never comes up; on a 4Kn drive
     * they are not, and picking the wrong one turns a valid record
     * into a refusal (or, worse, shifts every fixup by eight
     * sectors).
     *
     * So it is DERIVED rather than assumed: the record states its own
     * count, the record has its own length, and only one block size
     * divides them. Then that size is checked against the two it is
     * allowed to be, so this is still a refusal for anything odd
     * rather than an invitation to supply a count of three. */
    if ((size_t)(usa_cnt - 1) == 0) return 0;
    size_t blk = len / (size_t)(usa_cnt - 1);
    if (blk != 512 && blk != sector_size) return 0;
    if (blk * (size_t)(usa_cnt - 1) != len) return 0;
    if (usa_off + (size_t)usa_cnt * 2 > len) return 0;

    const unsigned char *usa = rec + usa_off;
    uint16_t seq = le16(usa);
    for (uint16_t i = 1; i < usa_cnt; i++) {
        size_t tail = (size_t)i * blk - 2;
        if (tail + 2 > len) return 0;
        /* If the tail does not carry the sequence number, this record
         * was torn: the write did not complete. Refusing is the point. */
        if (le16(rec + tail) != seq) return 0;
        rec[tail]     = usa[i * 2];
        rec[tail + 1] = usa[i * 2 + 1];
    }
    return 1;
}

/* Find an attribute of `type` in a fixed-up MFT record, returning a
 * pointer to its value and its length. Resident attributes only --
 * the two this file needs ($VOLUME_INFORMATION and the $LogFile's
 * header) are always resident, and a non-resident one of those types
 * is a volume we do not understand and will not guess about. */
static const unsigned char *find_attr(const unsigned char *rec, size_t len,
                                      uint32_t type, uint32_t *vlen)
{
    if (len < 56) return NULL;
    uint16_t off = le16(rec + 0x14);            /* first attribute */
    while ((size_t)off + 16 <= len) {
        uint32_t atype = le32(rec + off);
        if (atype == 0xFFFFFFFFu) break;        /* end marker */
        uint32_t alen = le32(rec + off + 4);
        /* A zero or unaligned length would loop for ever or walk off
         * the end; both are corruption and both stop us.
         *
         * 24, NOT 16. A resident attribute record is twenty-four bytes
         * before its value: type, length, flags, name, then
         * value_length at 0x10 and value_offset at 0x14. Accepting 16
         * meant reading those two fields out of whatever followed the
         * record -- six bytes past a 64 KiB MFT record, which an
         * adversarial review reproduced under AddressSanitizer as a
         * genuine stack overflow. */
        if (alen < 24 || alen % 8 || (size_t)off + alen > len) return NULL;
        if (atype == type) {
            if (rec[off + 8] != 0) return NULL;     /* non-resident */
            uint32_t vl = le32(rec + off + 0x10);
            uint16_t vo = le16(rec + off + 0x14);
            /* Bounded by the ATTRIBUTE, not by the record. Bounding by
             * the record is memory-safe and still wrong: a value that
             * runs past its own attribute makes the volume flags come
             * out of the next attribute along, and those bytes are
             * then logged and compared as if they were the volume's. */
            if ((size_t)vo + vl > alen) return NULL;
            if (vlen) *vlen = vl;
            return rec + off + vo;
        }
        off = (uint16_t)(off + alen);
    }
    return NULL;
}

/* ── non-resident attributes ─────────────────────────────────────────
 *
 * Everything above this point lives inside one MFT record. The two
 * questions that remain -- is there a saved Windows session in
 * hiberfil.sys, and does $LogFile have work outstanding -- cannot be
 * answered without following a file's data out onto the disk, so the
 * runlist decoder starts here.
 *
 * WHY IT IS WORTH THE CODE. Fast Startup is on by default on every
 * Windows 10 and 11 machine, and "shut down" on such a machine
 * hibernates the kernel session rather than closing it. It does NOT
 * set the volume's dirty bit. So the $Volume check above -- which is
 * the check most tools stop at -- passes a volume whose metadata is
 * about to be overwritten from RAM by a session that is still alive.
 * This is not an edge case; on a laptop bought in the last ten years
 * it is the ordinary case.
 */

/* A mapping-pairs walker. Each pair is a header byte whose low nibble
 * is the byte count of a run length and whose high nibble is the byte
 * count of a SIGNED delta from the previous run's start. A zero header
 * ends the list. Every length below comes off the disk, so every one
 * is checked before it is used to move a pointer. */
typedef struct {
    const unsigned char *p, *end;
    int64_t lcn;                       /* running, deltas accumulate  */
} runlist;

static void rl_start(runlist *r, const unsigned char *mp,
                     const unsigned char *end)
{ r->p = mp; r->end = end; r->lcn = 0; }

/* 1 and a run, or 0 for "no more" -- which covers both the proper end
 * marker and anything malformed, because a caller that has read a
 * partial runlist has read a file that is not the file it asked for. */
static int rl_next(runlist *r, uint64_t *count, int64_t *lcn)
{
    if (!r->p || r->p >= r->end) return 0;
    unsigned char h = *r->p++;
    if (h == 0) return 0;
    int lenb = h & 0x0F, offb = (h >> 4) & 0x0F;
    if (lenb == 0 || lenb > 8 || offb > 8) return 0;
    if (r->end - r->p < (ptrdiff_t)(lenb + offb)) return 0;

    uint64_t c = 0;
    for (int i = lenb - 1; i >= 0; i--) c = (c << 8) | r->p[i];
    r->p += lenb;
    if (c == 0 || c > (1ull << 48)) return 0;   /* a run of nothing, or nonsense */

    if (offb) {
        uint64_t u = 0;
        for (int i = offb - 1; i >= 0; i--) u = (u << 8) | r->p[i];
        /* Sign-extend from its own width. Shifting a negative value
         * left would be undefined, so the accumulation is unsigned and
         * the sign is put back by hand. */
        if (offb < 8 && (u & (1ull << (offb * 8 - 1))))
            u |= ~((1ull << (offb * 8)) - 1);
        r->p += offb;
        /* Accumulated as unsigned, where wrapping is defined, and only
         * then looked at as signed. `lcn += (int64_t)u` was undefined
         * on overflow, and the negative test after it only worked if
         * the thing the standard says may not happen had happened. */
        uint64_t next = (uint64_t)r->lcn + u;
        if ((int64_t)next < 0) return 0;
        r->lcn = (int64_t)next;
        *lcn = r->lcn;
    } else {
        *lcn = -1;                              /* sparse: reads as zero */
    }
    *count = c;
    return 1;
}

/* Find an attribute by type AND name, returning its header. Unlike
 * find_attr() above this does not insist the attribute be resident --
 * the caller decides, because $INDEX_ROOT must be resident and
 * $INDEX_ALLOCATION must not be, and mixing them up reads garbage.
 *
 * `name` is ASCII because every name this file looks for ("$I30", or
 * none at all) is ASCII; a name with a character outside ASCII simply
 * does not match, which is the correct answer. */
static const unsigned char *find_attr_named(const unsigned char *rec,
                                            size_t len, uint32_t type,
                                            const char *name)
{
    if (len < 56) return NULL;
    size_t nlen = name ? strlen(name) : 0;
    uint16_t off = le16(rec + 0x14);
    while ((size_t)off + 16 <= len) {
        uint32_t atype = le32(rec + off);
        if (atype == 0xFFFFFFFFu) break;
        uint32_t alen = le32(rec + off + 4);
        if (alen < 24 || alen % 8 || (size_t)off + alen > len) return NULL;
        if (atype == type) {
            uint8_t  anl = rec[off + 0x09];
            uint16_t ano = le16(rec + off + 0x0A);
            if (anl == nlen) {
                int same = 1;
                if (nlen) {
                    if ((size_t)ano + (size_t)anl * 2 > alen) return NULL;
                    for (size_t i = 0; i < nlen && same; i++) {
                        uint16_t ch = le16(rec + off + ano + i * 2);
                        if (ch != (unsigned char)name[i]) same = 0;
                    }
                }
                if (same) {
                    /* A NON-RESIDENT HEADER IS 0x40 BYTES, and this
                     * used to hand one back on the strength of the
                     * 24-byte resident minimum. Everything that
                     * followed then read data_size at 0x30 and
                     * initialized_size at 0x38 out of whatever came
                     * after the record: an adversarial review
                     * reproduced a heap overflow through
                     * check_logfile() with AddressSanitizer, from a
                     * volume whose $LogFile record simply ended in a
                     * 24-byte attribute. An attribute too short to
                     * contain its own header is corruption. */
                    if (alen < (rec[off + 0x08] ? 0x40u : 24u)) return NULL;
                    return rec + off;
                }
            }
        }
        off = (uint16_t)(off + alen);
    }
    return NULL;
}

/* The declared size of a non-resident attribute's data, or 0. Takes
 * the header length so it cannot be called on one too short to hold
 * the field -- belt and braces with find_attr_named's own check, at
 * the cost of one comparison. */
static uint64_t nonres_size(const unsigned char *a, uint32_t alen)
{ return alen >= 0x40 ? le64(a + 0x30) : 0; }

/* Read `n` bytes at virtual byte offset `voff` of a NON-RESIDENT
 * attribute whose header is `a` (length `alen`). Sparse runs read back
 * as zeroes, which is what they are.
 *
 * Only attributes that start at VCN 0 are handled. A file whose data
 * is split across several MFT records has an $ATTRIBUTE_LIST, which
 * this file does not parse -- so it returns -1 and the caller reports
 * "could not tell" rather than reading the wrong clusters confidently. */
static int read_nonres(int fd, const unsigned char *a, uint32_t alen,
                       uint32_t csize, uint64_t voff, void *buf, size_t n)
{
    if (alen < 0x40) return -1;                     /* not even a header */
    if (a[0x08] == 0) return -1;                    /* resident */
    if (le64(a + 0x10) != 0) return -1;             /* lowest_vcn != 0 */
    /* 0x00FF is the compression mask, 0x4000 is EFS. Sparse (0x8000)
     * is deliberately NOT here: a sparse run is a hole, the loop below
     * leaves it zero, and zero is what is actually stored there. */
    if (le16(a + 0x0C) & 0x40FF) return -1;
    uint16_t mpo = le16(a + 0x20);
    if (mpo < 0x40 || mpo >= alen) return -1;
    if (csize == 0) return -1;

    memset(buf, 0, n);
    runlist rl;
    rl_start(&rl, a + mpo, a + alen);

    /* EVERYTHING PAST initialized_size READS AS ZERO, and this matters
     * more than it looks. hiberfil.sys is allocated in full the moment
     * hibernation is switched on and initialized only when a session
     * is actually saved into it. Its clusters therefore hold whatever
     * the disk held before -- somebody's deleted files. Reading those
     * raw and testing them for "is this all zeroes" would report a
     * machine that has never hibernated as one that has, every time
     * the recycled clusters happened to be non-empty. */
    uint64_t init = le64(a + 0x38);
    uint64_t vcn = 0, want_lo = voff, want_hi = voff + n;
    size_t filled = 0;
    uint64_t count; int64_t lcn;
    while (rl_next(&rl, &count, &lcn)) {
        /* CHECKED, NOT ARGUED. The old code relied on rl_next's
         * 2^48 clamp to keep this from wrapping -- true today, and
         * true only because of a constant in another function that
         * says nothing about why it matters. If run_hi wraps below
         * run_lo the length below becomes about 2^64 and the bounds
         * test after it passes, because it wraps too. */
        if (vcn > UINT64_MAX / csize) return -1;
        uint64_t run_lo = vcn * csize;
        if (count > UINT64_MAX / csize) return -1;
        uint64_t span = count * csize;
        if (run_lo > UINT64_MAX - span) return -1;
        uint64_t run_hi = run_lo + span;
        if (vcn > UINT64_MAX - count) return -1;
        vcn += count;
        if (run_hi <= want_lo) continue;
        if (run_lo >= want_hi) break;

        uint64_t lo = run_lo > want_lo ? run_lo : want_lo;
        uint64_t hi = run_hi < want_hi ? run_hi : want_hi;
        size_t   at = (size_t)(lo - want_lo);
        size_t   sz = (size_t)(hi - lo);
        if (at + sz > n) return -1;
        if (lcn >= 0 && lo < init) {
            size_t real = sz;
            if (lo + real > init) real = (size_t)(init - lo);
            uint64_t phys = (uint64_t)lcn * csize + (lo - run_lo);
            if (read_at(fd, (char *)buf + at, real, phys) != 0) return -1;
        }
        /* else: sparse, and `buf` is already zero there. */
        filled += sz;
        if (filled >= n) return 0;
    }
    /* Short is not success: a caller asking for a header got part of
     * one, and part of a header is a number made up out of whatever
     * followed it. */
    return filled >= n ? 0 : -1;
}

/* ── the root directory, walked rather than searched ────────────────
 *
 * A proper B-tree descent needs NTFS's collation rules, which are a
 * Unicode uppercase table read from $UpCase. Walking every entry
 * instead needs none of that and cannot get the comparison subtly
 * wrong. The root directory of a Windows volume holds a few dozen
 * entries in a handful of 4K blocks: the cost is nothing and the
 * saving is a whole table we would have to be right about.
 */
static int name_is(const unsigned char *u16, uint8_t chars, const char *ascii)
{
    size_t n = strlen(ascii);
    if (chars != n) return 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = le16(u16 + i * 2);
        if (c > 0x7F) return 0;
        int a = (int)c, b = (unsigned char)ascii[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Walk one index node. Returns the MFT record number, -1 for "not in
 * this node", -2 for "this node does not make sense", and writes the
 * reference's SEQUENCE NUMBER through `seq`.
 *
 * The sequence number is why a stale directory entry cannot make this
 * answer a question about the wrong file: NTFS bumps it every time a
 * record is reused, so an entry left behind by an inconsistent index
 * points at a record that no longer claims that number. Dropping it
 * and keeping only the record number -- which is what this did -- is
 * how "is this volume hibernated" gets answered by whatever file
 * happens to occupy record 41 today. */
static int64_t node_walk(const unsigned char *nh, size_t avail,
                         const char *name, uint16_t *seq)
{
    if (avail < 16) return -2;
    uint32_t eo = le32(nh), il = le32(nh + 4);
    if (eo < 16 || il > avail || eo > il) return -2;
    size_t off = eo;
    while (off + 16 <= il) {
        const unsigned char *e = nh + off;
        uint16_t elen = le16(e + 8);
        uint16_t klen = le16(e + 10);
        uint16_t fl   = le16(e + 12);
        if (elen < 16 || elen % 8 || off + elen > il) return -2;
        /* The last entry carries no key -- it is the right-hand edge
         * of the node and its "name" is whatever happens to follow. */
        if (!(fl & 0x02) && klen >= 0x42 && (size_t)16 + klen <= elen) {
            uint8_t nl = e[0x10 + 0x40];
            if ((size_t)0x42 + (size_t)nl * 2 <= klen &&
                name_is(e + 0x10 + 0x42, nl, name)) {
                if (seq) *seq = le16(e + 6);
                return (int64_t)(le64(e) & 0xFFFFFFFFFFFFull);
            }
        }
        if (fl & 0x02) break;
        off += elen;
    }
    return -1;
}

/* The MFT record number of `name` in the root directory; -1 if it is
 * not there, -2 if we could not tell. */
static int64_t root_lookup(int fd, uint64_t mft_off, uint32_t mft_rec,
                           uint32_t bps, uint32_t csize, const char *name,
                           uint16_t *seq)
{
    unsigned char *rec = malloc(mft_rec);
    if (!rec) return -2;
    int64_t r = -2;

    if (read_at(fd, rec, mft_rec, mft_off + 5ull * mft_rec) != 0 ||
        memcmp(rec, "FILE", 4) != 0 || !apply_fixups(rec, mft_rec, bps))
        goto out;

    const unsigned char *ir = find_attr_named(rec, mft_rec, 0x90, "$I30");
    if (!ir || ir[0x08] != 0) goto out;            /* must be resident */
    uint32_t irlen = le32(ir + 0x10);
    uint16_t iroff = le16(ir + 0x14);
    uint32_t ahlen = le32(ir + 0x04);
    if ((size_t)iroff + irlen > ahlen || irlen < 0x20) goto out;
    const unsigned char *v = ir + iroff;

    uint32_t blk = le32(v + 0x08);                 /* index block size */
    const unsigned char *nh = v + 0x10;
    uint32_t nflags = le32(nh + 0x0C);

    int64_t hit = node_walk(nh, irlen - 0x10, name, seq);
    if (hit >= 0) { r = hit; goto out; }
    if (hit == -2) goto out;
    if (!(nflags & 1)) { r = -1; goto out; }       /* no children: absent */

    /* There are child blocks, so the answer may be in one of them. */
    const unsigned char *ia = find_attr_named(rec, mft_rec, 0xA0, "$I30");
    if (!ia || ia[0x08] == 0) goto out;
    if (blk < bps || blk > 65536 || blk % bps) goto out;
    uint64_t total = nonres_size(ia, le32(ia + 0x04));
    if (total == 0 || total % blk) goto out;
    if (total > 64ull * 1024 * 1024) goto out;     /* not a root directory */

    unsigned char *ib = malloc(blk);
    if (!ib) goto out;
    r = -1;
    for (uint64_t off = 0; off < total; off += blk) {
        if (read_nonres(fd, ia, le32(ia + 0x04), csize, off, ib, blk) != 0)
        { r = -2; break; }
        /* A block that has never been used is zeroes, not an error:
         * $INDEX_ALLOCATION is allocated in advance of being filled. */
        if (memcmp(ib, "INDX", 4) != 0) continue;
        if (!apply_fixups(ib, blk, bps)) { r = -2; break; }
        int64_t h = node_walk(ib + 0x18, blk - 0x18, name, seq);
        if (h >= 0) { r = h; break; }
        if (h == -2) { r = -2; break; }
    }
    free(ib);

out:
    free(rec);
    return r;
}

/* ── is there a saved Windows session on this volume? ─────────────── */

static ntfs_tri check_hiberfile(int fd, uint64_t mft_off, uint32_t mft_rec,
                                uint32_t bps, uint32_t csize)
{
    uint16_t seq = 0;
    int64_t n = root_lookup(fd, mft_off, mft_rec, bps, csize,
                            "hiberfil.sys", &seq);
    if (n == -1) return NTFS_NO;                   /* hibernation is off */
    if (n < 0)  return NTFS_UNSURE;

    ntfs_tri r = NTFS_UNSURE;
    unsigned char *rec = malloc(mft_rec);
    unsigned char *hdr = malloc(4096);
    if (!rec || !hdr) goto out;

    if (read_at(fd, rec, mft_rec, mft_off + (uint64_t)n * mft_rec) != 0 ||
        memcmp(rec, "FILE", 4) != 0 || !apply_fixups(rec, mft_rec, bps))
        goto out;
    if (!(le16(rec + 0x16) & 0x0001)) { r = NTFS_NO; goto out; }  /* deleted */
    /* The record must still be the one the directory named. If it is
     * not, we have found a stale entry, and the honest answer is that
     * we could not tell -- not an answer about a different file. */
    if (le16(rec + 0x10) != seq) goto out;

    const unsigned char *da = find_attr_named(rec, mft_rec, 0x80, NULL);
    if (!da) goto out;
    if (da[0x08] == 0) { r = NTFS_NO; goto out; }  /* resident: far too small */
    if (nonres_size(da, le32(da + 0x04)) < 4096) { r = NTFS_NO; goto out; }
    if (read_nonres(fd, da, le32(da + 0x04), csize, 0, hdr, 4096) != 0)
        goto out;

    /* THE RULE, AND WHICH WAY IT ERRS.
     *
     * "hibr" is a live saved session. All zeroes is the file Windows
     * leaves behind after it has resumed and thrown the session away.
     * ANYTHING ELSE is treated as a live session, because the cost of
     * the two mistakes is not remotely symmetric: calling a dead file
     * live wastes a restart, and calling a live one dead overwrites
     * the volume's metadata from RAM the next time Windows resumes. */
    if (!memcmp(hdr, "hibr", 4) || !memcmp(hdr, "HIBR", 4)) { r = NTFS_YES; goto out; }
    r = NTFS_NO;
    for (int i = 0; i < 4096; i++)
        if (hdr[i]) { r = NTFS_YES; break; }

out:
    free(rec);
    free(hdr);
    return r;
}

/* ── does $LogFile have anything outstanding? ─────────────────────── */

/* One restart page, checked. Returns 1 if it parsed, and sets `lsn`
 * and `clean`. */
static int restart_page(unsigned char *pg, uint32_t len, uint32_t bps,
                        uint64_t *lsn, int *clean)
{
    if (memcmp(pg, "RSTR", 4) != 0 && memcmp(pg, "CHKD", 4) != 0) return 0;
    if (!apply_fixups(pg, len, bps)) return 0;
    uint16_t ro = le16(pg + 0x18);
    if ((size_t)ro + 0x20 > len) return 0;
    const unsigned char *ra = pg + ro;
    *lsn = le64(ra + 0x00);
    uint16_t in_use = le16(ra + 0x0C);
    uint16_t flags  = le16(ra + 0x0E);
    /* 0xFFFF is "no client has the log open". Bit 1 is Windows'
     * own "the volume was unmounted cleanly". Either is enough; both
     * together is the ordinary state after a real shutdown. */
    *clean = (in_use == 0xFFFF) || (flags & 0x0002);
    return 1;
}

static ntfs_tri check_logfile(int fd, uint64_t mft_off, uint32_t mft_rec,
                              uint32_t bps, uint32_t csize)
{
    ntfs_tri r = NTFS_UNSURE;
    unsigned char *rec = malloc(mft_rec);
    unsigned char *pg  = malloc(8192);
    if (!rec || !pg) goto out;

    if (read_at(fd, rec, mft_rec, mft_off + 2ull * mft_rec) != 0 ||
        memcmp(rec, "FILE", 4) != 0 || !apply_fixups(rec, mft_rec, bps))
        goto out;
    const unsigned char *da = find_attr_named(rec, mft_rec, 0x80, NULL);
    if (!da || da[0x08] == 0) goto out;
    if (nonres_size(da, le32(da + 0x04)) < 8192) goto out;
    if (read_nonres(fd, da, le32(da + 0x04), csize, 0, pg, 8192) != 0)
        goto out;

    /* A log filled with 0xFF has been emptied -- by ntfsfix, or by a
     * format -- and has nothing outstanding by definition. */
    int all_ff = 1;
    for (int i = 0; i < 4096 && all_ff; i++) if (pg[i] != 0xFF) all_ff = 0;
    if (all_ff) { r = NTFS_NO; goto out; }

    uint32_t psz = le32(pg + 0x10);                /* system page size */
    if (psz < bps || psz > 4096 || psz % bps) goto out;

    /* BOTH restart pages, and the LATER one wins. They are written
     * alternately, so the stale one routinely says "clean" about a
     * state two shutdowns ago; taking whichever we read first would
     * be a coin toss on a volume that has work outstanding. */
    uint64_t lsn[2] = {0, 0};
    int ok[2] = {0, 0}, clean[2] = {0, 0};
    for (int i = 0; i < 2; i++)
        ok[i] = restart_page(pg + (uint32_t)i * psz, psz, bps, &lsn[i], &clean[i]);

    if (!ok[0] && !ok[1]) goto out;
    int use = (ok[0] && ok[1]) ? (lsn[1] > lsn[0] ? 1 : 0) : (ok[0] ? 0 : 1);
    r = clean[use] ? NTFS_NO : NTFS_YES;

out:
    free(rec);
    free(pg);
    return r;
}

/* A stable short name for each verdict, for the one machine-readable
 * line the dry run prints. Stable is the whole point: `why` is English
 * and will be rewritten and translated, and a table keyed on English
 * is a table that breaks when somebody improves a sentence. */
int ntfs_volume_bytes(const char *dev, uint64_t *out)
{
    if (out) *out = 0;
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned char b[512];
    int rc = -1;
    if (read_at(fd, b, sizeof b, 0) != 0) goto done;
    /* BitLocker first, as everywhere: every field below would be a
     * number invented by reading ciphertext. */
    if (ntfs_is_bitlocker(b)) goto done;
    if (memcmp(b + 3, "NTFS    ", 8) != 0) goto done;
    uint32_t bps = le16(b + 0x0B);
    if (bps < 256 || bps > 4096 || (bps & (bps - 1))) goto done;
    uint64_t sec = le64(b + 0x28);
    /* total_sectors counts the sectors of the volume EXCLUDING the
     * backup boot sector at the end, which is the convention NTFS
     * uses and the one ntfsresize reports against. The partition must
     * hold one more. */
    if (!sec || sec > (1ull << 48)) goto done;
    if (out) *out = (sec + 1) * bps;
    rc = 0;
done:
    close(fd);
    return rc;
}

const char *ntfs_verdict_name(ntfs_verdict v)
{
    switch (v) {
    case NTFS_OK:          return "ok";
    case NTFS_NOT_NTFS:    return "not-ntfs";
    case NTFS_BITLOCKER:   return "bitlocker";
    case NTFS_DIRTY:       return "dirty";
    case NTFS_LOG_UNCLEAN: return "log-unclean";
    case NTFS_HIBERNATED:  return "hibernated";
    case NTFS_UNREADABLE:  return "unreadable";
    case NTFS_STRANGE:     return "strange";
    }
    return "unknown";
}

/* ── the state of the volume ─────────────────────────────────────── */

void ntfs_read_state(const char *dev, ntfs_state *st)
{
    memset(st, 0, sizeof *st);

    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        refuse(st, NTFS_UNREADABLE,
               "This computer's Windows drive could not be read.",
               "It may be failing. A support engineer should look at it "
               "before anything is installed.");
        return;
    }

    unsigned char boot[512];
    if (read_at(fd, boot, sizeof boot, 0) < 0) {
        close(fd);
        refuse(st, NTFS_UNREADABLE,
               "The first part of the Windows drive could not be read.",
               "The drive may be failing. Nothing has been changed.");
        return;
    }

    /* BITLOCKER FIRST, ALWAYS, BEFORE ANY OTHER INTERPRETATION.
     * An encrypted volume's first sector is not an NTFS boot sector,
     * and every field read out of it below would be a number invented
     * by reading ciphertext. */
    if (ntfs_is_bitlocker(boot)) {
        close(fd);
        refuse(st, NTFS_BITLOCKER,
               "This computer's drive is encrypted with BitLocker.",
               "BitLocker has to be turned off completely first, which "
               "decrypts the drive. Suspending it is not enough. "
               "Search Windows for \"Manage BitLocker\".");
        return;
    }
    if (memcmp(boot + 3, "NTFS    ", 8) != 0 ||
        boot[510] != 0x55 || boot[511] != 0xAA) {
        /* WHICH "not NTFS" IS IT? The difference is the whole support
         * call. A sector with structure in it is an empty partition
         * or the wrong disk; a sector that is statistically random is
         * ciphertext, and under a sector-level encryption filter we
         * do not control there is no safe shrink at all. We cannot
         * name the product, so we do not pretend to. */
        int random = fde_looks_random(boot, sizeof boot);
        close(fd);
        if (random)
            refuse(st, NTFS_NOT_NTFS,
                   "This drive is scrambled by encryption software AurOS "
                   "does not recognise.",
                   "It has to be turned off and the drive decrypted "
                   "before AurOS can be installed. Nothing has been "
                   "changed.");
        else
            refuse(st, NTFS_NOT_NTFS,
                   "This does not look like a Windows drive.",
                   "Nothing has been changed. This may be the wrong disk.");
        return;
    }

    uint32_t bps = le16(boot + 0x0B);
    uint8_t  spc = boot[0x0D];
    st->bytes_per_sector  = bps;
    st->total_sectors     = le64(boot + 0x28);
    st->mft_lcn           = le64(boot + 0x30);
    st->serial            = le64(boot + 0x48);
    /* A sector size that is not a power of two between 256 and 4096,
     * or a cluster that is not a power of two, is not a volume any of
     * the arithmetic below is valid for. */
    if (bps < 256 || bps > 4096 || (bps & (bps - 1)) ||
        spc == 0 || (spc & (spc - 1))) {
        close(fd);
        refuse(st, NTFS_STRANGE,
               "This Windows drive is laid out in a way AurOS does not "
               "recognise.",
               "Nothing has been changed. Please send the support file "
               "to us rather than trying again.");
        return;
    }
    st->bytes_per_cluster = bps * spc;

    /* clusters_per_mft_record is signed: positive means clusters,
     * negative means 2^-n BYTES, which is how the usual 1024-byte
     * record on a 4096-byte cluster volume is expressed. Reading it
     * unsigned gives a record size of about 2^244. */
    int8_t cpr = (int8_t)boot[0x40];
    /* -cpr can be 128, and shifting a 32-bit value by 128 is undefined
     * -- not "gives zero". On x86-64 the count is masked to 5 bits, so
     * boot[0x40] = 0xD6 came out as 1024 and a volume claiming a
     * nonsensical record size was accepted as an ordinary one; a
     * different target, or a different optimiser, could do anything at
     * all with it, including deciding the size check below is
     * unreachable. */
    uint32_t mft_rec;
    if (cpr >= 0)           mft_rec = (uint32_t)cpr * st->bytes_per_cluster;
    else if (-(int)cpr < 32) mft_rec = 1u << (unsigned)(-(int)cpr);
    else                    mft_rec = 0;          /* refused just below */
    if (mft_rec < 256 || mft_rec > 65536 || mft_rec % bps) {
        close(fd);
        refuse(st, NTFS_STRANGE,
               "This Windows drive is laid out in a way AurOS does not "
               "recognise.",
               "Nothing has been changed.");
        return;
    }

    uint64_t mft_off = st->mft_lcn * st->bytes_per_cluster;

    /* ── $Volume, MFT record 3: the dirty flag ─────────────────────
     *
     * Bit 0 of the volume flags is what Windows sets when it wants
     * chkdsk to run, and what ntfsresize refuses on. We refuse on it
     * too, and for the same reason: resizing a filesystem whose own
     * metadata Windows has declared untrustworthy is the thing R9
     * forbids. */
    unsigned char rec[65536];
    st->dirty = NTFS_UNSURE;
    if (read_at(fd, rec, mft_rec, mft_off + 3ull * mft_rec) == 0 &&
        memcmp(rec, "FILE", 4) == 0 && apply_fixups(rec, mft_rec, bps)) {
        uint32_t vlen = 0;
        const unsigned char *vi = find_attr(rec, mft_rec, 0x70, &vlen);
        /* $VOLUME_INFORMATION is TWELVE bytes: eight reserved, then
         * the major and minor version, then the flags at offset 10.
         *
         * This said 14 and offset 12, which is one field along, and
         * the effect was not a wrong answer -- it was NO answer: a
         * real attribute is 12 bytes, `vlen >= 14` was never true,
         * and the dirty check below never ran on any volume at all.
         * A volume Windows had marked for chkdsk went through as
         * healthy. tools/ntfstest.sh is what found it, by building a
         * dirty volume by hand and insisting the reader say so. */
        if (vi && vlen >= 12) {
            st->volume_flags = le16(vi + 10);
            st->dirty = (st->volume_flags & 0x0001) ? NTFS_YES : NTFS_NO;
        }
    }
    if (st->dirty == NTFS_YES) {
        close(fd);
        refuse(st, NTFS_DIRTY,
               "Windows has marked this drive as needing a check.",
               "Start Windows, let it check the drive, and shut "
               "down properly. Then run this again.");
        return;
    }

    /* ── hiberfil.sys: is a Windows session still in there? ────────
     *
     * BEFORE the $LogFile check, because it is both commoner and
     * worse. Fast Startup is the default, it leaves a live session in
     * this file, and it does NOT set the dirty bit checked above --
     * so a tool that stops at the dirty bit sees a clean volume and
     * resizes a filesystem whose real metadata is in RAM, waiting to
     * be written back over ours at the next resume.
     *
     * It is also the refusal a person can clear in ten seconds, once
     * somebody tells her which ten seconds. */
    st->hibernated = check_hiberfile(fd, mft_off, mft_rec, bps,
                                     st->bytes_per_cluster);
    if (st->hibernated == NTFS_YES) {
        close(fd);
        refuse(st, NTFS_HIBERNATED,
               "Windows is not closed down -- it is only asleep, and "
               "the drive still holds the session.",
               "Start Windows. Then hold down the Shift key while you "
               "click Shut down: that closes Windows properly instead "
               "of saving it for later. Then run this again.");
        return;
    }

    /* ── $LogFile, MFT record 2: un-replayed transactions ──────────
     *
     * A clean shutdown leaves the log with nothing outstanding. If
     * there IS something outstanding, Windows has work to finish that
     * only Windows can finish -- and doing it for it, which is what
     * `ntfsfix --clear-dirty` amounts to, throws away whatever the
     * person had not saved. R2 records why that is never ours to do. */
    st->log_dirty = check_logfile(fd, mft_off, mft_rec, bps,
                                  st->bytes_per_cluster);
    if (st->log_dirty == NTFS_YES) {
        close(fd);
        refuse(st, NTFS_LOG_UNCLEAN,
               "Windows did not finish closing down last time, and it "
               "has work on this drive still to do.",
               "Start Windows and let it finish, then shut it down from "
               "the Start menu rather than by holding the power button. "
               "Then run this again.");
        return;
    }

    close(fd);
    st->verdict = NTFS_OK;
    if (st->dirty == NTFS_UNSURE || st->hibernated == NTFS_UNSURE ||
        st->log_dirty == NTFS_UNSURE)
        snprintf(st->why, sizeof st->why,
                 "The Windows drive looks healthy. Some of its records "
                 "are laid out in a way AurOS does not read for itself.");
    else
        snprintf(st->why, sizeof st->why,
                 "The Windows drive looks healthy and was shut down cleanly.");
}
