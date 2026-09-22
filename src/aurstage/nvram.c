/* nvram.c — see nvram.h. The only EFI-variable writer in this tree. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvram.h"

/* Overridable ONLY at compile time, and only by the unit test, which
 * builds this file on its own against a directory it made. There is
 * deliberately no way to redirect this at run time: an environment
 * variable that moves where boot entries are written is a way for a
 * machine in the field to end up writing them somewhere else. */
#ifndef NVRAM_DIR
#define NVRAM_DIR "/sys/firmware/efi/efivars"
#endif

/* EFI_GLOBAL_VARIABLE, spelled the way efivarfs spells it in a
 * filename: lowercase, hyphenated, and NOT the mixed-endian byte order
 * the same GUID has inside a partition table. */
#define GLOBAL_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"

#define ATTR_NV_BS_RT 0x00000007u   /* non-volatile, boot, runtime      */

/* linux/fs.h and sys/mount.h have fought over these for years. Two
 * defines are less trouble than the header. */
#define NV_IOC_GETFLAGS  _IOR('f', 1, long)
#define NV_IOC_SETFLAGS  _IOW('f', 2, long)
#define NV_IMMUTABLE_FL  0x00000010L

int nvram_present(void)
{
    struct stat st;
    return stat(NVRAM_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ── read-write for as long as it takes, and not one moment more ──
 *
 * boot.c mounts efivarfs read-only, on purpose: stage A writes
 * nothing and that includes NVRAM. Stage C has two variables to write
 * and no reason to leave the mount writable afterwards, so it is
 * remounted around each batch and put back even when the batch fails.
 *
 * A REMOUNT THAT DOES NOT HAPPEN IS NOT A REASON TO GIVE UP. Some
 * kernels leave efivarfs writable already (nothing in this image
 * mounts it but boot.c, but this program also runs under a test
 * harness and one day under somebody's initramfs), so an EROFS on the
 * write is the thing that decides, not the return of mount(2).
 */
static int rw_depth;

static void nvram_rw(void)
{
    if (rw_depth++ == 0)
        mount("efivarfs", NVRAM_DIR, "efivarfs", MS_REMOUNT, NULL);
}

static void nvram_ro(void)
{
    if (--rw_depth == 0)
        mount("efivarfs", NVRAM_DIR, "efivarfs", MS_REMOUNT | MS_RDONLY, NULL);
}

/* Clear the immutable bit, if the file has one and the filesystem
 * admits to the concept. efivarfs sets it on variables the kernel
 * does not consider removable; Boot#### and BootNext are both on its
 * removable list, so this normally does nothing -- which is exactly
 * why it is here rather than left out. A kernel that disagrees with
 * that list is a kernel on which the write silently fails with EPERM,
 * and EPERM on a boot entry is a machine that comes up in Windows
 * forever with no explanation. */
static void unlock(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    long fl = 0;
    if (ioctl(fd, NV_IOC_GETFLAGS, &fl) == 0 && (fl & NV_IMMUTABLE_FL)) {
        fl &= ~NV_IMMUTABLE_FL;
        ioctl(fd, NV_IOC_SETFLAGS, &fl);
    }
    close(fd);
}

/* One variable, written in ONE write(2). efivarfs requires it: the
 * attributes and the data are a single object to the firmware, and a
 * second write is a second SetVariable call with four bytes of
 * payload. */
static int put_var(const char *name, const void *data, size_t len,
                   char *why, size_t n)
{
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s-%s", NVRAM_DIR, name,
                         GLOBAL_GUID) >= sizeof path) {
        snprintf(why, n, "the name of a start-up setting was too long");
        return -1;
    }
    static uint8_t buf[4096];
    if (len + 4 > sizeof buf) {
        snprintf(why, n, "the AurOS start-up entry is too big for this "
                         "computer's firmware");
        return -1;
    }
    buf[0] = ATTR_NV_BS_RT & 0xFF;
    buf[1] = (ATTR_NV_BS_RT >> 8) & 0xFF;
    buf[2] = (ATTR_NV_BS_RT >> 16) & 0xFF;
    buf[3] = (ATTR_NV_BS_RT >> 24) & 0xFF;
    memcpy(buf + 4, data, len);

    nvram_rw();
    unlock(path);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        snprintf(why, n, "this computer would not let AurOS write a start-up "
                         "entry (%s)", strerror(errno));
        nvram_ro();
        return -1;
    }
    ssize_t k = write(fd, buf, len + 4);
    int e = errno;
    close(fd);
    nvram_ro();
    if (k != (ssize_t)(len + 4)) {
        snprintf(why, n, "this computer's firmware would not take a new "
                         "start-up entry (%s)",
                 k < 0 ? strerror(e) : "it was only partly written");
        return -1;
    }
    return 0;
}

