/* loader.c — see loader.h. Writes through wr.c and nvram.c, never itself. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "loader.h"
#include "nvram.h"
#include "aurstage.h"

static int read_at(int fd, void *b, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = pread(fd, (char *)b + got, n - got, (off_t)(off + got));
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}

int loader_bytes_needed(const image_src *img, uint64_t *need,
                        char *why, size_t n)
{
    uint64_t off = 0, len = 0;
    if (image_esp_extent(img, &off, &len, why, n) != 0) return -1;
    /* A SANITY FLOOR AND CEILING, because this number decides how much
     * of somebody's Windows gets taken. An ESP smaller than a shim and
     * a grub cannot hold the chain it is supposed to hold, and one
     * larger than two gigabytes is a manifest we do not believe. */
    if (len < 8ull * 1024 * 1024 || len > 2ull * 1024 * 1024 * 1024) {
        snprintf(why, n, "the copy of AurOS on the memory stick describes a "
                         "start-up area that is not a believable size");
        return -1;
    }
    *need = len;
    return 0;
}

int loader_write_boot(wr_target *t, const image_src *img,
                      const stage_layout *L, uint32_t sector,
                      void (*progress)(int percent), char *why, size_t n)
{
    enum { CH = 1u << 20, HOLD = 1u << 20 };
    uint64_t eoff = 0, elen = 0;
    if (image_esp_extent(img, &eoff, &elen, why, n) != 0) return -1;

    uint64_t dst = L->rec_first * (uint64_t)sector;
    uint64_t end = (L->rec_last + 1) * (uint64_t)sector;
    if (elen > end - dst) {
        /* Refused rather than truncated. A FAT filesystem cut short is
         * one whose directory entries point past its own end, which is
         * a partition firmware mounts and then cannot read. */
        snprintf(why, n,
                 "the part of this disk set aside to start AurOS from is "
                 "smaller than what has to go in it");
        return -1;
    }
    if (elen <= HOLD) {
        snprintf(why, n, "the copy of AurOS on the memory stick has nothing "
                         "in it to start the computer with");
        return -1;
    }

    int fd = open(img->dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "the AurOS memory stick could not be read");
        return -1;
    }
    static unsigned char buf[CH];
    uint64_t base = image_base_off(img) + eoff;
    int rc = -1, last = -1, armed = 0;

    if (wr_arm(t, WR_RECOVERY, dst, dst + elen, why, n) != 0) goto out;
    armed = 1;

    /* THE FIRST MEGABYTE LAST, the same way the root filesystem is
     * written. Until the end there is no BPB at the start of the
     * partition, so a machine interrupted halfway through has a
     * partition firmware will not mount at all -- rather than one it
     * mounts and reads a half-copied shim out of. */
    memset(buf, 0, HOLD);
    if (wr_bytes(t, WR_RECOVERY, dst, buf, HOLD, why, n) != 0) goto out;

    for (uint64_t at = HOLD; at < elen; ) {
        size_t take = elen - at > CH ? CH : (size_t)(elen - at);
        if (read_at(fd, buf, take, base + at) != 0) {
            snprintf(why, n, "the AurOS memory stick stopped responding "
                             "partway through");
            goto out;
        }
        if (wr_bytes(t, WR_RECOVERY, dst + at, buf, take, why, n) != 0) goto out;
        at += take;
        if (progress) {
            int pct = (int)(at * 50 / elen);
            if (pct != last) { progress(pct); last = pct; }
        }
    }
    if (wr_flush(t) != 0) {
        snprintf(why, n, "this computer's disk would not finish writing");
        goto out;
    }

    /* READ IT ALL BACK, against the stick. R5, and it is the only
     * check there is: nothing here parses FAT, so a wrong byte would
     * otherwise be found by the firmware and by nothing else. */
    for (uint64_t at = HOLD; at < elen; ) {
        size_t take = elen - at > CH ? CH : (size_t)(elen - at);
        if (read_at(fd, buf, take, base + at) != 0) {
            snprintf(why, n, "the AurOS memory stick stopped responding");
            goto out;
        }
        uint64_t bad = 0;
        if (wr_check(t, dst + at, buf, take, &bad) != 0) {
            snprintf(why, n,
                     "what was written to this computer's disk did not read "
                     "back the same, %llu MB in. The drive is failing.",
                     /* INTO THE COPY, not into the disk. wr_check
                      * reports an absolute device offset, and printing
                      * that sends a support engineer to look 475 GB
                      * into somebody's drive for a fault 12 MB into a
                      * partition. */
                     (unsigned long long)((bad - dst) / (1024 * 1024)));
            goto out;
        }
        at += take;
        if (progress) {
            int pct = 50 + (int)(at * 50 / elen);
            if (pct != last) { progress(pct); last = pct; }
        }
    }

    /* AND ONLY NOW THE FIRST MEGABYTE. After this the partition is a
     * filesystem firmware can start from. */
    if (read_at(fd, buf, HOLD, base) != 0) {
        snprintf(why, n, "the AurOS memory stick stopped responding");
        goto out;
    }
    if (wr_bytes(t, WR_RECOVERY, dst, buf, HOLD, why, n) != 0) goto out;
    if (wr_flush(t) != 0) {
        snprintf(why, n, "this computer's disk would not finish writing");
        goto out;
    }
    uint64_t bad = 0;
    if (wr_check(t, dst, buf, HOLD, &bad) != 0) {
        snprintf(why, n, "this computer's disk did not keep what was written "
                         "to the start of the AurOS start-up area");
        goto out;
    }
    rc = 0;
