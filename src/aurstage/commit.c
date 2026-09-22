/* commit.c — see commit.h. Writes only through wr.c. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>

#include "commit.h"

int commit_build(const gpt_table *old, const stage_layout *L,
                 gpt_table *out, char *why, size_t n)
{
    *out = *old;
    out->from_backup = 0;

    if (gpt_resize_entry(out, L->win_idx, L->win_last_new) != 0) {
        snprintf(why, n,
                 "the Windows partition could not be made smaller in the "
                 "partition table");
        return -1;
    }
    /* The order they are added in does not matter to the firmware --
     * entries are not required to be sorted -- but adding the root
     * first keeps the numbering stable across the two profiles this
     * has to work for. */
    if (gpt_add(out, GPT_TYPE_LINUX_ROOT, "AUROS-ROOT",
                L->root_first, L->root_last, NULL, why, n) < 0)
        return -1;
    /* The recovery partition is an EFI System partition: it has to be
     * launchable by firmware without anything else on the disk being
     * intact, which is the whole of what it is for. */
    if (gpt_add(out, GPT_TYPE_ESP, "AUROS-RECOVERY",
                L->rec_first, L->rec_last, NULL, why, n) < 0)
        return -1;
    return 0;
}

int commit_table(wr_target *t, const gpt_table *nw,
                 void (*after)(int step, void *ud), void *ud,
                 char *why, size_t n)
{
    size_t ab = gpt_array_bytes(nw);
    uint32_t ss = nw->sector;
    uint8_t *phdr = malloc(ss), *bhdr = malloc(ss);
    uint8_t *arr  = malloc(ab);
    int rc = -1;
    if (!phdr || !bhdr || !arr) {
        snprintf(why, n, "this computer ran out of memory");
        goto out;
    }

    gpt_serialize(nw, 1, phdr, arr);
    gpt_serialize(nw, 0, bhdr, arr);      /* the array is the same     */

    uint64_t alt  = nw->disk_sectors - 1;
    uint64_t barr = alt - (ab + ss - 1) / ss;

    /* 1. THE PRIMARY ENTRY ARRAY. This invalidates the primary header
     *    -- its CRC covers the array -- so from here until step 2
     *    every reader falls back to the backup, which is still the
     *    OLD one. The disk goes on reading exactly as it did. */
    if (wr_bytes(t, WR_GPT_PRIMARY, nw->entry_lba * ss, arr, ab, why, n) != 0)
        goto out;
    if (wr_flush(t) != 0) {
        snprintf(why, n, "this computer's disk would not finish writing");
        goto out;
    }
    if (after) after(1, ud);

    /* 2. AND THE SECTOR. THIS IS THE COMMIT, and it is the whole of
     *    it: one sector, and a sector write is atomic on every real
     *    device. Before it the machine has its old partitions; after
     *    it, its new ones. */
    if (wr_bytes(t, WR_GPT_PRIMARY, 1ull * ss, phdr, ss, why, n) != 0) goto out;
    if (wr_flush(t) != 0) {
        snprintf(why, n, "this computer's disk would not finish writing");
        goto out;
    }
    if (after) after(2, ud);

    /* 3. The backup catches up. A machine that loses power between
     *    step 2 and here has a correct primary and a stale backup,
     *    and every reader prefers the primary. */
    if (wr_bytes(t, WR_GPT_BACKUP, barr * ss, arr, ab, why, n) != 0) goto out;
    if (wr_bytes(t, WR_GPT_BACKUP, alt * ss, bhdr, ss, why, n) != 0) goto out;
    if (wr_flush(t) != 0) {
        snprintf(why, n, "this computer's disk would not finish writing");
        goto out;
    }
    if (after) after(3, ud);

    /* 4. Read it all back. A commit that is only in a write cache is
     *    not a commit, and the drives this product is for are old
     *    enough to have one. */
    uint64_t bad = 0;
    if (wr_check(t, 1ull * ss, phdr, ss, &bad) != 0 ||
        wr_check(t, nw->entry_lba * ss, arr, ab, &bad) != 0 ||
        wr_check(t, alt * ss, bhdr, ss, &bad) != 0 ||
        wr_check(t, barr * ss, arr, ab, &bad) != 0) {
        snprintf(why, n,
                 "the new partition table did not read back the same as it "
                 "was written.");
        goto out;
    }
    rc = 0;
out:
    free(phdr); free(bhdr); free(arr);
    return rc;
}

