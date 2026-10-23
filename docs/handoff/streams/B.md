# Stream B: the Windows installer wizard and preflight

Written for an engineer who has only this repository. Branch:
`claude/brave-hypatia-x72g1m` (common rules: `docs/handoff/STREAMS.md`;
owner decisions D1-D11: `docs/handoff/SESSION-2026-10-23.md`).

## Goal

`AurOS-Setup.exe` (built as `out/aurbridge-wizard.exe`) puts AurOS beside
Windows for someone who is not technical. The rules it keeps:

- Three screens and no typing: **Welcome** (the read-only check runs here),
  **Pick the look you want** (4 picture cards), **Ready** (what stays, the
  sizes, the time, one restart), then **Install AurOS** → a progress bar →
  **Restart now**.
- It stops only for real dangers (owner decision D4) and fixes what it can by
  itself. When nothing on the PC can fix it, the page gives the reason in one
  line, then *What you can do*, and has a single button: **Close**.
- Plain words for everything she sees (`docs/EASY.md`). Esc never closes
  anything. Every target is at least 44 px. Every page fits 1366x768 and
  1024x600 without scrolling. Narrator can read every page and press its
  buttons.
- D10 (a pre-ticked *Bring my Wi-Fi across*) and D11 (*Windows' drive lock is
  paused for one restart*) are built in, but switched off until the engine can
  keep them (see "Half-done").

## Files (stream B owns these)

| File | What it is |
|---|---|
| `src/aurbridge/wizard.c` | The whole UI. Everything is drawn by hand into a GDI back buffer (`render()`). Widgets: `W_BUTTON`, `W_CARD` and `W_TOGGLE` (only the Wi-Fi box). Pages: `page_welcome/blocked/style/ready/progress`. Test modes: `--navtest DIR`, `--selftest DIR`, `--shot DIR`. The screen-reader object (MSAA `IAccessible` written in C, the `acc_*` functions) sits just above the "Window" section. `choices_from_page()` fills `ab_choice` for the engine. |
| `src/aurbridge/preflight.{c,h}` | Read-only checks that produce a `pf_report`. `check_wifi()` sets `wifi_connected` and `wifi_name`, loads the WLAN API at run time and reads no key. The BitLocker policy depends on `AB_CHOICE_BITLOCKER_PAUSE`. |
| `src/aurbridge/fix.h`, `fix_win.c` | A short line for each preflight id, the automatic fixes, and `fix_instead()` (the *What you can do* sentences). `ADVANCED` is the helper page's address, built from `AUROS_SITE_W`, which `build/aurbridge` gets from `tools/repo.sh` (since `a2e7661`). |
| `src/aurbridge/sbdb.*` | Secure Boot trust database. |
| `src/aurbridge/aurbridge-info.rc`, `auros.ico` | VERSIONINFO ("AurOS Setup", OriginalFilename `AurOS-Setup.exe`, version from `AUROS_VERSION`) and the teal diamond icon. |
| `docs/shots/wizard/*.png` | Every page as it is rendered now: 24 pictures, including the 150% and 1024x600 variants. |
| `tools/wizard/check.sh`, `bmp2png.py` | The full local check (see "How to verify"). |

Edited outside B's files, as the smallest change each time: `build/aurbridge`
(added `-loleacc -loleaut32 -lole32 -luuid` to the wizard link) and
`.github/workflows/windows.yml` (the `screen reader    :` check in the
selftest step, and the bounded, retried `toolchain` step in `e6a888b`).

## Done and pushed

Commits `ed5381f` to `b4ab898` (`git log --oneline -- src/aurbridge/wizard.c`):

- `ed5381f`: three screens, no typing, and she picks the look. `525c4dd`: D4
  policy, and the stop page says why before it does anything.
- `c47eac1`, `075b022`: the image starts downloading while she reads, the PC
  stays awake, and the progress page.
- `ea9c296`: Ready gives the time from her own connection speed and the disk
  type (SSD or HDD).
- `9a6dc54`: Ready's sizes follow `shrink_pick_target` (`src/aurstage/shrink.c`):
  "AurOS takes 40 GB beside Windows … Windows keeps N GB free". It gives an
  honest decryption time, and the "can't install" page shows *What you can do*
  with one **Close** button.
- `a9c285e`: MSAA. The page name, the buttons, the look cards (radio buttons,
  the chosen one checked) and the Wi-Fi box (a check button) can all be read
  and pressed.
- `993ecf3`: D10/D11 UI and the preflight Wi-Fi probe, both behind the engine's
  defines.
