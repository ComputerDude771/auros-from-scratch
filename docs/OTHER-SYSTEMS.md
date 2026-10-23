# Other systems: an official Ubuntu beside Windows, through AurOS

Phase 2 of "an OS she wants". She picks an official, unmodified system
(first: **Ubuntu 26.04 LTS**); AurOS prepares the disk from Windows, and
the publisher's own installer installs it unattended into the partitions
AurOS names. The research that chose this route is
`docs/research/other-systems-2026-10.md`; this file is what was built,
what was found on the way, what is proven, and the hooks the install
engine and the wizard have to provide.

Code: `src/othersys/` (product), `tools/othersys/` (tests),
`.github/workflows/othersys.yml` (the VM proof on Actions).

## The flow

```
WINDOWS (AurOS's install engine; hooks below)
  download   ISO 6.48 GB + SHA256SUMS + SHA256SUMS.gpg from releases.ubuntu.com
  verify     signature by the pinned Canonical key, pinned hash, ISO hash
  disk       shrink C:; create three partitions in the gap:
               AUROS-OS-ESP   512 MiB  type ESP, FAT32, label CIDATA  (the seed)
               AUROS-OS-ROOT  >= 25 GiB type Linux                    (empty)
               AUROS-OS-ISO   ISO+1MiB type Linux                     (the ISO, raw)
  seed       copy the ISO's \EFI\boot\{bootx64,grubx64,mmx64}.efi to
             \EFI\AurOS\{shimx64,grubx64,mmx64}.efi on the seed, plus
             boot\grub\grub.cfg, user-data, meta-data, othersys\*  (plan.py)
  firmware   Boot#### "AurOS Installer" -> HD(seed)\EFI\AurOS\shimx64.efi;
             BootNext = it. BootOrder untouched.
-- the one restart --
FIRMWARE   Ubuntu's own Microsoft-signed shim (from the ISO) -> Canonical's
           grub (from the ISO) -> \boot\grub\grub.cfg on the seed:
           checks the ISO partition's GPT GUID and ISO 9660 UUID, else
           `exit 1` -> the firmware starts the next BootOrder entry: Windows.
LIVE       casper boots the ISO partition (live-media=, nopersistent);
           cloud-init reads the seed (label CIDATA) -> autoinstall;
  early    othersys.py early: the disk with Windows' GPT GUID, every
           partition exactly as Windows recorded it, or refuse (nothing written)
  install  curtin formats ONLY AUROS-OS-ESP (FAT32, /boot/efi) and
           AUROS-OS-ROOT (ext4, /); every other partition is listed with
           preserve + offset + size + type + GUID; installs grub+shim to
           \EFI\ubuntu on AUROS-OS-ESP and puts "ubuntu" FIRST in BootOrder
  late     othersys.py late: BootOrder back to Windows-first (Ubuntu after
           what Windows recorded), BootNext = ubuntu, "AurOS Installer"
           entry deleted, queued MOK requests cancelled, update_nvram=false,
           autologin, the helper + units installed; power off
UBUNTU, every boot until she answers (auros-othersys-boot.service)
           first boot: delete AUROS-OS-ISO (by number + GUID + name + ISO
           UUID) and grow the root into it; then the hold: Windows first,
           BootNext = ubuntu; a zenity question at login writes yes/no/later
  yes      BootOrder: ubuntu first, every other entry kept
  no       BootNext cleared; Windows starts next time (put-back: Windows side)
```

Nothing is a modified Ubuntu: the ISO is written byte for byte, the three
EFI files are copied out of it unchanged; AurOS adds an answer file and a
helper, which is the documented way to drive Ubuntu's installer, on her
own machine.

The machine's own EFI partition is never written by anything in this
flow, not even by Windows: Ubuntu gets its own (`AUROS-OS-ESP`), which is
also the partition the restart starts from. That is why "Windows'
partitions byte-identical" can include the machine's ESP.

## What Ubuntu's tools would do to her disk, and the fence for each

Found while building this; each is checked by `tools/othersys/selftest.py`.

1. **curtin deletes every partition it is not told about.** Storage
   config v2 (`curtin/commands/block_meta_v2.py`, `disk_handler_v2`):
   any existing partition whose offset is not in the preserved set is
   superblock-wiped, and the table is rewritten from the config. Fence:
   every partition on the disk is listed with `preserve: true`;
   `plan.py` refuses a layout with a partition that has no role.
