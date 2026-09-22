/* record.c — see record.h. Writes through wr.c and nothing else. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "record.h"
#include "gpt.h"
#include "wr.h"

/* 7E1C3B90-4D2A-4F16-8B77-2C6E5A9D0E33, mixed-endian as GPT stores it. */
const uint8_t RECORD_TYPE_GUID[16] = {
    0x90,0x3B,0x1C,0x7E, 0x2A,0x4D, 0x16,0x4F,
    0x8B,0x77, 0x2C,0x6E,0x5A,0x9D,0x0E,0x33 };

#define REC_MAGIC "AURREC01"

static void wr32v(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void wr64v(uint8_t *p, uint64_t v) { wr32v(p, (uint32_t)v); wr32v(p+4, (uint32_t)(v>>32)); }
static uint32_t rd32v(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t rd64v(const uint8_t *p)
{ return (uint64_t)rd32v(p) | ((uint64_t)rd32v(p+4) << 32); }

/*  0  8  "AURREC01"
 *  8  8  seq
 * 16  8  run_id
 * 24  4  step
 * 28  8  when (unix)
 * 36 160 note
 * 196 4  CRC32 of bytes 0..195
 */
static int slot_parse(const uint8_t *s, rec_entry *e)
{
    if (memcmp(s, REC_MAGIC, 8) != 0) return 0;
    if (gpt_crc32(s, 196) != rd32v(s + 196)) return 0;
    e->seq    = rd64v(s + 8);
    e->run_id = rd64v(s + 16);
    e->step   = rd32v(s + 24);
    e->when   = rd64v(s + 28);
    memcpy(e->note, s + 36, REC_NOTE);
    e->note[REC_NOTE - 1] = 0;
    return 1;
}

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

int rec_open(rec_target *r, const stage_machine *m, uint64_t run_id,
             char *why, size_t n)
{
    memset(r, 0, sizeof *r);
    r->run_id = run_id;

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
            if (memcmp(t.ent[k].type, RECORD_TYPE_GUID, 16) != 0) continue;
            uint64_t bytes = (t.ent[k].last - t.ent[k].first + 1) * (uint64_t)ss;
            if (bytes < (uint64_t)REC_SLOTS * REC_SLOT_BYTES) continue;
            if ((size_t)snprintf(r->dev, sizeof r->dev, "%s", dd)
                    >= sizeof r->dev)
                continue;
            r->base = t.ent[k].first * (uint64_t)ss;
            r->open = 1;

            /* The next sequence number, from whatever is already
             * there -- including records from other runs, so that the
             * ordering across runs stays true. */
            rec_entry e;
            if (rec_last(r, &e) == 0) r->seq = e.seq + 1;
            else                      r->seq = 1;
            return 0;
        }
    }
    snprintf(why, n,
             "AurOS could not find anywhere to write down what it is doing.");
    return -1;
}

int rec_last(const rec_target *r, rec_entry *out)
{
    if (!r->open) return -1;
    int fd = open(r->dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    uint8_t slot[REC_SLOT_BYTES];
    rec_entry best; int have = 0;
    memset(&best, 0, sizeof best);
    for (int i = 0; i < REC_SLOTS; i++) {
        if (read_at(fd, slot, sizeof slot,
                    r->base + (uint64_t)i * REC_SLOT_BYTES) != 0)
            continue;
        rec_entry e;
        memset(&e, 0, sizeof e);
        if (!slot_parse(slot, &e)) continue;   /* torn or never written */
        if (!have || e.seq > best.seq) { best = e; have = 1; }
    }
    close(fd);
    if (!have) return -1;
    *out = best;
    return 0;
}

int rec_is_ours(const rec_target *r, const rec_entry *e)
{ return r->run_id && e->run_id == r->run_id; }

int rec_write(rec_target *r, rec_step step, const char *note,
              char *why, size_t n)
{
    if (!r->open) { snprintf(why, n, "there is nowhere to write it"); return -1; }

    uint8_t slot[REC_SLOT_BYTES];
    memset(slot, 0, sizeof slot);
    memcpy(slot, REC_MAGIC, 8);
    wr64v(slot + 8,  r->seq);
    wr64v(slot + 16, r->run_id);
    wr32v(slot + 24, (uint32_t)step);
    wr64v(slot + 28, (uint64_t)time(NULL));
    if (note) snprintf((char *)slot + 36, REC_NOTE, "%s", note);
    wr32v(slot + 196, gpt_crc32(slot, 196));

    /* Round robin, so the slot before this one survives a torn write
     * of this one. Nothing is ever overwritten in place. */
    uint64_t at = r->base + (r->seq % REC_SLOTS) * (uint64_t)REC_SLOT_BYTES;

    wr_target t;
    if (wr_open(&t, r->dev, why, n) != 0) return -1;
    int rc = -1;
    if (wr_arm(&t, WR_LOG, r->base,
               r->base + (uint64_t)REC_SLOTS * REC_SLOT_BYTES, why, n) != 0)
        goto out;
    if (wr_bytes(&t, WR_LOG, at, slot, sizeof slot, why, n) != 0) goto out;
    if (wr_flush(&t) != 0) {
        snprintf(why, n, "the memory stick would not finish writing");
        goto out;
    }
    r->seq++;
    rc = 0;
out:
    wr_close(&t);
    return rc;
}

const char *rec_step_name(rec_step s)
{
    switch (s) {
    case REC_NONE:          return "none";
    case REC_BEGIN:         return "begin";
    case REC_GATE_OK:       return "checks-passed";
    case REC_SHRINK_BEGIN:  return "shrink-begin";
    case REC_SHRINK_END:    return "shrink-end";
    case REC_WRITE_BEGIN:   return "write-begin";
    case REC_WRITE_END:     return "write-end";
    case REC_PROBE_END:     return "probe-end";
    case REC_COMMIT_ARRAY:  return "commit-array";
    case REC_COMMIT_SECTOR: return "committed";
    case REC_COMMIT_BACKUP: return "commit-backup";
    case REC_SETTLE_END:    return "settled";
    case REC_DONE:          return "done";
    case REC_REFUSED:       return "refused";
    case REC_FAILED:        return "failed";
    }
    return "?";
}
