/* boot.c — coming up, without systemd and without udev.
 *
 * The two things this file exists to avoid are the two things a
 * general-purpose init would do for us:
 *
 *   MOUNTING. systemd would bring generators and automounts, and the
 *   one thing this environment must never do is mount the Windows
 *   volume. ntfsresize refuses a mounted volume, and a volume mounted
 *   read-write for an instant is a volume whose journal has changed.
 *   Nothing here mounts anything except the kernel's own pseudo
 *   filesystems.
 *
 *   DRIVERS. udev would load them, and udev is a daemon, a rule
 *   language and a database to carry in an initramfs that is trying to
 *   stay under eighty megabytes. What udev actually does for us is one
 *   thing -- read the modalias the kernel wrote for each device and
 *   ask modprobe for it -- and that is forty lines.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#include "aurstage.h"

/* ── the kernel command line ─────────────────────────────────────── */

static int cmdline_slurp(char *buf, size_t n)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, n - 1);
    close(fd);
    if (k <= 0) return -1;
    buf[k] = 0;
    return 0;
}

int stage_cmdline_has(const char *word)
{
    char buf[4096];
    if (cmdline_slurp(buf, sizeof buf) != 0) return 0;
    return strstr(buf, word) != NULL;
}

int stage_cmdline_value(const char *key, char *out, size_t n)
{
    char buf[4096];
    if (cmdline_slurp(buf, sizeof buf) != 0) return -1;
    char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\n' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return i ? 0 : -1;
}

/* ── saying things ───────────────────────────────────────────────── */

static int log_fd = -1;

static void say_v(const char *tag, const char *fmt, va_list ap)
{
    char line[512];
    /* snprintf RETURNS WHAT IT WOULD HAVE WRITTEN, not what it wrote.
     * Adding the two up and handing the total to write() meant that a
     * message longer than this buffer printed however many bytes of
     * the stack followed it, straight to the console. Nothing reaches
     * 512 characters today; the longest sentence in the program is
     * about 200. That is not a reason, it is luck, and it is one long
     * format string from ending. */
    int n = snprintf(line, sizeof line, "aurstage: %s", tag);
    if (n < 0 || n >= (int)sizeof line) n = (int)sizeof line - 1;
    int k = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    if (k < 0) k = 0;
    n += k;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    if (n < (int)sizeof line - 1) { line[n++] = '\n'; }
    line[n] = 0;
    /* The console first and always: if the log cannot be opened, or
     * the machine dies before it is, the screen is what is left. */
    ssize_t ignored = write(2, line, (size_t)n); (void)ignored;
    if (log_fd >= 0) { ignored = write(log_fd, line, (size_t)n); (void)ignored; }
}

void stage_say(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); say_v("", fmt, ap); va_end(ap); }

void stage_warn(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); say_v("WARNING: ", fmt, ap); va_end(ap); }

/* ── the kernel's own filesystems ────────────────────────────────── */

int stage_mount_pseudo(void)
{
    struct { const char *src, *dst, *type; unsigned long flags; } M[] = {
        { "proc",     "/proc",    "proc",     MS_NOSUID | MS_NOEXEC | MS_NODEV },
        { "sysfs",    "/sys",     "sysfs",    MS_NOSUID | MS_NOEXEC | MS_NODEV },
        { "devtmpfs", "/dev",     "devtmpfs", MS_NOSUID },
        { "tmpfs",    "/run",     "tmpfs",    MS_NOSUID | MS_NODEV },
        /* Boot entries cannot be read or armed from Linux without
         * this, and stage C has to be able to put BootOrder back. It
         * is mounted read-only here: stage A writes nothing, and that
         * includes NVRAM. */
        { "efivarfs", "/sys/firmware/efi/efivars", "efivarfs", MS_RDONLY },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        mkdir(M[i].dst, 0755);
        if (mount(M[i].src, M[i].dst, M[i].type, M[i].flags, NULL) == 0) continue;
        if (errno == EBUSY) continue;                /* already there */
        /* efivarfs is absent on a BIOS machine, which this product
         * refuses anyway -- but it refuses it with a sentence, not by
         * failing to boot. */
        if (!strcmp(M[i].type, "efivarfs")) {
            stage_warn("no EFI variables (%s). This machine may be BIOS-only.",
                       strerror(errno));
            continue;
        }
        stage_warn("could not mount %s (%s)", M[i].dst, strerror(errno));
        bad++;
    }
    return bad == 0;
}

/* ── drivers, the way udev does it, without udev ─────────────────── */

static int run(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Every device the kernel has found has written what it is into
 * `modalias`. Handing each of those to modprobe is the whole of what
 * udev's coldplug does for us. */
static int modalias_walk(const char *dir, int depth, int *loaded)
{
    if (depth > 12) return 0;
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        int n = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (n < 0 || n >= (int)sizeof path) continue;

        if (!strcmp(e->d_name, "modalias")) {
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char alias[256];
            ssize_t k = read(fd, alias, sizeof alias - 1);
            close(fd);
            if (k <= 0) continue;
            alias[k] = 0;
            while (k > 0 && (alias[k - 1] == '\n' || alias[k - 1] == ' '))
                alias[--k] = 0;
            if (!alias[0]) continue;
            const char *argv[] = { "/sbin/modprobe", "-q", alias, NULL };
            if (run(argv) == 0) (*loaded)++;
            continue;
        }
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;   /* symlinks loop forever */
        if (S_ISDIR(st.st_mode)) modalias_walk(path, depth + 1, loaded);
    }
    closedir(dp);
    return 0;
}

