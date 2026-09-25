/* plat_sim.c — a computer made of ordinary files. See plat.h.
 *
 * WHAT IT IS FOR: running the phase engine, all of it, against the
 * same synthetic machine the staging environment is tested on, so that
 * the memory stick and the journal the installer is given are the ones
 * AurBridge actually produces rather than ones a test wrote to match.
 *
 * WHAT IT IS NOT: a claim that the real thing works. Everything it
 * cannot simulate is a real gap and is listed here rather than in a
 * commit message nobody will find:
 *
 *   - Windows will not always give a raw handle to a disk it has
 *     mounted volumes on, and will not let go of a stick that
 *     Explorer has a window open on. Here, opening a file always
 *     works.
 *   - SetFirmwareEnvironmentVariableExW fails on machines where
 *     SeSystemEnvironmentPrivilege cannot be acquired, and silently
 *     does nothing on a few OEM firmwares. Here it writes a line to a
 *     text file.
 *   - manage-bde, chkdsk and diskpart are not run at all; the command
 *     is written down and success is assumed.
 *
 * A build that uses this says so on every screen, because a simulation
 * that could be mistaken for the real thing is how somebody comes to
 * believe a test result about a machine that was never touched.
 *
 * THE MACHINE, on disk:
 *
 *   $AURBRIDGE_SIM/disks.txt   one line per disk:
 *                              index<TAB>path<TAB>bytes<TAB>sector<TAB>
 *                              serial<TAB>model<TAB>removable
 *   $AURBRIDGE_SIM/esp/        a directory standing in for the ESP
 *   $AURBRIDGE_SIM/efivars.txt one NAME=VALUE per line
 *   $AURBRIDGE_SIM/ran.log     every command the engine would have run
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "plat.h"

#ifdef _WIN32
#  include <io.h>
#  define MKDIR(p) mkdir(p)
#  define OPENFLAGS O_BINARY
#else
#  define MKDIR(p) mkdir((p), 0755)
#  define OPENFLAGS 0
#endif

static int g_allowed = -1;

static const char *simdir(void)
{
    const char *d = getenv("AURBRIDGE_SIM");
    return (d && *d) ? d : "sim";
}

static void simpath(char *out, size_t n, const char *leaf)
{ snprintf(out, n, "%s/%s", simdir(), leaf); }

int plat_is_sim(void) { return 1; }
const char *plat_name(void) { return "a simulated computer made of files"; }

/* ── disks ───────────────────────────────────────────────────────── */

#define SIM_MAX_DISK 16
static plat_disk g_disk[SIM_MAX_DISK];
static char      g_path[SIM_MAX_DISK][1024];
static int       g_ndisk = -1;

static int load_disks(void)
{
    if (g_ndisk >= 0) return g_ndisk;
    g_ndisk = 0;
    char p[600];
    simpath(p, sizeof p, "disks.txt");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[900];
    while (fgets(line, sizeof line, f) && g_ndisk < SIM_MAX_DISK) {
        if (line[0] == '#' || line[0] == '\n') continue;
        plat_disk d;
        memset(&d, 0, sizeof d);
        char path[600] = "", ser[64] = "", mod[128] = "";
        unsigned long long bytes = 0; unsigned sect = 0; int rem = 0, idx = 0;
        /* Tab-separated, because a disk model contains spaces and a
         * serial can too. Reading it with %s split "Samsung SSD 860"
         * across three fields and gave every simulated machine a disk
         * whose serial was "SSD". */
        char *fld[7] = { NULL };
        int nf = 0;
        for (char *s = line, *t; nf < 7; s = NULL) {
            t = strtok(s, "\t\n");
            if (!t) break;
            fld[nf++] = t;
        }
        if (nf < 7) continue;
        idx   = atoi(fld[0]);
        snprintf(path, sizeof path, "%s", fld[1]);
        bytes = strtoull(fld[2], NULL, 10);
        sect  = (unsigned)strtoul(fld[3], NULL, 10);
        snprintf(ser, sizeof ser, "%s", fld[4]);
        snprintf(mod, sizeof mod, "%s", fld[5]);
        rem   = atoi(fld[6]);
        d.index = idx; d.size_bytes = bytes;
        d.logical_sector = sect ? sect : 512;
        snprintf(d.serial, sizeof d.serial, "%s", ser);
        snprintf(d.model,  sizeof d.model,  "%s", mod);
        d.removable = rem;
        /* A relative path is relative to the machine directory, so a
         * simulated machine can be moved without being rewritten. */
        if (path[0] == '/')
            snprintf(g_path[g_ndisk], sizeof g_path[0], "%s", path);
        else
            snprintf(g_path[g_ndisk], sizeof g_path[0], "%s/%s", simdir(), path);
        g_disk[g_ndisk++] = d;
    }
    fclose(f);
    return g_ndisk;
}

