/* fix_win.c — see fix.h. Windows only (the build puts *_win.c in both .exe
 * files and never in the simulation). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "fix.h"
#include "plat.h"
#include "aurbridge-baked.h"   /* AUROS_SITE_W: the website, from build/aurbridge */

/* ── the table ─────────────────────────────────────────────────────── */

static const fix_info FIXES[] = {
    { "insufficient-space", FIX_AUTO,
      L"Not enough free space on drive C:",
      L"We empty the Recycle Bin and clear out Windows’ leftover files.",
      L"We cleared what we safely could, and it still needs more room. Move "
      L"big files (videos, games) off C: and we carry on by ourselves." },
    { "bitlocker-system", FIX_AUTO,
      L"Drive C: is encrypted",
      L"We switch encryption off. It takes a while; leave the PC on.",
      L"Encryption is being switched off. That can take an hour or more; "
      L"leave the PC on and we carry on by ourselves." },
    { "bitlocker-other", FIX_AUTO,
      L"Another drive in this PC is encrypted",
      L"We switch encryption off. It takes a while; leave the PC on.",
      L"Encryption is being switched off. That can take an hour or more; "
      L"leave the PC on and we carry on by ourselves." },
    { "pending-reboot", FIX_RESTART,
      L"Windows is waiting to restart to finish an update",
      L"We restart it. This installer opens again by itself afterwards.", NULL },
    { "volume-dirty", FIX_RESTART,
      L"Windows wants to check drive C: for errors",
      L"We restart so Windows can check it. This installer opens again after.", NULL },
    { "not-on-ac", FIX_WAIT,
      L"The charger isn’t plugged in",
      L"Plug it in. We carry on by ourselves.", NULL },
    /* NOT STOPS ANY MORE (D4): a low battery on the charger, a USB stick
     * or memory card, an empty card reader. They are warnings now, said
     * in NOTES below, or nothing at all. */
    { "recovery-stick-ambiguous", FIX_WAIT,
      L"Two USB drives look the same to this computer",
      L"Take one of them out. We carry on by ourselves.", NULL },
    { "power-state-unknown", FIX_WAIT,
      L"We can’t tell if the charger is plugged in",
      L"Plug it in. We carry on by ourselves.", NULL },
    /* A restart is what usually clears these; once, not in a loop (the
     * wizard stops offering it after the installer has reopened). */
    { "bitlocker-unknown", FIX_RESTART,
      L"We couldn’t check whether drive C: is encrypted",
      L"We restart Windows and check again. This installer opens again by itself.", NULL },
    { "fde-unreadable", FIX_RESTART,
      L"We couldn’t check whether drive C: is encrypted",
      L"We restart Windows and check again. This installer opens again by itself.", NULL },
    { "fde-not-checked", FIX_RESTART,
      L"We couldn’t check whether drive C: is encrypted",
      L"We restart Windows and check again. This installer opens again by itself.", NULL },
    { "fde-check-failed", FIX_RESTART,
      L"We couldn’t check this PC for encryption programs",
      L"We restart Windows and check again. This installer opens again by itself.", NULL },
    { "space-unmeasurable", FIX_RESTART,
      L"We couldn’t measure the free space on drive C:",
      L"We restart Windows and check again. This installer opens again by itself.", NULL },
    { "not-elevated", FIX_CANNOT,
      L"The installer wasn’t given permission to make changes",
      L"Close it, open it again, and choose Yes when Windows asks.", NULL },
    { "arm-pc", FIX_CANNOT,
      L"AurOS doesn’t run on this kind of computer yet",
      L"This computer has a newer kind of chip (ARM) that AurOS isn’t made for.", NULL },
    { "firmware-bios", FIX_CANNOT,
      L"This computer starts up the older way (BIOS)",
      L"AurOS can’t go on it yet. The AurOS website says what can be done.", NULL },
    { "intel-rst", FIX_CANNOT,
      L"AurOS can’t see this computer’s drive",
      L"The drive is set up in a way AurOS can’t use yet. Nothing was changed.", NULL },
    { "mbr-four-primaries", FIX_CANNOT,
      L"This drive has no room for another section",
      L"AurOS can’t install on this drive’s layout yet.", NULL },
    { "smart-bad-sectors", FIX_CANNOT,
      L"This drive is wearing out",
      L"Installing could lose your files. The drive needs replacing first.", NULL },
    { "smart-predict-failure", FIX_CANNOT,
      L"This drive says it is about to fail",
      L"Installing could lose your files. The drive needs replacing first.", NULL },
    { "dynamic-disk", FIX_CANNOT,
      L"Windows is on a multi-drive setup",
      L"AurOS can’t install on this kind of setup.", NULL },
    { "storage-spaces", FIX_CANNOT,
      L"Windows is on a Storage Spaces pool",
      L"AurOS can’t install on this kind of setup.", NULL },
    { "spanned-system-volume", FIX_CANNOT,
      L"Drive C: is spread over more than one disk",
      L"AurOS can’t install on this kind of setup.", NULL },
    { "fde-third-party", FIX_CANNOT,
      L"Another encryption program protects this drive",
      L"Switch it off in that program, then open this installer again.", NULL },
    { "fde-managed", FIX_CANNOT,
      L"This PC’s encryption is managed by an organisation",
      L"Ask whoever manages it; AurOS can’t change it.", NULL },
    { "system-disk-unknown", FIX_CANNOT,
      L"We couldn’t tell which drive Windows is on",
      L"AurOS won’t guess which drive to write to, so it stops here.", NULL },
    { "system-disk-unreadable", FIX_CANNOT,
      L"We couldn’t read the drive Windows is on",
      L"AurOS won’t write to a drive it can’t read.", NULL },
    { "layout-unreadable", FIX_CANNOT,
      L"We couldn’t read how the Windows drive is laid out",
      L"AurOS won’t write to a drive it can’t read.", NULL },
    { "disk-unidentified", FIX_CANNOT,
      L"We couldn’t identify one of this PC’s drives",
      L"AurOS won’t write anywhere it can’t be sure of.", NULL },
    { "no-disks", FIX_CANNOT,
      L"We couldn’t read any drive in this PC",
      L"AurOS won’t write to a drive it can’t read.", NULL },
    { "sector-size-unknown", FIX_CANNOT,
      L"We couldn’t read how the Windows drive stores data",
      L"AurOS won’t write to a drive it can’t read.", NULL },
    { "system-disk-removable", FIX_CANNOT,
      L"Windows is running from a USB drive",
      L"AurOS installs only onto the PC’s own drive.", NULL },
    { "system-not-ntfs", FIX_CANNOT,
      L"Drive C: isn’t formatted the usual way",
      L"AurOS can only make room on an NTFS drive.", NULL },
    { "esp-missing", FIX_CANNOT,
      L"This PC’s start-up area isn’t on the Windows drive",
      L"AurOS can’t install on this layout yet.", NULL },
    { "esp-unreadable", FIX_CANNOT,
      L"We couldn’t read this PC’s start-up area",
      L"AurOS won’t write where it can’t read.", NULL },
    { "esp-low-space", FIX_CANNOT,
      L"This PC’s start-up area is too full",
      L"AurOS can’t fit its start-up files on this PC yet.", NULL },
};

