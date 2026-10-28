# v3: what was done, item by item

Worked on 2026-09-26, on branch `claude/confident-johnson-hxevk4`, which
is now the one line of work: `claude/laughing-cray-ayao6i` (no-stick
install, installer choices, Secure Boot through shim) was merged into
the original line's handoff, and with it the `aurfirst` fix `2d85777`.

Status words: **done** (built and tested, the test named), **partly**
(what is done and what is not are both said), **not done** (and why),
**cannot be done in code** (and what it takes instead).

## 1. The installer would not start: done

- `src/aurbridge/aurbridge.manifest` has no comments now; the
  explanation moved into `build/aurbridge`.
- `tools/pe_payload.py check` parses a manifest strictly, or the one
  inside a built `.exe`. `build/aurbridge` runs it before `windres` and
  on the linked wizard, and refuses to finish otherwise.
- `tools/manifesttest.sh` (10 checks) proves the gate goes red: `--` in a
  comment, `asInvoker`, a truncated manifest, the build refusing one end
  to end, the same `.exe` with `--` written into it, and the published
  `3e32e929` build. With the old manifest put back, it fails.
- Proven on real Windows: the old build fails there with the exact
  error the tester saw; the new one starts.

## 2. Antivirus: partly

- Done: `docs/TRY-IT.md` says what Avast/AVG do and how to add an
  exception; the `image-desktop` README says it too.
- Not done: code signing (item 11), and submitting to antivirus vendors,
  which needs a signed build and a person to do it.
- Not done: testing with antivirus running during an install.

## 3. Nothing ran on real Windows: done

`.github/workflows/windows.yml`, on every push, on a real Windows VM
(GitHub Actions `windows-latest`):

- the console tool's selftest, and preflight read-only against the
  machine;
- the wizard's navigation test, its selftest (window, preflight), and a
  plain start, the way a double-click starts it;
- the storage check, proven to find the real controller's driver;
- WinHTTP downloading all 20 image pieces through the installer's own
  inflater, `verdict=ok`;
- the published installer, downloaded from the public link, pinned by
  SHA-256, started;
- the known-bad build, required to fail to start.

Its first runs found two more bugs, both fixed (item 9).

## 4. Republish: done

`AurOS-Installer-test.exe` on `image-desktop` is **`04089423`**, built
from `ed67e18` (it fixes what the check finds; before it, `c70045fb` from `1bef5d7`), downloading the v3 image (`5dee5f44`). Before it was
published: `installtest` 37/37, `nosticktest` 45/45, `putbacktest`
16/16, `firstboottest` 32/32, `choicesboottest` 9/9, `manifesttest`
10/10, and every job in `windows.yml` on real Windows, including the
download of the whole v3 image by the installer's own code. After it
was published, `windows.yml` downloaded it from the public link, checked
its SHA-256 and started it. The README there says what was tested where.

In between (2026-09-26), `f971559d` was the first build that starts on
Windows; it installs the v2 image, without Put Windows back. `3e32e929`
is marked as the build that does not start.

## 5. The installer

| | | |
|---|---|---|
| 5.1 | Put Windows back | **done** (see below) |
| 5.2 | Memory-stick page, R11 typed confirmation | **not done**: a new wizard page with drive selection; the engine and tests for stick mode exist. |
| 5.3 | Screen readers | **not done**: needs a UI Automation or MSAA provider for the owner-drawn wizard. |
| 5.4 | Profile picker | **not done**: only the desktop image is published, so a picker would offer images that cannot be downloaded. |
| 5.5 | Staging has no screen of its own | **done** (2026-10-04): `src/aurscreen` paints the seven steps, aurstage's own sentence, a bar, and "Do not turn the computer off" from the first irreversible write; a refusal says "AurOS was not installed" in red with the `verdict=` line at the foot for a photo. Fed by PID 1 through a non-blocking pipe, so a screen that fails costs the install nothing. `tools/screentest.sh` 25/25 (including following a real install's serial log to the handover); `installtest` 37/37 with it running, screenshots in `docs/shots/staging-*.png`. |
| 5.6 | AurOS's own text is English only | **not done**. |
| 5.7 | PCs that trust only the 2023 key | **cannot be done in code**: needs a shim signed with Microsoft's 2023 key. Checked 2026-10-04: no Ubuntu shim has one (shim-signed 1.59 is Canonical + 2011; README used to claim 2011 + 2023, corrected). `build/shimpick` now refuses a shim without the 2011 signature instead of falling back to whatever file exists; an installed AurOS takes a 2023-signed shim by itself the day the archive has one and its firmware trusts it (7.4). |
| 5.8 | Secured-core firmware setting | unchanged; the installer shows the setting. |
| 5.9 | Staging grub.cfg found its files by search | **done**: the partition grub started from (`$cmdpath`) first, the search as fallback. |
| 5.10 | Closing the window threw a ready install away | **done**: it asks, with No as the default. |