/* Read one back. Returns the data length (attributes stripped) or -1.
 * Used for two things and no others: telling an occupied Boot####
 * slot from a free one, and reading a description to decide whether an
 * entry is ours. */
static int get_var(const char *fname, uint8_t *out, size_t n)
{
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", NVRAM_DIR, fname)
            >= sizeof path)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    uint8_t buf[4096];
    ssize_t k = read(fd, buf, sizeof buf);
    close(fd);
    if (k < 4) return -1;
    size_t len = (size_t)k - 4;
    if (len > n) len = n;
    memcpy(out, buf + 4, len);
    return (int)len;
}

/* ── the load option ─────────────────────────────────────────────── */

/* ASCII to UTF-16LE. Everything this file writes is ASCII by
 * construction -- "AurOS", a path out of a string literal -- so a
 * byte above 0x7F is a programming error rather than a user's name,
 * and it is refused rather than mangled into a boot entry nobody can
 * read. */
static size_t widen(const char *s, uint16_t *out, size_t max)
{
    size_t i = 0;
    for (; s[i]; i++) {
        if (i + 1 >= max) return 0;
        if ((unsigned char)s[i] > 0x7F) return 0;
        out[i] = (uint16_t)(unsigned char)s[i];
    }
    if (i + 1 > max) return 0;
    out[i] = 0;
    return i + 1;               /* characters, including the NUL       */
}

static void put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }

static void put_utf16(uint8_t *p, const uint16_t *w, size_t chars)
{ for (size_t i = 0; i < chars; i++) put16(p + i * 2, w[i]); }

/*
 * SHORT-FORM HARD DRIVE, THEN FILE, THEN END.
 *
 * Byte for byte what src/aurbridge/plat_win.c writes, and the test
 * compares the two. The comment there has the history: a bare
 * MEDIA_FILEPATH_DP is not one of the two short forms UEFI 2.10
 * s10.3.5 requires the boot manager to expand, so LoadImage's
 * LocateDevicePath has no handle to anchor to and returns
 * EFI_NOT_FOUND -- every machine consumes its BootNext, loads nothing
 * and comes back to Windows.
 */
size_t nvram_load_option(uint8_t *out, size_t n, const char *desc,
                         const nvram_hd *on, const char *loader,
                         const char *cmdline)
{
    if (!on || !on->number || !on->blocks) return 0;
    uint16_t wdesc[128], wload[512], wcmd[512];
    size_t dc = widen(desc, wdesc, 128);
    size_t lc = widen(loader, wload, 512);
    size_t cc = (cmdline && *cmdline) ? widen(cmdline, wcmd, 512) : 0;
    if (!dc || !lc || (cmdline && *cmdline && !cc)) return 0;

    size_t dl = dc * 2, fl = lc * 2, cl = cc * 2;
    size_t hd_node = 42;                /* UEFI 2.10 Table 10-13        */
    size_t file_node = 4 + fl;
    size_t end_node = 4;
    size_t dp = hd_node + file_node + end_node;
    size_t total = 6 + dl + dp + cl;
    if (total > n || dp > 0xFFFF || file_node > 0xFFFF) return 0;

    uint8_t *p = out;
    p[0] = 1; p[1] = 0; p[2] = 0; p[3] = 0;     /* LOAD_OPTION_ACTIVE   */
    put16(p + 4, (uint16_t)dp);
    p += 6;
    put_utf16(p, wdesc, dc); p += dl;

    memset(p, 0, hd_node);
    p[0] = 0x04; p[1] = 0x01;                   /* MEDIA / HARDDRIVE    */
    put16(p + 2, (uint16_t)hd_node);
    for (int i = 0; i < 4; i++) p[4 + i]  = (uint8_t)(on->number >> (8 * i));
    for (int i = 0; i < 8; i++) p[8 + i]  = (uint8_t)(on->first_lba >> (8 * i));
    for (int i = 0; i < 8; i++) p[16 + i] = (uint8_t)(on->blocks >> (8 * i));
    memcpy(p + 24, on->guid, 16);
    p[40] = 0x02;                               /* GPT                  */
    p[41] = 0x02;                               /* signature is a GUID  */
    p += hd_node;

    p[0] = 0x04; p[1] = 0x04;                   /* MEDIA / FILEPATH     */
    put16(p + 2, (uint16_t)file_node);
    put_utf16(p + 4, wload, lc);
    p += file_node;

    p[0] = 0x7F; p[1] = 0xFF; p[2] = 4; p[3] = 0;   /* END_ENTIRE       */
    p += end_node;
    if (cl) { put_utf16(p, wcmd, cc); p += cl; }
    return (size_t)(p - out);
}

