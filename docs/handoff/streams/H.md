# Stream H: other systems (phase 2): official Ubuntu beside Windows

Last updated 2026-10-23 04:20 UTC. Branch `claude/brave-hypatia-x72g1m`.
Common rules: `docs/handoff/STREAMS.md`. Design and hooks: `docs/OTHER-SYSTEMS.md` (read it first).

## Goal

She picks an official, unmodified system (first: Ubuntu 26.04.1 LTS; then
Xubuntu, Ubuntu Cinnamon, Ubuntu Budgie). AurOS prepares the disk from
Windows. Then Canonical's own installer (subiquity/curtin/casper) installs it
unattended into the two partitions AurOS names, under Secure Boot, with no
USB stick, with Windows kept byte-identical and **starting first until she
says "yes, everything works"**.

**P1** = the proof of that in a KVM VM on GitHub Actions (OVMF Secure Boot
with Microsoft's keys, NVMe, no network, the real Ubuntu ISO verified
against Canonical's signature).

## Files (stream H owns these only)

- `src/othersys/ubuntu/plan.py`: from layout.json + boot.json +
  choices.json, writes the seed: `user-data` (autoinstall), `meta-data`,
  `boot/grub/grub.cfg`, `othersys/` (helper + plan.json + SHA256SUMS).
  It refuses (exit 2) any layout that could let curtin touch Windows.
- `src/othersys/ubuntu/othersys.py`: the helper. Commands:
  - `early`: is the disk the one Windows recorded? Refuse otherwise.
  - `guard`: live session; keeps Windows first through the install.
  - `late`: firmware fix-up, cancel MOK, install the units.
  - `error`: install failed; Windows first, answer=failed.
  - `boot`: every Ubuntu start. Removes the ISO partition, grows root,
    holds Windows first until answered.
  - `answer`: yes/no/later from the question.
  - `status`.
- `src/othersys/ubuntu/{ask,units/*}`: the zenity question and the systemd units.
- `src/othersys/verify.py`: gpgv check of SHA256SUMS against the pinned
  key, then the pinned hash, then the ISO's own hash.
- `src/othersys/systems/*.json`: one manifest per system. Ubuntu plus the
  3 flavours; all have verified signed hashes and `source_id`.
- `src/othersys/keys/ubuntu-cdimage-2012.gpg`: the pinned Canonical key
  (843938DF228D22F7B3742BC0D94AA3F0EFE21092).
- `tools/othersys/`:
  - `mkdisk.py`: the test fixture. Lays out a 48 GiB disk as Windows 11
    leaves it, writes AurOS's 3 partitions and plants the firmware
    entries the way the Windows engine must.
  - `vmtest.py`: the VM phases plus results.md/json.
  - `checks.py`: every check.
  - `selftest.py`: every check and refusal proven to fail. No VM;
    81 checks, about 1 minute.
  - `run.sh VARIANT`, `runner-setup.sh`, `getiso.sh`.
  - `publish.sh`: evidence goes to the prerelease `othersys-<run id>`.
  - `isoinfo.py`: ISO facts over HTTP byte ranges.
  - `hooks/`: TEST ONLY serial reporters.
  - `fixtures/`: Canonical's real SHA256SUMS(+.gpg).
- `.github/workflows/othersys.yml`:
  - Jobs: gate, selftest, prep (creates the prerelease), vm matrix
    `[ntfs, powercut, nofixup, bitlocker]`, max-parallel 2.
  - Runs on a push touching these paths, or on a first commit line
    containing `[othersys]`, or via workflow_dispatch.
  - A first-line `[othersys-skip]` suppresses it.
- `docs/OTHER-SYSTEMS.md`: the flow, the traps and the exact hooks the Windows engine must write.

## Done and proven

- **selftest** 81/81 locally. Green on Actions in run 39897837863
  (commit 475c8bf, 66 checks at that time).
- **KVM run 39897837863** (commit 475c8bf). Evidence:
  `https://github.com/ComputerDude771/auros-from-scratch/releases/download/othersys-39897837863/<variant>-results.md`
  (also `<variant>-evidence.zip`, `<variant>-live.txt/png`,
  `serial-*.log`, desktop screenshots).
  - ntfs **install phase all green**:
    - Firmware started the ISO's shim from BootNext.
    - The early disk check matched.
    - Unattended install, BootNext to power-off: **570 s**.
    - Windows partitions byte-identical, GPT entries unchanged.
    - No MOK queued.
    - Exactly one "ubuntu" entry, on AUROS-OS-ESP.
    - The restart entry was deleted.
    - Final firmware: BootOrder `0002,0000,0001,0004` (Windows first),
      BootNext `0004` (Ubuntu).
    - The helper recorded the install.
    - curtin did NOT delete the Windows Boot Manager entry.
  - nofixup **green as a negative proof**:
    - Without the fix-up, the installer leaves BootOrder
      `0004,0002,0000,0001` (Ubuntu first).
    - The Windows-first check FAILS, as it must. Install time: 584 s.
  - ntfs **firstboot green**:
    - Power on to her desktop, signed in automatically: 55 s.
    - No MokManager screen in 8 screenshots.
    - Windows still byte-identical.
    - The ISO partition is deleted and Ubuntu has grown into its space.
    - Still Windows first with BootNext = Ubuntu (the hold).
  - ntfs **windowsfirst green**:
    - BootNext cleared: Windows Boot Manager in 2 s; Ubuntu did not start.
  - Pending at 04:20: ntfs confirm / afteryes / moknegative; bitlocker.

- Local TCG smoke (no KVM in the container): under Secure Boot + MS keys,
  the path BootNext → the ISO's shim on the FAT seed → grub → our
  grub.cfg → kernel → casper mounts the raw ISO partition works.
  When grub.cfg refuses, the firmware falls through to "Windows Boot
  Manager" by itself.

## Pushed but NOT yet run in a VM (all pushed with [othersys-skip])

- `df35590`: flavour manifests, plus `--system` in mkdisk/run/getiso.
  The matrix does not include the flavours yet.
- `3f96c43`: `AurOSOtherSysAnswer` NVRAM variable, vendor GUID
  `b89b3dc4-fc68-4749-a5e5-f3eb5f51d569`, data `answer=yes|no|failed\nsystem=<id>\n`.
  Written on yes/no/failed. vmtest's confirm phase checks it.
- `6c98a00`: the `guard` (power-cut safety), the `error` handler, and the
  `powercut` VM variant. The workflow gained `workflow_dispatch`.

## Next 5 steps

1. Read run 39897837863's results (`ntfs-results.md`,
   `bitlocker-results.md`, `nofixup-results.md`). Fix any red check;
   look at `ntfs-shots`/`*-desktop.png` (the zenity question must show
   on her desktop, signed in automatically).