- `b4ab898`: the BIOS and Intel-NVMe *What you can do* lines name
  `computerdude771.github.io/auros-from-scratch/advanced.html` (the page
  itself is stream K's).

Tests: `--navtest` has 12 sections and about 75 checks, all passing under Wine.
`--selftest` asks the real window through `AccessibleObjectFromWindow`. The
console `aurbridge.exe selftest` passes. `aurbridge-sim gzselftest` and
`sbselftest` pass.

**Not yet seen on real Windows.** Every windows.yml run since `ed5381f` was
superseded or cancelled. Two runs in a row hung in the `build` job's
`toolchain` step (`apt-get update`): 39894915049 for about 35 minutes and
39897928905 for 20 minutes. Both were cancelled by hand. `e6a888b` bounds that
step: each apt call has a time limit, it tries three times, and the step stops
at 12 minutes. Run 39899942038 (`df92af8`) then got through the toolchain step in 25 s but
stopped at the manifest gate (see Traps). The gate was fixed in `1e31149`;
the first windows.yml run at or after that commit is the one to watch.

## Half-done

- **D10/D11 are off.** They switch on when the engine (stream A2) adds these
  to `src/aurbridge/phases.h`: `int bring_wifi; int bitlocker_pause;` in
  `ab_choice`, plus `#define AB_CHOICE_BRING_WIFI 1` and
  `#define AB_CHOICE_BITLOCKER_PAUSE 1`. With them:
  - Ready shows the box, but only when `pf_report.wifi_connected` is set.
  - `choices_from_page()` sets `c->bring_wifi = shown && ticked` and
    `c->bitlocker_pause` (any volume with `bitlocker == 1`).
  - Preflight turns `bitlocker-system/-other` from a stop into an info note
    and measures the locked C: through the volume.

  Both paths were compiled and passed navtest against a local stub that was
  never committed. Reading the Wi-Fi key and writing the NetworkManager
  keyfile belong to the engine, not the wizard.
- Nothing is left unpushed.

## Next steps

1. **Get one green windows.yml run.** Run
   `gh run list --branch claude/brave-hypatia-x72g1m --workflow windows --limit 5`,
   then `gh run view <id> --json jobs`.
   - If `toolchain` fails after its three tries, the runner's apt mirror is
     the cause. Use `gh run rerun <id> --failed`.
   - Then read the `windows-source` job log: the selftest step must show
     `screen reader    : "…", N parts, button "…"` and `preflight done   : 1`.
   - If the WLAN probe or MSAA misbehaves on Server, fix `check_wifi()` in
     `preflight.c` or `acc_*` in `wizard.c`.
2. **When A2 lands the defines:** run `sh tools/wizard/check.sh`. Navtest
   section 12 must print the six "on Wi-Fi" checks, not "the engine cannot
   carry it yet". Add a permanent shot of Ready with Wi-Fi and BitLocker:
   in `--shot` mode after `shot_save(dir, "4a-ready")`, set
   `g_report.wifi_connected`, `wifi_name` and `volumes[0].bitlocker = 1`,
   call `shot_save(dir, "4b-ready-wifi-lock")`, then reset them. Re-shoot
   `docs/shots/wizard/`.
3. **End to end with the engine:** make sure the engine skips decryption when
   `bitlocker_pause` is set, and that unticking the Wi-Fi box means no keyfile.
   That is A2's code; B's part is that `ab_choice` carries her answer
   (navtest checks this).
4. **Check that stream K published `website/advanced.html`.** The address is
   `sh tools/repo.sh host` plus `advanced.html`, written into the wizard at
   build time (`ADVANCED` in `fix_win.c`). Re-shoot if the wording changes.
5. **Narrator on a real PC** (manual): Win+Ctrl+Enter, then Tab and Enter
   through all three screens. Each page should be read once when it appears,
   not every second.

## Traps

- **Disk fills.** The shots are 32-bpp BMPs. `check.sh` renders them in
  `/dev/shm` and keeps only PNGs.
- **`tools/manifesttest.sh` builds a copy of only part of the tree:**
  `build/`, `src/aurbridge/`, `tools/pe_payload.py` and `tools/repo.sh`. When
  `build/aurbridge` starts needing another file, add it to that copy, or
  windows.yml stops at the manifest gate, before anything runs on Windows.
  This happened in run 39899942038 and was fixed in `1e31149`.
- **build/aurbridge has a gate:** every `add(r, "<id>", …, PF_BLOCK` in
  `preflight.c` needs a line in `fix_win.c`, or the build stops.
- **`BITLOCKER_REMEDY` is `#ifndef AB_CHOICE_BITLOCKER_PAUSE`**, to avoid an
  unused-variable warning on the pause path. Keep the two in step.
- **Never link `-lwlanapi`.** Windows Server (the CI runner) has no
  wlanapi.dll; it is loaded with `LoadLibraryA`.
- **`CINTERFACE` and `COBJMACROS` come before `<windows.h>`** in `wizard.c`;
  the C `IAccessible` needs them.
- **Page names for the screen reader must not contain numbers that tick**
  (seconds or minutes left), or Narrator re-reads the page on every change.
  See `acc_page_name()`.
- **Navtest's "plain page" rule:** pages may hold only buttons, cards and the
  one `ID_WIFI` toggle, and the toggle never holds the way on. A new kind of
  widget fails navtest on purpose.
- **windows.yml uses `concurrency: cancel-in-progress: false`.** One hung run
  blocks every later one, and a newer push replaces the pending run, so most
  runs show "cancelled". A pending run is not a failure.
- **Every stream appends to the session log at once.** On a rebase conflict,
  keep both lines (delete only the conflict markers).
- Correction to an earlier message to the lead: the hung run 39894915049 ran
  about 35 minutes, not 90.

## How to verify

```sh
# Linux with mingw-w64, wine, xvfb, python3; from the repository root
sh tools/wizard/check.sh      # build + navtest + selftest + console selftest + shots
# ends "WIZARD CHECK: PASS"; look at out/wizard-shots/*.png
cp out/wizard-shots/*.png docs/shots/wizard/   # only when the pages changed
```

On real Windows, windows.yml's `windows-source` job runs the same `--selftest`
and fails if the screen-reader line is empty.
