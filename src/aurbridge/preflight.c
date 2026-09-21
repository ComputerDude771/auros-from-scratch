/* AurBridge preflight — see preflight.h for the design rule.
 * Strictly read-only. R-numbers reference docs/research/red-team.md. */

#include "preflight.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include <winioctl.h>
#include <winsvc.h>

/* ── result recording ────────────────────────────────────────────── */
static void add(pf_report *r, const char *id, const char *risk, pf_severity sev,
                const char *title, const char *detail, const char *remedy)
{
    if (r->n >= PF_MAX_RESULTS) return;
    pf_result *x = &r->results[r->n++];
    snprintf(x->id,     sizeof x->id,     "%s", id);
    snprintf(x->risk,   sizeof x->risk,   "%s", risk ? risk : "");
    snprintf(x->title,  sizeof x->title,  "%s", title);
    snprintf(x->detail, sizeof x->detail, "%s", detail ? detail : "");
    snprintf(x->remedy, sizeof x->remedy, "%s", remedy ? remedy : "");
    x->sev = sev;
    if (sev == PF_BLOCK) r->n_block++;
    else if (sev == PF_WARN) r->n_warn++;
}

/* ── registry helper ─────────────────────────────────────────────── */
static int reg_dword(HKEY root, const char *sub, const char *val, DWORD *out)
{
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0, sz = sizeof(DWORD), rc;
    rc = RegQueryValueExA(k, val, NULL, &type, (BYTE *)out, &sz);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS && type == REG_DWORD;
}

static int reg_key_exists(HKEY root, const char *sub)
{
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(k);
    return 1;
}

/* ── size formatting, for refusal text the user has to read ──────── */
/* A 100 MB start-up partition rendered as "0.1 GB" is a number nobody
 * can act on, so the unit follows the size. */
static void size_str(uint64_t bytes, char *out, size_t n)
{
    double v = (double)bytes;
    const char *u;
    if      (v >= 1073741824.0) { v /= 1073741824.0; u = "GB"; }
    else if (v >= 1048576.0)    { v /= 1048576.0;    u = "MB"; }
    else if (v >= 1024.0)       { v /= 1024.0;       u = "KB"; }
    else { snprintf(out, n, "%llu bytes", (unsigned long long)bytes); return; }
    if (v < 10.0) snprintf(out, n, "%.1f %s", v, u);
    else          snprintf(out, n, "%.0f %s", v, u);
}

/* ── R: elevation ────────────────────────────────────────────────── */
static void check_admin(pf_report *r)
{
    BOOL admin = FALSE;
    PSID grp = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &grp)) {
        CheckTokenMembership(NULL, grp, &admin);
        FreeSid(grp);
    }
    r->is_admin = admin ? 1 : 0;
    if (!admin)
        add(r, "not-elevated", "", PF_BLOCK, "AurBridge is not running as administrator",
            "Reading disk layout and writing a recovery partition both require "
            "administrator rights.",
            "Close this window, right-click AurBridge and choose 'Run as administrator'.");
}

/* ── R15: firmware + Secure Boot ─────────────────────────────────── */
typedef BOOL (WINAPI *PFN_GetFirmwareType)(PFIRMWARE_TYPE);

