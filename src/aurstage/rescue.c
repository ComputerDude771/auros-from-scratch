/* rescue.c — see rescue.h. Reads the machine; writes only through wr.c. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "rescue.h"
#include "gpt.h"
#include "ntfs.h"
#include "sha256.h"
#include "shrink.h"
#include "fault.h"

/* 7E1C3B90-4D2A-4F16-8B77-2C6E5A9D0E34, in GPT's mixed-endian order.
 * One past the record partition's GUID, generated with it, so that a
 * stick carrying one and not the other is an obvious mistake rather
 * than two unrelated numbers. */
const uint8_t RESCUE_TYPE_GUID[16] = {
    0x90,0x3B,0x1C,0x7E, 0x2A,0x4D, 0x16,0x4F,
    0x8B,0x77, 0x2C,0x6E,0x5A,0x9D,0x0E,0x34 };

/* ── the dullest possible byte handling ──────────────────────────── */

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

static void wr64(uint8_t *p, uint64_t v)
{ for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr32(uint8_t *p, uint32_t v)
{ for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* pread until the whole thing is there. A short read at the end of a
 * device is a failure here and not a truncation: every caller below is
 * deciding what to put back on somebody's disk. */
static int read_at(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t k = pread(fd, p, len, (off_t)off);
        if (k <= 0) return -1;
        p += k; off += (uint64_t)k; len -= (size_t)k;
    }
    return 0;
}

/* ── finding the area ────────────────────────────────────────────── */

int rescue_find_n(const stage_machine *m, rescue_area *out, int max)
{
    int found = 0;
    for (int i = 0; i < m->n_disks && found < max; i++) {
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
        for (uint32_t k = 0; k < t.n_entries && found < max; k++) {
            if (!gpt_used(&t.ent[k])) continue;
            if (memcmp(t.ent[k].type, RESCUE_TYPE_GUID, 16) != 0) continue;
            rescue_area *a = &out[found];
            memset(a, 0, sizeof *a);
            if ((size_t)snprintf(a->dev, sizeof a->dev, "%s", dd)
                    >= sizeof a->dev) continue;
            a->part_off   = t.ent[k].first * (uint64_t)t.sector;
            a->part_bytes = (t.ent[k].last - t.ent[k].first + 1)
                            * (uint64_t)t.sector;
            found++;
        }
    }
    return found;
}

int rescue_find(const stage_machine *m, const char *exclude_disk,
                rescue_area *out, char *why, size_t n)
{
    memset(out, 0, sizeof *out);
    for (int i = 0; i < m->n_disks; i++) {
        const stage_disk *d = &m->disk[i];
        if (exclude_disk && exclude_disk[0] &&
            strcmp(d->name, exclude_disk) == 0) continue;
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
            if (memcmp(t.ent[k].type, RESCUE_TYPE_GUID, 16) != 0) continue;
            if ((size_t)snprintf(out->dev, sizeof out->dev, "%s", dd)
                    >= sizeof out->dev) continue;
            out->part_off   = t.ent[k].first * (uint64_t)t.sector;
            out->part_bytes = (t.ent[k].last - t.ent[k].first + 1)
                              * (uint64_t)t.sector;
            return 0;
        }
    }
    snprintf(why, n,
             "the AurOS memory stick does not have a place to keep the way "
             "back on it. Make the stick again with the AurOS installer and "
             "try once more.");
    return -1;
}

/* ── what a capture of this machine would cost ───────────────────── */

/* The ESP, whole, plus the table, plus a page per NTFS volume, plus
 * the header. Rounded generously: an underestimate here is a refusal
 * discovered after the shrink, which is the one place in the product
 * where being wrong is expensive. */
static int esp_and_table_bytes(const stage_disk *d, uint64_t *need,
                               uint64_t *esp_lba, uint64_t *esp_blocks,
                               char *why, size_t n)
{
    char dd[80];
    snprintf(dd, sizeof dd, "/dev/%s", d->name);
    if (!d->sector_known) {
        snprintf(why, n,
                 "this computer will not say how big a block on its disk is, "
                 "and the way back cannot be saved without knowing.");
        return -1;
    }
    int fd = open(dd, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "the disk in this computer could not be read");
        return -1;
    }
    gpt_table t;
    int got = gpt_read(fd, (uint32_t)d->logical_sector, d->bytes, &t);
    close(fd);
    if (got != 0) {
        snprintf(why, n, "the way this disk is divided up could not be read");
        return -1;
    }
    if (t.from_backup) {
        snprintf(why, n,
                 "the main copy of this disk's partition table is already "
                 "damaged, and only the spare copy could be read. AurOS "
                 "will not promise to put back a machine that was broken "
                 "before it arrived. Repair the table from Windows first.");
        return -1;
    }
    int esps = 0;
    uint64_t elba = 0, eblk = 0;
    uint64_t ntfs_like = 0;
    for (uint32_t k = 0; k < t.n_entries; k++) {
        if (!gpt_used(&t.ent[k])) continue;
        if (memcmp(t.ent[k].type, GPT_TYPE_ESP, 16) == 0) {
            esps++;
            elba = t.ent[k].first;
            eblk = t.ent[k].last - t.ent[k].first + 1;
        } else {
            ntfs_like++;
        }
    }
    if (esps != 1) {
        snprintf(why, n,
                 esps == 0
                 ? "this computer has no EFI partition, so there is no "
                   "Windows startup to save and nothing to put back. AurOS "
                   "only installs on computers that start the modern way."
                 : "this computer has more than one EFI partition. AurOS "
                   "will not guess which one it starts from; saving the "
                   "wrong one produces a way back that leads to a black "
                   "screen.");
        return -1;
    }
    if (esp_lba) *esp_lba = elba;
    if (esp_blocks) *esp_blocks = eblk;

    uint64_t sec = (uint64_t)t.sector;
    uint64_t arr = (uint64_t)t.n_entries * t.entry_size;
    arr = ((arr + sec - 1) / sec) * sec;
    uint64_t total = RESCUE_HDR_BYTES
                   + sec                       /* protective MBR       */
                   + sec + arr                 /* primary head + array */
                   + sec + arr                 /* backup head + array  */
                   + eblk * sec                /* the ESP, whole       */
                   + ntfs_like * (8192 + sec + sec);  /* $Boot + spare */
    *need = total;
    return 0;
}

int rescue_size_needed(const stage_disk *d, uint64_t *need,
                       char *why, size_t n)
{ return esp_and_table_bytes(d, need, NULL, NULL, why, n); }

/* ── the header, on disk ─────────────────────────────────────────── */

static void hdr_pack(const rescue_payload *p, uint8_t *h)
{
    memset(h, 0, RESCUE_HDR_BYTES);
    memcpy(h, RESCUE_MAGIC, 8);
    wr64(h + 8,  p->payload_bytes);
    wr64(h + 16, p->disk_bytes);
    wr32(h + 24, p->sector);
    wr32(h + 28, p->n_sections);
    wr64(h + 32, p->captured_unix);
    wr64(h + 40, p->run_id);
    memcpy(h + 48,  p->serial, 80);
    memcpy(h + 128, p->model, 64);
    wr64(h + 192, p->esp_lba);
    wr64(h + 200, p->esp_blocks);
    memcpy(h + 208, p->body_sha, 32);
    for (uint32_t i = 0; i < p->n_sections; i++) {
        uint8_t *e = h + 256 + i * RESCUE_SEC_BYTES;
        wr32(e + 0,  p->sec[i].kind);
        wr32(e + 4,  p->sec[i].index);
        wr64(e + 8,  p->sec[i].disk_lba);
        wr64(e + 16, p->sec[i].lba_count);
        wr64(e + 24, p->sec[i].off);
        wr64(e + 32, p->sec[i].len);
        wr64(e + 40, p->sec[i].aux);
        wr64(e + 48, p->sec[i].aux2);
        memcpy(e + 56, p->sec[i].sha, 32);
    }
    /* Last, over everything else, with its own four bytes still zero. */
    wr32(h + 240, gpt_crc32(h, RESCUE_HDR_BYTES));
}

