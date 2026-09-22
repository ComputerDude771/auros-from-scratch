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

static HANDLE open_disk_flags(int index, int writable, DWORD flags)
{
    wchar_t path[64];
    _snwprintf(path, 63, L"\\\\.\\PhysicalDrive%d", index);
    path[63] = 0;
    return CreateFileW(path,
        GENERIC_READ | (writable ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, flags, NULL);
}

static HANDLE open_disk(int index, int writable)
{ return open_disk_flags(index, writable, 0); }

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
    /* ZEROED, AND ON THE STACK. It was a shared static reused across
     * every disk, so a descriptor whose serial was not NUL-terminated
     * within `got` picked up the PREVIOUS disk's bytes -- and the
     * serial is what decides which computer the installer was prepared
     * for. Four kilobytes of stack is cheaper than that. */
    unsigned char buf[4096];
    memset(buf, 0, sizeof buf);
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
static void give_stick(void);

static int g_stick_index = -1;

static int take_stick(int index, char *why, size_t wn)
{
    /* THE INDEX IS CHECKED, and it was not. `if (g_stick != INVALID)
     * return 0;` meant that once any disk was open for writing, every
     * later write went to THAT disk whatever it was asked for -- so
     * plat_write's per-disk guard, which plat.h presents as the whole
     * safety property of this file, held only for the first stick
     * written in the process. A user whose first stick failed and who
     * plugged in a second got writes to the first one. */
    if (g_stick != INVALID_HANDLE_VALUE) {
        if (g_stick_index == index) return 0;
        give_stick();
    }
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
        unsigned char ext[1024];
        DWORD got = 0;
        int mine = 0;
        if (DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                            ext, sizeof ext, &got, NULL)) {
            VOLUME_DISK_EXTENTS *e = (VOLUME_DISK_EXTENTS *)ext;
            for (DWORD k = 0; k < e->NumberOfDiskExtents; k++)
                if ((int)e->Extents[k].DiskNumber == index) mine = 1;
        }
        if (!mine || g_nvol >= MAX_VOL) { CloseHandle(v); continue; }
        /* BOTH RESULTS ARE READ. They used to be thrown away, so a
         * stick with an Explorer window open on it failed the raw
         * write afterwards and the user was told "the memory stick
         * stopped accepting what was written to it. Try a different
         * one" -- the hardware blamed for a condition this code had
         * already detected, with the correct remedy sitting in another
         * message forty lines below. */
        int locked = DeviceIoControl(v, FSCTL_LOCK_VOLUME, NULL, 0,
                                     NULL, 0, &got, NULL) ? 1 : 0;
        int dismounted = DeviceIoControl(v, FSCTL_DISMOUNT_VOLUME, NULL, 0,
                                         NULL, 0, &got, NULL) ? 1 : 0;
        if (!locked && !dismounted) {
            DWORD e = GetLastError();
            CloseHandle(v);
            for (int i = 0; i < g_nvol; i++) CloseHandle(g_vol[i]);
            g_nvol = 0;
            why_of(why, wn,
                   "Windows will not let go of the memory stick. Close any "
                   "Explorer window showing it, stop any antivirus scan of "
                   "it, and try again", e);
            return -1;
        }
        g_vol[g_nvol++] = v;
    }
    /* FILE_FLAG_NO_BUFFERING, deliberately.
     *
     * Everything here is whole sectors at sector offsets through
     * page-aligned VirtualAlloc buffers, so the flag costs nothing --
     * and without it the read-back that phase 2 does to catch a
     * counterfeit stick is served out of the cache manager's copy of
     * what we just wrote. A stick that acknowledges writes it does not
     * commit is exactly the population that check names, and it would
     * have passed. WRITE_THROUGH so a flush means the device, not the
     * cache. */
    g_stick = open_disk_flags(index, 1,
                              FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
    g_stick_index = index;
    if (g_stick == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        for (int i = 0; i < g_nvol; i++) CloseHandle(g_vol[i]);
        g_nvol = 0;
        g_stick_index = -1;
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
    g_stick_index = -1;
    for (int i = 0; i < g_nvol; i++) CloseHandle(g_vol[i]);
    g_nvol = 0;
}

void plat_release(void) { give_stick(); }

static int seek_to(HANDLE h, uint64_t off)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) ? 0 : -1;
}