static void check_firmware(pf_report *r)
{
    r->is_uefi = -1;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    PFN_GetFirmwareType fn = k32 ? (PFN_GetFirmwareType)(void *)
        GetProcAddress(k32, "GetFirmwareType") : NULL;

    if (fn) {
        FIRMWARE_TYPE ft = FirmwareTypeUnknown;
        if (fn(&ft)) r->is_uefi = (ft == FirmwareTypeUefi) ? 1 : 0;
    }
    if (r->is_uefi < 0)
        /* Win7 fallback: the documented probe is that a null-GUID query
         * fails with ERROR_INVALID_FUNCTION on legacy BIOS only. */
        r->is_uefi = reg_key_exists(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State") ? 1 : 0;

    if (r->is_uefi == 1) {
        add(r, "firmware-uefi", "", PF_PASS, "UEFI firmware", "Booted in UEFI mode.", "");
    } else {
        /* R8: Endless OS, better funded, failed for 20-30% of BIOS users
         * doing approximately this. There is no BootNext on MBR, so no
         * self-reverting boot attempt exists. */
        add(r, "firmware-bios", "R8", PF_BLOCK, "This PC uses legacy BIOS boot",
            "On BIOS/MBR systems there is no one-shot, self-reverting boot entry, so "
            "a failed first boot cannot automatically fall back to Windows. Comparable "
            "installers have failed for 20-30% of users on these systems.",
            "AurOS does not support legacy BIOS installs yet. If your PC supports UEFI, "
            "you would need a clean install rather than an upgrade.");
    }

    DWORD sb = 0;
    if (reg_dword(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
        "UEFISecureBootEnabled", &sb))
        r->secure_boot = sb ? 1 : 0;
    else
        r->secure_boot = -1;

    if (r->secure_boot == 1)
        add(r, "secure-boot-on", "R15", PF_WARN, "Secure Boot is enabled",
            "AurOS boots through a Microsoft-signed shim, but the first boot asks you to "
            "approve AurOS's key on a blue setup screen.",
            "No action now. We will show you exactly which buttons to press, with photos, "
            "before you restart.");
}

/* ── R6: power ───────────────────────────────────────────────────── */
/* The three states worth distinguishing, plus the one that used to be
 * folded into "on battery" and should never have been:
 *
 *   AC          plugged in. Proceed.
 *   BATTERY     genuinely running on a battery. Refuse (R6).
 *   NO_BATTERY  ACLineStatus is unknown but this machine has no battery
 *               at all. A desktop cannot run out of battery, so refusing
 *               it buys nothing and costs a user. Some desktops report
 *               255 here; treating that as "on battery" is a refusal
 *               with a false reason attached.
 *   UNKNOWN     we cannot tell. Unknown is never OK: refuse, but say so
 *               honestly rather than claiming the PC is on battery.
 */
enum { PF_PWR_AC = 0, PF_PWR_BATTERY, PF_PWR_NO_BATTERY, PF_PWR_UNKNOWN };

static int power_verdict(BYTE ac, BYTE flag, BYTE pct, int *on_ac,
                         int *has_battery, int *battery_pct)
{
    /* BatteryFlag 255 means "status unknown", and 255 has bit 128 set,
     * so a bare (flag & 128) test reads unknown as "no battery" — the
     * opposite of the safe answer. 128 only counts when the byte is a
     * real flag word. */
    int flag_known = (flag != 255);
    int no_battery = flag_known && (flag & 128);

    *has_battery  = flag_known ? (no_battery ? 0 : 1) : -1;
    *battery_pct  = (flag_known && !no_battery && pct != 255) ? (int)pct : -1;

    if (ac == 1) { *on_ac = 1; return PF_PWR_AC; }
    if (ac == 0) {
        *on_ac = 0;
        /* "Not on mains" with no battery fitted describes a machine that
         * cannot be running. The report contradicts itself, so neither
         * half of it is trustworthy. */
        return no_battery ? PF_PWR_UNKNOWN : PF_PWR_BATTERY;
    }
    *on_ac = -1;                                   /* ACLineStatus 255 */
    return no_battery ? PF_PWR_NO_BATTERY : PF_PWR_UNKNOWN;
}

static void check_power(pf_report *r)
{
    SYSTEM_POWER_STATUS ps;
    r->battery_percent = -1;
    r->on_ac_power = -1;
    r->has_battery = -1;

    if (!GetSystemPowerStatus(&ps)) {
        /* This used to return quietly, which let a machine whose power
         * state we could not read walk straight past the R6 gate. */
        add(r, "power-state-unknown", "R6", PF_BLOCK,
            "AurBridge cannot tell how this PC is powered",
            "Windows did not report a power status, so AurBridge cannot confirm the PC "
            "is on mains power. Losing power partway through changing the disk layout "
            "can leave the disk in a state where neither Windows nor AurOS can start.",
            "Make sure the PC is plugged into the wall and click Re-check. If this keeps "
            "happening, this PC's power reporting is broken and AurOS will not install "
            "on it.");
        return;
    }

    int verdict = power_verdict(ps.ACLineStatus, ps.BatteryFlag, ps.BatteryLifePercent,
                                &r->on_ac_power, &r->has_battery, &r->battery_percent);

    switch (verdict) {
    case PF_PWR_BATTERY:
        /* The partition table is not journalled. Power loss between data
         * movement and the table write cross-shreds the disk. */
        add(r, "not-on-ac", "R6", PF_BLOCK, "This PC is running on battery",
            "Losing power partway through changing the disk layout can leave the disk in "
            "a state where neither Windows nor AurOS can start.",
            "Plug in the charger, then click Re-check.");
        break;

    case PF_PWR_UNKNOWN:
        add(r, "power-state-unknown", "R6", PF_BLOCK,
            "AurBridge cannot tell whether this PC is plugged in",
            "Windows reports this PC's mains power as unknown, and it does have a "
            "battery. Losing power partway through changing the disk layout can leave "
            "the disk in a state where neither Windows nor AurOS can start.",
            "Check that the charger is plugged in at both ends and that the charging "
            "light is on, then click Re-check.");
        break;

    case PF_PWR_NO_BATTERY:
        /* Not a pass and not an obstacle: it explains why the battery
         * checks below did not run, and it is the honest reading of a
         * desktop that reports ACLineStatus 255. */
        add(r, "no-battery", "R6", PF_INFO, "This PC has no battery",
            "This PC has no battery fitted, so it runs on mains power only. There is "
            "nothing here that can run flat mid-install.",
            "Nothing to do. If you own an uninterruptible power supply, this is a good "
            "time to use it.");
        break;

    default:
        if (r->battery_percent >= 0 && r->battery_percent < 50)
            add(r, "battery-low", "R6", PF_BLOCK, "Battery is below 50%",
                "If the power cable is pulled out mid-install, the battery must be able "
                "to carry the machine to a safe stopping point.",
                "Leave it charging until the battery reaches at least 50%, then click "
                "Re-check.");
        break;
    }
}

/* ── pending servicing reboot ────────────────────────────────────── */
static void check_pending_reboot(pf_report *r)
{
    int pending = 0;
    if (reg_key_exists(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired"))
        pending = 1;
    if (reg_key_exists(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending"))
        pending = 1;

    if (pending)
        add(r, "pending-reboot", "", PF_BLOCK, "Windows has updates waiting for a restart",
            "Installing now would race a half-finished Windows update across the same disk.",
            "Restart Windows, let it finish updating, then run AurBridge again.");
}

/* ── R2: Fast Startup / hibernation ──────────────────────────────── */
static void check_fast_startup(pf_report *r)
{
    DWORD hiberboot = 0;
    int fast_on = reg_dword(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        "HiberbootEnabled", &hiberboot) && hiberboot != 0;

    char sysdrive[8] = "C:";
    GetEnvironmentVariableA("SystemDrive", sysdrive, sizeof sysdrive);

    char hib[MAX_PATH];
    snprintf(hib, sizeof hib, "%s\\hiberfil.sys", sysdrive);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    int hibfile = GetFileAttributesExA(hib, GetFileExInfoStandard, &fad) ? 1 : 0;

    if (fast_on || hibfile) {
        /* If Windows later resumes from a hibernation image taken before
         * we moved the partitions, it writes back a stale NTFS metadata
         * and partition map. Both operating systems are destroyed, and
         * silently. This is the single nastiest failure in the register. */
        add(r, "fast-startup", "R2", PF_BLOCK, "Fast Startup is switched on",
            "With Fast Startup, 'Shut down' does not fully shut down -- Windows saves its "
            "session to disk. If Windows later restores that saved session after the disk "
            "layout has changed, it can overwrite the new layout and damage both systems.",
            "AurBridge can switch Fast Startup off for you (powercfg /h off) and restart "
            "Windows once. This is quick, reversible, and must happen before anything else.");
    }
}

/* ── partition type GUIDs we have to recognise ───────────────────── */
static const GUID PF_GPT_ESP =
    { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B } };
/* R10: the two halves of a GPT dynamic disk. The MBR world announces
 * itself with partition type 0x42; the GPT world uses these instead, and
 * checking only 0x42 misses every dynamic disk made this decade. */
static const GUID PF_GPT_LDM_DATA =
    { 0xAF9B60A0, 0x1431, 0x4F62, { 0xBC,0x68,0x33,0x11,0x71,0x4A,0x69,0xAD } };
static const GUID PF_GPT_LDM_META =
    { 0x5808C8AA, 0x7E8F, 0x42E0, { 0x85,0xD2,0xE1,0xE9,0x04,0x34,0xCF,0xB3 } };
/* R10: a disk that is a member of a Storage Spaces pool carries this
 * type on the protective partition covering the whole disk. */
static const GUID PF_GPT_SPACES =
    { 0xE75CAF8F, 0xF680, 0x4CEE, { 0xAF,0xA3,0xB0,0x01,0xE5,0x6E,0xFC,0x2D } };

static int guid_eq(const GUID *a, const GUID *b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

/* ── boot-sector classification ──────────────────────────────────── */
/* AURBRIDGE.md, MUST NOT: never move, truncate or resize a partition
 * whose first sector carries "-FVE-FS-" at offset 3. Everything that
 * looks at a first sector goes through here so that test exists in
 * exactly one place. */
enum { PF_BS_NTFS = 0, PF_BS_FVE, PF_BS_FAT, PF_BS_OTHER };

static int boot_sector_kind(const BYTE *sec)
{
    if (!memcmp(sec + 3, "NTFS    ", 8)) return PF_BS_NTFS;
    if (!memcmp(sec + 3, "-FVE-FS-", 8)) return PF_BS_FVE;
    if (!memcmp(sec + 54, "FAT12   ", 8) || !memcmp(sec + 54, "FAT16   ", 8) ||
        !memcmp(sec + 54, "FAT     ", 8) || !memcmp(sec + 82, "FAT32   ", 8))
        return PF_BS_FAT;
    return PF_BS_OTHER;
}

static int mem_find(const BYTE *hay, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    if (m == 0 || n < m) return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (hay[i] == (BYTE)needle[0] && !memcmp(hay + i, needle, m)) return 1;
    return 0;
}

/* Read one aligned run of sectors. Offsets and lengths on a raw device
 * handle must be a multiple of the logical sector size; 4096 is a
 * multiple of every logical sector size in use, and GPT partitions are
 * aligned far coarser than that, so a 4 KB read is always legal. */
static int read_at(HANDLE h, uint64_t offset, BYTE *buf, DWORD len)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
    DWORD got = 0;
    if (!ReadFile(h, buf, len, &got, NULL) || got < len) return 0;
    return 1;
}

/* ── disk enumeration ────────────────────────────────────────────── */
static void desc_string(BYTE *buf, DWORD off, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!off) return;
    const char *s = (const char *)(buf + off);
    size_t i = 0, j = 0;
    while (s[i] == ' ') i++;                    /* vendor strings are padded */
    while (s[i] && j < outsz - 1) out[j++] = s[i++];
    while (j > 0 && out[j-1] == ' ') j--;
    out[j] = '\0';
}

/* R11: "removable" has to mean removable *bus*, not removable *media*.
 * STORAGE_DEVICE_DESCRIPTOR.RemovableMedia describes a device whose
 * medium can be swapped -- a card reader, an optical drive. A USB hard
 * drive reports FALSE, which is exactly the disk the guard exists for:
 * the external backup drive with fifteen years of photos on it. */
static int bus_is_removable(int bus)
{
    switch (bus) {
        case BusTypeUsb:                 /* 0x07 */
        case BusType1394:                /* 0x04 — IEEE 1394 / FireWire */
        case BusTypeSd:                  /* 0x0C */
        case BusTypeMmc:                 /* 0x0D */
            return 1;
        default:
            return 0;
    }
}

static void query_disk_props(HANDLE h, pf_disk *d)
{
    d->bus_type = -1;

    STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery, {0} };
    BYTE buf[2048]; DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                        buf, sizeof buf, &ret, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *sd = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        d->removable_media = sd->RemovableMedia ? 1 : 0;
        if (ret >= offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength) &&
            sd->BusType != BusTypeUnknown)
            d->bus_type = (int)sd->BusType;
        char vendor[64] = "", product[96] = "";
        desc_string(buf, sd->VendorIdOffset,  vendor,  sizeof vendor);
        desc_string(buf, sd->ProductIdOffset, product, sizeof product);
        desc_string(buf, sd->SerialNumberOffset, d->serial, sizeof d->serial);
        snprintf(d->model, sizeof d->model, "%s%s%s",
                 vendor, (vendor[0] && product[0]) ? " " : "", product);
    }

    /* The adapter descriptor is the second opinion. Some drivers leave
     * BusType unset on the device descriptor but fill it here. */
    if (d->bus_type < 0) {
        STORAGE_PROPERTY_QUERY aq = { StorageAdapterProperty, PropertyStandardQuery, {0} };
        BYTE abuf[1024]; DWORD aret = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &aq, sizeof aq,
                            abuf, sizeof abuf, &aret, NULL)) {
            STORAGE_ADAPTER_DESCRIPTOR *ad = (STORAGE_ADAPTER_DESCRIPTOR *)abuf;
            if (aret >= offsetof(STORAGE_ADAPTER_DESCRIPTOR, BusMajorVersion) &&
                ad->BusType != BusTypeUnknown)
                d->bus_type = (int)ad->BusType;
        }
    }

    d->is_removable = (d->bus_type >= 0 && bus_is_removable(d->bus_type))
                      || d->removable_media;

    /* AURBRIDGE.md: shrink takes sectors, partition structures take
     * bytes, NTFS allocates in clusters. Hard-coding 512 on a 4Kn disk
     * makes the partition 8x too small, so both numbers are read and a
     * missing answer is a block, not a default. */
    STORAGE_PROPERTY_QUERY sq = { StorageAccessAlignmentProperty, PropertyStandardQuery, {0} };
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR al;
    DWORD sret = 0;
    memset(&al, 0, sizeof al);
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &sq, sizeof sq,
                        &al, sizeof al, &sret, NULL) && sret >= sizeof al) {
        d->logical_sector  = al.BytesPerLogicalSector;
        d->physical_sector = al.BytesPerPhysicalSector;
    }
    if (!d->logical_sector) {
        /* Geometry knows the logical sector size even where the storage
         * stack declines the alignment descriptor. It says nothing about
         * the physical one, which is why this is a fallback and not the
         * answer. */
        DISK_GEOMETRY_EX gx;
        DWORD gret = 0;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                            &gx, sizeof gx, &gret, NULL))
            d->logical_sector = gx.Geometry.BytesPerSector;
    }

    GET_LENGTH_INFORMATION li;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &ret, NULL))
        d->size_bytes = (uint64_t)li.Length.QuadPart;
}

/* A sector size we are willing to do arithmetic with. */
static int sector_size_ok(uint32_t s)
{
    return s >= 512 && s <= 65536 && (s & (s - 1)) == 0;
}

/* Reads what the partition table says. Emits nothing: a dynamic disk
 * can carry several LDM partitions, and reporting from in here once
 * produced one identical refusal card per partition.
 *
 * Split from the ioctl so the classification can be tested against a
 * layout we build by hand -- a GPT dynamic disk and a Storage Spaces
 * pool member are not things a test machine can be talked into being. */
static void scan_layout(const DRIVE_LAYOUT_INFORMATION_EX *lay, pf_disk *d)
{
    d->partition_style = (lay->PartitionStyle == PARTITION_STYLE_GPT) ? 1 :
                         (lay->PartitionStyle == PARTITION_STYLE_MBR) ? 0 : 2;

    if (lay->PartitionStyle == PARTITION_STYLE_MBR) {
        int primaries = 0;
        for (DWORD i = 0; i < lay->PartitionCount && i < 128; i++) {
            const PARTITION_INFORMATION_EX *p = &lay->PartitionEntry[i];
            if (p->PartitionLength.QuadPart == 0) continue;
            primaries++;
            /* R10: MBR type 0x42 is the Logical Disk Manager. The real
             * layout lives in an LDM database and the table is decorative;
             * writing based on this view destroys the volume set. */
            if (p->Mbr.PartitionType == 0x42) d->is_dynamic = 1;
        }
        d->primary_partitions = primaries;
    } else if (lay->PartitionStyle == PARTITION_STYLE_GPT) {
        for (DWORD i = 0; i < lay->PartitionCount && i < 128; i++) {
            const PARTITION_INFORMATION_EX *p = &lay->PartitionEntry[i];
            if (p->PartitionLength.QuadPart == 0) continue;
            const GUID *t = &p->Gpt.PartitionType;
            if (guid_eq(t, &PF_GPT_LDM_DATA) || guid_eq(t, &PF_GPT_LDM_META))
                d->is_dynamic = 1;
            if (guid_eq(t, &PF_GPT_SPACES))
                d->is_storage_space = 1;
            if (guid_eq(t, &PF_GPT_ESP) && !d->esp_offset) {
                d->esp_offset = (uint64_t)p->StartingOffset.QuadPart;
                d->esp_length = (uint64_t)p->PartitionLength.QuadPart;
            }
        }
    }

    /* A Storage Spaces virtual disk presents itself as an ordinary
     * PhysicalDriveN with a bus type of its own. Its blocks are scattered
     * across the pool members, so its "partition table" describes nothing
     * we can safely write to. No WMI needed for either half of this. */
    if (d->bus_type == BusTypeSpaces) d->is_storage_space = 1;
}

static void query_layout(HANDLE h, pf_disk *d)
{
    BYTE buf[16384]; DWORD ret = 0;
    d->partition_style = 2;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
                         buf, sizeof buf, &ret, NULL)) {
        /* Unknown is never OK, but it is check_disks that decides: a
         * RAW style on the system disk is refused there along with the
         * rest of the disk's answers. */
        if (d->bus_type == BusTypeSpaces) d->is_storage_space = 1;
        return;
    }
    scan_layout((const DRIVE_LAYOUT_INFORMATION_EX *)buf, d);
}