static int hdr_unpack(const uint8_t *h, rescue_payload *p, char *why, size_t n)
{
    memset(p, 0, sizeof *p);
    if (memcmp(h, RESCUE_MAGIC, 8) != 0) {
        snprintf(why, n,
                 "there is no saved way back on this memory stick.");
        return -1;
    }
    uint8_t copy[RESCUE_HDR_BYTES];
    memcpy(copy, h, RESCUE_HDR_BYTES);
    uint32_t want = rd32(copy + 240);
    memset(copy + 240, 0, 4);
    if (gpt_crc32(copy, RESCUE_HDR_BYTES) != want) {
        snprintf(why, n,
                 "the saved way back on this memory stick is damaged and "
                 "cannot be trusted. Do not use it to put Windows back.");
        return -1;
    }
    p->payload_bytes = rd64(h + 8);
    p->disk_bytes    = rd64(h + 16);
    p->sector        = rd32(h + 24);
    p->n_sections    = rd32(h + 28);
    p->captured_unix = rd64(h + 32);
    p->run_id        = rd64(h + 40);
    memcpy(p->serial, h + 48, 79);  p->serial[79] = 0;
    memcpy(p->model,  h + 128, 63); p->model[63]  = 0;
    p->esp_lba    = rd64(h + 192);
    p->esp_blocks = rd64(h + 200);
    memcpy(p->body_sha, h + 208, 32);
    if (p->n_sections == 0 || p->n_sections > RESCUE_MAX_SEC ||
        p->sector < 512 || p->sector > 65536 ||
        (p->sector & (p->sector - 1)) ||
        p->disk_bytes < (uint64_t)p->sector * 64 ||
        p->payload_bytes <= RESCUE_HDR_BYTES) {
        snprintf(why, n,
                 "the saved way back on this memory stick describes "
                 "something impossible and will not be used.");
        return -1;
    }
    for (uint32_t i = 0; i < p->n_sections; i++) {
        const uint8_t *e = h + 256 + i * RESCUE_SEC_BYTES;
        p->sec[i].kind      = rd32(e + 0);
        p->sec[i].index     = rd32(e + 4);
        p->sec[i].disk_lba  = rd64(e + 8);
        p->sec[i].lba_count = rd64(e + 16);
        p->sec[i].off       = rd64(e + 24);
        p->sec[i].len       = rd64(e + 32);
        p->sec[i].aux       = rd64(e + 40);
        p->sec[i].aux2      = rd64(e + 48);
        memcpy(p->sec[i].sha, e + 56, 32);
        /* Every section must lie wholly inside the payload, and after
         * the header. A section that does not is not a section. */
        if (p->sec[i].len == 0 ||
            p->sec[i].off < RESCUE_HDR_BYTES ||
            p->sec[i].off > p->payload_bytes ||
            p->sec[i].len > p->payload_bytes - p->sec[i].off) {
            snprintf(why, n,
                     "the saved way back on this memory stick points outside "
                     "itself and will not be used.");
            return -1;
        }

        /* AND WHERE ON THE DISK IT SAYS IT CAME FROM, which is the
         * field that decides where the restore WRITES.
         *
         * This was checked nowhere, and every write offset in the
         * restore is `disk_lba * sector`. A section whose disk_lba is
         * 0x0020000000000000 multiplies to zero on a 4Kn disk, and the
         * captured EFI partition then lands on the partition table and
         * the head of C:. The payload hash does not catch it -- the
         * header is not in the hash -- and wr.c cannot, because by the
         * time it sees the offset the wrap has already happened. A
         * memory stick lives in a drawer for eighteen months and is
         * protected by one CRC-32; the numbers on it are input.
         *
         * len == lba_count * sector ties the payload to the extent, so
         * a section cannot claim to be a megabyte of disk and carry
         * four bytes. */
        uint64_t blocks = p->disk_bytes / p->sector;
        if (p->sec[i].lba_count == 0 ||
            p->sec[i].lba_count > blocks ||
            p->sec[i].disk_lba > blocks - p->sec[i].lba_count ||
            p->sec[i].len != p->sec[i].lba_count * (uint64_t)p->sector) {
            snprintf(why, n,
                     "the saved way back on this memory stick describes a "
                     "place on this disk that cannot exist, and will not be "
                     "used.");
            return -1;
        }

        /* The two entry-array sections carry the table's own shape in
         * `aux`/`aux2`, and fde_guard() walks the array with them. The
         * bounds are gpt.c's, so that a table this accepts is one the
         * rest of the product would accept. Without the lower bound on
         * the entry size, a forged 1-byte entry size walks 47 bytes
         * past the allocation. */
        if (p->sec[i].kind == RS_GPT_ARR || p->sec[i].kind == RS_GPT_ALT_ARR) {
            if (p->sec[i].aux == 0 || p->sec[i].aux > 4096 ||
                p->sec[i].aux2 < 128 || p->sec[i].aux2 > 4096 ||
                p->sec[i].aux * p->sec[i].aux2 > p->sec[i].len) {
                snprintf(why, n,
                         "the saved partition table on this memory stick "
                         "describes itself wrongly and will not be used.");
                return -1;
            }
        }
    }
    return 0;
}

int rescue_open(const rescue_area *area, rescue_payload *out,
                char *why, size_t n)
{
    int fd = open(area->dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "the AurOS memory stick could not be read");
        return -1;
    }
    uint8_t h[RESCUE_HDR_BYTES];
    int ok = read_at(fd, h, sizeof h, area->part_off) == 0;
    close(fd);
    if (!ok) {
        snprintf(why, n, "the AurOS memory stick could not be read");
        return -1;
    }
    if (hdr_unpack(h, out, why, n) != 0) return -1;
    if (out->payload_bytes > area->part_bytes) {
        snprintf(why, n,
                 "the saved way back claims to be larger than the space it "
                 "is in. It is damaged and will not be used.");
        return -1;
    }
    return 0;
}

/* ── capture ─────────────────────────────────────────────────────── */

/* THE PAYLOAD IS NEVER HELD IN MEMORY.
 *
 * The first version of this built the whole thing in a malloc'd buffer
 * and wrote it at the end. On this product's own target machine -- a
 * ten-year-old laptop with two gigabytes of RAM and an OEM EFI
 * partition of a gigabyte -- that allocation is half the machine, in
 * an initramfs, immediately before the only irreversible step. It
 * streams: a megabyte is read off the disk, hashed twice (once into
 * the section's own digest, once into the digest of the whole body)
 * and handed to the stick, and then that megabyte is forgotten.
 *
 * The header goes down LAST, at offset zero, after everything it
 * describes is already on the stick and flushed. A capture interrupted
 * anywhere before that has no magic at its start and is refused by
 * rescue_open, which is the behaviour wanted: a half-written way back
 * that announced itself would be worse than none. */
typedef struct {
    wr_target        *t;
    const rescue_area *area;
    int               disk_fd;
    uint32_t          sector;
    uint64_t          used;     /* header included                    */
    uint64_t          cap;
    sha256            body;
    rescue_payload    p;
} cap_ctx;

