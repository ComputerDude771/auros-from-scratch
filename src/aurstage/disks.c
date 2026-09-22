/* disks.c — what is on this machine, read and never written.
 *
 * THE ONLY PLACE IN THIS PROGRAM THAT OPENS A BLOCK DEVICE.
 *
 * It opens them O_RDONLY, and there is no other opener, so "stage A
 * writes nothing" is a property of the code's shape rather than of
 * everybody remembering. When stage C adds a writer it will be a
 * second, obvious, separately-named function, and the diff that adds
 * it will be the diff that has to justify it.
 *
 * Everything here comes from /sys and from reading the first sectors
 * of a device. No libblkid, no udev, no mounting -- especially no
 * mounting. `ntfsresize` refuses a mounted volume, and a Windows
 * volume mounted read-write for even a moment is a volume whose
 * journal we have altered, which is the one thing this environment
 * exists to avoid.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "aurstage.h"
#include "ntfs.h"
#include "sha256.h"

/* ── /sys, read as text ──────────────────────────────────────────── */

static int slurp(const char *dir, const char *leaf, char *out, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    while (k > 0 && (out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
    return (int)k;
}

static long long slurp_ll(const char *dir, const char *leaf, long long dflt)
{
    char buf[64];
    if (slurp(dir, leaf, buf, sizeof buf) < 0) return dflt;
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    return (end && end != buf) ? v : dflt;
}

/* ── reading a device, read-only, once ───────────────────────────── */

/* THE ONE OPENER. O_RDONLY is not a parameter and must not become
 * one. Stage A has no business writing to a disk, and the way to be
 * sure of that is for the program to contain no way to do it. */
static int open_ro(const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "/dev/%s", name);
    return open(path, O_RDONLY | O_CLOEXEC);
}

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

/* ── what filesystem is this? ────────────────────────────────────────
 *
 * Signature reading, not libblkid: three filesystems matter to this
 * program and each announces itself in its first block. Guessing wrong
 * here is not a cosmetic error -- it decides which partition we are
 * about to treat as Windows -- so each one is matched on its magic and
 * nothing is identified by size, order or position on the disk. */
static void identify(int fd, stage_part *p)
{
    p->fstype[0] = p->label[0] = p->uuid[0] = 0;

    unsigned char b[4096];
    if (read_at(fd, b, sizeof b, 0) < 0) return;

    /* BITLOCKER FIRST, AND IT IS NOT A DETAIL.
     *
     * An encrypted volume has no "NTFS" in its first sector, so this
     * used to fall through every branch below and come out as fstype
     * "" -- and the dry run, looking for a partition marked "ntfs",
     * told the owner of a perfectly ordinary encrypted laptop that
     * this computer has no Windows on it. That is both wrong and
     * useless: BitLocker is one of the commonest things this product
     * will meet, and the person can clear it in five minutes if
     * somebody tells her what it is.
     *
     * tools/stagetest.sh found this by planting the signature on a
     * real volume and insisting the machine say the word. The reading
     * of the partition never lied; the survey simply had no name for
     * what it was looking at. */
    if (ntfs_is_bitlocker(b)) {
        snprintf(p->fstype, sizeof p->fstype, "bitlocker");
        return;
    }

    /* NTFS: "NTFS    " at offset 3 of the boot sector, and a 0xAA55
     * signature. Both, because the OEM field alone appears in the wild
     * on things that are not NTFS. */
    if (!memcmp(b + 3, "NTFS    ", 8) && b[510] == 0x55 && b[511] == 0xAA) {
        snprintf(p->fstype, sizeof p->fstype, "ntfs");
        /* The serial the NTFS boot sector carries, printed the way
         * Windows prints it, so a support call can match it. */
        snprintf(p->uuid, sizeof p->uuid, "%02X%02X-%02X%02X",
                 b[0x4B], b[0x4A], b[0x49], b[0x48]);
        return;
    }
    /* FAT32, which on a GPT disk is almost always the ESP. */
    if (!memcmp(b + 0x52, "FAT32   ", 8) ||
        (!memcmp(b + 0x36, "FAT", 3) && b[510] == 0x55 && b[511] == 0xAA)) {
        snprintf(p->fstype, sizeof p->fstype, "vfat");
        return;
    }
    /* ext2/3/4: magic 0xEF53 at 0x38 of the superblock, which lives at
     * byte 1024. */
    unsigned char sb[1024];
    if (read_at(fd, sb, sizeof sb, 1024) == 0 &&
        sb[0x38] == 0x53 && sb[0x39] == 0xEF) {
        snprintf(p->fstype, sizeof p->fstype, "ext4");
        char *lab = (char *)sb + 0x78;               /* s_volume_name */
        int n = 0;
        while (n < 16 && lab[n]) n++;
        if (n) snprintf(p->label, sizeof p->label, "%.*s", n, lab);
        const unsigned char *u = sb + 0x68;          /* s_uuid */
        snprintf(p->uuid, sizeof p->uuid,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                 "%02x%02x%02x%02x%02x%02x",
                 u[0],u[1],u[2],u[3], u[4],u[5], u[6],u[7], u[8],u[9],
                 u[10],u[11],u[12],u[13],u[14],u[15]);
        return;
    }
}

static uint32_t le32_at(const unsigned char *p);
static uint64_t le64_at(const unsigned char *p);

/* ── the survey ──────────────────────────────────────────────────── */

/* A name too long for the fields below is REFUSED, not truncated.
 * Truncating one produces a name that is a valid prefix of a
 * different device, and this program is about to tell somebody which
 * disk it is going to resize. A device we cannot name exactly is a
 * device we will not touch. */
static int name_fits(const char *name)
{ return strlen(name) > 0 && strlen(name) < STAGE_NAME; }

/* Build a /sys path, REFUSING rather than truncating. A truncated path
 * names a different file, and this program is about to decide from it
 * which disk to resize. The callers all check name_fits() first, but
 * the property should be true of this function on its own: a guard you
 * have to hold in your head while reading three call sites is a guard
 * that stops being true when somebody adds a fourth. */
static int sys_path(char *out, size_t n, const char *name, const char *leaf)
{
    int k = leaf ? snprintf(out, n, "/sys/class/block/%s/%s", name, leaf)
                 : snprintf(out, n, "/sys/class/block/%s", name);
    return (k > 0 && (size_t)k < n) ? 0 : -1;
}

static int is_whole_disk(const char *name)
{
    char dir[64 + STAGE_NAME];
    if (sys_path(dir, sizeof dir, name, "device") != 0) return 0;
    struct stat st;
    if (stat(dir, &st) != 0) {
        /* No device link: either a partition, or something virtual.
         * A partition has a `partition` file; virtual devices (loop,
         * ram, dm) have neither, and this environment has no business
         * with those. */
        return 0;
    }
    if (sys_path(dir, sizeof dir, name, "partition") != 0) return 0;
    return stat(dir, &st) != 0;
}

/* The EFI System partition's type GUID, C12A7328-F81F-11D2-BA4B-
 * 00A0C93EC93B, in the mixed-endian order GPT stores it: first three
 * fields little-endian, last two as written. */
static const unsigned char ESP_GUID[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};

/* Read the disk's GPT and mark the partitions it names.
 *
 * WHY THIS HAD TO BE WRITTEN. stage_part carried an `is_esp` field
 * that NOTHING EVER SET. One place read it -- the third-party
 * encryption scan, to find the ESP a UEFI VeraCrypt install hides its
 * loader in -- so that scan was handed NULL every time and the half
 * of it that matters on a modern machine never ran. A field nobody
 * assigns is worse than a field that does not exist: the code that
 * reads it looks finished. An adversarial review found it; nothing
 * about the program's behaviour would have. */
static void mark_from_gpt(stage_disk *d)
{
    int fd = open_ro(d->name);
    if (fd < 0) return;
    uint32_t ss = (uint32_t)(d->logical_sector > 0 ? d->logical_sector : 512);
    if (ss < 512 || ss > 4096 || (ss & (ss - 1))) { close(fd); return; }

    unsigned char h[4096];
    if (pread(fd, h, ss, (off_t)ss) != (ssize_t)ss ||
        memcmp(h, "EFI PART", 8) != 0) { close(fd); return; }
    d->gpt = 1;

    uint64_t plba = le64_at(h + 72);
    uint32_t num  = le32_at(h + 80);
    uint32_t esz  = le32_at(h + 84);
    if (esz < 128 || esz > 4096 || num == 0 || num > 4096 || plba < 2)
    { close(fd); return; }

    unsigned char ent[4096];
    for (uint32_t i = 0; i < num && i < STAGE_MAX_PART * 4; i++) {
        uint64_t at = plba * ss + (uint64_t)i * esz;
        if (at / ss < plba) break;                      /* wrapped */
        if (pread(fd, ent, esz, (off_t)at) != (ssize_t)esz) break;
        uint64_t first = le64_at(ent + 32);
        if (!first) continue;                           /* unused entry */
        /* GPT entries are in DISK order and partition numbers follow
         * the entry index, which is what lets an entry be matched to
         * the /sys partition the kernel made from it. */
        for (int k = 0; k < d->n_parts; k++) {
            if (d->part[k].start_lba * 512ull != first * ss) continue;
            d->part[k].is_gpt = 1;
            if (!memcmp(ent, ESP_GUID, 16)) d->part[k].is_esp = 1;
        }
    }
    close(fd);
}

static void survey_parts(stage_disk *d)
{
    DIR *dp = opendir("/sys/class/block");
    if (!dp) return;
    struct dirent *e;
    size_t dl = strlen(d->name);
    while ((e = readdir(dp)) && d->n_parts < STAGE_MAX_PART) {
        if (strncmp(e->d_name, d->name, dl) != 0) continue;
        if (!e->d_name[dl]) continue;                  /* the disk itself */
        if (!name_fits(e->d_name)) continue;
        char dir[64 + STAGE_NAME];
        if (sys_path(dir, sizeof dir, e->d_name, NULL) != 0) continue;
        char tmp[64];
        if (slurp(dir, "partition", tmp, sizeof tmp) < 0) continue;

        stage_part *p = &d->part[d->n_parts];
        memset(p, 0, sizeof *p);
        if ((size_t)snprintf(p->name, sizeof p->name, "%s", e->d_name)
                >= sizeof p->name) continue;
        p->start_lba = (uint64_t)slurp_ll(dir, "start", 0);
        p->sectors   = (uint64_t)slurp_ll(dir, "size", 0);
        /* /sys reports sizes in 512-byte units WHATEVER the device's
         * logical sector size is. That is a kernel convention, not a
         * fact about the disk, and treating it as one is how a 4Kn
         * machine ends up with a partition eight times the wrong
         * size. */
        p->bytes = p->sectors * 512ull;

        int fd = open_ro(p->name);
        if (fd >= 0) { identify(fd, p); close(fd); }
        d->n_parts++;
    }
    closedir(dp);
}

int stage_survey(stage_machine *m)
{
    memset(m, 0, sizeof *m);
    DIR *dp = opendir("/sys/class/block");
    if (!dp) return 0;
    struct dirent *e;
    while ((e = readdir(dp)) && m->n_disks < STAGE_MAX_DISK) {
        if (e->d_name[0] == '.') continue;
        if (!name_fits(e->d_name)) {
            stage_warn("ignoring block device with an unusably long name");
            continue;
        }
        if (!is_whole_disk(e->d_name)) continue;

        stage_disk *d = &m->disk[m->n_disks];
        memset(d, 0, sizeof *d);
        if ((size_t)snprintf(d->name, sizeof d->name, "%s", e->d_name)
                >= sizeof d->name) continue;

        char dir[64 + STAGE_NAME];
        if (sys_path(dir, sizeof dir, d->name, NULL) != 0) continue;
        d->bytes = (uint64_t)slurp_ll(dir, "size", 0) * 512ull;
        d->removable = (int)slurp_ll(dir, "removable", 0);

        char qdir[96 + STAGE_NAME];
        snprintf(qdir, sizeof qdir, "%s/queue", dir);
        long long ls = slurp_ll(qdir, "logical_block_size", 0);
        d->sector_known    = ls > 0;
        d->logical_sector  = (int)(ls > 0 ? ls : 512);
        d->physical_sector = (int)slurp_ll(qdir, "physical_block_size",
                                           d->logical_sector);

        char ddir[96 + STAGE_NAME];
        snprintf(ddir, sizeof ddir, "%s/device", dir);
        if (slurp(ddir, "model", d->model, sizeof d->model) < 0)
            slurp(dir, "device/model", d->model, sizeof d->model);

        /* WHICH DISK IS THIS, and it is harder than it looks.
         *
         * NVMe publishes device/serial. virtio-blk publishes serial.
         * SATA -- which is most of the fleet this product exists for --
         * publishes NEITHER: the serial reaches udev through an ATA
         * IDENTIFY ioctl, and what /sys offers instead is the SCSI
         * inquiry data in device/vpd_pg80 and the identifier in
         * device/wwid. A check that only looked at the first two
         * therefore could not identify any ordinary laptop disk, which
         * an adversarial review noticed and no test would have: the
         * test machine is virtio. */
        static const char *const WHERE[] = {
            "device/serial", "serial", "device/wwid", "wwid",
            "device/vpd_pg80", "device/unique_id",
        };
        for (size_t w = 0; w < sizeof WHERE / sizeof WHERE[0]; w++) {
            char raw[160] = {0};
            /* slurp returns the LENGTH, and testing it against 0
             * skipped every attribute that was read successfully.
             * The whole identity check then reported "this computer's
             * disk will not say which one it is" on a machine that
             * says so plainly. */
            if (slurp(dir, WHERE[w], raw, sizeof raw) < 0) continue;
            /* vpd_pg80 is binary: a four-byte header then the ASCII
             * serial. Everything printable from it, trimmed. */
            const char *b = raw;
            if (!strcmp(WHERE[w], "device/vpd_pg80")) b = raw + 4;
            char clean[80]; size_t ci = 0;
            for (const char *q = b; *q && ci + 1 < sizeof clean; q++)
                if (*q > ' ' && (unsigned char)*q < 127) clean[ci++] = *q;
            clean[ci] = 0;
            if (ci >= 4) { snprintf(d->serial, sizeof d->serial, "%s", clean);
                           break; }
        }

        survey_parts(d);
        mark_from_gpt(d);
        m->n_disks++;
    }
    closedir(dp);
    return m->n_disks;
}

/* ── the shape of the partition table, in one number ─────────────── */

uint32_t le32_at(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t le64_at(const unsigned char *p)
{ return (uint64_t)le32_at(p) | ((uint64_t)le32_at(p + 4) << 32); }

int stage_gpt_sha256(const stage_disk *d, char *hex, size_t n)
{
    if (!d || !hex || n < 65) return -1;
    hex[0] = 0;
    uint32_t ss = (uint32_t)(d->logical_sector > 0 ? d->logical_sector : 512);
    if (ss < 512 || ss > 4096 || (ss & (ss - 1))) return -1;

    int fd = open_ro(d->name);
    if (fd < 0) return -1;

    unsigned char lba1[4096];
    if (pread(fd, lba1, ss, (off_t)ss) != (ssize_t)ss) { close(fd); return -1; }
    if (memcmp(lba1, "EFI PART", 8) != 0) { close(fd); return -1; }

    uint32_t hsize = le32_at(lba1 + 12);
    uint64_t plba  = le64_at(lba1 + 72);
    uint32_t num   = le32_at(lba1 + 80);
    uint32_t esz   = le32_at(lba1 + 84);
    /* Every one of these came off the disk, so every one is checked
     * before it becomes a length. A crafted header claiming four
     * billion entries is a read of sixteen terabytes. */
    if (hsize < 92 || hsize > ss) { close(fd); return -1; }
    if (esz < 128 || esz > 4096 || num == 0 || num > 4096)
    { close(fd); return -1; }
    uint64_t bytes = (uint64_t)num * esz;
    /* plba WAS THE ONE FIELD NOT CHECKED, and the comment above
     * claimed all of them were. plba * ss wraps for plba near 2^55 on
     * a 512-byte disk, `at` comes out 0, and the function then hashes
     * the protective MBR as if it were the entry array -- and returns
     * SUCCESS with a wrong, self-consistent digest. The result is a
     * refusal rather than a corruption, but a refusal nobody can
     * explain is its own kind of failure. */
    if (plba < 2 || plba > UINT64_MAX / ss) { close(fd); return -1; }
    uint64_t at0 = plba * ss;
    if (bytes > (16u << 20) || at0 > UINT64_MAX - bytes)
    { close(fd); return -1; }
    if (d->bytes && at0 + bytes > d->bytes) { close(fd); return -1; }

    sha256 h;
    sha256_start(&h);
    sha256_feed(&h, lba1, hsize);

    unsigned char buf[65536];
    uint64_t at = at0, left = bytes;
    while (left) {
        size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
        if (pread(fd, buf, want, (off_t)at) != (ssize_t)want) { close(fd); return -1; }
        sha256_feed(&h, buf, want);
        at += want; left -= want;
    }
    close(fd);

    unsigned char dig[32];
    sha256_done(&h, dig);
    sha256_hex(dig, hex, n);
    return 0;
}

/* ── when nothing turned up ──────────────────────────────────────────
 *
 * "This computer has no disk" is never true, and it is the worst
 * possible thing to tell somebody who is looking at the computer. The
 * useful thing is what IS on the bus: a machine with no storage
 * controller at all is a different problem from one with a RAID
 * controller that has nothing behind it, and the two lead to
 * different support calls.
 *
 * DELIBERATELY NOT A BIOS INSTRUCTION. The commonest cause of this is
 * Intel RST, and the obvious advice -- "change the disk mode to AHCI
 * in your computer's setup screen" -- stops Windows booting at all
 * until somebody does the safe-mode dance afterwards. Handing that
 * sentence to a person on their own is how this product breaks the
 * one thing it promised to preserve. So this states the facts and
 * leaves the instruction to somebody who can stay on the phone.
 *
 * (The VMD driver ships in this image for exactly this reason; see
 * build/staging. This is the message for when it was not enough.)
 */
void stage_report_controllers(void)
{
    DIR *dp = opendir("/sys/bus/pci/devices");
    if (!dp) return;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char dir[320];
        if ((size_t)snprintf(dir, sizeof dir, "/sys/bus/pci/devices/%s",
                             e->d_name) >= sizeof dir)
            continue;

        char cls[32] = {0};
        if (slurp(dir, "class", cls, sizeof cls) < 0) continue;
        /* 0x01 is mass storage. The subclass is the byte below it and
         * is what says whether this is a plain SATA controller or the
         * RAID mode that hides the disk behind it. */
        if (strncmp(cls, "0x01", 4) != 0) continue;
        unsigned sub = 0;
        if (sscanf(cls + 4, "%2x", &sub) != 1) sub = 0xFF;
        const char *kind =
            sub == 0x01 ? "IDE"   : sub == 0x04 ? "RAID (Intel RST or VMD)" :
            sub == 0x06 ? "SATA"  : sub == 0x08 ? "NVMe" :
            sub == 0x00 ? "SCSI"  : "storage";

        char ven[16] = {0}, dev[16] = {0}, drv[128] = {0};
        slurp(dir, "vendor", ven, sizeof ven);
        slurp(dir, "device", dev, sizeof dev);
        /* Which driver claimed it, if any. "No driver" and "a driver
         * that found nothing" are different failures. */
        char link[320 + 16];
        snprintf(link, sizeof link, "%s/driver", dir);
        ssize_t k = readlink(link, drv, sizeof drv - 1);
        if (k > 0) {
            drv[k] = 0;
            char *slash = strrchr(drv, '/');
            memmove(drv, slash ? slash + 1 : drv, strlen(slash ? slash + 1 : drv) + 1);
        } else {
            snprintf(drv, sizeof drv, "no driver");
        }

        if (!found++) stage_say("what this computer has instead:");
        stage_say("  %s  %s %s  %s", e->d_name, ven, dev, kind);
        stage_say("      %s", drv);
    }
    closedir(dp);
    if (!found)
        stage_say("this computer reports no storage controller at all, "
                  "which usually means the disk is behind something "
                  "the kernel did not enumerate");
}

static const char *human(uint64_t b, char *out, size_t n)
{
    /* Gibibytes, one decimal. A support engineer comparing this with
     * what Windows said needs the same unit Windows uses. */
    double g = (double)b / (1024.0 * 1024.0 * 1024.0);
    if (g >= 1.0) snprintf(out, n, "%.1f GiB", g);
    else          snprintf(out, n, "%llu MiB",
                           (unsigned long long)(b / (1024 * 1024)));
    return out;
}

void stage_report(const stage_machine *m)
{
    char sz[32];
    stage_say("this machine has %d disk%s", m->n_disks,
              m->n_disks == 1 ? "" : "s");
    for (int i = 0; i < m->n_disks; i++) {
        const stage_disk *d = &m->disk[i];
        stage_say("  %-10s %-10s %s%s  sectors %d/%d",
                  d->name, human(d->bytes, sz, sizeof sz),
                  d->model[0] ? d->model : "(no model)",
                  d->removable ? " [removable]" : "",
                  d->logical_sector, d->physical_sector);
        /* The sector size is printed even when it is the ordinary 512,
         * because the case that goes wrong silently is the one nobody
         * looked at. */
        for (int k = 0; k < d->n_parts; k++) {
            const stage_part *p = &d->part[k];
            stage_say("    %-12s %-10s %-5s %-12s %s",
                      p->name, human(p->bytes, sz, sizeof sz),
                      p->fstype[0] ? p->fstype : "?",
                      p->label[0] ? p->label : "-",
                      p->uuid[0] ? p->uuid : "-");
        }
    }
}
