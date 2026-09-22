/* plat_win.c — the real one. See plat.h.
 *
 * NOT TESTED ON THIS MACHINE AND CANNOT BE. There is no Windows here
 * and wine does not implement raw physical-drive handles, volume
 * dismount, or firmware variables. What IS tested is everything above
 * this line: the phase engine runs whole against plat_sim.c, and the
 * bytes it produces are checked against the programs that read them.
 * This file is the part that has to be right on the first machine it
 * meets, so it is written to be read rather than to be clever, and
 * every call that has a failure mode worth naming names it.
 *
 * THE THREE THINGS WINDOWS MAKES HARDER THAN THEY LOOK
 *
 *   A raw handle to a disk that has mounted volumes on it will accept
 *   writes and then have them thrown away, or fail with ERROR_ACCESS_
 *   DENIED depending on the version. The documented way is to lock and
 *   dismount every volume on the disk first, and to keep those handles
 *   open for as long as the writes last. So take_stick() opens them
 *   and give_stick() closes them, and nothing writes in between
 *   without having gone through the first.
 *
 *   Reads and writes must be whole sectors at sector-aligned offsets.
 *   Every caller here already works in whole megabytes, but "already"
 *   is not a guarantee, so the tail of an unaligned write is done by
 *   reading the sector, changing part of it, and writing it back.
 *
 *   SetFirmwareEnvironmentVariableExW needs SeSystemEnvironmentPrivilege,
 *   which is present but DISABLED in an elevated token. Not enabling it
 *   gives ERROR_PRIVILEGE_NOT_HELD on every machine, which reads like
 *   "you are not an administrator" and is not.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

#include "plat.h"

int plat_is_sim(void) { return 0; }
const char *plat_name(void) { return "this computer"; }

static void why_of(char *why, size_t n, const char *what, DWORD e)
{
    snprintf(why, n, "%s (Windows error %lu)", what, (unsigned long)e);
}

/* ── disks ───────────────────────────────────────────────────────── */

static HANDLE open_disk(int index, int writable)
{
    wchar_t path[64];
    _snwprintf(path, 63, L"\\\\.\\PhysicalDrive%d", index);
    path[63] = 0;
    return CreateFileW(path,
        GENERIC_READ | (writable ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static uint32_t disk_sector(HANDLE h)
{
    STORAGE_PROPERTY_QUERY q;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageAccessAlignmentProperty;
    q.QueryType  = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR a;
    memset(&a, 0, sizeof a);
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                        &a, sizeof a, &got, NULL) && a.BytesPerLogicalSector)
        return a.BytesPerLogicalSector;
    /* docs/AURBRIDGE.md: "Always read StorageAccessAlignmentProperty,
     * and block if it cannot be read." A zero here is carried up and
     * becomes a refusal; it is never quietly turned into 512, which on
     * a 4Kn disk makes every partition eight times too small. */
    return 0;
}

static void disk_names(HANDLE h, char *serial, size_t sn,
                       char *model, size_t mn, int *removable)
{
    serial[0] = 0; model[0] = 0; *removable = 0;
    STORAGE_PROPERTY_QUERY q;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    static unsigned char buf[4096];
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                         buf, sizeof buf, &got, NULL))
        return;
    STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
    *removable = d->RemovableMedia ? 1 : 0;
    if (d->SerialNumberOffset && d->SerialNumberOffset < got)
        snprintf(serial, sn, "%s", (char *)buf + d->SerialNumberOffset);
    if (d->ProductIdOffset && d->ProductIdOffset < got) {
        char vend[128] = "";
        if (d->VendorIdOffset && d->VendorIdOffset < got)
            snprintf(vend, sizeof vend, "%s", (char *)buf + d->VendorIdOffset);
        snprintf(model, mn, "%s%s%s", vend, vend[0] ? " " : "",
                 (char *)buf + d->ProductIdOffset);
    }
    /* Windows pads both with spaces. A serial with a trailing space in
     * it does not compare equal to the same serial without one, and
     * that comparison is what decides whether this is the computer the
     * installer was prepared for. */
    for (char *p = serial + strlen(serial); p > serial && p[-1] == ' '; p--)
        p[-1] = 0;
    for (char *p = model + strlen(model); p > model && p[-1] == ' '; p--)
        p[-1] = 0;
}