/* R5: SMART. Refuse on any pending or uncorrectable sector. */
static void query_smart(HANDLE h, pf_disk *d)
{
    d->smart_ok = -1;
    STORAGE_PREDICT_FAILURE pf;
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_PREDICT_FAILURE, NULL, 0,
                        &pf, sizeof pf, &ret, NULL)) {
        d->smart_ok = pf.PredictFailure ? 0 : 1;

        /* VendorSpecific holds the raw ATA SMART attribute table: 2 bytes
         * of revision then 30 twelve-byte entries. Attribute ids:
         * 5 reallocated, 197 pending, 198 uncorrectable, 9 power-on hours. */
        BYTE *a = pf.VendorSpecific + 2;
        for (int i = 0; i < 30; i++) {
            BYTE *e = a + i * 12;
            BYTE id = e[0];
            uint64_t raw = (uint64_t)e[5] | ((uint64_t)e[6] << 8) |
                           ((uint64_t)e[7] << 16) | ((uint64_t)e[8] << 24) |
                           ((uint64_t)e[9] << 32) | ((uint64_t)e[10] << 40);
            switch (id) {
                case 5:   d->smart_reallocated     = (uint32_t)raw; break;
                case 9:   d->smart_power_on_hours  = (uint32_t)raw; break;
                case 197: d->smart_pending         = (uint32_t)raw; break;
                case 198: d->smart_uncorrectable   = (uint32_t)raw; break;
            }
        }
    }
}

/* ── R4/R11: the one stick that may stay plugged in ──────────────── */
static char g_recovery_serial[64];

void pf_set_recovery_stick(const char *serial)
{
    snprintf(g_recovery_serial, sizeof g_recovery_serial, "%s", serial ? serial : "");
}

const char *pf_recovery_stick(void) { return g_recovery_serial; }

/* Serials come back from different drivers with different padding and
 * case, so compare only the alphanumerics. A serial shorter than four
 * usable characters is not an identifier -- plenty of cheap sticks
 * report "0" or nothing at all -- and must never match, because the
 * cost of a false match is writing to the user's backup drive. */
#define PF_SERIAL_MIN 4

static int serial_eq(const char *a, const char *b)
{
    int used = 0;
    while (*a && *b) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) break;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++; used++;
    }
    while (*a && !isalnum((unsigned char)*a)) a++;
    while (*b && !isalnum((unsigned char)*b)) b++;
    return !*a && !*b && used >= PF_SERIAL_MIN;
}

static int serial_usable(const char *s)
{
    int n = 0;
    for (; *s; s++) if (isalnum((unsigned char)*s)) n++;
    return n >= PF_SERIAL_MIN;
}

/* ── disks ───────────────────────────────────────────────────────── */
static void check_disks(pf_report *r)
{
    char sysdrive[8] = "C:";
    GetEnvironmentVariableA("SystemDrive", sysdrive, sizeof sysdrive);

    /* Which physical disk holds the system volume? It must be exactly
     * one extent -- a spanned or striped C: is not safely shrinkable. */
    int sys_disk = -1;
    int ioctl_failed = 0, spanned_reported = 0;
    {
        char path[16];
        snprintf(path, sizeof path, "\\\\.\\%s", sysdrive);
        HANDLE v = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (v != INVALID_HANDLE_VALUE) {
            BYTE buf[1024]; DWORD ret = 0;
            if (DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                                buf, sizeof buf, &ret, NULL)) {
                VOLUME_DISK_EXTENTS *ext = (VOLUME_DISK_EXTENTS *)buf;
                if (ext->NumberOfDiskExtents == 1) {
                    sys_disk = (int)ext->Extents[0].DiskNumber;
                    /* Where C: starts on that disk. Needed to read its
                     * first sector off the raw device (R1) rather than
                     * through the volume, which is the only way to see
                     * an encryption layer between the two. */
                    r->system_offset = (uint64_t)ext->Extents[0].StartingOffset.QuadPart;
                } else if (ext->NumberOfDiskExtents > 1) {
                    spanned_reported = 1;
                    add(r, "spanned-system-volume", "R10", PF_BLOCK,
                        "Windows is spread across more than one disk",
                        "This Windows installation spans multiple physical disks "
                        "(a spanned, striped or mirrored volume). Its layout cannot be "
                        "changed safely.",
                        "AurOS cannot install on this configuration.");
                } else {
                    /* Zero extents is not "spanned", it is "no answer". */
                    ioctl_failed = 1;
                }
            } else {
                ioctl_failed = 1;
            }
            CloseHandle(v);
        } else {
            ioctl_failed = 1;
        }
    }

    /* Safety invariant: UNKNOWN IS NEVER OK. If we cannot say with
     * certainty which physical disk holds Windows, we must not touch any
     * disk -- a wrong answer here is the wrong-target write (R11). */
    if (sys_disk < 0 && !spanned_reported)
        add(r, "system-disk-unknown", "R11", PF_BLOCK,
            "Could not determine which drive Windows is on",
            ioctl_failed
              ? "The query that maps the Windows drive to a physical disk did not "
                "succeed, so AurBridge cannot be certain which disk it would change."
              : "The Windows drive did not map to exactly one physical disk.",
            "This can happen on unusual storage setups (RAID, storage pools, some "
            "virtual machines). AurOS will not guess which disk to write to, so it "
            "stops here. Nothing has been changed.");
    r->system_disk = sys_disk;

    for (int i = 0; i < PF_MAX_DISKS; i++) {
        char path[32];
        snprintf(path, sizeof path, "\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        pf_disk *d = &r->disks[r->n_disks];
        memset(d, 0, sizeof *d);
        d->index = i;
        d->is_system = (i == sys_disk);
        query_disk_props(h, d);
        query_layout(h, d);
        query_smart(h, d);
        CloseHandle(h);
        r->n_disks++;

        char det[512], sz[32];

        /* R10: a dynamic disk's partition table is decorative, in both
         * the MBR and the GPT spelling of it. */
        if (d->is_dynamic && d->is_system)
            add(r, "dynamic-disk", "R10", PF_BLOCK, "This disk is a Windows dynamic disk",
                "Dynamic disks store their real layout in a separate database, so the "
                "normal partition table cannot be trusted. Changing it would destroy "
                "the volumes on this disk.",
                "AurOS cannot install onto a dynamic disk. Converting back to a basic "
                "disk is a destructive operation you should only do with a full backup.");
        else if (d->is_dynamic)
            add(r, "dynamic-disk-other", "R10", PF_WARN,
                "Another drive in this PC is a dynamic disk",
                "AurBridge will not touch that drive, but a dynamic disk can be part of "
                "a volume set that spans drives.",
                "Make sure you have a backup of anything stored on your other drives "
                "before continuing.");

        /* R10: Storage Spaces, either the virtual disk or a member of
         * the pool behind one. Detected from the bus type and the
         * protective partition GUID, so no WMI and no COM. */
        if (d->is_storage_space && d->is_system)
            add(r, "storage-spaces", "R10", PF_BLOCK, "This PC uses Storage Spaces",
                "Windows is installed on a Storage Spaces drive. What looks like one "
                "drive is really pieces of several, assembled by Windows, so the layout "
                "AurBridge can see is not the layout on the disks.",
                "AurOS cannot install onto Storage Spaces. Nothing has been changed.");
        else if (d->is_storage_space)
            add(r, "storage-spaces-other", "R10", PF_WARN,
                "Another drive in this PC belongs to a Storage Spaces pool",
                "AurBridge will not touch that drive.",
                "Make sure you have a backup of anything stored in the pool before "
                "continuing.");

        if (d->is_system) {
            /* R5: this is the drive we are about to work hardest. */
            if (d->smart_pending || d->smart_uncorrectable) {
                snprintf(det, sizeof det,
                    "%s reports %lu pending and %lu uncorrectable sectors after %lu "
                    "power-on hours.", d->model,
                    (unsigned long)d->smart_pending,
                    (unsigned long)d->smart_uncorrectable,
                    (unsigned long)d->smart_power_on_hours);
                add(r, "smart-bad-sectors", "R5", PF_BLOCK, "This hard drive is failing",
                    det,
                    "Do not install anything on this drive. Copy your important files to "
                    "an external drive or cloud storage now, while it still reads. "
                    "The drive should be replaced.");
            } else if (d->smart_ok == 0) {
                snprintf(det, sizeof det, "%s reports imminent failure (SMART).", d->model);
                add(r, "smart-predict-failure", "R5", PF_BLOCK, "This hard drive is failing",
                    det, "Back up your files to another drive immediately. "
                         "This drive should be replaced before installing anything.");
            } else if (d->smart_reallocated > 0) {
                snprintf(det, sizeof det,
                    "%s has reallocated %lu sectors. That is a sign of wear but not "
                    "immediate failure.", d->model, (unsigned long)d->smart_reallocated);
                add(r, "smart-reallocated", "R5", PF_WARN, "This drive shows some wear", det,
                    "Make sure you have a backup before continuing.");
            }

            /* R8: a full MBR table has no room for another partition. */
            if (d->partition_style == 0 && d->primary_partitions >= 4) {
                snprintf(det, sizeof det,
                    "Disk %d already has %d primary partitions, the maximum an MBR disk "
                    "allows.", i, d->primary_partitions);
                add(r, "mbr-four-primaries", "R8", PF_BLOCK,
                    "There is no room for another partition", det,
                    "Removing one would usually mean deleting the manufacturer's recovery "
                    "partition, which is often your only way to restore Windows. "
                    "AurBridge will not do that.");
            }

            /* The partition table is what the commit phase rewrites.
             * Failing to read it is not a partition style, it is a
             * missing answer. */
            if (d->partition_style == 2)
                add(r, "layout-unreadable", "R11", PF_BLOCK,
                    "The Windows drive's partition table could not be read",
                    "AurBridge could not read how the Windows drive is divided up. It "
                    "will not change a layout it cannot see.",
                    "This can happen on unusual storage setups and on some virtual "
                    "machines. AurOS stops here; nothing has been changed.");

            /* 512e vs 4Kn. Block rather than assume 512: the assumption
             * is silent and makes the new partition eight times too
             * small on a 4Kn disk. */
            if (!sector_size_ok(d->logical_sector) || !sector_size_ok(d->physical_sector)) {
                snprintf(det, sizeof det,
                    "This drive reports a logical sector size of %lu bytes and a physical "
                    "sector size of %lu bytes (0 means it would not say). AurBridge needs "
                    "both to work out where a partition may start and end.",
                    (unsigned long)d->logical_sector, (unsigned long)d->physical_sector);
                add(r, "sector-size-unknown", "R7", PF_BLOCK,
                    "This drive will not say how it is laid out", det,
                    "This is usually a storage driver that is out of date or unusual. "
                    "AurOS will not guess at this, because guessing wrong makes the new "
                    "partition the wrong size. Nothing has been changed.");
            } else {
                snprintf(det, sizeof det,
                    "%s: %lu-byte logical sectors, %lu-byte physical sectors.",
                    d->model[0] ? d->model : "The Windows drive",
                    (unsigned long)d->logical_sector,
                    (unsigned long)d->physical_sector);
                add(r, "sector-size", "R7", PF_INFO, "Drive sector size read", det, "");
            }

            /* Windows running from a USB disk (Windows To Go, or a
             * cloned rescue stick). Every later phase assumes the target
             * is an internal disk that will still be there after the
             * restart. */
            if (d->is_removable)
                add(r, "system-disk-removable", "R11", PF_BLOCK,
                    "Windows is running from a removable drive",
                    "The drive Windows starts from is on a removable connection (USB, "
                    "FireWire or a memory card). AurOS installs onto the PC's own disk.",
                    "If you meant to install onto this PC's internal drive, start "
                    "Windows from that drive instead and run AurBridge again.");
        }

        if (d->is_system && d->size_bytes == 0)
            add(r, "system-disk-unreadable", "R11", PF_BLOCK,
                "The Windows drive could not be read properly",
                "AurBridge could not read the size of the disk Windows is installed on.",
                "AurOS will not change a disk it cannot fully read. Nothing has been "
                "changed.");

        if (!d->is_system) {
            /* R11: an attached external drive is how installers write to
             * the wrong target. R4 makes a recovery USB mandatory and
             * phase 2 has to write to it, so exactly one nominated stick
             * is let through and everything else still refuses. */
            int nominated = g_recovery_serial[0] && d->is_removable &&
                            serial_eq(d->serial, g_recovery_serial);
            if (nominated) {
                d->is_recovery_stick = 1;
                if (r->recovery_disk >= 0) {
                    /* Two disks answering to the same serial: we cannot
                     * tell which one phase 2 would erase. */
                    snprintf(det, sizeof det,
                        "Two connected drives report the same serial number (%s), so "
                        "AurBridge cannot tell them apart.", d->serial);
                    add(r, "recovery-stick-ambiguous", "R11", PF_BLOCK,
                        "Two drives look identical to this PC", det,
                        "Unplug everything except the one stick you are using for "
                        "recovery, then click Re-check.");
                } else {
                    r->recovery_disk = r->n_disks - 1;
                    size_str(d->size_bytes, sz, sizeof sz);
                    snprintf(det, sizeof det,
                        "%s (%s, serial %s) is the stick you chose for recovery.",
                        d->model[0] ? d->model : "A removable drive", sz, d->serial);
                    add(r, "recovery-stick", "R4", PF_WARN,
                        "This stick will be erased", det,
                        "Everything on this stick will be wiped and replaced with the "
                        "rescue files that can put Windows back. Copy anything you want "
                        "to keep off it first. Every other drive must be unplugged.");
                }
            } else if (d->is_removable && !serial_usable(d->serial)) {
                /* A stick with no serial can never be nominated, so
                 * saying "unplug everything except your recovery stick"
                 * and nothing else would leave a user re-running this
                 * forever with the stick they already chose. */
                size_str(d->size_bytes, sz, sizeof sz);
                snprintf(det, sizeof det,
                    "%s (%s) does not report a serial number, so AurBridge cannot tell "
                    "it apart from any other drive you plug in.",
                    d->model[0] ? d->model : "A removable drive", sz);
                add(r, "removable-no-serial", "R11", PF_BLOCK,
                    "One of the plugged-in drives cannot be identified", det,
                    "Unplug everything except the one stick you are using for recovery, "
                    "then click Re-check. If this is the stick you meant to use for "
                    "recovery, you will need a different one: AurBridge only writes to a "
                    "drive it can recognise again afterwards.");
            } else if (d->is_removable) {
                size_str(d->size_bytes, sz, sizeof sz);
                snprintf(det, sizeof det, "%s (%s) is plugged in on drive %d.",
                         d->model[0] ? d->model : "A removable drive", sz, i);
                add(r, "removable-attached", "R11", PF_BLOCK,
                    "Unplug the drives that are not your recovery stick", det,
                    "Unplug everything except the one stick you are using for recovery, "
                    "then click Re-check. That one stick is the only removable drive "
                    "AurBridge will write to, and having nothing else connected makes "
                    "writing to the wrong drive impossible.");
            } else if (d->bus_type < 0) {
                /* Cannot say how this disk is attached, so cannot say
                 * whether the guard above applies to it. */
                snprintf(det, sizeof det,
                    "Drive %d (%s) would not say how it is connected to this PC.",
                    i, d->model[0] ? d->model : "unnamed");
                add(r, "disk-unidentified", "R11", PF_BLOCK,
                    "One of the drives in this PC cannot be identified", det,
                    "Unplug everything except the one stick you are using for recovery, "
                    "then click Re-check. If this drive is built into the PC and the "
                    "message stays, AurOS will not install on this PC.");
            }
        }
    }

    /* A nomination that matches nothing is worth saying out loud: the
     * user may have unplugged the stick, or be looking at a stale
     * serial. It is not a refusal -- phase 2 is where a missing
     * recovery USB stops the install (R4). */
    snprintf(r->recovery_serial, sizeof r->recovery_serial, "%s", g_recovery_serial);
    if (g_recovery_serial[0] && r->recovery_disk < 0)
        add(r, "recovery-stick-absent", "R4", PF_INFO,
            "Your recovery stick is not plugged in",
            "The stick you chose for recovery is not connected to this PC right now.",
            "Plug it back in before you start installing. AurBridge cannot continue "
            "without it.");

    if (r->n_disks == 0)
        add(r, "no-disks", "", PF_BLOCK, "No disks could be read",
            "AurBridge could not open any physical drive.",
            "Make sure you started AurBridge as administrator.");
}

