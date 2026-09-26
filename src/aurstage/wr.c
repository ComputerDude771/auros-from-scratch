/* wr.c — see wr.h. The one file in this directory that writes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "wr.h"

static const char *kind_name(wr_kind k)
{
    switch (k) {
    case WR_ROOT:        return "the AurOS area";
    case WR_RECOVERY:    return "the AurOS start-up area";
    case WR_GPT_PRIMARY: return "the partition table";
    case WR_GPT_BACKUP:  return "the spare partition table";
    case WR_LOG:         return "the installer's notes";
    case WR_MIRROR:      return "the way back, on this computer";
    case WR_RESCUE:      return "the way back, on the memory stick";
    case WR_RESTORE:     return "putting a saved part of Windows back";
    case WR_N:           break;
    }
    return "somewhere unnamed";
}

int wr_open(wr_target *t, const char *dev, char *why, size_t n)
{
    memset(t, 0, sizeof *t);
    t->fd = -1;
    if (!dev || !dev[0]) { snprintf(why, n, "no disk was named"); return -1; }
    if ((size_t)snprintf(t->dev, sizeof t->dev, "%s", dev) >= sizeof t->dev) {
        /* Refused, not truncated: a truncated device name is a valid
         * prefix of a different device, and this one is about to be
         * written to. disks.c makes the same call for the same
         * reason. */
        snprintf(why, n, "that disk's name is too long to use safely");
        return -1;
    }

    /* THE ONE O_RDWR IN src/aurstage. build/staging greps for writable
     * opens and allows them only in this file. */
    t->fd = open(t->dev, O_RDWR | O_CLOEXEC);
    if (t->fd < 0) {
        snprintf(why, n, "this computer's disk could not be opened for "
                         "writing (%s)", strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(t->fd, &st) == 0 && S_ISREG(st.st_mode)) {
        t->dev_bytes = (uint64_t)st.st_size;
    } else {
        /* BLKGETSIZE64 */
        unsigned long long b = 0;
        if (ioctl(t->fd, _IOR(0x12, 114, size_t), &b) == 0) t->dev_bytes = b;
    }
    if (t->dev_bytes == 0) {
        snprintf(why, n, "this computer would not say how big its disk is");
        close(t->fd); t->fd = -1;
        return -1;
    }
    return 0;
}

int wr_arm(wr_target *t, wr_kind k, uint64_t lo, uint64_t hi,
           char *why, size_t n)
{
    if (k < 0 || k >= WR_N) { snprintf(why, n, "no such area"); return -1; }
    if (hi <= lo) {
        snprintf(why, n, "%s was given no room", kind_name(k));
        return -1;
    }
    if (hi > t->dev_bytes) {
        snprintf(why, n, "%s would run off the end of the disk", kind_name(k));
        return -1;
    }
    for (int i = 0; i < WR_N; i++) {
        if (i == (int)k || !t->win[i].armed) continue;
        if (lo < t->win[i].hi && t->win[i].lo < hi) {
            /* TWO WINDOWS THAT OVERLAP ARE ONE WINDOW WITH TWO NAMES,
             * and the whole point of naming them is that a write
             * cannot land somewhere its purpose does not describe. */
            snprintf(why, n, "%s and %s would overlap",
                     kind_name(k), kind_name((wr_kind)i));
            return -1;
        }
    }
    t->win[k].lo = lo; t->win[k].hi = hi; t->win[k].armed = 1;
    return 0;
}

void wr_disarm(wr_target *t, wr_kind k)
{ if (k >= 0 && k < WR_N) t->win[k].armed = 0; }

int wr_bytes(wr_target *t, wr_kind k, uint64_t off,
             const void *buf, size_t len, char *why, size_t n)
{
    if (t->fd < 0) { snprintf(why, n, "the disk is not open"); return -1; }
    if (k < 0 || k >= WR_N || !t->win[k].armed) {
        snprintf(why, n, "a write to %s was attempted with nothing armed",
                 kind_name(k));
        return -1;
    }
    if (len == 0) return 0;
    /* Overflow before bounds: off + len can wrap, and a wrapped sum
     * compares as inside any window. */
    if (off > UINT64_MAX - len) {
        snprintf(why, n, "a write was asked for at an impossible place");
        return -1;
    }
    if (off < t->win[k].lo || off + len > t->win[k].hi) {
        /* A REFUSAL, NOT A CLAMP. A clamped write is a write that went
         * somewhere else and said nothing. */
        snprintf(why, n,
                 "a write would have landed outside %s and was refused",
                 kind_name(k));
        return -1;
    }

    const char *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t w = pwrite(t->fd, p + done, len - done, (off_t)(off + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            snprintf(why, n, "this computer's disk would not accept a write "
                             "(%s)", strerror(errno));
            return -1;
        }
        if (w == 0) {
            snprintf(why, n, "this computer's disk stopped accepting writes");
            return -1;
        }
        done += (size_t)w;
    }
    t->written += len;
    t->touched = 1;
    return 0;
}

