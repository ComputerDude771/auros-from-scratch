# AurBridge — the Windows installer

> The only component of AurOS that can destroy a stranger's data.
> Everything here is written to be read by someone looking for the bug
> that loses a family's photos.

## Governing rules

1. **Refuse first.** A refused install costs one user. A failed install
   costs a user, their data, a support case, and possibly a lawsuit.
   Refusing is a designed product outcome with a plain-language remedy,
   never an error dialog.
2. **The machine can always go back.** A complete, verified recovery
   payload exists on removable media *and* on the Windows volume before
   the first destructive byte; a bootable recovery partition exists
   before the partition table is committed, reachable from the boot menu
   without a USB stick, a second computer, or us. (This rule used to say
   the partition came first. It cannot — see "The recovery payload".)
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

**Nothing destructive happens on the Windows side. Not one byte.**

```
 WINDOWS — every step here is reversible by doing nothing
 0  INSPECT      read-only. preflight. no side effects whatsoever.
 1  CONSENT      plain-language disclosure; recorded
 2  PREPARE      recovery payload -> the recovery USB and a file on the
                 Windows volume. Staging environment -> the ESP, or the
                 USB if the ESP has no room.
 3  HANDOFF      BootNext (one-shot, self-reverting). Nothing else.
 ── THE RESTART — exactly one, and this is it ──

 STAGING — the AurOS initramfs, same boot, no second restart
 4  SHRINK       ntfsresize, FILESYSTEM ONLY. The partition entry is
                 not touched, so Windows still boots after this step.
 5  WRITE        image into the freed region BY OFFSET, old partition
                 table still in force
 6  VERIFY       read back and hash. Abort here changes nothing.
 7  PROBE        mount the new root read-only, load ITS drivers and
                 firmware, test WiFi/backlight/audio on the real
                 machine. Abort here changes nothing.
 8a BOOT         the image's OWN EFI partition, copied whole into a
                 partition of ours, still before the commit
 8  COMMIT       the new GPT: the primary entry array, then LBA 1, then
                 the backup. Then the boot entry, partx, resize2fs, the
                 saved copy of Windows, switch_root.

 AUROS
 9  FIRSTBOOT    the desktop asks, once per session until answered.
                 Every boot re-arms BootNext so the machine keeps
                 coming back while AurOS works.
10  IMPORT       "yes" rewrites BootOrder -- the one place that does --
                 and only then may Ferry read the old Windows volume.
```

**Abort at any phase ≤7 leaves the machine bootable into Windows.** That
is a hard requirement, tested, not a goal. The entire product has
exactly **one** non-restartable window — phase 4 — and everything after
it writes only into space that is already free.

### Why the destructive work is after the restart, not before

This is the decision the rest of the document hangs on, so the actual
reason is worth stating plainly, because two weaker ones were believed
here first.

**The real reason is the pagefile.** Windows' online shrink cannot move
`pagefile.sys`, and disabling the pagefile does not remove the file
until after a restart. So online shrink *costs* a restart — the one
thing this design is spending its whole budget to avoid. Offline
`ntfsresize` treats the pagefile as an ordinary file and relocates it.
The one-restart promise requires offline shrink; it does not merely
tolerate it.

**The second reason is what happens when the restart fails.** Firmware
that ignores `BootNext` is not rare on old Lenovo and HP machines with a
full NVRAM store. Under this ordering that is a non-event: nothing has
changed, Windows comes back, and AurBridge re-arms. Under the ordering
this document used to describe — shrink and repartition inside live
Windows, *then* restart — the same firmware quirk means Windows boots
onto a disk that has already been repartitioned underneath it.

**The reason that was in this document is wrong**, and is recorded here
so nobody restores it: it argued from Endless OS failing for 20-30% of
users. Endless's Windows installer created a file inside the existing
NTFS filesystem (`C:\endless\endless.img`) — **it never shrank NTFS and
never repartitioned**. The failure rate being cited is for *MBR
bootloader replacement on BIOS firmware*, which is a different
operation, and using it to justify a decision about NTFS shrink is a
category error. It does support the BIOS/MBR refusal, which is why that
refusal stands.

The transferable Endless evidence points the other way, in favour of
this design: `eos-boot-helper` rewrites the partition table **of the
disk it is booting from, from inside the initramfs, in the same boot**,
and has done in production for years. That is exactly phase 8.

## The staging environment is the initramfs

It is not a second operating system and it is not a second reboot. The
kernel and initramfs AurOS already needs in order to boot *are* the
staging environment: they come up after the one restart, do the
destructive work with a full Linux toolset and real block-device access,
and then `switch_root` into the installed system **in the same boot**.

That buys the single most valuable mitigation available for the hardware
risk: it can **test WiFi, backlight, audio and suspend on the real
machine before anything is committed**. If WiFi will not come up, we
abort and leave Windows alone — instead of discovering it after the user
has no way to reach help.

**The firmware does not go in the initramfs.** `linux-firmware` is
700 MB to 1.5 GB; even a WiFi-only subset is 150-400 MB, and an OEM ESP
is commonly 100 MB and 85-95% full. So the probe happens in phase 7,
*after* the image has been written and verified — mount the new root
read-only, load **its** modules and **its** `/lib/firmware`, and test
against that. The initramfs stays under ~80 MB, and the probe still
happens before anything is committed. This is strictly better than
carrying firmware, not a compromise.

