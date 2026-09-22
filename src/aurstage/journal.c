/* journal.c — see journal.h.
 *
 * A STRICT READER, ON PURPOSE. This parses a file written by another
 * program, in another language, on the other side of a restart, and
 * the decision it feeds is whether to resize somebody's only copy of
 * their photographs. Every unexpected shape is a refusal. There is no
 * recovery, no skipping of fields it does not know, and no default for
 * anything that matters.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "journal.h"
#include "aurstage.h"

/* ── a flat-JSON reader, and nothing more ────────────────────────── */

static const char *skip_ws(const char *p)
{ while (*p && (unsigned char)*p <= ' ') p++; return p; }

/* Copy a JSON string body into `out`. Returns the character after the
 * closing quote, or NULL. Only the escapes a Windows path needs are
 * understood; anything else is a refusal, because silently dropping an
 * escape we do not know changes the value. */
static const char *read_string(const char *p, char *out, size_t n)
{
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            switch (*p++) {
            case '\\': c = '\\'; break;
            case '"':  c = '"';  break;
            case '/':  c = '/';  break;
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            default:   return NULL;
            }
        }
        if (i + 1 < n) out[i++] = c;
        else return NULL;                 /* too long is not truncated */
    }
    if (*p != '"') return NULL;
    out[i] = 0;
    return p + 1;
}

/* Copy `val` into a fixed field, or refuse. Never truncates: see the
 * call site. */
static int take(char *dst, size_t n, const char *val)
{
    size_t len = strlen(val);
    if (len + 1 > n) return 0;
    memcpy(dst, val, len + 1);
    return 1;
}

int journal_read(const char *path, journal *j)
{
    memset(j, 0, sizeof *j);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;                  /* not there: not a record */
    /* From here on the file EXISTS, so every failure below sets
     * `corrupt`. See the header: "there is one and it is wrong" and
     * "there is none" are different machines. */
    j->corrupt = 1;
    char buf[16384];
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return 0;
    buf[k] = 0;

    const char *p = skip_ws(buf);
    if (*p != '{') return 0;
    p++;

    for (;;) {
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }

        char key[64];
        p = read_string(p, key, sizeof key);
        if (!p) return 0;
        p = skip_ws(p);
        if (*p != ':') return 0;
        p = skip_ws(p + 1);

        if (*p == '"') {
            char val[512];
            p = read_string(p, val, sizeof val);
            if (!p) return 0;
            /* A value too long for its field is a REFUSAL, exactly as
             * in read_string above. snprintf() would truncate, and a
             * truncated disk serial still compares equal to a
             * different disk whose serial shares that prefix -- which
             * is the one comparison this whole file exists to make. */
            #define STR(name, field) \
                if (!strcmp(key, name)) { \
                    if (!take(j->field, sizeof j->field, val)) return 0; \
                }
            STR("disk_serial",  disk_serial)
            STR("disk_model",   disk_model)
            STR("win_part",     win_part)
            STR("gpt_sha256",   gpt_sha256)
            STR("stage",        stage)
            #undef STR
        } else {
            char *end = NULL;
            unsigned long long v = strtoull(p, &end, 10);
            if (!end || end == p) return 0;
            if (!strcmp(key, "disk_bytes"))      j->disk_bytes     = v;
            else if (!strcmp(key, "logical_sector")) j->logical_sector = (uint32_t)v;
            else if (!strcmp(key, "win_start_lba"))  j->win_start_lba  = v;
            else if (!strcmp(key, "win_sectors"))    j->win_sectors    = v;
            else if (!strcmp(key, "win_ntfs_serial")) j->win_ntfs_serial = v;
            else if (!strcmp(key, "written_unix"))   j->written_unix   = v;
            p = end;
        }
    }
    /* The four fields the whole check rests on. A journal missing any
     * of them cannot do its job, and a journal that cannot do its job
     * must not be treated as one that can. */
    if (!j->disk_serial[0] || !j->win_start_lba || !j->win_sectors)
        return 0;
    j->present = 1;
    j->corrupt = 0;
    return 1;
}

const char *journal_verdict_name(journal_verdict v)
{
    switch (v) {
    case JOURNAL_MATCH:         return "match";
    case JOURNAL_NONE:          return "none";
    case JOURNAL_UNREADABLE:    return "unreadable";
    case JOURNAL_WRONG_DISK:    return "wrong-disk";
    case JOURNAL_MOVED:         return "moved";
    case JOURNAL_RESIZED:       return "resized";
    case JOURNAL_STALE:         return "stale";
    case JOURNAL_TABLE_CHANGED: return "table-changed";
    case JOURNAL_CORRUPT:       return "corrupt";
    case JOURNAL_CLOCK:         return "clock-behind";
    case JOURNAL_SECTORS:       return "sector-size-changed";
    }
    return "unknown";
}

/* ── does the machine still match? ───────────────────────────────── */

static int read_sys(const char *dir, const char *leaf, char *out, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "/sys/class/block/%s/%s", dir, leaf);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    while (k > 0 && (out[k-1] == '\n' || out[k-1] == ' ')) out[--k] = 0;
    return 0;
}

/* Far enough back that Windows has had time to move things. Not a
 * guess about how long an update takes -- it is how long an armed
 * one-shot boot may sit unconsumed before it stops being the boot the
 * user asked for and starts being a surprise. */
#define JOURNAL_MAX_AGE_S  (3 * 24 * 60 * 60)

