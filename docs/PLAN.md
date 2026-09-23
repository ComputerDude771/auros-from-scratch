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
reboot again into the result. We get one — and the way we get it is the
opposite of what this section used to claim.

**Nothing destructive happens inside Windows.** Windows only inspects,
gets consent, writes a recovery payload into free space, and arms
`BootNext`. The restart then lands in **the AurOS initramfs**, which is
the installer: it shrinks NTFS, writes the image, verifies it, probes
the hardware, commits the partition table, and `switch_root`s into the
finished system **in the same boot**. One power cycle, and every
destructive operation happens somewhere recovery code can run.

The reason it has to be this way round is specific and mechanical:
**Windows' online shrink cannot move `pagefile.sys`, and disabling the
pagefile does not remove the file until after a restart.** So shrinking
from inside Windows *costs* a restart — the exact thing this design
spends its whole budget avoiding. Offline `ntfsresize` treats the
pagefile as an ordinary file and relocates it.

The second reason is what happens when the restart does not: firmware
that ignores `BootNext` is not rare on old machines with a full NVRAM
store. Under this ordering that is a non-event — nothing has changed and
Windows comes back. Under the old one, Windows boots onto a disk that
has already been repartitioned underneath it.

This is still tractable only because of one choice: we **write a
prebuilt filesystem image** into the new region rather than implementing
an ext4 writer. A write loop is boring and auditable; an ext4 writer is
a corruption-bug factory. The image is **grown to fill the partition**
with `resize2fs` before `switch_root`, where a real kernel is available.

*Why not just boot an installer from a USB stick?* Because "insert USB,
change boot order, press F12" is exactly the wall the non-technical user
hits. `BootNext` removes it: the staging environment boots without the
user touching firmware or a boot menu, from the ESP, or from the
recovery USB when the ESP has no room. Removing that wall is the
product.

See `docs/AURBRIDGE.md` for the phase list and the power-loss table.

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
 WINDOWS — nothing here changes the disk layout
 1  Website          user picks "Download for Windows"
 2  Download         one signed .exe
 3  Launch           SmartScreen does NOT warn  (requires code signing)
 4  Preflight        UEFI? BitLocker? third-party FDE? dynamic disk?
                     real reclaimable space? sector size? battery?
                     → refuse loudly if unsafe
 5  Backup gate      verify a backup exists, or make one, or explicit override
 6  Consent          plain language: what changes, what is kept, how to undo
 7  Choose           dual-boot (default) or replace
 8  Desktop          which archetype — see docs/SHELLS.md
 9  Personalize      language · keyboard · timezone · theme · apps
10  Fetch            download the payload matching those choices
11  Recovery         payload -> the USB and a file on the Windows volume;
                     staging kernel + initramfs -> the ESP (or the USB)
12  Inventory        record what to migrate from Windows
13  Boot handoff     re-run preflight, then set BootNext
14  RESTART          ← the only one

 STAGING — the AurOS initramfs, same boot
15  Re-verify        the machine still matches the journal, NTFS is clean,
                     the disk is visible, the surface reads
16  Shrink           ntfsresize, filesystem only. Windows still boots.
17  Write            image by offset, old partition table still in force
18  Verify           read back and hash
19  Probe            mount the new root, load ITS drivers and firmware,
                     test WiFi / backlight / audio on the real machine
20  Commit           new GPT · partx · resize2fs · recovery partition
21  switch_root      into the finished system

 AUROS
