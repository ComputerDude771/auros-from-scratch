/* efivar.c — EFI variables, read and written from inside AurOS.
 *
 * See aurfirst.h for why this is a second implementation rather than a
 * shared one. The short form: the staging environment may not write
 * BootOrder and this program's whole job is to write it once, so they
 * do not share the thing the rule is about.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aurfirst.h"

#ifndef AF_DIR_EFIVARS
#define AF_DIR_EFIVARS "/sys/firmware/efi/efivars"
#endif

#define ATTR_NV_BS_RT 0x00000007u

#define AF_IOC_GETFLAGS  _IOR('f', 1, long)
#define AF_IOC_SETFLAGS  _IOW('f', 2, long)
#define AF_IMMUTABLE_FL  0x00000010L

static int path_of(char *out, size_t n, const char *name)
{
    int k = snprintf(out, n, "%s/%s-%s", AF_DIR_EFIVARS, name, AF_GLOBAL_GUID);
    return (k > 0 && (size_t)k < n) ? 0 : -1;
}

/* efivarfs marks variables it does not consider removable immutable.
 * BootOrder, BootNext and Boot#### are all on the kernel's removable
 * list, so this normally does nothing -- which is exactly why it is
 * here. A kernel that disagrees is a kernel on which the write fails
 * with EPERM, and EPERM writing BootOrder means a machine that never
 * becomes the thing its owner just said yes to. */
static void unlock(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    long fl = 0;
    if (ioctl(fd, AF_IOC_GETFLAGS, &fl) == 0 && (fl & AF_IMMUTABLE_FL)) {
        fl &= ~AF_IMMUTABLE_FL;
        ioctl(fd, AF_IOC_SETFLAGS, &fl);
    }
    close(fd);
}

int af_var_get(const char *name, uint8_t *out, size_t n)
{
    char path[512];
    if (path_of(path, sizeof path, name) != 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    uint8_t buf[8192];
    ssize_t k = read(fd, buf, sizeof buf);
    close(fd);
    if (k < 4) return -1;
    size_t len = (size_t)k - 4;
    if (len > n) len = n;
    memcpy(out, buf + 4, len);
    return (int)len;
}

int af_var_put(const char *name, const void *data, size_t len,
               char *why, size_t n)
{
    char path[512];
    if (path_of(path, sizeof path, name) != 0) {
        snprintf(why, n, "the name of a start-up setting was too long");
        return -1;
    }
    /* ONE write(2). The attributes and the data are a single object to
     * the firmware; a second write is a second SetVariable call with
     * four bytes of payload. */
    uint8_t buf[8192];
    if (len + 4 > sizeof buf) {
        snprintf(why, n, "that start-up setting is too big for this "
                         "computer's firmware");
        return -1;
    }
    buf[0] = ATTR_NV_BS_RT & 0xFF;
    buf[1] = (ATTR_NV_BS_RT >> 8) & 0xFF;
    buf[2] = (ATTR_NV_BS_RT >> 16) & 0xFF;
    buf[3] = (ATTR_NV_BS_RT >> 24) & 0xFF;
    memcpy(buf + 4, data, len);

    unlock(path);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        snprintf(why, n, "this computer would not let AurOS change its "
                         "start-up settings (%s)", strerror(errno));
        return -1;
    }
    ssize_t k = write(fd, buf, len + 4);
    int e = errno;
    close(fd);
    if (k != (ssize_t)(len + 4)) {
        snprintf(why, n, "this computer's firmware would not take the new "
                         "start-up setting (%s)",
                 k < 0 ? strerror(e) : "it was only partly written");
        return -1;
    }
    return 0;
}

int af_var_del(const char *name, char *why, size_t n)
{
    char path[512];
    if (path_of(path, sizeof path, name) != 0) return -1;
    if (access(path, F_OK) != 0) return 0;      /* already gone */
    unlock(path);
    if (unlink(path) != 0) {
        snprintf(why, n, "this computer would not let AurOS clear a start-up "
                         "setting (%s)", strerror(errno));
        return -1;
    }
    return 0;
}

/* Boot#### is four UPPERCASE hexadecimal digits per UEFI 3.3.
 * BootOrder, BootNext and BootCurrent all begin with the same four
 * letters, so the check is on the shape of the whole name. */
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
    if (strcmp(fname + 9, AF_GLOBAL_GUID) != 0) return 0;
    *num = v;
    return 1;
}

static int cmp16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

int af_boot_numbers(uint16_t *out, int max)
{
    DIR *d = opendir(AF_DIR_EFIVARS);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        unsigned num;
        if (!boot_num_of(e->d_name, &num)) continue;
        if (n < max) out[n++] = (uint16_t)num;
    }
    closedir(d);
    /* readdir order is the filesystem's, not the firmware's. Sorted,
     * because a rebuilt BootOrder that shuffles somebody's entries
     * every time this runs is a machine whose boot menu changes for no
     * reason. */
    qsort(out, (size_t)n, sizeof *out, cmp16);
    return n;
}

void af_desc_of(const uint8_t *opt, int len, char *out, size_t n)
{
    size_t o = 0;
    for (int i = 6; i + 1 < len && o + 1 < n; i += 2) {
        uint16_t c = (uint16_t)(opt[i] | (opt[i + 1] << 8));
        if (!c) break;
        out[o++] = (c < 0x80) ? (char)c : '?';
    }
    out[o] = 0;
}

int af_efivars_present(void)
{
    struct stat st;
    return stat(AF_DIR_EFIVARS, &st) == 0 && S_ISDIR(st.st_mode);
}

int af_efivars_writable(void)
{
    return access(AF_DIR_EFIVARS, W_OK) == 0;
}
