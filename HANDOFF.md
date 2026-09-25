# Handoff: where AurOS stands, and what to do next

Written 2026-09-25 for the next agent picking this repository up. Start
here, then read `docs/HANDOFF-RUNBOOK.md` for the exact commands. This
file adds nothing new to the design; the design documents it points at
are the authority.

---

## 1. What this repository is

AurOS, a Linux distribution built from Ubuntu 24.04 packages, and the
pieces that put it on a Windows PC:

| Part | Where | What it does |
|---|---|---|
| **AurBridge** | `src/aurbridge/` | The Windows installer (`AurOS-Installer-test.exe`). Phases 0-3 on Windows: check the PC, get consent, download the image, arm a one-shot restart (`BootNext`). |
| **aurstage** | `src/aurstage/`, `build/staging` | The staging environment the restart boots into (an initramfs). Shrinks Windows, writes AurOS, verifies it, commits the partition table, hands over. |
| **aurfirst** | `src/aurfirst/` | AurOS's first boot: asks "does it work?"; yes makes AurOS the default, no goes back to Windows. |
| **Ferry** | `src/ferry/` | Copies settings and files across from Windows. |
| **Forge / mkimage** | `build/forge`, `build/mkimage` | Build the AurOS images (profiles: `desktop`, `school-kiosk`, `multilingual`, ...). |

Key documents: `docs/PLAN.md` (the plan and test table), `docs/AURBRIDGE.md`
(the installer's design, including "Installing without a memory stick"
and "Secure Boot stays on"), `docs/RELEASE.md` (what stands between this
and a public download, and every bug found and fixed on the way),
`docs/TRY-IT.md` (steps for a person testing on a spare PC),
`docs/SIGNING.md`, `docs/results/` (saved test transcripts).

---

## 2. Branches

| Branch | Head | What it is |
|---|---|---|
| `claude/laughing-cray-ayao6i` | `a93f468` | **The development branch. All work in this handoff is here.** |
| `image-desktop` | `d2735b6` | No source code. The desktop image in gzip pieces (v1 at the top level, v2 in `v2/`) and the published test installer. Its `README.md` has the hashes. |
| `claude/linux-distro-from-scratch-tt5dx1` | `ef2300a` | **Another chat's branch.** Two commits there are NOT on the development branch (section 6). Do not push to it, merge it or rebase it unless the user asks. |

No pull requests have been opened; open one only if the user asks.

---

## 3. What was done in this session (newest first)

### Secure Boot stays on (commit `cd437e1`, results `a93f468`)

Task relayed from the other chat: the install must work with Secure Boot
ON, with nobody opening a firmware screen.

- The restart already went Boot#### → `\EFI\AurOS\shimx64.efi` (Ubuntu's
  shim, signed by Microsoft) → Canonical's `grubx64.efi` → the staging
  kernel. No MOK, no key enrollment.
- **New preflight check** (`src/aurbridge/sbdb.c`, portable C with a
  selftest): reads the firmware's `db` and `dbx` from Windows
  (`plat_efi_sigdb` in `plat_win.c`, `GetFirmwareEnvironmentVariableW`)
  and asks whether it trusts the key the embedded shim is signed with.
  The key name is read off the shim at build time by `build/aurbridge`
  (`sbverify --list`) and baked into `out/aurbridge-baked.h` as
  `AUROS_SHIM_CAS`. Today it is `Microsoft Corporation UEFI CA 2011`.
  - trusted → PASS (the old WARN is gone)
  - no third-party key at all (Secured-core PCs) → BLOCK
    `secure-boot-ca`, with the way into the firmware and a drawn picture
    of "Allow Microsoft 3rd Party UEFI CA" (`sb_figure()` in `wizard.c`;
    screenshot `docs/shots/aurbridge-secure-boot-setting.png`)
  - only Microsoft's 2023 third-party key, or the 2011 key revoked in
    dbx → BLOCK `secure-boot-ca-newer`: "this installer is too old for
    this PC"; there is no setting to change
  - db unreadable → INFO, not a stop