Where the ESP has no room even for that, the staging environment boots
from the **recovery USB**, which R4 makes mandatory anyway. `BootNext`
points at the USB's boot entry: no boot menu, no F12, no user
interaction, the same single restart. The USB stops being pure
conversion cost and gets a second job.

The user clicks once and never touches firmware, a boot menu, or a USB
stick. They see one uninterrupted branded flow. They do not count power
cycles; they count lost photos.

## The recovery payload, and the rule that had to change

The governing rule used to read: *a recovery partition exists before the
first destructive byte.* **That is not achievable, and the reason is
worth understanding before anything is built on it.**

On a typical OEM layout — ESP, MSR, C:, OEM recovery — **there is no
unallocated space on the disk.** A 600 MB partition cannot be created
before the shrink, because the shrink is what creates the space. Free
space *inside* the C: filesystem is a different quantity entirely, and
measuring that one instead is how this mistake survives review.

**Rule 2 now reads:** a complete, verified copy of this machine's
Windows start-up exists on removable media before the first destructive
byte; a second copy exists on the machine itself before the install is
finished.

### Who makes it, and where it lives — both changed during stage D

The design said AurBridge would build the payload on Windows, in phase
2, "where it is running on Windows with a filesystem", and that stage C
would copy it into the recovery partition. Building it produced two
corrections:

**Stage C makes the capture, not AurBridge.** The only reason to do it
on the Windows side was that Windows has a filesystem to write files
into — and the stick does not need one. Everything on it is a raw
partition found by type GUID, so the capture can be made from the
staging environment, before phase 4, by the same code that will later
put it back. One implementation instead of two, in one language, and no
class of "Windows wrote it, Linux must parse it" bugs at all. AurBridge
still *makes room* for it in phase 2: the size is dominated by this
machine's EFI partition, which is 100 MB on one laptop and a gigabyte on
the next, so the stick's layout is computed from the machine.

**It is not a file on a FAT partition.** It gets its own raw extent —
`AUROS-SAVED`, with its own type GUID — on the stick and again on the
disk. The recovery partition beside it is an EFI System partition,
because firmware has to be able to launch it; putting somebody's
captured Windows in a *file* on that FAT filesystem would mean mounting
FAT read-write, from an initramfs, on the one path whose entire purpose
is surviving a machine that has already gone wrong. A raw extent has no
metadata to corrupt and is one contiguous run by construction — the same
three reasons `record.h` gives for the same decision.