static int find_disk(int index)
{
    load_disks();
    for (int i = 0; i < g_ndisk; i++) if (g_disk[i].index == index) return i;
    return -1;
}

int plat_disks(plat_disk *out, int max)
{
    load_disks();
    int n = g_ndisk < max ? g_ndisk : max;
    for (int i = 0; i < n; i++) out[i] = g_disk[i];
    return n;
}

static int io(int index, uint64_t off, void *buf, size_t n, int writing,
              char *why, size_t wn)
{
    int i = find_disk(index);
    if (i < 0) { snprintf(why, wn, "there is no disk %d", index); return -1; }
    int fd = open(g_path[i], (writing ? O_RDWR : O_RDONLY) | OPENFLAGS);
    if (fd < 0) {
        snprintf(why, wn, "disk %d could not be opened: %s",
                 index, strerror(errno));
        return -1;
    }
    char *p = buf;
    int rc = 0;
    while (n) {
        ssize_t k = writing ? pwrite(fd, p, n, (off_t)off)
                            : pread(fd, p, n, (off_t)off);
        if (k <= 0) {
            snprintf(why, wn, "disk %d would not %s at %llu",
                     index, writing ? "take bytes" : "give bytes",
                     (unsigned long long)off);
            rc = -1; break;
        }
        p += k; off += (uint64_t)k; n -= (size_t)k;
    }
    close(fd);
    return rc;
}

int plat_read(int index, uint64_t off, void *buf, size_t n,
              char *why, size_t wn)
{ return io(index, off, buf, n, 0, why, wn); }

void plat_allow_write(int index) { g_allowed = index; }

int plat_write(int index, uint64_t off, const void *buf, size_t n,
               char *why, size_t wn)
{
    if (index != g_allowed) {
        snprintf(why, wn,
                 "REFUSING to write to disk %d. AurBridge only ever writes to "
                 "the memory stick you chose, and that is disk %d.",
                 index, g_allowed);
        return -1;
    }
    return io(index, off, (void *)buf, n, 1, why, wn);
}

int plat_flush(int index, char *why, size_t wn)
{
    int i = find_disk(index);
    if (i < 0) { snprintf(why, wn, "there is no disk %d", index); return -1; }
    int fd = open(g_path[i], O_RDONLY | OPENFLAGS);
    if (fd < 0) { snprintf(why, wn, "disk %d could not be opened", index); return -1; }
#ifndef _WIN32
    int rc = fsync(fd);
#else
    int rc = 0;
#endif
    close(fd);
    if (rc != 0) { snprintf(why, wn, "disk %d would not confirm", index); return -1; }
    return 0;
}

int plat_reread(int index, char *why, size_t wn)
{ (void)index; (void)why; (void)wn; return 0; }

void plat_release(void) { g_allowed = -1; }

/* ── ordinary files ──────────────────────────────────────────────── */

int plat_file_size(const char *path, uint64_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (out) *out = (uint64_t)st.st_size;
    return 0;
}

int plat_file_read(const char *path, uint64_t off, void *buf, size_t n,
                   char *why, size_t wn)
{
    int fd = open(path, O_RDONLY | OPENFLAGS);
    if (fd < 0) { snprintf(why, wn, "%s could not be read", path); return -1; }
    char *p = buf;
    int rc = 0;
    while (n) {
        ssize_t k = pread(fd, p, n, (off_t)off);
        if (k <= 0) { snprintf(why, wn, "%s is shorter than expected", path);
                      rc = -1; break; }
        p += k; off += (uint64_t)k; n -= (size_t)k;
    }
    close(fd);
    return rc;
}