/* ── warnings: one line each ─────────────────────────────────────── */

static const struct { const char *id; const wchar_t *line; } NOTES[] = {
    { "removable-attached",
      L"A USB stick or memory card is plugged in. That\u2019s fine: AurOS won\u2019t touch it." },
    { "removable-no-serial",
      L"A USB stick or memory card is plugged in. That\u2019s fine: AurOS won\u2019t touch it." },
    { "recovery-stick-ambiguous",
      L"A USB stick or memory card is plugged in. That\u2019s fine: AurOS won\u2019t touch it." },
    { "low-ram",
      L"This computer has little memory, so AurOS may be slow with many things open." },
    { "smart-reallocated",
      L"This computer\u2019s drive shows some wear. Keep copies of your photos somewhere else too." },
    { "dynamic-disk-other",
      L"Another drive in this computer has an unusual setup. AurOS won\u2019t touch it." },
    { "storage-spaces-other",
      L"Another drive in this computer has an unusual setup. AurOS won\u2019t touch it." },
    { "fde-container-software",
      L"Encryption software is installed. Close anything you have open in it first." },
    { "intel-rst",
      L"This computer\u2019s drive uses Intel\u2019s storage setting. AurOS checks it can see it." },
};

/* ── when it cannot go on: what she can do instead ────────────────── */

/* The page for "someone who knows computers" (website/advanced.html):
 * switching a BIOS-mode PC to the newer way, and Intel's "RAID On" to
 * AHCI, safely. Written out in full, because she will copy it down.
 * The site's address is baked by build/aurbridge from the repository it
 * builds for (tools/repo.sh), so a move to another repository edits
 * nothing here. */
#define ADVANCED AUROS_SITE_W L"advanced.html"

