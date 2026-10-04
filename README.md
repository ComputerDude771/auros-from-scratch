# AurOS desktop image, in pieces

This branch holds no source code. It is the AurOS `desktop` image,
compressed with gzip and cut into pieces under GitHub's 100 MB file
limit, so that the no-stick AurBridge installer can download it.

| | |
|---|---|
| image | `auros-desktop.img`, 5,314,183,168 bytes |
| image SHA-256 | `ba0ccded062d80bbf50784b22e28ad5d04718c7beb0f9f3b361e853aaa76d086` |
| compressed SHA-256 | `279fe4f6722903a0d985dc26da11b69fe7f4c290c1c9f3f0d0dcd01b4ae78fc9` |
| pieces | `pieces.txt`: name, size and SHA-256 of each |

Rebuild it yourself with `cat auros-desktop.img.gz.* | gunzip > auros-desktop.img`
and check the SHA-256 above. The installer does the same thing, checking
each piece and then the whole image before it uses any of it.

Deleting this branch removes the image from the installer's reach; any
installer built against it will then refuse, before changing anything,
that it cannot download AurOS.

## v2: the installer's choices, and a real locale

`v2/` holds a newer build of the same image: first boot applies the
language, keyboard, time zone, look and desktop chosen in the installer
(`/usr/lib/auros/choices.sh`), and the image has the `locales` package,
so its locale is actually generated. The pieces at the top level are
v1, kept so that installers built against them keep working.

| | |
|---|---|
| image | `auros-desktop.img`, 5,325,717,504 bytes |
| image SHA-256 | `c361d06d700f90feb9628fd3b486b07eb23e1991aa751605409b163f6ba5a45a` |
| compressed SHA-256 | `1c93135eb1ad402299d05add705f2da485f25360bd13d0ea0d7753ea5bc328a1` |
| pieces | `v2/pieces.txt` |

## v3: Put Windows back, and security updates

`v3/` holds the next build of the same image, from branch
`claude/confident-johnson-hxevk4`:

- "Put Windows back": a row in Settings, and an entry in AurOS's own
  start-up menu, that restart into the installer's restore
  (`tools/putbacktest.sh`, 16/16, with Secure Boot on);
- the `aurfirst` fix for a second install after Windows was put back
  (`2d85777`);
- Ubuntu's security updates, installed daily by `unattended-upgrades`,
  except the kernel, grub and shim (they would regenerate AurOS's
  start-up menu; see `docs/issues/v3/STATUS.md`, 7.4).

Replaced once, on 2026-09-26, before any installer used it: an
independent review found that "Put Windows back" could be triggered by
a double-click and that a kernel update would drop AurOS's menu. The
pieces here are the fixed build.

| | |
|---|---|
| image | `auros-desktop.img`, 5,325,717,504 bytes |
| image SHA-256 | `5dee5f443b8129387764e0547a41e9798f8d931b109b8102e79d6deb5afef5ee` |
| compressed SHA-256 | `f7925ad05005fb91e59c62501709176d45a47858f6fda691aa255579a12e0ed5` |
| pieces | `v3/pieces.txt` |

## v4: updates that keep it starting, and a screen for the one restart

`v4/` holds the next build of the same image, from branch
`claude/admiring-shannon-xm1g0e`, commit `8593f7f`:

- **The kernel, grub and shim take security updates** like everything
  else. `update-grub` and `grub-install` are AurOS's own
  (`rootfs/usr/lib/auros/bootchain`): a kernel update rewrites AurOS's
  start-up menu with Put Windows back still in it and the previous kernel
  as an automatic fallback; a new shim and grub go on AurOS's EFI
  partition only after checking -- the way the firmware and shim do --
  that this PC will start them and they will start everything it needs,
  including the Put Windows back kernel on Windows' partition. The
  firmware's boot order is never written.
- **The start-up menu no longer prints "error: prohibited by secure boot
  policy"**, and grub's fallback to the previous kernel works.
- **The installer's restart has a screen**: seven steps, a bar, and "Do
  not turn the computer off" from the first change that cannot be undone.