**And it is not a file on C: either.** The old design put a second copy
in `C:\AurOS\recovery\` on the grounds that `ntfsresize` relocates
files rather than destroying them. True, and irrelevant: the copy exists
to survive the case where C: is what went wrong.

### The payload

One blob, `AURRSC01`, whose 4096-byte header is specified byte by byte
in `src/aurstage/rescue.h` so that a support tool five years from now
can read a stick without reading our source. Every section carries its
own SHA-256 and the payload carries one over all of them; the header is
written **last**, after everything it describes is on the stick and
flushed, so a capture interrupted by a power cut has no magic at its
start and is refused rather than half-believed.

| Section | Why |
|---|---|
| the protective MBR, one sector | never restored unless it differs; captured because a machine that boots nothing at all is usually missing exactly this |
| the primary GPT header, LBA 1 | the sector the restore commits with |
| the primary entry array, **at the header's own `PartitionEntryLBA`, for `NumberOfPartitionEntries × SizeOfPartitionEntry` bytes** | not 32 sectors, not 2048 — see below |
| the backup header, at the header's own `AlternateLBA` | |
| the backup entry array, **at the BACKUP header's own `PartitionEntryLBA`** | which is not derivable from the primary's and is not always `AlternateLBA − 32` |
| the entire ESP, raw, by offset | R12: OEM EFI partitions hold vendor boot files and firmware capsules as well as `\EFI\Microsoft\Boot`. Never mounted — a read-write mount is a write, and mounting is unavailable in precisely the case this exists for |
| per NTFS volume: `$Boot` | without it a restored GPT describes a filesystem that is no longer there |
| per NTFS volume: the backup boot sector, and the sector count the filesystem claimed | the backup boot sector *moves* when a volume is resized, and nothing else on the disk records how big C: used to be |

A BitLocker volume is captured deliberately as *nothing*: its geometry
is never changed by this product, so there is nothing about it to put
back — and copying 8 KiB of somebody's ciphertext onto a memory stick is
a thing to not do.

### Putting it back

`aurstage.restore` on the kernel command line, in the same staging
environment the install ran in. **"Put Windows back" in AurOS does not
restore anything itself** — it arms a flag and restarts. A restore
rewrites the partition table of the disk it is running from, and doing
that from inside AurOS means rewriting the table under a mounted root
and growing a volume inside a partition whose entry has just changed
under a kernel that has not re-read it.

Order, each step idempotent so the whole thing can simply be run again:

1. **the partition table**, committed in one sector — primary array,
   then LBA 1, then the backup, exactly as the install commits, for the
   reason `commit.h` sets out at length;
2. **the EFI partition**, whole;
3. **each NTFS volume grown back** to the size it claimed.

The grow is last because it is the only step that is both long and
documented-restartable, and because a machine interrupted after step 2
boots Windows with a smaller C: — a machine somebody can use, back up
from, and finish the restore on. There is an unavoidable window where
the machine boots nothing: the restore's job is to delete the partition
AurOS is on, and no ordering makes both systems bootable throughout.
That window is what the stick is for.

### Two things a shell prototype got wrong, kept here so they stay wrong

**Never restore a fixed sector count.** `src/recovery/mkrecovery`
restored 2048 sectors from LBA 0. The first partition on a 4Kn disk
starts at LBA 256 and on plenty of 512e OEM layouts at 34, 40 or 63, so
that writes a megabyte — eight megabytes on 4Kn — of capture-time bytes
over live filesystem data. Capture and restore exactly what the captured
header's own fields describe, and nothing else.

**Never grow with `--force` without reading the volume's state first,**
and then only for one named condition. `--force` suppresses the refusal
on untrustworthy metadata, which is the only thing standing between the
restore and R9. But `ntfsresize` marks a volume dirty after every
successful resize *and then refuses to touch a dirty volume without
`-f`* — so the volume our own installer shrank is, by construction, one
that cannot be grown back unless somebody decides the dirty bit is ours.
`rescue.c` decides it, narrowly: the only thing wrong is the dirty bit,
the log is clean, nothing is hibernated, none of the three is UNSURE,
and the volume's NTFS serial is the one the capture recorded. Everything
else is still a refusal with "run chkdsk from Windows and try again".

## Boot handoff (phase 3)

UEFI only. Write the staging kernel and initramfs to the ESP — never
reformat it, see R12 — then set **`BootNext`**, not `BootOrder`.

`BootNext` is one-shot and self-reverting: firmware consumes it on the
next boot and falls back to the previous order by itself. If the staging
environment fails to start, the machine comes back up in Windows with no
user action and nothing has been changed. That property is why BIOS/MBR
is refused — it has no equivalent, and without it a failed first boot is
a brick.

Two things must happen right at the end, in this order:

1. **Re-run preflight.** The user has been reading for several minutes;
   Windows Update can arrive in that window, and servicing-on-shutdown
   will consume the restart and rewrite parts of the ESP.
2. **Disarm `BootNext` if the restart does not actually happen.** A user
   can cancel a restart and an application can block one. An armed
   `BootNext` that is consumed three days later, after the disk has
   changed, is a trap. The staging environment therefore also re-verifies
   the machine against `journal.json` — disk serial, GPT hash, NTFS
   start LBA and sector count — and aborts on any mismatch.

`journal.json` also carries the **profile**. `image_find()` walks every
disk in the machine and every `AUROS-IMAGE` partition on each, so a
second AurOS stick left plugged in — one made last spring for a
different profile — is a second candidate; without a name to ask for,
the staging environment installs whichever it reaches first. It has been
able to insist on a profile since it was written, and for a while its
one caller passed `NULL`, so it never did.

Two things this is **not**, both of which an earlier draft of this
paragraph claimed. A stick AurBridge makes holds exactly one image:
phase 2 rewrites the whole GPT with three partitions, so re-making a
stick replaces the image rather than adding a second. And the wizard has
no profile picker yet — it writes the constant `desktop` — so on today's
shipped build this compares a constant with itself. The wire is here and
the refusal is real the moment there is more than one image within
reach; the picker is on the list, not in the tree.

`BootOrder` is only rewritten in phase 10, after the user confirms.

## Making the machine able to start AurOS (phase 8a)

The phases above shrink Windows, write AurOS, verify it, commit a new
partition table and hand over to the installed system in the same boot.
For a long time that was the whole of it, and it was not enough: nothing
wrote a bootloader, so the machine lost AurOS at the next restart. Every
test passed on a computer that could not start what had just been
installed on it.

**AurOS is started from an EFI System partition of its own, never from
the machine's.** R12 says an OEM ESP holds vendor boot files and
firmware capsules beside `\EFI\Microsoft\Boot` and is never mounted by
this product; a read-write mount of it is a write to the one partition
whose loss means Windows never starts again. Writing FAT structures into
it by hand instead means understanding somebody else's filesystem well
enough to extend it, which is a bigger thing to be wrong about than
anything else in the installer. UEFI launches whatever a `Boot####`
entry names and does not care which EFI System partition that is, so the
planner's `rec` extent — which existed already, typed `ef00`, and was
written by nothing — became `AUROS-BOOT`, and the machine's own ESP is
still a partition this installer has only ever read. The end-to-end test
asserts exactly that: an md5 of the machine's ESP before and after.

**What goes in it is the image's own ESP, copied whole.** Not assembled.
`build/mkimage` already builds a complete Secure Boot chain inside the
image — Canonical's dual-signed shim, their signed grub, the
`BOOTX64.CSV` that lets shim's fallback create a real NVRAM entry, and
`grub.cfg` in the three directories that need it — and that is the ESP
QEMU boots in the build. Copying its bytes means the artifact that was
tested and the artifact on the user's machine are the same object, which
is `image.h`'s own argument for shipping a whole-disk image rather than a
bare root filesystem. It also means there is no FAT-writing code
anywhere in the installer: no directory entries, no cluster allocator,
no long-name encoder, and nowhere for any of them to be subtly wrong on
a machine nobody can reach.

