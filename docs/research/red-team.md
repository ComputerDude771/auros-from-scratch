# Adversarial Risk Register

Produced by a hostile review of the product concept. Ranked by expected
damage (severity × probability × irreversibility). Every mitigation here
is a **requirement on AurBridge**, not a suggestion.

> The review's one-line verdict: *"a consumer-grade disk-destruction tool
> with a wizard on it"* — unless every hard stop below is implemented and
> a recovery partition exists before the first destructive byte.

---

## R1 — BitLocker / Device Encryption · CATASTROPHIC · HIGH

Windows 11 24H2 enables Device Encryption automatically on clean
installs when the user signs in with a Microsoft account, and 24H2
*relaxed* the hardware requirements, so far more machines are silently
encrypted than users believe. Changing the disk layout changes the TPM
PCR measurements; on next Windows boot BitLocker demands a 48-digit
recovery key the user has never seen, stored in a Microsoft account
reachable only from a browser — on the computer that now runs AurOS.

**We would destroy the data while trying to preserve it**, which is
worse than wiping it, because the user was promised Windows stays intact.

**Required:**
- Detect protection state (`Win32_EncryptableVolume`, `manage-bde -status`)
  before anything.
- **Typed-back proof of possession** of the recovery key. Not a checkbox —
  the user types a portion of the key back.
- Suspend with `manage-bde -protectors -disable -RebootCount 0`, and
  disclose that this leaves the volume decrypted at rest.
- TPM-only protector with no key available → **hard stop, no override.**

## R2 — Windows Fast Startup · CATASTROPHIC · HIGH (on by default)

"Shut down" on Windows 8/10/11 hibernates the kernel session to
`hiberfil.sys`. If Windows later *resumes* from that image after we have
repartitioned, it writes back an in-memory NTFS metadata and partition
map from *before* our changes. Result: NTFS metadata pointing at blocks
we gave to Linux, our filesystem overwritten by Windows' cached writes.
**Both operating systems destroyed, silently, hours later.**

Also: `ntfs3`/`ntfs-3g` refuse read-write mount of a hibernated volume,
so file import fails on a majority of machines if unhandled. The common
workaround (`ntfsfix --clear-dirty`) deletes the user's unsaved session.

**Required:** detect the hibernation flag in the NTFS volume header *and*
`hiberfil.sys`; require a **verified full shutdown** (`powercfg /h off`,
forced real restart, re-verify flag clear) as a hard gate; never offer a
Windows boot entry that could resume a stale hibernation image.

## R3 — The one-reboot constraint was the root cause · RESOLVED

Doing all destructive work inside live Windows means that between the
first destructive write and a successful Linux boot **there is no
environment in which recovery code can run.** Fail at 60% → the machine
boots into nothing.

**Decision taken:** the literal one-reboot promise is dropped. AurBridge
reboots into a branded, recovery-capable AurOS staging environment that
owns its own failure, then into the desktop. One click, one uninterrupted
branded experience, two power cycles. See `docs/PLAN.md §2.1`.

## R4 — Single-PC catastrophic failure · CATASTROPHIC · HIGH at scale

Install fails at 70%. Windows bootloader replaced, Linux not bootable.
The user has one computer, now a brick, no USB stick, no second machine,
and their photos on a partition they cannot reach. This is not a support
problem, it is a product-existence problem.

**Required — all of these, not a subset:**
1. **Recovery partition written FIRST**, before the first destructive
   operation: rescue image, original partition table, original MBR/BCD,
   one-button "Put Windows back". *The single highest-value engineering
   decision available.*
2. **Never remove the ability to boot Windows** until the new system has
   booted and the user has confirmed it works. Use UEFI `BootNext` — it
   is one-shot and self-reverting; firmware restores the old order
   automatically if the boot fails.
3. **Two-stage commit.** Stage 1: shrink + write + add boot entry, Windows
   untouched and still default. Stage 2 (only after a confirmed-good
   Linux boot): import files, optionally make AurOS default.
4. **Mandatory recovery USB.** Refuse to install without one. This costs
   conversion. Take the hit.
5. Printed/emailed recovery card generated *before* the install.

For the user with no USB, no second device and no phone: **there is no
mitigation.** Gate them out at the front door.