/* A fixed argument vector, as everywhere a program is run from here.
 * Returns the exit status, or -1. */
static int run_fixed(const char *const argv[], char *tail, size_t tn)
{
    int p[2];
    if (tail && tn) tail[0] = 0;
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1); dup2(p[1], 2);
        if (p[1] > 2) close(p[1]);
        setenv("LC_ALL", "C", 1);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(p[1]);
    size_t got = 0;
    char buf[1024];
    for (;;) {
        ssize_t k = read(p[0], buf, sizeof buf);
        if (k <= 0) break;
        if (tail && tn) {
            for (ssize_t i = 0; i < k; i++) {
                if (got + 1 < tn) tail[got++] = buf[i];
            }
            tail[got] = 0;
        }
    }
    close(p[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int commit_settle(const char *disk_dev, const char *root_dev,
                  char *why, size_t n)
{
    /* 1. Tell the kernel the table changed. BLKRRPART rather than
     *    shelling out to partx: it is one ioctl, it is what partx
     *    itself calls, and it keeps another program out of the
     *    initramfs. */
    int fd = open(disk_dev, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ioctl(fd, _IO(0x12, 95));          /* BLKRRPART */
        close(fd);
    }
    /* The node for the new partition has to appear before anything can
     * be done to it. devtmpfs creates it, and there is no udev here to
     * wait on, so this waits on the file. */
    for (int i = 0; i < 100; i++) {
        if (access(root_dev, F_OK) == 0) break;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (access(root_dev, F_OK) != 0) {
        snprintf(why, n,
                 "this computer did not notice its new layout. AurOS is on "
                 "the disk and Windows still starts; please restart the "
                 "computer.");
        return -1;
    }

    /* 2. e2fsck FIRST. Not a precaution: resize2fs refuses outright
     *    with "Please run 'e2fsck -f' first" whenever s_lastcheck is
     *    older than s_mtime, which is true of every image mkimage
     *    produces, because it makes the filesystem and then mounts it
     *    to fill it. Without this the root never grows -- silently, on
     *    every install. -p is "preen": fix what is safe, refuse what
     *    is not, never ask a question there is nobody to answer. */
    char tail[1024];
    const char *fsck[] = { "/sbin/e2fsck", "-fp", root_dev, NULL };
    int rc = run_fixed(fsck, tail, sizeof tail);
    /* 0 = clean, 1 = fixed something. Anything else is a filesystem we
     * should not be growing. */
    if (rc != 0 && rc != 1) {
        snprintf(why, n,
                 "the new AurOS filesystem did not pass its own check, so it "
                 "was left at the size it was written.");
        return -1;
    }

    /* 3. Grow it into the partition. */
    const char *grow[] = { "/sbin/resize2fs", root_dev, NULL };
    rc = run_fixed(grow, tail, sizeof tail);
    if (rc != 0) {
        /* NOT FATAL, and this is a judgement worth stating: AurOS is
         * written, verified and bootable at the size it came as. A
         * root that did not grow is a smaller disk than the person
         * chose, which is a disappointment; refusing here would
         * instead leave them with a committed partition table and no
         * system, which is a catastrophe. */
        snprintf(why, n,
                 "AurOS was installed but could not be grown to fill the "
                 "space it was given.");
        return 1;
    }
    why[0] = 0;
    return 0;
}