int plat_read(int index, uint64_t off, void *buf, size_t n,
              char *why, size_t wn)
{
    /* UNBUFFERED, ALWAYS.
     *
     * The read-back that phase 2 does to catch a stick which
     * acknowledges writes it does not commit has to come from the
     * device, not from the cache manager's copy of what we just wrote
     * -- and by the time it runs, plat_reread() has already closed the
     * unbuffered handle, so this is where it matters. Everything here
     * is whole sectors at sector offsets through a page-aligned
     * VirtualAlloc buffer, which is exactly what the flag requires. */
    HANDLE h = (index == g_allowed && g_stick != INVALID_HANDLE_VALUE)
             ? g_stick
             : open_disk_flags(index, 0, FILE_FLAG_NO_BUFFERING);
    if (h == INVALID_HANDLE_VALUE) {
        why_of(why, wn, "a disk in this computer could not be read",
               GetLastError());
        return -1;
    }
    uint32_t ss = disk_sector(h);
    if (!ss) {
        /* NOT 512. docs/AURBRIDGE.md blocks on a disk that will not
         * state its block size for the system disk; a stick is no
         * different, and a guessed 512 on a 4Kn device makes every
         * offset here eight times wrong. */
        if (h != g_stick) CloseHandle(h);
        snprintf(why, wn,
                 "this drive will not say how large its blocks are, and "
                 "AurOS will not guess.");
        return -1;
    }
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
    if (!ss) {
        snprintf(why, wn,
                 "the memory stick will not say how large its blocks are, "
                 "and AurOS will not guess.");
        return -1;
    }

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

/* MultiByteToWideChar returns 0 and leaves the destination PARTLY
 * WRITTEN when the input does not fit or is not valid UTF-8, and every
 * caller here then NUL-terminated the end of an uninitialised buffer
 * and handed it to CreateFileW -- or, in one case, wrote it into
 * NVRAM. Not reachable with the wizard's own inputs today, and nothing
 * enforced that. */
static int to_wide(const char *in, wchar_t *out, int cap)
{
    int k = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
    if (k <= 0 || k > cap) { out[0] = 0; return -1; }
    return 0;
}

static HANDLE open_file(const char *path, int writing)
{
    wchar_t w[1024];
    if (to_wide(path, w, 1024) != 0) return INVALID_HANDLE_VALUE;
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
    if (to_wide(path, w, 1024) != 0) return;
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
    if (to_wide(from, wf, 1024) != 0 || to_wide(to, wt, 1024) != 0) {
        snprintf(why, wn, "a file path on this computer could not be read");
        return -1;
    }
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

/* AND IT COMES BACK EVEN IF NOBODY ASKS.
 *
 * Every return inside phase_handoff pairs open with close -- but the
 * wizard's cancel path deliberately returns early while a phase is
 * mid-write, so a window closed during the 13 MB copy left the EFI
 * System Partition mounted, writable, in Explorer, across reboots,
 * until somebody ran `mountvol /D` by hand. plat.h says this is given
 * back; atexit is what makes that true when the program does not get
 * to say so itself. */
static void esp_atexit(void) { plat_esp_close(); }

int plat_esp_open(char *root, size_t n, char *why, size_t wn)
{
    static int armed;
    if (!armed) { atexit(esp_atexit); armed = 1; }
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
                          const char *desc, const plat_partition *on,
                          const char *loader, const char *cmdline);

int plat_boot_find(const char *desc, uint16_t *num_out, char *why, size_t wn)
{
    (void)why; (void)wn;
    wchar_t wdesc[128];
    if (to_wide(desc, wdesc, 128) != 0) return -1;
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

int plat_boot_make(const char *desc, const plat_partition *on,
                   const char *loader, const char *cmdline,
                   uint16_t *num_out, char *why, size_t wn)
{
    if (!on || !on->number || !on->blocks) {
        snprintf(why, wn,
                 "AurOS could not work out which part of this disk its "
                 "start-up files are on, and will not write a start-up entry "
                 "that names no partition.");
        return -1;
    }
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
         * is free only when reading it fails BECAUSE THERE IS NOTHING
         * THERE.
         *
         * That last clause is the whole of this paragraph's history.
         * The scan used a 16-byte buffer, and
         * GetFirmwareEnvironmentVariableW returns 0 on ANY failure,
         * including ERROR_INSUFFICIENT_BUFFER. A real load option is
         * 150-250 bytes -- "Windows Boot Manager" alone is more than
         * 16 -- so on the first run on every UEFI machine Boot0000 was
         * declared free and then OVERWRITTEN. On a machine whose
         * BootOrder is {0000}, which is the common OEM shape, aborting
         * afterwards left no Windows Boot Manager and no AurOS: "no
         * bootable device". ab_abort() clears BootNext and nothing
         * else, so there was no way back from it either.
         *
         * The buffer is now the size of a real entry, and the error
         * code is read rather than assumed. */
        num = 0xFFFF;
        static unsigned char probe[4096];
        for (int i = 0; i < 0x2000; i++) {
            wchar_t name[16];
            _snwprintf(name, 15, L"Boot%04X", i); name[15] = 0;
            SetLastError(ERROR_SUCCESS);
            DWORD got = GetFirmwareEnvironmentVariableW(name, EFI_GLOBAL,
                                                        probe, sizeof probe);
            if (got > 0) continue;                   /* occupied       */
            DWORD e = GetLastError();
            if (e != ERROR_ENVVAR_NOT_FOUND && e != ERROR_NOT_FOUND &&
                e != ERROR_FILE_NOT_FOUND)
                continue;      /* something is there and we could not
                                * read it; it is not ours to take      */
            num = (uint16_t)i;
            break;
        }
        if (num == 0xFFFF) {
            snprintf(why, wn,
                     "this computer has no room left in its start-up menu.");
            return -1;
        }
    }
    static unsigned char opt[2048];
    size_t k = load_option(opt, sizeof opt, desc, on, loader, cmdline);
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
 * SHORT-FORM HARD DRIVE, THEN FILE, THEN END, which is what
 * efibootmgr writes and what the specification requires.
 *
 * The first version of this emitted File() and End and nothing else,
 * reasoning that naming the partition would name one the staging
 * environment is about to change the table of. Both halves of that
 * were wrong. UEFI 2.10 s10.3.5 defines exactly two short forms the
 * boot manager must expand, and a bare MEDIA_FILEPATH_DP is neither:
 * LoadImage resolves the path with LocateDevicePath, which with no
 * device node has no handle to anchor to and returns EFI_NOT_FOUND. So
 * every machine would have consumed its one-shot BootNext, loaded
 * nothing, and come back to Windows -- which is at least the safe
 * direction, and is also a product that never installs. And the
 * install never touches the EFI partition's entry: plan.c carves
 * everything out of the gap the shrink makes, so the HD() node stays
 * true.
 */
static size_t load_option(unsigned char *out, size_t n,
                          const char *desc, const plat_partition *on,
                          const char *loader, const char *cmdline)
{
    wchar_t wdesc[128], wload[512], wcmd[512];
    if (to_wide(desc, wdesc, 128) != 0) return 0;
    if (to_wide(loader, wload, 512) != 0) return 0;
    wcmd[0] = 0;
    if (cmdline && *cmdline && to_wide(cmdline, wcmd, 512) != 0) return 0;

    size_t dl = (wcslen(wdesc) + 1) * 2;
    size_t fl = (wcslen(wload) + 1) * 2;
    size_t hd_node   = 42;              /* UEFI 2.10 Table 10-13        */
    size_t file_node = 4 + fl;          /* type, subtype, length, path  */
    size_t end_node  = 4;
    size_t dp = hd_node + file_node + end_node;
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

    /* MEDIA_DEVICE_PATH(0x04) / MEDIA_HARDDRIVE_DP(0x01), length 42 */
    memset(p, 0, hd_node);
    p[0] = 0x04; p[1] = 0x01;
    p[2] = (unsigned char)(hd_node & 0xFF);
    p[3] = (unsigned char)(hd_node >> 8);
    for (int i = 0; i < 4; i++) p[4 + i]  = (unsigned char)(on->number >> (8 * i));
    for (int i = 0; i < 8; i++) p[8 + i]  = (unsigned char)(on->first_lba >> (8 * i));
    for (int i = 0; i < 8; i++) p[16 + i] = (unsigned char)(on->blocks >> (8 * i));
    memcpy(p + 24, on->guid, 16);       /* as it lies on the disk       */
    p[40] = 0x02;                       /* PartitionFormat: GPT         */
    p[41] = 0x02;                       /* SignatureType: GUID          */
    p += hd_node;

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
    if (to_wide(cmdline, w, 1024) != 0) {
        if (tail && n) snprintf(tail, n, "the command could not be read");
        return -1;
    }

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


int plat_file_append(const char *to, const void *buf, size_t n,
                     char *why, size_t wn)
{
    HANDLE f = CreateFileA(to, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, wn, "the installer could not finish writing to this "
                          "computer's start-up partition.");
        return -1;
    }
    const char *p = buf;
    DWORD done = 0, wrote = 0;
    while (done < n) {
        if (!WriteFile(f, p + done, (DWORD)(n - done), &wrote, NULL) || !wrote) {
            CloseHandle(f);
            snprintf(why, wn, "this computer's start-up partition is full.");
            return -1;
        }
        done += wrote;
    }
    CloseHandle(f);
    return 0;
}

uint64_t plat_free_space(const char *path)
{
    char dir[MAX_PATH * 4];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '\\');
    char *fwd   = strrchr(dir, '/');
    if (fwd > slash) slash = fwd;
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExA(dir, &avail, NULL, NULL)) return 0;
    return (uint64_t)avail.QuadPart;
}

/* ── what the installer carries inside itself ────────────────────── */
/*
 * A PE RESOURCE, and not bytes appended to the file, for one reason:
 * Authenticode. A signature is a certificate table at the end of the
 * executable, so anything appended after signing is outside the
 * signature and anything appended before it moves when the signature
 * lands. A resource is inside the image the signature covers, which
 * means the installer can carry a kernel and be signed, in either
 * order, and neither breaks the other.
 *
 * It is unpacked to the temporary directory because the things that
 * read it -- the ESP copy in phase 3 -- take a path. It is removed
 * again on the way out, and a leftover from a crash is 28 MB in a
 * directory Windows cleans.
 */
#define RT_AUROS_PAYLOAD  10        /* RT_RCDATA */

static char g_payload_tmp[2][MAX_PATH];
static int  g_payload_n;

static int payload_id(const char *name)
{
    if (!strcmp(name, PAYLOAD_KERNEL)) return 1;
    if (!strcmp(name, PAYLOAD_INITRD)) return 2;
    return 0;
}

int plat_payload_embedded(void)
{
    /* BOTH, or it is not carrying the payload. An executable with one
     * of the two is an executable that fails halfway through a restart
     * it has already arranged. */
    for (int i = 1; i <= 2; i++)
        if (!FindResourceA(NULL, MAKEINTRESOURCEA(i),
                           MAKEINTRESOURCEA(RT_AUROS_PAYLOAD)))
            return 0;
    return 1;
}

int plat_payload(const char *name, char *path, size_t pn, char *why, size_t wn)
{
    int id = payload_id(name);
    if (!id) { snprintf(why, wn, "the installer asked itself for something "
                                 "it does not carry."); return -1; }
    HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(id),
                            MAKEINTRESOURCEA(RT_AUROS_PAYLOAD));
    if (!r) {
        snprintf(why, wn,
                 "this copy of the installer is incomplete -- the part that "
                 "starts your computer is missing from it. Download it "
                 "again.");
        return -1;
    }
    DWORD len = SizeofResource(NULL, r);
    HGLOBAL h = LoadResource(NULL, r);
    const void *p = h ? LockResource(h) : NULL;
    if (!p || !len) {
        snprintf(why, wn, "the installer could not read its own contents.");
        return -1;
    }
    /* MAX_PATH + 1, which is the documented maximum this returns. Given
     * exactly MAX_PATH it writes NOTHING and returns the size it
     * needed -- a non-zero value that passed the old test -- leaving
     * `dir` uninitialised stack, which was then formatted into a path
     * and used. */
    char dir[MAX_PATH + 1];
    DWORD dn = GetTempPathA(sizeof dir, dir);
    if (dn == 0 || dn > MAX_PATH) {
        snprintf(why, wn, "this computer would not say where temporary files "
                          "go.");
        return -1;
    }
    char out[MAX_PATH];
    if (_snprintf(out, sizeof out - 1, "%saurbridge-%s-%lu", dir, name,
                  (unsigned long)GetCurrentProcessId()) < 0) {
        snprintf(why, wn, "the path for a temporary file was too long.");
        return -1;
    }
    out[sizeof out - 1] = 0;
    /* WRITTEN WHOLE OR NOT AT ALL. A half-written kernel in a place
     * with a plausible name is the one thing worse than none: the copy
     * to the EFI partition would succeed and the restart would find
     * nothing to start. */
    char tmp[MAX_PATH];
    /* CHECKED, like its sibling above. Truncated, `tmp` can come out
     * EQUAL to `out` -- and then the DeleteFileA(out) below removes
     * the file that was just written and the move fails, with the
     * payload gone. */
    if (_snprintf(tmp, sizeof tmp - 1, "%s.part", out) < 0) {
        snprintf(why, wn, "the path for a temporary file was too long.");
        return -1;
    }
    tmp[sizeof tmp - 1] = 0;
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, wn, "the installer could not write a temporary file.");
        return -1;
    }
    DWORD done = 0, wrote = 0;
    int okw = 1;
    while (done < len) {
        if (!WriteFile(f, (const char *)p + done, len - done, &wrote, NULL) ||
            wrote == 0) { okw = 0; break; }
        done += wrote;
    }
    CloseHandle(f);
    if (!okw) {
        DeleteFileA(tmp);
        snprintf(why, wn, "this computer ran out of room for a temporary "
                          "file.");
        return -1;
    }
    DeleteFileA(out);
    if (!MoveFileA(tmp, out)) {
        DeleteFileA(tmp);
        snprintf(why, wn, "the installer could not put its own contents "
                          "where it needs them.");
        return -1;
    }
    if (g_payload_n < 2) {
        _snprintf(g_payload_tmp[g_payload_n], MAX_PATH - 1, "%s", out);
        g_payload_tmp[g_payload_n][MAX_PATH - 1] = 0;
        g_payload_n++;
    }
    _snprintf(path, pn - 1, "%s", out);
    path[pn - 1] = 0;
    return 0;
}