/* ── the Boot#### namespace ──────────────────────────────────────── */

/* Is this directory entry a global Boot#### variable, and which one?
 * Returns 1 and fills `num`. Deliberately strict: Boot#### is four
 * UPPERCASE hexadecimal digits per UEFI 3.3, and BootOrder, BootNext
 * and BootCurrent all begin with the same four letters. */
static int boot_num_of(const char *fname, unsigned *num)
{
    if (strncmp(fname, "Boot", 4) != 0) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = fname[4 + i];
        if (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') v = v * 16 + (unsigned)(c - 'A' + 10);
        else return 0;
    }
    if (fname[8] != '-') return 0;
    if (strcmp(fname + 9, GLOBAL_GUID) != 0) return 0;
    *num = v;
    return 1;
}

/* The description out of a load option, as ASCII. The layout is
 * attributes(4), device path length(2), then a NUL-terminated UTF-16LE
 * string. A non-ASCII character becomes '?' -- this is only ever
 * compared against our own ASCII descriptions, and mangling somebody
 * else's entry's name into a local variable is harmless where
 * mangling it back onto the machine would not be. */
static void desc_of(const uint8_t *opt, int len, char *out, size_t n)
{
    size_t o = 0;
    for (int i = 6; i + 1 < len && o + 1 < n; i += 2) {
        uint16_t c = (uint16_t)(opt[i] | (opt[i + 1] << 8));
        if (!c) break;
        out[o++] = (c < 0x80) ? (char)c : '?';
    }
    out[o] = 0;
}

/* Walk every Boot#### variable once, calling `fn`. One walk, because
 * opendir on efivarfs is a SetVariable-free enumeration of firmware
 * state and doing it three times is three chances for it to change
 * underneath us. `fn` returns non-zero to stop. */
typedef int (*boot_fn)(unsigned num, const char *fname, const uint8_t *opt,
                       int len, void *ud);

static int walk_boots(boot_fn fn, void *ud, uint8_t *used, size_t used_n)
{
    DIR *d = opendir(NVRAM_DIR);
    if (!d) return -1;
    struct dirent *e;
    int stopped = 0;
    while ((e = readdir(d))) {
        unsigned num;
        if (!boot_num_of(e->d_name, &num)) continue;
        if (used && num / 8 < used_n) used[num / 8] |= (uint8_t)(1u << (num % 8));
        if (stopped || !fn) continue;
        uint8_t opt[4096];
        int len = get_var(e->d_name, opt, sizeof opt);
        if (len < 6) continue;
        if (fn(num, e->d_name, opt, len, ud)) stopped = 1;
    }
    closedir(d);
    return 0;
}

typedef struct { const char *want; int found; unsigned num; } find_ctx;

static int find_cb(unsigned num, const char *fname, const uint8_t *opt,
                   int len, void *ud)
{
    (void)fname;
    find_ctx *c = ud;
    char desc[256];
    desc_of(opt, len, desc, sizeof desc);
    if (strcmp(desc, c->want) != 0) return 0;
    c->found = 1; c->num = num;
    return 1;
}

