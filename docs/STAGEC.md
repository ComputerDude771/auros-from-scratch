# Stage C and D — the destructive half, and the way back

> This document exists because five designs were written for this work
> and five adversarial reviews took them apart. What follows is what
> survived. The findings that killed a design are recorded next to the
> thing they killed, because the whole value of having found them is
> that nobody restores the version that was wrong.

`docs/AURBRIDGE.md` is still the contract. This is how it gets built.

---

## The foundation: a layout, and a partition table

Everything in stage C rests on two pieces that did not exist, and the
most serious mistake in the first round of designs was in the first of
them.

### The free space is a GAP, not the end of the disk

The first design computed the new partitions from the end of the disk:
put the recovery partition at `usable_end - 600 MiB`, and run the AurOS
root from the shrunk Windows volume up to it.

**On the layout `docs/AURBRIDGE.md` itself calls typical — ESP, MSR,
C:, OEM recovery — that is wrong on every machine.** The OEM recovery
partition sits at the *end* of the disk. Shrinking C: creates a gap
*between C: and WinRE*; it creates nothing at the end. Both computed
extents land on top of WinRE, the overlap guard fires, and the product
refuses its entire target population while believing it is being
careful.

So the layout is computed inside the gap:

```
  gap_start = win_start + win_new_sectors        (where C: now ends)
  gap_end   = the start of the next populated partition entry at or
              after win_start + win_old_sectors, or last_usable + 1
              when there is none
```

and everything is placed between those two numbers.

`gap_end` comes **from the GPT entry array, never from `/sys`.** The
survey's partition list is `readdir()` order, capped at
`STAGE_MAX_PART`, and contains only partitions the kernel chose to
instantiate — a partition the block layer skipped is invisible to it,
and laying the AurOS root across one is not a mistake anything later
can catch.

### `gpt.{h,c}` — the table itself

Not a bash-out-an-afternoon file, which is what the first design left
it as: it named no file, no function, no CRC32, and no PARTUUID
generation, for the single most dangerous write in the product.

  - `HeaderSize` is read **from the existing header**, never assumed to
    be 92. An OEM that wrote 96 gets a primary header whose CRC covers
    four bytes too few; firmware that validates strictly falls back to
    the backup and the machine boots a layout we did not intend.
  - CRC32 (the ordinary IEEE one, the same polynomial zlib uses) is
    ours, in this file. `sha256.c` is the only hash in the tree today.
  - New PARTUUIDs come from `/dev/urandom`.
  - Refuses when there is no free entry slot, rather than growing the
    array.

### `plan.{h,c}` — where things go

`plan_check()` takes the **`gpt_table`**, not a cached scalar. Its
enumerated bound is "no proposed extent overlaps any populated entry
other than the Windows entry being shrunk", and it is checked at every
point where the plan is used, not once at the start.

`min_partition_bytes` is the **product floor** (24 GB by default, and
a kiosk profile may honestly declare a smaller one), not the image
size. The first design set it to the size at which the image merely
fits — about 4.7 GiB — which silently deletes the documented floor and
hands a user an AurOS with no room in it, having shrunk their Windows
to get there.

### `geom_may_touch()` — the MUST NOT, at every site

The MUST NOT box requires the `-FVE-FS-` check "in code, at every site
that touches partition geometry". Three new sites touch it —
`plan_compute`, arming a write target, and the commit — and leaning on
a tri-state some other subsystem computed earlier in the run is not the
same thing. One helper, called from all three, that `pread`s the
partition's first sector read-only and refuses.

---

## What gets written, and where it comes from

`build/mkimage` emits a whole-disk GPT image. Writing *that* into a
partition embeds a nested GPT and boots nothing — the doc has flagged
this for some time.

**The staging environment translates it.** The image's own GPT names
`AUROS-ROOT` with the Linux-root type GUID; that extent is a plain
ext4, and it is the only thing that goes into the new root partition.
Verified: on the current desktop image it is sectors 1050624..10289118,
and its superblock magic reads 0xEF53 with the label AUROS-ROOT.

Two rules the reviews forced:

  - **`mkfs.ext4` must come from the profile's own rootfs**, run
    through its loader with `--library-path`, exactly as `build/staging`
    already does for `depmod`. A build host with newer e2fsprogs writes
    a feature (`orphan_file`, for one) that the rootfs's `e2fsck` and
    `resize2fs` do not know — and the failure lands *after* the commit.
    This is the third time this project has met this bug.
  - **`resize2fs` will refuse on every image `mkimage` produces**, with
    "Please run 'e2fsck -f' first", because `mkimage` mounts the
    filesystem to fill it and `s_mtime` then exceeds `s_lastcheck`. So
    `e2fsck -fp` runs first, with a fixed argument vector, the same
    discipline as `ntfsresize`. Without it the root never grows and
    every install silently hands the user a 7 GB filesystem inside the
    200 GB partition they chose.

### Finding the stick

**Not by `/sys`'s `removable` flag.** That is the SCSI RMB bit: thumb
drives usually set it, but USB SSDs and anything in a USB-to-SATA
enclosure do not — and a 4.9 GiB payload is exactly what a user puts on
the big fast drive they already own. The refusal it produced told them
to plug in a stick that was already plugged in.

The stick is identified by **what is on it**: a partition whose type
GUID is ours and whose manifest matches the journal's profile and image
hash.

---

## The ESP is the fragile thing on the disk

