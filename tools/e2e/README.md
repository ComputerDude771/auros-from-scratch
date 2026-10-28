# tools/e2e: AurOS installed from a real Windows, end to end

Every other test here either runs the Windows half on a Windows that
cannot restart into the installer (`.github/workflows/windows.yml`) or
runs the restart half against a simulated Windows (`tools/nosticktest.sh`).
This one does the whole thing on a real Windows 11, inside a VM on a
GitHub Actions Linux runner, and keeps a screenshot of every screen.

## What it does, and what each step proves

| Step | What happens | What it proves |
|---|---|---|
| M1 `e2e.py windows` | Windows 11 Enterprise evaluation, downloaded from Microsoft (`get-windows.sh`), installed unattended (`guest/autounattend.xml`) under QEMU/KVM: q35, OVMF with Secure Boot on and Microsoft's keys, TPM 2.0 (swtpm), a 128 GB SATA SSD (AHCI, TRIM), e1000e, 1280x800, USB tablet. EFI 100 MB, MSR, C:, Recovery after C:. Fast Startup, SmartScreen, Defender and UAC left as Windows ships them; Windows Update and Edge's first-run tour off by policy (`guest/specialize.cmd`). | A real, current Windows with a PC's boot set-up. `windows-facts.json` records what Windows itself says: Secure Boot, TPM, BitLocker state, Fast Startup, partitions, firmware boot entries. |
| (cache) | The installed machine (disk, firmware variables, TPM state) is kept in the Actions cache. | Later runs start from the same fresh Windows in minutes. |
| M2 `e2e.py flow` | Edge opens the real website (served from the runner, `http://10.0.2.2:8000`), its download button ("Get AurOS") is pressed (any boxes gating it ticked first); Edge's download warnings answered (Keep, Keep anyway); the file's SHA-256 checked; SmartScreen's "More info", "Run anyway"; UAC's Yes (Alt+Y on the secure desktop); then the wizard: the teal primary button pressed whenever it is enabled, check boxes ticked, the typed word typed, until "Restart now". Then the restart is watched: firmware, the staging screen, AurOS's first start, and "Yes, it all works" is pressed. | The published installer, as a person gets it, takes this PC to AurOS: downloaded, trusted past Windows' warnings, elevated, preflight passes on a real machine, the image downloads and checks, BootNext is honoured by real firmware under Secure Boot (shim, grub, staging kernel), the real NTFS is shrunk, AurOS written, verified and started, and its first question answered. |
| M3 `e2e.py flow` (end) and `e2e.py inspect` | In AurOS: the store ("Get more programs") opened, a program searched for, installed and opened. Then the machine is shut down and its disk read with `qemu-nbd`, read-only. | A person can add a program; the program is on AurOS's disk, not just on the screen. |