22  First boot       start the shell
23  Ferry            import files, bookmarks, wallpaper, wifi, locale
24  Welcome          short tour; Windows is still there and still boots
```

**Every step up to and including 19 must leave the machine bootable into
Windows.** That is a hard requirement, not a goal. Step 16 is the only
non-restartable window in the entire product; everything after it writes
into space that is already free.

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

1. **Secure Boot — answered, and not by us.** AurOS boots through
   Canonical's `shimx64.efi.dualsigned` and their signed
   `grubx64.efi`, copied out of the image rather than rebuilt, so
   Microsoft's signature is already on them and no submission is
   needed. `tools/loadertest.sh` proves it against
   `OVMF_CODE_4M.ms.fd` with Microsoft's keys enrolled and Secure
   Boot enforcing. The shim is *dual*-signed — by the 2011 CA as well
   as the 2023 one — and the 2011 CA is the one old firmware trusts,
   which is the whole reason the chain is borrowed: the dual-signing
   window closed in June 2026, so a distribution applying on its own
   today gets a 2023-only shim that would not boot on exactly the
   machines this product exists for. See `docs/SIGNING.md`.
2. **Code signing — still a purchase.** An unsigned `.exe` gets
   SmartScreen's full-screen panel whose only visible button says
   *Don't run*. For an audience defined as non-technical that is
   fatal, and for the ones who find the hidden *Run anyway* it is
   worse: they have been taught to click past a security warning
   immediately before handing a program their whole disk. The
   pipeline is written and tested (`build/sign`, `tools/signtest.sh`,
   12 checks); what is missing is an OV or EV certificate on a
   hardware token or cloud HSM, which since June 2023 is the only
   form any public CA will issue. `docs/SIGNING.md` says what to buy
   and why EV is the one worth the difference here.
3. **BitLocker — refused, permanently.** There is no shrink path for
   a protected volume, online or offline, and suspension does not
   decrypt a sector. Preflight blocks and the remedy says what is
   actually required — fully decrypt — rather than implying that
   producing the recovery key clears it. `docs/AURBRIDGE.md` has the
   reasoning; this matters more over time, not less, as Windows 11
   24H2 enables device encryption on more clean installs.
4. **NTFS shrink limits — measured, never promised.** `ntfsresize
   --no-action` is asked before anything is said to the user, and the
   number it gives is the number the plan is built from. A machine
   where the answer is "not enough" is refused with the disk
   untouched.
5. **Liability.** This product repartitions consumer disks. Someone
   will lose data. The entity, EULA and support path must exist
   before the first public download — and the entity is the same one
   the code-signing certificate is issued to, so it is on the
   critical path twice.
6. **No Firefox from this builder.** The network these images are
   built on answers `403` for every Mozilla and Launchpad host
   (`packages.mozilla.org`, `ppa.launchpadcontent.net`,
   `ftp.mozilla.org`, `download.mozilla.org`), so all five ship
   Epiphany. `build/forge` tries three routes and refuses to
   substitute quietly — `ALLOW_BROWSER_FALLBACK=1` is the deliberate
   override, recorded in every manifest and in
   `/etc/auros/build-warnings` — but the fix is a network policy, not
   a commit. See `docs/results/images.md`.
7. **Nowhere to download the image from.** The installer fetches it,
   resumably, against a hash baked in at build time
   (`tools/exetest.sh`, 14 checks) — and `AUROS_IMAGE_URL` is empty,
   because there is no host yet. A release build refuses without one.

Detailed findings on each are being gathered and land in `docs/research/`.

---

## 6. Where the work is right now

**The whole path works, end to end, on a synthetic machine.**

A person double-clicks one `.exe`. It looks at the computer and refuses
the ones it cannot help; it explains and asks; it writes a memory stick
and arms a one-shot boot entry; the machine restarts **once** into the
staging environment, which shrinks Windows, writes AurOS, verifies it,
probes the hardware, commits a new partition table in one sector,
writes a signed boot chain into a partition of its own, puts AurOS in
the firmware's menu without making it the default, and hands over to it
in the same boot. AurOS comes up, asks whether it works, and only when
somebody says yes does it become what the computer starts — and only
then may it read a single file off the old Windows volume.

What proves it, and the numbers are the current transcripts in
`docs/results/`:

| | |
|---|---|
| `installtest.sh` | 37 — install, start, put Windows back twice, refuse a damaged copy, refuse a stick for another AurOS |
| `loadertest.sh` | 18 — and does it come back up in AurOS with the stick out, including under Secure Boot |
| `powercuttest.sh` | 138 — the power goes at each of seventeen named instants |
| `matrixtest.sh` | 10 — shapes of computer that cannot be bought |
| `imagetest.sh` | 23 — the image is found by what is on it, including on a stick holding two |
| `bridgetest.sh` | 13 — the two halves agree about the bytes |
| `aurfirsttest.sh` | 46 — BootOrder is untouched until somebody says so, and nothing is ever dropped from it |
| `nvramtest.sh` | 25 — the boot entry, decoded by something that did not write it |
| `wrtest.sh` | 23 — writes land only where they were armed to |
| `exetest.sh` | 17 — one file, and a download that resumes |
| `signtest.sh` | 12 — it signs, and a changed byte breaks it |
| `welcometest.c` | 27 — the question, and what each answer asks for |
| `targets.c` | 10021 — everything she has to press, at every text size |
| `modaltest.c` | 53 — a panel covers the desktop and can always be left |

**What is left is not code.** A certificate. A host for the image. A
network that allows Firefox. A legal entity. And the thing none of the
above can substitute for: a real, old, dusty PC with a real firmware
and a real disk that lies about having flushed.