/* Make every directory on the way to a file. */
static void mkparents(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *s = tmp + 1; *s; s++) {
        if (*s != '/' && *s != '\\') continue;
        char c = *s; *s = 0;
        MKDIR(tmp);
        *s = c;
    }
}

int plat_file_put(const char *to, const void *buf, size_t n,
                  char *why, size_t wn)
{
    mkparents(to);
    int fd = open(to, O_WRONLY | O_CREAT | O_TRUNC | OPENFLAGS, 0644);
    if (fd < 0) { snprintf(why, wn, "%s could not be written", to); return -1; }
    const char *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k <= 0) { close(fd); snprintf(why, wn, "%s could not be written", to);
                      return -1; }
        p += k; n -= (size_t)k;
    }
#ifndef _WIN32
    fsync(fd);
#endif
    close(fd);
    return 0;
}

int plat_file_copy(const char *from, const char *to, char *why, size_t wn)
{
    int in = open(from, O_RDONLY | OPENFLAGS);
    if (in < 0) { snprintf(why, wn, "%s could not be read", from); return -1; }
    mkparents(to);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | OPENFLAGS, 0644);
    if (out < 0) { close(in); snprintf(why, wn, "%s could not be written", to);
                   return -1; }
    static char buf[1 << 16];
    int rc = 0;
    for (;;) {
        ssize_t k = read(in, buf, sizeof buf);
        if (k == 0) break;
        if (k < 0) { snprintf(why, wn, "%s could not be read", from); rc = -1; break; }
        ssize_t done = 0;
        while (done < k) {
            ssize_t w = write(out, buf + done, (size_t)(k - done));
            if (w <= 0) { snprintf(why, wn, "%s could not be written", to);
                          rc = -1; break; }
            done += w;
        }
        if (rc) break;
    }
#ifndef _WIN32
    if (rc == 0) fsync(out);
#endif
    close(in); close(out);
    return rc;
}

/* ── the EFI System Partition ────────────────────────────────────── */

int plat_esp_open(char *root, size_t n, char *why, size_t wn)
{
    char p[600];
    simpath(p, sizeof p, "esp");
    MKDIR(p);
    struct stat st;
    if (stat(p, &st) != 0) {
        snprintf(why, wn, "this simulated computer has no EFI partition");
        return -1;
    }
    snprintf(root, n, "%s", p);
    return 0;
}

void plat_esp_close(void) { }

/* ── the firmware ────────────────────────────────────────────────── */

static int var_get(const char *name, char *val, size_t n)
{
    char p[600];
    simpath(p, sizeof p, "efivars.txt");
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[900];
    int found = -1;
    size_t ln = strlen(name);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, name, ln) != 0 || line[ln] != '=') continue;
        char *v = line + ln + 1;
        size_t l = strlen(v);
        while (l && (v[l-1] == '\n' || v[l-1] == '\r')) v[--l] = 0;
        snprintf(val, n, "%s", v);
        found = 0;               /* the LAST one wins, like a rewrite */
    }
    fclose(f);
    return found;
}

static int var_set(const char *name, const char *val)
{
    char p[600];
    simpath(p, sizeof p, "efivars.txt");
    mkparents(p);
    FILE *f = fopen(p, "a");
    if (!f) return -1;
    fprintf(f, "%s=%s\n", name, val ? val : "");
    fclose(f);
    return 0;
}

int plat_boot_find(const char *desc, uint16_t *num_out, char *why, size_t wn)
{
    (void)why; (void)wn;
    for (int i = 0; i < 0x2000; i++) {
        char name[16], val[700];
        snprintf(name, sizeof name, "Boot%04X", i);
        if (var_get(name, val, sizeof val) != 0) continue;
        /* "desc|loader|cmdline" */
        char *bar = strchr(val, '|');
        size_t dl = bar ? (size_t)(bar - val) : strlen(val);
        if (dl == strlen(desc) && strncmp(val, desc, dl) == 0) {
            if (num_out) *num_out = (uint16_t)i;
            return 0;
        }
    }
    return -1;
}

