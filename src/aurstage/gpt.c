/* gpt.c — see gpt.h. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gpt.h"

/* ── little-endian, because the disk is ──────────────────────────── */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p)
{ return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p+4, (uint32_t)(v>>32)); }

/* ── CRC-32, IEEE, the one the specification names ───────────────── */
uint32_t gpt_crc32(const void *data, size_t n)
{
    static uint32_t tab[256];
    static int built;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        built = 1;
    }
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── the type GUIDs, in the order they sit on the disk ───────────── */
const uint8_t GPT_TYPE_ESP[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };            /* C12A7328-… */
const uint8_t GPT_TYPE_MSDATA[16] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
    0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7 };            /* EBD0A0A2-… */
const uint8_t GPT_TYPE_LINUX_ROOT[16] = {
    0xE3,0xBC,0x68,0x4F, 0xCD,0xE8, 0xB1,0x4D,
    0x96,0xE7, 0xFB,0xCA,0xF9,0x84,0xB7,0x09 };            /* 4F68BCE3-… */
const uint8_t GPT_TYPE_LINUX[16] = {
    0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
    0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4 };            /* 0FC63DAF-… */

int gpt_used(const gpt_entry *e)
{
    for (int i = 0; i < 16; i++) if (e->type[i]) return 1;
    return 0;
}

/* ── reading ─────────────────────────────────────────────────────── */

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

/* Parse one header block. 0 on success. */
static int parse_header(const uint8_t *h, uint32_t sector, gpt_table *t)
{
    if (memcmp(h, "EFI PART", 8) != 0) return -1;

    uint32_t hsize = rd32(h + 12);
    /* AS FOUND, and bounded: a header claiming to be longer than the
     * block it lives in would CRC over bytes nobody read. */
    if (hsize < 92 || hsize > sector) return -1;

    /* The CRC covers HeaderSize bytes with its own field zeroed. */
    uint8_t tmp[4096];
    if (hsize > sizeof tmp) return -1;
    memcpy(tmp, h, hsize);
    wr32(tmp + 16, 0);
    if (gpt_crc32(tmp, hsize) != rd32(h + 16)) return -1;

    t->header_size  = hsize;
    t->revision     = rd32(h + 8);
    t->my_lba       = rd64(h + 24);
    t->alt_lba      = rd64(h + 32);
    t->first_usable = rd64(h + 40);
    t->last_usable  = rd64(h + 48);
    memcpy(t->disk_guid, h + 56, 16);
    t->entry_lba    = rd64(h + 72);
    t->n_entries    = rd32(h + 80);
    t->entry_size   = rd32(h + 84);

    if (t->entry_size < 128 || t->entry_size > 4096 ||
        t->entry_size % 8 || t->n_entries == 0 ||
        t->n_entries > GPT_MAX_ENT)
        return -1;
    if (t->entry_lba < 2 && t->my_lba == 1) return -1;
    return 0;
}

static int parse_entries(int fd, const uint8_t *h, gpt_table *t)
{
    size_t bytes = (size_t)t->n_entries * t->entry_size;
    if (bytes > (size_t)GPT_MAX_ENT * 4096) return -1;
    uint8_t *arr = malloc(bytes);
    if (!arr) return -1;
    int rc = -1;
    if (read_at(fd, arr, bytes, t->entry_lba * t->sector) != 0) goto out;
    if (gpt_crc32(arr, bytes) != rd32(h + 88)) goto out;

    memset(t->ent, 0, sizeof t->ent);
    for (uint32_t i = 0; i < t->n_entries; i++) {
        const uint8_t *e = arr + (size_t)i * t->entry_size;
        memcpy(t->ent[i].type, e, 16);
        memcpy(t->ent[i].uuid, e + 16, 16);
        t->ent[i].first = rd64(e + 32);
        t->ent[i].last  = rd64(e + 40);
        t->ent[i].attrs = rd64(e + 48);
        for (int c = 0; c < GPT_NAME_CH; c++)
            t->ent[i].name[c] = rd16(e + 56 + c * 2);
    }
    rc = 0;
out:
    free(arr);
    return rc;
}

int gpt_read(int fd, uint32_t sector, uint64_t disk_bytes, gpt_table *t)
{
    memset(t, 0, sizeof *t);
    if (sector < 512 || sector > 4096 || (sector & (sector - 1))) return -1;
    t->sector = sector;
    t->disk_sectors = disk_bytes / sector;

    uint8_t h[4096];

    /* The primary first. */
    if (read_at(fd, h, sector, sector) == 0 &&
        parse_header(h, sector, t) == 0 &&
        parse_entries(fd, h, t) == 0) {
        t->valid = 1;
        return 0;
    }

    /* THE BACKUP, and this is not a nicety. A power cut during a
     * commit is precisely the case where the primary is torn and the
     * backup is the truth, and it is the case somebody is running a
     * rescue tool in. Reading only the primary means the rescue tool
     * is blind exactly when it is needed. */
    if (t->disk_sectors < 2) return -1;
    memset(t, 0, sizeof *t);
    t->sector = sector;
    t->disk_sectors = disk_bytes / sector;
    uint64_t alt = t->disk_sectors - 1;
    if (read_at(fd, h, sector, alt * sector) != 0) return -1;
    if (parse_header(h, sector, t) != 0) return -1;
    if (parse_entries(fd, h, t) != 0) return -1;
    t->valid = 1;
    t->from_backup = 1;
    return 0;
}

int gpt_find_start(const gpt_table *t, uint64_t first_lba)
{
    for (uint32_t i = 0; i < t->n_entries; i++)
        if (gpt_used(&t->ent[i]) && t->ent[i].first == first_lba)
            return (int)i;
    return -1;
}

