/* winvol.c — see winvol.h. Reads only; the mount is MS_RDONLY. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#include "winvol.h"

#define WINVOL_DIR   "/run/aurstage"
#define WINVOL_ROOT  "/run/aurstage/windows"

static int g_mounted;

int winvol_mounted(void) { return g_mounted; }

/* modprobe with a fixed argument vector, the same discipline shrink.c
 * keeps for ntfsresize: nothing a caller passes can become an option. */
static int load_ntfs3(void)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        const char *argv[] = { "/sbin/modprobe", "-q", "ntfs3", NULL };
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

int winvol_mount(const char *windev, char *root, size_t rn,
                 char *why, size_t n)
{
    if (g_mounted) {
        snprintf(root, rn, "%s", WINVOL_ROOT);
        return 0;
    }
    if (load_ntfs3() != 0) {
        snprintf(why, n, "this copy of the installer cannot read the Windows "
                         "drive (the part of it that reads NTFS is missing).");
        return -1;
    }
    if ((mkdir(WINVOL_DIR, 0700) != 0 && errno != EEXIST) ||
        (mkdir(WINVOL_ROOT, 0700) != 0 && errno != EEXIST)) {
        snprintf(why, n, "there was nowhere to look at the Windows drive from.");
        return -1;
    }
    unsigned long fl = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME;
    if (mount(windev, WINVOL_ROOT, "ntfs3", fl, "") != 0) {
        snprintf(why, n, "the Windows drive could not be opened to read the "
                         "copy of AurOS on it (%s). Start Windows, shut it down "
                         "from the Start menu, and try again.", strerror(errno));
        return -1;
    }
    g_mounted = 1;
    /* AND ASK WHETHER IT IS. MS_RDONLY is a request; a kernel that
     * answered it with a writable mount would be a kernel this program
     * has no business trusting with somebody's Windows. */
    struct statvfs sv;
    if (statvfs(WINVOL_ROOT, &sv) != 0 || !(sv.f_flag & ST_RDONLY)) {
        char w2[120];
        winvol_umount(w2, sizeof w2);
        snprintf(why, n, "the Windows drive could not be opened read-only, so "
                         "AurOS did not look at it.");
        return -1;
    }
    snprintf(root, rn, "%s", WINVOL_ROOT);
    return 0;
}

int winvol_umount(char *why, size_t n)
{
    if (!g_mounted) return 0;
    sync();
    if (umount2(WINVOL_ROOT, 0) != 0) {
        snprintf(why, n, "the Windows drive could not be let go of (%s), and "
                         "AurOS will not resize a drive that is still open.",
                 strerror(errno));
        return -1;
    }
    g_mounted = 0;
    return 0;
}
