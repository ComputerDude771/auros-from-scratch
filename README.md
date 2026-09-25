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

## The test installer

`AurOS-Installer-test.exe` is the unsigned no-stick test build of the
AurBridge installer that downloads the pieces above. It is for spare
PCs and virtual machines only; read `docs/TRY-IT.md` on the development
branch before running it.

| | |
|---|---|
| SHA-256 | `3e32e929f57c5c2673f4547a09b44c827743d191120d22a57dfc6f28a4f3e875` |
| downloads | the v2 image (`v2/pieces.txt`) |
| built from | branch `claude/laughing-cray-ayao6i`, commit `cd437e1` |
| tested | `tools/installtest.sh` 37/37, `tools/nosticktest.sh` 45/45, `tools/choicesboottest.sh` 9/9; the same piece list downloaded and checked from here under Wine |

It keeps what is chosen on its *Make it yours* page: AurOS starts in
that language, keyboard layout, time zone, look and desktop.

**Leave Secure Boot on.** The restart goes through Microsoft-signed
shim; before changing anything the installer reads the firmware's list
of trusted keys and, on the rare PC that would refuse shim (some
Secured-core laptops), stops and shows the one setting to switch on. It
writes nothing outside `\EFI\AurOS`, so a PC that also has Ubuntu is
fine.

Earlier test installers: `540a0bf2...` (v2, choices kept; wrote a file
in `\EFI\ubuntu` and refused PCs with Ubuntu), `005fe4d8...` (v1, ignored
the choices).