2. Start a VM run on HEAD so the guard, error and answer-variable
   changes are proven in KVM:
   `gh workflow run othersys.yml --ref claude/brave-hypatia-x72g1m`.
   If dispatch is refused because the file is not on the default branch,
   push a trivial change under `tools/othersys/` with `[othersys]` in the
   first line. Expect `powercut-results.md`:
   - the guard acted;
   - at the cut, Windows first and no BootNext;
   - the next start is Windows.
3. Add `xubuntu, ubuntucinnamon, ubuntu-budgie` to the matrix (run.sh
   already maps them), keeping max-parallel 2. Their flavour slideshow
   and session names differ: check `OTHERSYS-TEST SESSION-UP` still fires
   (`tools/othersys/hooks/report`, autostart desktop file).
4. A "no" VM phase: inbox answer `no`, then check BootNext is cleared,
   `AurOSOtherSysAnswer=answer=no`, and the next start is Windows.
   Optionally the `error` path, via `plan.json test.fail_early` (exists).
5. Hand the engine (Windows side, not stream H) its contract from
   `docs/OTHER-SYSTEMS.md` "The data the Windows side must write":
   - 3 partitions;
   - layout/boot/choices JSON, and a C port of plan.py;
   - Boot#### "AurOS Installer" plus BootNext;
   - the OpenPGP verifier;
   - the `AurOSOtherSysAnswer` reader and the put-back.

## Traps (found the hard way)

- curtin v2 **wipes and drops every partition not listed** with
  `preserve: true`. It **writes a preserved partition back without its
  type GUID** unless `partition_type` is given. plan.py lists all
  partitions with offset/size/type/uuid; the lint checks it.
- casper booted from a disk partition **appends an ext4 "writable"
  partition** to that disk unless `nopersistent` is set. grub.cfg also
  passes `live-media=/dev/disk/by-partuuid/<ISO>`.
- curtin puts "ubuntu" **first** in BootOrder. No autoinstall key stops
  it. `late` and the guard put Windows back; the boot unit re-asserts it
  until "yes".
- The ISO's signed grub (CD build) leaves **`$cmdpath` empty**. Take the
  disk from `$root`. `probe --part-uuid` and `--fs-uuid` pin the ISO.
- The stand-in efivarfs in selftest is a plain directory, so `var_set`
  truncates only when `OTHERSYS_TEST_EFIVARS` is set.
- Container: ~5 GB disk shared by ~15 agents, and no KVM. **Never
  download an ISO locally.** Use `isoinfo.py` (HTTP ranges) or Actions.
  Don't `pgrep -f`/`pkill -f` with a pattern your own shell matches.
- Actions artifacts can't be fetched from the container. Evidence lives
  on the prerelease; curl it.

## How to verify

- `python3 -I tools/othersys/selftest.py`: expect "81 checked, 0 not as
  expected". Needs sgdisk, sfdisk, mtools, ovmf, gpgv, python3-pil and
  tesseract.
- `gh run list --workflow othersys.yml --repo ComputerDude771/auros-from-scratch`.
  Then:
  `curl -sL https://github.com/ComputerDude771/auros-from-scratch/releases/download/othersys-<run>/ntfs-results.md`.
  Every row needs `ok = yes`. A "must fail" row is `expected=fail` with
  `ok=yes`.
- Live progress during a run: `<variant>-live.txt` and `<variant>-live.png`
  on the same prerelease (every 2 min).