/* ── R7: how much can this volume actually give up? ──────────────── */
/*
 * GetDiskFreeSpaceEx answers a different question -- free space *inside*
 * the filesystem -- and AURBRIDGE.md records measuring that one instead
 * as the mistake that survives review. What matters is where the data
 * ends, and that is written down in $Bitmap.
 *
 * One pass over the bitmap yields two floors, because two resizers care
 * about two different things:
 *
 *   offline  ntfsresize, which AurBridge uses in phase 4, relocates
 *            every file it finds. Its floor is the number of allocated
 *            clusters, wherever on the volume they sit.
 *   online   Windows' own shrink will not move the pagefile, hiberfil,
 *            VSS store, $MFT, $Bitmap or $UsnJrnl, so its floor is the
 *            *last* allocated cluster. This is the number behind R7's
 *            "30 GB free may yield 2 GB offered".
 *
 * The offline floor decides, because it is the shrink this product
 * performs. The online floor is reported because it is what the user
 * would be offered if they tried this themselves in Disk Management,
 * and the gap between the two is usually the whole story.
 *
 * FSCTL_GET_VOLUME_BITMAP is a documented read-only query. The two
 * obvious alternatives are not usable here: FSCTL_SHRINK_VOLUME's
 * ShrinkPrepare needs a write handle and leaves the volume in a prepared
 * state that must be committed or aborted, which preflight may not do;
 * and `diskpart shrink querymax` means spawning a console process from a
 * GUI installer and parsing localised text, for a number that describes
 * the online shrink we do not perform.
 */
/* What AurOS asks for, and what Windows must be left with. The second
 * number is not slack: Windows Update alone wants roughly this much
 * free, and ntfsresize needs headroom above the data it relocates.
 * Shrinking to the last free byte is an install that fits and a PC that
 * stops working a month later. */
#define PF_AUROS_NEED   (28ULL * 1024 * 1024 * 1024)
#define PF_WINDOWS_KEEP ( 8ULL * 1024 * 1024 * 1024)

static uint64_t shrink_giveable(uint64_t offline_shrinkable)
{
    return offline_shrinkable > PF_WINDOWS_KEEP
         ? offline_shrinkable - PF_WINDOWS_KEEP : 0;
}

static void bitmap_scan(const BYTE *bits, uint64_t nbits, uint64_t base,
                        uint64_t *used, uint64_t *end)
{
    static const BYTE popcount[16] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };
    uint64_t whole = nbits / 8, i;
    for (i = 0; i < whole; i++) {
        BYTE b = bits[i];
        if (!b) continue;
        *used += popcount[b & 0xF] + popcount[b >> 4];
        for (int k = 7; k >= 0; k--)
            if (b & (1u << k)) {
                uint64_t e = base + i * 8 + (uint64_t)k + 1;
                if (e > *end) *end = e;
                break;
            }
    }
    for (uint64_t k = whole * 8; k < nbits; k++) {
        if (bits[k / 8] & (1u << (k % 8))) {
            (*used)++;
            if (base + k + 1 > *end) *end = base + k + 1;
        }
    }
}

static void measure_shrink(const char *drive, pf_volume *v)
{
    char path[16];
    snprintf(path, sizeof path, "\\\\.\\%s", drive);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    NTFS_VOLUME_DATA_BUFFER nv;
    DWORD ret = 0;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                         &nv, sizeof nv, &ret, NULL) || nv.BytesPerCluster == 0) {
        CloseHandle(h);
        return;
    }
    v->cluster_bytes = nv.BytesPerCluster;
    uint64_t total = (uint64_t)nv.TotalClusters.QuadPart;

    /* 1 MiB of bitmap covers eight million clusters, so even a 2 TB
     * volume is a few dozen round trips. */
    const DWORD BITBUF = 1u << 20;
    const DWORD HDR    = (DWORD)offsetof(VOLUME_BITMAP_BUFFER, Buffer);
    BYTE *buf = (BYTE *)malloc(BITBUF);
    if (!buf) { CloseHandle(h); return; }

    uint64_t lcn = 0, used = 0, end = 0;
    int ok = 1;
    while (lcn < total) {
        STARTING_LCN_INPUT_BUFFER in;
        in.StartingLcn.QuadPart = (LONGLONG)lcn;
        memset(buf, 0, BITBUF);          /* unwritten tail reads as free */
        ret = 0;
        BOOL rc = DeviceIoControl(h, FSCTL_GET_VOLUME_BITMAP, &in, sizeof in,
                                  buf, BITBUF, &ret, NULL);
        DWORD err = rc ? ERROR_SUCCESS : GetLastError();
        if (!rc && err != ERROR_MORE_DATA) { ok = 0; break; }

        VOLUME_BITMAP_BUFFER *bm = (VOLUME_BITMAP_BUFFER *)buf;
        uint64_t base = (uint64_t)bm->StartingLcn.QuadPart;
        uint64_t bits = (uint64_t)bm->BitmapSize.QuadPart;
        uint64_t cap  = (uint64_t)(BITBUF - HDR) * 8;
        if (bits > cap) bits = cap;
        if (base + bits <= lcn) { ok = 0; break; }   /* no forward progress */
        bitmap_scan(bm->Buffer, bits, base, &used, &end);
        lcn = base + bits;
    }
    free(buf);
    CloseHandle(h);
    if (!ok) return;

    uint64_t size = total * v->cluster_bytes;
    v->used_bytes          = used * v->cluster_bytes;
    v->shrink_floor_bytes  = end * v->cluster_bytes;
    v->offline_shrinkable  = size > v->used_bytes         ? size - v->used_bytes        : 0;
    v->online_shrinkable   = size > v->shrink_floor_bytes ? size - v->shrink_floor_bytes: 0;
    v->shrink_measured     = 1;
}