int plat_boot_make(const char *desc, const plat_partition *on,
                   const char *loader, const char *cmdline,
                   uint16_t *num_out, char *why, size_t wn)
{
    if (!on || !on->number || !on->blocks) {
        snprintf(why, wn, "no partition was named for the start-up entry");
        return -1;
    }
    uint16_t num;
    if (plat_boot_find(desc, &num, why, wn) != 0) {
        /* The first free number, searched upward. Reusing a number an
         * entry already has is how somebody's Fedora disappears.
         *
         * 0xFFFF as the sentinel, not 0: starting from 0 meant that a
         * machine with every slot taken silently targeted Boot0000
         * instead of refusing. The real implementation got this right
         * and this one did not, which is the wrong way round for the
         * one that is exercised on every test run. */
        num = 0xFFFF;
        for (int i = 0; i < 0x2000; i++) {
            char name[16], val[700];
            snprintf(name, sizeof name, "Boot%04X", i);
            if (var_get(name, val, sizeof val) != 0) { num = (uint16_t)i; break; }
        }
        if (num == 0xFFFF) {
            snprintf(why, wn, "no room left in the start-up menu");
            return -1;
        }
    }
    char name[16], val[900];
    snprintf(name, sizeof name, "Boot%04X", num);
    /* The partition is in the stored line too, so that the end-to-end
     * test can see the real one rather than a placeholder. */
    snprintf(val, sizeof val, "%s|%s|%s|HD(%u,GPT,%llu,%llu)",
             desc, loader, cmdline ? cmdline : "",
             (unsigned)on->number, (unsigned long long)on->first_lba,
             (unsigned long long)on->blocks);
    if (var_set(name, val) != 0) {
        snprintf(why, wn, "this simulated computer would not take a boot entry");
        return -1;
    }
    if (num_out) *num_out = num;
    return 0;
}

int plat_boot_next(uint16_t num, char *why, size_t wn)
{
    char v[16];
    snprintf(v, sizeof v, "%04X", num);
    if (var_set("BootNext", v) != 0) {
        snprintf(why, wn, "this simulated computer would not take BootNext");
        return -1;
    }
    return 0;
}

int plat_boot_next_clear(char *why, size_t wn)
{
    if (var_set("BootNext", "") != 0) {
        snprintf(why, wn, "BootNext could not be cleared");
        return -1;
    }
    return 0;
}

/* ── running something else ──────────────────────────────────────── */

int plat_run(const char *cmdline, char *tail, size_t n)
{
    char p[600];
    simpath(p, sizeof p, "ran.log");
    mkparents(p);
    FILE *f = fopen(p, "a");
    if (f) { fprintf(f, "%s\n", cmdline); fclose(f); }
    /* NOT RUN. The simulated machine records what would have happened
     * and reports success, and every caller is written knowing that --
     * which is why nothing here decides anything on the strength of a
     * command's output. */
    if (tail && n) snprintf(tail, n, "(not run: simulated machine)");
    return 0;
}

int plat_file_append(const char *to, const void *buf, size_t n,
                     char *why, size_t wn)
{
    FILE *f = fopen(to, "ab");
    if (!f) {
        snprintf(why, wn, "the installer could not finish writing to this "
                          "computer's start-up partition.");
        return -1;
    }
    size_t k = fwrite(buf, 1, n, f);
    int bad = (k != n);
    if (fclose(f) != 0) bad = 1;
    if (bad) {
        snprintf(why, wn, "this computer's start-up partition is full.");
        return -1;
    }
    return 0;
}

/* A simulated computer does not restart; the test does that itself by
 * booting the disk. Written down, like every command, in ran.log. */
int plat_restart(char *why, size_t wn)
{
    (void)why; (void)wn;
    char cmd[] = "restart";
    char tail[8];
    plat_run(cmd, tail, sizeof tail);
    return 0;
}

int plat_file_delete(const char *path, char *why, size_t wn)
{
    if (unlink(path) == 0 || errno == ENOENT) return 0;
    snprintf(why, wn, "%s could not be removed", path);
    return -1;
}

