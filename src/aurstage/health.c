/* health.c — see health.h. Reads. Never writes. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "health.h"

static int slurp_one(const char *dir, const char *leaf, char *out, size_t n)
{
    char path[512];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, leaf) >= sizeof path)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    if (k < 0) return -1;
    out[k] = 0;
    while (k > 0 && (out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
    return (int)k;
}

/* ── the power ───────────────────────────────────────────────────── */

void power_read(const char *root, power_state *out)
{
    memset(out, 0, sizeof *out);
    out->percent = -1;
    if (!root) root = "/sys/class/power_supply";

    DIR *dp = opendir(root);
    if (!dp) return;                    /* found stays 0: we cannot tell */
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char dir[640];
        if ((size_t)snprintf(dir, sizeof dir, "%s/%s", root, e->d_name)
                >= sizeof dir)
            continue;

        char type[32] = {0};
        if (slurp_one(dir, "type", type, sizeof type) < 0) continue;
        out->found = 1;

        if (!strcmp(type, "Mains") || !strcmp(type, "USB") ||
            !strcmp(type, "USB_PD") || !strcmp(type, "USB_PD_DRP")) {
            char on[16] = {0};
            /* "online" is 1 when something is actually plugged in.
             * A Mains supply that exists and is offline is a laptop
             * with the charger unplugged, which is exactly the case
             * this is here to catch. */
            if (slurp_one(dir, "online", on, sizeof on) >= 0 && on[0] == '1')
                out->on_mains = 1;
            continue;
        }
        if (!strcmp(type, "Battery")) {
            out->has_battery = 1;
            char cap[16] = {0}, st[24] = {0};
            if (slurp_one(dir, "capacity", cap, sizeof cap) > 0) {
                int v = atoi(cap);
                /* The highest battery, when a machine has two. The one
                 * that runs out first is not the one that matters:
                 * the machine keeps going while ANY of them has
                 * charge. */
                if (v > out->percent) out->percent = v;
            }
            if (slurp_one(dir, "status", st, sizeof st) > 0 &&
                (!strcmp(st, "Charging") || !strcmp(st, "Full")))
                out->charging = 1;
        }
    }
    closedir(dp);
}

int power_ok(const power_state *p, char *why, size_t n)
{
    /* A DESKTOP IS FINE. No battery and no mains supply reported is
     * the ordinary shape of a desktop computer, which cannot run out
     * of charge because it was never on any. Refusing those would
     * refuse most of the machines this product is for. */
    if (!p->found || (!p->has_battery && !p->on_mains)) {
        snprintf(why, n, "This computer is plugged into the wall.");
        return 1;
    }

    if (p->on_mains) {
        snprintf(why, n, "This computer is plugged in.");
        return 1;
    }

    /* R6: not on AC is a refusal on its own. The battery level is in
     * the sentence anyway, because "plug it in" is easier to act on
     * when she can see how little is left. */
    if (p->percent >= 0)
        snprintf(why, n,
                 "This computer is running on its battery (%d%% left). "
                 "The next step cannot be stopped once it starts.",
                 p->percent);
    else
        snprintf(why, n,
                 "This computer is running on its battery. The next step "
                 "cannot be stopped once it starts.");
    return 0;
}

/* ── the drive ───────────────────────────────────────────────────────
 *
 * SMART, read through the kernel's SCSI generic pass-through, which is
 * how a SATA disk behind libata answers an ATA command from user
 * space. The alternative is HDIO_DRIVE_CMD, which libata has been
 * deprecating for a decade and which never worked through a USB
 * bridge; SG_IO with an ATA-12 pass-through works on both.
 *
 * NVMe answers a different question in a different way -- an admin
 * command for the SMART/health log page -- so both are here.
 *
 * WHAT THIS DELIBERATELY IS NOT. It is not smartmontools. It reads
 * four numbers and forms one opinion; it does not decode 200 vendor
 * attributes or pretend to know what a given manufacturer means by
 * attribute 187. The four it reads are the four R5 names, and they are
 * the four that are comparable across drives.
 */

#define SG_IO_ 0x2285
struct sg_io_hdr_ {
    int interface_id; int dxfer_direction; unsigned char cmd_len;
    unsigned char mx_sb_len; unsigned short iovec_count;
    unsigned int dxfer_len; void *dxferp; unsigned char *cmdp;
    unsigned char *sbp; unsigned int timeout; unsigned int flags;
    int pack_id; void *usr_ptr; unsigned char status;
    unsigned char masked_status; unsigned char msg_status;
    unsigned char sb_len_wr; unsigned short host_status;
    unsigned short driver_status; int resid; unsigned int duration;
    unsigned int info;
};

/* An ATA SMART READ DATA, wrapped in a 12-byte ATA pass-through. */
static int ata_smart(int fd, unsigned char page[512])
{
    unsigned char cdb[12] = {
        0xA1,        /* ATA PASS-THROUGH (12)                        */
        0x08,        /* PIO data-in, 512-byte block                  */
        0x0E,        /* t_length=2 (sector count), byt_blok, t_dir=in*/
        0xD0,        /* FEATURES = SMART READ DATA                   */
        0x01,        /* SECTOR COUNT                                 */
        0x00,        /* LBA low                                      */
        0x4F,        /* LBA mid  = 0x4F  \ the SMART signature       */
        0xC2,        /* LBA high = 0xC2  /                           */
        0x00,        /* DEVICE                                       */
        0xB0,        /* COMMAND = SMART                              */
        0x00, 0x00,
    };
    unsigned char sense[32];
    struct sg_io_hdr_ h;
    memset(&h, 0, sizeof h);
    h.interface_id = 'S';
    h.dxfer_direction = -3;             /* SG_DXFER_FROM_DEV          */
    h.cmd_len = sizeof cdb;
    h.mx_sb_len = sizeof sense;
    h.dxfer_len = 512;
    h.dxferp = page;
    h.cmdp = cdb;
    h.sbp = sense;
    h.timeout = 10000;
    if (ioctl(fd, SG_IO_, &h) < 0) return -1;
    if (h.host_status || (h.driver_status & 0x0F)) return -1;
    return 0;
}

