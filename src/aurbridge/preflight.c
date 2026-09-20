/* AurBridge preflight — see preflight.h for the design rule.
 * Strictly read-only. R-numbers reference docs/research/red-team.md. */

#include "preflight.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <winioctl.h>

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
static void check_power(pf_report *r)
{
    SYSTEM_POWER_STATUS ps;
    r->battery_percent = -1;
    r->on_ac_power = -1;
    if (!GetSystemPowerStatus(&ps)) return;

    r->on_ac_power = (ps.ACLineStatus == 1) ? 1 : (ps.ACLineStatus == 0 ? 0 : -1);
    if (ps.BatteryFlag != 128 && ps.BatteryLifePercent != 255)
        r->battery_percent = ps.BatteryLifePercent;

    if (r->on_ac_power != 1) {
        /* The partition table is not journalled. Power loss between data
         * movement and the table write cross-shreds the disk. */
        add(r, "not-on-ac", "R6", PF_BLOCK, "This PC is running on battery",
            "Losing power partway through changing the disk layout can leave the disk in "
            "a state where neither Windows nor AurOS can start.",
            "Plug in the charger, then click Re-check.");
    } else if (r->battery_percent >= 0 && r->battery_percent < 50) {
        add(r, "battery-low", "R6", PF_BLOCK, "Battery is below 50%",
            "If the power cable is pulled out mid-install, the battery must be able to "
            "carry the machine to a safe stopping point.",
            "Leave it charging until the battery reaches at least 50%, then click Re-check.");
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

static void query_disk_props(HANDLE h, pf_disk *d)
{
    STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery, {0} };
    BYTE buf[2048]; DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                        buf, sizeof buf, &ret, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *sd = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        d->is_removable = sd->RemovableMedia ? 1 : 0;
        char vendor[64] = "", product[96] = "";
        desc_string(buf, sd->VendorIdOffset,  vendor,  sizeof vendor);
        desc_string(buf, sd->ProductIdOffset, product, sizeof product);
        desc_string(buf, sd->SerialNumberOffset, d->serial, sizeof d->serial);
        snprintf(d->model, sizeof d->model, "%s%s%s",
                 vendor, (vendor[0] && product[0]) ? " " : "", product);
    }

    GET_LENGTH_INFORMATION li;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &ret, NULL))
        d->size_bytes = (uint64_t)li.Length.QuadPart;
}

static void query_layout(HANDLE h, pf_disk *d, pf_report *r)
{
    BYTE buf[16384]; DWORD ret = 0;
    d->partition_style = 2;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
                         buf, sizeof buf, &ret, NULL))
        return;

    DRIVE_LAYOUT_INFORMATION_EX *lay = (DRIVE_LAYOUT_INFORMATION_EX *)buf;
    d->partition_style = (lay->PartitionStyle == PARTITION_STYLE_GPT) ? 1 :
                         (lay->PartitionStyle == PARTITION_STYLE_MBR) ? 0 : 2;

    if (lay->PartitionStyle == PARTITION_STYLE_MBR) {
        int primaries = 0;
        for (DWORD i = 0; i < lay->PartitionCount && i < 128; i++) {
            PARTITION_INFORMATION_EX *p = &lay->PartitionEntry[i];
            if (p->PartitionLength.QuadPart == 0) continue;
            primaries++;
            /* R10: MBR type 0x42 is the Logical Disk Manager. The real
             * layout lives in an LDM database and the table is decorative;
             * writing based on this view destroys the volume set. */
            if (p->Mbr.PartitionType == 0x42 && d->is_system)
                add(r, "dynamic-disk", "R10", PF_BLOCK, "This disk is a Windows dynamic disk",
                    "Dynamic disks store their real layout in a separate database, so the "
                    "normal partition table cannot be trusted. Changing it would destroy "
                    "the volumes on this disk.",
                    "AurOS cannot install onto a dynamic disk. Converting back to a basic "
                    "disk is a destructive operation you should only do with a full backup.");
        }
        d->primary_partitions = primaries;
    }
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
                } else {
                    spanned_reported = 1;
                    add(r, "spanned-system-volume", "R10", PF_BLOCK,
                        "Windows is spread across more than one disk",
                        "This Windows installation spans multiple physical disks "
                        "(a spanned, striped or mirrored volume). Its layout cannot be "
                        "changed safely.",
                        "AurOS cannot install on this configuration.");
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
        query_layout(h, d, r);
        query_smart(h, d);
        CloseHandle(h);
        r->n_disks++;

        char det[512];
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
        }

        if (d->is_system && d->size_bytes == 0)
            add(r, "system-disk-unreadable", "R11", PF_BLOCK,
                "The Windows drive could not be read properly",
                "AurBridge could not read the size of the disk Windows is installed on.",
                "AurOS will not change a disk it cannot fully read. Nothing has been "
                "changed.");

        /* R11: an attached external drive is how installers write to the
         * wrong target. Cheap to refuse, prevents a whole category. */
        if (d->is_removable && !d->is_system) {
            snprintf(det, sizeof det, "Removable drive %d (%s) is connected.", i, d->model);
            add(r, "removable-attached", "R11", PF_BLOCK, "Unplug external drives first",
                det,
                "Disconnect every USB stick, memory card and external hard drive, then "
                "click Re-check. This makes it impossible to write to the wrong disk.");
        }
    }

    if (r->n_disks == 0)
        add(r, "no-disks", "", PF_BLOCK, "No disks could be read",
            "AurBridge could not open any physical drive.",
            "Make sure you started AurBridge as administrator.");
}