int plat_file_rename(const char *from, const char *to, char *why, size_t wn)
{
    mkparents(to);
    if (rename(from, to) == 0) return 0;
    snprintf(why, wn, "%s could not be moved to %s", from, to);
    return -1;
}

uint64_t plat_free_space(const char *path)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    struct statvfs v;
    if (statvfs(dir, &v) != 0) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}

/* ── what the installer carries inside itself ────────────────────── */
/*
 * There is no PE here and so no resource to read. The simulation --
 * and a developer build of the Windows binary, which is built before
 * there is a staging environment to embed -- take the same two files
 * out of a directory instead, so the phase engine is given a path
 * either way and does not know which world it is in.
 *
 *   $AURBRIDGE_SIM/payload/staging-kernel
 *   $AURBRIDGE_SIM/payload/staging-initrd
 */
int plat_payload_embedded(void)
{
    char p[600];
    simpath(p, sizeof p, "payload/" PAYLOAD_KERNEL);
    if (access(p, R_OK) != 0) return 0;
    simpath(p, sizeof p, "payload/" PAYLOAD_INITRD);
    return access(p, R_OK) == 0;
}

int plat_payload(const char *name, char *path, size_t pn, char *why, size_t wn)
{
    if (strcmp(name, PAYLOAD_KERNEL) && strcmp(name, PAYLOAD_INITRD) &&
        strcmp(name, PAYLOAD_SHIM) && strcmp(name, PAYLOAD_GRUB) &&
        strcmp(name, PAYLOAD_MOKMGR)) {
        snprintf(why, wn, "the installer asked itself for something it does "
                          "not carry.");
        return -1;
    }
    char p[600];
    snprintf(p, sizeof p, "payload/%s", name);
    char full[600];
    simpath(full, sizeof full, p);
    if (access(full, R_OK) != 0) {
        snprintf(why, wn,
                 "this copy of the installer is incomplete -- the part that "
                 "starts your computer is missing from it. Download it "
                 "again.");
        return -1;
    }
    if ((size_t)snprintf(path, pn, "%s", full) >= pn) {
        snprintf(why, wn, "the path for a temporary file was too long.");
        return -1;
    }
    return 0;
}

/* Nothing was unpacked, so there is nothing to remove. Defined rather
 * than left out: a caller that has to ask which world it is in before
 * tidying up is a caller that will forget. */
void plat_payload_free(void) { }

/* ── fetching the image ──────────────────────────────────────────── */
/*
 * HTTP over a plain socket, spoken by hand, and deliberately only the
 * six lines of it this needs. The point is not to be an HTTP client:
 * it is that the RESUME LOGIC -- ask from where we got to, refuse a
 * server that ignores it, never write byte zero of a body into the
 * middle of a file -- is the part that goes wrong, and it is the same
 * code path on both platforms. A test can start a server that answers
 * a Range and one that ignores it, and find out which of those this
 * survives.
 *
 * file:// is here too, because "the image is already on this machine"
 * is the common case and the caller should not have to know.
 */
static int fetch_file_url(const char *url, const char *dest,
                          int (*progress)(uint64_t, uint64_t, void *),
                          void *ud, char *why, size_t wn)
{
    const char *src = url + 7;                  /* file:// */
    FILE *in = fopen(src, "rb");
    if (!in) { snprintf(why, wn, "the copy of AurOS to install could not be "
                                 "found."); return -1; }
    fseek(in, 0, SEEK_END);
    long total = ftell(in);
    fseek(in, 0, SEEK_SET);
    FILE *out = fopen(dest, "wb");
    if (!out) { fclose(in);
                snprintf(why, wn, "AurOS could not write the file it is "
                                  "downloading."); return -1; }
    static char buf[256 * 1024];
    uint64_t got = 0;
    size_t k;
    int rc = 0;
    while ((k = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, k, out) != k) {
            snprintf(why, wn, "this computer ran out of room for the "
                              "download.");
            rc = -1; break;
        }
        got += k;
        if (progress && progress(got, (uint64_t)total, ud)) {
            snprintf(why, wn, "the download was stopped."); rc = -1; break;
        }
    }
    fclose(in); fclose(out);
    return rc;
}

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* One header's value, as a pointer into `head`, or NULL. Case-folded
 * the way HTTP requires, and anchored to the start of a line -- so
 * "X-Content-Length:" is not mistaken for "Content-Length:", which a
 * plain strcasestr does. */
