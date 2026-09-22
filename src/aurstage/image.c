/* image.c — see image.h. Reads the stick; writes only through wr.c. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "image.h"
#include "gpt.h"
#include "sha256.h"

/* A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41, in GPT's mixed-endian order. */
const uint8_t IMAGE_TYPE_GUID[16] = {
    0x9C,0x5E,0x2A,0xA1, 0x6E,0xAB, 0x4D,0x4E,
    0x9F,0x35, 0x5B,0x1C,0x0A,0x2E,0x7D,0x41 };

/* The manifest AurBridge writes at the very start of the image
 * partition, before the image itself. Fixed-width and dull on purpose:
 * it is read by a program running as root about to write five
 * gigabytes somewhere, and a parser is a thing that can be wrong.
 *
 * All offsets below are from the START OF THE PARTITION; the code
 * adds part_off to reach them on the disk.
 *
 *   0   8   "AURIMG01"
 *   8   8   image length, bytes
 *  16   8   root extent offset inside the image
 *  24   8   root extent length
 *  32   4   the image's own logical block size
 *  36  32   SHA-256 of the root extent
 *  68  64   profile id, NUL-padded
 * 132   8   EFI partition offset inside the image
 * 140   8   EFI partition length
 * 148  32   SHA-256 of the EFI partition
 * 180 ...   reserved, zero
 *
 * It occupies the first 4096 bytes of the partition; the image starts
 * at 4096.
 */
#define MAN_BYTES   4096
#define MAN_MAGIC   "AURIMG01"

static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t rd64(const uint8_t *p)
{ return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4) << 32); }

static int read_at(int fd, void *b, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = pread(fd, (char *)b + got, n - got, (off_t)(off + got));
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}

/* One partition of the image's OWN table, by type GUID.
 *
 * The image is a whole-disk GPT image sitting inside a partition on
 * the stick, 4096 bytes past its start, so its LBA 1 is at
 * part_off + MAN_BYTES + one of ITS blocks. Nothing here uses
 * gpt_read(): that wants an fd whose offset 0 is the disk, and this
 * one's offset 0 is the stick.
 *
 * Returns 0 and fills `off`/`len` -- byte offsets INSIDE THE IMAGE,
 * the same frame the manifest's root_off uses -- or -1 with a
 * sentence. `missing` distinguishes "the table was unreadable" from
 * "the table is fine and has no such partition", because those two
 * deserve different sentences from different callers.
 */
