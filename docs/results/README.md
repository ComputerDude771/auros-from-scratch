# What actually ran

These are transcripts, not claims. Each is the output of the named
tool on the commit that added it here, kept so that a later reader can
see what "green" meant rather than take somebody's word for it.

| file | what it is | run it yourself |
|---|---|---|
| `roundtrip.txt` | install AurOS on a synthetic machine, start it, put Windows back from the stick, put it back again from the copy on the computer with no stick, refuse a damaged copy. 32 checks. | `sudo sh tools/installtest.sh` |
| `nostick.txt` | the no-stick mode: AurBridge's phase engine arms the machine with the image inside its NTFS volume; the staging environment installs with one disk and nothing else plugged in, keeps the way back on the disk, puts Windows back from it; both refusals leave the disk byte-for-byte unchanged; and the restart from firmware with Secure Boot on goes through shim and grub, with the disk's serial deliberately reported differently on each side. 31 checks. | `sudo sh tools/nosticktest.sh` |
| `choicesboot.txt` | the real desktop image, started from firmware for the first time with a `choices.conf` on a second disk's EFI partition: it comes up Spanish (the locale generated, not just named), with a Spanish keyboard from Windows' KLID, Paris time from Windows' zone name, the Moss look and the taskbar desktop, all recorded so Ferry leaves them alone. 9 checks. | `sudo sh tools/choicesboottest.sh` |
| `loader.txt` | **with the power off and the stick out, does it come back up in AurOS?** Install, then start the machine from firmware with no `-kernel` and no memory stick — and then again on `OVMF_CODE_4M.ms.fd` with Microsoft's keys enrolled and Secure Boot enforcing. Also: the machine's own EFI partition, byte for byte, unchanged. 17 checks. | `sudo sh tools/loadertest.sh` |
| `powercut.txt` | the power goes at each of seventeen named instants — twelve during the install, five during the restore — and Windows comes back with every file byte-identical. 138 checks. | `sudo AURSTAGE_FAULT=1 ./build/staging && sudo sh tools/powercuttest.sh` |
| `matrix.txt` | ten shapes of computer through the dry run, which writes nothing: 4Kn, an OEM gigabyte EFI partition, a first partition at LBA 34, MBR, BIOS, BitLocker, a hibernated Windows, two Windows volumes, no room. | `sudo sh tools/matrixtest.sh` |
| `images.md`, `*.json` | the five profile images, their digests, and what each one cost to build. | `sudo ./build/all` |
| `build-all.txt` | the transcript of the run that made them, interrupted twice by bugs it found in forge and resumed both times. | |

## What these do not prove

Written here rather than left for somebody to discover.

**One firmware.** OVMF is not an Insyde H2O from 2014, and firmware
variety is the one thing on this list that cannot be synthesised at
all. The mitigation is the dry run: it writes nothing, prints one
machine-readable line, and is worth shipping on its own to build a
hardware matrix before anyone's disk is at risk.

**One virtual disk.** A real drive that lies about having flushed is a
property of that drive. What the power-cut matrix tests is the ORDER
writes reach the device and that each commit is one sector, which is
the property the design rests on.

**Nothing about a real power supply**, a failing cell, or a laptop
whose battery disconnects under load. R6's answer to those is the AC
gate and the refusal on battery, not this.

**The Windows half is not in any of these.** `src/aurbridge/plat_win.c`
cannot run here — there is no Windows, and wine has no raw disk
handles, no volume dismount and no firmware variables. What is tested
is the phase engine above it, against a computer made of files
(`plat_sim.c`), and the bytes it produces against the programs that
read them (`tools/bridgetest.sh`). `plat_win.c` meets a real machine
for the first time on the first machine it meets.

That now includes the download. `plat_fetch` exists twice — WinHTTP on
Windows, hand-written HTTP in the simulation — and only the second one
is exercised, by `tools/exetest.sh` against a server it starts itself.
What the test covers is the logic the two share and the part that
actually goes wrong: a server that ignores `Range` and answers 200
where a 206 was asked for. What it does not cover is WinHTTP.

**Nothing here has been on a network that allows Firefox.** This
builder's egress policy answers `403` for every Mozilla and Launchpad
host, so the three routes `build/forge` now tries are all unexercised
and the five images in `images.md` ship Epiphany. The build refuses to
substitute quietly now, which is the check that was missing when they
were made — but a refusal is not the same as a Firefox.