## R5 — Failing drive · CATASTROPHIC · MEDIUM-HIGH (concentrated in our market)

NTFS shrink *moves files*, reading sectors untouched for years; then we
write several GB — the most aggressive workload that drive has ever seen.
We are selecting for exactly this population: the pitch is "your old PC".

**Required:** read SMART first (reallocated, pending, uncorrectable,
power-on hours). **Refuse on any pending/uncorrectable sectors, no
override.** Read-verify the region to be reclaimed before committing.
Read-back-verify every written block.

*A user told "your hard drive is failing, back up now" is a user we
saved. A user whose drive dies during our install is one we killed.*

## R6 — Power loss mid-repartition · CATASTROPHIC · MEDIUM

The partition table is **not journalled** and data movement during a
shrink is not atomic with it.

**Required:** exactly one atomic commit point — all data movement first
with the *old* table still in force, verify, then write the new partition
table as a single sector write + flush. Keep the pristine table in the
recovery partition and a second location. Refuse on battery <50% or not
on AC. On-disk transactional journal so a restarted installer knows which
step it died in. *This one is genuinely solvable with careful ordering.*

## R7 — NTFS shrink refuses or under-delivers · HIGH · VERY HIGH (the normal case)

Windows shrinks only from the end of the volume and will not relocate
pagefile, hiberfil, VSS storage, `$MFT`, `$Bitmap`, `$UsnJrnl` while
running. 30GB free may yield 2GB offered. Documented ceiling ~50% even
ideally.

**Required:** offline shrink with `ntfsresize` in the staging environment,
after a clean `chkdsk`. Disable pagefile/hibernation, delete shadow
copies, MFT-aware defrag first — all disclosed and revertible.
**Do not write our own NTFS resizer.**

## R8 — BIOS/MBR machines · CATASTROPHIC · HIGH on BIOS

Endless OS — better funded, shipping real hardware — **failed for 20-30%
of users on BIOS systems** doing approximately this, and concluded
dual-boot is for evaluation, not long-term use.

On BIOS/MBR there is no `BootNext`: no one-shot, self-reverting boot
attempt. Overwrite the MBR, get it wrong, get a blinking cursor.
Compounding: MBR allows **four primary partitions**, and a typical OEM
Win10 layout already has four (System Reserved + Windows + OEM Recovery +
OEM Diagnostics). **A fifth partition cannot be created.**

**Required:** detect the 4-primary case and stop with an explanation.
Strongly consider refusing BIOS/MBR entirely at v1.

## R9 — Already-sick NTFS · CATASTROPHIC · MEDIUM

Feeding a corrupt `$MFT` into a resizer turns "a few unreadable files"
into "an unmountable volume". **Required:** mandatory `chkdsk /f /r`
clean result; refuse on dirty bit; refuse on any reported correction.
(`chkdsk /r` on a 1TB spinning disk takes hours — design the UX for it.)

## R10 — Dynamic disks, Storage Spaces, Intel RST/VMD · CATASTROPHIC · MEDIUM

Dynamic disks: the partition table is decorative; the real layout is in an
LDM database. A naive parser sees one `0x42` partition spanning the disk
and gets the geometry completely wrong.

Intel RST/VMD: NVMe reachable only through the VMD controller. Linux may
see **no disks at all** → `INACCESSIBLE_BOOT_DEVICE`, Linux edition. The
fix is a BIOS change our user cannot make.

**Required:** detect and **refuse cleanly** on all three. Build `vmd` and
`nvme` into the kernel image, not as initramfs modules. Never talk users
through BIOS menus.

## R11 — Multi-disk wrong-target write · CATASTROPHIC · MEDIUM

Old desktop: small SSD with Windows, large HDD with 15 years of photos.
Or an external drive attached during install.

**Required:** enumerate every block device; show model/serial/size/label/
content-sample; require typed confirmation of the target disk's size or
label; **refuse to run with removable media attached.** Cheap, prevents a
whole category.

## R12 — The EFI System Partition · HIGH · MEDIUM-HIGH

OEM machines often ship a 100MB ESP already 85-95% full. Reformatting it
destroys `\EFI\Microsoft\Boot\*` and Windows' ability to boot.

**Required:** measure free ESP space first; create our **own** ESP if
insufficient; **never format an existing ESP**; keep the EFI payload
minimal; back up the whole ESP to the recovery partition first.