A FAT filesystem has no journal, and the machine's ESP holds
`\EFI\Microsoft\Boot`. Temp-name-and-rename makes each *file* atomic
and does nothing for FAT *metadata*: one torn 512-byte FAT sector holds
128 cluster entries, and they may as easily belong to `bootmgfw.efi` as
to us.

So, in this order:

1. Before the first read-write mount of the machine's ESP, read the
   whole ESP partition **raw** and keep a byte-exact copy.
2. Create the recovery partition **before** the merge, so there is
   somewhere durable to put that copy.
3. Merge with `-o sync`, and read every written file back afterwards
   from a fresh read-only mount.

The first design merged into the ESP *after* the GPT commit and
described a power cut there as leaving "at most one temp file". That is
the ordering that leaves a machine which will not boot Windows.

---

## The journal, and resuming

Three fatal mistakes, all in the same small piece:

  - **Read the last record before writing this boot's first one.** The
    first design wrote `REC_C_BEGIN` and *then* asked for the highest
    surviving record — which was the one it had just written — so the
    resume ladder always took its first branch and re-ran `ntfsresize`
    on a volume that might be mid-resize.
  - **A record must carry a run identity.** Without one, a refusal from
    last Tuesday is indistinguishable from this run's progress.
  - **FIBMAP is relative to the filesystem, not the disk.** The
    absolute offset is `partition_start * 512 + block * blocksize`, and
    the window must be independently checked to lie inside the ESP
    partition. This is the one site where an off-by-a-partition-start
    writes over the primary GPT.

And: **never touch FAT metadata after the gate.** The log and the
journal live in fixed-size files preallocated by AurBridge, in one
contiguous run each (AurBridge can verify that from Windows with
`FSCTL_GET_RETRIEVAL_POINTERS` and refuse while the user still has
Windows and a remedy), written through the raw partition fd.

---

## Refusals must say what will actually happen

Every pre-shrink refusal used to end "Nothing has been changed", which
is true and useless. `BootNext` was consumed by the boot that is
running, so switching the machine on again boots **Windows**, not the
installer. A user who follows the old sentence exactly concludes the
install silently failed.

> Plug the AurOS memory stick back into this computer, then switch it
> on. Windows will start as usual — nothing on this computer has been
> changed. Open AurBridge and press Start installing again.

And when the staging environment booted **from the USB**, it must not
reboot at all: it must say "take the stick out, then press Enter",
because the firmware will otherwise boot the stick again and loop.

---

## Stage D — the way back

  - **Never restore a fixed sector count.** `parttable.bin` as "LBA
    0..2047, restored verbatim" writes capture-time bytes over the
    start of the first partition on any disk whose first partition
    begins below LBA 2048 — which is always true on 4Kn, and true of
    plenty of 512e OEM layouts. Capture and restore exactly what the
    captured header's own fields describe.
  - **Never grow with `--force` without reading the volume's state
    first.** `--force` suppresses the refusal on untrustworthy
    metadata, which is the only thing standing between the restore and
    R9 — and the restore's own "run chkdsk and try again" outcome is
    reachable only when `ntfsresize` refuses, which `--force` prevents.
    Dirty, unclean log, hibernated, or any UNSURE: do not run it.
  - **The commit is one sector.** Rule 4 says so. Staging a 2048-LBA
    write and calling it atomic is not it.

    **And the order everybody agreed on does not achieve it.** The doc
    said "backup header, then primary, one flush"; the review said
    "write the backup array and header, flush; write the primary array,
    flush; then write LBA 1 alone — that single sector is the commit".
    `tools/committest.sh` snapshotted the disk after each flush and
    asked an independent tool what it saw. Backup-first fails: writing
    the primary **entry array** invalidates the primary header, because
    the header carries a CRC of the array, so every reader falls back to
    the backup — which backup-first has already replaced with the new
    layout. The machine's partitions change at the array write, not at
    LBA 1.

    So the primary array goes first and the backup goes **last**. During
    the window where the primary is invalid, readers fall back to a
    backup that still describes the old layout, and the disk goes on
    reading exactly as it did. Then LBA 1 lands — one sector, atomic on
    every real device — and that is the commit. The cost is a brief
    stale backup afterwards, which no reader prefers over a valid
    primary.

    The test asserts the interrupted disk reports *Main partition table:
    ERROR, Backup: OK* — the half-written primary rejected rather than
    believed — which is what correct looks like here, not "no problems
    found".
  - **The recovery partition must hold this machine's payload.** A
    prebuilt static image cannot: `parttable.bin`, `esp-backup.tar`,
    `bcd-backup.bin`, `ntfs-boot.bin` and the original sector count are
    all machine-specific. AurBridge builds the per-machine container on
    the USB in phase 2, where it is running on Windows with a
    filesystem; stage C copies it into the recovery partition and
    hash-checks it against the USB copy, which is what the doc says.
  - **Identify the ESP by geometry, not by mounting it.** Mounting is
    unavailable in exactly the case tier 2 exists for, and a read-write
    mount is itself a write.
  - **Never delete boot entries by exclusion.** "Entries `efi.tab` did
    not record" also selects the Fedora the user installed last month
    and the vendor diagnostics entry that was never in `BootOrder`.
    Identify our own entries positively.

---

## What is honestly not provable here

Real hardware. The R4 matrix — deliberately fail at each step and pull
the power — can be driven in QEMU against a synthetic machine, and that
is worth doing and is what the harness does. It is not the same as a
power cut on a 2013 Toshiba with a volatile write cache, and this
document does not pretend otherwise.