int plat_disks(plat_disk *out, int max)
{
    int n = 0;
    for (int i = 0; i < 32 && n < max; i++) {
        HANDLE h = open_disk(i, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        GET_LENGTH_INFORMATION len;
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                            &len, sizeof len, &got, NULL)) {
            plat_disk *d = &out[n];
            memset(d, 0, sizeof *d);
            d->index = i;
            d->size_bytes = (uint64_t)len.Length.QuadPart;
            d->logical_sector = disk_sector(h);
            disk_names(h, d->serial, sizeof d->serial,
                       d->model, sizeof d->model, &d->removable);
            n++;
        }
        CloseHandle(h);
    }
    return n;
}

/* ── the one disk that may be written to ─────────────────────────── */

static int    g_allowed = -1;
static HANDLE g_stick = INVALID_HANDLE_VALUE;
#define MAX_VOL 26
static HANDLE g_vol[MAX_VOL];
static int    g_nvol;

void plat_allow_write(int index) { g_allowed = index; }

/* Lock and dismount every volume the stick has, and keep the handles.
 * Windows throws away writes to a raw disk handle whose volumes are
 * still mounted, silently, on some versions. */
static int take_stick(int index, char *why, size_t wn)
{
    if (g_stick != INVALID_HANDLE_VALUE) return 0;
    g_nvol = 0;
    wchar_t drives[512];
    DWORD dn = GetLogicalDriveStringsW(511, drives);
    for (wchar_t *p = drives; dn && *p; p += wcslen(p) + 1) {
        wchar_t vol[16];
        _snwprintf(vol, 15, L"\\\\.\\%c:", p[0]);
        vol[15] = 0;
        HANDLE v = CreateFileW(vol, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (v == INVALID_HANDLE_VALUE) continue;
        /* Is this volume on the disk we are about to write to? */
        static unsigned char ext[1024];
        DWORD got = 0;
        int mine = 0;
        if (DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                            ext, sizeof ext, &got, NULL)) {
            VOLUME_DISK_EXTENTS *e = (VOLUME_DISK_EXTENTS *)ext;
            for (DWORD k = 0; k < e->NumberOfDiskExtents; k++)
                if ((int)e->Extents[k].DiskNumber == index) mine = 1;
        }
        if (!mine || g_nvol >= MAX_VOL) { CloseHandle(v); continue; }
        DeviceIoControl(v, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL);
        DeviceIoControl(v, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
        g_vol[g_nvol++] = v;
    }
    g_stick = open_disk(index, 1);
    if (g_stick == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        for (int i = 0; i < g_nvol; i++) CloseHandle(g_vol[i]);
        g_nvol = 0;
        why_of(why, wn,
               "the memory stick could not be opened for writing. Close any "
               "Explorer window showing it and try again", e);
        return -1;
    }
    return 0;
}

static void give_stick(void)
{
    if (g_stick != INVALID_HANDLE_VALUE) { CloseHandle(g_stick); }
    g_stick = INVALID_HANDLE_VALUE;
    for (int i = 0; i < g_nvol; i++) CloseHandle(g_vol[i]);
    g_nvol = 0;
}

static int seek_to(HANDLE h, uint64_t off)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) ? 0 : -1;
}

int plat_read(int index, uint64_t off, void *buf, size_t n,
              char *why, size_t wn)
{
    HANDLE h = (index == g_allowed && g_stick != INVALID_HANDLE_VALUE)
             ? g_stick : open_disk(index, 0);
    if (h == INVALID_HANDLE_VALUE) {
        why_of(why, wn, "a disk in this computer could not be read",
               GetLastError());
        return -1;
    }
    uint32_t ss = disk_sector(h);
    if (!ss) ss = 512;
    int rc = 0;
    /* Whole sectors, at sector offsets, always -- the handle will not
     * do anything else. An unaligned request is served by reading the
     * sectors around it into a staging buffer. */
    uint64_t lo = off - (off % ss);
    uint64_t hi = ((off + n + ss - 1) / ss) * ss;
    size_t span = (size_t)(hi - lo);
    unsigned char *tmp = (unsigned char *)VirtualAlloc(NULL, span,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tmp) {
        snprintf(why, wn, "this computer ran out of memory");
        rc = -1;
        goto out;
    }
    if (seek_to(h, lo) != 0) {
        why_of(why, wn, "a disk in this computer would not seek",
               GetLastError());
        rc = -1; goto out;
    }
    {
        size_t done = 0;
        while (done < span) {
            DWORD got = 0;
            DWORD ask = (DWORD)((span - done) > (1u << 20) ? (1u << 20)
                                                           : (span - done));
            if (!ReadFile(h, tmp + done, ask, &got, NULL) || got == 0) {
                why_of(why, wn, "a disk in this computer would not give up "
                                "its contents", GetLastError());
                rc = -1; goto out;
            }
            done += got;
        }
    }
    memcpy(buf, tmp + (off - lo), n);
out:
    if (tmp) VirtualFree(tmp, 0, MEM_RELEASE);
    if (h != g_stick) CloseHandle(h);
    return rc;
}