static const char *hdr_of(const char *head, const char *name)
{
    size_t nl = strlen(name);
    const char *p = head;
    /* Skip the status line. */
    p = strchr(p, '\n');
    while (p) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        p = strchr(p, '\n');
    }
    return NULL;
}

/* `bytes <first>-<last>/<total>`. Returns 0 and fills both, or -1.
 *
 * THE FIRST NUMBER IS THE ONE THAT MATTERS, and it was not being read
 * at all: only the status code decided, and a proxy that answers 206
 * with `bytes 0-N/N` whatever it was asked for then had its byte zero
 * written at our offset. The comment above promised this was refused;
 * an adversarial review reproduced a 5120-byte file where the image is
 * 4096. Reading the total and not the start is the whole bug. */
static int parse_range(const char *v, uint64_t *first, uint64_t *total)
{
    if (!v) return -1;
    while (*v == ' ') v++;
    if (strncasecmp(v, "bytes", 5) == 0) v += 5;
    while (*v == ' ') v++;
    char *e = NULL;
    unsigned long long f = strtoull(v, &e, 10);
    if (!e || e == v || *e != '-') return -1;
    const char *sl = strchr(e, '/');
    if (!sl) return -1;
    unsigned long long t = strtoull(sl + 1, NULL, 10);
    *first = (uint64_t)f;
    *total = (uint64_t)t;
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    ssize_t k;
    do { k = read(fd, buf, n); } while (k < 0 && errno == EINTR);
    return (int)k;
}

/* Everything one request does. `*have` is where to continue from and
 * is updated; `*redirect` comes back non-empty when the caller should
 * follow it. Returns 0 on a complete body, 1 on "ask again" (a
 * redirect, or a range the server would not honour), -1 on a refusal. */