## R13 — Hardware compatibility on old PCs · HIGH · VERY HIGH

- **WiFi**: Broadcom BCM43xx is everywhere in 2010-2016 laptops, split
  across `b43`/`brcmsmac`/`brcmfmac`/`wl` with conflicting per-revision
  requirements. **WiFi failure is the highest-frequency catastrophic
  outcome in this register** — no internet, no docs, no way to push a fix,
  and it is silent until after the reboot.
- **NVIDIA**: pre-Kepler needs the 390 legacy branch, unsupported on 6.8+
  without distro-maintained patches. nouveau cannot reclock on Fermi/Kepler.
- **Backlight**: wrong path → panel at 0%. User sees a black screen and
  reports that we bricked their laptop. From where they stand, correct.
- **Suspend/resume, audio (HDA quirk tables)**: per-machine ACPI quirks.
- **Firmware**: `linux-firmware` is 700MB-1GB+ installed. Ship it all
  (fat download) or guess, and every wrong guess is a dead WiFi card.

Google ships ChromeOS Flex with a **certified models list** and three
status tiers, and does not guarantee function outside it. *If Google
needs a whitelist, we need a whitelist.*

**Required given the "ship to everyone" decision:** test WiFi, backlight,
audio and suspend **from inside the staging environment, before
committing anything destructive.** If WiFi will not come up there, abort
and leave Windows alone. This is the strongest single argument for the
staged-install architecture and is worth more than any other mitigation.

## R14 — musl · RESOLVED

musl means no NVIDIA driver ever, and no prebuilt Chrome, Zoom, Teams,
Spotify, Steam or Dropbox. For schools, "we need Chrome for our state
testing platform" is an instant disqualification.

**Decision taken:** switched to glibc, built from source. Still fully
from-scratch; removes the ecosystem blocker. See `pkg/core/glibc`.

## R15 — Secure Boot, and the 2026 CA transition · HIGH · HIGH

shim-review requires a legal entity with company registration records, EV
certificate details, SBAT generation ≥5, demonstrated NX compatibility,
and PGP-verified security contacts. Official guidance says 2-3 months;
a from-scratch bootloader will get more scrutiny — budget 6 months.

**The 2026 transition is aimed at our exact market:** Microsoft's 2011
third-party UEFI CA is expiring, replaced by the 2023 CA. Newly-signed
shims chain to the 2023 CA, and **old machines' firmware predates it and
often cannot be updated.** We would pay the cost and delay of signing
without getting the coverage.

## R16 — Kernel currency

Upstream 6.8 is EOL. We build from Ubuntu's `linux-source-6.8.0`, which
*is* maintained with backported fixes through 2029 for 24.04 — but a
consumer product should track a genuine LTS line (6.12 / 6.6 / 6.1).
kernel.org is egress-blocked in this build environment; **revisit the
kernel source before any public release.**

---

## The five things that must be true

1. **The installer can always put the machine back.** Recovery partition
   written before the first destructive byte; reachable from a boot menu
   without a USB stick, a second computer, or us.
2. **Nothing destructive happens until the new system has booted and the
   user has confirmed it works.** Two-stage commit, self-reverting
   `BootNext`, Windows bootable and default until the user says otherwise.
3. **We know the machine before we touch it, and refuse the ones we
   can't handle.** Refusing must be a first-class, well-designed product
   outcome — not an error dialog.
4. **Hardware enablement is the unpriced risk.** Given the decision to
   ship broadly from a from-scratch base, this is where the project's
   actual danger now sits. Probe-before-commit in the staging environment
   is the primary control.
5. **A real legal entity, insurance, and a lawyer-drafted consent flow
   before the first public download.** The EU Product Liability Directive
   (2024/2853) applies from 9 December 2026 and cannot be disclaimed away.

## The single most likely cause of death

> *"The gap between the code that exists and the code that matters. The
> from-scratch path is a gravity well of interesting, tractable,
> aesthetically satisfying problems — writing a PNG encoder is fun, and
> writing a BitLocker detection path that correctly refuses to run is
> not."*

**Countermeasure adopted:** AurBridge's preflight and safety engine is
now the build priority, ahead of further desktop work.