static uint64_t file_size(const char *dir, const char *name)
{
    char p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    snprintf(p, sizeof p, "%s\\%s", dir, name);
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

/* ── volumes: BitLocker, dirty bit, free space ───────────────────── */
static int volume_is_bitlocker(const char *drive)
{
    /* A BitLocker volume carries the "-FVE-FS-" signature where NTFS
     * would put its OEM id. Reading one sector is enough and needs no
     * COM/WMI, which keeps preflight dependency-free.
     *
     * The read is 4096 bytes, not 512: a device handle only accepts
     * lengths that are a multiple of the logical sector size, so the
     * 512-byte read this used to do failed outright on a 4Kn disk --
     * and a failed read here returned "not BitLocker". */
    char path[16];
    snprintf(path, sizeof path, "\\\\.\\%s", drive);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    BYTE sec[4096];
    int res = -1;
    if (read_at(h, 0, sec, sizeof sec))
        res = (boot_sector_kind(sec) == PF_BS_FVE) ? 1 : 0;
    CloseHandle(h);
    return res;
}

/* R1. The refusal below cannot be cleared by producing the recovery
 * key, so it must not read as though it can. AURBRIDGE.md: there is no
 * shrink path for a BitLocker-protected volume, online or offline --
 * cryptsetup's BITLK support has no resize, ntfsresize sees -FVE-FS-
 * rather than NTFS, Windows' own shrink is unavailable on an encrypted
 * volume, and Suspend-BitLocker does not decrypt a single sector. Only
 * a full decryption clears it, and a refusal a user cannot clear by
 * following its own instructions is a user who re-runs forever. */
static const char *BITLOCKER_REMEDY =
    "BitLocker has to be turned off completely -- not suspended, and not just "
    "written down. In Windows, search for 'Manage BitLocker', open it, and choose "
    "'Turn off BitLocker' for this drive. Windows then decrypts the whole drive, "
    "which can take several hours; leave the PC plugged in and let it finish. Run "
    "AurBridge again when it says the drive is decrypted. Save your 48-digit "
    "recovery key somewhere safe first, from aka.ms/myrecoverykey -- you will need "
    "it if anything interrupts the decryption.";

static void check_volumes(pf_report *r)
{
    char sysdrive[8] = "C:";
    GetEnvironmentVariableA("SystemDrive", sysdrive, sizeof sysdrive);

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && r->n_volumes < PF_MAX_VOLUMES; i++) {
        if (!(mask & (1u << i))) continue;
        char root[8];  snprintf(root, sizeof root, "%c:\\", 'A' + i);
        char drive[8]; snprintf(drive, sizeof drive, "%c:",  'A' + i);
        if (GetDriveTypeA(root) != DRIVE_FIXED) continue;

        pf_volume *v = &r->volumes[r->n_volumes++];
        memset(v, 0, sizeof *v);
        snprintf(v->mount, sizeof v->mount, "%s", drive);

        char fs[16] = "";
        GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fs, sizeof fs);
        snprintf(v->fs, sizeof v->fs, "%s", fs);

        ULARGE_INTEGER freeb, total, freetotal;
        if (GetDiskFreeSpaceExA(root, &freeb, &total, &freetotal)) {
            v->size_bytes = total.QuadPart;
            v->free_bytes = freeb.QuadPart;
        }
        v->bitlocker = volume_is_bitlocker(drive);

        int is_sys = (_stricmp(drive, sysdrive) == 0);
        if (is_sys) r->system_volume = r->n_volumes - 1;

        if (v->bitlocker == 1) {
            char det[512];
            snprintf(det, sizeof det,
                "Drive %s is encrypted with BitLocker.%s", drive,
                is_sys ? " This is the drive Windows is installed on. There is no way "
                         "to resize an encrypted drive safely: the tools that could do "
                         "it cannot read it, and the ones that can read it will not "
                         "resize it." : "");
            /* R1: the highest-consequence item in the product. */
            add(r, is_sys ? "bitlocker-system" : "bitlocker-other", "R1", PF_BLOCK,
                "This drive is encrypted with BitLocker", det, BITLOCKER_REMEDY);
        } else if (v->bitlocker < 0 && is_sys) {
            /* Unknown is never OK, and this one used to pass silently. */
            add(r, "bitlocker-unknown", "R1", PF_BLOCK,
                "AurBridge cannot tell whether this drive is encrypted",
                "The first sector of the Windows drive could not be read, so AurBridge "
                "cannot check for BitLocker. Resizing an encrypted drive destroys it.",
                "Restart Windows and run AurBridge again as administrator. If the "
                "message stays, AurOS will not install on this PC.");
        }

        if (is_sys) {
            /* R9: feeding an already-corrupt $MFT into a resizer turns a
             * few bad files into an unmountable volume. */
            char vpath[16]; snprintf(vpath, sizeof vpath, "\\\\.\\%s", drive);
            HANDLE h = CreateFileA(vpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD dirty = 0, ret = 0;
                if (DeviceIoControl(h, FSCTL_IS_VOLUME_DIRTY, NULL, 0,
                                    &dirty, sizeof dirty, &ret, NULL) &&
                    (dirty & 1)) {
                    v->dirty = 1;
                    add(r, "volume-dirty", "R9", PF_BLOCK,
                        "Windows has marked this drive as needing repair",
                        "The drive is flagged for a disk check. Changing the layout of a "
                        "filesystem that is already damaged can make it unreadable.",
                        "Open Command Prompt as administrator, run  chkdsk C: /f  and "
                        "restart. When it finishes cleanly, run AurBridge again.");
                }
                CloseHandle(h);
            }

            if (strcmp(v->fs, "NTFS") != 0 && v->fs[0]) {
                add(r, "system-not-ntfs", "", PF_BLOCK, "Unexpected filesystem on the Windows drive",
                    "The Windows drive is not formatted as NTFS.", "AurOS cannot resize it.");
                continue;         /* the measurements below are NTFS-only */
            }

            v->pagefile_bytes = file_size(sysdrive, "pagefile.sys");
            v->hiberfil_bytes = file_size(sysdrive, "hiberfil.sys");
            v->hibernated     = v->hiberfil_bytes > 0;
            if (v->bitlocker == 0) measure_shrink(drive, v);

            char det[512], a[32], b[32], c[32];

            if (!v->shrink_measured) {
                /* Unknown is never OK: without this the fallback would
                 * be free space, which is the number R7 exists to say
                 * is the wrong one. An encrypted volume is already
                 * refused above, with the remedy that clears it, and
                 * does not collect a second card here. */
                if (v->bitlocker == 0)
                    add(r, "space-unmeasurable", "R7", PF_BLOCK,
                        "AurBridge could not measure the space on the Windows drive",
                        "Windows would not report which parts of the drive are in use, so "
                        "AurBridge cannot tell how much room it could free. It will not "
                        "guess: guessing high is how an install runs out of space "
                        "halfway.",
                        "Open Command Prompt as administrator, run  chkdsk C: /f  and "
                        "restart. When it finishes cleanly, run AurBridge again.");
            } else {
                uint64_t giveable = shrink_giveable(v->offline_shrinkable);

                size_str(giveable, a, sizeof a);
                size_str(v->used_bytes, b, sizeof b);
                size_str(PF_AUROS_NEED, c, sizeof c);
                if (giveable < PF_AUROS_NEED) {
                    snprintf(det, sizeof det,
                        "Drive %s holds %s of files. Once Windows keeps the 8 GB it needs "
                        "to carry on updating itself, about %s can be freed. AurOS needs "
                        "%s: room for the system, your files, and the rescue files that "
                        "can put Windows back.", drive, b, a, c);
                    add(r, "insufficient-space", "R7", PF_BLOCK, "Not enough free space",
                        det,
                        "Empty the Recycle Bin, then use Windows' Disk Cleanup, or move "
                        "some large files to an external drive. Then click Re-check.");
                } else {
                    snprintf(det, sizeof det,
                        "Drive %s holds %s of files and can give up about %s, measured "
                        "from the drive's own record of which parts are in use.",
                        drive, b, a);
                    add(r, "space-measured", "R7", PF_INFO, "Free space measured", det, "");
                }

                /* R7's headline number, and the reason the shrink does
                 * not happen in Windows. Not an obstacle: phase 4 does
                 * this offline, where the limit does not apply. */
                if (v->online_shrinkable < PF_AUROS_NEED) {
                    size_str(v->online_shrinkable, a, sizeof a);
                    size_str(v->free_bytes, b, sizeof b);
                    snprintf(det, sizeof det,
                        "Drive %s shows %s free, but Windows itself could only hand back "
                        "%s of it: files it refuses to move while it is running (the "
                        "paging file, the hibernation file, restore points) sit at the "
                        "far end of the drive.", drive, b, a);
                    add(r, "shrink-online-limited", "R7", PF_INFO,
                        "Windows on its own could not free this space", det,
                        "Nothing to do. AurOS moves those files from its own rescue "
                        "environment, where they are not in use.");
                }
            }
        }
    }
}

/* ── R12: the EFI System Partition ───────────────────────────────── */
/* The staging environment is written here, and this partition is never
 * reformatted. Both need its free space measured first, and the ESP has
 * no drive letter -- so it is found by matching the volume list against
 * the partition's offset on disk. Assigning a letter with `mountvol`
 * would be a change to the machine, which preflight may not make. */
