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

/* ── the survey ──────────────────────────────────────────────────── */

/* A name too long for the fields below is REFUSED, not truncated.
 * Truncating one produces a name that is a valid prefix of a
 * different device, and this program is about to tell somebody which
 * disk it is going to resize. A device we cannot name exactly is a
 * device we will not touch. */
static int name_fits(const char *name)
{ return strlen(name) > 0 && strlen(name) < STAGE_NAME; }

static int is_whole_disk(const char *name)
{
    char dir[64 + STAGE_NAME];
    snprintf(dir, sizeof dir, "/sys/class/block/%s/device", name);
    struct stat st;
    if (stat(dir, &st) != 0) {
        /* No device link: either a partition, or something virtual.
         * A partition has a `partition` file; virtual devices (loop,
         * ram, dm) have neither, and this environment has no business
         * with those. */
        return 0;
    }
    snprintf(dir, sizeof dir, "/sys/class/block/%s/partition", name);
    return stat(dir, &st) != 0;
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
        snprintf(dir, sizeof dir, "/sys/class/block/%s", e->d_name);
        char tmp[64];
        if (slurp(dir, "partition", tmp, sizeof tmp) < 0) continue;

        stage_part *p = &d->part[d->n_parts];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", e->d_name);
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
        snprintf(d->name, sizeof d->name, "%s", e->d_name);

        char dir[64 + STAGE_NAME];
        snprintf(dir, sizeof dir, "/sys/class/block/%s", d->name);
        d->bytes = (uint64_t)slurp_ll(dir, "size", 0) * 512ull;
        d->removable = (int)slurp_ll(dir, "removable", 0);

        char qdir[96 + STAGE_NAME];
        snprintf(qdir, sizeof qdir, "%s/queue", dir);
        d->logical_sector  = (int)slurp_ll(qdir, "logical_block_size", 512);
        d->physical_sector = (int)slurp_ll(qdir, "physical_block_size", 512);

        char ddir[96 + STAGE_NAME];
        snprintf(ddir, sizeof ddir, "%s/device", dir);
        if (slurp(ddir, "model", d->model, sizeof d->model) < 0)
            slurp(dir, "device/model", d->model, sizeof d->model);

        survey_parts(d);
        m->n_disks++;
    }
    closedir(dp);
    return m->n_disks;
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
