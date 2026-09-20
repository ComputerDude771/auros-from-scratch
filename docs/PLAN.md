# AurOS — Product Plan

> **Exit condition.** A non-technical person on an old Windows PC opens a
> website, downloads one file, double-clicks it, reads and agrees to what
> will happen, and — after **exactly one restart** — is sitting in a
> working, good-looking Linux desktop with their own files already there.
>
> Nothing short of that whole chain counts as done.

---

## 1. What we are actually building

Four products that have to work as one:

| # | Component | What it is |
|---|-----------|------------|
| 1 | **AurOS** | The distribution. Our init, package manager, desktop shell, theme engine, on an upstream kernel and libc. |
| 2 | **AurBridge** | The Windows `.exe`. Preflight, consent, repartition, write, boot-handoff. The riskiest component by far. |
| 3 | **Ferry** | First-boot migration: pulls files and settings off the intact Windows partition. |
| 4 | **Forge** | The customization layer. One declarative profile file → a branded, locked-down, localized build. This is the commercial differentiator. |

Plus a website, a build pipeline, signed artifacts, and an update channel.

---

## 2. The decisions, and why

The instruction was to ask "why this?" at every step. These are the calls
that matter, with the reasoning exposed so they can be attacked.

### 2.1 Why one reboot is achievable

The normal Linux install is two reboots: boot an installer, install,
reboot again into the result. We get one by **doing the entire install
from inside Windows** and rebooting straight into a finished system.

That is only tractable because of one choice: we **write a prebuilt
filesystem image byte-for-byte** into the new partition rather than
implementing an ext4 writer for Windows. Writing a raw image is a loop
over `WriteFile` on a volume handle — boring, auditable, and hard to get
subtly wrong. Implementing ext4 write support in Windows userspace is a
corruption bug factory. The image is created small and **grown to fill
the partition on first boot**, where we have a real Linux kernel and
`resize2fs`.

*Why not just boot an installer?* Because "insert USB, change boot order,
press F12" is exactly the wall the non-technical user hits. Removing it
is the product.

### 2.2 Why we keep the Windows partition

The brief floated exporting files to cloud storage and pulling them back.
We should not do that. Keeping the Windows partition and mounting it from
Linux is **safer, faster, free, private, and reversible**. The kernel has
read NTFS natively since 5.15 (`ntfs3`). No data leaves the machine.

Cloud export stays in the design only as the fallback for users who
choose to erase the whole disk — and if it is ever used, it is
client-side encrypted before upload.

### 2.3 Why from-scratch, and exactly how far

This is the decision most likely to be wrong, so it gets the most words.

**What "from scratch" buys:** total control of the layer the user sees
and the layer an organization customizes. No upstream theme fighting our
theme. No init system we did not design. No package manager whose
policies we inherit.

**What it costs:** hardware enablement. Twenty years of quirk handling —
WiFi chipsets, GPU drivers, suspend/resume on a 2013 Dell — is not
something anyone reproduces. On *old consumer hardware specifically*,
that quirk handling **is** the product working or not working.

**So the line is drawn deliberately:**

```
  OURS, written here          init · package manager · desktop shell
                              theme engine · installer · migration
                              profile/customization system
  ───────────────────────────────────────────────────────────────────
  UPSTREAM, built by us       kernel · musl · busybox · firmware
  from source                 graphics stack · browser
```

Everything a user or an administrator touches is ours. Everything that
talks to silicon is upstream, compiled by our own build system from
source we pin and checksum. This is what Gentoo and Arch do, and it is
the only version of "from scratch" that ships.

*Nobody writes their own Mesa.* Saying otherwise would be the
"make-believe solution" the brief warned against.

### 2.4 Why musl and busybox

Small enough to read end to end, static-links cleanly, keeps the base
system in single-digit megabytes — which matters when the installer
payload is something a user downloads over a home connection. The cost is
glibc incompatibility for some binary-only software; we accept it for the
base and revisit if it blocks a must-have app.

