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
- Ubuntu's security updates, installed daily by `unattended-upgrades`.

| | |
|---|---|
| image | `auros-desktop.img`, 5,325,717,504 bytes |
| image SHA-256 | `8ee78e9a671e860fdfadd809ddc3f53790f5930f2fc457a0171246f4d4431999` |
| compressed SHA-256 | `a5a8b373201aa425102ab6e6de1a8d9d7ad468d788ce1336950626a862d914b0` |
| pieces | `v3/pieces.txt` |

## The test installer

`AurOS-Installer-test.exe` is the unsigned no-stick test build of the
AurBridge installer that downloads the pieces above. It is for spare
PCs and virtual machines only; read `docs/TRY-IT.md` on the development
branch before running it.

| | |
|---|---|
| SHA-256 | `f971559d2edc03710bbcde0ad1f43270deedcbd184d82378dc1f2e188c724015` |
| downloads | the v2 image (`v2/pieces.txt`) |
| built from | branch `claude/confident-johnson-hxevk4`, commit `3775c3d` |
| tested **on real Windows** | GitHub Actions `windows-latest` (`.github/workflows/windows.yml`): the wizard starts the way a double-click starts it, passes its navigation test, and runs preflight read-only against the machine; the console tool's selftest and preflight; WinHTTP downloads all 20 v2 pieces and the installer's own inflater checks the image (`verdict=ok`); and the previous build, below, fails to start there exactly as it did on the first real PC. After publishing, the same workflow downloads this file from here, checks the SHA-256 above, and starts it. |
| tested in simulation | `tools/installtest.sh` 37/37, `tools/nosticktest.sh` 45/45, `tools/manifesttest.sh` 10/10 on this file, `tools/aurfirsttest.sh` 65/65, `tools/choicestest.sh` 21/21, Ferry 43/43 |
| not tested | a real PC's disk being resized and written: that is what a spare PC is for |

It keeps what is chosen on its *Make it yours* page: AurOS starts in
that language, keyboard layout, time zone, look and desktop.

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

- `3e32e929...` (2026-09-25, v2): **does not start on Windows.** Its
  manifest had `--` inside an XML comment; Windows refuses it with "The
  application has failed to start because its side-by-side
  configuration is incorrect". Wine did not, so every test passed. Do
  not use it.
- `540a0bf2...` (v2, choices kept; wrote a file in `\EFI\ubuntu` and
  refused PCs with Ubuntu).
- `005fe4d8...` (v1, ignored the choices).