static void check_esp(pf_report *r)
{
    r->esp_found = -1;
    if (r->system_disk < 0) return;              /* already refused (R11) */

    const pf_disk *sd = NULL;
    for (int i = 0; i < r->n_disks; i++)
        if (r->disks[i].index == r->system_disk) sd = &r->disks[i];
    if (!sd) return;

    if (!sd->esp_offset) {
        r->esp_found = 0;
        if (r->is_uefi == 1)
            add(r, "esp-missing", "R12", PF_BLOCK,
                "The start-up partition is not on the Windows drive",
                "This PC started in UEFI mode, but the small partition that holds its "
                "start-up files is not on the drive Windows is installed on. AurOS would "
                "have to change a drive it was not asked to touch.",
                "This usually means the PC starts from a second drive or a memory card. "
                "AurOS will not install on this arrangement. Nothing has been changed.");
        return;
    }

    /* Walk the volume list for the one whose single extent starts where
     * the ESP does. */
    char name[MAX_PATH];
    HANDLE find = FindFirstVolumeA(name, sizeof name);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            size_t len = strlen(name);
            if (len < 2 || name[len-1] != '\\') continue;

            char dev[MAX_PATH];
            snprintf(dev, sizeof dev, "%s", name);
            dev[len-1] = '\0';                 /* CreateFile wants no slash */
            HANDLE h = CreateFileA(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) continue;

            int is_esp = 0;
            BYTE buf[1024]; DWORD ret = 0;
            if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                                buf, sizeof buf, &ret, NULL)) {
                VOLUME_DISK_EXTENTS *ex = (VOLUME_DISK_EXTENTS *)buf;
                is_esp = ex->NumberOfDiskExtents == 1 &&
                         (int)ex->Extents[0].DiskNumber == r->system_disk &&
                         (uint64_t)ex->Extents[0].StartingOffset.QuadPart == sd->esp_offset;
            }
            CloseHandle(h);
            if (!is_esp) continue;

            /* Querying free space mounts the volume if it is not
             * mounted already, which is a read: nothing is written to a
             * FAT volume by looking at its allocation table. A failure
             * here leaves esp_found at -1, which is the refusal. */
            ULARGE_INTEGER freeb, total, freetotal;
            if (GetDiskFreeSpaceExA(name, &freeb, &total, &freetotal)) {
                r->esp_found      = 1;
                r->esp_size_bytes = total.QuadPart;
                r->esp_free_bytes = freeb.QuadPart;
                snprintf(r->esp_volume, sizeof r->esp_volume, "%s", name);
            }
            break;
        } while (FindNextVolumeA(find, name, sizeof name));
        FindVolumeClose(find);
    }

    char det[512], a[32], b[32];
    if (r->esp_found != 1) {
        add(r, "esp-unreadable", "R12", PF_BLOCK,
            "AurBridge could not read the start-up partition",
            "The small partition this PC starts from was found on the disk, but Windows "
            "would not report how much room is left in it. AurOS has to put its start-up "
            "files somewhere, and it will not write into a partition it cannot measure.",
            "Restart Windows and run AurBridge again as administrator. If the message "
            "stays, AurOS will not install on this PC.");
        return;
    }

    /* AURBRIDGE.md: the staging kernel and initramfs go here, and the
     * initramfs is budgeted at ~80 MB. An OEM ESP is commonly 100 MB and
     * 85-95% full, so falling back to the recovery USB is the designed
     * path, not a fault. */
    const uint64_t STAGING_NEED = 96ULL * 1024 * 1024;
    size_str(r->esp_free_bytes, a, sizeof a);
    size_str(r->esp_size_bytes, b, sizeof b);
    if (r->esp_free_bytes < STAGING_NEED) {
        snprintf(det, sizeof det,
            "The start-up partition is %s with %s free -- not enough for the rescue "
            "system AurOS starts from. It will start from your recovery stick instead. "
            "Nothing in the start-up partition is removed or reformatted.", b, a);
        add(r, "esp-low-space", "R12", PF_INFO,
            "The start-up partition is nearly full", det,
            "Nothing to do, but the recovery stick now has to stay plugged in for the "
            "restart.");
    } else {
        snprintf(det, sizeof det,
            "The start-up partition is %s with %s free.", b, a);
        add(r, "esp-space", "R12", PF_INFO, "Start-up partition measured", det, "");
    }
}

/* ── R1: third-party full-disk encryption ────────────────────────── */
/*
 * AURBRIDGE.md: abort unconditionally. There is no safe shrink
 * underneath a sector-level encryption filter we do not control.
 *
 * Two detections, because neither is sufficient on its own:
 *
 *  1. The ciphertext test is the authoritative one and needs no vendor
 *     name at all. Read C:'s first sector twice -- once through the
 *     volume, where any filter has already decrypted it, and once from
 *     the raw disk at the same offset, below the filter. Windows says
 *     the volume is NTFS; if the disk does not also say NTFS, something
 *     between them is transforming sectors, and whatever it is, we
 *     cannot shrink under it.
 *  2. The driver-service names identify the product so the refusal can
 *     name it. Where a name is wrong we lose the better wording, not
 *     the refusal. Names were chosen to be vendor-specific enough that
 *     they cannot collide with an ordinary Windows or an antivirus --
 *     Symantec's eeCtrl, for instance, ships with plain Norton and is
 *     deliberately not in this list.
 *
 * SERVICE_QUERY_CONFIG on the SCM is read-only and needs no COM, which
 * keeps preflight free of WMI the way the BitLocker path is.
 */
static const struct { const char *service; const char *product; int enterprise; }
FDE_DRIVERS[] = {
    { "veracrypt",  "VeraCrypt",                     0 },
    { "truecrypt",  "TrueCrypt",                     0 },
    /* Policy-driven products: these encrypt when told to, possibly
     * while the wizard is open, so they refuse on presence alone. */
    { "SGDisx",     "Sophos SafeGuard",              1 },
    { "SGDisk",     "Sophos SafeGuard",              1 },
    { "MfeEpePc",   "Trellix (McAfee) Drive Encryption", 1 },
    { "MfeEpeHost", "Trellix (McAfee) Drive Encryption", 1 },
    { "SafeBoot",   "McAfee Endpoint Encryption",    1 },
    { "SbAlg",      "McAfee Endpoint Encryption",    1 },
    { "PGPwded",    "Symantec Endpoint Encryption",  1 },
    { "PGPdisk",    "Symantec Endpoint Encryption",  1 },
    { NULL, NULL, 0 }
};

/* 1 loads at boot, 0 present but not loading, -1 absent or unreadable. */
static int driver_loads(SC_HANDLE scm, const char *name)
{
    SC_HANDLE svc = OpenServiceA(scm, name, SERVICE_QUERY_CONFIG);
    if (!svc) return -1;
    BYTE buf[8192]; DWORD need = 0;
    int res = -1;
    QUERY_SERVICE_CONFIGA *cfg = (QUERY_SERVICE_CONFIGA *)buf;
    if (QueryServiceConfigA(svc, cfg, sizeof buf, &need))
        res = (cfg->dwStartType <= SERVICE_AUTO_START) ? 1 : 0;
    CloseServiceHandle(svc);
    return res;
}

static void check_encryption(pf_report *r)
{
    r->fde_third_party = -1;
    if (r->system_disk < 0) return;              /* already refused (R11) */

    /* The ciphertext test below compares what Windows says the system
     * volume is against what the disk says at the same offset. Both
     * halves have to be worth comparing. A system volume that is not
     * NTFS, and a volume that does not start where we think it starts,
     * are each already a refusal of their own; adding an encryption
     * verdict on top of one would be an accusation we cannot support. */
    const pf_volume *sv = (r->system_volume >= 0 && r->system_volume < r->n_volumes)
                        ? &r->volumes[r->system_volume] : NULL;
    if (!sv || strcmp(sv->fs, "NTFS") != 0 || r->system_offset == 0) {
        add(r, "fde-not-checked", "R1", PF_BLOCK,
            "AurBridge could not check the Windows drive for encryption",
            "Checking for encryption means comparing what Windows reports about the "
            "Windows drive against what is actually written on the disk, and one of "
            "those two could not be established.",
            "Another message above says which part of this PC AurBridge could not read. "
            "Nothing has been changed.");
        return;
    }

    /* ── the named products ───────────────────────────────────────── */
    const char *found = NULL;
    int found_enterprise = 0;
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        add(r, "fde-check-failed", "R1", PF_BLOCK,
            "AurBridge could not check this PC for encryption software",
            "Windows would not let AurBridge read the list of installed drivers, so it "
            "cannot rule out disk-encryption software. Resizing a drive underneath "
            "encryption destroys it.",
            "Restart Windows and run AurBridge again as administrator. If the message "
            "stays, AurOS will not install on this PC.");
        return;
    }
    for (int i = 0; FDE_DRIVERS[i].service; i++) {
        if (driver_loads(scm, FDE_DRIVERS[i].service) == 1) {
            found = FDE_DRIVERS[i].product;
            found_enterprise = FDE_DRIVERS[i].enterprise;
            break;
        }
    }
    CloseServiceHandle(scm);

    /* VeraCrypt's UEFI system encryption puts its loader on the ESP;
     * the BIOS one puts it in the first track of the disk. Either is
     * proof of system encryption rather than of container use. */
    int vc_system = 0;
    if (r->esp_volume[0]) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%sEFI\\VeraCrypt\\DcsBoot.efi", r->esp_volume);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) vc_system = 1;
        /* VeraCrypt renames Microsoft's loader aside when it installs.
         * The .vc extension is the one that cannot collide with a
         * backup some other tool left behind: a stray bootmgfw.efi.bak
         * is common and would be a refusal for no reason. */
        snprintf(p, sizeof p, "%sEFI\\Microsoft\\Boot\\bootmgfw_ms.vc", r->esp_volume);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) vc_system = 1;
    }

    /* ── the ciphertext test ──────────────────────────────────────── */
    int kind = -1;
    {
        char path[32];
        snprintf(path, sizeof path, "\\\\.\\PhysicalDrive%d", r->system_disk);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            BYTE sec[4096];
            if (read_at(h, r->system_offset, sec, sizeof sec))
                kind = boot_sector_kind(sec);
            /* The BIOS-mode VeraCrypt/TrueCrypt loader lives in the
             * first track, ahead of every partition. */
            BYTE track[65536];
            if (!vc_system && read_at(h, 0, track, sizeof track)) {
                if (mem_find(track, sizeof track, "VeraCrypt") ||
                    mem_find(track, sizeof track, "TrueCrypt"))
                    vc_system = 1;
            }
            CloseHandle(h);
        }
    }

    /* The system volume told Windows it is NTFS -- anything else is
     * already refused by check_volumes -- so the disk must say so too. */
    char det[512];
    if (kind < 0) {
        add(r, "fde-unreadable", "R1", PF_BLOCK,
            "AurBridge could not read the start of the Windows drive",
            "The Windows drive's first sector could not be read directly from the disk, "
            "so AurBridge cannot tell whether the drive is encrypted by something it "
            "does not control.",
            "Restart Windows and run AurBridge again as administrator. If the message "
            "stays, AurOS will not install on this PC.");
        return;
    }
    if (kind == PF_BS_FVE) {
        /* BitLocker, already refused by check_volumes with the remedy
         * that actually clears it. Nothing to add. */
        r->fde_third_party = 0;
        return;
    }
    if (kind != PF_BS_NTFS || vc_system) {
        r->fde_third_party = 1;
        snprintf(det, sizeof det,
            "The Windows drive is encrypted by %s, which sits underneath Windows and "
            "encrypts every sector. Windows can read the drive; anything working on the "
            "disk directly -- including the part of AurOS that would resize it -- sees "
            "scrambled data.",
            found ? found : "software that is not BitLocker");
        add(r, "fde-third-party", "R1", PF_BLOCK,
            "This drive is encrypted by other software", det,
            "The drive has to be fully decrypted before AurOS can install. Open the "
            "encryption software that is managing it and choose permanently decrypt (in "
            "VeraCrypt: System, then Permanently Decrypt System Partition/Drive). That "
            "rewrites the whole drive and can take several hours; leave the PC plugged "
            "in. Run AurBridge again when it has finished.");
        return;
    }

    r->fde_third_party = 0;
    if (found && found_enterprise) {
        /* Not encrypted today, but this is a managed machine and the
         * policy that encrypts it can arrive at any time, including
         * between this check and phase 4. */
        snprintf(det, sizeof det,
            "%s is installed and set to start with Windows. The drive is not encrypted "
            "right now, but software like this encrypts on instruction from whoever "
            "manages the PC, which can happen at any moment.", found);
        add(r, "fde-managed", "R1", PF_BLOCK,
            "This PC is managed by disk-encryption software", det,
            "This PC belongs to a workplace or school setup. Ask whoever manages it to "
            "remove the encryption software before installing AurOS, or use a PC that "
            "is yours to change.");
    } else if (found) {
        snprintf(det, sizeof det,
            "%s is installed on this PC. The Windows drive itself is not encrypted, so "
            "AurOS can still install, but any encrypted files or containers you keep "
            "must be closed first.", found);
        add(r, "fde-container-software", "R1", PF_WARN,
            "Encryption software is installed", det,
            "Close and dismount anything you have open in it before you start "
            "installing, and make sure you have a backup.");
    }
}