| | |
|---|---|
| image | `auros-desktop.img`, 5,333,057,536 bytes |
| image SHA-256 | `b36100653b1b3dd339c6209cdd60ab169706717f512f2bb5dee0273dd404da41` |
| compressed SHA-256 | `6aca4ea259c9792f8cb59c4ae02697775909dc2ce2a8eaea2e6ac4f62b8d19fa` |
| pieces | `v4/pieces.txt` |

## The test installer

`AurOS-Installer-test.exe` is the unsigned no-stick test build of the
AurBridge installer that downloads the v4 pieces above. It is for spare
PCs and virtual machines only; read `docs/TRY-IT.md` on the development
branch before running it.

| | |
|---|---|
| SHA-256 | `ef903efd9cdeb10e15e52b3ce9e4eac83788f3396ac9f029ed04f2cfca1c5891` |
| downloads | the v4 image (`v4/pieces.txt`) |
| built from | branch `claude/admiring-shannon-xm1g0e`, commit `8593f7f` |
| tested in simulation | on this build: `installtest` 37/37, `nosticktest` 45/45, `bootupdatetest` 27/27, `putbacktest` 16/16, `choicesboottest` 9/9, `firstboottest` 32/32, `bootchaintest` 72/72, `screentest` 24/24, `manifesttest` 9/9; the installer's own downloader fetched and checked every v4 piece (`verdict=ok`) before this was pushed |
| tested on real Windows | `.github/workflows/windows.yml` on every push of the source branch; after publishing, it downloads this file from here and the v4 image with the installer's own code |
| reviewed | two independent reviews of the boot chain; every finding fixed (`docs/issues/v3/STATUS.md`) |
| not tested | a real PC's disk being resized and written |

New in this build (v4): **the restart has a screen** -- seven steps, a
bar, and "Do not turn the computer off" from the first change that cannot
be undone; a stop says "AurOS was not installed" with the `verdict=` line
kept on screen for a photo -- and **the AurOS it installs keeps itself
up to date**, kernel, grub and shim included, without losing Put Windows
back.

From the build before it: **it fixes what it finds.** The stop page lists each
problem in one line with what happens about it, and *Fix these for me*
does it: frees space on C:, switches BitLocker off, switches Fast
Startup off, restarts once for waiting updates and opens again by
itself. Charger, battery and USB drives need no button: it re-checks
every few seconds and carries on. The window now fits the screen, so
the page and its buttons can always be reached. It installs the AurOS
that was ordered, with Windows' language, keyboard and time zone; there
are no pages to pick a look or a desktop.

What it cannot fix it says in one line and leaves alone: a PC that
starts the old way (BIOS), a failing drive, some multi-drive setups.

**Leave Secure Boot on.** The restart goes through Microsoft-signed
shim; before changing anything the installer reads the firmware's list
of trusted keys and, on the rare PC that would refuse shim (some
Secured-core laptops), stops and shows the one setting to switch on. It
writes nothing outside `\EFI\AurOS`, so a PC that also has Ubuntu is
fine.

**Antivirus.** Avast and AVG hold unsigned new programs for a few hours
("Suspicious file detected", then "This needs a closer look"). Add an
exception for the file, or wait. `docs/TRY-IT.md` has the steps.

Earlier test installers:

- `04089423...` (2026-09-29, v3 image): fixes what it finds; the
  restart showed console text, and the installed AurOS took no kernel,
  grub or shim updates.
- `c70045fb...` (2026-09-26, v3 image): described problems instead of
  fixing them, and on a small screen its bottom was off the screen.
- `f971559d...` (2026-09-26, v2 image): the first that starts on
  Windows. Works; installs the v2 image, which has no "Put Windows back".
- `3e32e929...` (2026-09-25, v2): **does not start on Windows.** Its
  manifest had `--` inside an XML comment; Windows refuses it with "The
  application has failed to start because its side-by-side
  configuration is incorrect". Wine did not, so every test passed. Do
  not use it.
- `540a0bf2...` (v2, choices kept; wrote a file in `\EFI\ubuntu` and
  refused PCs with Ubuntu).
- `005fe4d8...` (v1, ignored the choices).
