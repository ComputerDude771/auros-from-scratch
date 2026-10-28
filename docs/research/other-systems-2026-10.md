# Other operating systems through the AurOS installer

Research date **2026-10-23**, for phase 2 ("an OS she wants" = AurOS styles
first, then other systems; `docs/handoff/SESSION-2026-10-23.md`).
Point-in-time; re-check before betting user data on any line.

How each claim was checked:

- **[V]** I looked at the primary artifact myself today: an ISO's files read
  over HTTP byte ranges from `releases.ubuntu.com` / `cdimage.ubuntu.com`
  (`sbverify --list`, the grub image's built-in module list, `grub.cfg`,
  manifests), the Ubuntu archive's `Packages.gz` for noble and resolute,
  `.deb` contents, or installer source code on GitHub (raw files, branch
  `main`/`master` as of today).
- **[P]** a primary source read through a search-engine summary (WebFetch
  could not resolve any host from this session, and the shell's proxy blocks
  linuxmint.com, zorin.com, fedoraproject.org, elementary.io, Google).
- **[2]** a secondary source (press, forum, community wiki).
- **[?]** could not be confirmed; stated as unknown.

Settled elsewhere and not repeated: the 2011/2023 Microsoft CA change and
which PCs have which `db` (`every-machine-2026-10.md` section 1), Rufus-style
user-mode disk writes (`signing-trust.md`).

---

## 0. The answer in five lines

1. **Route B: the official, unmodified system, installed by its own installer
   running unattended.** It should run *inside AurOS's machinery*: AurOS
   shrinks C:, makes and commits the partitions, owns BootNext/BootOrder and
   the way back, and the distro's installer only formats and fills partitions
   it is given. Route A cannot use the names people ask for, and it cannot
   rebuild Mint, Zorin, elementary or Pop from the Ubuntu archive anyway.
2. **First systems:** Ubuntu 26.04 LTS. Then, with the same installer and
   config, Xubuntu, Ubuntu Cinnamon and Ubuntu Budgie 26.04. Then Linux Mint
   22.3 (Cinnamon), then Fedora Workstation. Fedora is the only one with a
   2023-signed shim.
3. **Not now:** Kubuntu and Lubuntu (Calamares has no unattended mode),
   elementary OS (its installer has no answer file), Pop!_OS (its download
   page says to turn Secure Boot off), ChromeOS Flex (no dual boot, wipes the
   disk), Ubuntu MATE (no 26.04 LTS image).
4. **Names:** the publisher's own product name, used as a plain description of
   the unmodified image we download from the publisher ("Ubuntu 26.04 LTS",
   "Linux Mint 22.3 Cinnamon", "Fedora® Workstation 45"), plus a not-affiliated
   line. No logos until each publisher says yes in writing. AurOS's own images
   stay "AurOS, based on Ubuntu".
5. **The bar moves.** Route B cannot hide the install behind the download the
   way D1 does. Expect the download plus 5-15 more minutes after the restart
   (estimated, not measured). Mint is the smallest download (about 2.9 GB).

---

## 1. Facts that apply to both routes

### 1.1 Secure Boot status of the images as of today