static int add_section(cap_ctx *c, uint32_t kind, uint32_t index,
                       uint64_t lba, uint64_t blocks,
                       uint64_t aux, uint64_t aux2, char *why, size_t n)
{
    static uint8_t buf[1u << 20];
    if (c->p.n_sections >= RESCUE_MAX_SEC) {
        snprintf(why, n,
                 "this computer has more pieces to save than the way back "
                 "can hold. AurOS will not install on it.");
        return -1;
    }
    uint64_t len = blocks * (uint64_t)c->sector;
    if (len == 0 || len > c->cap - c->used) {
        snprintf(why, n,
                 "the space for the way back on the memory stick is too "
                 "small for this computer.");
        return -1;
    }
    rescue_section *s = &c->p.sec[c->p.n_sections];
    s->kind = kind; s->index = index;
    s->disk_lba = lba; s->lba_count = blocks;
    s->off = c->used; s->len = len;
    s->aux = aux; s->aux2 = aux2;

    sha256 own; sha256_start(&own);
    uint64_t at = 0;
    while (at < len) {
        size_t chunk = (size_t)(len - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (read_at(c->disk_fd, buf, chunk,
                    lba * (uint64_t)c->sector + at) != 0) {
            snprintf(why, n,
                     "part of this disk would not be read, so there is no "
                     "trustworthy way back. The disk may be failing; do not "
                     "install anything on it.");
            return -1;
        }
        sha256_feed(&own, buf, chunk);
        sha256_feed(&c->body, buf, chunk);
        if (wr_bytes(c->t, WR_RESCUE, c->area->part_off + c->used + at,
                     buf, chunk, why, n) != 0) return -1;
        at += chunk;
    }
    sha256_done(&own, s->sha);
    c->used += len;
    c->p.n_sections++;
    return 0;
}

int rescue_capture(const stage_disk *d, const rescue_area *area,
                   uint64_t run_id,
                   void (*progress)(int percent), char *why, size_t n)
{
    uint64_t need = 0, esp_lba = 0, esp_blocks = 0;
    if (esp_and_table_bytes(d, &need, &esp_lba, &esp_blocks, why, n) != 0)
        return -1;
    if (need > area->part_bytes) {
        snprintf(why, n,
                 "the space kept on the memory stick for the way back is "
                 "%llu MB, and this computer needs %llu MB. Make the stick "
                 "again on a larger drive.",
                 (unsigned long long)(area->part_bytes / (1024 * 1024)),
                 (unsigned long long)((need + 1024 * 1024 - 1) / (1024 * 1024)));
        return -1;
    }

    char dd[80];
    snprintf(dd, sizeof dd, "/dev/%s", d->name);
    if (strcmp(dd, area->dev) == 0) {
        snprintf(why, n,
                 "the way back would be saved onto the very disk it exists "
                 "to rescue. Refusing: a copy that disappears with the "
                 "thing it is a copy of is not a copy.");
        return -1;
    }

    int fd = open(dd, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(why, n, "the disk could not be read"); return -1; }

    gpt_table t;
    if (gpt_read(fd, (uint32_t)d->logical_sector, d->bytes, &t) != 0) {
        close(fd);
        snprintf(why, n, "the way this disk is divided up could not be read");
        return -1;
    }
    uint32_t sec = t.sector;
    uint64_t arr_bytes = (uint64_t)t.n_entries * t.entry_size;
    uint64_t arr_blocks = (arr_bytes + sec - 1) / sec;

    /* The BACKUP header's array lives wherever the BACKUP header says,
     * which is not derivable from the primary's PartitionEntryLBA and
     * is not always alt_lba - 32. Read it. */
    uint8_t alt[4096];
    if (sec > sizeof alt || read_at(fd, alt, sec, t.alt_lba * sec) != 0) {
        close(fd);
        snprintf(why, n,
                 "the spare copy of this disk's partition table could not be "
                 "read. AurOS will not start an install it cannot undo.");
        return -1;
    }
    if (memcmp(alt, "EFI PART", 8) != 0) {
        close(fd);
        snprintf(why, n,
                 "the spare copy of this disk's partition table is missing or "
                 "damaged. Repair it from Windows before installing AurOS: "
                 "without it there is no complete way back.");
        return -1;
    }
    uint64_t alt_arr_lba = rd64(alt + 72);

    wr_target wt;
    if (wr_open(&wt, area->dev, why, n) != 0) { close(fd); return -1; }
    if (wr_arm(&wt, WR_RESCUE, area->part_off, area->part_off + need,
               why, n) != 0) { wr_close(&wt); close(fd); return -1; }

    cap_ctx c;
    memset(&c, 0, sizeof c);
    c.t = &wt; c.area = area; c.disk_fd = fd; c.sector = sec;
    c.cap = need; c.used = RESCUE_HDR_BYTES;
    sha256_start(&c.body);
    c.p.disk_bytes = d->bytes;
    c.p.sector = sec;
    c.p.run_id = run_id;
    c.p.captured_unix = (uint64_t)time(NULL);
    c.p.esp_lba = esp_lba;
    c.p.esp_blocks = esp_blocks;
    snprintf(c.p.serial, sizeof c.p.serial, "%s", d->serial);
    snprintf(c.p.model,  sizeof c.p.model,  "%s", d->model);

#define ADD(kind, idx, lba, blocks, aux, aux2)                        \
    do { if (add_section(&c, (kind), (idx), (lba), (blocks),          \
                         (aux), (aux2), why, n) != 0) goto fail; } while (0)

    ADD(RS_MBR,         0, 0,            1,          0, 0);
    ADD(RS_GPT_HDR,     0, 1,            1,          0, 0);
    ADD(RS_GPT_ARR,     0, t.entry_lba,  arr_blocks, t.n_entries, t.entry_size);
    ADD(RS_GPT_ALT_HDR, 0, t.alt_lba,    1,          0, 0);
    ADD(RS_GPT_ALT_ARR, 0, alt_arr_lba,  arr_blocks, t.n_entries, t.entry_size);
    if (progress) progress(5);
    ADD(RS_ESP,         0, esp_lba,      esp_blocks, 0, 0);
    if (progress) progress(80);
    /* Halfway-ish through the one long write this makes. A machine
     * that dies here has a stick with no magic at the front of its
     * rescue area, which rescue_open refuses -- and a disk that has
     * not been touched. */
    fault_maybe("capture-mid");

    /* Every other partition, looked at rather than assumed about. */
    for (uint32_t k = 0; k < t.n_entries; k++) {
        if (!gpt_used(&t.ent[k])) continue;
        if (memcmp(t.ent[k].type, GPT_TYPE_ESP, 16) == 0) continue;
        uint64_t first = t.ent[k].first, last = t.ent[k].last;
        uint8_t first_sec[4096];
        if (sec > sizeof first_sec) continue;
        /* A READ THAT FAILS IS NOT "NOT NTFS".
         *
         * This used to `continue`, eleven lines from add_section()
         * where the identical failure is a refusal. A C: whose first
         * sector has a media error -- the ten-year-old laptop this
         * product is for -- was then captured with no record of itself
         * at all, and the restore afterwards said "Windows is back
         * exactly as it was" over a drive left at its shrunken size. */
        if (read_at(fd, first_sec, sec, first * sec) != 0) {
            snprintf(why, n,
                     "the first block of one of this computer's drives would "
                     "not read, so there is no trustworthy way back. The "
                     "disk may be failing; do not install anything on it.");
            goto fail;
        }
        if (ntfs_is_bitlocker(first_sec)) {
            /* Captured deliberately as nothing. A BitLocker volume's
             * geometry is never changed by this product, so there is
             * nothing about it to put back -- and copying 8 KiB of
             * somebody's ciphertext onto a memory stick is a thing to
             * not do. */
            continue;
        }
        if (memcmp(first_sec + 3, "NTFS    ", 8) != 0) continue;

        uint32_t bps = rd16(first_sec + 0x0B);
        uint64_t tot = rd64(first_sec + 0x28);
        if (bps < 512 || bps > 4096 || tot == 0) continue;
        uint64_t boot_blocks = (8192 + sec - 1) / sec;
        uint64_t bak_byte = tot * (uint64_t)bps;
        if (bak_byte % sec != 0) {
            snprintf(why, n,
                     "the Windows drive on this computer is laid out in a way "
                     "AurOS cannot save a complete way back for. It will not "
                     "be changed.");
            goto fail;
        }
        uint64_t bak_lba = first + bak_byte / sec;
        if (bak_lba > last) {
            snprintf(why, n,
                     "the Windows drive claims to be larger than the space it "
                     "is in. This is damage that predates AurOS; run 'chkdsk "
                     "/f' from Windows and try again.");
            goto fail;
        }
        ADD(RS_NTFS_BOOT,    k + 1, first,   boot_blocks, tot, last);
        ADD(RS_NTFS_BAKBOOT, k + 1, bak_lba, 1,           tot, last);
    }
#undef ADD

    c.p.payload_bytes = c.used;
    sha256_done(&c.body, c.p.body_sha);

    /* The header, last, over everything already on the stick. */
    {
        uint8_t h[RESCUE_HDR_BYTES];
        hdr_pack(&c.p, h);
        if (wr_bytes(&wt, WR_RESCUE, area->part_off, h, sizeof h, why, n) != 0)
            goto fail;
    }
    if (wr_flush(&wt) != 0) {
        snprintf(why, n,
                 "the memory stick would not confirm that the way back was "
                 "written to it. Try a different stick.");
        goto fail;
    }
    wr_disarm(&wt, WR_RESCUE);
    wr_close(&wt);
    close(fd); fd = -1;
    if (progress) progress(90);

    /* And read it back off the stick as a stranger would, through the
     * same code the restore will use. A capture checked only against
     * the buffer it came from proves that memory equals memory. */
    {
        rescue_payload back;
        if (rescue_open(area, &back, why, n) != 0) return -1;
        if (rescue_verify(area, &back, NULL, why, n) != 0) return -1;
        if (memcmp(back.body_sha, c.p.body_sha, 32) != 0 ||
            back.payload_bytes != c.p.payload_bytes) {
            snprintf(why, n,
                     "the way back did not come back off the memory stick the "
                     "way it went on. Try a different stick.");
            return -1;
        }
    }
    if (progress) progress(100);
    return 0;

fail:
    wr_close(&wt);
    if (fd >= 0) close(fd);
    return -1;
}

int rescue_verify(const rescue_area *area, const rescue_payload *p,
                  void (*progress)(int percent), char *why, size_t n)
{
    int fd = open(area->dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "the AurOS memory stick could not be read");
        return -1;
    }
    uint8_t buf[1u << 16];
    sha256 whole; sha256_start(&whole);
    uint64_t at = RESCUE_HDR_BYTES;
    while (at < p->payload_bytes) {
        size_t chunk = (size_t)(p->payload_bytes - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (read_at(fd, buf, chunk, area->part_off + at) != 0) {
            close(fd);
            snprintf(why, n,
                     "the way back could not be read off the memory stick.");
            return -1;
        }
        sha256_feed(&whole, buf, chunk);
        at += chunk;
        if (progress) progress((int)(100 * at / p->payload_bytes));
    }
    unsigned char got[32];
    sha256_done(&whole, got);
    if (memcmp(got, p->body_sha, 32) != 0) {
        close(fd);
        snprintf(why, n,
                 "the way back saved on this memory stick is damaged. Do not "
                 "use it to put Windows back; it would make things worse.");
        return -1;
    }
    /* And each section on its own, so that a failure names the piece.
     * The whole-payload hash above cannot: it says the stick is bad,
     * which is true and useless when the ESP is fine and one boot
     * sector is not. */
    for (uint32_t i = 0; i < p->n_sections; i++) {
        sha256 s; sha256_start(&s);
        uint64_t left = p->sec[i].len, off = p->sec[i].off;
        while (left) {
            size_t chunk = (size_t)(left > sizeof buf ? sizeof buf : left);
            if (read_at(fd, buf, chunk, area->part_off + off) != 0) {
                close(fd);
                snprintf(why, n, "the way back could not be read off the "
                                 "memory stick.");
                return -1;
            }
            sha256_feed(&s, buf, chunk);
            off += chunk; left -= chunk;
        }
        unsigned char d2[32];
        sha256_done(&s, d2);
        if (memcmp(d2, p->sec[i].sha, 32) != 0) {
            close(fd);
            snprintf(why, n,
                     "the saved copy of part %u of this computer's startup is "
                     "damaged on the memory stick.", p->sec[i].kind);
            return -1;
        }
    }
    close(fd);
    return 0;
}

int rescue_mirror(wr_target *t, const rescue_area *area,
                  const rescue_payload *p, uint64_t dst_off,
                  char *why, size_t n)
{
    int fd = open(area->dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "the AurOS memory stick could not be read");
        return -1;
    }
    static uint8_t buf[1u << 20];
    uint64_t at = 0;
    while (at < p->payload_bytes) {
        size_t chunk = (size_t)(p->payload_bytes - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (read_at(fd, buf, chunk, area->part_off + at) != 0) {
            close(fd);
            snprintf(why, n, "the way back could not be read off the stick.");
            return -1;
        }
        if (wr_bytes(t, WR_RECOVERY, dst_off + at, buf, chunk, why, n) != 0) {
            close(fd);
            return -1;
        }
        at += chunk;
    }
    if (wr_flush(t) != 0) {
        close(fd);
        snprintf(why, n, "the disk would not confirm the copy of the way back");
        return -1;
    }
    /* AND READ IT BACK, which rescue.h has promised since it was
     * written and this did not do. The copy exists for the person who
     * reused the stick eighteen months ago; one that was never
     * compared is one she finds out about on the day she needs it. */
    at = 0;
    while (at < p->payload_bytes) {
        size_t chunk = (size_t)(p->payload_bytes - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (read_at(fd, buf, chunk, area->part_off + at) != 0) {
            close(fd);
            snprintf(why, n, "the way back could not be read off the stick.");
            return -1;
        }
        uint64_t bad = 0;
        if (wr_check(t, dst_off + at, buf, chunk, &bad) != 0) {
            close(fd);
            snprintf(why, n,
                     "this computer did not keep the copy of the way back it "
                     "was given (at %llu MB into it).",
                     (unsigned long long)(bad / (1024 * 1024)));
            return -1;
        }
        at += chunk;
    }
    close(fd);
    return 0;
}

/* ── putting it back ─────────────────────────────────────────────── */

static const rescue_section *find_sec(const rescue_payload *p,
                                      uint32_t kind, uint32_t index)
{
    for (uint32_t i = 0; i < p->n_sections; i++)
        if (p->sec[i].kind == kind &&
            (index == 0 || p->sec[i].index == index))
            return &p->sec[i];
    return NULL;
}

/* Pull one section's bytes off the stick. Small sections only -- the
 * ESP is streamed, never held. */
static int sec_bytes(int stick, const rescue_area *area,
                     const rescue_section *s, void *buf, size_t want)
{
    if (s->len != want) return -1;
    return read_at(stick, buf, want, area->part_off + s->off);
}

static void part_node(const char *disk_dev, int idx1, char *out, size_t n)
{
    size_t l = strlen(disk_dev);
    int digit = l && disk_dev[l - 1] >= '0' && disk_dev[l - 1] <= '9';
    snprintf(out, n, "%s%s%d", disk_dev, digit ? "p" : "", idx1);
}

static void talk(rescue_say say, void *ud, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (say) say(line, ud);
}

/* THE MUST-NOT, at the one site most likely to break it, because "put
 * it back" feels safe. Any partition entry the restore would move,
 * resize or delete is checked against the bytes on the disk RIGHT NOW,
 * not against anything recorded: a recorded flag can be stale and the
 * disk cannot. */
static int fde_guard(int disk_fd, uint32_t sector,
                     const gpt_table *live, int live_ok,
                     const uint8_t *cap_arr, uint32_t n_ent, uint32_t ent_sz,
                     char *why, size_t n)
{
    uint8_t first[4096];
    if (sector > sizeof first) {
        snprintf(why, n, "this disk uses blocks AurOS cannot read");
        return -1;
    }

    /* EVERY PARTITION THE RESTORE TOUCHES, NOT ONLY THE ONES IT MOVES.
     *
     * The first version of this checked only entries whose extent
     * differs between the live table and the captured one, on the
     * premise that those are the ones whose geometry changes. The
     * premise is wrong, and a review found the hole before it shipped:
     * the restore also writes $Boot into volumes whose entries it
     * leaves exactly where they are, and runs ntfsresize inside them.
     * A machine where the user turned on BitLocker for D: after the
     * install -- which Windows 11 does by itself on first sign-in --
     * has a D: whose extent is unchanged and whose first sector must
     * never be written. So every used entry in both tables is read.
     *
     * AND A READ THAT FAILS IS A REFUSAL. It used to `continue`, which
     * is the permissive answer, on the one check whose whole point is
     * that the disk cannot lie the way a recorded flag can. A sector
     * that will not read is a sector we do not know the contents of. */
    uint64_t seen[GPT_MAX_ENT * 2];
    int n_seen = 0;
    /* A table with more entries than this list can hold would have
     * some of its partitions checked and some not, which is the shape
     * of a guard that looks like one. gpt.c refuses a table above
     * GPT_MAX_ENT for the same reason; the captured one is held to the
     * same bound rather than to the specification's 4096. */
    if (n_ent > GPT_MAX_ENT) {
        snprintf(why, n,
                 "the saved partition table has more parts in it than AurOS "
                 "can check. It will not be used.");
        return -1;
    }
    /* Direction 1: something on the disk now whose entry the captured
     * table does not reproduce exactly. Its geometry is about to
     * change, so it must not be encrypted. */
    if (live_ok) {
        for (uint32_t k = 0; k < live->n_entries; k++) {
            if (!gpt_used(&live->ent[k])) continue;
            if (n_seen >= (int)(sizeof seen / sizeof seen[0])) {
                snprintf(why, n,
                         "this disk is divided into more parts than AurOS can "
                         "check for encryption. Nothing has been written.");
                return -1;
            }
            seen[n_seen++] = live->ent[k].first;
        }
    }
    for (uint32_t j = 0; j < n_ent; j++) {
        const uint8_t *e = cap_arr + (uint64_t)j * ent_sz;
        int used = 0;
        for (int q = 0; q < 16; q++) if (e[q]) { used = 1; break; }
        if (!used) continue;
        uint64_t f = rd64(e + 32);
        int have = 0;
        for (int q = 0; q < n_seen; q++) if (seen[q] == f) { have = 1; break; }
        if (have) continue;
        if (n_seen >= (int)(sizeof seen / sizeof seen[0])) {
            snprintf(why, n,
                     "this disk is divided into more parts than AurOS can "
                     "check for encryption. Nothing has been written.");
            return -1;
        }
        seen[n_seen++] = f;
    }

    for (int i = 0; i < n_seen; i++) {
        uint64_t f = seen[i];
        if (read_at(disk_fd, first, sector, f * (uint64_t)sector) != 0) {
            snprintf(why, n,
                     "REFUSING: block %llu of this disk would not read, so "
                     "AurOS cannot tell whether there is an encrypted drive "
                     "there. The disk may be failing. Nothing has been "
                     "written to it.", (unsigned long long)f);
            return -1;
        }
        if (ntfs_is_bitlocker(first)) {
            snprintf(why, n,
                     "REFUSING: the part of this disk starting at block %llu "
                     "is BitLocker-encrypted, and putting the saved layout "
                     "back would write to it. That destroys the encrypted "
                     "drive completely and nothing brings it back -- not "
                     "chkdsk, not a recovery key. Unlock and turn off "
                     "BitLocker from Windows first.", (unsigned long long)f);
            return -1;
        }
    }
    return 0;
}

/* READ IT BACK. R5 asks for it on every written block, commit.c and
 * image.c do it, and the restore did not do it once -- not even for
 * LBA 1, the sector that decides whether the machine boots. A drive
 * that accepts a write and drops it then produced "the original layout
 * is back" and a computer that starts nothing.
 *
 * wr_check() counts what it compares, so wr_verified() is what a test
 * asserts on; that is the reason this goes through it rather than
 * doing its own pread. */
static int checked(wr_target *t, uint64_t off, const void *expect, size_t len,
                   const char *what, char *why, size_t n)
{
    uint64_t bad = 0;
    if (wr_check(t, off, expect, len, &bad) == 0) return 0;
    snprintf(why, n,
             "this disk did not give back what was written to it at %llu MB "
             "while putting %s back. The disk is failing. Do not restart the "
             "computer.", (unsigned long long)(bad / (1024 * 1024)), what);
    return -1;
}

/* The head of a volume was overwritten, so the captured $Boot and the
 * captured spare boot sector are the only copies of it left.
 *
 * THIS IS THE MOST DANGEROUS WRITE IN THE RESTORE and the first
 * version of it had no guard at all. A review found the path: a user
 * who turns BitLocker on for D: after the install, or who reformats
 * D: as exFAT, has a volume whose first sector is no longer NTFS --
 * which is exactly the condition that sends the caller here. It would
 * then have stamped a stale NTFS boot sector over the FVE header that
 * points at the encryption keys, and told her to run chkdsk.
 *
 * So identity is PROVED before anything is written, and the proof does
 * not come from the sector that is missing:
 *
 *   - the sector at the recorded backup-boot LBA must still be exactly
 *     the bytes the capture recorded there. NTFS keeps that copy in
 *     the volume's last sector; nothing but this volume puts those
 *     bytes at that offset, and a reformat or an encryption pass
 *     changes it. If it matches, this is our volume and the head is
 *     what was lost.
 *   - and the live head must not be a filesystem anybody recognises.
 *     Something unreadable is damage. Something that IS a filesystem
 *     is somebody's data, whatever the capture says used to be there.
 *
 * If neither can be established, nothing is written and the person is
 * told what is actually true, which is that AurOS cannot tell what is
 * on that part of the disk any more. */
static int head_is_a_filesystem(const uint8_t *b, uint32_t sector)
{
    if (ntfs_is_bitlocker(b)) return 1;
    static const char *oem[] = { "NTFS    ", "MSDOS5.0", "MSWIN4.1", "EXFAT   ",
                                 "FAT32   ", "-FVE-FS-", NULL };
    for (int i = 0; oem[i]; i++)
        if (memcmp(b + 3, oem[i], 8) == 0) return 1;
    /* ext2/3/4: the superblock magic 0xEF53 at byte 1080. */
    if (sector > 1080 + 1 && b[1080] == 0x53 && b[1081] == 0xEF) return 1;
    /* An MBR or a boot sector of some kind. */
    if (sector >= 512 && b[510] == 0x55 && b[511] == 0xAA) return 1;
    return 0;
}

static int restore_boot_sectors(const char *disk_dev, const rescue_area *area,
                                const rescue_payload *p, uint32_t idx,
                                uint32_t sec, int stick, char *why, size_t n)
{
    const rescue_section *b1 = find_sec(p, RS_NTFS_BOOT, idx);
    const rescue_section *b2 = find_sec(p, RS_NTFS_BAKBOOT, idx);
    if (!b1 || !b2) {
        snprintf(why, n,
                 "the saved copy has no record of the first blocks of drive "
                 "%u, so they cannot be put back.", idx);
        return -1;
    }
    static uint8_t buf[8192];

    /* ── the proof ─────────────────────────────────────────────── */
    {
        int dfd = open(disk_dev, O_RDONLY | O_CLOEXEC);
        if (dfd < 0) {
            snprintf(why, n, "the disk in this computer could not be read");
            return -1;
        }
        uint8_t live_bak[4096], cap_bak[4096], live_head[4096];
        int ok = sec <= sizeof live_bak &&
                 b2->len == sec &&
                 read_at(dfd, live_bak, sec, b2->disk_lba * sec) == 0 &&
                 read_at(dfd, live_head, sec, b1->disk_lba * sec) == 0 &&
                 read_at(stick, cap_bak, sec, area->part_off + b2->off) == 0;
        close(dfd);
        if (!ok) {
            snprintf(why, n,
                     "drive %u could not be read, so AurOS cannot tell "
                     "whether the saved copy of its first blocks belongs to "
                     "it. Nothing has been written to it.", idx);
            return -1;
        }
        if (memcmp(live_bak, cap_bak, sec) != 0) {
            snprintf(why, n,
                     "REFUSING: what is on drive %u now is not the drive the "
                     "saved copy was made from -- it has been reformatted, "
                     "encrypted or replaced since. Putting the saved first "
                     "blocks back would destroy whatever is on it now. "
                     "Nothing has been written to it.", idx);
            return -1;
        }
        if (head_is_a_filesystem(live_head, sec)) {
            snprintf(why, n,
                     "REFUSING: drive %u already holds a filesystem AurOS did "
                     "not put there. Nothing has been written to it.", idx);
            return -1;
        }
    }
    wr_target t;
    if (wr_open(&t, disk_dev, why, n) != 0) return -1;
    const rescue_section *two[2] = { b1, b2 };
    for (int i = 0; i < 2; i++) {
        const rescue_section *s = two[i];
        if (s->len > sizeof buf ||
            read_at(stick, buf, (size_t)s->len, area->part_off + s->off) != 0) {
            snprintf(why, n, "the saved copy could not be read off the stick.");
            wr_close(&t); return -1;
        }
        if (wr_arm(&t, WR_RESTORE, s->disk_lba * sec,
                   s->disk_lba * sec + s->len, why, n) != 0 ||
            wr_bytes(&t, WR_RESTORE, s->disk_lba * sec, buf,
                     (size_t)s->len, why, n) != 0 ||
            wr_flush(&t) != 0 ||
            checked(&t, s->disk_lba * sec, buf, (size_t)s->len,
                    "a drive's first blocks", why, n) != 0)
            { wr_close(&t); return -1; }
        wr_disarm(&t, WR_RESTORE);
    }
    wr_close(&t);
    (void)sec;
    return 0;
}

int rescue_restore(const char *disk_dev, const rescue_area *area,
                   rescue_say say, void *ud, rescue_outcome *out,
                   char *why, size_t n)
{
    memset(out, 0, sizeof *out);

    rescue_payload p;
    if (rescue_open(area, &p, why, n) != 0) return -1;
    talk(say, ud, "checking the saved copy of this computer's startup");
    if (rescue_verify(area, &p, NULL, why, n) != 0) return -1;
    talk(say, ud, "the saved copy is complete and undamaged");

    int stick = open(area->dev, O_RDONLY | O_CLOEXEC);
    if (stick < 0) {
        snprintf(why, n, "the AurOS memory stick could not be read");
        return -1;
    }
    int disk_fd = open(disk_dev, O_RDONLY | O_CLOEXEC);
    if (disk_fd < 0) {
        close(stick);
        snprintf(why, n, "the disk in this computer could not be read");
        return -1;
    }

    /* ── is this even the right disk ───────────────────────────────
     *
     * R11 in the direction nobody plans for. By the time somebody runs
     * this they are frightened and copying commands out of a forum,
     * and writing a saved partition table onto the wrong disk puts the
     * spare copy in the wrong place and describes partitions that do
     * not exist. */
    uint64_t live_bytes = 0;
    if (ioctl(disk_fd, _IOR(0x12, 114, size_t), &live_bytes) != 0 ||
        live_bytes == 0) {
        /* THE FAILURE OF THIS CHECK IS NOT A PASS. It used to set
         * live_bytes to 0 and then skip the comparison, so a disk that
         * would not state its size was one the captured partition
         * table could be written onto unconditionally. wr.c refuses a
         * device it cannot size; so does this. */
        close(stick); close(disk_fd);
        snprintf(why, n,
                 "this computer will not say how large its disk is, and "
                 "AurOS will not write a saved partition table onto a disk "
                 "it cannot identify. Nothing has been changed.");
        return -1;
    }
    if (live_bytes != p.disk_bytes) {
        close(stick); close(disk_fd);
        snprintf(why, n,
                 "REFUSING: what was saved came from a %llu MB disk, and this "
                 "one is %llu MB. This is not the disk the copy was made "
                 "from, and putting it here would destroy what is on it.",
                 (unsigned long long)(p.disk_bytes / (1024 * 1024)),
                 (unsigned long long)(live_bytes / (1024 * 1024)));
        return -1;
    }

    uint32_t sec = p.sector;
    const rescue_section *s_mbr = find_sec(&p, RS_MBR, 0);
    const rescue_section *s_hdr = find_sec(&p, RS_GPT_HDR, 0);
    const rescue_section *s_arr = find_sec(&p, RS_GPT_ARR, 0);
    const rescue_section *s_ahd = find_sec(&p, RS_GPT_ALT_HDR, 0);
    const rescue_section *s_aar = find_sec(&p, RS_GPT_ALT_ARR, 0);
    const rescue_section *s_esp = find_sec(&p, RS_ESP, 0);
    if (!s_mbr || !s_hdr || !s_arr || !s_ahd || !s_aar || !s_esp) {
        close(stick); close(disk_fd);
        snprintf(why, n,
                 "the saved copy of this computer's startup is missing a "
                 "piece it cannot be put back without.");
        return -1;
    }

    /* The captured entry array, in memory. It is at most 16 MiB by the
     * bounds gpt.c enforces, and everything below needs to look at it
     * more than once. */
    uint8_t *cap_arr  = malloc((size_t)s_arr->len);
    uint8_t *cap_aarr = malloc((size_t)s_aar->len);
    uint8_t cap_hdr[4096], cap_ahd[4096], cap_mbr[4096];
    if (!cap_arr || !cap_aarr || sec > sizeof cap_hdr ||
        sec_bytes(stick, area, s_hdr, cap_hdr, sec) != 0 ||
        sec_bytes(stick, area, s_ahd, cap_ahd, sec) != 0 ||
        sec_bytes(stick, area, s_mbr, cap_mbr, sec) != 0 ||
        read_at(stick, cap_arr, (size_t)s_arr->len,
                area->part_off + s_arr->off) != 0) {
        free(cap_arr); free(cap_aarr); close(stick); close(disk_fd);
        snprintf(why, n, "the saved copy could not be read off the stick.");
        return -1;
    }
    uint32_t n_ent  = (uint32_t)s_arr->aux;
    uint32_t ent_sz = (uint32_t)s_arr->aux2;
    if (!n_ent || !ent_sz || (uint64_t)n_ent * ent_sz > s_arr->len) {
        free(cap_arr); free(cap_aarr); close(stick); close(disk_fd);
        snprintf(why, n, "the saved partition table describes itself wrongly.");
        return -1;
    }

    /* What is on the disk right now, read softly: the machine this runs
     * on most often is one whose table has already gone. */
    gpt_table live;
    int live_ok = gpt_read(disk_fd, sec, p.disk_bytes, &live) == 0;
    if (!live_ok)
        talk(say, ud, "this disk has no readable layout at the moment");

    /* AND THE DISK'S OWN IDENTIFIER, when there is still a table to
     * read it from. Two identical drives in one desktop -- the Windows
     * one and the one full of photographs -- are the same size, so
     * size alone is not identity, and the DiskGUID at offset 56 of the
     * header is the only thing on a disk that is meant to be unique.
     * Stage C never changes it: commit.c copies the old table and edits
     * entries, so the disk AurOS installed onto still carries the GUID
     * the capture recorded. */
    if (live_ok && memcmp(live.disk_guid, cap_hdr + 56, 16) != 0) {
        free(cap_arr); free(cap_aarr); close(stick); close(disk_fd);
        snprintf(why, n,
                 "REFUSING: this disk is not the one the saved copy was made "
                 "from. It is the same size, and it is a different disk. "
                 "Nothing has been written to it.");
        return -1;
    }

    if (fde_guard(disk_fd, sec, &live, live_ok, cap_arr, n_ent, ent_sz,
                  why, n) != 0) {
        free(cap_arr); free(cap_aarr); close(stick); close(disk_fd);
        return -1;
    }

    /* ── EVERY VOLUME, MEASURED BEFORE THE FIRST DESTRUCTIVE BYTE ──
     *
     * The "this drive is larger than it was" refusal used to live in
     * the grow loop, which runs AFTER the table has been committed and
     * the ESP rewritten -- so a user who had extended D: in Windows
     * since the install got her partition entry truncated below its own
     * filesystem, an unmountable drive, and the sentence "Nothing has
     * been written to it."
     *
     * Everything that decision needs is available here, with the disk
     * untouched: the captured size is in the section, and the live size
     * is in the volume's own boot sector, which is read by offset
     * because the partition nodes still describe the AurOS layout. */
    for (uint32_t i = 0; i < p.n_sections; i++) {
        if (p.sec[i].kind != RS_NTFS_BOOT) continue;
        uint8_t head[4096];
        if (sec > sizeof head ||
            read_at(disk_fd, head, sec, p.sec[i].disk_lba * sec) != 0)
            continue;              /* unreadable or gone: the loop below
                                    * decides, with its own guards */
        if (memcmp(head + 3, "NTFS    ", 8) != 0) continue;
        uint32_t bps = rd16(head + 0x0B);
        uint64_t now = rd64(head + 0x28);
        if (bps < 512 || bps > 4096) continue;
        if (now > p.sec[i].aux) {
            free(cap_arr); free(cap_aarr); close(stick); close(disk_fd);
            snprintf(why, n,
                     "REFUSING: drive %u is larger than it was before AurOS "
                     "was installed -- somebody has made it bigger since. "
                     "Putting the saved layout back would leave part of that "
                     "drive outside its own partition, and Windows would not "
                     "open it. Nothing has been written to this disk.",
                     p.sec[i].index);
            return -1;
        }
    }

    /* Does the protective record at the very front need putting back?
     * Stage C never writes it, so on an install we undo it will match
     * and nothing is written. It is checked rather than assumed
     * because a machine that boots nothing at all is usually missing
     * exactly this sector, and that machine is the one holding this
     * tool. */
    uint8_t live_mbr[4096];
    int mbr_differs = 0;
    if (read_at(disk_fd, live_mbr, sec, 0) == 0)
        mbr_differs = memcmp(live_mbr, cap_mbr, sec) != 0;
    close(disk_fd);

    uint64_t hdr_lba  = s_hdr->disk_lba;      /* 1                     */
    uint64_t arr_lba  = s_arr->disk_lba;
    uint64_t alt_lba  = s_ahd->disk_lba;
    uint64_t aarr_lba = s_aar->disk_lba;
    uint64_t prim_hi  = arr_lba * sec + s_arr->len;
    if ((hdr_lba + 1) * sec > prim_hi) prim_hi = (hdr_lba + 1) * sec;
    uint64_t back_lo  = aarr_lba * sec;
    uint64_t back_hi  = aarr_lba * sec + s_aar->len;
    if (alt_lba * sec < back_lo) back_lo = alt_lba * sec;
    if ((alt_lba + 1) * sec > back_hi) back_hi = (alt_lba + 1) * sec;

    wr_target t;
    if (wr_open(&t, disk_dev, why, n) != 0) {
        free(cap_arr); free(cap_aarr); close(stick); return -1;
    }
    /* The primary window starts at byte 0 here and at LBA 1 in the
     * install. Different purpose: the install has no business touching
     * the protective record, and this does. */
    if (wr_arm(&t, WR_GPT_PRIMARY, 0, prim_hi, why, n) != 0 ||
        wr_arm(&t, WR_GPT_BACKUP, back_lo, back_hi, why, n) != 0) {
        wr_close(&t); free(cap_arr); free(cap_aarr); close(stick); return -1;
    }

    talk(say, ud, "putting this computer's original layout back");
    if (mbr_differs) {
        if (wr_bytes(&t, WR_GPT_PRIMARY, 0, cap_mbr, sec, why, n) != 0 ||
            wr_flush(&t) != 0) goto table_failed;
        out->mbr_restored = 1;
        talk(say, ud, "the record at the very front of the disk was "
                      "missing or wrong, and has been put back");
    }
    /* Step 1: the primary array. While this is down and LBA 1 is not,
     * the primary header's checksum no longer matches, so every reader
     * on earth falls back to the spare copy -- which still describes
     * the disk exactly as it is this second. Nothing changes yet. */
    if (wr_bytes(&t, WR_GPT_PRIMARY, arr_lba * sec, cap_arr,
                 (size_t)s_arr->len, why, n) != 0 ||
        wr_flush(&t) != 0 ||
        checked(&t, arr_lba * sec, cap_arr, (size_t)s_arr->len,
                "the partition table", why, n) != 0) goto table_failed;
    fault_maybe("restore-array");
    /* Step 2: LBA 1. One sector. This is the instant the machine's
     * layout becomes the one it had before AurOS. */
    if (wr_bytes(&t, WR_GPT_PRIMARY, hdr_lba * sec, cap_hdr, sec,
                 why, n) != 0 ||
        wr_flush(&t) != 0 ||
        checked(&t, hdr_lba * sec, cap_hdr, sec,
                "the partition table", why, n) != 0) goto table_failed;
    fault_maybe("restore-sector");
    /* Step 3: the spare copy catches up. */
    /* FROM ITS OWN SECTION, not from the primary's buffer. The two
     * arrays are the same bytes on every table this product has ever
     * made, and writing the primary's with the BACKUP's length was a
     * heap over-read waiting for the day they were not -- and it made
     * the captured backup array, sixteen kilobytes this spends time
     * reading, hashing and checking, dead weight. */
    if (read_at(stick, cap_aarr, (size_t)s_aar->len,
                area->part_off + s_aar->off) != 0) {
        snprintf(why, n, "the saved copy could not be read off the stick.");
        goto table_failed;
    }
    if (wr_bytes(&t, WR_GPT_BACKUP, aarr_lba * sec, cap_aarr,
                 (size_t)s_aar->len, why, n) != 0 ||
        wr_bytes(&t, WR_GPT_BACKUP, alt_lba * sec, cap_ahd, sec,
                 why, n) != 0 ||
        wr_flush(&t) != 0 ||
        checked(&t, aarr_lba * sec, cap_aarr, (size_t)s_aar->len,
                "the spare partition table", why, n) != 0 ||
        checked(&t, alt_lba * sec, cap_ahd, sec,
                "the spare partition table", why, n) != 0) goto table_failed;
    fault_maybe("restore-backup");
    wr_disarm(&t, WR_GPT_PRIMARY);
    wr_disarm(&t, WR_GPT_BACKUP);
    out->table_restored = 1;
    talk(say, ud, "the original layout is back");

    /* Make the kernel look again, so the partition nodes the grow
     * needs describe the partitions that now exist. */
    {
        int f = open(disk_dev, O_RDONLY | O_CLOEXEC);
        if (f >= 0) { ioctl(f, _IO(0x12, 95)); close(f); }
    }

    /* ── the EFI partition, whole, streamed off the stick ─────────── */
    talk(say, ud, "putting the Windows startup files back");
    if (wr_arm(&t, WR_RESTORE, s_esp->disk_lba * sec,
               s_esp->disk_lba * sec + s_esp->len, why, n) != 0)
        goto esp_failed;
    {
        static uint8_t buf[1u << 20];
        uint64_t at = 0;
        while (at < s_esp->len) {
            size_t chunk = (size_t)(s_esp->len - at);
            if (chunk > sizeof buf) chunk = sizeof buf;
            if (read_at(stick, buf, chunk, area->part_off + s_esp->off + at)
                    != 0) {
                snprintf(why, n, "the saved startup files could not be read "
                                 "off the memory stick.");
                goto esp_failed;
            }
            if (wr_bytes(&t, WR_RESTORE, s_esp->disk_lba * sec + at,
                         buf, chunk, why, n) != 0 ||
                checked(&t, s_esp->disk_lba * sec + at, buf, chunk,
                        "the Windows startup files", why, n) != 0)
                goto esp_failed;
            at += chunk;
            if (at * 2 >= s_esp->len) fault_maybe("restore-esp-mid");
        }
    }
    if (wr_flush(&t) != 0) {
        snprintf(why, n, "the disk would not confirm the startup files");
        goto esp_failed;
    }
    wr_disarm(&t, WR_RESTORE);
    out->esp_restored = 1;
    talk(say, ud, "the Windows startup files are back");
    wr_close(&t);

    /* ── and the filesystems ──────────────────────────────────────── */
    for (uint32_t i = 0; i < p.n_sections; i++) {
        if (p.sec[i].kind != RS_NTFS_BOOT) continue;
        const rescue_section *sb = &p.sec[i];
        uint32_t idx = sb->index;
        uint64_t want_sectors = sb->aux;

        uint8_t cap_boot[8192];
        if (sb->len > sizeof cap_boot ||
            read_at(stick, cap_boot, (size_t)sb->len,
                    area->part_off + sb->off) != 0) continue;
        uint32_t bps = rd16(cap_boot + 0x0B);
        if (bps < 512 || bps > 4096) continue;

        char dev[80];
        part_node(disk_dev, (int)idx, dev, sizeof dev);
        for (int w = 0; w < 100 && access(dev, F_OK) != 0; w++) {
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (access(dev, F_OK) != 0) {
            talk(say, ud, "this computer has not noticed drive %u yet; "
                          "restart it and run this again", idx);
            out->volumes_left_small++;
            continue;
        }

        uint64_t now_bytes = 0;
        if (ntfs_volume_bytes(dev, &now_bytes) != 0) {
            /* The head of the volume is not NTFS any more. The captured
             * $Boot and its spare are the only copies left. They are
             * the right answer when something wrote over the start of
             * the volume, and the wrong one if a shrink had already
             * finished when that happened -- and nothing on the disk
             * can tell those apart. So it is said plainly rather than
             * decided quietly. */
            talk(say, ud, "drive %u no longer starts with a Windows "
                          "filesystem; putting the saved copy of its first "
                          "blocks back", idx);
            if (restore_boot_sectors(disk_dev, area, &p, idx, sec, stick,
                                     why, n) != 0) {
                free(cap_arr); free(cap_aarr); close(stick);
                return -1;
            }
            talk(say, ud, "if Windows does not start, run 'chkdsk /f' from "
                          "Windows installation media");
            out->volumes_left_small++;
            continue;
        }
        /* PLUS ONE, AND IT IS NOT A ROUNDING.
         *
         * $Boot's total_sectors counts the volume EXCLUDING the backup
         * boot sector at the end; ntfs_volume_bytes() and ntfsresize
         * both work in the other convention, which includes it. Mixing
         * the two makes a perfectly restored volume look one sector
         * short of its target for ever: the first run of this reported
         * "grew but is still short" on a volume it had just put back
         * exactly, and left every restore saying Windows had less
         * space than it did. */
        uint64_t want_bytes = (want_sectors + 1) * (uint64_t)bps;
        if (now_bytes > want_bytes) {
            free(cap_arr); free(cap_aarr); close(stick);
            snprintf(why, n,
                     "REFUSING: drive %u is larger than it was before AurOS "
                     "was installed. Making it smaller could delete somebody's "
                     "files, and this tool never makes a drive smaller. "
                     "Nothing has been written to it.", idx);
            return -1;
        }
        if (now_bytes == want_bytes) {
            talk(say, ud, "drive %u is already its original size", idx);
            out->volumes_grown++;
            continue;
        }

        /* WHETHER THIS VOLUME MAY BE GROWN, decided here rather than
         * by the tool, because the tool's answer is both too coarse
         * and, on the one volume this always meets, wrong.
         *
         * ntfsresize marks a volume dirty after every successful
         * resize, so that Windows checks it at the next start -- and
         * then refuses to touch a dirty volume without --force. So the
         * volume our own installer shrank is by construction one that
         * cannot be grown back unless somebody decides the dirty bit
         * is ours. Nothing but this can decide that, because only this
         * has the capture.
         *
         * The conditions, all of them, all checked:
         *   - the only thing wrong is the dirty bit;
         *   - the log is clean and there is no hibernated session, and
         *     neither is merely UNSURE -- an unreadable answer is a
         *     refusal, never a pass;
         *   - the volume's NTFS serial number is the one recorded in
         *     the capture, so this is the same filesystem and not one
         *     somebody has reformatted in the meantime;
         *   - and it is smaller than the capture recorded, which is
         *     what a shrink leaves and nothing else does.
         *
         * Anything else, including a dirty bit on a volume whose serial
         * has changed, gets the refusal and the chkdsk sentence. */
        ntfs_state ns;
        ntfs_read_state(dev, &ns);
        uint64_t cap_serial = rd64(cap_boot + 0x48);
        int only_dirty = ns.verdict == NTFS_DIRTY &&
                         ns.dirty == NTFS_YES &&
                         ns.log_dirty == NTFS_NO &&
                         ns.hibernated == NTFS_NO &&
                         ns.serial == cap_serial;
        if (ns.verdict != NTFS_OK && !only_dirty) {
            talk(say, ud, "drive %u cannot be made its full size again yet: "
                          "%s", idx, ns.why);
            talk(say, ud, "%s", ns.remedy);
            talk(say, ud, "Windows will start, with less space on it than it "
                          "had. Fix the above and run this again to finish.");
            out->volumes_left_small++;
            continue;
        }

        talk(say, ud, "making drive %u its full size again", idx);
        fault_maybe("restore-grow");
        shrink_result sr;
        resize_do(dev, want_bytes, only_dirty, NULL, &sr);
        uint64_t after = 0;
        if (ntfs_volume_bytes(dev, &after) != 0) after = 0;
        if (after == want_bytes) {
            out->volumes_grown++;
            talk(say, ud, "drive %u is its full size again", idx);
        } else if (after > now_bytes) {
            /* NTFS ALLOCATES IN CLUSTERS AND GIVES ONE BACK, so an
             * exact match is not reachable and never was. Measured:
             * ntfsresize sets total_sectors to
             * (floor(size / cluster) - 1) * sectors_per_cluster, and
             * mkntfs sets it to one less than the partition -- which
             * is not a multiple of the cluster size, so the original
             * number cannot be reproduced by any --size at all. The
             * volume comes back up to one cluster short of where it
             * was, Windows shows C: at its full size, and treating
             * that as a failure would report every correct restore as
             * a broken one. */
            /* after > want_bytes is not excluded by the branch above,
             * and an unsigned subtraction in the wrong direction told
             * one tester her drive was seventeen million terabytes
             * short. It is on the screen of somebody watching a
             * restore, so it is worth the two lines. */
            uint64_t shortfall = after < want_bytes ? want_bytes - after : 0;
            uint64_t cluster = ns.bytes_per_cluster ? ns.bytes_per_cluster
                                                    : 65536;
            if (shortfall <= cluster) {
                out->volumes_grown++;
                talk(say, ud, "drive %u is back to within a hair of its "
                              "original size", idx);
            } else {
                out->volumes_left_small++;
                talk(say, ud, "drive %u grew but is still %llu MB short. "
                              "Windows will start; run this again to finish.",
                     idx, (unsigned long long)(shortfall / (1024 * 1024)));
            }
        } else {
            out->volumes_left_small++;
            talk(say, ud, "drive %u would not grow: %s", idx,
                 sr.why[0] ? sr.why : "the resizing tool refused");
            talk(say, ud, "Windows will start, with less space on it. Run "
                          "'chkdsk /f' from Windows and try this again.");
        }
    }

    free(cap_arr); free(cap_aarr);
    close(stick);
    if (out->volumes_left_small)
        talk(say, ud, "Windows should start now. One or more drives are "
                      "smaller than they were; running this again after "
                      "Windows has started will finish the job.");
    else
        talk(say, ud, "Windows is back exactly as it was. Restart the "
                      "computer.");
    return 0;

table_failed:
    wr_close(&t); free(cap_arr); free(cap_aarr); close(stick);
    if (out->table_restored) return -1;
    /* Nothing landed, or only the array did -- and while only the array
     * is down, readers use the spare copy, which still describes the
     * disk as it is. Saying so is the difference between a person who
     * restarts and one who does not. */
    {
        char had[300];
        snprintf(had, sizeof had, "%s", why);
        snprintf(why, n,
                 "%s The disk was not changed in a way that stops it "
                 "starting; restart the computer and try again.", had);
    }
    return -1;

esp_failed:
    wr_close(&t); free(cap_arr); free(cap_aarr); close(stick);
    return -1;
}