int wr_check(wr_target *t, uint64_t off, const void *expect, size_t len,
             uint64_t *first_bad)
{
    if (first_bad) *first_bad = 0;
    if (t->fd < 0 || len == 0) return -1;
    enum { CH = 1u << 20 };
    static unsigned char got[CH];
    const unsigned char *want = expect;
    size_t done = 0;
    while (done < len) {
        /* Spelled out in two statements rather than a ternary: the
         * compiler could not otherwise see that `take` is bounded by
         * the buffer, and warned that pread might be handed nine
         * quintillion bytes. A bound the compiler can check is worth
         * more than one only the author can. */
        size_t take = len - done;
        if (take > sizeof got) take = sizeof got;
        size_t g = 0;
        while (g < take) {
            /* `room` is what is provably left in the buffer, computed
             * from the buffer rather than from the request. gcc's
             * fortify check follows that and stops warning; more to
             * the point, so does a reader. */
            size_t room = sizeof got - g;
            size_t want_now = take - g;
            if (want_now > room) want_now = room;
            ssize_t k = pread(t->fd, got + g, want_now, (off_t)(off + done + g));
            if (k <= 0) { if (first_bad) *first_bad = off + done + g; return -1; }
            g += (size_t)k;
        }
        if (memcmp(got, want + done, take) != 0) {
            for (size_t i = 0; i < take; i++)
                if (got[i] != want[done + i]) {
                    if (first_bad) *first_bad = off + done + i;
                    break;
                }
            return -1;
        }
        done += take;
    }
    t->verified += len;
    return 0;
}

int wr_flush(wr_target *t)
{
    if (t->fd < 0) return -1;
    /* fsync, and then the block device's own cache. A commit sitting
     * in a drive's volatile write cache is not a commit, and the
     * drives this product is for are old enough to have one and
     * honest enough to lose it. */
    if (fsync(t->fd) != 0) return -1;
    ioctl(t->fd, _IO(0x12, 97));            /* BLKFLSBUF */
    return 0;
}

/* A SCRATCH FILE IN MEMORY, for the no-stick mode's copy of the way
 * back. Here and not in install.c because creating a file is a writable
 * open, and this is the one file allowed to make one. It may only make
 * one under /run/aurstage/, which in this environment is RAM: nothing
 * here can name a disk, and a path that tries to leave the directory is
 * refused rather than normalised. */
int wr_scratch(const char *path, uint64_t bytes, char *why, size_t n)
{
    static const char dir[] = "/run/aurstage/";
    if (!path || strncmp(path, dir, sizeof dir - 1) != 0 ||
        strstr(path, "..") || strchr(path + sizeof dir - 1, '/')) {
        snprintf(why, n, "refusing to make a scratch file outside %s", dir);
        return -1;
    }
    if ((mkdir("/run", 0755) != 0 && errno != EEXIST) ||
        (mkdir("/run/aurstage", 0700) != 0 && errno != EEXIST)) {
        snprintf(why, n, "there is nowhere in memory to keep the way back");
        return -1;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0) {
        snprintf(why, n, "there is nowhere in memory to keep the way back (%s)",
                 strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        snprintf(why, n, "there is not enough memory to keep the way back");
        return -1;
    }
    close(fd);
    return 0;
}

void wr_close(wr_target *t)
{
    if (t->fd >= 0) { fsync(t->fd); close(t->fd); }
    t->fd = -1;
    for (int i = 0; i < WR_N; i++) t->win[i].armed = 0;
}

uint64_t wr_written(const wr_target *t)  { return t->written; }
uint64_t wr_verified(const wr_target *t) { return t->verified; }
int      wr_untouched(const wr_target *t){ return !t->touched; }