/* ── volumes: BitLocker, dirty bit, free space ───────────────────── */
static int volume_is_bitlocker(const char *drive)
{
    /* A BitLocker volume carries the "-FVE-FS-" signature where NTFS
     * would put its OEM id. Reading one sector is enough and needs no
     * COM/WMI, which keeps preflight dependency-free. */
    char path[16];
    snprintf(path, sizeof path, "\\\\.\\%s", drive);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    BYTE sec[512]; DWORD got = 0;
    int res = -1;
    if (ReadFile(h, sec, sizeof sec, &got, NULL) && got >= 512)
        res = (memcmp(sec + 3, "-FVE-FS-", 8) == 0) ? 1 : 0;
    CloseHandle(h);
    return res;
}

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

        if (v->bitlocker == 1) {
            char det[512];
            snprintf(det, sizeof det,
                "Drive %s is encrypted with BitLocker.%s", drive,
                is_sys ? " This is the drive Windows is installed on." : "");
            /* R1: the highest-consequence item in the product. Changing
             * the boot path changes the TPM PCRs; without the recovery
             * key the preserved Windows partition is ciphertext forever. */
            add(r, is_sys ? "bitlocker-system" : "bitlocker-other", "R1", PF_BLOCK,
                "This drive is encrypted with BitLocker", det,
                "Before anything can change, you need your BitLocker recovery key -- a "
                "48-digit number. Find it at aka.ms/myrecoverykey while this PC still "
                "works, or ask whoever set up this PC. AurBridge will ask you to type "
                "part of it back to prove you have it. Without it, changing the disk can "
                "lock you out of your own files permanently.");
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

            if (strcmp(v->fs, "NTFS") != 0 && v->fs[0])
                add(r, "system-not-ntfs", "", PF_BLOCK, "Unexpected filesystem on the Windows drive",
                    "The Windows drive is not formatted as NTFS.", "AurOS cannot resize it.");

            /* Need room for AurOS plus the recovery partition plus slack. */
            const uint64_t NEED = 28ULL * 1024 * 1024 * 1024;
            if (v->free_bytes && v->free_bytes < NEED) {
                char det[512];
                snprintf(det, sizeof det,
                    "Drive %s has %.1f GB free. AurOS needs about %.0f GB: room for the "
                    "system, your files, and a rescue partition that can put Windows back.",
                    drive, (double)v->free_bytes / 1073741824.0,
                    (double)NEED / 1073741824.0);
                add(r, "insufficient-space", "R7", PF_BLOCK, "Not enough free space", det,
                    "Empty the Recycle Bin, then use Windows' Disk Cleanup, or move some "
                    "large files to an external drive. Then click Re-check.");
            }
        }
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
    r->secure_boot = -1;
    r->system_disk = -1;

    check_admin(r);
    if (!r->is_admin) return;      /* everything below needs raw disk access */

    check_firmware(r);
    check_power(r);
    check_pending_reboot(r);
    check_fast_startup(r);
    check_disks(r);
    check_volumes(r);
    check_storage_controller(r);
    check_memory(r);

    if (r->n_block == 0)
        add(r, "ready", "", PF_PASS, "This PC is ready",
            "Every safety check passed.", "");
}

int pf_is_go(const pf_report *r) { return r->n_block == 0; }
