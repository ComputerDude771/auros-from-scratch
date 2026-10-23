/* probe.c — see probe.h. Mounts read-only; writes nothing. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
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

/* Bring an interface up.
 *
 * WITHOUT THIS THE WHOLE CHECK IS A LIE. Linux returns EINVAL for
 * /sys/class/net/<if>/carrier while the device is down -- the kernel
 * genuinely does not know, because nothing has powered the link up --
 * and this environment has no network manager, no dhcp client and no
 * udev to do it. So the probe reported "no network cable" on a
 * machine with a cable in it, which is the exact wrong answer for the
 * one abort this phase exists for.
 *
 * This is a write, but not to a disk: it sets IFF_UP on an interface,
 * which is what testing whether the machine can get online requires. */
static void link_up(const char *ifname)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct ifreq r;
    memset(&r, 0, sizeof r);
    /* Refused rather than truncated: a truncated interface name is a
     * valid name for a different interface. */
    if (strlen(ifname) >= IFNAMSIZ) { close(s); return; }
    memcpy(r.ifr_name, ifname, strlen(ifname) + 1);
    if (ioctl(s, SIOCGIFFLAGS, &r) == 0 && !(r.ifr_flags & IFF_UP)) {
        r.ifr_flags |= IFF_UP;
        ioctl(s, SIOCSIFFLAGS, &r);
    }
    close(s);
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
        /* Only for the real thing: a synthetic /sys in a test has no
         * interface to bring up, and asking the kernel about one
         * would be asking about the machine running the test. */
        if (!strcmp(nd, "/sys/class/net")) link_up(e->d_name);
        snprintf(p, sizeof p, "%s/%s/carrier", nd, e->d_name);
        if (read_first_line(p, v, sizeof v) == 0 && v[0] == '1') up = 1;
    }
    closedir(dp);
    if (!up && !strcmp(nd, "/sys/class/net")) {
        /* A link that has just been brought up takes a moment to
         * settle -- autonegotiation on real copper is a second or
         * two. Asking once and concluding "no cable" would fail every
         * machine with a gigabit port. */
        for (int tries = 0; tries < 30 && !up; tries++) {
            struct timespec ts = { 0, 200 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            DIR *d2 = opendir(nd);
            if (!d2) break;
            struct dirent *e2;
            while ((e2 = readdir(d2)) && !up) {
                if (e2->d_name[0] == '.' || !strcmp(e2->d_name, "lo")) continue;
                char p2[640], v2[32];
                snprintf(p2, sizeof p2, "%s/%s/wireless", nd, e2->d_name);
                struct stat st2;
                if (stat(p2, &st2) == 0) continue;
                snprintf(p2, sizeof p2, "%s/%s/carrier", nd, e2->d_name);
                if (read_first_line(p2, v2, sizeof v2) == 0 && v2[0] == '1')
                    up = 1;
            }
            closedir(d2);
        }
    }
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

/* ── a loop device, by hand ──────────────────────────────────────────
 *
 * losetup is not in this image and is not worth adding for four
 * ioctls. The loop driver is (build/staging carries
 * kernel/drivers/block), and this is the whole of what losetup does.
 */
struct loop_info64_ {
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t  lo_file_name[64], lo_crypt_name[64], lo_encrypt_key[32];
    uint64_t lo_init[2];
};
#define LOOP_CTL_GET_FREE_ 0x4C82
#define LOOP_SET_FD_       0x4C00
#define LOOP_CLR_FD_       0x4C01
#define LOOP_SET_STATUS64_ 0x4C04

/* Returns the loop device number, or -1. `backing` stays open in the
 * caller until the loop is cleared. */
static int loop_attach(const char *disk, uint64_t off, char *dev, size_t n,
                       int *backing_fd)
{
    *backing_fd = -1;
    /* READ-ONLY, ALL OF IT. The first version opened these O_RDWR
     * because that is what losetup does, and build/staging's write
     * gate stopped the build -- correctly: only wr.c may open
     * something writable. It turns out none of these ioctls needs it.
     * LOOP_CTL_GET_FREE, LOOP_SET_FD and LOOP_SET_STATUS64 all work
     * on a read-only descriptor, and the backing file is opened
     * read-only with LO_FLAGS_READ_ONLY besides, so the loop cannot
     * write to the disk even if something asked it to.
     *
     * The gate found that. It is the second time it has turned a
     * "make the check allow this" into "the check was right". */
    int ctl = open("/dev/loop-control", O_RDONLY | O_CLOEXEC);
    if (ctl < 0) return -1;
    int num = ioctl(ctl, LOOP_CTL_GET_FREE_);
    close(ctl);
    if (num < 0) return -1;
    if ((size_t)snprintf(dev, n, "/dev/loop%d", num) >= n) return -1;

    int lo = open(dev, O_RDONLY | O_CLOEXEC);
    if (lo < 0) return -1;
    int bf = open(disk, O_RDONLY | O_CLOEXEC);
    if (bf < 0) { close(lo); return -1; }
    if (ioctl(lo, LOOP_SET_FD_, bf) < 0) { close(bf); close(lo); return -1; }

    struct loop_info64_ i;
    memset(&i, 0, sizeof i);
    i.lo_offset = off;
    i.lo_flags = 1;                       /* LO_FLAGS_READ_ONLY */
    snprintf((char *)i.lo_file_name, sizeof i.lo_file_name, "%s", disk);
    if (ioctl(lo, LOOP_SET_STATUS64_, &i) < 0) {
        ioctl(lo, LOOP_CLR_FD_, 0);
        close(bf); close(lo);
        return -1;
    }
    close(lo);
    *backing_fd = bf;
    return num;
}

static void loop_detach(const char *dev, int backing_fd)
{
    int lo = open(dev, O_RDONLY | O_CLOEXEC);
    if (lo >= 0) { ioctl(lo, LOOP_CLR_FD_, 0); close(lo); }
    if (backing_fd >= 0) close(backing_fd);
}

void probe_run_at(const char *disk_dev, uint64_t off, probe_result *out)
{
    memset(out, 0, sizeof *out);
    char dev[64]; int bf = -1;
    if (loop_attach(disk_dev, off, dev, sizeof dev, &bf) < 0) {
        out->verdict = PROBE_UNMOUNTABLE;
        snprintf(out->why, sizeof out->why,
                 "AurOS could not open the system it just wrote.");
        snprintf(out->remedy, sizeof out->remedy,
                 "Nothing on this computer has been changed and Windows will "
                 "start as usual.");
        return;
    }
    probe_run(dev, out);
    loop_detach(dev, bf);
}

/* LET GO OF THE NEW ROOT, AND CHECK THAT IT HAPPENED.
 *
 * This used to be a bare umount() whose answer nobody read. A firmware
 * load the poll above triggered can still be in flight -- a driver that
 * asks for a second blob once the first is in -- and holding a file in
 * PROBE_AT, umount() then fails with EBUSY and the new root stays
 * mounted through the loop device. Nothing said so, and two steps later
 * commit_settle() runs e2fsck and resize2fs on that same filesystem
 * while it is still mounted. So: settle and retry for a few seconds, and
 * only if it is still busy detach it lazily (it then vanishes from the
 * namespace and is released when the last reference goes). And the
 * /lib/firmware symlink this run made points into PROBE_AT, so it is
 * removed afterwards -- restored to "not there" -- rather than left
 * dangling into an empty directory for anything later to follow. */
static void probe_letgo(int made_link, probe_result *out)
{
    out->unmounted = 0;
    for (int t = 0; t < 40; t++) {            /* up to ~10 s            */
        if (umount(PROBE_AT) == 0) { out->unmounted = 1; break; }
        if (errno != EBUSY) break;
        struct timespec ts = { 0, 250 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (!out->unmounted && umount2(PROBE_AT, MNT_DETACH) == 0)
        out->unmounted = 2;                   /* detached lazily        */
    if (made_link) {
        char tgt[64];
        ssize_t k = readlink("/lib/firmware", tgt, sizeof tgt - 1);
        if (k > 0) {
            tgt[k] = 0;
            if (!strcmp(tgt, PROBE_AT "/lib/firmware")) unlink("/lib/firmware");
        }
    }
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

    /* POINT THE KERNEL'S FIRMWARE LOADER AT THE NEW ROOT'S FIRMWARE.
     *
     * The initramfs carries none -- linux-firmware is 700 MB to 1.5 GB,
     * against an OEM ESP commonly 100 MB and nearly full -- so the only
     * firmware on this machine is at PROBE_AT "/lib/firmware", in the
     * root we have just written and verified. Without pointing the
     * loader there, iwlwifi / rtw88 / ath10k / mt76 and nearly every
     * modern Wi-Fi chip load their .ko and then NEVER register a radio,
     * because the firmware request that their probe() makes fails
     * silently -- so phase 7 saw "no wireless" on a laptop that has it.
     * That was the Wi-Fi-only blocker the 2026-10-23 audit found, and
     * it is exactly the hardware this product exists for.
     *
     * The documented override is to write PROBE_AT "/lib/firmware" to
     * /sys/module/firmware_class/parameters/path -- but that is a
     * writable open, and build/staging's gate refuses any writable open
     * in this directory but wr.c's and nvram.c's. Rather than widen a
     * safety gate for a shippable build, this uses the mechanism that
     * needs no writable open at all: a symlink at /lib/firmware, which
     * is the kernel's default firmware search path and which this
     * initramfs does not otherwise have. The loader's filp_open()
     * follows it into the new root. A symlink is a tmpfs entry, not a
     * disk write and not an open flag, so the gate is neither tripped
     * nor weakened. If /lib/firmware already exists this is a harmless
     * no-op, and a failure here only means a firmware-needing card is
     * not tested -- which is recorded, never fatal. */
    int made_link = symlink(PROBE_AT "/lib/firmware", "/lib/firmware") == 0;
    /* EEXIST, or no permission: the default search still covers whatever
     * is already at /lib/firmware, and it is not ours to remove later. */

    probe_modalias("/sys/devices", 0, &out->modules_loaded);
    probe_look(NULL, out);
    /* WI-FI REGISTRATION IS ASYNCHRONOUS. A driver returns from probe()
     * and then loads its firmware and registers a PHY a second or two
     * later; asking /sys/class/ieee80211 once, the instant modprobe
     * returns, misses it on real hardware. So poll for a few seconds --
     * the network is recorded and never required (probe_judge), so a
     * few seconds is the whole cost of knowing whether her Wi-Fi came
     * up. Only the real /sys; a test drives probe_look/probe_judge
     * directly against a synthetic one. */
    for (int t = 0; t < 40 && out->wifi_devices == 0; t++) {
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        out->wifi_devices = count_class("/sys/class/ieee80211");
    }
    probe_letgo(made_link, out);
    probe_judge(out);
    if (!out->unmounted) {
        out->verdict = PROBE_UNMOUNTABLE;
        snprintf(out->why, sizeof out->why,
                 "AurOS could not let go of the system it had just checked.");
        snprintf(out->remedy, sizeof out->remedy,
                 "Nothing has been committed and Windows will start as usual.");
    }
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

    /* NETWORK IS RECORDED, NEVER REQUIRED.
     *
     * This used to abort when there was no wireless and no cable -- and
     * that abort came AFTER the shrink, so a Wi-Fi-only laptop with no
     * firmware for its card (the case the symlink above now fixes) was
     * left with a smaller Windows and no AurOS, told to "plug in a
     * network cable". The 2026-10-23 audit found that strands most
     * Wi-Fi-only laptops, which is the hardware this product is for.
     *
     * Two things changed it. The firmware loader is now pointed at the
     * new root (above), so a card that has firmware actually comes up;
     * and the installer carries her Wi-Fi across (docs/AURBRIDGE.md D2),
     * so first boot is online with nothing typed. A machine with no
     * network at all is still a machine she can use -- AurOS installs,
     * starts, and sets the network up from what came across. So the
     * network is measured and written into the record, and it never
     * decides whether the install may go on. PROBE_NO_DRIVERS (above)
     * stays fatal: that is OUR fault, not a limit of her machine. */
    int online = out->wifi_devices > 0 || out->wired_up;
    out->verdict = PROBE_OK;
    snprintf(out->why, sizeof out->why,
             "%d driver%s loaded; %s%s%s",
             out->modules_loaded, out->modules_loaded == 1 ? "" : "s",
             online ? (out->wifi_devices ? "wireless works"
                                         : "a network cable is in")
                    : "no network yet (recorded; AurOS sets it up from your "
                      "Wi-Fi after it starts)",
             out->backlight   ? ", the screen brightness works" : "",
             out->sound_cards ? ", sound works" : "");
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