Every press is a QMP keyboard or tablet event (what a person's hands
do); none is a hidden test mode of the product. Before each press a
screenshot is kept with a magenta cross where the press lands
(`shots/NNNN-click-*.png`). The installer draws its own window, so it is
driven by sight: its primary button is the only large solid block of the
accent colour (#7DD3C0), and a page with no enabled button is either
waiting (checking, downloading), has empty check boxes (found by shape),
or names a word to type ("Type AGREE ...", read by OCR); a page with
nothing to click that sits still for a minute gets Enter (the wizard's
Enter is its primary action). A restart the installer makes by itself
(a countdown after "Restart now") is noticed by QEMU's reset event. Microsoft's own
dialogs and Edge are located by a small helper inside Windows
(`guest/helper.ps1`) that reads UI Automation and reports names and
rectangles; it never presses anything. UAC is on the secure desktop,
which nothing inside the session can see: it is found by OCR and
answered with the keyboard.

## Testing a new installer (the short version)

1. In `.github/workflows/e2e.yml`, `env:`, set
   `INSTALLER_URL: https://github.com/${{ github.repository }}/releases/download/<TAG>/AurOS-Setup.exe`
   and `INSTALLER_SHA256: <its sha256>` (empty accepts any file and prints
   its hash).
2. Commit and push to a `claude/**` branch. Changing `e2e.yml` is what
   starts the run (so does `[e2e]` in a commit message). With the
   installed Windows cached, M2 starts about 8 minutes after the push.
3. Watch `https://github.com/OWNER/REPO/releases/download/e2e-<RUN_ID>/live-<VARIANT>.png`
   and `.../live-<VARIANT>.txt` (refreshed every 3 minutes); when a
   variant's job ends, `.../e2e-<RUN_ID>/e2e-evidence-<VARIANT>.zip` has
   every screenshot and log, and the release's notes get its timings.
   `<RUN_ID>` is the number in the Actions run's URL; `<VARIANT>` is
   `base` for the plain PC (see Variants). If no release could be made
   for the run, the job says so with a warning, and the files go into
   the newest `e2e-*` prerelease as `r<RUN_ID>-<file>`. The job's summary
   page has the links either way.

## Variants: the PCs people have

Each is its own job, two at a time (the account's runners are shared).
A harness change runs `base` only; in the head commit's subject
`[e2e:all]` runs every variant (`E2E_VARIANTS` in the workflow),
`[e2e:base,hdd]` those, `[e2e:none]` nothing. Only the base Windows is
kept in the Actions cache (a golden is several GB, and the repository's
caches share 10 GB); the others install Windows afresh, about 15
minutes, and `hdd` uses the base one.

| Variant | The PC | How it is made |
|---|---|---|
| `base` | Windows 11, 128 GB SSD, Fast Startup on (Windows' default) | as described above |
| `hdd` | the same, on a hard disk | QEMU block throttling: 100 MB/s, 120 IO/s (`E2E_DISK_THROTTLE`); the timings are the wall clock on such a disk |
| `bitlocker` | BitLocker / device encryption on | first logon: `Enable-BitLocker C: -TpmProtector -UsedSpaceOnly`, waited until fully encrypted; the installer must deal with it |
| `smalldisk` | a 72 GB drive whose leftovers in `C:\Windows\Temp` leave 30 GB free | first logon makes one leftover file of (free − 30 GB): 22 GB to give against the 28 needed, enough once the installer's space fix empties that folder |
| `pending` | Windows waiting for a restart after servicing | at the test's logon a scheduled task enables a Windows feature with `/norestart` (real `RebootPending`, cleared by a real restart) |
| `win10` | Windows 10 Enterprise 22H2 evaluation | `WINDOWS_VERSION=10` in `get-windows.sh` |

Each variant must also get the stop it was made for: `VARIANT_EXPECT`
in `e2e.py` names the words a wizard page must show, that "Fix these
for me" was pressed, and (pending) that Windows restarted for the fix.
A variant whose PC came out wrong fails instead of quietly testing the
plain PC again.

The variant's Windows is set up by `guest/firstlogon.ps1` from
`e2e\variant.txt` on the answer CD (`mkanswer.sh`, `E2E_GOLDEN`,
`E2E_DISK_GB`).

## Running it

It runs on GitHub Actions (`.github/workflows/e2e.yml`) when a push to a
`claude/**` branch changes `tools/e2e/**` or the workflow, or when the
head commit message contains `[e2e]`. It needs nothing else: the
repository is public, KVM is on the standard runners, and the evidence
goes to a prerelease using the workflow's own token.

The evidence of run N: `https://github.com/OWNER/REPO/releases/download/e2e-N/e2e-evidence-VARIANT.zip`
(also an artifact named `e2e-evidence-VARIANT`). While it runs,
`live-VARIANT.png` (the newest screenshot) and `live-VARIANT.txt`
(timings and the driver's last lines) in the same release are refreshed
every three minutes. Inside the zip: `windows/` (M1) and
`flow/` (M2, M3), each with `shots/` (every screen, numbered in order),
`timings.md`, `driver.log`, `serial.log` (firmware, and AurOS's console),
`guest-events.log`, `ui-elements.log` (what UI Automation saw in Edge and
SmartScreen), `guest-diagnostics.log`, and `FAILED.txt` when it failed;
`inspect/disk-inspection.txt` for M3.

### Pointing it at a new build

All in `env:` at the top of `.github/workflows/e2e.yml`:

- `INSTALLER_URL` / `INSTALLER_SHA256`: the installer to test, for
  example a prerelease asset
  (`https://github.com/OWNER/REPO/releases/download/TAG/AurOS-Setup.exe`).
  The website's links to any `AurOS*.exe` are rewritten to it, the file
  keeps the name the URL ends in, and the downloaded file must hash to
  `INSTALLER_SHA256` (empty: accept and print). The installer carries its
  own image URL, so nothing else changes. Editing these two lines in the
  workflow is itself what starts a run.
- `INSTALLER_PATH`: a file on the runner instead, for example an artifact
  a `build` job uploaded (add `needs: build` and an
  `actions/download-artifact` step before the flow step). It is served
  from the runner and the website links to it. The SHA-256 check then
  wants `INSTALLER_SHA256` set to that file's hash, or empty to accept it.
- `WEBSITE_DIR` (default `website`): the site Edge opens; `none` gives a
  one-link page. `WEBSITE_PAGE` (env of the flow step) picks the page;
  `DOWNLOAD_LINK` is a regular expression for the link's accessible name
  (default: "Get AurOS", or an older "Download ... installer"). Only the
  page's own elements are candidates for it, and only Edge's own (never
  the page's, which shows pictures and words of the same warnings) for
  Keep / Show more / Keep anyway / Open file.
- The store: `STORE_NAME`, `STORE_QUERY`, `STORE_RESULT`, `STORE_INSTALL`,
  `STORE_OPEN`, `APP_FILES` at the top of `e2e.py` (or as env).
- `GOLDEN_VERSION`: bump to throw the cached Windows away. The cache key
  also changes whenever a file in `guest/` other than `helper.ps1`
  changes; `helper.ps1` is fetched from the runner at every logon and can
  change freely.
- `E2E_STORE=0` skips M3.

`python3 tools/e2e/selftest.py` (no VM, seconds; the job's first step)
checks that each variant's expected words still match the wizard's
titles in `src/aurbridge`, and every way `publish.sh` can publish.

Locally (Linux with KVM, `runner-setup.sh` lists the packages):

```sh
sh tools/e2e/get-windows.sh /big/win.iso
sh tools/e2e/mkanswer.sh /big/answer.iso
python3 tools/e2e/e2e.py windows /big/out/windows /big/golden --iso /big/win.iso --answer /big/answer.iso
python3 tools/e2e/e2e.py flow /big/out/flow /big/golden /big/work
python3 tools/e2e/e2e.py inspect /big/out/inspect /big/work
python3 tools/e2e/vision.py some-shot.png   # what the driver sees in a screenshot
```

## Known limits: it is a VM, not a PC

- The disk is QEMU's AHCI with a plain SATA SSD: no Intel RST/VMD, no
  NVMe, no vendor firmware. The firmware is OVMF, which honours BootNext
  and Secure Boot correctly; real vendor firmware sometimes does not.
- One disk, 128 GB, freshly installed Windows: a few GB used, no
  fragmentation, no OneDrive, no third-party antivirus.
- Windows is an Enterprise evaluation, signed in with a local account.
  BitLocker/device encryption is whatever Windows decided on this
  hardware (`windows-facts.json` says which).
- The screen is the firmware's framebuffer (Microsoft Basic Display
  Adapter) at 1280x800, 100% scaling.
- The network is QEMU's user-mode NAT: the image comes from GitHub as it
  would at home, but faster and without Wi-Fi.
- Windows Update is off by policy so that nothing restarts the machine
  under the test; a real PC may have an update waiting (the installer's
  preflight checks for that).