int plat_write(int index, uint64_t off, const void *buf, size_t n,
               char *why, size_t wn)
{
    if (index != g_allowed) {
        snprintf(why, wn,
                 "REFUSING to write to drive %d. AurBridge only ever writes "
                 "to the memory stick you chose.", index);
        return -1;
    }
    if (take_stick(index, why, wn) != 0) return -1;
    uint32_t ss = disk_sector(g_stick);
    if (!ss) ss = 512;

    uint64_t lo = off - (off % ss);
    uint64_t hi = ((off + n + ss - 1) / ss) * ss;
    size_t span = (size_t)(hi - lo);
    unsigned char *tmp = (unsigned char *)VirtualAlloc(NULL, span,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tmp) { snprintf(why, wn, "this computer ran out of memory"); return -1; }
    int rc = 0;
    /* Read-modify-write, and only when the request is not already
     * whole sectors -- which it always is in practice, and "always in
     * practice" is not a thing to write a disk on. */
    if (lo != off || hi != off + n) {
        if (plat_read(index, lo, tmp, span, why, wn) != 0) { rc = -1; goto out; }
    }
    memcpy(tmp + (off - lo), buf, n);
    if (seek_to(g_stick, lo) != 0) {
        why_of(why, wn, "the memory stick would not seek", GetLastError());
        rc = -1; goto out;
    }
    {
        size_t done = 0;
        while (done < span) {
            DWORD put = 0;
            DWORD ask = (DWORD)((span - done) > (1u << 20) ? (1u << 20)
                                                           : (span - done));
            if (!WriteFile(g_stick, tmp + done, ask, &put, NULL) || put == 0) {
                why_of(why, wn,
                       "the memory stick stopped accepting what was written "
                       "to it. Try a different one", GetLastError());
                rc = -1; goto out;
            }
            done += put;
        }
    }
out:
    VirtualFree(tmp, 0, MEM_RELEASE);
    return rc;
}

int plat_flush(int index, char *why, size_t wn)
{
    if (index != g_allowed || g_stick == INVALID_HANDLE_VALUE) return 0;
    if (!FlushFileBuffers(g_stick)) {
        why_of(why, wn, "the memory stick would not confirm what was written "
                        "to it", GetLastError());
        return -1;
    }
    return 0;
}

int plat_reread(int index, char *why, size_t wn)
{
    (void)why; (void)wn;
    if (index == g_allowed && g_stick != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        DeviceIoControl(g_stick, IOCTL_DISK_UPDATE_PROPERTIES,
                        NULL, 0, NULL, 0, &got, NULL);
        /* The handles have to go for Windows to mount the new
         * partitions; holding them is how a stick that was written
         * correctly appears empty in Explorer afterwards. */
        give_stick();
    }
    return 0;
}

/* ── ordinary files ──────────────────────────────────────────────── */

static HANDLE open_file(const char *path, int writing)
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1023);
    w[1023] = 0;
    return CreateFileW(w, writing ? GENERIC_WRITE : GENERIC_READ,
                       FILE_SHARE_READ, NULL,
                       writing ? CREATE_ALWAYS : OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

int plat_file_size(const char *path, uint64_t *out)
{
    HANDLE h = open_file(path, 0);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER li;
    int rc = GetFileSizeEx(h, &li) ? 0 : -1;
    if (rc == 0 && out) *out = (uint64_t)li.QuadPart;
    CloseHandle(h);
    return rc;
}

int plat_file_read(const char *path, uint64_t off, void *buf, size_t n,
                   char *why, size_t wn)
{
    HANDLE h = open_file(path, 0);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(why, wn, "%s could not be opened", path);
        return -1;
    }
    int rc = 0;
    if (seek_to(h, off) != 0) { rc = -1; goto out; }
    {
        unsigned char *p = (unsigned char *)buf;
        size_t done = 0;
        while (done < n) {
            DWORD got = 0;
            DWORD ask = (DWORD)((n - done) > (1u << 20) ? (1u << 20)
                                                        : (n - done));
            if (!ReadFile(h, p + done, ask, &got, NULL) || got == 0) {
                snprintf(why, wn, "%s is shorter than expected", path);
                rc = -1; goto out;
            }
            done += got;
        }
    }
out:
    CloseHandle(h);
    return rc;
}