static const struct { const char *id; const wchar_t *line; } INSTEAD[] = {
    { "arm-pc",
      L"Keep using Windows on this computer. AurOS goes on most other Windows "
      L"computers, so a different one will probably be fine." },
    { "firmware-bios",
      L"Keep using Windows for now. Someone who knows computers may be able to "
      L"switch it to the newer way; how is on " ADVANCED },
    { "intel-rst",
      L"Keep using Windows for now. Someone who knows computers can change one "
      L"setting so AurOS can see the drive; how is on " ADVANCED },
    { "smart-bad-sectors",
      L"Copy your photos and documents somewhere else today, while the drive "
      L"still works. Then have the drive replaced." },
    { "smart-predict-failure",
      L"Copy your photos and documents somewhere else today, while the drive "
      L"still works. Then have the drive replaced." },
    { "not-elevated",
      L"Close this, open AurOS Setup again from your Downloads, and press Yes "
      L"when Windows asks whether it may make changes." },
};

const wchar_t *fix_instead(const char *id)
{
    for (size_t i = 0; i < sizeof INSTEAD / sizeof INSTEAD[0]; i++)
        if (!strcmp(INSTEAD[i].id, id)) return INSTEAD[i].line;
    return NULL;
}

const wchar_t *fix_note(const char *id)
{
    for (size_t i = 0; i < sizeof NOTES / sizeof NOTES[0]; i++)
        if (!strcmp(NOTES[i].id, id)) return NOTES[i].line;
    return NULL;
}

const fix_info *fix_lookup(const char *id)
{
    for (size_t i = 0; i < sizeof FIXES / sizeof FIXES[0]; i++)
        if (!strcmp(FIXES[i].id, id)) return &FIXES[i];
    return NULL;
}

const fix_info *fix_at(int i)
{
    if (i < 0 || (size_t)i >= sizeof FIXES / sizeof FIXES[0]) return NULL;
    return &FIXES[i];
}

/* ── freeing space ─────────────────────────────────────────────────── */

/* Everything inside `dir`, not `dir` itself, best effort: a file in use is
 * skipped, not an error. NEVER THROUGH A LINK. A junction or symbolic link
 * inside a temp folder can point anywhere -- at somebody's documents --
 * and a recursive delete that follows it deletes them. Links are removed
 * as links (RemoveDirectory on a junction removes the junction only). */