The copy goes down **before** the commit, into the gap, while the old
table is still in force — so rule 4 still holds, the layout still
changes at one sector write, and a machine cut here is the "write and
verify" row of the table above. The first megabyte is written last, for
the reason the root extent's is: until the end there is no BPB at the
start of the partition, so an interrupted copy is a partition the
firmware will not mount at all rather than one it mounts and reads half
a shim out of.

**The boot partition is as big as what goes in it**, measured from the
image, not a constant. It was 600 MiB, chosen when that partition was
going to hold a rescue kernel and a copy of this machine's Windows
startup as well; both of those moved elsewhere. A constant that
disagrees with the thing being copied is either gigabytes of somebody's
Windows taken for nothing or an install that fails after the shrink.

### The boot entry, and the second gate

`src/aurstage/nvram.c` is the only file in the staging environment that
may write an EFI variable, and `build/staging` enforces that the way it
enforces `wr.c`'s monopoly on disk writes — by name, with checks, not by
widening a pattern. Writing a `Boot####` needs `O_WRONLY|O_CREAT` on a
file under efivarfs, and the three honest options were to loosen the
existing gate, to fold NVRAM into `wr.c`, or to add a second gate. The
first is the exception nobody reviewed; the second makes `wr.c`'s single
sentence — every disk write is in this file — false.

So `nvram.c` is allowed the writable open every other file is refused,
and pays four checks for it: it must open nothing read-write, it must
never name a device, it must still write something, and it must name the
efivarfs mount point. **NVRAM deserves a gate more than the disk does.**
A wrong byte on a disk costs a partition; a wrong `Boot####` costs a
machine that starts nothing at all, with no error message, on hardware
whose firmware setup screen the owner has never seen.

`BootOrder` is not written, and there is deliberately no function in
that file that writes it — the way a rule like this gets broken is
somebody finding a function that already does the thing. Windows stays
the machine's default until phase 10. What phase 8 arms is `BootNext`,
one-shot and cleared by the firmware as it is used, so the first restart
after an install reaches AurOS and a machine that cannot start AurOS
comes back to Windows by itself with nobody doing anything.

The entry the Windows half wrote to get here — "AurOS Installer" — is
deleted once "AurOS" exists. Leaving it means a boot menu with two AurOS
lines in it, one of which restarts an installer that will correctly
refuse, to somebody who did not ask for an installer.

### What proves it

`tools/loadertest.sh` installs, and then starts the machine **the way
its owner would**: firmware, one disk, no memory stick, and no `-kernel`
on the QEMU command line. Every other end-to-end test in the tree hands
QEMU a kernel directly, which is right when what is being tested is the
staging environment and is also why none of them had ever asked this
question. It then does it again against `OVMF_CODE_4M.ms.fd` with
Microsoft's own keys enrolled and Secure Boot enforcing, which is the
only way to find out whether the signed chain in the image is a chain
this firmware actually trusts.

## BitLocker: refused, and why it stays refused

**There is no shrink path for a BitLocker-protected volume, online or
offline.** Offline, `cryptsetup`'s BITLK support has no resize operation
and never modifies the on-device header, while `ntfsresize` sees
`-FVE-FS-` rather than NTFS and refuses. Online, shrink is unavailable
on an encrypted volume and the guidance is to turn encryption *off* —
a full decryption, hours of whole-disk rewriting on exactly the aged
drive R5 is about. Suspension does not help: `Suspend-BitLocker` writes
a clear-key protector and does not decrypt a single sector.

So preflight blocks, unconditionally, and the remedy says what is
actually required — fully decrypt — rather than implying that producing
the recovery key clears it. It does not, and a refusal the user cannot
clear by following its own instructions is a user who re-runs forever.

This matters more over time, not less: Windows 11 24H2 enables device
encryption automatically on clean installs, with the hardware
requirements relaxed.

> ### MUST NOT
>
> **Never move, truncate or resize a partition entry whose first sector
> carries `-FVE-FS-` at offset 3.** Shrinking the partition without
> shrinking the volume destroys the trailing FVE metadata copy and the
> ciphertext behind it: instant, total, unrecoverable loss of an
> encrypted volume. It is the most destructive single mistake available
> anywhere in this codebase and it is a two-line mistake to make. The
> check belongs in code, at every site that touches partition geometry,
> not in this document.

Third-party full-disk encryption (VeraCrypt system encryption, Sophos,
Trellix, Symantec) → **abort unconditionally**. There is no safe shrink
underneath a sector-level encryption filter we do not control.

## Shrink (phase 4) — the one irreversible step

Performed offline, from the staging environment, with `ntfsresize`.

**`--force` must be unreachable by construction**, not merely unpassed.
It authorises resizing a filesystem whose own metadata Windows has
declared untrustworthy, which is R9 verbatim, and it is one word away at
all times. Never `ntfsfix --clear-dirty` either: R2 records why — it
discards the user's unsaved session.

On a dirty or hibernated volume, `ntfsresize` refuses. **So do we.** The
remedy is the chkdsk one preflight already writes: a user-visible
restart outside our flow, before the install starts. That is acceptable.
A second restart *inside* the flow is not.

The authoritative check is not "did Windows say it shut down cleanly".
It is the on-disk NTFS state read from Linux, immediately before
touching anything: volume flags, `$LogFile` restart-area state, and the
presence and size of `hiberfil.sys`.

Order matters, and this order is the reason phase 4 is the only
non-restartable window in the product:

1. `ntfsresize --no-action` for the true achievable size. Not an
   estimate from filesystem free space — R7 requires the real number.
2. **Surface-test** the region being reclaimed *and* the region NTFS
   will relocate into. Read every sector; refuse on any error. R5, and
   it is cheap.
3. Resize **the filesystem only**. Do not touch the partition entry. A
   partition larger than its filesystem mounts and boots normally, so
   **if we stop here, Windows still boots.**

`ntfsresize` documents restart-safety for *expansion* only. Treat an
interrupted shrink as a damaged volume, which is why the AC-power gate
and the surface test guard this step specifically.

⚠ **512e vs 4Kn**: shrink takes *sectors*, partition structures take
*bytes*, NTFS allocates in *clusters*. Hard-coding 512 on a 4Kn disk
makes the partition 8× too small. Always read
`StorageAccessAlignmentProperty`, and block if it cannot be read.

**We do not write our own NTFS resizer.** That is how a few unreadable
files become an unmountable volume.

## Write and commit (phases 5-8)

The image is written **by offset, with the old partition table still in
force**, then read back and verified against its hash. Only after that
does the new GPT get written: backup header first, primary last, one
flush.

Borrowed directly from `eos-installer`, and worth keeping: **write the
first megabyte last.** Zero it, write everything else, verify, and only
then lay down the first megabyte. A partially written install is then
never a bootable-looking install and never claims to be a partition
table.

Then `partx --update`, `resize2fs`, create the recovery partition from
the freed space, and `switch_root`.

⚠ **The image the build produces does not currently match this.**
`build/mkimage` emits a whole-disk GPT image with its own protective
MBR, ESP and root partition. Writing *that* into a partition embeds a
nested GPT and boots nothing. Either the build publishes a bare root
filesystem image and the ESP as a file tree, or the staging environment
translates the whole-disk image. The staging environment makes the
second viable; nothing on the Windows side did, which is how the
mismatch survived.

Writing a filesystem image was chosen over implementing an ext4 writer
for Windows: a write loop is boring and auditable; an ext4 writer is a
corruption-bug factory. Per the signing research, **no kernel-mode
driver is required** — Rufus performs partition-table rewrites, volume
lock/dismount and raw sector writes from an elevated user-mode process,
which keeps attestation signing off the critical path.

## Phases 9 and 10: the machine stops being on loan

The installer finishes and hands over to AurOS in the same boot. At
that moment the machine has AurOS on it, has a signed boot chain in a
partition of its own, has a `Boot####` entry pointing at it, and has
`BootNext` armed — and switching it on still reaches Windows. That is
rule 3, and it held for a long time with nothing anywhere that let the
person change it.

### The hold

`BootNext` is consumed by the firmware as it is used. The installer
arms it and then hands over in the same boot, so it is still armed when
the machine is next switched off — exactly once. Use AurOS, shut down,
start (BootNext fires), shut down, start again: the second start
reaches Windows, with AurOS installed, working, and unreachable without
the firmware's boot menu.

So `aurfirst hold` re-arms it on **every** boot until the question is
answered, from a systemd unit that runs before the desktop. The machine
keeps coming back to AurOS while AurOS works, and the moment it does
not the firmware falls through to `BootOrder`, which still says
Windows, and nobody has to do anything. One NVRAM write per boot is a
fair price for that.

### The question

`src/aurshell/welcome.c` is the only thing in the product that asks
whether AurOS works, and the only place a person can say no. It opens
by itself on the first frame of every session until it is answered — a
notification would be wrong twice, because it goes away and the one
thing that must not go away is the only route to the decision. "Let me
look first" closes it for this session and it is back tomorrow, and the
panel says so in those words: somebody who does not know it will come
back is somebody who answers it to make it go away.

It never writes NVRAM, mounts anything or runs Ferry. It writes one
word into `aurshell`'s own runtime directory — mode 0700, owned by the
person using the machine — and a systemd path unit runs
`rootfs/usr/lib/auros/answer.sh` as root. **The word is not a command.**
It is matched against a fixed list of three, each of which is something
the person at the keyboard is entitled to do, and anything else is
written down and ignored. A setuid binary would mean anything that can
run a program here can rewrite what the machine starts; a polkit rule
means an action, a policy and an agent for three verbs.

### The only place that writes `BootOrder`

`src/aurstage/nvram.h` says at length that the staging environment may
not write `BootOrder` and that there is deliberately no function there
that does, because the way a rule like that gets broken is somebody
finding a function that already does the thing. `aurfirst confirm` is
that function, in a different program, after the answer.

It **promotes**. It never removes anybody else's entry, and on firmware
that ships with no `BootOrder` at all it builds one — writing just our
number would be a boot menu with one thing in it and Windows gone,
which is the one thing this whole design has spent its budget not
doing.

That one contains **every** `Boot####` on the machine, ours first — but
not in plain numeric order. Not every `Boot####` is something to boot:
the UEFI load-option attributes distinguish an entry that is not
`ACTIVE`, one that is `HIDDEN`, and one whose category is APPLICATION
rather than boot — which is what a manufacturer's diagnostics partition
and the firmware setup entry are. In numeric order those land ahead of
Windows on a machine whose owner never asked for it, and undoing that
means the firmware menu this product exists to keep her out of. So they
go **behind** the entries the firmware vouches for.