/* The SMART data page is 30 attributes of 12 bytes from offset 2.
 * Each is id, flags, current, worst, then a 48-bit raw value. */
static uint64_t attr_raw(const unsigned char *page, int want, int *found)
{
    if (found) *found = 0;
    for (int i = 0; i < 30; i++) {
        const unsigned char *a = page + 2 + i * 12;
        if (a[0] != want) continue;
        uint64_t v = 0;
        for (int b = 5; b >= 0; b--) v = (v << 8) | a[5 + b];
        if (found) *found = 1;
        return v;
    }
    return 0;
}

/* NVMe: the SMART / health information log page (0x02). */
struct nvme_passthru_ {
    uint8_t opcode, flags; uint16_t rsvd1; uint32_t nsid;
    uint32_t cdw2, cdw3; uint64_t metadata; uint64_t addr;
    uint32_t metadata_len, data_len;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
    uint32_t timeout_ms, result;
};
#define NVME_ADMIN_ 0xC0484E41u   /* _IOWR('N', 0x41, struct ...)     */

static int nvme_smart(int fd, unsigned char log[512])
{
    struct nvme_passthru_ c;
    memset(&c, 0, sizeof c);
    c.opcode = 0x02;                      /* GET LOG PAGE             */
    c.nsid = 0xFFFFFFFFu;
    c.addr = (uint64_t)(uintptr_t)log;
    c.data_len = 512;
    c.cdw10 = 0x02 | (((512 / 4) - 1) << 16);   /* LID 2, NUMD        */
    c.timeout_ms = 10000;
    return ioctl(fd, NVME_ADMIN_, &c) < 0 ? -1 : 0;
}

static void smart_judge(smart_state *out);

static uint64_t le48(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 5; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void smart_read(const char *dev, smart_state *out)
{
    memset(out, 0, sizeof *out);
    out->verdict = SMART_UNKNOWN;
    snprintf(out->why, sizeof out->why,
             "This drive does not report its own health.");
    snprintf(out->remedy, sizeof out->remedy,
             "That is common and is not a fault by itself.");

    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;

    unsigned char page[512];
    int have = 0;
    if (ata_smart(fd, page) == 0) {
        close(fd);
        smart_parse_ata(page, out);
        return;
    } else if (nvme_smart(fd, page) == 0) {
        /* The NVMe health log has no per-sector counts: the comparable
         * facts are the critical-warning bits and the media-error
         * count. Bit 2 is "reliability degraded", which is the same
         * statement an ATA pending sector makes. */
        uint8_t crit = page[0];
        out->power_on_hours = le48(page + 128);
        out->uncorrectable  = le48(page + 160);   /* media errors      */
        out->pending        = (crit & 0x04) ? 1 : 0;
        have = 1;
    }
    close(fd);
    if (!have) return;

    out->answered = 1;
    smart_judge(out);
}

/* The four numbers, and the one opinion formed from them. */
static void smart_judge(smart_state *out)
{
    if (out->pending || out->uncorrectable) {
        out->verdict = SMART_FAILING;
        snprintf(out->why, sizeof out->why,
                 "This computer's drive is failing. It has %llu sector%s it "
                 "can no longer read.",
                 (unsigned long long)(out->pending + out->uncorrectable),
                 (out->pending + out->uncorrectable) == 1 ? "" : "s");
        /* THE MOST IMPORTANT SENTENCE IN THIS FILE. She came here to
         * install something; she is leaving with the news that her
         * drive is dying, and the next thing she does decides whether
         * she keeps her photographs. */
        snprintf(out->remedy, sizeof out->remedy,
                 "Do not install anything on it. Copy your photographs and "
                 "documents onto a USB stick or an external drive today, "
                 "and have the drive replaced.");
        return;
    }
    if (out->reallocated) {
        out->verdict = SMART_WORN;
        snprintf(out->why, sizeof out->why,
                 "This drive has had to move %llu sector%s that went bad, "
                 "but nothing is failing right now.",
                 (unsigned long long)out->reallocated,
                 out->reallocated == 1 ? "" : "s");
        snprintf(out->remedy, sizeof out->remedy,
                 "It is worth having a backup. AurOS can still be installed.");
        return;
    }
    out->verdict = SMART_GOOD;
    snprintf(out->why, sizeof out->why,
             "This drive reports no bad sectors (%llu hours of use).",
             (unsigned long long)out->power_on_hours);
    out->remedy[0] = 0;
}

void smart_parse_ata(const unsigned char page[512], smart_state *out)
{
    memset(out, 0, sizeof *out);
    int f;
    out->reallocated    = attr_raw(page, 5,   &f);
    out->pending        = attr_raw(page, 197, &f);
    out->uncorrectable  = attr_raw(page, 198, &f);
    out->power_on_hours = attr_raw(page, 9,   &f);
    out->answered = 1;
    smart_judge(out);
}

const char *smart_verdict_name(smart_verdict v)
{
    switch (v) {
    case SMART_GOOD:    return "good";
    case SMART_FAILING: return "failing";
    case SMART_WORN:    return "worn";
    case SMART_UNKNOWN: return "unknown";
    }
    return "?";
}