void plat_payload_free(void)
{
    for (int i = 0; i < g_payload_n; i++) DeleteFileA(g_payload_tmp[i]);
    g_payload_n = 0;
}

/* ── fetching the image ──────────────────────────────────────────── */
/*
 * WinHTTP, and a Range request every time -- not only on a resume.
 *
 * THE STATUS CODE IS NOT ENOUGH, and the first version of this said it
 * was. A server or proxy that answers 206 with `bytes 0-N/N` whatever
 * it was asked for is not hypothetical -- it is a mis-tuned CDN edge
 * and several caching proxies -- and reading only the status meant its
 * byte zero was written at our offset. A review reproduced a 5120-byte
 * file where the image is 4096, with the download reported as
 * successful. So the Content-Range's FIRST number decides where the
 * body goes, and a server that starts past where we are leaves a hole
 * and is refused.
 */
#include <winhttp.h>

static int fetch_progress_cancel(int (*cb)(uint64_t, uint64_t, void *),
                                 void *ud, uint64_t got, uint64_t total)
{ return cb ? cb(got, total, ud) : 0; }

/* `bytes <first>-<last>/<total>`, as WinHTTP hands it back. */
static int win_parse_range(const wchar_t *v, uint64_t *first, uint64_t *total)
{
    while (*v == L' ') v++;
    if (!_wcsnicmp(v, L"bytes", 5)) v += 5;
    while (*v == L' ') v++;
    wchar_t *e = NULL;
    unsigned __int64 f = _wcstoui64(v, &e, 10);
    if (!e || e == v || *e != L'-') return -1;
    const wchar_t *sl = wcschr(e, L'/');
    if (!sl) return -1;
    *first = (uint64_t)f;
    *total = (uint64_t)_wcstoui64(sl + 1, NULL, 10);
    return 0;
}