**Behind, not out**, and that distinction is the whole safety of it.
The first version removed them, and a review built the machine it
breaks: firmware with no `BootOrder` — which is firmware that has
already shown it does not keep the boot variables tidy — and a Windows
entry with `LOAD_OPTION_ACTIVE` clear, which is how several vendors
record "the user switched this off in the boot menu" rather than
deleting the variable. Windows was filtered out, a `BootOrder` holding
only AurOS was written, `confirm` reported success and stamped the
answer permanently, and `aurfirst decline` then refused to undo it
because ours was the only entry left. The way back was gone and the
program had removed it while saying the opposite.

A `BootOrder` is a list of **numbers**. Firmware skips a number whose
entry it will not start, so carrying one costs nothing; leaving one out
can cost somebody their other operating system. An entry `aurfirst`
cannot read at all — unreadable, or longer than it will look at — is
ordered with the ones it vouches for, because not being able to read an
entry is not evidence against it.

The ordering applies only where there was no `BootOrder` to begin with.
An order that already names a hidden entry is preserved exactly:
somewhere there is a laptop whose vendor put it there on purpose, and
reordering it is not this program's decision.

`aurfirst decline` clears the one-shot **before** it records the answer.
The other order leaves an instant where the machine has stopped
re-arming and is still armed once, so the next start reaches AurOS just
after she said it does not work.

### And then Ferry

Importing somebody's documents is the first thing this product does
that restarting cannot undo, so it waits for the answer. `aurfirst
ferry` is the gate and it lives in one place, so `answer.sh` does not
re-derive it from a stamp file it would have to know about. Ferry
itself is unchanged: it mounts the old Windows volume read-only,
refuses a hibernated or dirty one out loud, and reports what could not
come across.

## Power loss, step by step

The point of the ordering above is that this table has exactly one bad
row.

| Loss during | Disk state | Back to Windows? |
|---|---|---|
| 0-3 (Windows side) | untouched | yes, automatically |
| 4 verification | untouched | yes, automatically |
| **4 the resize itself** | **NTFS possibly inconsistent** | **only via the recovery USB and chkdsk; worst case, data loss** |
| 5-8a write, verify, boot partition | NTFS smaller, old GPT in force, garbage in free space | yes — `BootOrder` still points at Windows Boot Manager and the ESP is untouched |
| 8 GPT commit | one sector write; the backup header is already correct | yes, via recovery restore |
| 9-10 | new layout, Windows partition intact and bootable | yes |

## The wizard (`src/aurbridge/wizard.c`)

The GUI is one owner-drawn Win32 window: no common controls, no dialog
manager, no theme API. A grey system button in the middle of the Nocturne
palette reads as unfinished software, and a user who does not trust the
installer is a user who clicks through the disclosure without reading it.
Shapes are rasterised into a 32-bit DIB with a signed-distance field so
corners are anti-aliased; text is GDI with `CLEARTYPE_QUALITY` on the same
surface. DPI awareness is requested through `GetProcAddress`
(`SetProcessDpiAwarenessContext`, falling back to `SetProcessDPIAware`) so
the binary still loads on Windows 7.

Pages, in order:

```
 1 WELCOME      what AurOS is, what this will do
 2 CHECKING     pf_run() on a worker thread; live checklist of its results
 3 BLOCKED      one card per PF_BLOCK: title, detail, REMEDY, R-number
 4 BACKUP       backup + recovery-USB confirmation, both required
 5 CONSENT      the disclosure; typed acknowledgement, not a checkbox
 6 CHOOSE       dual-boot (default) or replace Windows (extra gate)
 7 DESKTOP      which archetype — see docs/SHELLS.md
 8 PERSONALIZE  language, keyboard, time zone, theme
 9 READY        summary, and confirmation of the target drive by name
10 PROGRESS     the phase list above, with per-phase state
```

**The refusal is a page, not a dialog.** BLOCKED has no continue button, no
"advanced", no override, and `nav_allowed()` refuses every page from the
backup gate onward whenever the stored report has a block — so adding one
by accident would not create a way through. `--navtest` exercises that
gate directly, including the case where the user has already ticked every
box and a block appears afterwards.

**Pressing "Start installing" re-runs preflight** and lands on BLOCKED
instead of the phase list if anything changed while the user was reading.

### What the wizard needs from preflight, and does not have

- **`pf_run()` has no progress callback.** It is one blocking call, so the
  checklist cannot show a check going from pending to passed as it happens;
  the wizard runs it on a worker thread and reveals the real results in
  order once it returns. A `pf_progress_cb` on `pf_run()` would make that
  honest rather than staged.
- **`pf_run()` returns early when not elevated**, leaving a report with one
  block and no machine facts. Consumers must not assume `n_disks > 0` on a
  blocked report. The wizard handles this; it is worth stating in the API.
- The wizard **links preflight directly** and reads `pf_is_go()`. The `0`/`1`
  exit-code contract below is for the CLI and CI, not for the GUI.

### Corrections to this document, found while building it

- **Phase 1 is not where consent is asked.** The user has consented on page
  5 long before phase 1 runs. Phase 1 *records* that consent and does the
  BitLocker proof-of-possession. Renaming it in a reader's head as "ask the
  user" produces a wizard that asks twice.
- **The recovery-USB gate is a promise, not a check.** Page 4 only gets the
  user to confirm they have a stick; nothing verifies one exists until
  phase 2 writes it. R4 requires refusing without one, so phase 2 must
  refuse — page 4 cannot.