static int img_part_by_type(const image_src *s, const uint8_t type[16],
                            uint64_t *off, uint64_t *len, int *missing,
                            char *why, size_t n)
{
    if (missing) *missing = 0;
    int fd = open(s->dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "the AurOS image could not be read");
        return -1;
    }
    uint8_t h[4096];
    int rc = -1;
    uint32_t ss = s->image_sector ? s->image_sector : 512;
    if (ss < 512 || ss > 4096 || (ss & (ss - 1))) {
        snprintf(why, n, "the AurOS image describes an impossible disk");
        goto out;
    }
    if (read_at(fd, h, ss, s->part_off + MAN_BYTES + ss) != 0 ||
        memcmp(h, "EFI PART", 8) != 0) {
        snprintf(why, n, "the AurOS image does not look like an AurOS image");
        goto out;
    }
    uint64_t plba = rd64(h + 72);
    uint32_t num  = rd32(h + 80), esz = rd32(h + 84);
    if (esz < 128 || esz > 4096 || num == 0 || num > 256) {
        snprintf(why, n, "the AurOS image's own table is not readable");
        goto out;
    }
    /* THE BLOCK NUMBER CAME OFF THE STICK, so it is bounded before it
     * is multiplied. Unbounded it wraps -- the offset casts negative
     * and pread fails with the wrong sentence -- and merely LARGE it
     * reads the entry array from somewhere else on the stick
     * entirely, outside the image partition. */
    if (!plba || plba > s->image_bytes / ss ||
        (uint64_t)num * esz > s->image_bytes) {
        snprintf(why, n, "the AurOS image's own table is not where it says "
                         "it is");
        goto out;
    }
    uint8_t *arr = malloc((size_t)num * esz);
    if (!arr) { snprintf(why, n, "out of memory reading the AurOS image"); goto out; }
    if (read_at(fd, arr, (size_t)num * esz,
                s->part_off + MAN_BYTES + plba * ss) != 0) {
        free(arr);
        snprintf(why, n, "the AurOS image's own table could not be read");
        goto out;
    }
    uint64_t o = 0, l = 0;
    for (uint32_t i = 0; i < num; i++) {
        const uint8_t *e = arr + (size_t)i * esz;
        if (memcmp(e, type, 16) != 0) continue;
        uint64_t first = rd64(e + 32), last = rd64(e + 40);
        if (!first || last < first) continue;
        /* Bounded before the multiply, for the reason above. The sum
         * is checked below as well; both are needed, because the sum
         * check cannot see a product that has already wrapped. */
        if (last >= s->image_bytes / ss) continue;
        o = first * ss;
        l = (last - first + 1) * ss;
        break;
    }
    free(arr);
    if (!l) {
        if (missing) *missing = 1;
        snprintf(why, n, "the AurOS image does not contain the part of "
                         "itself AurOS was looking for");
        goto out;
    }
    /* INSIDE THE IMAGE, and checked here rather than by each caller:
     * an extent the manifest's own length does not contain is a
     * length that would be read off the end of the stick. */
    if (o + l < o || o + l > s->image_bytes) {
        snprintf(why, n, "the AurOS image describes a part of itself that "
                         "is not inside it");
        goto out;
    }
    if (off) *off = o;
    if (len) *len = l;
    rc = 0;
out:
    close(fd);
    return rc;
}

/* Is the image's own GPT consistent with what the manifest claims? */
static int cross_check(const image_src *s, char *why, size_t n)
{
    uint64_t off = 0, len = 0;
    int missing = 0;
    if (img_part_by_type(s, GPT_TYPE_LINUX_ROOT, &off, &len, &missing,
                         why, n) != 0) {
        if (missing)
            snprintf(why, n, "the AurOS image has no root filesystem in it");
        return -1;
    }
    if (off != s->root_off || len != s->root_len) {
        /* OUR OWN BUILD, WRONG. Worth the twenty lines that catch it:
         * the alternative is writing the image's ESP bytes into the
         * root partition and finding out after the shrink. */
        snprintf(why, n,
                 "the AurOS image and its description do not agree with each "
                 "other");
        return -1;
    }
    return 0;
}

uint64_t image_base_off(const image_src *s)
{ return s->part_off + MAN_BYTES; }

int image_esp_extent(const image_src *s, uint64_t *off, uint64_t *len,
                     char *why, size_t n)
{
    uint64_t o = 0, l = 0;
    int missing = 0;
    if (img_part_by_type(s, GPT_TYPE_ESP, &o, &l, &missing, why, n) != 0) {
        if (missing)
            snprintf(why, n, "the copy of AurOS on the memory stick has "
                             "nothing in it to start the computer with");
        return -1;
    }
    /* THE MANIFEST AND THE TABLE HAVE TO AGREE, exactly as they do for
     * the root extent -- and for the same reason, which is that the
     * hash below is of what the MANIFEST describes and the bytes
     * copied are what the TABLE describes. Two numbers that are only
     * checked separately are two numbers that can be different. */
    if (s->have_esp_sha && (s->esp_off != o || s->esp_len != l)) {
        snprintf(why, n,
                 "the AurOS image and its description do not agree about "
                 "the part that starts a computer");
        return -1;
    }
    if (off) *off = o;
    if (len) *len = l;
    return 0;
}