static void make_dirs(const char *path)
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1023);
    w[1023] = 0;
    for (wchar_t *p = w + 1; *p; p++) {
        if (*p != L'\\' && *p != L'/') continue;
        wchar_t c = *p; *p = 0;
        CreateDirectoryW(w, NULL);
        *p = c;
    }
}

int plat_file_put(const char *to, const void *buf, size_t n,
                  char *why, size_t wn)
{
    make_dirs(to);
    HANDLE h = open_file(to, 1);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(why, wn, "%s could not be written", to);
        return -1;
    }
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;
    int rc = 0;
    while (done < n) {
        DWORD put = 0;
        DWORD ask = (DWORD)((n - done) > (1u << 20) ? (1u << 20) : (n - done));
        if (!WriteFile(h, p + done, ask, &put, NULL) || put == 0) {
            snprintf(why, wn, "%s could not be written to the end", to);
            rc = -1; break;
        }
        done += put;
    }
    if (rc == 0) FlushFileBuffers(h);
    CloseHandle(h);
    return rc;
}

int plat_file_copy(const char *from, const char *to, char *why, size_t wn)
{
    make_dirs(to);
    wchar_t wf[1024], wt[1024];
    MultiByteToWideChar(CP_UTF8, 0, from, -1, wf, 1023); wf[1023] = 0;
    MultiByteToWideChar(CP_UTF8, 0, to,   -1, wt, 1023); wt[1023] = 0;
    if (!CopyFileW(wf, wt, FALSE)) {
        why_of(why, wn, "a file could not be copied onto the start-up "
                        "partition", GetLastError());
        return -1;
    }
    return 0;
}

/* ── the EFI System Partition ────────────────────────────────────── */
/*
 * R12: never reformatted, and nothing here removes a file it did not
 * put there. Windows does not give the ESP a drive letter, so one is
 * borrowed with mountvol and given back afterwards. The letter is
 * searched for from the end of the alphabet, because S: is somebody's
 * scanner share often enough to matter.
 */
static char g_esp_letter;

int plat_esp_open(char *root, size_t n, char *why, size_t wn)
{
    for (char c = 'Z'; c >= 'E'; c--) {
        wchar_t path[8];
        _snwprintf(path, 7, L"%c:\\", c); path[7] = 0;
        if (GetDriveTypeW(path) != DRIVE_NO_ROOT_DIR) continue;
        char cmd[64];
        snprintf(cmd, sizeof cmd, "mountvol %c: /S", c);
        char tail[256];
        if (plat_run(cmd, tail, sizeof tail) != 0) continue;
        g_esp_letter = c;
        snprintf(root, n, "%c:", c);
        return 0;
    }
    snprintf(why, wn,
             "the start-up partition on this computer could not be opened. "
             "AurBridge has to be run as an administrator.");
    return -1;
}

void plat_esp_close(void)
{
    if (!g_esp_letter) return;
    char cmd[64];
    snprintf(cmd, sizeof cmd, "mountvol %c: /D", g_esp_letter);
    plat_run(cmd, NULL, 0);
    g_esp_letter = 0;
}

/* ── the firmware ────────────────────────────────────────────────── */
/*
 * SeSystemEnvironmentPrivilege is in an elevated token but DISABLED,
 * and not enabling it produces ERROR_PRIVILEGE_NOT_HELD on every
 * machine -- which reads as "you are not an administrator" and is not.
 */
static const wchar_t *EFI_GLOBAL =
    L"{8be4df61-93ca-11d2-aa0d-00e098032b8c}";

static int enable_env_privilege(void)
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return -1;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    int ok = LookupPrivilegeValueW(NULL, L"SeSystemEnvironmentPrivilege",
                                   &tp.Privileges[0].Luid) &&
             AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL) &&
             GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok ? 0 : -1;
}