- **R11's typed confirmation of the target drive is not implemented yet.**
  Page 8 names the drive (model and size) behind a checkbox. A typed
  confirmation belongs on a target-selection page, which does not exist
  because multi-disk selection is not built.
- **An owner-drawn window has no accessibility tree.** There is no UI
  Automation, so a screen reader sees nothing. For a consumer installer
  aimed at people who need help, that is a gap with legal weight in some
  markets, and it is the price of not using system controls. It needs an
  IAccessible/UIA provider before any public download.

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

Phase 0 (preflight) is implemented and builds as a native `.exe`. So is
the phase engine: `src/aurbridge/phases.{h,c}` runs phases 0–3 in order,
and `wizard.c` starts it on a worker thread and reads two numbers on its
timer. **Nothing in `wizard.c` opens a handle to a disk, a volume or a
boot entry, and nothing should be added there** — everything that leaves
the program goes through `plat.h`, which has two implementations:
`plat_win.c`, the real one, and `plat_sim.c`, a computer made of
ordinary files.

That second one is the only reason any of this is testable.
`out/aurbridge-sim` runs the whole engine against a synthetic machine,
so the memory stick and the journal the installer is tested with are the
ones AurBridge produces rather than ones a test wrote to match. What it
cannot exercise is named in `plat_sim.c`'s own header: raw handles
Windows will not always give, `SetFirmwareEnvironmentVariableExW`,
`manage-bde`. Those are what `plat_win.c` is, and they meet a real
machine for the first time on the first machine they meet.

`build/aurbridge` refuses to publish a Windows binary that contains the
simulated platform.

Build order, and nothing from a later stage before an earlier one:

**A — make the staging environment exist and boot. No disk writes at
all.** ✅ **Done**, except the staging UI. `src/aurstage/`,
`build/staging`, `tools/stagetest.sh`.

> **Correction.** This paragraph used to open by listing kernel-config
> gaps — no `CONFIG_VMD`, no `CONFIG_EFIVAR_FS`, no device-mapper, no
> wireless — as the first blocker. **Every one of them is already
> satisfied.** The project builds on Ubuntu's `linux-image-generic`,
> where `CONFIG_VMD=m`, `CONFIG_EFIVAR_FS=y`, `CONFIG_BLK_DEV_DM=y`,
> `CONFIG_CFG80211=m` and `CONFIG_MAC80211=m`. That list was written
> against a custom kernel this project no longer builds, and following
> it would have meant a kernel build nobody needed before the first
> line of the thing that was actually missing.

A minimal init — not systemd, because a generator or an automount that
mounts NTFS behind our back is the one thing this environment must
never do. It loads drivers by walking `/sys` for `modalias` and handing
each to `modprobe`, which is the whole of what udev's coldplug does for
us and needs no udev. 12 MB against an ~80 MB budget.

`switch_root` into an already-installed AurOS is proven, in the same
boot, with the whole disk hashed before and after: **byte for byte
unchanged**, in every case including both abort paths.

Still to do in A: `aurshell` as the staging UI, with a progress model
that survives a forty-minute resize on a 5400 rpm disk without looking
hung; and `BootNext` → staging end to end, which needs the Windows side
to arm it.

**B — read-only verification, still no writes.** ✅ **Done.**
`src/aurstage/{journal,ntfs,shrink,fde,sha256}.{h,c}`, the `dry_run()`
in `main.c`, `tools/ntfstest.sh` and the stage B half of
`tools/stagetest.sh`.

Boot it with `aurstage.dry` on the kernel command line: it looks at
the machine, says what it would and would not do, prints one
machine-readable line, and powers off having changed nothing. The
whole disk is hashed before and after every test case and is
**byte for byte unchanged** in all of them, including every refusal.

What it checks, in order, and what stops it:

| | | |
|---|---|---|
| the record | disk serial, Windows start and length, **SHA-256 of the whole partition table**, the disk's sector size, age | wrong disk, moved, resized, table changed, sector size changed, older than three days, or a clock set *before* the record was written |
| third-party encryption | product names in the disk's first megabyte and in the ESP; a first sector that is statistically random | any hit — unconditional, as above |
| which partition | the record names it; without a record, exactly one candidate is required | more than one Windows volume and nothing to choose by |
| the machine | a GPT disk and EFI variables; a sector size the disk will state | MBR, BIOS boot, or a disk that will not say |
| the volume | `-FVE-FS-` first and always, then the `$Volume` dirty flag, then `hiberfil.sys`, then the `$LogFile` restart area | BitLocker, dirty, hibernated, unclean log |
| the size | `ntfsresize --info --no-action`, against a floor (24 GB, `aurstage.min_gb=` to change it) | it refuses, will not give a number, or there is not enough room |
| the disk | every sector of the region that would be reclaimed | one that will not read, or a drive too slow to finish in four hours |
| the controller | what is on the PCI bus, when no disk appeared at all | — |

**"More than one Windows volume" is the normal case, not a corner.**
Every OEM laptop has a WinRE recovery partition, and it is NTFS. Picking
the first NTFS partition the kernel happens to list — which is hash
order, not disk order — means measuring a 500 MB recovery partition and
reporting the machine convertible. So the record chooses, and when there
is no record and more than one candidate, nothing chooses.