int image_find(const stage_machine *m, const char *want_profile,
               image_src *out, char *why, size_t n)
{
    memset(out, 0, sizeof *out);
    int seen_any = 0;

    for (int i = 0; i < m->n_disks; i++) {
        const stage_disk *d = &m->disk[i];
        char dd[80];
        snprintf(dd, sizeof dd, "/dev/%s", d->name);
        int fd = open(dd, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        gpt_table t;
        uint32_t ss = (uint32_t)(d->logical_sector > 0 ? d->logical_sector : 512);
        int got = gpt_read(fd, ss, d->bytes, &t);
        close(fd);
        if (got != 0) continue;

        for (uint32_t k = 0; k < t.n_entries; k++) {
            if (!gpt_used(&t.ent[k])) continue;
            if (memcmp(t.ent[k].type, IMAGE_TYPE_GUID, 16) != 0) continue;

            /* The partition number the kernel gave it. Built from the
             * disk name the way Linux does, which differs for names
             * ending in a digit. */
            uint64_t poff = t.ent[k].first * (uint64_t)t.sector;

            int pf = open(dd, O_RDONLY | O_CLOEXEC);
            if (pf < 0) continue;
            uint8_t man[MAN_BYTES];
            int ok = read_at(pf, man, sizeof man, poff) == 0;
            close(pf);
            if (!ok || memcmp(man, MAN_MAGIC, 8) != 0) continue;

            seen_any = 1;
            image_src c;
            memset(&c, 0, sizeof c);
            if ((size_t)snprintf(c.dev, sizeof c.dev, "%s", dd)
                    >= sizeof c.dev)
                continue;               /* refused, not truncated */
            c.part_off = poff;
            c.image_bytes  = rd64(man + 8);
            c.root_off     = rd64(man + 16);
            c.root_len     = rd64(man + 24);
            c.image_sector = rd32(man + 32);
            memcpy(c.root_sha, man + 36, 32);
            c.have_sha = 1;
            c.esp_off = rd64(man + 132);
            c.esp_len = rd64(man + 140);
            memcpy(c.esp_sha, man + 148, 32);
            for (int q = 0; q < 32; q++)
                if (c.esp_sha[q]) { c.have_esp_sha = 1; break; }
            memcpy(c.profile, man + 68, 63);
            c.profile[63] = 0;

            /* image_bytes came off the stick too, so it is not a
             * bound until something makes it one. An image that claims
             * to be larger than the partition holding it makes every
             * check below it meaningless -- and every one of those
             * checks is written as `a > b - a` rather than `a + b > c`
             * for the same reason: the sum is the thing that wraps. */
            uint64_t part_blocks = t.ent[k].last - t.ent[k].first + 1;
            uint64_t part_bytes  = part_blocks * (uint64_t)t.sector;
            if (part_blocks > UINT64_MAX / t.sector ||
                part_bytes < MAN_BYTES ||
                c.image_bytes > part_bytes - MAN_BYTES) {
                snprintf(why, n,
                         "the AurOS memory stick describes a copy of AurOS "
                         "larger than the space it is in");
                return -1;
            }
            if (!c.root_len || c.root_off > c.image_bytes ||
                c.root_len > c.image_bytes - c.root_off) {
                snprintf(why, n,
                         "the AurOS memory stick describes an image that does "
                         "not fit inside itself");
                return -1;
            }
            if (want_profile && want_profile[0] &&
                strcmp(want_profile, c.profile) != 0)
                continue;          /* somebody else's stick, or an older one */

            if (cross_check(&c, why, n) != 0) return -1;
            *out = c;
            return 0;
        }
    }

    if (seen_any)
        snprintf(why, n,
                 "the AurOS memory stick in this computer is for a different "
                 "version of AurOS.");
    else
        /* SAY WHAT WILL ACTUALLY HAPPEN. BootNext is one-shot and was
         * consumed by the boot that is running, so switching the
         * machine on again boots Windows, not the installer. The old
         * sentence -- "and switch it on again" -- read as "and it will
         * carry on", and a person who followed it exactly concluded
         * the install had silently failed. */
        snprintf(why, n,
                 "the AurOS memory stick is not in this computer.");
    return -1;
}

/* Hash a region of a file. */
static int hash_region(const char *dev, uint64_t off, uint64_t len,
                       unsigned char out[32], void (*progress)(int))
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    enum { CH = 1u << 20 };
    static unsigned char buf[CH];
    sha256 h; sha256_start(&h);
    uint64_t done = 0;
    int last = -1;
    while (done < len) {
        size_t take = len - done > CH ? CH : (size_t)(len - done);
        if (read_at(fd, buf, take, off + done) != 0) { close(fd); return -1; }
        sha256_feed(&h, buf, take);
        done += take;
        if (progress) {
            int pct = (int)(done * 100 / len);
            if (pct != last) { progress(pct); last = pct; }
        }
    }
    close(fd);
    sha256_done(&h, out);
    return 0;
}

