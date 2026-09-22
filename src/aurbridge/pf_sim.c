/* pf_sim.c — a preflight report for the machine made of files.
 *
 * preflight.c is three thousand lines of Windows: WMI, SMART
 * pass-through, BitLocker, StorageAccessAlignmentProperty. None of it
 * can run here, and none of it is what the phase engine is being
 * tested for. What the phase engine needs from a report is a handful
 * of facts -- which disk Windows is on, where its EFI partition is,
 * where C: starts and how big it is -- and those can be read off a
 * simulated disk honestly, by looking at its partition table, which is
 * exactly what the Windows version does through a different door.
 *
 * WHAT IS DELIBERATELY NOT SIMULATED: a single refusal. Every check
 * this file skips is one preflight does for real, and a report built
 * here has no BLOCKs in it because nothing looked. So a green run
 * against this proves the phase engine works on a machine that passes
 * preflight, and proves nothing whatever about preflight -- which is
 * tested separately, on Windows, by `aurbridge preflight`.
 */
#include <stdio.h>
#include <string.h>

#include "preflight.h"
#include "plat.h"

static uint64_t g64(const uint8_t *p)
{ uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint32_t g32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* EFI System, in GPT's mixed-endian order. */
static const uint8_t ESP_GUID[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };

int pf_is_go(const pf_report *r) { return r && r->n_block == 0; }

void pf_run(pf_report *r)
{
    memset(r, 0, sizeof *r);
    r->is_uefi = 1; r->secure_boot = -1; r->is_admin = 1;
    r->on_ac_power = 1; r->has_battery = 0; r->battery_percent = -1;
    r->system_disk = -1; r->system_volume = -1;
    r->esp_found = -1; r->fde_third_party = 0; r->recovery_disk = -1;

    plat_disk d[PF_MAX_DISKS];
    int nd = plat_disks(d, PF_MAX_DISKS);
    char why[PLAT_WHY];

    for (int i = 0; i < nd && r->n_disks < PF_MAX_DISKS; i++) {
        pf_disk *pd = &r->disks[r->n_disks];
        memset(pd, 0, sizeof *pd);
        pd->index = d[i].index;
        pd->size_bytes = d[i].size_bytes;
        pd->logical_sector = d[i].logical_sector;
        pd->physical_sector = d[i].logical_sector;
        pd->is_removable = d[i].removable;
        pd->removable_media = d[i].removable;
        pd->bus_type = -1;
        pd->smart_ok = 1;
        snprintf(pd->serial, sizeof pd->serial, "%s", d[i].serial);
        snprintf(pd->model,  sizeof pd->model,  "%s", d[i].model);
        pd->partition_style = 2;                    /* RAW until read  */

        uint32_t ss = d[i].logical_sector ? d[i].logical_sector : 512;
        uint8_t hdr[4096];
        if (plat_read(d[i].index, ss, hdr, ss, why, sizeof why) != 0) {
            r->n_disks++;
            continue;
        }
        if (memcmp(hdr, "EFI PART", 8) != 0) { r->n_disks++; continue; }
        pd->partition_style = 1;
        uint64_t elba = g64(hdr + 72);
        uint32_t ne = g32(hdr + 80), es = g32(hdr + 84);
        if (!ne || ne > 512 || es < 128 || es > 4096) { r->n_disks++; continue; }

        for (uint32_t k = 0; k < ne; k++) {
            uint8_t e[512];
            if (es > sizeof e) break;
            if (plat_read(d[i].index, elba * ss + (uint64_t)k * es, e, es,
                          why, sizeof why) != 0) break;
            int used = 0;
            for (int q = 0; q < 16; q++) if (e[q]) { used = 1; break; }
            if (!used) continue;
            uint64_t first = g64(e + 32), last = g64(e + 40);
            if (memcmp(e, ESP_GUID, 16) == 0) {
                pd->esp_offset = first * ss;
                pd->esp_length = (last - first + 1) * ss;
                continue;
            }
            /* NTFS, by what its first sector says it is -- never by
             * the partition type code, which an OEM sets to whatever
             * it likes. */
            uint8_t b[4096];
            if (plat_read(d[i].index, first * ss, b, ss, why, sizeof why) != 0)
                continue;
            if (memcmp(b + 3, "NTFS    ", 8) != 0) continue;
            if (r->n_volumes >= PF_MAX_VOLUMES) continue;
            pf_volume *v = &r->volumes[r->n_volumes];
            memset(v, 0, sizeof *v);
            snprintf(v->mount, sizeof v->mount, "C:");
            snprintf(v->fs, sizeof v->fs, "NTFS");
            v->size_bytes = (last - first + 1) * ss;
            v->disk_index = r->n_disks;
            v->bitlocker = 0; v->dirty = 0; v->hibernated = 0;
            v->shrink_measured = 1;
            v->cluster_bytes = 4096;
            /* The FIRST NTFS volume on a non-removable disk is taken to
             * be Windows. On a real machine preflight knows, because
             * Windows tells it which volume it booted from; here there
             * is nothing to ask, so the first one wins and the machine
             * description is written to make that true. */
            if (!d[i].removable && r->system_volume < 0) {
                r->system_disk   = r->n_disks;
                r->system_volume = r->n_volumes;
                r->system_offset = first * ss;
            }
            r->n_volumes++;
        }
        if (pd->esp_length && r->system_disk == r->n_disks) {
            r->esp_found = 1;
            r->esp_size_bytes = pd->esp_length;
        }
        r->n_disks++;
    }
    if (r->system_disk < 0) {
        pf_result *res = &r->results[r->n++];
        memset(res, 0, sizeof *res);
        snprintf(res->id, sizeof res->id, "no-windows");
        snprintf(res->risk, sizeof res->risk, "-");
        res->sev = PF_BLOCK;
        snprintf(res->title, sizeof res->title, "No Windows drive");
        snprintf(res->detail, sizeof res->detail,
                 "This simulated computer has no NTFS volume on a "
                 "non-removable disk.");
        snprintf(res->remedy, sizeof res->remedy,
                 "Check the machine description.");
        r->n_block++;
    }
}
