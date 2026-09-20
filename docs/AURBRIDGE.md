# AurBridge — the Windows installer

> The only component of AurOS that can destroy a stranger's data.
> Everything here is written to be read by someone looking for the bug
> that loses a family's photos.

## Governing rules

1. **Refuse first.** A refused install costs one user. A failed install
   costs a user, their data, a support case, and possibly a lawsuit.
   Refusing is a designed product outcome with a plain-language remedy,
   never an error dialog.
2. **The machine can always go back.** A recovery partition exists
   *before* the first destructive byte, and is reachable from the boot
   menu without a USB stick, a second computer, or us.
3. **Nothing irreversible until AurOS has booted and the user has said
   it works.** Two-stage commit. Windows stays bootable and default
   until the user chooses otherwise.
4. **One atomic commit point per destructive phase.** All data movement
   happens with the old layout still in force; the new partition table is
   a single sector write.
5. **Read-only by default.** `aurbridge preflight` and the wizard's
   inspection phase never write. Destructive subcommands re-run preflight
   and abort on any block.

## Phases

```
 0  INSPECT      read-only. preflight. no side effects whatsoever.
 1  CONSENT      plain-language disclosure; BitLocker key proof-of-possession
 2  PREPARE      recovery USB + recovery partition. Still non-destructive
                 to Windows: only free space is consumed.
 3  SHRINK       the one destructive step on the Windows side
 4  WRITE        raw image -> new partition, read-back verified
 5  HANDOFF      bootloader to ESP, BootNext (one-shot, self-reverting)
 ── RESTART ──
 6  STAGE        AurOS staging environment: probe hardware, grow fs,
                 verify WiFi/backlight/audio BEFORE committing
 7  FIRSTBOOT    desktop; user confirms "this works"
 8  COMMIT       Ferry imports files; optionally make AurOS default
```

**Abort at any phase ≤5 leaves the machine exactly as it was.** That is
a hard requirement, tested, not a goal.

## Why the staging environment exists (phase 6)

The original brief asked for exactly one restart, doing everything from
inside live Windows. That was dropped deliberately, and this is the
reasoning, kept here so nobody re-adopts it by accident:

Between the first destructive write and a successful AurOS boot there
would be **no environment in which recovery code can run**. Fail at 60%
and the machine boots into nothing. Endless OS — better funded, shipping
real hardware — failed for 20-30% of users on BIOS systems doing
approximately this, and concluded dual-boot was for evaluation only.

The staging environment also buys the single most valuable mitigation
available for the hardware risk: it can **test WiFi, backlight, audio and
suspend on the real machine before anything is committed**. If WiFi will
not come up there, we abort and leave Windows alone — instead of
discovering it after the user has no way to reach help.

The user still clicks once and never touches firmware, a boot menu, or a
USB stick. They see one uninterrupted branded flow. They do not count
power cycles; they count lost photos.

## The recovery partition (built in phase 2, before anything destructive)

A ~600 MB FAT32 partition, marked with a discoverable type GUID, holding:

| Contents | Why |
|---|---|
| `bootx64.efi` + rescue kernel/initramfs | bootable without external media |
| `parttable.bin` — original GPT, primary + backup | exact restore |
| `esp-backup.tar` — the entire ESP as found | R12: never lose `\EFI\Microsoft\Boot` |
| `bcd-backup.bin` — Windows BCD store | restores the Windows boot path |
| `bitlocker-key.txt` — if the user consented to save it | the R1 escape hatch |
| `journal.json` — transactional install log | a resumed installer knows which step it died in |
| `machine.json` — full preflight snapshot | support can see the machine without the machine |

Its menu entry offers exactly one prominent action: **"Put Windows
back."** That restores the partition table, the ESP and the BCD, then
reboots.

The recovery partition is also written to the mandatory **recovery USB**.
The partition covers "AurOS won't boot"; the USB covers "the disk's
partition table is gone". Both are needed; neither substitutes.