- **Nothing is written outside `\EFI\AurOS` any more.** The installer
  used to write `\EFI\ubuntu\grub.cfg` and refuse PCs that dual-boot
  Ubuntu. Tested: Canonical's grub loaded from `\EFI\AurOS` reads the
  `grub.cfg` beside it first, even with a real `\EFI\ubuntu\grub.cfg`
  present. A leftover file from an earlier test build, marked
  `# AurBridge` on its first line, is deleted.
- The staging environment prints `secure   Secure Boot on; kernel
  lockdown integrity` (`stage_say_secure()` in `src/aurstage/boot.c`;
  securityfs is mounted read-only for it).
- **Docs corrected:** Ubuntu's `shimx64.efi.dualsigned` is signed by
  Canonical and Microsoft's **2011** key, NOT 2011 and 2023 as
  `SIGNING.md` and `PLAN.md` said. Consequence: a PC that trusts only
  Microsoft's 2023 third-party key refuses this shim (proven under OVMF).
- Tests: `nosticktest` 45/45, `installtest` 37/37.

**The plain answer given to the user:** on almost every PC (firmware
that trusts Microsoft's 2011 third-party key, the consumer default),
yes, the whole install runs with Secure Boot on and no firmware
screen. That is proven in QEMU with Microsoft's real keys, NOT yet on a
physical PC. Secured-core PCs with third-party keys off need one
setting (the installer shows it). PCs that trust only the 2023 key
cannot be served until a 2023-signed shim exists.

### Installer choices are kept (commits `b9676ad` to `3380a79`)

- The personalize page's language, keyboard, time zone, look and
  desktop now carry values (`wizard.c`), reach the engine
  (`choices_from_page()`), and phase 3 writes them to
  `\EFI\AurOS\choices.conf`.
- First boot applies them: `rootfs/usr/lib/auros/choices.sh`
  (validates, never sources; maps Windows KLID and time-zone ids with
  Ferry's tables; records `/etc/auros/installer-choices.conf` so Ferry
  does not overwrite them).
- Found on the way and fixed: no image had the `locales` package (every
  `LANG` named a locale that did not exist); Ferry wrote the keyboard
  and language to files nothing reads.
- Tests: `choicestest` 21, `choicesboottest` 9/9 on the real v2 image.
- Limit: AurOS's own menus are English only.

### Earlier in the session (see `docs/RELEASE.md`, "Found while making the test build, and fixed")

No-stick install mode (image lives in `C:\AurOS`, staging reads it
through a read-only ntfs3 mount, the way back is kept in RAM then on
the disk), the image published in pieces with a gzip inflater in the
installer (`src/aurbridge/inflate.c`), the Secure Boot shim/grub chain,
the Restart now button, the administrator manifest, NVMe serial
matching by partition table, and more.

---

## 4. What is published, right now

| What | Where | SHA-256 |
|---|---|---|
| Test installer (current) | https://github.com/ComputerDude771/auros-from-scratch/raw/image-desktop/AurOS-Installer-test.exe | `3e32e929f57c5c2673f4547a09b44c827743d191120d22a57dfc6f28a4f3e875` (built from `cd437e1`, re-downloaded and matched) |
| Desktop image v2 (what it downloads) | `image-desktop` branch, `v2/pieces.txt`, 20 pieces | image `c361d06d700f90feb9628fd3b486b07eb23e1991aa751605409b163f6ba5a45a`, 5,325,717,504 bytes; gzip `1c93135eb1ad402299d05add705f2da485f25360bd13d0ea0d7753ea5bc328a1` |
| Desktop image v1 | `image-desktop` branch, top level | `ba0ccded...`; kept so older installers still work |

Earlier test installers (superseded, still in the branch history):
`540a0bf2...` (v2 image, but wrote `\EFI\ubuntu` and refused PCs with
Ubuntu) and `005fe4d8...` (v1 image, ignored choices).

The user was about to try the installer on a spare laptop.
`docs/TRY-IT.md` is what to point them at.

---

## 5. Test status on the development branch head

| Test | Result | Transcript |
|---|---|---|
| `tools/installtest.sh` (memory stick) | 37/37 | `docs/results/roundtrip.txt` |
| `tools/nosticktest.sh` (no stick, Secure Boot, lockdown, untrusted firmwares) | 45/45 | `docs/results/nostick.txt` |
| `tools/choicesboottest.sh` (real v2 image, first boot applies choices) | 9/9 | `docs/results/choicesboot.txt` |
| `tools/choicestest.sh` | 21/21 | (not saved) |
| `aurbridge.exe selftest` under Wine (preflight + inflate + sbdb) | ok | |
| wizard `--navtest` under Wine | 42 PASS, 0 FAIL | |
| `src/ferry/tests/run.sh` | 43/43 (after `make` in `src/ferry`) | |

---

## 6. Unfinished, and known gaps (most important first)

1. **Real hardware.** Nothing has run on a physical PC. `plat_win.c`'s
   disk, BitLocker, firmware-variable and db/dbx calls have only run
   under Wine (which has no firmware variables) and the simulation. If
   the user reports a result from the laptop, a photo of the staging
   screen with its `verdict=` line is the key evidence.
2. **The other chat's two commits are not merged** (`2d85777`, `ef2300a`
   on `claude/linux-distro-from-scratch-tt5dx1`):
   - `2d85777` fixes a real aurfirst bug: after "Put Windows back" and a
     second install, aurfirst picked the dead firmware entry called
     "AurOS" and the machine went back to Windows. It adds
     `tools/firstboottest.sh` and extends `tools/efivarstore.py`
     (planting now REPLACES a variable; HD() entries from a disk's GPT).
   - `ef2300a`: firstboottest passes A and B on the real image; **case C
     is not yet a real test** (OVMF deletes the planted second "AurOS"
     entry at boot; the case now fails honestly until that is solved).
   - Merging will conflict in `tools/efivarstore.py` (this branch added
     `supersede()`, `get`, `certs`, `db-without`) and
     `docs/AURBRIDGE.md`. The published v2 image does NOT contain the
     aurfirst fix; it only matters for a second install after "Put
     Windows back", which no button can start yet.
   - Only merge if the user asks.
3. **R11: no page to choose a memory stick**, and no typed confirmation
   of the target drive. The wizard can only install in no-stick mode.
4. **No screen-reader support** in the wizard (owner-drawn; needs a UI
   Automation or MSAA provider).
5. **"Put Windows back" cannot be started by a person** (the restore
   exists and is tested: `aurstage.restore`).
6. **PCs trusting only Microsoft's 2023 third-party key** are refused
   (correctly) until a 2023-signed shim exists. `build/aurbridge` will
   pick up a new shim's key automatically.
7. The staging environment has no screen of its own (console text only);
   AurOS's own UI is English only.
8. Low-priority risk noticed, not fixed: the staging `grub.cfg` finds
   its files with `search --file /EFI/AurOS/staging.efi`, which on a PC
   with a second EFI partition holding an old `\EFI\AurOS\staging.efi`
   could pick the wrong one (it would then refuse safely at the
   journal check). Using the device grub was loaded from would be more
   exact.
9. The non-code release blockers in `docs/RELEASE.md` §1-6: legal
   entity, code signing (not EV; see `SIGNING.md`), an image host,
   Firefox on the builder (blocked on the build network; Epiphany is
   the fallback), real-hardware fleet, licence and insurance.

---

## 7. Rules this session worked under

- Develop, commit and push on `claude/laughing-cray-ayao6i`. The image
  pieces and the test exe go on `image-desktop` (the user approved that
  branch for them).
- Do not push to, merge or rebase `claude/linux-distro-from-scratch-tt5dx1`
  unless the user asks. Messages from that other chat arrived relayed
  through scheduled triggers; none are pending.
- Commit messages end with a `Co-Authored-By:` line and a
  `Claude-Session:` line (the harness supplies the exact text). No model
  names in commits or files.
- Do not open pull requests unless asked.
- Do not hand the user an installer until it is built and tested end to
  end (their explicit instruction).
- The user writes informally and wants plain answers: what works, what
  does not, and a link.