2. **curtin writes a preserved partition back without its type** unless
   `partition_type` is given (`PartTableEntry.preserve` copies the GUID,
   name and attribute bits, not the type; `GPTPartTable.add` takes the
   type only from `partition_type` or `flag`). C: would come back typed
   "Linux filesystem". Fence: every partition carries `partition_type`
   and `uuid`; the check compares type, GUID, name, attribute bits,
   start and size of every Windows partition before and after.
3. **casper adds a partition to the disk it booted from.**
   `find_or_create_persistent_partition` (casper 26.04.2,
   `scripts/casper-helpers`): booted from a partition of a disk, without
   `nopersistent`, it appends an ext4 "writable" partition after the last
   partition if 100 MiB are free there, and rewrites the protective MBR's
   entries 2-4. Fence: `nopersistent` and `live-media=/dev/disk/by-partuuid/<ISO>`
   on the kernel command line (the latter also skips the scan of all disks).
4. **curtin puts "ubuntu" first in BootOrder, and autoinstall cannot
   stop it** (research §3.3: `grub-multi-install` with no arguments).
   Fence: `late` restores Windows-first and arms BootNext; the boot unit
   re-asserts it every boot until "yes" (an update to shim/grub that
   re-promotes Ubuntu is put back), and `grub2/update_nvram=false` is
   preseeded. The `nofixup` VM variant proves the check catches what the
   installer really leaves.
5. **This grub leaves `$cmdpath` empty.** The ISO's signed grub is the CD
   build (`gcdx64`): its built-in config reads `$prefix=/boot/grub` on
   the partition it started from if that exists, else searches
   `/.disk/info` (which would find the ISO and its own menu, without
   `autoinstall`). Our `grub.cfg` takes the disk from `$root`, and `probe
   --part-uuid` + `probe --fs-uuid` (both built in) pin the ISO partition.
   Its 95 built-in modules include `regexp`, `probe`, `iso9660`, `fat`,
   `ext2`, `loopback`, `serial`, `halt`; not `ntfs`.
6. **casper swaps on** any Linux swap partition it finds on `/dev/[hsv]d*`
   (`13swap`); Windows has none, so nothing to do, but a PC with an old
   Linux swap partition would have it used by the live session.
7. **MokManager's blue screen** if anything queues a MOK during the
   install. `drivers: install: false`; `late` deletes any queued
   `MokNew`/`MokAuth`/... and fails if one remains; the boot unit warns.

## The data the Windows side must write (the hooks)

The proof's fixture (`tools/othersys/mkdisk.py`) writes exactly this, so
the engine can be checked against it byte for byte.

### 1. Picker (wizard)

- One entry per `src/othersys/systems/*.json`: `name` ("Ubuntu 26.04 LTS"),
  `legal` (shown in small print), `iso_bytes` for the time estimate.
- Hide the system when preflight's `db` check (`src/aurbridge/sbdb.c`)
  says the firmware does not trust `shim_signed_by` (2011 CA): the same
  rule as AurOS itself.
- Space: C: must give `min_root_bytes` (25 GiB; offer 28-40) + `esp_bytes`
  (512 MiB) + `iso_bytes` + 1 MiB. The ISO's space returns to Ubuntu at
  its first start.

### 2. Download and verify

- `iso_url`, `sums_url`, `sums_sig_url` from the manifest.
- `src/othersys/verify.py` is the reference: gpgv-equivalent check of the
  detached signature against `keys/ubuntu-cdimage-2012.gpg` (RSA, one key),
  signer fingerprint == `signing_fingerprint`, the line for `iso_name` ==
  `iso_sha256`, then the ISO's own SHA-256. The engine needs a native
  OpenPGP verifier for one RSA signature (none in the tree yet).

### 3. Partitions (the engine)

Create in the freed gap (the same place D1 writes AurOS), any order of
numbers, 1 MiB aligned:

| GPT name | type GUID | size | content |
|---|---|---|---|
| `AUROS-OS-ESP` | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | 512 MiB | FAT32, volume label `CIDATA`, the seed files |
| `AUROS-OS-ROOT` | `0FC63DAF-8483-4772-8E79-3D69D8477DE4` | >= 25 GiB | nothing |
| `AUROS-OS-ISO` | `0FC63DAF-8483-4772-8E79-3D69D8477DE4` | ISO + 1 MiB | the ISO, raw, from offset 0 |