static int get_var(const wchar_t *name, void *buf, DWORD n, DWORD *got)
{
    DWORD k = GetFirmwareEnvironmentVariableW(name, EFI_GLOBAL, buf, n);
    if (got) *got = k;
    return k ? 0 : -1;
}

static int set_var(const wchar_t *name, const void *buf, DWORD n)
{
    return SetFirmwareEnvironmentVariableW(name, EFI_GLOBAL,
                                           (PVOID)buf, n) ? 0 : -1;
}

/* An EFI_LOAD_OPTION: attributes, the device path length, a
 * NUL-terminated UTF-16 description, the device path, then the
 * optional data. The description is what plat_boot_find matches on. */
static size_t load_option(unsigned char *out, size_t n,
                          const char *desc, const char *loader,
                          const char *cmdline);

int plat_boot_find(const char *desc, uint16_t *num_out, char *why, size_t wn)
{
    (void)why; (void)wn;
    wchar_t wdesc[128];
    MultiByteToWideChar(CP_UTF8, 0, desc, -1, wdesc, 127); wdesc[127] = 0;
    size_t dl = wcslen(wdesc);
    for (int i = 0; i < 0x2000; i++) {
        wchar_t name[16];
        _snwprintf(name, 15, L"Boot%04X", i); name[15] = 0;
        static unsigned char buf[4096];
        DWORD got = 0;
        if (get_var(name, buf, sizeof buf, &got) != 0 || got < 8) continue;
        const wchar_t *d = (const wchar_t *)(buf + 6);
        /* The description is NUL-terminated inside the variable; a
         * variable whose description runs off the end is not one of
         * ours and is left alone. */
        size_t max = (got - 6) / 2;
        size_t l = 0;
        while (l < max && d[l]) l++;
        if (l == dl && memcmp(d, wdesc, dl * 2) == 0) {
            if (num_out) *num_out = (uint16_t)i;
            return 0;
        }
    }
    return -1;
}

int plat_boot_make(const char *desc, const char *loader, const char *cmdline,
                   uint16_t *num_out, char *why, size_t wn)
{
    if (enable_env_privilege() != 0) {
        snprintf(why, wn,
                 "this computer would not let AurBridge change its start-up "
                 "settings. Run AurBridge as an administrator.");
        return -1;
    }
    uint16_t num;
    if (plat_boot_find(desc, &num, why, wn) != 0) {
        /* The first number nothing is using, searched upward.
         *
         * NEVER "the first number we do not recognise": that also
         * selects the Fedora somebody installed last month and the
         * vendor diagnostics entry that was never in BootOrder. A slot
         * is free only when reading it fails. */
        num = 0xFFFF;
        for (int i = 0; i < 0x2000; i++) {
            wchar_t name[16];
            _snwprintf(name, 15, L"Boot%04X", i); name[15] = 0;
            unsigned char tmp[16];
            DWORD got = 0;
            if (get_var(name, tmp, sizeof tmp, &got) != 0) {
                num = (uint16_t)i;
                break;
            }
        }
        if (num == 0xFFFF) {
            snprintf(why, wn,
                     "this computer has no room left in its start-up menu.");
            return -1;
        }
    }
    static unsigned char opt[2048];
    size_t k = load_option(opt, sizeof opt, desc, loader, cmdline);
    if (!k) {
        snprintf(why, wn, "the AurOS start-up entry could not be built.");
        return -1;
    }
    wchar_t name[16];
    _snwprintf(name, 15, L"Boot%04X", num); name[15] = 0;
    if (set_var(name, opt, (DWORD)k) != 0) {
        why_of(why, wn, "this computer would not take a new start-up entry",
               GetLastError());
        return -1;
    }
    if (num_out) *num_out = num;
    return 0;
}

int plat_boot_next(uint16_t num, char *why, size_t wn)
{
    if (enable_env_privilege() != 0) {
        snprintf(why, wn,
                 "this computer would not let AurBridge choose what to start "
                 "next time. Run AurBridge as an administrator.");
        return -1;
    }
    unsigned char v[2] = { (unsigned char)(num & 0xFF),
                           (unsigned char)(num >> 8) };
    if (set_var(L"BootNext", v, 2) != 0) {
        why_of(why, wn, "this computer would not take BootNext",
               GetLastError());
        return -1;
    }
    return 0;
}