journal_verdict journal_check(const journal *j, const stage_machine *m,
                              char *why, size_t n)
{
    if (!j || !j->present) {
        snprintf(why, n, "Nothing on this computer asked for this.");
        return JOURNAL_NONE;
    }

    /* WHICH disk, by serial -- never by position. "the first one" is
     * how a machine with an SSD and a spinning disk gets the wrong
     * one, and the enumeration order of two controllers is not a
     * promise the kernel makes. */
    /* The survey already asked every place a disk publishes its
     * identity -- see the note in disks.c about SATA, which publishes
     * it in none of the obvious ones. Asking again here, differently,
     * is how two answers to the same question appear. */
    const stage_disk *d = NULL;
    for (int i = 0; i < m->n_disks && !d; i++)
        if (m->disk[i].serial[0] && !strcmp(m->disk[i].serial, j->disk_serial))
            d = &m->disk[i];
    if (!d) {
        /* Say whether we found NO serial at all, or found serials that
         * simply are not this one: they are different problems and
         * lead to different support calls. */
        int any = 0;
        for (int i = 0; i < m->n_disks; i++)
            if (m->disk[i].serial[0]) any = 1;
        if (!any) {
            snprintf(why, n,
                     "This computer's disk will not say which one it is.");
            return JOURNAL_UNREADABLE;
        }
        snprintf(why, n,
                 "This is not the disk the installer was prepared for.");
        return JOURNAL_WRONG_DISK;
    }

    /* AND THE WINDOWS PARTITION ITSELF. The serial says it is the
     * right disk; this says the disk is still laid out the way it was
     * when somebody looked at it and decided this was safe.
     *
     * A partition that has moved or changed size between the arming
     * and the boot means Windows has repartitioned, or a recovery tool
     * has, or this is a restored image -- and every number the shrink
     * is about to be given was measured against the old layout. */
    const stage_part *w = NULL;
    for (int k = 0; k < d->n_parts; k++)
        if (d->part[k].start_lba == j->win_start_lba) { w = &d->part[k]; break; }
    if (!w) {
        snprintf(why, n,
                 "The Windows part of this disk is not where it was when "
                 "the installer looked at it.");
        return JOURNAL_MOVED;
    }
    if (w->sectors != j->win_sectors) {
        snprintf(why, n,
                 "The Windows part of this disk is not the size it was "
                 "when the installer looked at it.");
        return JOURNAL_RESIZED;
    }

    /* AND THE WHOLE TABLE, not just the Windows entry.
     *
     * The two checks above catch a Windows partition that has moved
     * or changed size. They cannot see a partition ADDED after it, or
     * one removed, or a type changed, or a recovery tool having
     * rewritten the table with the same Windows entry in it -- and
     * every one of those changes where stage C is allowed to write.
     * The hash is one number and covers all of them.
     *
     * Only checked if the journal carries one. A journal from an
     * older AurBridge that does not is not a journal that failed;
     * inventing a comparison against an empty string would refuse
     * every machine. */
    /* NO HASH IS NOT A PASS. A record without one simply cannot make
     * this check, and the caller is told so through the verdict name
     * rather than being handed a plain "match" that hides it. */
    if (!j->gpt_sha256[0]) {
        snprintf(why, n, "This is the computer the installer was prepared "
                         "for, though its record does not say how the disk "
                         "was divided up.");
    }
    if (j->gpt_sha256[0]) {
        char now[65];
        if (stage_gpt_sha256(d, now, sizeof now) != 0) {
            snprintf(why, n,
                     "This computer's partition table could not be read "
                     "back to check it.");
            return JOURNAL_UNREADABLE;
        }
        int same = 1;
        for (int i = 0; i < 64 && same; i++) {
            char a = now[i], b = j->gpt_sha256[i];
            if (b >= 'A' && b <= 'F') b = (char)(b + 32);
            if (a != b) same = 0;
        }
        if (!same || j->gpt_sha256[64]) {
            snprintf(why, n,
                     "The way this disk is divided up has changed since "
                     "the installer looked at it.");
            return JOURNAL_TABLE_CHANGED;
        }
    }

    /* THE SECTOR SIZE, if the record states one. A disk whose
     * emulation mode has changed between the arming and the boot
     * reports every offset differently, and the shrink is about to be
     * given numbers measured under the old one. */
    if (j->logical_sector && d->sector_known &&
        (int)j->logical_sector != d->logical_sector) {
        snprintf(why, n,
                 "This disk is reporting itself differently than it did "
                 "when the installer looked at it.");
        return JOURNAL_SECTORS;
    }

    if (j->written_unix) {
        time_t now = time(NULL);
        if (now > 0 && (uint64_t)now < j->written_unix) {
            /* THE CLOCK IS BEHIND THE RECORD, which cannot happen on a
             * machine that is telling the truth about the time. A dead
             * CMOS battery puts a laptop in 2010, and there is no
             * network here and no NTP to correct it -- so the staleness
             * check below, the thing that catches a BootNext consumed
             * three weeks late, silently could not fire. Falling
             * through to MATCH was the old behaviour and it was the
             * wrong way to be wrong. */
            snprintf(why, n,
                     "This computer's clock is set earlier than when the "
                     "installer prepared it, so AurOS cannot tell how "
                     "long ago that was.");
            return JOURNAL_CLOCK;
        }
        if (now > 0 && (uint64_t)now - j->written_unix > JOURNAL_MAX_AGE_S) {
            snprintf(why, n,
                     "This was prepared more than three days ago. Windows "
                     "has had time to change the disk since.");
            return JOURNAL_STALE;
        }
    }
    snprintf(why, n, "This is the computer the installer was prepared for.");
    return JOURNAL_MATCH;
}