static int fetch_once(const char *host, const char *port, const char *path,
                      const char *dest, uint64_t *have, char *redirect,
                      size_t rn,
                      int (*progress)(uint64_t, uint64_t, void *), void *ud,
                      char *why, size_t wn)
{
    redirect[0] = 0;
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) {
        snprintf(why, wn, "AurOS could not reach the place it downloads from.");
        return -1;
    }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        if (fd >= 0) close(fd);
        freeaddrinfo(ai);
        snprintf(why, wn, "AurOS could not reach the place it downloads from. "
                          "Check this computer is online.");
        return -1;
    }
    freeaddrinfo(ai);
    /* A STALLED SERVER MUST NOT BE FOREVER. Without this a connection
     * that goes quiet halfway through five gigabytes leaves the wizard
     * on the same percentage until somebody switches the machine off. */
    {
        struct timeval tv = { 120, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    char req[1600];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AurBridge\r\n"
                      "Accept-Encoding: identity\r\n"
                      "Range: bytes=%llu-\r\nConnection: close\r\n\r\n",
                      path, host, (unsigned long long)*have);
    if (rl <= 0 || (size_t)rl >= sizeof req) {
        close(fd);
        snprintf(why, wn, "that address is too long.");
        return -1;
    }
    {
        int at = 0;
        while (at < rl) {
            ssize_t k = write(fd, req + at, (size_t)(rl - at));
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { close(fd);
                          snprintf(why, wn, "AurOS could not ask for the "
                                            "download."); return -1; }
            at += (int)k;
        }
    }

    char head[8192];
    size_t hn = 0;
    int terminated = 0;
    while (hn + 1 < sizeof head) {
        int k = read_all(fd, head + hn, 1);
        if (k <= 0) break;
        hn++;
        if (hn >= 4 && !memcmp(head + hn - 4, "\r\n\r\n", 4)) { terminated = 1; break; }
    }
    head[hn] = 0;
    /* A HEADER THAT NEVER ENDED IS NOT A HEADER. Without this the loop
     * simply stopped, the status line still parsed, and the rest of the
     * header text was written into the image as though it were body. */
    if (!terminated) {
        close(fd);
        snprintf(why, wn, "the place AurOS downloads from did not answer "
                          "properly.");
        return -1;
    }
    int status = 0;
    if (sscanf(head, "HTTP/1.%*d %d", &status) != 1) {
        close(fd);
        snprintf(why, wn, "the place AurOS downloads from did not answer "
                          "properly.");
        return -1;
    }

    if (status >= 301 && status <= 308 && status != 304 && status != 305) {
        const char *loc = hdr_of(head, "location");
        close(fd);
        if (!loc) {
            snprintf(why, wn, "the place AurOS downloads from sent it "
                              "somewhere and did not say where.");
            return -1;
        }
        size_t i = 0;
        while (loc[i] && loc[i] != '\r' && loc[i] != '\n' && i + 1 < rn) {
            redirect[i] = loc[i]; i++;
        }
        redirect[i] = 0;
        return redirect[0] ? 1 : -1;
    }
    /* 416: our offset is at or past the end of what the server has,
     * which is what a file that is already complete -- and wrong --
     * looks like. Starting again is the answer, and it is the case the
     * retry in ensure_image exists for. */
    if (status == 416) {
        close(fd);
        if (*have == 0) {
            snprintf(why, wn, "the place AurOS downloads from has nothing "
                              "there any more.");
            return -1;
        }
        *have = 0;
        return 1;
    }
    if (status != 200 && status != 206) {
        close(fd);
        snprintf(why, wn, "the place AurOS downloads from answered %d. Try "
                          "again later.", status);
        return -1;
    }
    /* Chunked bodies are not decoded here, and writing the chunk-size
     * lines into somebody's operating system image is worse than
     * saying so. Asking for identity above makes this rare. */
    {
        const char *te = hdr_of(head, "transfer-encoding");
        if (te && strncasecmp(te, "identity", 8) != 0) {
            close(fd);
            snprintf(why, wn, "the place AurOS downloads from is sending it "
                              "in a form AurOS cannot read. Try again later.");
            return -1;
        }
    }

    uint64_t total = 0;
    if (status == 206) {
        uint64_t first = 0;
        if (parse_range(hdr_of(head, "content-range"), &first, &total) != 0) {
            close(fd);
            snprintf(why, wn, "the place AurOS downloads from did not answer "
                              "properly.");
            return -1;
        }
        /* THE SERVER DECIDES WHERE ITS BYTES GO, not us. A start beyond
         * where we are would leave a hole in the file; one before it
         * merely rewrites bytes we already had, which is free. */
        if (first > *have) {
            close(fd);
            snprintf(why, wn, "the place AurOS downloads from sent the wrong "
                              "part of the file. Try again later.");
            return -1;
        }
        *have = first;
    } else {
        /* 200 where a continuation was asked for: the server ignored
         * the Range, so its first byte is byte zero of the file. */
        *have = 0;
        const char *cl = hdr_of(head, "content-length");
        if (cl) total = strtoull(cl, NULL, 10);
    }

    FILE *out = fopen(dest, *have ? "r+b" : "wb");
    if (!out) { close(fd);
                snprintf(why, wn, "AurOS could not write the file it is "
                                  "downloading."); return -1; }
    if (*have && fseeko(out, (off_t)*have, SEEK_SET) != 0) {
        fclose(out); close(fd);
        snprintf(why, wn, "AurOS could not write the file it is downloading.");
        return -1;
    }
    static char buf[256 * 1024];
    int rc = 0;
    for (;;) {
        int k = read_all(fd, buf, sizeof buf);
        if (k < 0) { snprintf(why, wn, "the download stopped partway through. "
                                       "It will carry on from here if you try "
                                       "again."); rc = -1; break; }
        if (k == 0) break;
        if (fwrite(buf, 1, (size_t)k, out) != (size_t)k) {
            snprintf(why, wn, "this computer ran out of room for the "
                              "download."); rc = -1; break;
        }
        *have += (uint64_t)k;
        if (progress && progress(*have, total, ud)) {
            snprintf(why, wn, "the download was stopped."); rc = -1; break;
        }
    }
    /* A CLOSE THAT FAILS IS A WRITE THAT FAILED. A full disk is often
     * only discovered at the flush, and reporting that download as
     * finished is how the hash gets blamed for it. */
    if (fclose(out) != 0 && rc == 0) {
        snprintf(why, wn, "this computer ran out of room for the download.");
        rc = -1;
    }
    close(fd);
    if (rc != 0) return -1;
    /* AND IT HAS TO HAVE ALL ARRIVED. Stopping on end-of-file and
     * reporting success meant a connection cut at 100 bytes of 4096 was
     * a completed download -- and the accusation the user eventually
     * got was that the network had tampered with it. */
    if (total && *have != total) {
        snprintf(why, wn, "the download stopped partway through. It will carry "
                          "on from here if you try again.");
        return -1;
    }
    return 0;
}