static void check_memory(pf_report *r)
{
    MEMORYSTATUSEX m = { .dwLength = sizeof m };
    if (!GlobalMemoryStatusEx(&m)) return;
    r->ram_bytes = m.ullTotalPhys;
    if (m.ullTotalPhys < 1900ULL * 1024 * 1024) {
        char det[256];
        snprintf(det, sizeof det, "This PC has %.1f GB of memory.",
                 (double)m.ullTotalPhys / 1073741824.0);
        add(r, "low-ram", "", PF_WARN, "This PC has little memory", det,
            "AurOS will run, but keep the number of open browser tabs modest.");
    }
}

/* ── driver / RST detection (R10) ────────────────────────────────── */
static void check_storage_controller(pf_report *r)
{
    /* If the SATA/NVMe controller is in Intel RST / VMD remap mode, a
     * kernel without vmd support sees no disk at all: we would write a
     * perfect image and boot to "no bootable device". The Windows-side
     * tell is which driver owns the boot disk's controller. */
    static const char *rst_services[] = { "iaStorAC", "iaStorVD", "iaStorV", "iaStorA", NULL };
    for (int i = 0; rst_services[i]; i++) {
        char key[256];
        snprintf(key, sizeof key, "SYSTEM\\CurrentControlSet\\Services\\%s", rst_services[i]);
        DWORD start = 0;
        if (reg_dword(HKEY_LOCAL_MACHINE, key, "Start", &start) && start <= 3) {
            char det[512];
            snprintf(det, sizeof det,
                "The storage controller is managed by Intel Rapid Storage Technology "
                "(%s). In this mode the drive is presented through an Intel remapping "
                "layer that AurOS may not be able to see.", rst_services[i]);
            add(r, "intel-rst", "R10", PF_WARN,
                "This PC uses Intel Rapid Storage Technology", det,
                "AurBridge will verify from the rescue environment that AurOS can see "
                "your drive before changing anything. If it cannot, nothing is changed.");
            return;
        }
    }
}

/* ── public API ──────────────────────────────────────────────────── */
void pf_run(pf_report *r)
{
    memset(r, 0, sizeof *r);
    r->secure_boot     = -1;
    r->system_disk     = -1;
    r->system_volume   = -1;
    r->has_battery     = -1;
    r->esp_found       = -1;
    r->fde_third_party = -1;
    r->recovery_disk   = -1;

    check_admin(r);
    if (!r->is_admin) return;      /* everything below needs raw disk access */

    check_firmware(r);
    check_power(r);
    check_pending_reboot(r);
    check_fast_startup(r);
    check_disks(r);
    check_volumes(r);
    check_esp(r);                  /* before check_encryption: it looks on the ESP */
    check_encryption(r);
    check_storage_controller(r);
    check_memory(r);

    if (r->n_block == 0)
        add(r, "ready", "", PF_PASS, "This PC is ready",
            "Every safety check passed.", "");
}

int pf_is_go(const pf_report *r) { return r->n_block == 0; }

/* ── self-test ───────────────────────────────────────────────────── */
/* Everything above that decides something from bytes rather than from a
 * live machine is reachable from here, because the machines these
 * checks exist for are exactly the ones we do not have: a 4Kn disk, a
 * GPT dynamic disk, a desktop that reports its power state as unknown,
 * a VeraCrypt-encrypted boot sector. */
static int g_fail;

static void t(const char *name, int ok)
{
    if (!ok) g_fail++;
    printf("  %-56s %s\n", name, ok ? "ok" : "FAIL");
}

static void fill_bs(BYTE *sec, const char *oem, const char *fat54, const char *fat82)
{
    memset(sec, 0, 512);
    if (oem)   memcpy(sec + 3,  oem,   8);
    if (fat54) memcpy(sec + 54, fat54, 8);
    if (fat82) memcpy(sec + 82, fat82, 8);
}