/* One request. `*have` is where to continue from and is updated to
 * where the server actually started. 0 done, 1 "ask again from
 * nothing", -1 refused. */
static int win_fetch_once(HINTERNET s, const URL_COMPONENTS *uc,
                          const wchar_t *path, const char *dest,
                          uint64_t *have,
                          int (*progress)(uint64_t, uint64_t, void *),
                          void *ud, char *why, size_t wn)
{
    HINTERNET c = WinHttpConnect(s, uc->lpszHostName, uc->nPort, 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", path, NULL,
                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                          uc->nScheme == INTERNET_SCHEME_HTTPS
                              ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!q) {
        if (c) WinHttpCloseHandle(c);
        snprintf(why, wn, "AurOS could not reach the place it downloads from.");
        return -1;
    }
    wchar_t range[64];
    _snwprintf(range, 63, L"Range: bytes=%I64u-", (unsigned __int64)*have);
    range[63] = 0;
    WinHttpAddRequestHeaders(q, range, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    int rc = -1;
    HANDLE f = INVALID_HANDLE_VALUE;
    if (!WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(q, NULL)) {
        snprintf(why, wn, "AurOS could not reach the place it downloads from. "
                          "Check this computer is online.");
        goto out;
    }
    DWORD status = 0, slen = sizeof status;
    if (!WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE |
                             WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                             WINHTTP_NO_HEADER_INDEX)) {
        /* Unchecked, this left `status` at 0 and printed "answered 0". */
        snprintf(why, wn, "the place AurOS downloads from did not answer "
                          "properly.");
        goto out;
    }
    if (status == 416) {
        /* Our offset is at or past the end of what the server has,
         * which is what a file that is already complete -- and wrong
         * -- looks like. Starting again is the answer. */
        if (*have == 0) {
            snprintf(why, wn, "the place AurOS downloads from has nothing "
                              "there any more.");
            goto out;
        }
        *have = 0;
        rc = 1;
        goto out;
    }
    if (status != 200 && status != 206) {
        snprintf(why, wn,
                 "the place AurOS downloads from answered %lu. Try again "
                 "later.", (unsigned long)status);
        goto out;
    }

    uint64_t total = 0;
    if (status == 206) {
        wchar_t cr[160]; DWORD cl = sizeof cr;
        uint64_t first = 0;
        if (!WinHttpQueryHeaders(q, WINHTTP_QUERY_CONTENT_RANGE,
                                 WINHTTP_HEADER_NAME_BY_INDEX, cr, &cl,
                                 WINHTTP_NO_HEADER_INDEX) ||
            win_parse_range(cr, &first, &total) != 0) {
            snprintf(why, wn, "the place AurOS downloads from did not answer "
                              "properly.");
            goto out;
        }
        if (first > *have) {
            snprintf(why, wn, "the place AurOS downloads from sent the wrong "
                              "part of the file. Try again later.");
            goto out;
        }
        *have = first;
    } else {
        /* 200 where a continuation was asked for: the server ignored
         * the Range, so its first byte is byte zero of the file. */
        *have = 0;
        /* AS A STRING, NOT AS A NUMBER. WINHTTP_QUERY_FLAG_NUMBER is
         * 32-bit, and the image is five gigabytes: the value either
         * failed to come back at all (a progress bar stuck at 0% for
         * forty minutes) or came back truncated (one that fills at
         * 1 GiB and then sits at 100% for the other four fifths).
         * FLAG_NUMBER64 would be the obvious answer and is 8.1+. */
        wchar_t lenbuf[64]; DWORD ll = sizeof lenbuf;
        if (WinHttpQueryHeaders(q, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &ll,
                                WINHTTP_NO_HEADER_INDEX))
            total = (uint64_t)_wcstoui64(lenbuf, NULL, 10);
    }

    f = CreateFileA(dest, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    *have ? OPEN_ALWAYS : CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, wn, "AurOS could not write the file it is downloading.");
        goto out;
    }
    if (*have) {
        LARGE_INTEGER to; to.QuadPart = (LONGLONG)*have;
        if (!SetFilePointerEx(f, to, NULL, FILE_BEGIN) || !SetEndOfFile(f)) {
            snprintf(why, wn, "AurOS could not write the file it is "
                              "downloading.");
            goto out;
        }
    }

    static char buf[256 * 1024];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(q, buf, sizeof buf, &got)) {
            snprintf(why, wn, "the download stopped partway through. It will "
                              "carry on from here if you try again.");
            goto out;
        }
        if (got == 0) break;
        DWORD wrote = 0, at = 0;
        while (at < got) {
            if (!WriteFile(f, buf + at, got - at, &wrote, NULL) || !wrote) {
                snprintf(why, wn, "this computer ran out of room for the "
                                  "download.");
                goto out;
            }
            at += wrote;
        }
        *have += got;
        if (fetch_progress_cancel(progress, ud, *have, total)) {
            snprintf(why, wn, "the download was stopped.");
            goto out;
        }
    }
    /* AND IT HAS TO HAVE ALL ARRIVED. Stopping on end-of-file and
     * reporting success meant a connection cut partway was a completed
     * download, and the accusation the user eventually got was that
     * the network had tampered with it. */
    if (total && *have != total) {
        snprintf(why, wn, "the download stopped partway through. It will "
                          "carry on from here if you try again.");
        goto out;
    }
    rc = 0;
out:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    WinHttpCloseHandle(q);
    WinHttpCloseHandle(c);
    return rc;
}

int plat_fetch(const char *url, const char *dest,
               int (*progress)(uint64_t got, uint64_t total, void *ud),
               void *ud, char *why, size_t wn)
{
    wchar_t wurl[1024];
    if (to_wide(url, wurl, 1024) != 0) {
        snprintf(why, wn, "that address is not one AurOS can use."); return -1;
    }
    URL_COMPONENTS uc;
    wchar_t host[256], path[1024], extra[1024];
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName  = host;  uc.dwHostNameLength  = 256;
    uc.lpszUrlPath   = path;  uc.dwUrlPathLength   = 1024;
    /* THE QUERY STRING IS PART OF THE ADDRESS. Left out, a pre-signed
     * S3 or CloudFront URL, a ?token=, or a mirror selector is fetched
     * without the thing that authorises it, and the server answers
     * 403 -- which surfaced as "the place AurOS downloads from
     * answered 403. Try again later." for ever. */
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(why, wn, "that address is not one AurOS can use."); return -1;
    }
    if (uc.dwExtraInfoLength) {
        if (wcslen(path) + wcslen(extra) + 1 >= 1024) {
            snprintf(why, wn, "that address is too long.");
            return -1;
        }
        wcscat(path, extra);
    }

    uint64_t have = 0;
    {
        HANDLE e = CreateFileA(dest, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (e != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(e, &sz)) have = (uint64_t)sz.QuadPart;
            CloseHandle(e);
        }
    }

    /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY IS 8.1 AND LATER, and this
     * product targets Windows 7 -- preflight.c, wizard.c and the design
     * document all carry Win7 fallbacks. On an older system WinHttpOpen
     * fails with ERROR_INVALID_PARAMETER and the first thing the
     * download did was tell the user "this computer would not let AurOS
     * use the internet", which reads as a firewall and is neither.
     * Nothing about retrying helps, because it is our API call. */
    HINTERNET s = WinHttpOpen(L"AurBridge", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(L"AurBridge", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { snprintf(why, wn, "this computer would not let AurOS use the "
                                "internet."); return -1; }
    /* Resolve, connect, send, receive. The receive timeout is the one
     * that matters: this is a five gigabyte transfer on a connection
     * that is probably why the machine is being replaced. The connect
     * timeout is LONGER than WinHTTP's default rather than shorter,
     * which an earlier version of this made it while the comment
     * claimed the opposite. */
    WinHttpSetTimeouts(s, 30000, 60000, 60000, 300000);

    int rc = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        int r = win_fetch_once(s, &uc, path, dest, &have, progress, ud,
                               why, wn);
        if (r == 0) { rc = 0; break; }
        if (r < 0) break;
        /* r == 1: start again from nothing, once. */
    }
    WinHttpCloseHandle(s);
    return rc;
}

#endif /* _WIN32 */