int image_verify(const image_src *s, void (*progress)(int), char *why, size_t n)
{
    if (!s->have_sha) {
        snprintf(why, n, "the AurOS memory stick does not say what the image "
                         "should look like");
        return -1;
    }
    unsigned char got[32];
    if (hash_region(s->dev, s->part_off + MAN_BYTES + s->root_off,
                    s->root_len, got,
                    progress) != 0) {
        snprintf(why, n,
                 "the AurOS memory stick could not be read all the way "
                 "through. It may be faulty, or it may have been unplugged.");
        return -1;
    }
    if (memcmp(got, s->root_sha, 32) != 0) {
        snprintf(why, n,
                 "the copy of AurOS on the memory stick is damaged. It will "
                 "need to be written again.");
        return -1;
    }

    /* AND THE PART THAT STARTS THE COMPUTER, here, at the gate, where
     * the refusal is free.
     *
     * loader.c copies that extent onto the machine and reads it back
     * against the STICK -- the same bytes it just wrote -- so without
     * this a bit-flip in the shim or in grub is copied faithfully,
     * verified faithfully, and reported as a successful install, and
     * the machine then fails to start AurOS with no message at all.
     * It is not destructive (BootNext self-reverts to Windows) and it
     * is a product that does not work while saying it does, on a
     * computer whose Windows has already been shrunk.
     *
     * A stick from an older build carries no such hash. That is a
     * refusal rather than a shrug: this is the one check standing
     * between a damaged boot chain and a machine that will not start,
     * and "the stick is old" is a thing the person can fix in ten
     * minutes with the disk untouched. */
    if (!s->have_esp_sha || !s->esp_len) {
        snprintf(why, n,
                 "this AurOS memory stick was made by an older version and "
                 "does not say what the start-up files should look like. "
                 "Make it again.");
        return -1;
    }
    if (hash_region(s->dev, s->part_off + MAN_BYTES + s->esp_off,
                    s->esp_len, got, NULL) != 0) {
        snprintf(why, n,
                 "the AurOS memory stick could not be read all the way "
                 "through. It may be faulty, or it may have been unplugged.");
        return -1;
    }
    if (memcmp(got, s->esp_sha, 32) != 0) {
        snprintf(why, n,
                 "the part of the memory stick that starts a computer is "
                 "damaged. It will need to be written again.");
        return -1;
    }
    return 0;
}

