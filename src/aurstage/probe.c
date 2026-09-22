/* probe.c — see probe.h. Mounts read-only; writes nothing. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "probe.h"
#include "aurstage.h"

#define PROBE_AT "/probe"

static int run_quiet(const char *const argv[])
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

/* How many entries a /sys class directory has. */
static int count_class(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    struct dirent *e; int n = 0;
    while ((e = readdir(dp))) if (e->d_name[0] != '.') n++;
    closedir(dp);
    return n;
}

static int read_first_line(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    char *nl = strchr(out, '\n'); if (nl) *nl = 0;
    return 0;
}

/* Any wired interface with a carrier. */
static int wired_carrier(const char *sysroot)
{
    char nd[320];
    snprintf(nd, sizeof nd, "%s/class/net", sysroot);
    DIR *dp = opendir(nd);
    if (!dp) return 0;
    struct dirent *e; int up = 0;
    while ((e = readdir(dp)) && !up) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        char p[640], v[32];
        /* Skip wireless: it is counted separately, and a wireless
         * interface with a carrier still needs a network chosen. */
        snprintf(p, sizeof p, "%s/%s/wireless", nd, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0) continue;
        snprintf(p, sizeof p, "%s/%s/carrier", nd, e->d_name);
        if (read_first_line(p, v, sizeof v) == 0 && v[0] == '1') up = 1;
    }
    closedir(dp);
    return up;
}

/* Walk /sys for modaliases and hand each to the IMAGE's modprobe
 * against the IMAGE's modules. Deliberately a copy of boot.c's walk
 * rather than a shared one: that one loads into this environment from
 * the initramfs, this one loads from a mounted root, and the two will
 * drift apart -- the shared version would grow a flag and the flag
 * would eventually be wrong on the destructive path. */
static void probe_modalias(const char *dir, int depth, int *loaded)
{
    if (depth > 12) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        int n = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (n < 0 || n >= (int)sizeof path) continue;
        if (!strcmp(e->d_name, "modalias")) {
            char alias[256];
            if (read_first_line(path, alias, sizeof alias) != 0) continue;
            if (!alias[0]) continue;
            const char *argv[] = { "/sbin/modprobe", "-q",
                                   "-d", PROBE_AT, alias, NULL };
            if (run_quiet(argv) == 0) (*loaded)++;
            continue;
        }
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) probe_modalias(path, depth + 1, loaded);
    }
    closedir(dp);
}

void probe_run(const char *root_dev, probe_result *out)
{
    memset(out, 0, sizeof *out);
    mkdir(PROBE_AT, 0755);

    /* READ-ONLY AND noload. MS_RDONLY alone does not stop ext4
     * replaying its journal, and a replay is a write -- to the
     * filesystem we have just hash-verified, at the one point in the
     * run where the disk is supposed to be exactly what the build
     * made. */
    if (mount(root_dev, PROBE_AT, "ext4", MS_RDONLY, "noload") != 0) {
        out->verdict = PROBE_UNMOUNTABLE;
        snprintf(out->why, sizeof out->why,
                 "The copy of AurOS that was written will not start.");
        snprintf(out->remedy, sizeof out->remedy,
                 "Nothing on this computer has been changed and Windows will "
                 "start as usual. Please send the support file to us.");
        return;
    }

    /* Does the image carry modules for the kernel that is running?
     * Asked FIRST, because every answer below is meaningless if not. */
    struct utsname u;
    char mdir[320];
    if (uname(&u) == 0) {
        snprintf(mdir, sizeof mdir, PROBE_AT "/lib/modules/%s", u.release);
        struct stat st;
        out->kernel_matches = (stat(mdir, &st) == 0 && S_ISDIR(st.st_mode));
    }

    probe_modalias("/sys/devices", 0, &out->modules_loaded);
    probe_look(NULL, out);
    umount(PROBE_AT);
    probe_judge(out);
}

void probe_look(const char *sysroot, probe_result *out)
{
    if (!sysroot) sysroot = "/sys";
    char d[320];
    snprintf(d, sizeof d, "%s/class/ieee80211", sysroot);
    out->wifi_devices = count_class(d);
    snprintf(d, sizeof d, "%s/class/backlight", sysroot);
    out->backlight    = count_class(d);
    snprintf(d, sizeof d, "%s/class/sound", sysroot);
    out->sound_cards  = count_class(d);
    out->wired_up     = wired_carrier(sysroot);
}

void probe_judge(probe_result *out)
{
    /* OUR FAULT FIRST. If nothing loaded, or the image has no modules
     * for this kernel, the hardware has not been tested at all and
     * saying "this computer's WiFi does not work" is a lie about the
     * machine. */
    if (!out->kernel_matches || out->modules_loaded == 0) {
        out->verdict = PROBE_NO_DRIVERS;
        snprintf(out->why, sizeof out->why,
                 "AurOS could not load this computer's drivers from the "
                 "system it just wrote, so it could not test anything.");
        snprintf(out->remedy, sizeof out->remedy,
                 "This is a fault in the AurOS memory stick, not in this "
                 "computer. Nothing has been changed and Windows will start "
                 "as usual.");
        return;
    }

    if (out->wifi_devices == 0 && !out->wired_up) {
        /* THE ABORT THIS PHASE EXISTS FOR. */
        out->verdict = PROBE_NO_NETWORK;
        snprintf(out->why, sizeof out->why,
                 "AurOS cannot get this computer online: it found no "
                 "wireless and no network cable.");
        snprintf(out->remedy, sizeof out->remedy,
                 "Nothing has been changed and Windows will start as usual. "
                 "Plug in a network cable and try again, or send us the "
                 "support file.");
        return;
    }

    out->verdict = PROBE_OK;
    snprintf(out->why, sizeof out->why,
             "%d driver%s loaded; %s%s%s%s",
             out->modules_loaded, out->modules_loaded == 1 ? "" : "s",
             out->wifi_devices ? "wireless works" : "a network cable is in",
             out->backlight   ? ", the screen brightness works" : "",
             out->sound_cards ? ", sound works" : "",
             (!out->backlight || !out->sound_cards) ? "" : "");
}

const char *probe_verdict_name(probe_verdict v)
{
    switch (v) {
    case PROBE_OK:          return "ok";
    case PROBE_NO_NETWORK:  return "no-network";
    case PROBE_NO_DRIVERS:  return "no-drivers";
    case PROBE_UNMOUNTABLE: return "unmountable";
    }
    return "?";
}
