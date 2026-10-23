# Stream P — every profile builds and boots

Owner of: `.github/workflows/profiles.yml`, `profiles/{office,revive,school-kiosk,multilingual}.profile`,
`tools/profiletest*`, this file. Branch `claude/brave-hypatia-x72g1m` only (rules: `docs/handoff/STREAMS.md`).

## Goal

The four non-desktop profiles (office, revive, school-kiosk, multilingual) must each build on
GitHub Actions (with Firefox, no browser fallback) and start in a KVM VM to their own desktop,
with a screenshot, an image size and a boot result for each.

## Status (update this table after every step)

| profile | lint | built on Actions | boots to desktop | image size | screenshot |
|---|---|---|---|---|---|
| office | pass (was FAIL: thunderbird snap stub) | not yet | not yet | - | - |
| revive | pass (was FAIL: 3 unknown excludes) | not yet | not yet | - | - |
| school-kiosk | pass | not yet | not yet | - | - |
| multilingual | pass | not yet | not yet | - | - |

## Done and pushed

- `47f6d90` office: `thunderbird` removed (noble's is a snap stub; forge pins stubs to -1, so the
  build died). revive: exclude list uses noble's real names (one unknown name made apt purge
  nothing). school-kiosk: neutral brand name, blank enrollment URL.
- `tools/profiletest.sh`:
  - `sh tools/profiletest.sh lint [profile...]` — seconds, no root; downloads noble's Packages
    indexes once (cache `$PROFILETEST_PKGS`, default `/tmp/profiletest-pkgs`).
  - `sh tools/profiletest.sh selftest` — proves every check fails on broken input (20/20 pass).
  - `sudo sh tools/profiletest.sh boot <profile> [evidence-dir]` — needs `out/auros-<profile>.img`;
    checks the image's packages/policy/browser, injects a probe unit (NOT part of AurOS) into a
    scratch copy, boots OVMF+QEMU (KVM if `/dev/kvm` writable) at 1280x800, reads the probe's
    `PROFILETEST key=value` lines from serial, screendumps via QMP, writes
    `<profile>.png`, `<profile>-report.txt`, `<profile>-serial.txt`, `<profile>-image.txt`.
    Locally verified only the QMP screendump + PNG writer (OVMF with no disk); the full boot
    has not yet run anywhere.

## Half-done / not pushed

- `.github/workflows/profiles.yml` pushed (first run in flight); step 1 below describes it. If it is not on the
  branch, write it as below.

## Next 5 steps

1. Write `.github/workflows/profiles.yml`: push on `claude/**`; a `gate` job (like build.yml's)
   runs when the subject has `[profiles]` (optionally `[profiles office revive]` for a subset) or
   the push changed `profiles/`, `tools/profiletest*` or `profiles.yml`. Then a matrix job over the
   four profiles with `max-parallel: 1`, `fail-fast: false`, and job-level
   `concurrency: profiles-<ref>-<profile>` (`cancel-in-progress: false`; NOT workflow-level:
   docs-only pushes would replace a pending real run). Each leg: free disk, apt tools
   (debootstrap squashfs-tools zstd gdisk dosfstools mtools e2fsprogs sbsigntool python3 cpio
   file binutils qemu-system-x86 qemu-utils ovmf pigz), KVM udev rule (copy tests.yml), restore
   the debootstrap cache with build.yml's exact key formula (key `debootstrap-desktop-<k>`) and
   untar into `work/forge/<profile>/`, `sudo env -u ALLOW_BROWSER_FALLBACK ./build/all <profile> --keep=raw`,
   `pigz -9` size, `sudo sh tools/profiletest.sh boot <profile> ev/`, publish `ev/*` to prerelease
   `profiles-<run_id>` with `gh release create/upload` (needs `permissions: contents: write`).
2. Push; watch `gh run list -w profiles.yml`; read failures with `gh run view --log-failed`.
3. Fix profile-specific breakage in the four profile files only (anything in `build/forge` is
   shared: smallest edit, say so in the commit body).
4. Download the PNGs (`curl -sSLO https://github.com/ComputerDude771/auros-from-scratch/releases/download/profiles-<RUN>/<profile>.png`),
   LOOK at each, commit them to `docs/shots/profiles/`, fill the table above.
5. Report sizes and boot results in this file and in the Progress log of
   `docs/handoff/SESSION-2026-10-23.md` (append one line).

## Traps

- Any push touching `profiles/` also triggers the desktop `build.yml` (its gate greps `^profiles/`).
  Batch profile edits.
- Free plan: 20 concurrent jobs shared by ~15 agents. Keep max-parallel 1.
- `school-kiosk`'s work dir is `work/forge/school-kiosk/` (forge uses `profile_id`).
- forge refuses a build whose browser fell back to Epiphany unless `ALLOW_BROWSER_FALLBACK=1`;
  on Actions Mozilla is reachable, so do not set it.
- revive has `software_store=""`: no "Get more programs" on its desktop at all (a profile design
  choice for 2 GB RAM, not a bug in the shell). Owner may want it back; ask, don't decide.
- S1 (hardening) may add zram/oomd to every image; revive already installs zram-tools + earlyoom.
  Two zram setups would fight: check `swap=` in revive's report.
- Office has no mail program now (see the comment in office.profile).

## How to verify

`sh tools/profiletest.sh selftest && sh tools/profiletest.sh lint` locally (both exit 0). On
Actions: the `profiles` run is green, and each profile's PNG shows its desktop (office: rail in
slate; revive: taskbar in ember; school-kiosk: locked, only the browser; multilingual: rail,
French).
