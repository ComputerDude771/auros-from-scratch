# Stream F: install AurOS from a real Windows, end to end (handoff)

Updated 2026-10-23 04:50 UTC. Branch `claude/brave-hypatia-x72g1m`.
Files: `.github/workflows/e2e.yml`, `tools/e2e/**` (nothing else).
Full user guide: `tools/e2e/README.md`. This page is the state of the work.

## Goal

Prove on a real Windows (in a KVM VM on GitHub Actions) what a person does:
Windows 11 from Microsoft's own ISO, unattended, to the desktop (M1) →
Edge opens our website, presses "Get AurOS", keeps the download, runs the
installer, goes through the wizard, Windows restarts into AurOS (M2) →
AurOS's store installs a program and opens it, and with the machine off the
program's files are on AurOS's disk (M3). Every step has a screenshot.
Then the same on the PCs people really have (variants, below).

## Status

| Milestone | State | Evidence |
|---|---|---|
| M1 Windows to desktop | **green** | runs 39891036021, 39891768538; prerelease `e2e-39891768538`. Windows 11 Enterprise Eval 10.0.26300 (26H2), Secure Boot 1, TPM 2.0 (swtpm), Fast Startup on, Smart App Control 2 (evaluation), BitLocker off. Setup from the ISO takes ~13 min on Actions (ISO download ~3 min). |
| M2 website → download → wizard → restart | **half**: download proven, wizard not yet reached | run 39894079407 (prerelease `e2e-39894079407`): Edge opened the runner-served website, the driver pressed the real "Get AurOS", Edge downloaded all 34.9 MB, then stalled at Edge's "isn't commonly downloaded" flyout (see Traps). Fix pushed in `208b3ed`; **re-run 39896941765 in progress** (doing Windows Setup again, see Cache). |
| M3 store install, program on disk | not reached | code is in `e2e.py` (store from the app list, search retried, exact "Install", then "Open" or the "Press here to open it" card; `inspect` checks the files with the machine off) |
| Variants | code pushed (`f70760f`); made safe in the 04:40 push (two at a time, base by default, only base cached); expectations per variant still to write (Next steps 4) | run 39897340342 cancelled by me: six jobs at once breaks the max-parallel ≤ 2 rule. Its smalldisk job showed first-logon variant setup works (leftover file made at 04:09). |

Unpushed work: none. The working tree was clean at 04:25.

## Open problem: no live view or evidence since 03:49

Every e2e run's `gh release create e2e-<run>` since 03:49 gets
`HTTP 403: Resource not accessible by integration`
(`.../releases`), at init and at final. othersys.yml does the same thing
with the same `permissions: contents: write` and worked at 03:58. I didn't find
the cause: there are no rulesets, all the commits are on the branch, and the
workflow headers are identical. `publish.sh` hides the failure (the step stays
green). The evidence zip still goes up as an Actions **artifact**
(`e2e-evidence` / `e2e-evidence-<variant>`), but artifacts can't be downloaded
from the agent container (blob host blocked). Job logs can be read with the
GitHub MCP `get_job_logs` once a job finishes.

## Point the harness at a new installer

In `.github/workflows/e2e.yml`, `env:`: set `INSTALLER_URL` to the
`AurOS-Setup.exe` URL (e.g. a prerelease asset) and `INSTALLER_SHA256` (empty
= accept and print the hash). Pushing that edit starts a run. Today it points
at `raw/image-desktop/AurOS-Installer-test.exe`. Stream E's log says to
re-point it at prerelease `v0.1.0-dd39389`'s `AurOS-Setup.exe` once that
is published. Details: README "Pointing it at a new build".

## Next steps

1. **Evidence visible again**: DONE in the 04:40 push, but not yet proven
   on Actions. `publish.sh init` tries e2e-<run> at the commit, then at the
   default branch, then the newest existing e2e-* prerelease with files named
   `r<RUN>-<file>`. It warns (`::warning::`) and puts the links in the job
   summary. Tested locally with a fake `gh` (all four paths). Check the first
   run after the push: its job summary names where the files went.
2. **Get M2 green, then M3**: read run 39896941765's result (job log via MCP;
   evidence wherever step 1 puts it). If the wizard is reached, follow the
   screenshots. The driver must press only Edge's own Keep / Show more /
   Keep anyway / Open file, never the page's.