**5.1, Put Windows back.**
- AurOS's own start-up menu: "Put Windows back (remove AurOS)", a
  submenu whose default is "No, keep AurOS"; "Yes" starts the
  installer's Canonical-signed staging kernel with `aurstage.restore`,
  so it works with Secure Boot on and when AurOS does not start.
- Settings: a "Put Windows back" row on a converted machine; the remove
  row must be pressed twice.
- `answer.sh` and `aurfirst putback`: the fourth word. It checks the
  restore's files are present, chooses the menu entry once (grubenv),
  arms BootNext even on a machine that declined, and restarts; a failure
  takes back what it wrote.
- Tests: `aurfirsttest` 65/65; `targets` measures the new page;
  `tools/putbacktest.sh` boots the real image with Secure Boot on from
  the button to `aurstage.restore` and not again: 16/16 on the v3
  image (`docs/results/putback.txt`). Its first run failed, and found
  that `answer.sh`'s sandbox made `/run` and `/boot` read-only; fixed.
- On the way: `request.c` matched the word after throwing away every
  non-letter, so "con firm" read as `confirm`. It is exact now.

## 6. First boot

| | | |
|---|---|---|
| 6.1 | The `aurfirst` fix was in no branch or image | **done**: merged; `aurfirsttest` 54, now 65 with the putback cases; in the v3 image. |
| 6.2 | `firstboottest` case C | **done**: OVMF does not prune; when it adds an entry it takes the first number neither `BootOrder` nor `BootNext` mentions and writes over what is there. Reproduced with OVMF and no OS; the fixture now lists the other entry in `BootOrder`, which is also how a confirmed-then-restored install leaves it. Result: `firstboottest` 32/32 on the v3 image, case C included (`docs/results/firstboot.txt`). |
| 6.3 | Dead "AurOS" entry after a restore | unchanged, accepted and documented. |

## 7. Images, build and hosting

| | | |
|---|---|---|
| 7.1 | No Firefox | **cannot be done here**: this environment's network refuses every Mozilla host. The environment's network policy is the fix. |
| 7.2 | Rebuild images | **partly**: the desktop image is rebuilt and published as `v3/` (`5dee5f44`); the other four profiles are not rebuilt (nothing downloads them yet). |
| 7.3 | Image hosted on a git branch | **not done**: moving to GitHub Releases needs a release made from the GitHub page or an account with that permission. |
| 7.4 | No update channel | **done for the boot chain** (2026-10-04): the kernel, grub and shim take security updates like everything else. `update-grub` and `grub-install` are diverted to `rootfs/usr/lib/auros/bootchain`, which rewrites AurOS's own menu (the previous kernel as grub's automatic fallback, by entry number) and installs a new shim + grub on AurOS's EFI partition only after checking, the way EDK2 and shim do, that this firmware starts the shim and the shim starts the grub, every kernel in the menu and the Put Windows back kernel on Windows' partition (SBAT included). When no new shim passes, a new grub goes in alone if the installed shim starts it. BootOrder is never written; `grub-multi-install` never lists or mounts Windows' partition. `tools/bootchaintest.sh` 71/71 (every check also switched off and shown to stop refusing); `tools/bootupdatetest.sh` boots the real image under Secure Boot through a real `dpkg`/`apt` update, a new kernel, and a damaged kernel grub must fall back from. Two independent reviews; their findings are fixed. **Still open:** AurOS's own programs have no update path. |
| 7.5 | Azure Artifact Signing step | **not done**: cannot be tested without an account. |
| 7.6 | Manifest unchecked by the build | **done** (item 1). |

## 8. Ferry

**Not done**: Wi-Fi passwords (offline SYSTEM-DPAPI), Chrome/Edge
passwords, mail, the app list. **Partly** for OneDrive (8.9): the
wizard's backup page now says online-only files will not come across
and how to make them local first.