int image_write_root(wr_target *t, const image_src *s, uint64_t dst_off,
                     void (*progress)(int), char *why, size_t n)
{
    enum { CH = 1u << 20, HOLD = 1u << 20 };
    if (s->root_len <= HOLD) {
        snprintf(why, n, "the AurOS image is impossibly small");
        return -1;
    }
    int fd = open(s->dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "the AurOS memory stick could not be read");
        return -1;
    }
    static unsigned char src[CH];
    uint64_t base = s->part_off + MAN_BYTES + s->root_off;
    int rc = -1;
    int last = -1;

    /* 1. THE FIRST MEGABYTE, ZEROED. Until the very end there must be
     *    no superblock at dst_off, so that a machine interrupted
     *    anywhere in the middle has something nothing will mount --
     *    rather than a filesystem that merely has something wrong. */
    memset(src, 0, HOLD);
    if (wr_bytes(t, WR_ROOT, dst_off, src, HOLD, why, n) != 0) goto out;

    /* 2. EVERYTHING ELSE. */
    for (uint64_t at = HOLD; at < s->root_len; ) {
        size_t take = s->root_len - at > CH ? CH : (size_t)(s->root_len - at);
        if (read_at(fd, src, take, base + at) != 0) {
            snprintf(why, n,
                     "the AurOS memory stick stopped responding partway "
                     "through. Nothing on the Windows drive has been touched.");
            goto out;
        }
        if (wr_bytes(t, WR_ROOT, dst_off + at, src, take, why, n) != 0) goto out;
        at += take;
        if (progress) {
            int pct = (int)(at * 50 / s->root_len);
            if (pct != last) { progress(pct); last = pct; }
        }
    }
    if (wr_flush(t) != 0) {
        snprintf(why, n, "this computer's disk would not finish writing");
        goto out;
    }

    /* 3. READ IT BACK, all of it, against the stick -- and hash what
     *    is on the platter at the same time, so the two questions
     *    "did the copy land" and "was the source what the build made"
     *    are both answered by bytes that are actually on the disk.
     *    R5: read-back-verify every written block. */
    sha256 h; sha256_start(&h);
    memset(src, 0, HOLD);
    /* The first megabyte is not there yet, so the hash takes it from
     * the source; the verification below covers it when it lands. */
    if (read_at(fd, src, HOLD, base) != 0) {
        snprintf(why, n, "the AurOS memory stick stopped responding");
        goto out;
    }
    sha256_feed(&h, src, HOLD);
    for (uint64_t at = HOLD; at < s->root_len; ) {
        size_t take = s->root_len - at > CH ? CH : (size_t)(s->root_len - at);
        if (read_at(fd, src, take, base + at) != 0) {
            snprintf(why, n, "the AurOS memory stick stopped responding");
            goto out;
        }
        uint64_t bad = 0;
        if (wr_check(t, dst_off + at, src, take, &bad) != 0) {
            snprintf(why, n,
                     "what was written to this computer's disk did not read "
                     "back the same, %llu MB in. The drive is failing.",
                     /* INTO THE COPY. wr_check reports an absolute
                      * device offset; printing that sends somebody to
                      * look hundreds of gigabytes into a disk for a
                      * fault a few megabytes into a partition. */
                     (unsigned long long)((bad - dst_off) / (1024 * 1024)));
            goto out;
        }
        sha256_feed(&h, src, take);
        at += take;
        if (progress) {
            int pct = 50 + (int)(at * 50 / s->root_len);
            if (pct != last) { progress(pct); last = pct; }
        }
    }
    unsigned char dig[32];
    sha256_done(&h, dig);
    if (s->have_sha && memcmp(dig, s->root_sha, 32) != 0) {
        snprintf(why, n,
                 "the copy of AurOS that was written does not match what it "
                 "should be.");
        goto out;
    }

    /* 4. AND ONLY NOW THE FIRST MEGABYTE. After this the region is a
     *    filesystem. */
    if (read_at(fd, src, HOLD, base) != 0) {
        snprintf(why, n, "the AurOS memory stick stopped responding");
        goto out;
    }
    if (wr_bytes(t, WR_ROOT, dst_off, src, HOLD, why, n) != 0) goto out;
    if (wr_flush(t) != 0) {
        snprintf(why, n, "this computer's disk would not finish writing");
        goto out;
    }
    uint64_t bad = 0;
    if (wr_check(t, dst_off, src, HOLD, &bad) != 0) {
        snprintf(why, n,
                 "the start of the new system did not read back the same.");
        goto out;
    }
    rc = 0;
out:
    close(fd);
    return rc;
}