3. **Variants, made safe for the shared account**: DONE in the 04:40 push.
   `max-parallel: 2`; a harness change runs `base` only; `[e2e:all]` runs
   all; `[e2e:a,b]` runs those; `[e2e:none]` skips. Only the base golden goes
   into the Actions cache; the others build Windows fresh (~15 min) and never
   save. Variants: `base`; `hdd` (base golden, QEMU throttle 100 MB/s,
   120 IOPS); `bitlocker` (first logon `Enable-BitLocker -TpmProtector
   -UsedSpaceOnly`, waits for FullyEncrypted); `smalldisk` (72 GB disk, 12 GB
   leftover in `C:\Windows\Temp`); `pending` (at the test's logon, a SYSTEM
   task runs `dism /enable-feature ... /norestart` → real `RebootPending`);
   `win10` (`WINDOWS_VERSION=10` in `get-windows.sh`).
4. **What each variant must see**: code DONE at 04:50 (`VARIANT_EXPECT` and
   `Flow.check_variant` in `e2e.py`; smalldisk's leftover is now sized from
   the free space). Not yet run: run `[e2e:smalldisk,pending]`, then
   `[e2e:bitlocker,win10]`, once base is green. Background
   (from `src/aurbridge/preflight.c` and
   `fix_win.c`): `pending-reboot` is PF_BLOCK with FIX_RESTART (the driver must
   take the restart, log on again, and run the installer again from Downloads).
   `insufficient-space` is FIX_AUTO (`free_space`), and the flow must then go
   on. `bitlocker-system` is FIX_AUTO (pause BitLocker, owner decision D11).
   Check that the 12 GB leftover file is what `free_space` actually cleans, and
   size it so the space check fails before the fix and passes after. Write each
   expectation as a check that fails when the fix is missing.
5. **Timings for the owner**: the hdd variant's wall clock for each phase
   (`timings.md` in each evidence zip); a 2013-laptop budget is the bar.

## Traps (each cost a run)

- **Edge's download flyout is a web page** (Edge WebUI, so a "document" to UI
  Automation). A filter that ignores web documents, meant to stop the
  website's own "Keep anyway" text from being clicked, also hid Edge's
  flyout. Fixed: only a document that fills the window counts as the website;
  the flyout is reopened from the toolbar if it closes; the step fails after
  8 minutes with a screenshot.
- **Display timeout**: Windows blanks the screen and screenshots go black.
  The driver now moves the pointer 1 px every 2 minutes without input.
- **Cache eviction**: key `e2e-windows-v1-48c56e1d96769621` hit at 03:19
  and was gone by 03:49 (other workflows' caches pushed it out). Don't
  count on the cache. It saves ~10 min at best.
- **Release 403** (above): a green publish step doesn't mean anything was
  published. Check `releases/download/e2e-<run>/...` with curl.
- **Smart App Control is in evaluation (2)** on a fresh Windows 11. It can
  switch itself on and block an unsigned installer (no code signing, owner
  D1). Watch for its block dialog in M2 screenshots.
- `[e2e:...]` is read from the commit **subject** only; the README alone
  doesn't start a run; the `concurrency` group is on the e2e job, so a
  push doesn't cancel a queued run.

## How to verify

- The Actions run: `https://github.com/ComputerDude771/auros-from-scratch/actions/workflows/e2e.yml`.
  The step names are the milestones: "M1 - ...", "M2/M3 - ...", "M3 - the
  program is on AurOS's disk".
- Evidence (when publishing works): `releases/download/e2e-<RUN>/live[-<variant>].png`
  and `.txt` while it runs, `e2e-evidence[-<variant>].zip` after (screenshots
  `shots/`, `driver.log`, `timings.md`, serial logs, `windows-facts.json`).
- Locally without KVM: `python3 tools/e2e/selftest.py` (36 checks: variant
  words against the wizard's titles, publish.sh's four ways out with a fake
  `gh`; also the job's first step), `python3 -m pyflakes tools/e2e/*.py`,
  `sh -n tools/e2e/*.sh`, `sh tools/e2e/mkanswer.sh /tmp/a.iso` (then check
  `autounattend.xml` in it with `xmllint`), and `python3 tools/e2e/vision.py shot.png`
  on any screenshot to see what the driver reads.