### 2.5 Why the desktop renders directly to DRM/KMS

No X11, no Wayland compositor dependency, no Mesa requirement for the
shell itself. The shell owns every pixel, so theming is total rather than
"as much as the toolkit allows". It also boots on anything with a
framebuffer, which on old hardware is a real advantage.

The cost is that we implement text rendering, input, and window
management ourselves, and that third-party GTK/Qt apps need a
compositor alongside. That is a known, scoped cost — not a surprise.

---

## 3. The end-to-end flow, as a contract

Each step is a testable checkpoint.

```
 1  Website          user picks "Download for Windows"
 2  Download         one signed .exe
 3  Launch           SmartScreen does NOT warn  (requires code signing)
 4  Preflight        UEFI? Secure Boot? BitLocker? disk space? battery?
                     known-bad hardware? → refuse loudly if unsafe
 5  Backup gate      verify a backup exists, or make one, or explicit override
 6  Consent          plain language: what changes, what is kept, how to undo
 7  Choose           dual-boot (default) or replace
 8  Personalize      language · keyboard · timezone · theme · apps
 9  Fetch            download the payload matching those choices
10  Prepare          suspend BitLocker · disable hiber/pagefile · shrink NTFS
                     create partitions · write image · verify checksum
11  Boot handoff     install loader to ESP · set BootNext
12  Inventory        record what to migrate from Windows
13  RESTART          ← the only one
14  First boot       grow filesystem · detect hardware · start shell
15  Ferry            import files, bookmarks, wallpaper, wifi, locale
16  Welcome          short tour; Windows is still there and still boots
```

**Every step before 13 must be abortable with the machine left exactly as
it was.** That is a hard requirement, not a goal.

---

## 4. Workstreams

| ID | Stream | Status |
|----|--------|--------|
| W1 | Distro core — toolchain, base system, init, package manager | **in progress** |
| W2 | Desktop shell + theme engine | theme engine done; shell in progress |
| W3 | Forge — profiles, image builder, lockdown, locale | not started |
| W4 | AurBridge — the Windows installer | not started |
| W5 | Ferry — migration engine | research in progress |
| W6 | Website, signing, update channel | not started |
| W7 | Safety, hardware matrix, rollback, QA | research in progress |
| W8 | Documentation and runbooks | this file |

---

## 5. Known hard blockers

Recorded now so they are never a late surprise. These are product
prerequisites, not engineering tasks — some cost money and calendar time
that no amount of code replaces.

1. **Secure Boot.** An unsigned bootloader will not start on a machine
   with Secure Boot enabled, which is most Windows 10/11 hardware. The
   real fix is a Microsoft-signed `shim`, which requires a legal entity
   and a review process measured in months.
2. **Code signing.** An unsigned `.exe` triggers SmartScreen's "unknown
   publisher" wall. For an audience defined as *non-technical*, that wall
   is fatal to conversion. Requires a certificate on a hardware token.
3. **BitLocker.** Common on modern Windows. The volume cannot be shrunk
   until protection is suspended.
4. **NTFS shrink limits.** Unmovable files cap what can be reclaimed. Must
   be measured before promising the user anything.
5. **Liability.** This product repartitions consumer disks. Someone will
   lose data. The entity, EULA, and support path must exist before the
   first public download.

Detailed findings on each are being gathered and land in `docs/research/`.

---

## 6. Where the work is right now

**Done and verified:**
- `aurora` theme engine — inheritance, colour algebra, 9 render targets, 4 themes
- `aurwall` — procedural wallpaper renderer + PNG encoder with its own DEFLATE
- `aurb` — source-to-package build system with dependency resolution
- musl 1.2.4 built from pristine upstream source; toolchain verified musl-only

**Next:** busybox, kernel, `aurinit`, the package manager, a bootable ISO
proven in QEMU — then AurBridge.