int stage_load_modules(void)
{
    int loaded = 0;
    modalias_walk("/sys/devices", 0, &loaded);
    /* The filesystems we will need to READ. Not ntfs3: this
     * environment never mounts the Windows volume, and a module that
     * is not loaded cannot be mounted by accident. */
    static const char *FS[] = { "ext4", "vfat", "nls_iso8859_1", "crc32c" };
    for (size_t i = 0; i < sizeof FS / sizeof FS[0]; i++) {
        const char *argv[] = { "/sbin/modprobe", "-q", FS[i], NULL };
        if (run(argv) == 0) loaded++;
    }
    return loaded;
}

int stage_wait_for_disks(int timeout_ms)
{
    /* A USB or NVMe controller can take seconds to enumerate, and an
     * installer that decided there was no disk because it asked too
     * early is an installer that refuses on perfectly good hardware. */
    struct timespec step = { 0, 200 * 1000 * 1000 };
    for (int waited = 0; waited <= timeout_ms; waited += 200) {
        stage_machine m;
        if (stage_survey(&m) > 0) {
            for (int i = 0; i < m.n_disks; i++)
                if (m.disk[i].n_parts > 0) return 1;
        }
        nanosleep(&step, NULL);
    }
    return 0;
}

/* ── handing over ────────────────────────────────────────────────── */

/* Move the pseudo filesystems across rather than unmounting them: the
 * system we are about to become needs them, and remounting from
 * inside a switch_root that has already thrown away the old root is
 * not something to attempt. */
static void carry_across(const char *from, const char *to)
{
    mkdir(to, 0755);
    if (mount(from, to, NULL, MS_MOVE, NULL) != 0)
        stage_warn("could not carry %s across (%s)", from, strerror(errno));
}

/* Empty the initramfs so its memory is given back, then become the
 * real root. This is the ordinary switch_root dance and the ordinary
 * hazards apply: PID 1 may not have a cwd inside what it is about to
 * delete, and the delete must not cross into the new root. */
static void wipe_initramfs(int root_dev, const char *dir, int depth)
{
    if (depth > 16) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[1024];
        int n = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (n < 0 || n >= (int)sizeof path) continue;
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        /* Never leave the initramfs's own device. The new root is
         * already mounted at /newroot by now, and deleting into it
         * would be deleting the system we are about to run. */
        if ((int)st.st_dev != root_dev) continue;
        if (S_ISDIR(st.st_mode)) {
            wipe_initramfs(root_dev, path, depth + 1);
            rmdir(path);
        } else {
            unlink(path);
        }
    }
    closedir(dp);
}

int stage_switch_root(const char *root_dev)
{
    const char *NEW = "/newroot";
    mkdir(NEW, 0755);

    /* READ-ONLY *AND* noload, AND THE SECOND HALF IS THE POINT.
     *
     * MS_RDONLY does not stop ext4 replaying its journal. A read-only
     * mount of an unclean filesystem recovers it -- the kernel writes
     * to the device -- and `noload` is the option that says do not.
     * So the old comment here was exactly backwards: it claimed a
     * read-only mount was safer than a writable one that "succeeds and
     * replays a journal", when a read-only mount replays it too.
     *
     * This matters beyond tidiness. build/staging's grep gate looks
     * for writable open() flags, and a mount(2) is invisible to it, so
     * nothing would have caught this. And the claim stagetest.sh makes
     * on every case -- the whole disk, byte for byte unchanged -- was
     * only true because the test's root filesystem is always clean.
     * The first power-pull test in stage C would have produced an
     * unclean one and quietly falsified it.
     *
     * An unclean root that we refuse to recover will fail to mount, and
     * that is the right outcome: the installed system's own init
     * recovers it, on purpose, when it is allowed to write. */
    if (mount(root_dev, NEW, "ext4", MS_RDONLY, "noload") != 0) {
        stage_warn("cannot mount %s as the new root (%s)",
                   root_dev, strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat("/newroot/sbin/init", &st) != 0 &&
        stat("/newroot/usr/sbin/init", &st) != 0 &&
        stat("/newroot/lib/systemd/systemd", &st) != 0 &&
        stat("/newroot/usr/lib/systemd/systemd", &st) != 0) {
        stage_warn("%s has no init in it; not handing over", root_dev);
        umount(NEW);
        return -1;
    }
    stage_say("handing over to the system on %s, in this same boot", root_dev);

    carry_across("/dev",  "/newroot/dev");
    carry_across("/proc", "/newroot/proc");
    carry_across("/sys",  "/newroot/sys");
    carry_across("/run",  "/newroot/run");

    if (stat("/", &st) != 0) return -1;
    int initramfs_dev = (int)st.st_dev;

    if (chdir(NEW) != 0) { stage_warn("chdir: %s", strerror(errno)); return -1; }
    wipe_initramfs(initramfs_dev, "/", 0);
    if (mount(".", "/", NULL, MS_MOVE, NULL) != 0) {
        stage_warn("could not move the new root into place (%s)",
                   strerror(errno));
        return -1;
    }
    if (chroot(".") != 0) { stage_warn("chroot: %s", strerror(errno)); return -1; }
    if (chdir("/") != 0)  { stage_warn("chdir: %s", strerror(errno)); return -1; }

    /* Whatever the installed system uses as PID 1. systemd first
     * because that is what this image actually ships; the others are
     * there so a differently-built root is not a dead end. */
    static const char *INITS[] = {
        "/sbin/init", "/usr/sbin/init",
        "/lib/systemd/systemd", "/usr/lib/systemd/systemd", NULL
    };
    for (int i = 0; INITS[i]; i++) {
        if (access(INITS[i], X_OK) != 0) continue;
        char *const argv[] = { (char *)INITS[i], NULL };
        execv(INITS[i], argv);
    }
    stage_warn("nothing in the new root would start as init");
    return -1;
}
