# 3. Nothing ever started the installer on real Windows

**Severity:** process. This is the reason issue 1 reached a tester.
**Status:** not fixed.

## What went wrong

The published installer was described as tested. The `image-desktop`
README lists `installtest.sh` 37/37, `nosticktest.sh` 45/45,
`choicesboottest.sh` 9/9, and "the same piece list downloaded and checked
from here under Wine". All of that is true. None of it involves Windows'
own program loader:

- **Wine** runs the program, but its side-by-side/manifest handling is
  lenient. It loaded a manifest that Windows rejects.
- **The simulated machine** (`out/aurbridge-sim`) runs the same phase
  engine as a Linux program. It never goes near the `.exe`.
- **Checking the download** (SHA-256, "is a PE32+ GUI x86-64 file",
  every image piece re-downloaded and verified) proves you got the right
  file. It doesn't prove the file runs. That check was done on
  2026-09-26 just before the tester's attempt, and it was reported as
  "make sure it works". It could only show that the file downloads
  intact, not that Windows would start it.

The project's own rule ("prove a check fails when the thing it checks is
broken before believing it") had nothing to apply to here: **no check
existed** for "Windows will load this program".

## What needs to be added

In rough order of cost:

1. **A strict manifest check in the build** (see
   [01-sxs-manifest.md](01-sxs-manifest.md), items 2–3). It's cheap and
   runs on Linux. It catches this exact bug, but no other bug that only
   shows up on Windows.
2. **A smoke test on a real Windows machine for every build that gets
   published.** The minimum is: Windows loads the program, it creates its
   window, and it exits cleanly. Options:
   - **GitHub Actions `windows-latest` runner.** Start the wizard from
     PowerShell (`Start-Process -PassThru`) and fail on error 14001 or
     any failure to launch. A small `--selftest` switch on the wizard
     (create the window, then exit 0 without touching any disk) would make
     this clean. The runner is already elevated, so `requireAdministrator`
     doesn't need a prompt.
   - **A Windows VM in QEMU** (Microsoft's evaluation images), run from
     the same machine that already runs the OVMF tests. Heavier, but it
     can go on to run *Check this PC* for real, which reads the disk and
     the firmware.
3. **On Windows, run the console tool's read-only preflight in CI**
   (`aurbridge.exe`, no manifest). This exercises `plat_win.c` against a
   real Windows disk and firmware API for the first time. Today that code
   has only ever run under Wine.
4. **Word "tested" carefully in the release notes.** In the
   `image-desktop` README and `docs/TRY-IT.md`, list which tests ran on
   Windows and which on Wine or a simulation. Right now "tested" reads as
   if it covers Windows.

## Other things Wine may be hiding (unverified, worth a look)

These haven't been seen to fail. They are the kinds of Windows behaviour
Wine is known to model loosely, in code AurBridge depends on:

- whether raw `\\.\PhysicalDriveN` access and the IOCTLs
  (`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`, `IOCTL_STORAGE_QUERY_PROPERTY`)
  return what the code expects on NVMe, on RAID/RST controllers, and on
  4Kn drives
- `GetFirmwareEnvironmentVariable` / `SetFirmwareEnvironmentVariable`,
  which need `SeSystemEnvironmentPrivilege` to be enabled explicitly
- behaviour under Windows 11's Smart App Control, Memory Integrity (HVCI)
  and Controlled Folder Access
- WinHTTP going through a system proxy or a corporate TLS-inspecting proxy
- DPI scaling on real 125%/150% displays (the wizard calls
  `SetProcessDpiAwarenessContext` at run time, which is fine, but it has
  never been looked at on real hardware)