int nvram_boot_set(const char *desc, const nvram_hd *on, const char *loader,
                   const char *cmdline, uint16_t *num_out,
                   char *why, size_t n)
{
    if (!nvram_present()) {
        snprintf(why, n, "this computer does not let AurOS see its start-up "
                         "settings, so AurOS cannot add itself to the menu.");
        return -1;
    }
    if (!on || !on->number || !on->blocks) {
        snprintf(why, n, "AurOS will not write a start-up entry that names "
                         "no partition.");
        return -1;
    }

    /* One walk: which numbers are taken, and is one of them already
     * ours. */
    uint8_t used[0x2000 / 8];
    memset(used, 0, sizeof used);
    find_ctx fc = { desc, 0, 0 };
    if (walk_boots(find_cb, &fc, used, sizeof used) != 0) {
        snprintf(why, n, "this computer's start-up menu could not be read.");
        return -1;
    }

    unsigned num;
    if (fc.found) {
        num = fc.num;               /* ours from a previous attempt     */
    } else {
        num = 0x10000;
        for (unsigned i = 0; i < 0x2000; i++)
            if (!(used[i / 8] & (1u << (i % 8)))) { num = i; break; }
        if (num > 0xFFFF) {
            snprintf(why, n, "this computer has no room left in its start-up "
                             "menu.");
            return -1;
        }
    }

    static uint8_t opt[2048];
    size_t k = nvram_load_option(opt, sizeof opt, desc, on, loader, cmdline);
    if (!k) {
        snprintf(why, n, "the AurOS start-up entry could not be built.");
        return -1;
    }
    char name[16];
    snprintf(name, sizeof name, "Boot%04X", num);
    if (put_var(name, opt, k, why, n) != 0) return -1;

    /* READ IT BACK. Everything else this installer writes is read back
     * and compared; a boot entry is the one write whose failure is
     * silent and total, so it gets the same treatment. */
    char fname[64];
    snprintf(fname, sizeof fname, "%s-%s", name, GLOBAL_GUID);
    uint8_t back[4096];
    int got = get_var(fname, back, sizeof back);
    if (got != (int)k || memcmp(back, opt, k) != 0) {
        snprintf(why, n, "this computer's firmware did not keep the start-up "
                         "entry AurOS gave it.");
        return -1;
    }
    if (num_out) *num_out = (uint16_t)num;
    return 0;
}

int nvram_boot_next(uint16_t num, char *why, size_t n)
{
    uint8_t v[2] = { (uint8_t)(num & 0xFF), (uint8_t)(num >> 8) };
    if (put_var("BootNext", v, sizeof v, why, n) != 0) return -1;
    uint8_t back[8];
    int got = get_var("BootNext-" GLOBAL_GUID, back, sizeof back);
    if (got != 2 || memcmp(back, v, 2) != 0) {
        snprintf(why, n, "this computer's firmware did not keep what AurOS "
                         "asked it to start next time.");
        return -1;
    }
    return 0;
}

typedef struct { const char *want; char names[8][64]; int n; } forget_ctx;

static int forget_cb(unsigned num, const char *fname, const uint8_t *opt,
                     int len, void *ud)
{
    (void)num;
    forget_ctx *c = ud;
    char desc[256];
    desc_of(opt, len, desc, sizeof desc);
    if (strcmp(desc, c->want) != 0) return 0;
    if (c->n < 8) snprintf(c->names[c->n++], 64, "%s", fname);
    return 0;                       /* every one of them, not the first */
}

int nvram_boot_forget(const char *desc, char *why, size_t n)
{
    if (!nvram_present()) {
        snprintf(why, n, "this computer's start-up settings are not readable.");
        return -1;
    }
    forget_ctx fc; fc.want = desc; fc.n = 0;
    if (walk_boots(forget_cb, &fc, NULL, 0) != 0) {
        snprintf(why, n, "this computer's start-up menu could not be read.");
        return -1;
    }
    int gone = 0;
    nvram_rw();
    for (int i = 0; i < fc.n; i++) {
        char path[512];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", NVRAM_DIR,
                             fc.names[i]) >= sizeof path)
            continue;
        unlock(path);
        if (unlink(path) == 0) gone++;
    }
    nvram_ro();
    if (gone != fc.n) {
        snprintf(why, n, "%d of this computer's old AurOS start-up entries "
                         "could not be removed", fc.n - gone);
        return gone ? gone : -1;
    }
    return gone;
}