## 9. Real hardware: partly

The installer has run on a real Windows machine (a VM on real Windows,
item 3), which found two preflight bugs, both fixed:

- **The start-up partition.** Preflight wanted 96 MB free (an old guess)
  and said "it will start from your recovery stick" in an installer
  with no stick. It measures what it carries now (about 33 MB) and
  refuses, with the sizes, when that does not fit; phase 3 checks again
  before its first write.
- **Intel RST.** It warned any PC where Intel's storage driver was
  merely installed, which Windows does everywhere. It now asks the
  device tree which driver runs the Windows disk's controller, and the
  workflow proves that walk finds the real one.

What remains is what item 9 says: installs on real PCs.

## 10. Documentation and website

| | | |
|---|---|---|
| 10.1 | Website out of date | **done**: `download.html`, `safety.html`. |
| 10.2 | Two handoffs | **done**: one `HANDOFF.md`, and `docs/handoff/` updated. |
| 10.3 | "Tested" without saying where | **done**: the `image-desktop` README separates real Windows from simulation. |
| 10.4 | TRY-IT gaps | **done**: antivirus, and what the side-by-side error was. |
| 10.5 | "One file" and eight steps | **partly**: the installer fixes Fast Startup, low space, BitLocker and a waiting restart itself (`src/aurbridge/fix_win.c`), and carries on by itself when the charger or a USB drive is sorted. Still refusals: legacy BIOS PCs, failing drives, multi-drive setups. |
| 10.6 | Theme template placeholder | checked: only in `aurora new`'s scaffold, never shipped. |

Also found and fixed: `build/aurbridge` died silently on a developer
build with no shim (`[ ] &&` as a loop's last command under `set -e`);
the staging restore told people without a stick to plug one in;
`tools/welcometest.c` could not pass when built the way
`tools/README.md` says (it counted the answer directory as the panel's
file, and never created it).

## An independent review of all of the above

A separate reviewer read every source change made for v3, looking for
defects. Its findings, all fixed and in the v3 image and installer:

1. "Put Windows back" could fire by accident: a double-click, or Enter
   held for half a second, made both presses. Enter no longer repeats
   into panels, the second press must come 1.5 s after the first, and
   the menu then shows the restore for ten seconds, not three.
2. Automatic updates would have regenerated `grub.cfg` and dropped
   AurOS's menu, and could have reordered `BootOrder` (see 7.4).
3. A second attempt could be refused for good: preflight counted an
   earlier attempt's files as used. Proven fixed on a real Windows EFI
   partition by `windows.yml`, with the opposite case refusing.
4. Taking back `next_entry` after a failure was not checked.
5. Phase 3's room check was too strict with leftovers, and let a
   completely full partition through.
6. `auros-answer.service` failed as a whole on a root without
   `/boot/grub`.
7. Settings said "Restarting" when a request was already waiting.

Also found by it: two backticks in `build/mkimage`'s `grub.cfg` heredoc
that ran commands on the build host.

## 11. Non-code release blockers: cannot be done in code

A legal entity, a code-signing certificate, a licence and support
path, insurance, a tested hardware list. `docs/RELEASE.md` has the
steps and costs.

## Found on 2026-10-04, while closing 5.5 and 7.4

Each was found by a test or a reviewer, and each has a test now.

- **The start-up menu printed "error: prohibited by secure boot policy"**
  on every start, up to 54 times: Canonical's grub refuses `loadfont`
  and every module from disk under Secure Boot, and the menu fell back to
  grub's ASCII font (its frame drawn in `?`). The menu is on the
  firmware's text console now, and no modules are copied to the prefix.
- **grub's fallback never worked.** `set fallback=<id>` is silently
  ignored by Ubuntu's grub 2.12 (it takes a number); with a kernel that
  would not start, the machine sat at the menu for ever. Seen in
  `bootupdatetest` boot 3 before the fix, passing after.
- **Shim and grub package updates changed nothing that starts the PC**:
  their postinsts only call grub-install when `/boot/grub/x86_64-efi/core.efi`
  exists, which only grub-install makes.
- **"Nothing has been changed" after Windows had been shrunk.** The
  hardware check after the shrink reused the dry run's sentence. It now
  says the drive is smaller but still works.
- **`sbverify --cert` is not a trust check**: it passes a shim against a
  certificate made up a moment ago. See `docs/handoff/TRAPS.md`.
