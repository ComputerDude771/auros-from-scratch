# What actually ran

These are transcripts, not claims. Each is the output of the named
tool on the commit that added it here, kept so that a later reader can
see what "green" meant rather than take somebody's word for it.

| file | what it is | run it yourself |
|---|---|---|
| `roundtrip.txt` | install AurOS on a synthetic machine, start it, put Windows back from the stick, put it back again from the copy on the computer with no stick, refuse a damaged copy. 32 checks. | `sudo sh tools/installtest.sh` |
| `powercut.txt` | the power goes at each of fourteen named instants — nine during the install, five during the restore — and Windows comes back with every file byte-identical. 111 checks. | `sudo AURSTAGE_FAULT=1 ./build/staging && sudo sh tools/powercuttest.sh` |
| `matrix.txt` | ten shapes of computer through the dry run, which writes nothing: 4Kn, an OEM gigabyte EFI partition, a first partition at LBA 34, MBR, BIOS, BitLocker, a hibernated Windows, two Windows volumes, no room. | `sudo sh tools/matrixtest.sh` |

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