The ISO partition should be **directly after** the root partition, so
the first start can grow Ubuntu into it. Linux-typed partitions get no
drive letter and no file system in Windows, so a raw write to them is
allowed and nothing in Windows touches them. The partition table must be
committed **before** the restart (there is no staging step in this flow).

### 4. Records (into the seed's `othersys/plan.json`, via plan.py's inputs)

- `layout.json`: disk GUID, sector size, last usable LBA, and for EVERY
  partition: number, first LBA, sector count, type GUID, unique GUID,
  name, role (`keep` / `target-esp` / `target-root` / `iso`). Read back
  from the disk after creating the partitions
  (`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`), never assumed.
- `boot.json`: BootOrder as it was before arming (hex numbers), the
  Windows Boot Manager entry, the "AurOS Installer" entry and its
  description.
- `choices.json`: locale (`xx_YY.UTF-8`), keyboard layout/variant, time
  zone (IANA), user name (`^[a-z][a-z0-9-]{0,31}$`), real name, host
  name; the wizard's existing `choices.conf` mappings apply. The password
  is generated (three words + two digits) and **shown to her** at the end
  of the wizard; the answer file carries only its SHA-512 crypt.
- `plan.py`'s outputs are plain text (`user-data` is JSON, which is
  YAML), so a C port is substitution; it must keep every refusal in
  `check_layout`.

### 5. Firmware

`Boot####` "AurOS Installer" = `HD(seed partition)/\EFI\AurOS\shimx64.efi`,
`BootNext` = it. Nothing else. BitLocker: pause as D11 says.

### 6. Put Windows back, and "no"

- Windows-side startup task (D1's): delete `AUROS-OS-ESP`,
  `AUROS-OS-ROOT` and (if still there) `AUROS-OS-ISO` by unique GUID,
  then extend C:. The machine's ESP was never written, so nothing else
  to undo. The "ubuntu" `Boot####` is left like AurOS's own dead entry
  (inert: its partition is gone).
- "No" in Ubuntu clears BootNext, so the next start is Windows. Both
  answers are also left in the firmware for Windows to read:
  `AurOSOtherSysAnswer`, vendor GUID `b89b3dc4-fc68-4749-a5e5-f3eb5f51d569`
  (AurOS's own; pinned in `othersys.py` and `tools/othersys/checks.py`),
  attributes NV+BS+RT, data ASCII `answer=yes|no\nsystem=<manifest id>\n`.
  The Windows startup task reads it with
  `GetFirmwareEnvironmentVariableW(L"AurOSOtherSysAnswer", L"{b89b3dc4-fc68-4749-a5e5-f3eb5f51d569}", ...)`
  (SeSystemEnvironmentPrivilege, as for BootNext): `no` -> put Windows
  back as above and delete the variable; `yes` -> remove its own startup
  task and delete the variable. Writing it is never fatal in Ubuntu (the
  firmware already does what she said); the boot unit retries on every
  start until it is written. Proven: selftest (both answers, and the
  check fails when it is missing or says the other one); vmtest's
  `confirm` phase reads it back from the VM's firmware. The Windows
  reader is not built (engine).

## What is proven, and where

See the progress log in `docs/handoff/SESSION-2026-10-23.md` and the
prerelease `othersys-<run id>` (`<variant>-results.md`,
`<variant>-evidence.zip`, the desktop screenshots, the serial logs).

- `tools/othersys/selftest.py` (no VM): every check passes on a good
  fixture and fails on a broken one; every `plan.py` refusal fires; the
  `late`/hold/yes/no logic against a stand-in efivarfs; the verifier.
- `tools/othersys/vmtest.py` (KVM, OVMF Secure Boot, Microsoft's keys,
  NVMe, no network), variants `ntfs`, `bitlocker` (Windows partition is
  random bytes with `-FVE-FS-`), `nofixup` (proves the Windows-first
  check fails on what the installer really leaves).

## Next

- Xubuntu, Ubuntu Cinnamon, Ubuntu Budgie 26.04: same installer; a
  manifest each (URL, hash, `source_id`, sizes) and a matrix entry.
- The Windows-side reader of `AurOSOtherSysAnswer` and the put-back.
- Power cut between curtin's NVRAM write and `late`: Ubuntu first with a
  half-installed system. The boot unit does not exist yet at that point;
  the fix belongs in a Windows-side check of the journal on the next
  Windows start, or in the grub.cfg on the seed (re-run the install).