**Three states, not two.** Each of the volume questions can also come
back *don't know* — an `$ATTRIBUTE_LIST`, a compressed attribute, a
record torn by a power cut. Stage B may go on with that, because
`ntfsresize` is the backstop; **stage C may not**, so the report line
carries `sure=yes` or `sure=no` and only `yes` is a machine stage C will
touch.

**Hibernation is the one that matters.** Fast Startup is the default
on Windows 10 and 11, and "shut down" on such a machine hibernates the
kernel session rather than closing it — **without setting the dirty
bit**. A tool that stops at the dirty bit, which is where most stop,
sees a clean volume and resizes a filesystem whose real metadata is in
RAM waiting to be written back over ours at the next resume. So the
reader follows `hiberfil.sys` properly: root directory index,
runlists, the 4 KB header, and `initialized_size` — because the file
is allocated in full when hibernation is switched on and its clusters
hold whatever the disk held before, so reading them raw would report a
machine that has never hibernated as one that has.

**The `--force` rule holds by construction.** The argument vector is a
fixed array in `shrink.c` with no parameter a caller could add to.
`build/staging` refuses to build an image if anything in
`src/aurstage/` opens a device writably — read-only is checked now,
not remembered.

Two gaps, written down rather than quietly skipped: R5 asks for a
surface test of the region NTFS will relocate *into* as well, which
needs `$Bitmap` and is not done; and the report line goes to the
console and nowhere else, because stage B has nowhere to write it.

**What three adversarial reviews found, after it all passed.** Worth
recording, because every one of these was invisible to a test that only
checked the answer:

- The dirty-flag check had **never run** — `$VOLUME_INFORMATION` is
  twelve bytes with its flags at offset 10, and the guard asked for 14.
- It then had no *don't know*: a `$Volume` record torn by a power cut —
  exactly what the fixup check exists to detect — turned correct
  detection of corruption into the sentence "shut down cleanly".
- A heap overflow and a stack overflow, both reproduced under
  AddressSanitizer, from an attribute record too short to hold its own
  header. `tools/ntfstest.sh` now builds with `-fsanitize=address,undefined`.
- `stage_part.is_esp` was **never assigned by anything**, so the half of
  the encryption scan that looks in the ESP — the half that matters on a
  UEFI machine — was handed NULL every time.
- The disk serial was read only from `device/serial` and `serial`, which
  NVMe and virtio publish and **SATA does not**. The check could not
  identify an ordinary laptop disk; the test machine is virtio, so
  nothing said so.
- `mount(..., MS_RDONLY)` on ext4 still replays the journal, so the
  handover wrote to the disk on any unclean root. `noload` now. The
  grep gate cannot see a `mount(2)` — that is written down too.
- `ntfsresize`'s answer is printed *after* its progress bars, and the
  capture buffer filled from the front, so a large fragmented volume —
  the intended population — was refused for a buffer size.
- `init` was dynamically linked against the **build host's** glibc.

**That is the first shippable artifact**, and it is worth shipping on
its own to build a hardware matrix before anyone's disk is at risk.

**C — destructive, one step at a time, each with its own kill-the-power
test.** ✅ **Done.** Filesystem-only shrink, write by offset, read-back
verify, probe from the freshly written root, the one-sector GPT commit,
`BLKRRPART`, `e2fsck`, `resize2fs`, `switch_root`.
`src/aurstage/{gpt,plan,wr,health,image,probe,commit,record,install}.{h,c}`,
and one file — `wr.c` — that the build proves is the only one in the
directory allowed to open a device writably.

**D — the way back.** ✅ **Done.** `src/aurstage/rescue.{h,c}` and
`aurstage.restore`. `tools/installtest.sh` does the whole round trip on
a synthetic machine: install, start AurOS, put Windows back from the
stick, put it back again from the copy on the computer with no stick
plugged in, and refuse a damaged copy with the disk byte-for-byte
untouched — checking afterwards that the three original partitions are
at their original sectors, the EFI partition is byte-for-byte what it
was, the Windows *filesystem* is its full size again, and every file in
it has the md5sum it had before anything was touched.

**The R4 test.** `tools/powercuttest.sh`. The design said "on real
hardware, including one power-pull per step", and that is still the
thing to do before this ships to anybody. What is available instead is
better than nothing and better in one respect than a cord and a
stopwatch: the installer names its own dangerous instants (`fault.h`)
and a fault-injection build can be told to stop dead at exactly one of
them, so every run is the same run and a failure is reproducible by its
name. Fourteen of them — nine during the install and five during the
restore — each followed by a restore and a byte-for-byte comparison
against the machine as it was.

What that does **not** prove is written down rather than glossed: a
particular drive lying about having flushed; one firmware that is not
OVMF; anything about the electrical behaviour of a real power supply.
`tools/matrixtest.sh` covers the geometries — 4Kn, a gigabyte OEM EFI
partition, a first partition at LBA 34, MBR, BIOS, BitLocker,
hibernated, two Windows volumes, no room — and firmware variety is the
one thing in this list that cannot be synthesised at all. The mitigation
for it is the dry run: it writes nothing, prints one machine-readable
line, and is worth shipping on its own to build a hardware matrix before
anyone's disk is at risk.

Nothing destructive ships until the fault matrix is green on every named
instant and the dry run has been through a fleet.