out:
    /* ONLY IF THIS CALL ARMED IT. A failed arm that disarms anyway is
     * a function that can shut somebody else's window, and WR_RECOVERY
     * is no longer the only claim on this extent's neighbourhood. */
    if (armed) wr_disarm(t, WR_RECOVERY);
    close(fd);
    return rc;
}

int loader_register(const gpt_table *nw, const stage_layout *L,
                    uint16_t *entry_out, char *why, size_t n)
{
    /* THE PARTITION AS THE COMMIT LEFT IT, found by its start block.
     * Never by index: gpt_add appends into the first free slot, which
     * is not the last slot on a table somebody has already deleted a
     * partition from. */
    int idx = -1;
    for (uint32_t k = 0; k < nw->n_entries; k++)
        if (gpt_used(&nw->ent[k]) && nw->ent[k].first == L->rec_first &&
            memcmp(nw->ent[k].type, GPT_TYPE_ESP, 16) == 0)
            { idx = (int)k; break; }
    if (idx < 0) {
        snprintf(why, n, "AurOS could not find the part of the disk it just "
                         "made to start from");
        return -1;
    }

    nvram_hd hd;
    memset(&hd, 0, sizeof hd);
    hd.number    = (uint32_t)idx + 1;
    hd.first_lba = nw->ent[idx].first;
    hd.blocks    = nw->ent[idx].last - nw->ent[idx].first + 1;
    memcpy(hd.guid, nw->ent[idx].uuid, 16);

    uint16_t num = 0;
    if (nvram_boot_set(LOADER_ENTRY_DESC, &hd, LOADER_PATH, NULL, &num,
                       why, n) != 0)
        return -1;
    if (entry_out) *entry_out = num;
    /* THE ENTRY IS DOWN. A one-shot that will not arm after that is
     * worth a different sentence: the machine can be started into
     * AurOS from its own boot menu, and telling somebody the install
     * did not reach the menu sends them to do all of it again. */
    if (nvram_boot_next(num, why, n) != 0) return 1;

    /* The entry the Windows half made to get here has done its job.
     * Leaving it means a boot menu with two AurOS lines in it, one of
     * which restarts an installer that will refuse -- correctly, and
     * to somebody who did not want an installer. A failure to remove
     * it is not a failure of the install. */
    char ignored[200];
    nvram_boot_forget(LOADER_INSTALLER_DESC, ignored, sizeof ignored);
    return 0;
}