static void empty_dir(const wchar_t *dir, int depth)
{
    if (depth > 32) return;
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2 - 1, L"%ls\\*", dir);
    pat[MAX_PATH * 2 - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t p[MAX_PATH * 2];
        _snwprintf(p, MAX_PATH * 2 - 1, L"%ls\\%ls", dir, fd.cFileName);
        p[MAX_PATH * 2 - 1] = 0;
        DWORD a = fd.dwFileAttributes;
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (a & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p);
            else DeleteFileW(p);
            continue;
        }
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            empty_dir(p, depth + 1);
            RemoveDirectoryW(p);
        } else {
            if (a & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(p, a & ~FILE_ATTRIBUTE_READONLY);
            DeleteFileW(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Windows' own Disk Cleanup, told which kinds of files to remove and run
 * without asking. Only kinds that are Windows' leftovers: NOT "Downloads"
 * (somebody's files) and NOT "Previous Windows installation(s)" (the way
 * back from the last big Windows update). */
static void disk_cleanup(void)
{
    static const char *SAFE[] = {
        "Active Setup Temp Folders", "D3D Shader Cache", "Delivery Optimization Files",
        "Device Driver Packages", "Downloaded Program Files", "Internet Cache Files",
        "Old ChkDsk Files", "Recycle Bin", "Setup Log Files",
        "System error memory dump files", "System error minidump files",
        "Temporary Files", "Temporary Setup Files", "Thumbnail Cache",
        "Update Cleanup", "Windows Error Reporting Files",
        "Windows Upgrade Log Files", NULL };
    const char *ROOT =
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VolumeCaches\\";
    DWORD two = 2;
    for (int i = 0; SAFE[i]; i++) {
        char key[256];
        snprintf(key, sizeof key, "%s%s", ROOT, SAFE[i]);
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegSetValueExA(k, "StateFlags0421", 0, REG_DWORD, (const BYTE *)&two, sizeof two);
            RegCloseKey(k);
        }
    }
    char sys[MAX_PATH], cmd[MAX_PATH + 64];
    if (!GetSystemDirectoryA(sys, sizeof sys)) return;
    snprintf(cmd, sizeof cmd, "%s\\cleanmgr.exe", sys);
    if (GetFileAttributesA(cmd) == INVALID_FILE_ATTRIBUTES) return;   /* not on this edition */
    /* Not plat_run, which waits for ever: "Update Cleanup" can take a long
     * time on a PC that has had years of updates. Fifteen minutes, then the
     * check decides whether what it has freed so far is enough; Disk
     * Cleanup carries on by itself either way. */
    snprintf(cmd, sizeof cmd, "\"%s\\cleanmgr.exe\" /sagerun:421", sys);
    wchar_t w[MAX_PATH + 64];
    if (!MultiByteToWideChar(CP_UTF8, 0, cmd, -1, w, MAX_PATH + 64)) return;
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, w, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return;
    WaitForSingleObject(pi.hProcess, 15 * 60 * 1000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

static int free_space(char *why, size_t n)
{
    (void)why; (void)n;
    SHEmptyRecycleBinW(NULL, NULL,
                       SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    wchar_t t[MAX_PATH];
    DWORD k = GetTempPathW(MAX_PATH, t);
    if (k > 3 && k < MAX_PATH) {
        if (t[k - 1] == L'\\') t[k - 1] = 0;
        empty_dir(t, 0);
    }
    wchar_t w[MAX_PATH];
    if (GetWindowsDirectoryW(w, MAX_PATH)) {
        wcsncat(w, L"\\Temp", MAX_PATH - wcslen(w) - 1);
        empty_dir(w, 0);
    }
    disk_cleanup();
    return 0;
}

/* ── encryption off ────────────────────────────────────────────────── */

/* manage-bde -off on every fixed drive. On a drive that is not encrypted it
 * says so and changes nothing; on one that is, Windows starts decrypting
 * in the background and the check keeps saying "encrypted" until it has
 * finished, which is what the page waits for. */
static int encryption_off(char *why, size_t n)
{
    char sys[MAX_PATH], exe[MAX_PATH + 32];
    if (!GetSystemDirectoryA(sys, sizeof sys)) { snprintf(why, n, "Windows would not say where it is"); return -1; }
    snprintf(exe, sizeof exe, "%s\\manage-bde.exe", sys);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        snprintf(why, n, "this edition of Windows cannot switch encryption off from a "
                         "program. Open Settings, Privacy & security, Device encryption, "
                         "and switch it off there.");
        return -1;
    }
    DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d++) {                 /* C: onwards */
        if (!(drives & (1u << d))) continue;
        char root[4] = { (char)('A' + d), ':', '\\', 0 };
        if (GetDriveTypeA(root) != DRIVE_FIXED) continue;
        char cmd[MAX_PATH + 64], tail[256];
        snprintf(cmd, sizeof cmd, "\"%s\" -off %c:", exe, 'A' + d);
        plat_run(cmd, tail, sizeof tail);
    }
    return 0;
}

int fix_run(const char *id, char *why, size_t n)
{
    if (!strcmp(id, "insufficient-space")) return free_space(why, n);
    if (!strcmp(id, "bitlocker-system") || !strcmp(id, "bitlocker-other"))
        return encryption_off(why, n);
    snprintf(why, n, "there is no fix for this that a program can do");
    return -1;
}

/* ── restart, and carry on ─────────────────────────────────────────── */

#define RESUME_TASK "AurOS installer - continue"

/* A scheduled task, not a Run key: Windows does not start a program that
 * needs administrator rights from a Run or RunOnce key, and this one does.
 * At this person's next sign-in, with their highest rights, once: the
 * installer removes the task as the first thing it does (fix_resume_done). */
int fix_resume_arm(char *why, size_t n)
{
    wchar_t self[MAX_PATH];
    char selfa[MAX_PATH * 3], user[256] = "", dom[256] = "", cmd[2048], tail[256];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH) ||
        !WideCharToMultiByte(CP_UTF8, 0, self, -1, selfa, sizeof selfa, NULL, NULL)) {
        snprintf(why, n, "the installer could not find itself on this PC");
        return -1;
    }
    GetEnvironmentVariableA("USERNAME", user, sizeof user);
    GetEnvironmentVariableA("USERDOMAIN", dom, sizeof dom);
    snprintf(cmd, sizeof cmd,
             "schtasks.exe /create /f /tn \"" RESUME_TASK "\" /sc onlogon /it /rl highest "
             "/ru \"%s\\%s\" /tr \"\\\"%s\\\" --resume\"", dom, user, selfa);
    if (plat_run(cmd, tail, sizeof tail) != 0) {
        snprintf(why, n, "Windows would not let the installer open itself again after "
                         "the restart");
        return -1;
    }
    return 0;
}

void fix_resume_done(void)
{
    char tail[128];
    plat_run("schtasks.exe /delete /f /tn \"" RESUME_TASK "\"", tail, sizeof tail);
}

int fix_restart_and_continue(char *why, size_t n)
{
    if (fix_resume_arm(why, n) != 0) return -1;
    if (plat_restart(why, n) != 0) {
        fix_resume_done();
        return -1;
    }
    return 0;
}