uint64_t gpt_gap_end(const gpt_table *t, uint64_t after_lba)
{
    uint64_t best = t->last_usable + 1;
    for (uint32_t i = 0; i < t->n_entries; i++) {
        if (!gpt_used(&t->ent[i])) continue;
        uint64_t s = t->ent[i].first;
        if (s >= after_lba && s < best) best = s;
    }
    return best;
}

int gpt_overlaps(const gpt_table *t, uint64_t first, uint64_t last, int except)
{
    for (uint32_t i = 0; i < t->n_entries; i++) {
        if ((int)i == except) continue;
        if (!gpt_used(&t->ent[i])) continue;
        if (first <= t->ent[i].last && t->ent[i].first <= last)
            return (int)i;
    }
    return -1;
}

int gpt_resize_entry(gpt_table *t, int idx, uint64_t new_last)
{
    if (idx < 0 || (uint32_t)idx >= t->n_entries) return -1;
    gpt_entry *e = &t->ent[idx];
    if (!gpt_used(e)) return -1;
    /* SHRINK ONLY. Growing an entry is how a partition swallows the
     * one after it, and nothing in this product needs to grow one --
     * the restore rebuilds the whole captured table instead. */
    if (new_last >= e->last) return -1;
    if (new_last < e->first) return -1;
    e->last = new_last;
    return 0;
}

/* A version-4 UUID out of the kernel's randomness. Not rand(): two
 * partitions with the same PARTUUID is a machine that mounts the wrong
 * one, and this program has no business inventing entropy. */
static int mkguid(uint8_t g[16])
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (read_at(fd, g, 16, 0) != 0) {
        ssize_t k = read(fd, g, 16);
        close(fd);
        if (k != 16) return -1;
        return 0;
    }
    close(fd);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);      /* version 4        */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);      /* variant          */
    return 0;
}

int gpt_add(gpt_table *t, const uint8_t type[16], const char *name_ascii,
            uint64_t first, uint64_t last, uint8_t uuid_out[16],
            char *why, size_t n)
{
    if (last < first) {
        snprintf(why, n, "an empty partition was proposed");
        return -1;
    }
    if (first < t->first_usable || last > t->last_usable) {
        snprintf(why, n,
                 "the proposed partition does not fit between the parts of "
                 "the disk the firmware allows");
        return -1;
    }
    int hit = gpt_overlaps(t, first, last, -1);
    if (hit >= 0) {
        snprintf(why, n,
                 "the proposed partition would sit on top of partition %d",
                 hit + 1);
        return -1;
    }
    for (uint32_t i = 0; i < t->n_entries; i++) {
        if (gpt_used(&t->ent[i])) continue;
        gpt_entry *e = &t->ent[i];
        memset(e, 0, sizeof *e);
        memcpy(e->type, type, 16);
        if (mkguid(e->uuid) != 0) {
            snprintf(why, n, "this computer would not supply a random number");
            return -1;
        }
        if (uuid_out) memcpy(uuid_out, e->uuid, 16);
        e->first = first; e->last = last;
        for (int c = 0; c < GPT_NAME_CH && name_ascii && name_ascii[c]; c++)
            e->name[c] = (uint16_t)(unsigned char)name_ascii[c];
        return (int)i;
    }
    /* NOT GROWN. Enlarging the entry array moves FirstUsableLBA, which
     * on a disk whose first partition starts right after it means
     * moving it on top of somebody's filesystem. */
    snprintf(why, n,
             "this disk's partition table is full and AurOS will not enlarge "
             "it");
    return -1;
}

/* ── writing ─────────────────────────────────────────────────────── */

size_t gpt_array_bytes(const gpt_table *t)
{ return (size_t)t->n_entries * t->entry_size; }

void gpt_serialize(const gpt_table *t, int primary, uint8_t *hdr, uint8_t *arr)
{
    size_t bytes = gpt_array_bytes(t);
    memset(arr, 0, bytes);
    for (uint32_t i = 0; i < t->n_entries; i++) {
        uint8_t *e = arr + (size_t)i * t->entry_size;
        const gpt_entry *s = &t->ent[i];
        memcpy(e, s->type, 16);
        memcpy(e + 16, s->uuid, 16);
        wr64(e + 32, s->first);
        wr64(e + 40, s->last);
        wr64(e + 48, s->attrs);
        for (int c = 0; c < GPT_NAME_CH; c++) {
            e[56 + c * 2]     = (uint8_t)s->name[c];
            e[56 + c * 2 + 1] = (uint8_t)(s->name[c] >> 8);
        }
    }

    uint64_t alt = t->disk_sectors - 1;
    /* The backup array sits immediately below the backup header. */
    uint64_t back_arr = alt - (bytes + t->sector - 1) / t->sector;

    memset(hdr, 0, t->sector);
    memcpy(hdr, "EFI PART", 8);
    wr32(hdr + 8,  t->revision ? t->revision : 0x00010000u);
    wr32(hdr + 12, t->header_size);
    wr32(hdr + 16, 0);                       /* CRC, filled in below  */
    wr32(hdr + 20, 0);
    wr64(hdr + 24, primary ? 1 : alt);
    wr64(hdr + 32, primary ? alt : 1);
    wr64(hdr + 40, t->first_usable);
    wr64(hdr + 48, t->last_usable);
    memcpy(hdr + 56, t->disk_guid, 16);
    wr64(hdr + 72, primary ? t->entry_lba : back_arr);
    wr32(hdr + 80, t->n_entries);
    wr32(hdr + 84, t->entry_size);
    wr32(hdr + 88, gpt_crc32(arr, bytes));
    wr32(hdr + 16, gpt_crc32(hdr, t->header_size));
}