| Image | shim | Microsoft signature on shim | grub | Source |
|---|---|---|---|---|
| Ubuntu 24.04.5.1 desktop (built 2026-09-14) | 15.8-0ubuntu1 | **UEFI CA 2011 only** | 2.12-1ubuntu7.3 | [V] `EFI/boot/bootx64.efi` from the ISO, `sbverify --list` |
| Ubuntu 26.04.1 desktop (2026-08-26) | 15.8-0ubuntu2 | **UEFI CA 2011 only** | 2.14-2ubuntu1, Canonical 2022 v1 | [V] same method |
| Ubuntu 26.10 desktop (2026-10-13) | 15.8-0ubuntu2 | **UEFI CA 2011 only** | 2.14-2ubuntu1 | [V] same method |
| Ubuntu 26.04.1 kernel | n/a | Canonical Secure Boot Signing (2022 v1) | n/a | [V] `casper/vmlinuz`, `sbverify --list` |
| resolute `shim-signed` | 1.59+15.8-0ubuntu2 | 2011 (per earlier report) | n/a | [V] resolute `Packages.gz`; `every-machine-2026-10.md` |
| Fedora `shim-x64` | 16.1-6 (2026-02-06) "add signature from Microsoft UEFI CA 2023 signer" on x86_64, so **2011 + 2023** | | | [P] Fedora package changelog via search; a Fedora 44 user on 16.1-5 still had 2011 only ([discussion.fedoraproject.org](https://discussion.fedoraproject.org/t/updating-the-shim-package-in-fedora-44-16-1-8/194569)). Which shim the **Fedora 45 ISO** carries: [?] |
| Linux Mint 22.3, Zorin OS 18, elementary OS 8 | All three are built on Ubuntu 24.04 and ship Ubuntu kernels, so they are expected to carry Ubuntu's 15.8 shim | | | [?] I could not reach their mirrors. elementary's build config (`elementary/os`, `mkosi.images/liveiso/mkosi.conf`, next release on resolute) installs `shim-signed` + `grub-efi` + `casper` [V]. One Zorin 18.1 user hit "Invalid Key" on a 2023-only PC ([forum.zorin.com](https://forum.zorin.com/t/secure-boot-invalid-key-on-zorin-os-18-1-acer-aspire-looking-for-solutions/68483/7), [2]) |
| Pop!_OS 24.04 | none | n/a | systemd-boot | [P] System76's download page still says to disable Secure Boot. The tracking issue was still open in 2026 ([pop-os/pop#3395](https://github.com/pop-os/pop/issues/3395), [2]) |
| ChromeOS Flex | Google's own | supports Secure Boot | | [P] Google FAQ via search. Irrelevant, see 4.2 |

**So what:** every Ubuntu-family ISO published today, including 26.10 from
ten days ago, fails on the same 2023-only PCs that AurOS already refuses.
Route B with Fedora is the only way to reach those PCs this year.

### 1.2 What is in the Ubuntu archive (for Route A)

[V] `archive.ubuntu.com` `Packages.gz`, noble + noble-updates (main,
universe), checked again against resolute (26.04):

| Desktop | Meta-package in the archive | Installer the official ISO uses |
|---|---|---|
| Ubuntu (GNOME) | `ubuntu-desktop-minimal` 1.539.2 (main) | `ubuntu-desktop-bootstrap` snap (Flutter + subiquity) [V 26.04.1 manifest] |
| Kubuntu | `kubuntu-desktop` 1.451.2 (universe) | **Calamares** 3.3.14 [V 26.04.1 manifest] |
| Xubuntu | `xubuntu-desktop(-minimal)` 2.262 | subiquity (`casper/install-sources.yaml` present) [V] |
| Lubuntu | `lubuntu-desktop` 24.04.10 | **Calamares** 3.3.14 [V] |
| Ubuntu MATE | `ubuntu-mate-desktop` 1.296 | **No 26.04 release on cdimage.** Only 24.04.x, 25.10 and 26.10 [V directory listing] |
| Ubuntu Budgie | `ubuntu-budgie-desktop` 0.126 | subiquity [V] |
| Ubuntu Cinnamon | `ubuntucinnamon-desktop` 24.04.4; `cinnamon` 6.0.4 in noble, 6.4.13 in resolute | subiquity [V] |
| Linux Mint's own apps | **None**: `mintupdate`, `mintinstall`, `mint-themes`, `mint-artwork`, `xed`, `xreader`, `xviewer`, `pix`, `hypnotix`, `warpinator`, `sticky`, `webapp-manager` are absent from noble and resolute. Only `mint-y-icons`, `libxapp1` and `xapps-common` are there. | |
| elementary (Pantheon) | **None**: no `gala`, `wingpanel`, `pantheon`, `io.elementary.*`. Only `libgranite`. | |
| Zorin | **None**: no `zorin-*`. | |
| Pop!_OS (COSMIC) | **None**: no `cosmic-*`, in noble or in resolute. | |

There is one more fidelity gap. The real Ubuntu desktop is not only debs. The
26.04.1 image seeds snaps: `firefox`, `snap-store` (App Center),
`firmware-updater`, `desktop-security-center`, `gnome-46-2404` and others
[V `casper/minimal.manifest`]. In the archive `firefox` is
`1:1snap1-0ubuntu5`, a transitional package for the snap [V]. A faithful
"Ubuntu" from forge would need snap seeding from Canonical's Snap Store.

---

## 2. Route A: rebuild each flavour with forge

### 2.1 What it gives

Every flavour inherits everything already proven: the D1 fast path (written
while the download runs), the one-sector GPT commit, power-cut tests, "does it
work?", Put Windows back, and `bootchain` updates. One image format, one boot
chain (`\EFI\AurOS`), one set of tests.

### 2.2 What can be built faithfully

- **Close to faithful from the archive alone:** Kubuntu, Xubuntu, Lubuntu,
  Ubuntu Budgie, Ubuntu Cinnamon (but with the archive's older Cinnamon:
  6.0.4 on noble, while Mint ships newer). Ubuntu GNOME is close only if forge
  learns snap seeding.
- **Needs a third-party repository:** Linux Mint (`packages.linuxmint.com`
  for every Mint app and the artwork), Zorin (Zorin's repo), elementary
  (Pantheon from elementary's PPA), Pop (System76's COSMIC repo). Without them
  you get "Cinnamon on Ubuntu", not Mint.

### 2.3 Trademarks and licences

**Canonical (Ubuntu, Kubuntu, Xubuntu, Edubuntu).** [V] The policy text,
`canonical/canonical.com` `templates/legal/intellectual-property-policy/index.md`,
`update_date: "15 July 2015"`, still current on `main` today:

> "You can redistribute Ubuntu, but only where there has been no modification
> to it." ... "Any redistribution of modified versions of Ubuntu must be
> approved, certified or provided by Canonical if you are going to associate
> it with the Trademarks. Otherwise you must remove and replace the
> Trademarks and will need to recompile the source code to create your own
> binaries."

> "Canonical's Trademarks (registered in word and logo form) include: UBUNTU,
> KUBUNTU, EDUBUNTU, XUBUNTU, JUJU, LANDSCAPE" ... "You will require
> Canonical's permission to use: (i) any mark ending with the letters UBUNTU
> or BUNTU which is sufficiently similar to the Trademarks ... (ii) any
> Trademark in a domain name or URL" ... "You cannot use the Trademarks in
> software titles. If you are producing software for use with or on Ubuntu
> you may reference Ubuntu, but must avoid: (i) any implication of
> endorsement".

> §2: "where any other licence grants rights, this policy does not modify or
> reduce those rights under those licences."

That §2 sentence is the "trump clause" added on 2015-07-15 after the
GPL dispute. SFC said the revised policy complies with the GPL but still
"creates confusion and possible risk"
([sfconservancy.org, 2015-07-15](https://sfconservancy.org/news/2015/jul/15/ubuntu-ip-policy/), [P]).

What this means for Route A:

- A forge-built Kubuntu, Xubuntu or Lubuntu is a **modified Ubuntu**. It
  cannot be called Ubuntu, Kubuntu, Xubuntu, or any "...buntu" name, and its
  branding packages would have to go (e.g. `kubuntu-settings-desktop`,
  `kubuntu-wallpapers`). Lubuntu is not in Canonical's list, but it ends in
  "buntu".
- The **"recompile"** sentence reaches beyond names. Read strictly, it asks
  anyone shipping a modified Ubuntu without approval to rebuild the binaries.
  The trump clause narrows it for copyleft packages only. **This already
  applies to the AurOS image today** (Canonical binaries in a modified image,
  with `os-release` rebranded by `build/forge`). Nobody has raised it in this
  repo. Mint, Zorin, elementary and Pop live with the same question. In
  2013-14 Canonical asked Mint to license the binaries it reuses, and the
  terms were never published
  ([InfoWorld](https://www.infoworld.com/article/2243953/will-canonical-force-linux-mint-to-license-ubuntu-binary-packages.html), [2]).
  **Action whichever route is chosen:** write to Canonical (the policy's
  contact; "it may take up to a week") and ask about AurOS.
- Safe wording: "**AurOS**, based on Ubuntu" and "with the KDE Plasma
  desktop" / "with the Cinnamon desktop". An old Ubuntu wiki page says a
  derivative may describe itself as based on Ubuntu, but that only Canonical's
  own statement is binding
  ([DerivativeDistroHowto](https://wiki.ubuntu.com/DerivativeDistroHowto), [2]).
  A Canonical statement in 2015: "no licence is needed to make a derivative
  distribution of Kubuntu. All you need to do is remove obvious uses of the
  Kubuntu trademark" ([Phoronix forum quote](https://www.phoronix.com/forums/forum/software/distributions/41162-no-license-needed-for-kubuntu-derivatives?p=515012), [2]).

**Linux Mint.** I found no formal trademark policy page.

- Mint's branding post says derived distributions may refer to the name but
  may not use it as part of their own name, may never call themselves a
  "Linux Mint Edition", and may not put the logo in splash screens, boot menus
  or the default wallpaper
  ([blog.linuxmint.com/?p=209](https://blog.linuxmint.com/?p=209), circa 2008, [P via search]).
- Forum staff say that unmodified ISOs may be redistributed, and that
  commercial use of the name, or selling machines with a customised Mint,
  needs Clem Lefebvre's permission
  ([forums.linuxmint.com t=169087](https://forums.linuxmint.com/viewtopic.php?t=169087), [2]).
- **Are Mint's packages redistributable?** The code is. `debian/copyright`
  for `mint-themes`, `mintupdate`, `mint-x-icons` says GPL-3+, `mintinstall`
  GPL-3+/2+, `mintsystem` GPL-2+, and `mint-y-icons` GPL-3+ with icons under
  CC-BY-SA-4 [V GitHub `linuxmint/*` master]. `xapp` is LGPL-3
  ([Debian copyright](https://metadata.ftp-master.debian.org/changelogs/main/x/xapp/oldstable_copyright), [P]).
  The brand is not. I found no published terms for `packages.linuxmint.com`
  [?]. An image with Mint's apps and artwork is a "Mint derivative" under
  Mint's branding rules, and still a modified Ubuntu under Canonical's.

**Zorin.** "The 'Z' ... logomark and the 'ZORIN' wordmark are registered
trademarks of Zorin Technology Group Ltd." Use is "for non-commercial use
only", must not imply affiliation, must carry a disclosure that the marks are
registered, and the licence is revoked for anyone distributing paid editions
([zorin.com/press](https://zorin.com/press/), [P via search]). Software
licences are per package, in `/usr/share/doc/<pkg>/copyright`
([zorin.com/legal/terms](https://zorin.com/legal/terms/), [P]). Staff say
Pro may not be redistributed ([forum](https://forum.zorin.com/t/is-zorin-os-free-distribution/25800), [2]).

**elementary.** "elementary" is a trademark of elementary, Inc. Branding can
be redistributed only if the software stays substantially unchanged or the
changes are approved. Driver and hardware enablement are fine
([elementary.io/brand](https://elementary.io/brand), [P via search]).

### 2.4 Route A verdict

It is technically the cheapest per flavour for the Ubuntu flavours, and it is
the only route that keeps the 10-minute fast path. But it **cannot deliver
what the owner asked for**: she cannot be shown "Linux Mint" or "Ubuntu"
(or "Kubuntu", "Zorin", "elementary"). What she can be shown is "AurOS with
the Cinnamon desktop". AurOS's own style choice (phase 1) already covers
"looks like Windows / like a Mac / big buttons". Route A also multiplies the
open Canonical question by every flavour, and makes AurOS the support desk and
update channel for each of them.

---

## 3. Route B: the official system, installed unattended

### 3.1 Booting an official ISO from the internal disk under Secure Boot

**The chain.** AurOS already has a Microsoft-signed Ubuntu shim and
Canonical's grub in `\EFI\AurOS`, reached by BootNext. Ubuntu's ISO
kernels are Canonical-signed (26.04.1: Canonical 2022 v1 [V]). Mint, Zorin
and elementary are expected to ship the same Ubuntu kernels, but I did not
inspect them [?]. So that same chain can start their kernel and initrd, and
shim verifies them against Canonical's CA. Fedora's kernel is Red Hat-signed, so Fedora needs
its own Microsoft-signed shim. AurBridge would copy `EFI/BOOT/BOOTX64.EFI` +
`grubx64.efi` from the Fedora ISO to `\EFI\AurOS-fedora` from Windows.

**Can Canonical's signed grub loopback an ISO under Secure Boot?** Yes. The
modules that matter are built in, because under Secure Boot grub can only use
built-in modules. [V] I listed the module objects inside each signed grub's
`mods` section:

| Signed grub | `loopback` | `iso9660` | `fat` | `ext2` | `ntfs` | `exfat` | `lvm`/`luks` |
|---|---|---|---|---|---|---|---|
| noble 2.12-1ubuntu7.3 (what AurOS ships; 124 modules) | yes | yes | yes | yes | **yes** | no | yes |
| 26.04.1 / 26.10 ISO 2.14-2ubuntu1 (95 modules) | yes | yes | yes | yes | **no** | no | **no** |

So noble's grub can loop-mount an ISO stored on an unencrypted NTFS volume,
and 26.04's grub cannot. Neither can read exFAT or a BitLocker volume. The
Ubuntu ISOs themselves ship `boot/grub/loopback.cfg`
(`linux /casper/vmlinuz iso-scan/filename=${iso_path} ...`) [V 26.04.1], so
loop-booting is an intended use.

**Can the live system find the ISO?** casper's
`scripts/casper-premount/20iso_scan` takes `iso-scan/filename=` and mounts the
containing partition at `/isodevice`. Its `is_supported_fs` accepts
`ext2|ext3|ext4|xfs|jfs|reiserfs|vfat|ntfs|iso9660|btrfs|udf` [V
`casper_1.498`]. The helpers say "originally from lupin-helpers": lupin is
Wubi's casper support (see section 4). Whether the initramfs `ntfs` mount
resolves to ntfs3 on 26.04's kernel: [?] needs a VM run.

**Where to put the ISO, given BitLocker.** On Windows 11 24H2+ clean installs
C: is encrypted by default (`every-machine-2026-10.md` section 2), so neither
grub nor casper can read it. The ISO must live outside C:, in the freed space.
Three ways, all reusing D1's "write raw into unallocated space":

1. **Write the ISO raw into a temporary partition.** The partition then
   contains an ISO9660 filesystem. grub reads it with `iso9660`, and casper
   finds `/casper` on it by scanning block devices. This is the least code.
   But the medium is read-only, so the answer file cannot sit "at the root of
   the installation medium". Pass it another way: cloud-init NoCloud, or
   `subiquity.autoinstallpath=` (see 3.2).
2. **Copy the ISO's contents onto a FAT32 partition** made by Windows. This
   is what Ubuntu's own OEM "reset partition" does: "a FAT32 file system
   containing the entire content of the installer image, so that the user can
   start the installer from GRUB or EFI without using the installation media"
   [V subiquity `doc/reference/autoinstall-reference.rst`]. The limit is
   FAT32's 4 GiB per file. Largest file: Ubuntu 26.04.1 `minimal.squashfs`
   3,432,136,704 B (fits); 24.04.5.1 2,238,521,344 B (fits); Ubuntu Cinnamon
   26.04.1 3,933,966,336 B (fits, barely); **Kubuntu 26.04.1
   `filesystem.squashfs` 4,381,224,960 B (two ISO extents; does not fit)**
   [V]. Zorin Pro reportedly exceeds FAT limits too
   ([computingforgeeks](https://computingforgeeks.com/install-zorin-os-step-by-step-with-screenshots/), [2]).
3. **Put an ISO file on an NTFS partition**, then noble grub `loopback` plus
   casper `iso-scan/filename=`. This works for Ubuntu-family ISOs. For
   Fedora, Anaconda's `inst.repo=hd:<device>:<path>` "mount[s] the given disk
   partition and install[s] from ISO file" [V `rhinstaller/anaconda`
   `docs/user-guide/boot-options.rst`]. Which filesystems Anaconda's dracut
   mounts: [?].

Whichever is used, the staging partition is temporary. It must sit outside
the range the OS will occupy, and Put Windows back has to remove it.

### 3.2 Unattended install, per installer

| Installer (who uses it) | Unattended? | Targets only given space? | User, locale, keyboard, timezone | Evidence |
|---|---|---|---|---|
| **subiquity** via `ubuntu-desktop-bootstrap` (Ubuntu, Xubuntu, Ubuntu Cinnamon, Ubuntu Budgie 26.04) | **Yes.** `autoinstall.yaml` at the medium root, `subiquity.autoinstallpath=`, or cloud-init. Zero-touch needs `autoinstall` on the kernel command line, or it asks "Continue with autoinstall? (yes\|no)" (we write the grub.cfg, so we can add it). The desktop app runs a non-interactive wizard: loading, confirm (skipped once installing), install, done. | **Yes, two ways.** (a) `storage: layout: {name: direct, mode: use_gap}` installs into the largest gap. It is in the code (`run_autoinstall_guided`, `validate_layout_mode` accepts `reformat_disk`/`use_gap`) but **not in the reference docs**. (b) A full curtin `storage: config:` with `preserve: true` on the disk, ESP and Windows partitions, and a `format` on a partition AurOS created. curtin "will verify that the partition exists and that the size and flag match". **(b) is the one to use**: nothing is chosen by the installer. | `identity`, `locale`, `keyboard`, `timezone`, `drivers`, `late-commands`, `shutdown` keys all exist | [V] subiquity `doc/tutorial/providing-autoinstall.rst`, `doc/explanation/zero-touch-autoinstall.rst`, `subiquity/server/controllers/storage.py`; `ubuntu-desktop-provision` `installer_wizard.dart`; curtin `doc/topics/storage.rst` |
| **Ubiquity** (Linux Mint 22.x; Zorin 18 reportedly [?]) | **Yes.** Boot parameter `automatic-ubiquity` makes `start-ubiquity-dm` run `ubiquity --automatic`, with a preseed | `partman-auto/init_automatically_partition` has `biggest_free` | via preseed (d-i keys) | [V] `linuxmint/ubiquity` `scripts/start-ubiquity-dm`, `d-i/source/partman-auto/automatically_partition/biggest_free`. A May 2026 Mint-derivative thread could not automate the disk page until it used `--automatic` ([linuxfr](https://linuxfr.org/forums/linux-debian-ubuntu/posts/resolu-mais-surement-perfectible-mint-derivee-et-installation-par-preseed), [2]). A preseed run on Mint 22.3 is untested here [?] |
| **live-installer** (LMDE; **Linux Mint 23, due December 2026**) | **No answer file found.** "automated" in its code means "erase this disk and partition it". | Manual partitioning or erase-disk only | interactive | [V] `linuxmint/live-installer` `installer.py` ("Create partitions on the selected disk (automated installation)"), changelog up to 2026-10-13. Mint 23 replaces Ubiquity with it ([Phoronix](https://www.phoronix.com/news/Mint-23-Alfa), [9to5Linux](https://9to5linux.com/linux-mint-will-adopt-a-longer-development-cycle-starting-with-linux-mint-23), [2]); release target December 2026 ([forums.linuxmint.com t=467436](https://forums.linuxmint.com/viewtopic.php?t=467436), [2]) |
| **Calamares** (Kubuntu, Lubuntu 26.04) | **No.** The feature request for a one-click predefined install is open ([calamares#2229](https://zff.dev/Calamares/calamares/issues/2229), [2]). Kubuntu forum answer: "Calamares does not support this" ([kubuntuforums 680155](https://www.kubuntuforums.net/forum/currently-supported-releases/kubuntu-24-04-nitpick-noble-lts/pre-installation-ay/680155-is-there-any-way-to-automate-the-installation-of-kubuntu-24-04-with-the-new-calamares-installer), [2]) | n/a | n/a | [V] Calamares 3.3.14 in both 26.04.1 manifests |
| **elementary installer** (elementary OS 8) | No answer-file option found [?]. Code search returned nothing | | | |
| **Anaconda** (Fedora) | **Yes, but not from the Workstation Live ISO**: "kickstarts are not supported with live images" ([bug 1027160](https://bugzilla.redhat.com/show_bug.cgi?id=1027160); [pagure fedora-workstation#85](https://www.pagure.io/fedora-workstation/issue/85), won't fix, [P]). Use the netinstall (Everything) image with `inst.ks=` plus a kickstart that installs the Workstation environment from the network | `part --onpart=`, `clearpart --none` | `user`, `lang`, `keyboard`, `timezone` | [V] Anaconda `boot-options.rst` (`inst.ks`, `inst.repo=hd:`); [P] search summaries for the Live restriction |
| ChromeOS Flex | n/a: "dual booting is not supported", and a full install "erases all existing data ... and OS" | | | [P] Google ChromeOS Flex FAQ and install guide via search |

### 3.3 What each installer does to BootOrder and the ESP, and how to stop it

AurOS's rules are: `BootOrder` is written only by `aurfirst confirm`, and the
ESP is written only under `\EFI\AurOS`, only from Windows. An official
installer breaks both, so each case needs a fix.

- **Ubuntu (curtin).** On Debian-family UEFI systems, curtin runs
  `dpkg-reconfigure grub-efi-amd64`, `update-grub`, then
  `/usr/lib/grub/grub-multi-install` **with no arguments**. Its
  `update_nvram: False` adds `--no-nvram` only on the other branch, the plain
  `grub-install` call [V curtin `commands/install_grub.py`
  `gen_uefi_install_commands`]. `grub-multi-install` passes `"$@"` to
  `grub-install` and never reads `grub2/update_nvram` [V `grub-common`
  2.12-1ubuntu7.3]. **So an `ubuntu` boot entry is created and put first,
  and the autoinstall setting cannot stop it.** subiquity also passes
  `reorder_uefi: False` [V `subiquity/models/subiquity.py`]. Without that,
  curtin would push BootCurrent (our staging entry) first.
  **Fix:** `late-commands` restore the BootOrder that AurBridge recorded in
  Windows, set BootNext to the new entry, and run
  `curtin in-target -- debconf-set-selections` with
  `grub-efi-amd64 grub2/update_nvram boolean false`. That template exists,
  default true, and the package's postinst honours it through
  `no_nvram_arg()` [V `grub-efi-amd64` 2.12-1ubuntu7.3 control files]. It
  stops later shim/grub updates from re-promoting Ubuntu before she says
  yes. The ESP gets `\EFI\ubuntu\{shimx64,grubx64,mmx64}.efi` and
  `grub.cfg`. `\EFI\BOOT` is touched only with `force_efi_extra_removable`
  (default off).
- **Ubiquity (Mint 22.x).** `grub-installer` adds `--no-nvram` only on
  `powerpc/chrp_pegasos`. On amd64/efi it creates an entry, and the extra
  removable copy is a low-priority preseed question [V `linuxmint/ubiquity`
  `d-i/source/grub-installer/grub-installer`]. **Fix:**
  `ubiquity/success_command` does the same restore and debconf setting as
  above. Mint's own new installer runs `grub-install --force <device>` with no
  NVRAM control [V `live-installer` `installer.py`].
- **Anaconda (Fedora): the clean one.** Kickstart
  `bootloader --leavebootorder` ("prevents the installer from making changes
  to the existing list of bootable images") [V `pykickstart`
  `commands/bootloader.py`] makes Anaconda use `efibootmgr -C` (create, do not
  add to BootOrder) instead of `-c`. It also skips removing old entries [V
  anaconda `pyanaconda/modules/storage/bootloader/efi.py`]. The ESP gets
  `\EFI\fedora`.
- **The MOK screen.** shim-signed generates a MOK for DKMS modules when Secure
  Boot is on. If one gets queued, shim shows MokManager's blue screen at the
  next boot: exactly the firmware-looking screen this product forbids. Mint's
  installer had to "Remove MOK generated by shim-signed" (2026-04-15) [V
  live-installer changelog]. **Fix:** `drivers: install: false` in
  autoinstall (the default is false) [V reference], and a `late-commands`
  check that `mokutil --list-new` is empty, failing the install otherwise.

### 3.4 "Does it work?" and Put Windows back under Route B

- **Does it work?** The official system has no such question, so a small
  AurOS add-on goes in through `late-commands` / `success_command` / `%post`
  from the staging partition: a first-boot prompt plus `confirm` / `decline` /
  `putback`. It is software the person installs on her own machine, which
  Canonical's policy allows ("You can modify Ubuntu for personal ... use"). We
  do not redistribute anything modified, because her PC downloads the ISO from
  the publisher. Give it AurOS's name, not the distro's. After "yes" it should
  shrink to a removable "Put Windows back" app.
- **Put Windows back** still works. AurOS made the partitions (3.2, option b),
  so it knows exactly what to delete. Added to the restore: delete the
  distro's ESP directory (`\EFI\ubuntu` or `\EFI\fedora`, from Windows, the
  only side that writes the ESP) and its NVRAM entry, then extend C:. Refuse
  up front if `\EFI\ubuntu` / `\EFI\fedora` already exists: it belongs to
  someone else.
- **Power cut during the official install.** This is the new failure window.
  The installer formats and writes for several minutes after the restart,
  and it adds its NVRAM entry near the end, before `late-commands` run. A cut
  between the two leaves a first-place `ubuntu` entry pointing at a
  nearly-installed system. That needs a `powercuttest` case and a staging-side
  repair: if the journal says "official install not finished", restore the
  recorded BootOrder and offer retry or put-back.

### 3.5 Verification on the Windows side

- **Ubuntu:** `SHA256SUMS` + detached `SHA256SUMS.gpg`. Today's 26.04 file is
  signed by key fingerprint `8439 38DF 228D 22F7 B374 2BC0 D94A A3F0 EFE2 1092`
  [V `gpg --list-packets`]. Pin that fingerprint in the installer. 26.04.1
  desktop: `601e30fb...c55c` [V].
- **Mint** publishes `sha256sum.txt` (+ `.gpg`); **Fedora** publishes a
  `CHECKSUM` file signed with the release key. I could not reach either from
  here [?].
- The installer needs a small OpenPGP verifier (Ed25519/RSA detached
  signatures, no keyring UI). The repo has none today.

### 3.6 Sizes and time

| System | Download | Source | Install after the restart (estimate, **not measured**) |
|---|---|---|---|
| AurOS desktop image (Route A) | 5,333,057,536 B | [V] HANDOFF.md | ~1 min (D1 fast path) |
| Ubuntu 26.04.1 desktop | 6.0 GiB | [V] releases.ubuntu.com listing | 5-10 min SSD, much longer on HDD |
| Ubuntu 24.04.5.1 desktop | 5.8 GiB | [V] | same |
| Xubuntu / Ubuntu Cinnamon / Ubuntu Budgie 26.04.1 | 4.8 / 5.3 / 3.8 GiB | [V] cdimage listing | 5-10 min SSD |
| Kubuntu / Lubuntu 26.04.1 | 4.7 / 3.7 GiB | [V] | (not automatable) |
| Linux Mint 22.3 Cinnamon (2026-01-08/13) | ~2.9 GB | [2] [lecrabeinfo](https://lecrabeinfo.net/telecharger/linux-mint-22-3-cinnamon/) | 6-12 min SSD |
| Zorin OS 18 Core | ~3.6 GB | [2] computingforgeeks | [?] |
| Fedora Workstation 45 Live | ~2.8 GB (beta) | [2] ostechnix/computingforgeeks; F45 targeted 2026-10-20, fallbacks 10-27 and 11-03, **release not confirmed** | Not usable for kickstart. Netinstall (~1 GB [?]) plus ~2-3 GB of packages during install: 15-30 min [?] |

At 100 Mbit/s, 6 GiB is about 8.5 minutes. **Route B makes the 10-minute goal
depend on her connection more than Route A does, because the install cannot
start until the restart.** Mint is the best fit for the time bar.

### 3.7 Route B verdict

It is feasible today, by documented, source-verified mechanisms, for Ubuntu
26.04 and its three subiquity flavours, and for Fedora (via netinstall). It is
feasible for Mint 22.3 through Ubiquity preseed, unverified on a real Mint ISO,
with a deadline: Mint 23 changes installers in December 2026. It is not
feasible without modifying the installer for Kubuntu, Lubuntu, elementary or
Mint 23. It is not possible for Pop!_OS with Secure Boot on, or for ChromeOS
Flex beside Windows.

---

## 4. Prior art, and what broke for each

| Product | How it worked | What broke / lesson | Source |
|---|---|---|---|
| **Wubi** (Ubuntu, 2008-2013) | Windows program: downloads and checks the ISO, extracts kernel+initrd, adds an entry to the **Windows boot loader**, writes a preseed, allocates a **loop file on NTFS**; "the actual installation is performed within Linux after rebooting" | Dropped from 13.04: "has not been updated to work with Windows 8", "nearly unusable on Windows 8", bugs like "13.04 installer doesn't create user account", "fails to detect 12.04.2 and 13.04 AMD64 ISO", and nobody to maintain it. **Lessons: a Windows-side installer for someone else's ISO breaks whenever either side changes, and needs an owner who tests every release. Root-on-NTFS-loop was fragile.** | [P] [ubuntu-devel, Graber, 2013-04-02](https://lists.ubuntu.com/archives/ubuntu-devel/2013-April/037049.html) (read in full); [P] 13.04 release notes via search |
| **mint4win** (Mint's Wubi fork) | Same code (`src/wubi/...`, `data/grub.install.cfg` with `automatic-ubiquity`) | Abandoned with Wubi | [V] `linuxmint/mint4win` exists, files found by code search |
| **WubiUEFI** (hakuna-m, active: repo updated 2026-10-19, 233 open issues) | Wubi for UEFI. Signs its own boot loader with its own key | Under Secure Boot its loader "is signed by an unknown signature. So either disable Secure Boot ... or import the key" (MOK). **Fails the no-firmware-screen bar. Reusing a distro's Microsoft-signed shim is what avoids this.** | [V] wiki `FAQ.md`, README |
| **Debian win32-loader** | Windows program that edits the Windows boot loader to start the Debian installer | 2022: dropped from Debian CDs and live images. Its trick "quite unlikely to work on modern (Secure Boot?) machines"; orphaned | [P] [debian-cd 2022-07/08](https://lists.debian.org/debian-cd/2022/08/msg00028.html) via search |
| **UNetbootin** "hard disk" mode | Copies boot files to C:, adds a Windows boot entry | BIOS-era. UEFI complaints, last release Feb 2021 | [2] AlternativeTo / forums |
| **Ventoy** | Boots ISOs from files; own key | Needs a one-time MokManager "Enroll key" under Secure Boot; some distro grubs then refuse it | [2] [Ventoy FAQ #3536](https://github.com/ventoy/Ventoy/issues/3536) |
| **Fedora Media Writer**, **Chromebook Recovery Utility** (ChromeOS Flex), Rufus | Make a USB stick | USB only. Flex then wipes the whole disk | [2]/[P] |
| **Ubuntu's own OEM "reset partition"** | Installer contents on an internal FAT32 partition, bootable from GRUB/EFI | Proof that Canonical supports running its installer from the internal disk | [V] autoinstall reference |
| **linwin** (2026-10-06, 0 stars) | The mirror image: installs Windows from Linux, unattended, no USB | Shows the idea is alive; nothing to learn yet | [V] GitHub search |

No maintained 2025-26 tool installs an official Linux from Windows beside it
with Secure Boot on and no MOK screen: the search found blog workarounds only
[2]. AurOS would be the first. That is also the warning from Wubi: it needs an
owner and CI for every point release.

---

## 5. Recommendation

### 5.1 Route

**Route B, inside AurOS's machinery ("AurOS prepares and guards; the
publisher's installer installs").**

1. In Windows, during the download: verify SHA256SUMS + GPG; shrink C:;
   write the staging partition (raw ISO or FAT32 copy) into the tail of the
   free space; record BootOrder, Wi-Fi, locale, keyboard, timezone and user
   name.
2. Staging commits the GPT in one sector with the target partitions created
   but empty, then starts the official kernel through `\EFI\AurOS` (Ubuntu
   family) or `\EFI\AurOS-fedora` (Fedora's own shim).
3. The official installer formats and fills only those partitions (curtin
   `preserve: true` + `format`; kickstart `--onpart`; Ubiquity preseed), then
   AurOS's post-install hook restores BootOrder, sets BootNext, pins
   `update_nvram` off, checks no MOK is queued, and adds the small "does it
   work? / Put Windows back" helper.
4. Yes / no / put back work as today.

Route A stays for AurOS's own styles only (phase 1), named "AurOS".

### 5.2 First systems, in order

1. **Ubuntu 26.04 LTS** (6.0 GiB). The best-documented automation, verified
   in source.
2. **Xubuntu, Ubuntu Cinnamon, Ubuntu Budgie 26.04 LTS**: the same subiquity
   path, nearly free once 1 works.
3. **Linux Mint 22.3 Cinnamon** (~2.9 GB, supported to 2029): Ubiquity
   preseed. Pin to 22.x. Mint 23 (Dec 2026, live-installer) needs either an
   upstream automation mode or stays out.
4. **Fedora Workstation** (netinstall + kickstart): its own boot chain, the
   cleanest boot-order handling, and the only option for 2023-only PCs.
5. Candidate after a spike: **Zorin OS 18 Core** (confirm the installer, the
   shim and that preseed works; non-commercial brand terms).

### 5.3 Names she can be shown

Plain-text product names as descriptions of the unmodified image the
installer downloads from the publisher, never inside our product's name, no
logos until permission arrives in writing, and a footer such as "Ubuntu is a
trademark of Canonical Ltd. Linux Mint ... Fedora is a trademark of Red Hat,
Inc. AurOS is not affiliated with or endorsed by them."

- "Ubuntu 26.04 LTS", "Xubuntu 26.04 LTS", "Ubuntu Cinnamon 26.04 LTS",
  "Ubuntu Budgie 26.04 LTS". Unmodified redistribution is allowed and we do
  not even redistribute. Anything "...buntu" in our own naming, URLs or
  merchandise needs permission (§4).
- "Linux Mint 22.3 Cinnamon". Commercial use needs Clem's permission.
- "Fedora® Workstation 45", with the attribution statement, for unmodified
  media ([docs.fedoraproject.org/legal/trademarks](https://docs.fedoraproject.org/ga/legal/trademarks/), [P]).
- "Zorin OS 18 Core", only while AurOS is non-commercial, with the
  registered-marks disclosure. Never offer Pro.
- AurOS's own images: "AurOS (based on Ubuntu)". Desktops are named as
  software ("with the KDE Plasma desktop"), never "Kubuntu" or "Mint
  Edition".

### 5.4 Main risks

1. **Secure Boot coverage.** Every Ubuntu-family ISO is 2011-only, including
   26.10 (2026-10-13). Only Fedora's shim adds 2023. The picker must hide the
   systems a PC's `db` cannot boot (preflight already reads `db`).
2. **Installer drift.** `mode: use_gap` is undocumented (use explicit curtin
   config instead). Mint swaps installers in Dec 2026. Every point release
   needs an OVMF run in CI (Wubi's failure mode).
3. **Boot order and the ESP.** Ubuntu's curtin path cannot be told not to
   write NVRAM (3.3). A power cut between that write and our fix-up boots a
   half-finished system first.
4. **MokManager's blue screen** if anything queues a MOK.
5. **Time.** Download plus post-restart install will exceed 10 minutes on
   many connections. Mint is the smallest.
6. **Legal.** Nominative names are low risk, logos are not. The Canonical
   "recompile" clause is an open question for AurOS's own image today (2.3).
   Ask Canonical, Mint and Zorin before launch.
7. **Unverified here:** Mint/Zorin/elementary shims, Mint's preseed on 22.3,
   Fedora's grub `loopback`, ntfs3 in casper, BitLocker plus subiquity
   automation on a real Windows 11 disk.

### 5.5 Engineering plan

| Phase | Work | Done when |
|---|---|---|
| **0. Letters (owner, 1 day)** | Write to Canonical (IP policy contact), Linux Mint (Clem) and Zorin: "a free Windows installer that downloads your unmodified ISO from your servers and installs it beside Windows; may we show your name / logo?" Ask Canonical about the AurOS image itself. | Written answers on file |
| **1. Ubuntu spike (≈1 week)** | `tools/officialtest.sh`: OVMF + MS keys + SB on + a BitLocker'd Windows disk. Staging partition (raw ISO), grub.cfg with `autoinstall`, autoinstall with curtin `config` (`preserve: true`), identity/locale/keyboard/timezone from Windows, `late-commands` for BootOrder/BootNext/`update_nvram`/MOK check/helper. Prove each check fails when its thing is broken. | Installs, boots via BootNext, "no" returns to Windows, put-back leaves the disk as before (hash the ESP and the GPT before and after) |
| **2. Product (≈2 weeks)** | A `systems` manifest (name, version, ISO URL, SUMS + signature URL, pinned key fingerprint, installer kind, size, ESP dir, legal line, which CA it needs). OpenPGP verifier in AurBridge. Picker screen (text plus our own screenshots). Power-cut cases for the new window. Xubuntu/Cinnamon/Budgie through the same path. | `windows.yml` e2e passes for Ubuntu, `powercuttest` covers the official-install window |
| **3. Mint 22.3 (≈1 week)** | Preseed: `automatic-ubiquity`, partman on the given partitions, `success_command`. Check Mint's ISO shim/grub and signing key. Decide on Mint 23: contribute an automation mode to `linuxmint/live-installer` (GPL, active) or keep 22.x. | Same tests as phase 1 |
| **4. Fedora (≈2 weeks)** | Copy Fedora's shim+grub to `\EFI\AurOS-fedora` from Windows. Netinstall + `inst.ks=` + `bootloader --leavebootorder` + `part --onpart` + `%post` helper RPM. A test VARS with no 2011 CA (`every-machine` section 1) to prove it is the one that boots there. | Boots and installs on 2011-only and on 2023-only OVMF |
| **5. Later** | Zorin spike. "Ubuntu with KDE Plasma" via autoinstall `packages: [kubuntu-desktop]` as a Kubuntu alternative (named so, not "Kubuntu", until Canonical says otherwise). | |

---

## 6. What I could not confirm

- The shim/grub inside current Linux Mint 22.3, Zorin OS 18.x and elementary
  OS 8.x ISOs (mirrors blocked from this session).
- Whether Zorin 18 still uses Ubiquity, and whether Ubiquity's automatic mode
  works end to end on Mint 22.3 / Zorin 18.
- Whether Fedora 45 has shipped (target 2026-10-20, fallbacks 10-27 / 11-03)
  and which shim build its media carry. Whether Fedora's signed grub has
  `loopback` built in. Which filesystems `inst.repo=hd:` accepts.
- Whether an initramfs `mount -t ntfs` reaches ntfs3 on 26.04's kernel for
  casper `iso-scan`.
- How subiquity's autoinstall behaves with a BitLocker partition on the disk.
  The `use_gap`/`config` paths I read do not consult the BitLocker check that
  the interactive UI uses (`has_bitlocker_GET`), but I did not run it.
- A formal Linux Mint trademark policy (only blog/forum statements found), and
  the terms of `packages.linuxmint.com`.
- Real install times. All "after the restart" times above are estimates.