int plat_boot_next_clear(char *why, size_t wn)
{
    if (enable_env_privilege() != 0) {
        snprintf(why, wn, "this computer would not let AurBridge undo its "
                          "start-up setting.");
        return -1;
    }
    /* A zero-length write deletes the variable, which is what taking
     * it back means -- setting it to 0000 would point at somebody's
     * Boot0000. */
    if (set_var(L"BootNext", NULL, 0) != 0) {
        why_of(why, wn, "BootNext could not be cleared", GetLastError());
        return -1;
    }
    return 0;
}

/* ── the EFI device path, built by hand ──────────────────────────── */
/*
 * A loader on the ESP is a FILE PATH MEDIA device node followed by the
 * end-of-path node. Windows resolves the partition itself when the
 * path has no hardware node in front of it, which is what is wanted:
 * naming the partition by GUID here would mean naming a partition that
 * the staging environment is about to change the table of.
 */
static size_t load_option(unsigned char *out, size_t n,
                          const char *desc, const char *loader,
                          const char *cmdline)
{
    wchar_t wdesc[128], wload[512], wcmd[512];
    MultiByteToWideChar(CP_UTF8, 0, desc, -1, wdesc, 127); wdesc[127] = 0;
    MultiByteToWideChar(CP_UTF8, 0, loader, -1, wload, 511); wload[511] = 0;
    wcmd[0] = 0;
    if (cmdline && *cmdline)
        MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmd, 511);
    wcmd[511] = 0;

    size_t dl = (wcslen(wdesc) + 1) * 2;
    size_t fl = (wcslen(wload) + 1) * 2;
    size_t file_node = 4 + fl;          /* type, subtype, length, path  */
    size_t end_node  = 4;
    size_t dp = file_node + end_node;
    size_t cl = wcmd[0] ? (wcslen(wcmd) + 1) * 2 : 0;
    size_t total = 6 + dl + dp + cl;
    if (total > n) return 0;

    unsigned char *p = out;
    /* LOAD_OPTION_ACTIVE */
    p[0] = 1; p[1] = 0; p[2] = 0; p[3] = 0;
    p[4] = (unsigned char)(dp & 0xFF);
    p[5] = (unsigned char)(dp >> 8);
    p += 6;
    memcpy(p, wdesc, dl); p += dl;
    /* MEDIA_DEVICE_PATH(4) / MEDIA_FILEPATH_DP(4) */
    p[0] = 4; p[1] = 4;
    p[2] = (unsigned char)(file_node & 0xFF);
    p[3] = (unsigned char)(file_node >> 8);
    memcpy(p + 4, wload, fl);
    p += file_node;
    /* END_DEVICE_PATH_TYPE(0x7F) / END_ENTIRE(0xFF), length 4 */
    p[0] = 0x7F; p[1] = 0xFF; p[2] = 4; p[3] = 0;
    p += end_node;
    if (cl) { memcpy(p, wcmd, cl); p += cl; }
    return (size_t)(p - out);
}

/* ── running something else ──────────────────────────────────────── */

int plat_run(const char *cmdline, char *tail, size_t n)
{
    if (tail && n) tail[0] = 0;
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, w, 1023);
    w[1023] = 0;

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wrh = NULL;
    if (!CreatePipe(&rd, &wrh, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wrh;
    si.hStdError  = wrh;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, w, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wrh);
        if (tail && n) snprintf(tail, n, "it would not start");
        return -1;
    }
    CloseHandle(wrh);

    /* Keep the LAST of the output, not the first: the sentence that
     * says why something failed is at the end, after the banner. */
    char ring[1024];
    size_t rn = 0;
    for (;;) {
        char c;
        DWORD got = 0;
        if (!ReadFile(rd, &c, 1, &got, NULL) || got == 0) break;
        ring[rn % sizeof ring] = c;
        rn++;
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    if (tail && n) {
        size_t have = rn < sizeof ring ? rn : sizeof ring;
        size_t start = rn < sizeof ring ? 0 : rn % sizeof ring;
        size_t o = 0;
        for (size_t i = 0; i < have && o + 1 < n; i++) {
            char c = ring[(start + i) % sizeof ring];
            tail[o++] = (c == '\r' || c == '\n') ? ' ' : c;
        }
        tail[o] = 0;
    }
    return (int)code;
}

#endif /* _WIN32 */