int plat_fetch(const char *url, const char *dest,
               int (*progress)(uint64_t got, uint64_t total, void *ud),
               void *ud, char *why, size_t wn)
{
    char here[1200];
    if (strlen(url) + 1 > sizeof here) {
        snprintf(why, wn, "that address is too long.");
        return -1;
    }
    snprintf(here, sizeof here, "%s", url);

    uint64_t have = 0;
    {
        FILE *e = fopen(dest, "rb");
        if (e) { if (fseeko(e, 0, SEEK_END) == 0) {
                     off_t v = ftello(e);
                     if (v > 0) have = (uint64_t)v; }
                 fclose(e); }
    }

    /* Redirects and one restart, bounded. A mirror that redirects to a
     * CDN is ordinary; a loop of them is not. */
    for (int hop = 0; hop < 8; hop++) {
        if (!strncmp(here, "file://", 7))
            return fetch_file_url(here, dest, progress, ud, why, wn);
        if (strncmp(here, "http://", 7)) {
            snprintf(why, wn, "that address is not one AurOS can use.");
            return -1;
        }
        char host[256], port[8] = "80", path[1024];
        const char *h = here + 7;
        const char *slash = strchr(h, '/');
        const char *colon = memchr(h, ':', slash ? (size_t)(slash - h) : strlen(h));
        size_t hl = colon ? (size_t)(colon - h)
                          : (slash ? (size_t)(slash - h) : strlen(h));
        if (hl == 0 || hl >= sizeof host) {
            snprintf(why, wn, "that address is not one AurOS can use.");
            return -1;
        }
        memcpy(host, h, hl); host[hl] = 0;
        if (colon) {
            size_t pl = (slash ? (size_t)(slash - colon - 1) : strlen(colon + 1));
            if (pl == 0 || pl >= sizeof port) {
                snprintf(why, wn, "that address is not one AurOS can use.");
                return -1;
            }
            memcpy(port, colon + 1, pl); port[pl] = 0;
        }
        if ((size_t)snprintf(path, sizeof path, "%s", slash ? slash : "/")
                >= sizeof path) {
            snprintf(why, wn, "that address is too long.");
            return -1;
        }

        char next[1200];
        int rc = fetch_once(host, port, path, dest, &have, next, sizeof next,
                            progress, ud, why, wn);
        if (rc == 0) return 0;
        if (rc < 0) return -1;
        if (next[0]) {
            if (!strncmp(next, "http://", 7) || !strncmp(next, "file://", 7)) {
                snprintf(here, sizeof here, "%s", next);
            } else if (next[0] == '/') {
                snprintf(here, sizeof here, "http://%s%s%s%s", host,
                         strcmp(port, "80") ? ":" : "",
                         strcmp(port, "80") ? port : "", next);
            } else {
                snprintf(why, wn, "the place AurOS downloads from sent it "
                                  "somewhere AurOS cannot follow.");
                return -1;
            }
        }
        /* rc == 1 with no redirect is the 416 restart: same address,
         * from nothing. */
    }
    snprintf(why, wn, "the place AurOS downloads from keeps sending AurOS "
                      "somewhere else.");
    return -1;
}
#endif /* !_WIN32 */