int pf_selftest(void)
{
    BYTE sec[512];
    g_fail = 0;
    printf("\n  preflight self-test\n\n");

    fill_bs(sec, "NTFS    ", NULL, NULL);
    t("boot sector: NTFS", boot_sector_kind(sec) == PF_BS_NTFS);
    fill_bs(sec, "-FVE-FS-", NULL, NULL);
    t("boot sector: BitLocker -FVE-FS-", boot_sector_kind(sec) == PF_BS_FVE);
    fill_bs(sec, "MSDOS5.0", NULL, "FAT32   ");
    t("boot sector: FAT32 ESP", boot_sector_kind(sec) == PF_BS_FAT);
    fill_bs(sec, "MSDOS5.0", "FAT16   ", NULL);
    t("boot sector: FAT16", boot_sector_kind(sec) == PF_BS_FAT);
    for (int i = 0; i < 512; i++) sec[i] = (BYTE)(i * 37 + 11);   /* ciphertext */
    t("boot sector: encrypted -> OTHER", boot_sector_kind(sec) == PF_BS_OTHER);
    memset(sec, 0, 512);
    t("boot sector: zeroed MSR -> OTHER", boot_sector_kind(sec) == PF_BS_OTHER);

    t("bus: USB is removable",      bus_is_removable(BusTypeUsb));
    t("bus: FireWire is removable", bus_is_removable(BusType1394));
    t("bus: SD is removable",       bus_is_removable(BusTypeSd));
    t("bus: MMC is removable",      bus_is_removable(BusTypeMmc));
    t("bus: SATA is not",          !bus_is_removable(BusTypeSata));
    t("bus: NVMe is not",          !bus_is_removable(BusTypeNvme));
    t("bus: RAID is not",          !bus_is_removable(BusTypeRAID));

    {   /* the USB hard drive the old flag missed */
        pf_disk d; memset(&d, 0, sizeof d);
        d.bus_type = BusTypeUsb; d.removable_media = 0;
        d.is_removable = (d.bus_type >= 0 && bus_is_removable(d.bus_type)) || d.removable_media;
        t("USB HDD (RemovableMedia FALSE) is removable", d.is_removable == 1);
        d.bus_type = BusTypeSata; d.removable_media = 1;
        d.is_removable = (d.bus_type >= 0 && bus_is_removable(d.bus_type)) || d.removable_media;
        t("SATA card reader (RemovableMedia TRUE) is removable", d.is_removable == 1);
        d.bus_type = BusTypeSata; d.removable_media = 0;
        d.is_removable = (d.bus_type >= 0 && bus_is_removable(d.bus_type)) || d.removable_media;
        t("internal SATA disk is not removable", d.is_removable == 0);
    }

    {
        int ac, bat, pct;
        t("power: AC=1 -> AC",
          power_verdict(1, 1, 90, &ac, &bat, &pct) == PF_PWR_AC && ac == 1 && pct == 90);
        t("power: AC=0 with battery -> BATTERY",
          power_verdict(0, 1, 40, &ac, &bat, &pct) == PF_PWR_BATTERY && ac == 0);
        t("power: AC=255 no battery -> NO_BATTERY (desktop)",
          power_verdict(255, 128, 255, &ac, &bat, &pct) == PF_PWR_NO_BATTERY && bat == 0);
        t("power: AC=255 with battery -> UNKNOWN",
          power_verdict(255, 1, 80, &ac, &bat, &pct) == PF_PWR_UNKNOWN);
        t("power: BatteryFlag 255 is unknown, not 'no battery'",
          power_verdict(255, 255, 255, &ac, &bat, &pct) == PF_PWR_UNKNOWN && bat == -1);
        t("power: AC=0 with no battery is self-contradictory -> UNKNOWN",
          power_verdict(0, 128, 255, &ac, &bat, &pct) == PF_PWR_UNKNOWN);
        t("power: no battery reports no percentage",
          power_verdict(255, 128, 100, &ac, &bat, &pct) == PF_PWR_NO_BATTERY && pct == -1);
    }

    t("serial: exact match",            serial_eq("0123456789AB", "0123456789AB"));
    t("serial: case and spacing",       serial_eq("  ab-12-cd  ", "AB12CD"));
    t("serial: different sticks",      !serial_eq("0123456789AB", "0123456789AC"));
    t("serial: empty never matches",   !serial_eq("", ""));
    t("serial: too short never matches", !serial_eq("0", "0"));
    t("serial: prefix is not a match", !serial_eq("ABCD1234", "ABCD"));
    t("serial: usable",                 serial_usable("ABCD1234"));
    t("serial: not usable",            !serial_usable("  -  "));

    {
        GUID g = PF_GPT_LDM_DATA;
        t("gpt: LDM data GUID",     guid_eq(&g, &PF_GPT_LDM_DATA));
        t("gpt: LDM data != meta", !guid_eq(&g, &PF_GPT_LDM_META));
        g = PF_GPT_LDM_META;
        t("gpt: LDM metadata GUID", guid_eq(&g, &PF_GPT_LDM_META));
        g = PF_GPT_SPACES;
        t("gpt: Storage Spaces GUID", guid_eq(&g, &PF_GPT_SPACES));
        g = PF_GPT_ESP;
        t("gpt: ESP GUID",          guid_eq(&g, &PF_GPT_ESP));
        t("gpt: ESP is not LDM",   !guid_eq(&g, &PF_GPT_LDM_DATA));
        /* byte order, spelled out: AF9B60A0-1431-4F62-BC68-3311714A69AD */
        const BYTE want[16] = { 0xA0,0x60,0x9B,0xAF, 0x31,0x14, 0x62,0x4F,
                                0xBC,0x68,0x33,0x11,0x71,0x4A,0x69,0xAD };
        t("gpt: LDM data GUID byte order",
          memcmp(&PF_GPT_LDM_DATA, want, 16) == 0);
    }

    {   /* Partition tables we cannot make a test machine produce. */
        static BYTE lb[sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                       8 * sizeof(PARTITION_INFORMATION_EX)];
        DRIVE_LAYOUT_INFORMATION_EX *lay = (DRIVE_LAYOUT_INFORMATION_EX *)lb;
        pf_disk d;

        #define LAY_BEGIN(style, count) do {                                  \
            memset(lb, 0, sizeof lb); memset(&d, 0, sizeof d); d.bus_type = -1;\
            lay->PartitionStyle = (style); lay->PartitionCount = (count);      \
        } while (0)
        #define LAY_GPT(i, type, off, len) do {                               \
            lay->PartitionEntry[i].PartitionStyle = PARTITION_STYLE_GPT;       \
            lay->PartitionEntry[i].StartingOffset.QuadPart = (off);            \
            lay->PartitionEntry[i].PartitionLength.QuadPart = (len);           \
            lay->PartitionEntry[i].Gpt.PartitionType = (type);                 \
        } while (0)
        #define LAY_MBR(i, type, len) do {                                    \
            lay->PartitionEntry[i].PartitionStyle = PARTITION_STYLE_MBR;       \
            lay->PartitionEntry[i].PartitionLength.QuadPart = (len);           \
            lay->PartitionEntry[i].Mbr.PartitionType = (type);                 \
        } while (0)

        /* An ordinary UEFI Windows disk: ESP, MSR, Windows, OEM recovery. */
        static const GUID MSR   = { 0xE3C9E316, 0x0B5C, 0x4DB8,
                                    { 0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE } };
        static const GUID BASIC = { 0xEBD0A0A2, 0xB9E5, 0x4433,
                                    { 0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 } };
        LAY_BEGIN(PARTITION_STYLE_GPT, 4);
        LAY_GPT(0, PF_GPT_ESP, 1048576, 104857600);
        LAY_GPT(1, MSR,        105906176, 16777216);
        LAY_GPT(2, BASIC,      122683392, 500107862016ULL);
        LAY_GPT(3, BASIC,      500230545408ULL, 1073741824);
        scan_layout(lay, &d);
        t("layout: plain GPT Windows disk is not dynamic", !d.is_dynamic);
        t("layout: plain GPT Windows disk is not a storage space", !d.is_storage_space);
        t("layout: ESP found at its offset", d.esp_offset == 1048576);
        t("layout: ESP length recorded", d.esp_length == 104857600);
        t("layout: style reported as GPT", d.partition_style == 1);

        /* GPT dynamic disk: the case the 0x42-only test missed entirely. */
        LAY_BEGIN(PARTITION_STYLE_GPT, 3);
        LAY_GPT(0, PF_GPT_ESP,      1048576, 104857600);
        LAY_GPT(1, PF_GPT_LDM_META, 105906176, 1048576);
        LAY_GPT(2, PF_GPT_LDM_DATA, 106954752, 500107862016ULL);
        scan_layout(lay, &d);
        t("layout: GPT dynamic disk (LDM data + metadata)", d.is_dynamic == 1);

        LAY_BEGIN(PARTITION_STYLE_GPT, 1);
        LAY_GPT(0, PF_GPT_LDM_DATA, 1048576, 500107862016ULL);
        scan_layout(lay, &d);
        t("layout: GPT dynamic disk, LDM data alone", d.is_dynamic == 1);

        LAY_BEGIN(PARTITION_STYLE_GPT, 1);
        LAY_GPT(0, PF_GPT_LDM_META, 1048576, 1048576);
        scan_layout(lay, &d);
        t("layout: GPT dynamic disk, LDM metadata alone", d.is_dynamic == 1);

        /* Storage Spaces, both ways in. */
        LAY_BEGIN(PARTITION_STYLE_GPT, 1);
        LAY_GPT(0, PF_GPT_SPACES, 1048576, 500107862016ULL);
        scan_layout(lay, &d);
        t("layout: Storage Spaces pool member", d.is_storage_space == 1);
        t("layout: pool member is not called dynamic", !d.is_dynamic);

        LAY_BEGIN(PARTITION_STYLE_GPT, 1);
        LAY_GPT(0, BASIC, 1048576, 500107862016ULL);
        d.bus_type = BusTypeSpaces;
        scan_layout(lay, &d);
        t("layout: Storage Spaces virtual disk, by bus type", d.is_storage_space == 1);

        /* A zero-length entry is padding in a 128-entry GPT, not a
         * partition; counting it would be a false four-primaries. */
        LAY_BEGIN(PARTITION_STYLE_GPT, 8);
        LAY_GPT(0, PF_GPT_ESP, 1048576, 104857600);
        scan_layout(lay, &d);
        t("layout: empty GPT entries ignored", d.esp_offset == 1048576 && !d.is_dynamic);

        /* MBR: the 0x42 dynamic disk, and R8's full table. */
        LAY_BEGIN(PARTITION_STYLE_MBR, 4);
        LAY_MBR(0, 0x42, 500107862016ULL);
        scan_layout(lay, &d);
        t("layout: MBR dynamic disk (type 0x42)", d.is_dynamic == 1);
        t("layout: style reported as MBR", d.partition_style == 0);

        LAY_BEGIN(PARTITION_STYLE_MBR, 4);
        LAY_MBR(0, 0x07, 104857600);
        LAY_MBR(1, 0x07, 400000000000ULL);
        LAY_MBR(2, 0x27, 20000000000ULL);
        LAY_MBR(3, 0x07, 1073741824);
        scan_layout(lay, &d);
        t("layout: four MBR primaries counted (R8)", d.primary_partitions == 4);
        t("layout: four ordinary primaries are not dynamic", !d.is_dynamic);

        LAY_BEGIN(PARTITION_STYLE_MBR, 4);
        LAY_MBR(0, 0x07, 104857600);
        LAY_MBR(1, 0x07, 400000000000ULL);
        scan_layout(lay, &d);
        t("layout: empty MBR slots not counted", d.primary_partitions == 2);

        LAY_BEGIN(PARTITION_STYLE_RAW, 0);
        scan_layout(lay, &d);
        t("layout: unreadable table reports RAW", d.partition_style == 2);
        #undef LAY_BEGIN
        #undef LAY_GPT
        #undef LAY_MBR
    }

    {   /* $Bitmap: 64 clusters, in use at 0, 1, 5 and 63 */
        BYTE bits[8] = { 0x23, 0,0,0,0,0,0, 0x80 };
        uint64_t used = 0, end = 0;
        bitmap_scan(bits, 64, 0, &used, &end);
        t("bitmap: counts clusters in use", used == 4);
        t("bitmap: finds the last one",     end == 64);

        used = 0; end = 0;
        BYTE empty[8] = { 0,0,0,0,0,0,0,0 };
        bitmap_scan(empty, 64, 0, &used, &end);
        t("bitmap: empty run", used == 0 && end == 0);

        used = 0; end = 0;
        BYTE tail[2] = { 0x00, 0x04 };       /* bit 10 set, 12 bits valid */
        bitmap_scan(tail, 12, 1000, &used, &end);
        t("bitmap: partial byte at the end", used == 1 && end == 1011);

        /* The shape R7 is about: 30 GB free, all of it below a pagefile
         * parked at the end of the volume. */
        used = 0; end = 0;
        BYTE spread[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0x80 };
        bitmap_scan(spread, 64, 0, &used, &end);
        uint64_t cluster = 4096, total = 64;
        uint64_t offline = (total - used) * cluster;
        uint64_t online  = (total - end)  * cluster;
        t("bitmap: offline floor beats online floor", offline > online && online == 0);
    }

    {   /* R7, to scale: a 500 GB drive with 30 GB free, all of it
         * below a pagefile parked at the end. Windows offers 2 GB;
         * ntfsresize can move the pagefile and offers the lot. */
        const uint64_t G = 1024ULL * 1024 * 1024;
        uint64_t size = 500 * G, used = 470 * G, floor = 498 * G;
        uint64_t offline = size - used, online = size - floor;
        t("shrink: online ceiling is the R7 number",  online == 2 * G);
        t("shrink: offline ceiling is much larger",   offline == 30 * G);
        t("shrink: 30 GB free does not clear 28 GB once Windows keeps 8",
          shrink_giveable(offline) == 22 * G && shrink_giveable(offline) < PF_AUROS_NEED);
        t("shrink: 60 GB free does clear it",
          shrink_giveable(60 * G) == 52 * G && shrink_giveable(60 * G) >= PF_AUROS_NEED);
        t("shrink: exactly the reserve gives nothing", shrink_giveable(PF_WINDOWS_KEEP) == 0);
        t("shrink: less than the reserve never underflows",
          shrink_giveable(0) == 0 && shrink_giveable(1) == 0);
        t("shrink: free space alone would have said yes",
          (size - used) >= PF_AUROS_NEED);   /* the old check's answer */
    }

    {   /* Numbers the user is asked to act on. */
        char b[32];
        size_str(104857600ULL, b, sizeof b);
        t("size: a 100 MB ESP is not '0.1 GB'", !strcmp(b, "100 MB"));
        size_str(6291456ULL, b, sizeof b);
        t("size: 6 MB free on the ESP", !strcmp(b, "6.0 MB"));
        size_str(500107862016ULL, b, sizeof b);
        t("size: a 500 GB drive", !strcmp(b, "466 GB"));
        size_str(28ULL * 1024 * 1024 * 1024, b, sizeof b);
        t("size: what AurOS asks for", !strcmp(b, "28 GB"));
        size_str(0, b, sizeof b);
        t("size: nothing", !strcmp(b, "0 bytes"));
    }

    t("sector size: 512 ok",     sector_size_ok(512));
    t("sector size: 4096 ok",    sector_size_ok(4096));
    t("sector size: 0 refused", !sector_size_ok(0));
    t("sector size: 520 refused (not a power of two)", !sector_size_ok(520));
    t("sector size: 1 MB refused", !sector_size_ok(1048576));

    {
        BYTE track[2048];
        memset(track, 0xE9, sizeof track);
        t("loader scan: clean first track", !mem_find(track, sizeof track, "VeraCrypt"));
        memcpy(track + 1000, "VeraCrypt Boot Loader", 21);
        t("loader scan: VeraCrypt boot loader", mem_find(track, sizeof track, "VeraCrypt"));
        memset(track, 0, sizeof track);
        memcpy(track + 4, "TrueCrypt", 9);
        t("loader scan: TrueCrypt boot loader", mem_find(track, sizeof track, "TrueCrypt"));
    }

    {   /* the nomination, end to end, without a disk */
        pf_set_recovery_stick("AA11BB22");
        t("recovery stick: nomination is remembered",
          serial_eq(pf_recovery_stick(), "aa11bb22"));
        t("recovery stick: another disk is not it",
          !serial_eq("ZZ99", pf_recovery_stick()));
        pf_set_recovery_stick(NULL);
        t("recovery stick: withdrawn", pf_recovery_stick()[0] == '\0');
        t("recovery stick: no nomination matches nothing",
          !serial_eq("AA11BB22", pf_recovery_stick()));
    }

    printf("\n  %s\n\n", g_fail ? "SELF-TEST FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}

#ifdef PF_SELFTEST
int main(void) { return pf_selftest(); }
#endif