## Boot handoff (phase 5)

UEFI only. Install our loader to the ESP (never reformat it — see R12,
and create our own ESP if the existing one lacks free space), then set
**`BootNext`**, not `BootOrder`.

`BootNext` is one-shot and self-reverting: firmware consumes it on the
next boot and falls back to the previous order by itself. If AurOS fails
to start, the machine comes back up in Windows with no user action. That
property is why BIOS/MBR is refused — it has no equivalent, and without
it a failed first boot is a brick.

`BootOrder` is only rewritten in phase 8, after the user confirms.

## BitLocker handling (R1 — the highest-consequence path)

```
detect  → -FVE-FS- boot-sector signature (native, no COM)
          then Win32_EncryptableVolume for full status
refuse  → ConversionStatus 2/3/4/5 (encryption mid-flight)
refuse  → no Numerical Password protector retrievable
require → user types part of the 48-digit key back. Proof of
          possession, not a checkbox.
offer   → save/print the key; write it to the recovery partition
suspend → Suspend-BitLocker -MountPoint C: -RebootCount 0
```

**`-RebootCount 0` is load-bearing.** The default is 1, meaning "restore
protection after one restart". Our restart goes into AurOS, so the
suspension would expire without Windows ever re-sealing the VMK against
the new PCR values. The next Windows boot then demands the recovery key.
`0` suspends indefinitely (a clear-key protector is written into the
volume metadata) until an explicit `Resume-BitLocker`.

Third-party full-disk encryption (VeraCrypt system encryption, Sophos,
Trellix, Symantec) → **abort unconditionally**. There is no safe shrink
underneath a sector-level encryption filter we do not control.

## Shrink (phase 3)

Performed **offline from the staging environment** with `ntfsresize`,
not online from Windows. Online shrink is capped by unmovable files
(pagefile, hiberfil, VSS storage, `$MFT`, `$Bitmap`, `$UsnJrnl`) and
commonly offers 2 GB on a disk with 30 GB free.

Preparation, all disclosed and revertible: disable hibernation, disable
the pagefile, delete shadow copies, consolidate free space.

**We do not write our own NTFS resizer.** That is how a few unreadable
files become an unmountable volume.

⚠ **512e vs 4Kn**: shrink takes *sectors*, partition structures take
*bytes*, NTFS allocates in *clusters*. Hard-coding 512 on a 4Kn disk
makes the partition 8× too small. Always read
`StorageAccessAlignmentProperty`.

## Write (phase 4)

A prebuilt filesystem image is written byte-for-byte to the new
partition, then **read back and verified** against its hash. Grown to
fill the partition on first boot with `resize2fs`, where a real kernel is
available.

Chosen over implementing an ext4 writer for Windows: a write loop is
boring and auditable; an ext4 writer is a corruption-bug factory.

Per the signing research, **no kernel-mode driver is required** — Rufus
performs partition-table rewrites, volume lock/dismount and raw sector
writes from an elevated user-mode process. That keeps attestation
signing off the critical path.

## Exit codes

`aurbridge preflight` returns `0` for go, `1` for blocked. The wizard and
CI both depend on this.

## Signing

Ship **OV, not EV**: since 2024 EV buys nothing over OV for SmartScreen.
Keys on a hardware token or cloud HSM (June 2023 requirement).

**Signing is not optional.** Smart App Control blocks unsigned code by
default and auto-enables for exactly our target profile — a
non-technical user on a clean-installed Windows 11 with a Microsoft
account. An unsigned AurBridge is not merely scary for that user; it is
unrunnable.

## Still to build

Phases 1-8. Preflight (phase 0) is implemented and builds as a native
`.exe`; everything downstream of it is specified here and not yet
written. Nothing destructive ships until the recovery partition and the
"Put Windows back" path are implemented and tested by deliberately
failing an install at each phase.
