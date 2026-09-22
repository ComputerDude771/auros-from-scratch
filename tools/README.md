# tools — proofs, not demos

Each of these exists because a change here was too easy to get subtly
wrong to accept on inspection alone. They are meant to be run, and they
exit non-zero when they fail.

| | What it proves |
|---|---|
| `recip_proof.c` | The blur's multiply-and-shift replacement for integer division is **exact** — checked over every (window, sum) pair a blur can produce, all 4.2 million of them, for radius 1..128. Not a spot check. |
| `blur_equiv.c` | The rewritten blur matches the one it replaced pixel for pixel, on adversarial full-contrast noise across 9 radii × 8 region shapes including off-screen, 1×1 and 1-pixel-wide slivers. Also times both. |
| `kerning.c` | How much kerning the engine actually applies, per font. The answer used to be "none" for almost every modern typeface. |
| `contrast.c` | Can the person this is for actually read it? WCAG relative-luminance contrast for every meaningful colour pair, in every theme. A floor, not a target: clearing it does not make a design good, failing it makes one unusable. |
| `contactsheet.c` | All six archetypes for one theme, in one image, at a real panel size. A design decision is not judged one screen at a time — what separates a system from a look is whether it survives six different interaction models. |
| `modaltest.c` | A panel is modal, and stays modal. **Only one covers the desktop at a time** (every pair, both orders). **Nothing behind it answers** — main.c is read as text and every guard must ask `SHELL_PANEL_OPEN`, because four hand-written lists of "which panels are open" had drifted to three, four, three and two entries. **There is a way out without a mouse** — Escape, and the keyboard's choice of power action must be the same one a click on that rectangle makes. The macro's own coverage is taken from `shell.h` rather than from a list in the test: every `int something_open;` in the context struct must be named by `SHELL_PANEL_OPEN`, the macro must name nothing the struct has not, and the count must match the test's own — so a seventh panel fails this by name instead of being tested by six. |
| `notifytest.c` | The notification server, driven over a **real session bus started by the test** with real clients — `gdbus` for well-formed calls, and **raw libdbus for malformed ones, because `gdbus` introspects first and refuses to send them** (the check that used to use it would have passed against a server with no type checking at all). It asserts the **text on the card**, not just the count: `Saved <report 2024>.pdf` must survive, `<b>` must not, a long line in a non-Latin script must never be cut through a character. It proves what a sending program may decide (that its message looks urgent) and may not (that it never goes away, that it pushes the battery warning off the screen, that it takes an id already in use, that it puts up a card too small a screen cannot draw). Every malformed call must be **answered** — one that is silently dropped hangs the caller for libdbus's 25-second default. |
| `bttest.c` | What `bluetoothctl` prints becomes what she reads. The "Connected" and "Used before" tags come from two flags **nothing in the program ever set** — the function that reads them was written and called from nowhere. Also: a device that has never announced a name prints its address twice, and a list of six rows reading `FC-58-FA-21-03-9C` is a list she cannot choose from. |
| `welcometest.c` | **Phase 9: the only place anybody can say AurOS works, or say no to it.** It appears only while the question is open — not on a machine that answered yesterday, not on one AurOS was installed onto directly — and each button asks for exactly one word. The checks that matter are the negative ones: escape and a press on the background close it or do nothing and **answer nothing**; a second press while a request is in flight does not replace it; a result file caught half written is not an answer; and one press leaves exactly one new file behind, which is the request. The panel's entire privilege is writing that word into its own user's runtime directory. |
| `wlhostile.c` | Fifteen clients that misbehave on purpose, each the shortest sequence that used to break something, against a real compositor. It **measures** as well as survives: a case fails if one dispatch takes over 300ms or peak memory grows by more than 64MB, because a compositor frozen for nine seconds used to pass the old "is it still serving?" question — the freeze was over by the time it was asked. Two cases invert the assertion: the point of *copying to the clipboard twice* is that the CLIENT must live, and the harness focuses the window first, because an offer only ever reaches the focused client and without that the case tested nothing. |
| `stagetest.sh` | The staging environment — where the one restart lands and where every destructive step will happen. Builds a synthetic machine (ESP + a **real NTFS filesystem**, made by the image's own `mkntfs` + an ext4 root), boots the initramfs in QEMU, and **hashes the whole disk before and after every case**, including both abort paths. `docs/AURBRIDGE.md` says an abort must leave the machine bootable into Windows, "tested, not a goal" — the hash is what makes that a fact rather than an intention. |
| `installtest.sh` | **The whole product, both directions, on one synthetic machine.** A 3 GiB disk with an EFI partition, a real NTFS Windows holding seven files with recorded md5sums, and an OEM recovery partition at the far end so the space a shrink creates is a *gap* and not room at the end. It installs for real — shrink, write by offset, verify, probe, one-sector commit, grow, `switch_root` — and the new system prints from inside itself. Then it puts Windows back, twice: once from the memory stick and once from the copy on the computer with no stick plugged in, asserting each time *which* copy was used. Afterwards: the three original partitions at their original sectors, the EFI partition byte-for-byte, the Windows **filesystem** its full size again (not merely the partition entry — checking only the table let a restore pass with the filesystem still at 16408 sectors out of 2097151), and every file identical to the md5sums taken before anything was touched. Every refusal case hashes the whole disk and requires it byte-for-byte unchanged. |
| `powercuttest.sh` | **R4 and R6, made to happen.** The installer names its own dangerous instants (`src/aurstage/fault.h`) and a fault-injection build stops dead at exactly one of them, so every run is the same run and a failure is reproducible by its name — which killing a VM at a wall-clock moment is not. Fourteen instants, nine during the install and five during the restore; the restore half matters more, because a machine running it is already one something has gone wrong with. Above the line the disk must be byte-for-byte untouched; below it the user's files must survive the cut *before* any restore is attempted, and then the restore must bring the machine back. What it does **not** prove is in its own header: one firmware, one virtual disk's cache behaviour, nothing about a real power supply. |
| `exetest.sh` · `pe_payload.py` | **One file somebody double-clicks.** It was three, and the message a person got for not knowing that was "the copy of AurOS to install could not be found" — a true sentence about a mistake we made. The staging environment now lives inside the executable as a PE resource, and whether it is *really* in there is a question about the resource directory, answered by a second implementation in python that never asks the program that wrote it. It also checks the interaction that decided the mechanism: **signing must not disturb the payload**, which is only true because a resource is inside the image Authenticode covers and appended bytes are not. Then the download, against a server it controls: a whole transfer, a resumed one, a **server that ignores `Range`** and answers 200 where a 206 was asked for (whose first byte would otherwise land four gigabytes into the file), and a connection cut partway — where what arrived must be *kept*, because a five gigabyte download that restarts is a person who gives up. |
| `signtest.sh` | **Is the thing a stranger double-clicks actually signed?** The certificate is a purchase and cannot be in a test; everything else can, and the parts that go wrong are not the expected ones — a signature nobody verified after applying it, signing twice (which appends rather than replaces), SHA-1 (refused by Windows since 2016 and still some tools' default), and a release that shipped unsigned because the key was not configured and nothing checked. A throwaway CA is made in the test so the chain is real, and a byte changed after signing must break it. |
| `loadertest.sh` | **With the power off and the stick out, does it come back up in AurOS?** Every other end-to-end test hands QEMU a kernel with `-kernel`, which is right when what is being tested is the staging environment and is exactly why none of them had ever asked this: `installtest` passed all thirty-two of its checks on a machine that would have lost AurOS at the next restart. This one installs, then starts the machine with firmware, one disk, no stick and no `-kernel` — and then does it again on `OVMF_CODE_4M.ms.fd` with Microsoft's keys enrolled and Secure Boot enforcing, which is the only way to find out whether the signed chain copied out of the image is a chain real firmware trusts. It also checks what the installer did **not** do: the machine's own EFI partition, byte for byte, unchanged. |
| `nvramtest.sh` · `aurfirsttest.sh` | The two programs that write EFI variables, and the rule that separates them. `nvramtest` decodes the installer's boot entry with a **second implementation in another language** — two implementations of one wire format is the pair that drifts, and the symptom of drift is a computer that starts nothing — and makes each of the four NVRAM gate checks fire, because a gate that has never fired is a gate nobody should trust. `aurfirsttest` proves the sentence the whole design rests on: until somebody says AurOS works, `BootOrder` is not touched by anything; confirming **promotes** AurOS without removing anybody else, and on firmware that ships with no `BootOrder` builds one that still contains every entry. It also throws `reboot`, `CONFIRM`, `confirm; rm -rf`, `../../bin/sh` and an empty file at the one privilege boundary in the desktop. |
| `matrixtest.sh` | **The hardware we cannot buy.** Ten machines synthesised in QEMU and put through the dry run, which writes nothing: 4Kn (built through a loop device with `--sector-size 4096`, because `sgdisk` on a plain file always writes 512-byte LBAs and a 4Kn machine cannot be made any other way), an OEM gigabyte EFI partition, a first partition at LBA 34 as disks were made before 2010, MBR, BIOS, BitLocker, a hibernated Windows, two Windows volumes with nothing to choose by, and a machine with no room. Each row is a shape of computer somebody actually owns. |
| `bridgetest.sh` | **Do the two halves agree about the bytes?** AurBridge writes on Windows; the staging environment reads after the restart and refuses the install if it does not recognise what it finds. They are separate programs in separate toolchains and nothing in either build would notice a renamed field. This links the *real* writer (`src/aurbridge/format.c`) and the *real* reader (`src/aurstage/journal.c`) into one program and makes the second read what the first wrote, field by field — then hands the cpio to `cpio`, the gzip to `gzip` and the stick's partition table to `sgdisk`, because the things that read these formats on a user's machine are the kernel and the firmware. The partition-table hash is checked against a third implementation in python that follows the words in `aurstage.h` and shares no code with either side. |
| `gpttest.sh` · `wrtest.sh` · `committest.sh` | The three files that can destroy a disk, each on its own. `gpttest` checks CRC32 against a published vector and the table against `sgdisk`; `wrtest` injects faults into the write gate and requires every out-of-window write to be a **refusal and not a clamp**; `committest` snapshots the disk after each flush and asks an independent tool what a reader would see at that instant — which is how the commit order was found to be backwards. |
| `healthtest.sh` · `shrinktest.sh` · `imagetest.sh` · `probetest.sh` · `ntfstest.sh` | The gates, one at a time: AC power and SMART; what `ntfsresize` will actually say and what it does when it is lying; finding and hashing the image on the stick; the hardware probe; and the NTFS reader, against volumes built to be in each bad state. |
| `hittest.c` | Two things. **Clicks land where the archetype paints** — sweeps `click()` across four resolutions and checks each consumed click against a mask of what was actually drawn. **Nothing highlights that cannot be clicked** — wherever hovering changes the frame, clicking must change it further. |

```sh
cc -O2 -o /tmp/recip_proof tools/recip_proof.c && /tmp/recip_proof
cc -O2 -std=gnu11 -o /tmp/blur_equiv tools/blur_equiv.c src/aurshell/draw.c -lm && /tmp/blur_equiv
cc -O2 -std=gnu11 -o /tmp/sheet tools/contactsheet.c src/aurshell/draw.c \
   src/aurshell/shellcommon.c src/aurshell/anim.c src/aurshell/layouts/*.c \
   src/common/theme.c src/common/wall.c src/common/font.c src/common/png.c -lm
# the conf is a RESOLVED shell.conf, not a .theme -- see below
/tmp/sheet /tmp/nocturne.conf /tmp/sheet.png

cc -O2 -std=gnu11 -I src/common -o /tmp/kerning tools/kerning.c src/common/font.c -lm
/tmp/kerning 36 /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf

cc -O2 -std=gnu11 -o /tmp/launchtest tools/launchtest.c src/aurshell/draw.c \
   src/aurshell/shellcommon.c src/aurshell/anim.c src/aurshell/layouts/*.c \
   src/common/theme.c src/common/font.c -lm && /tmp/launchtest

cc -O2 -std=gnu11 -o /tmp/targets tools/targets.c src/aurshell/foot.c \
   src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
   src/aurshell/net.c src/aurshell/settings.c src/aurshell/bt.c \
   src/aurshell/welcome.c src/aurshell/run.c src/aurshell/power.c \
   src/aurshell/layouts/*.c src/common/theme.c src/common/font.c \
   -I src/aurshell -I src/common -lm && /tmp/targets

cc -O2 -std=gnu11 -o /tmp/modaltest tools/modaltest.c src/aurshell/foot.c \
   src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
   src/aurshell/layouts/*.c src/common/theme.c src/common/font.c \
   -I src/aurshell -I src/common -lm && /tmp/modaltest   # from the repo root

cc -O2 -std=gnu11 -o /tmp/powertest tools/powertest.c src/aurshell/power.c \
   src/aurshell/run.c -I src/aurshell -lm && /tmp/powertest

cc -O2 -std=gnu11 -o /tmp/notifytest tools/notifytest.c src/aurshell/notify.c \
   src/aurshell/foot.c src/aurshell/draw.c src/aurshell/shellcommon.c \
   src/aurshell/anim.c src/aurshell/layouts/*.c src/common/theme.c \
   src/common/font.c $(pkg-config --cflags --libs dbus-1) \
   -I src/aurshell -I src/common -lm && /tmp/notifytest   # needs dbus-daemon + gdbus

cc -O2 -std=gnu11 -o /tmp/bttest tools/bttest.c src/aurshell/bt.c \
   src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
   src/aurshell/foot.c src/aurshell/layouts/*.c src/common/theme.c \
   src/common/font.c -I src/aurshell -I src/common -lm && /tmp/bttest

# The welcome question. WELCOME_RUN and WELCOME_STATE are redirected so
# the test never writes into a running desktop's runtime directory.
# The three -D paths are a directory the test makes and fills; it must
# not be the binary's own path, which is what this recipe used to say
# -- the first run created /tmp/welcometest as a directory and every
# link after it failed with "Is a directory".
cc -O2 -std=gnu11 -o /tmp/welcometest tools/welcometest.c \
   src/aurshell/welcome.c src/aurshell/foot.c src/aurshell/draw.c \
   src/aurshell/shellcommon.c src/aurshell/anim.c src/aurshell/layouts/*.c \
   src/common/theme.c src/common/font.c -I src/aurshell -I src/common -lm \
   -DWELCOME_RUN='"/tmp/welcometest.d"' \
   -DWELCOME_STATE='"/tmp/welcometest.d/first.state"' \
   -DWELCOME_FOUND='"/tmp/welcometest.d/found.json"' && /tmp/welcometest

sh tools/signtest.sh       # needs osslsigncode + openssl
sh tools/exetest.sh        # needs mingw + python3; starts a local server
sh tools/aurfirsttest.sh   # phases 9 and 10: BootOrder, BootNext, the gate
sh tools/nvramtest.sh      # the installer's boot entry, and its gate
sudo sh tools/loadertest.sh  # does the machine start AurOS by itself

./build/staging desktop && sudo sh tools/stagetest.sh   # needs qemu + losetup

sh tools/filetypes.sh      # needs packages_files installed on this machine
sh tools/plainwords.sh
sh tools/permissions.sh    # needs a rootfs a build has produced
sudo sh tools/failtest.sh  # needs a built image, qemu and OVMF

cc -O2 -std=gnu11 -o /tmp/powertest tools/powertest.c src/aurshell/power.c \
   src/aurshell/run.c && /tmp/powertest

# every screen of Settings, against a laptop made out of directories
cc -O2 -std=gnu11 -o /tmp/setsheet tools/setsheet.c src/aurshell/power.c \
   src/aurshell/run.c src/aurshell/foot.c src/aurshell/draw.c \
   src/aurshell/shellcommon.c src/aurshell/anim.c src/aurshell/layouts/*.c \
   src/common/theme.c src/common/font.c src/common/png.c -lm
AUROS_BACKLIGHT=/tmp/bl AUROS_POWER_SUPPLY=/tmp/ps \
  AUROS_SHELLS=$PWD/shells AUROS_THEME_DIR=$PWD/themes \
  /tmp/setsheet /tmp/nocturne.conf /tmp/set.png 1024 600 2.0

cc -O2 -std=gnu11 -o /tmp/kiosktest tools/kiosktest.c src/aurshell/apps.c \
   src/aurshell/shellcommon.c src/aurshell/draw.c src/aurshell/anim.c \
   src/aurshell/layouts/*.c src/common/theme.c src/common/font.c -lm && /tmp/kiosktest

cc -O2 -std=gnu11 -o /tmp/stridetest tools/stridetest.c src/aurshell/draw.c \
   src/aurshell/shellcommon.c src/aurshell/anim.c src/aurshell/layouts/*.c \
   src/common/theme.c src/common/font.c -lm && /tmp/stridetest

cc -O2 -std=gnu11 -o /tmp/contrast tools/contrast.c src/common/theme.c -lm
/tmp/contrast /tmp/*.conf            # add --strict if the design uses rules

# the compositor: generate the protocol code first, then build both
mkdir -p build/gen
for x in /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
         /usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
         /usr/share/wayland-protocols/stable/viewporter/viewporter.xml \
         /usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml; do
  n=$(basename "$x" .xml | sed s/-unstable-v1//)
  wayland-scanner server-header "$x" "build/gen/$n-server.h"
  wayland-scanner private-code  "$x" "build/gen/$n-protocol.c"
done
cc -O2 -std=gnu11 -o /tmp/wltest tools/wltest.c src/aurwl/aurwl.c \
   src/aurshell/draw.c src/common/png.c build/gen/*-protocol.c \
   -Ibuild/gen $(pkg-config --cflags --libs wayland-server xkbcommon) -lm
/tmp/wltest -s 8 -o /tmp/shm -- weston-simple-shm       # does a client arrive?
sh tools/wlstress.sh /tmp/wltest                        # does it survive one crashing?

# and again with AddressSanitizer -- the plain build survived two of the
# stress cases while corrupting the heap, which is worse than crashing
cc -g -O1 -fsanitize=address -std=gnu11 -o /tmp/wltest_asan tools/wltest.c \
   src/aurwl/aurwl.c src/aurshell/draw.c src/common/png.c build/gen/*-protocol.c \
   -Ibuild/gen $(pkg-config --cflags --libs wayland-server xkbcommon) -lm
sh tools/wlstress.sh /tmp/wltest_asan

# and against clients that are actively trying to break it
wayland-scanner client-header \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  build/gen/xdg-shell-client.h
cc -O2 -std=gnu11 -o /tmp/wlhostile tools/wlhostile.c src/aurwl/aurwl.c \
   src/aurshell/draw.c build/gen/*-protocol.c -Ibuild/gen \
   $(pkg-config --cflags --libs wayland-server wayland-client xkbcommon) -lm
/tmp/wlhostile

cc -O2 -std=gnu11 -o /tmp/hittest tools/hittest.c src/aurshell/draw.c \
   src/aurshell/shellcommon.c src/aurshell/anim.c src/aurshell/layouts/*.c \
   src/common/theme.c src/common/font.c -lm && /tmp/hittest

cc -O2 -std=gnu11 -o /tmp/keytest tools/keytest.c src/aurwl/aurwl.c \
   src/aurshell/draw.c build/gen/*-protocol.c -Ibuild/gen \
   $(pkg-config --cflags --libs wayland-server xkbcommon) -lm && /tmp/keytest

cc -O2 -std=gnu11 -o /tmp/nettest tools/nettest.c src/aurshell/net.c \
   src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
   src/aurshell/layouts/*.c src/common/theme.c src/common/font.c -lm \
   && /tmp/nettest

# every screen of the wifi panel in one picture; the last number is the
# text size SHE chose, and 2.0 on a 1024x600 panel is the hard case
cc -O2 -std=gnu11 -o /tmp/netsheet tools/netsheet.c src/aurshell/draw.c \
   src/aurshell/shellcommon.c src/aurshell/anim.c src/aurshell/foot.c \
   src/aurshell/layouts/*.c src/common/theme.c src/common/font.c \
   src/common/png.c -lm
/tmp/netsheet /tmp/nocturne.conf /tmp/net.png 1024 600 2.0
```

`blur_equiv` carries a verbatim copy of the *old* implementation, so it
keeps working as a reference after the original is gone from the tree.
It tolerates a difference of at most 2/255 per channel, which is what
reordering the separable passes costs in intermediate rounding; on real
frames the observed maximum is 1/255 on under 0.5% of subpixels.

`hittest` exists because `rail` painted with the surface's size and
hit-tested against a hardcoded 1600×900, so on the 1366×768 panels this
product targets, the default desktop's clicks landed in the wrong place.
The first version of the test compared the *shape* of the clickable
region across resolutions and passed with the bug still in — a region
clipped by the screen edge changes size even when the geometry behind it
is constant. Comparing clicks against painted pixels catches it at every
resolution. Checked by putting the bug back: 0.7–7.9% of clicks land on
bare wallpaper, against 0.0% when it is fixed.

`hittest`'s second check exists because of a different kind of lie. `rail`
— the **default** archetype — highlighted the six tiles on its Home card,
and its click handler never looked at them: the card underneath swallowed
the click and did nothing. The first thing anyone ever clicked in this
operating system silently failed. The first check could not catch it,
because the click *was* consumed and it *did* land on painted pixels.

Its first version compared the post-click frame against the **resting**
frame and passed with the bug still in — the post-click frame still
carries the hover highlight, so it always differed and nothing ever
looked dead. It has to compare click against **hover**. Checked by
putting the bug back: 135 of 135 reactive points go dead.

That check then found two more of the same kind in `dock`: clicking a
favourite that was not already running did nothing (the archetype's
entire promise is that your programs are there "whether running or
not"), and the 4px lead-in band where an icon starts growing as the
pointer arrives magnified the icon and then swallowed the click — which
is precisely the row you hit when you shove the pointer at a dock and
click the moment it responds.

**Resolving a theme to a `shell.conf`.** Several of these take a
*resolved* config (`col_bg = 0x...`), not a `.theme` file
(`bg="#..."`). Passing a `.theme` does not fail — it silently falls back
to the built-in defaults, which look like the current theme, so you see
the old colours and conclude your change did nothing:

```sh
S=$(mktemp -d); printf 'sandstone\n' > $S/theme
tail -n +2 themes/templates/shell.conf.tpl | \
  AURORA_THEMES=themes AURORA_TEMPLATES=themes/templates \
  AURORA_STATE=$S AURORA_CACHE=$S/c ./src/aurora/aurora render > /tmp/sandstone.conf
```

`AURSHELL_CLOCK=<unix seconds>` pins the clock, so two renders differ
only where you changed something. Without it a comparison picks up
whatever minute each run happened to land in, which reads as a
hundred-level regression in the top bar.

`contrast` exists because the audience is someone whose ten-year-old
laptop got too slow: an aging TN panel with a washed-out gamma curve,
often at an angle, often in a bright room, often sixty-year-old eyes. A
palette that reads beautifully on a designer's monitor can be genuinely
unusable there, and no amount of taste makes up for it.

It separates **required** from **advisory**, because the answer depends
on the design. WCAG exempts a disabled control — looking unavailable is
the point — and a divider between two regions that are already clearly
separated is decoration. But that flips the moment a design replaces
shadows with hairlines: then the rule *is* the structure, and an
invisible rule is an invisible structure. Every current theme sits near
**1.4:1** on `col_overlay`, so any direction built on rules has to raise
it and prove it with `--strict`.

`kerning` exists because the answer was silently "none". `font.c` read
only the legacy `kern` table, and almost every typeface drawn this
century ships its kerning solely in GPOS. Measured across every font
installed here, **every proportional face went from 0.00px to kerning
9 or 10 of 10 test pairs**; monospace and symbol faces correctly still
report 0.00, which is what they should do.

This mattered for a design reason, not a technical one: the redesign
asks for a display face at 36px, and at that size unkerned `Ta`, `Wo`
and `P.` visibly fall apart. Badly spaced display type is itself one of
the things that makes software look machine-made — and the alternative
was to pick typefaces around the engine's limitation rather than for
their merits.

`keytest` exists because every text field in this product read one
hardcoded table — `qwertyuiop`, `asdfghjkl`, `zxcvbnm` — with a comment
above it claiming that a real session got its characters from the
keymap. No such path existed. So there were no capital letters anywhere
in the operating system, no character that needs Shift, and on a French
keyboard every letter was wrong — on a machine whose own profile said
`keyboard_layout="fr"`, because that field was read by nobody either.
The visible consequence is small and total: a wifi password with a
capital in it cannot be typed, so the machine cannot get online.

It checks the three claims that replaced the table — modifiers resolve
through the real keymap (Shift+1 is `!`, and Caps Lock is *not* Shift,
which would turn a password's `1` into `!` invisibly); keys that are
not text produce none; and the layout is read from
`/etc/default/keyboard`, which is the file the rest of the system
already uses and `build/forge` already writes. Its last case feeds it a
layout name that does not exist, because a typo in a profile must leave
a keyboard in the wrong language rather than no keyboard at all.

`powertest` and `setsheet` exist because every laptop this product is
for has a battery and a backlight, and no build host has either. So the
machines are made out of directories: the kernel publishes all of it as
small files, and a tree of small files is a laptop as far as this code
is concerned. The cases are the ones that are wrong on real hardware —
two batteries, a full battery on the mains (plugged in and *not*
charging, which is not the same sentence), an empty battery bay
reporting zeros that must not read as a flat battery, a panel that
counts to 96000 and one that counts to 7, and a laptop exposing both a
real panel control and the firmware's seven-step version of it, where
picking the wrong one is how a brightness slider moves and changes
nothing.

`setsheet` earned itself on its first render: the percentage on each
slider was painted straight through the `+` button, the archetype and
theme lists came up empty, and a machine set to UTC — which every
shipped image is — showed forty cities with nothing marked as current.

`failtest.sh` checks the one screen in this product that only appears
when everything else has gone wrong, the only way it can be checked: by
making everything else go wrong on a real machine and looking at the
screen. It took three attempts to get that screen right, and not one of
the three failures was visible in the unit file.

The first was `OnFailure=getty@tty1.service` — a login prompt on a
machine whose owner has never been told the account name or the
password, and whose password the image expires on purpose. The second
was a separate unit drawing a message, killed two seconds in by the
shell's own `TTYVHangup=yes`. The third was adding `Conflicts=` to stop
the shell restarting underneath it, which changed the signal from
SIGHUP to SIGTERM and nothing else, because `Conflicts` is symmetric
and something always starts the shell again. The answer was to stop
having two units: the message is the shell's own `ExecStopPost`.

It does not copy the image — they are several gigabytes. It adds a
drop-in that makes the shell fail, boots with `-snapshot` so the guest
writes nothing back, and removes the drop-in on the way out, including
when interrupted. Checked by running it against an image built from the
previous design: "aurshell.service never runs aursorry, so a failed
desktop shows her nothing."

`permissions.sh` exists because the same bug happened three times in
one day. The product puts a control on the screen, the control runs
something, and the permission service refuses it in silence: the
Internet button, then Turn off and opening a USB stick, then
double-clicking a downloaded `.deb` — which is the single sentence the
whole product is built around. Every one of them looked right and
pressed cleanly. None of them did anything, and nothing said so: not
the build, not the journal, not the screen.

The cause is always the same. `aurshell` is a systemd service, and
polkit's shipped rules are written in terms of a logind session, so
every `allow_active=yes` in the distribution reads as `auth_admin` for
us — and there is no authentication agent in this session for the
prompt to appear in, and no password to type into it if there were.
gdebi is worse: its action is `auth_admin` for *all three* cases, so a
session does not save it either.

It asks two questions of the image that was actually built, for every
thing the product offers to do: does this permission exist at all (a
typo in an action id is a grant that silently is not one, and polkit
says nothing about an id it has never heard of), and does anything
grant it. Checked by putting both failures back: removing the gdebi
grant reports "Install a downloaded program — REFUSED", and misspelling
`login1.power-off` reports "NO SUCH ACTION".

`nettest` exists because the wifi panel's job is to turn what nmcli
printed into something she can press, and that translation cannot be
checked by looking at a screen: on the developer's machine every
network is well-behaved. The cases that break it are all in somebody
else's house — a name with a colon in it (which nmcli writes as `\:`,
and which a naive split truncates, so the name handed back to nmcli to
join is one that does not exist); a mesh answering twice for one house;
a network that announces no name at all; the one she is already on,
buried at position nine because a neighbour's is stronger. It also
checks every failure sentence, because which sentence she is shown when
it goes wrong is decided by reading nmcli's own, and getting that
reading wrong means the wrong words at the worst moment.

It caught its own author immediately, though on the test rather than
the code: the expected name in the colon case was written with a stray
backslash in it.

`netsheet` is `contactsheet` for the wifi panel: all five of its
screens, rendered from the panel's own painting code against a real
theme at a real panel size, because running it needs a radio, a daemon
and somewhere with networks in range. It reaches the panel's private
state by including `net.c` rather than by adding a way in for it — a
door cut into shipping code so a picture can be taken is a door that is
there on the machine too.

It earned itself in one render. At 200% text on a 1024×600 panel — the
exact machine and the exact person this product is for — the band at
the bottom painted *Internet*, *Smaller* and "Words are 100% bigger"
through each other, illegible, in the one strip that exists to still
work when everything else has stopped being legible; the list showed
one network out of six, because the buttons had been scaled in
proportion to the type and a hundred-pixel-tall button is no easier to
press than a sixty-pixel one; and "1 of 6" was painted exactly where
the buttons now were. None of the three is visible at 100%.

`wltest` fails a window that maps but arrives as one flat colour, not
just one that never maps. A window full of one colour is what a client
draws when it has given up, and a compositor that hands out buffers but
never reads them back would otherwise pass its own test.

`wlstress.sh` exists because of a real crash: a SIGKILLed terminal
segfaulted the compositor. libwayland destroys a dead client's resources
in an order the compositor does not choose, and it freed the wl_surface
before running the xdg_surface destructor, which then wrote through the
freed pointer. That is not a corner case -- it is every crash of every
program, and it meant one misbehaving application closed every other
window on the machine. No polite client can reach it, which is exactly
why the polite tests all passed.

To see the whole desktop with real software in it, without a screen:

```sh
aurshell --shell shells/rail.shell --png /tmp/desk.png --size 1366 768 \
         --with-app 'epiphany-browser https://example.com'
```

That runs the same compositor, reconcile and input routing the booted
machine runs. Only the destination differs.

`stridetest` is the one harness that tests the environment every other
harness cannot produce. A scanout buffer's rows are padded out to the
hardware's pitch alignment -- 1366 pixels wide becomes 1376 of stride on
Intel graphics -- and `surface_new()` allocates with stride == width, so
every PNG render, the contact sheet, the hit-test harness and the
preview tool run in a world where the two are equal. QEMU is 1024 wide
and 1024 * 4 is already aligned, so it does not reproduce there either.

It renders each archetype twice, once tight and once padded by an odd
number of pixels, and requires the two to be pixel-identical inside the
visible rectangle -- and the padding to be untouched. Load real fonts or
it proves nothing about text: with NULL fonts `shell_text()` returns
immediately and the primitive that was actually broken never runs.

`wlhostile` is a Wayland client that misbehaves on purpose, and a
harness that runs it against a fresh compositor. Each case passes only
if the compositor refused the hostile client AND then still served an
ordinary one -- refusing is the correct answer, so "it disconnected
them" is a pass and "it kept talking to them" would not be.

Every case in it was a real defect. Two were four-request crashes in a
process that owns the display; one wrote 127 bytes of the client's
choosing into freed memory; two were hangs, which in this process are
as fatal as a crash and harder to explain to the person it happens to.

`kiosktest` treats the kiosk allow-list as what the product says it is:
a security control. On a managed machine it is the only thing limiting
what can run, `allow_tty` is off so there is no console to escape to,
and an unreadable policy file denies everything.

It was bypassable three ways, each of them one file dropped in
`~/.local/share/applications` — name it after an allowed program, claim
that program's `StartupWMClass`, or write `Exec=allowed ; something
else`, because the exec test accepted any suffix after a space. All
three compared the list against something the person writing the file
controls. The list is now matched against the resolved absolute path of
the program the entry runs, the user's directory is not searched at all
while a lockdown is in force, and there is no shell in the launch path.

The last check in it is that a FIFO and a symlink to `/dev/zero` in
that directory do not hang the scan. Either one used to block forever,
before the compositor starts and before the first frame — so the
desktop never appeared, and on a kiosk there was no way back in.

`plainwords.sh` enforces one rule from docs/EASY.md: every word the
product puts on a screen is a word she uses. It reads the strings the
shell paints, the sentences that describe each archetype at install
time, what a build calls itself, and the Windows-side installer's own
words -- and fails on any of the jargon listed in docs/EASY.md.

It deliberately does not read `fprintf(stderr, ...)`. Developer output
is allowed to say "compositor" and "stride", because the person reading
it can act on it. Mixing the two lists is how a rule like this becomes
unenforceable and then ignored.

Its first version reported "ok" and **could not fail**: it looked only
at the line containing the `shell_text` call, and almost every call in
this codebase wraps, so the string sat on the next line and was never
examined. It now reads every literal and filters down to prose. Check
it can still fail before trusting it -- put "partition" in a label and
confirm it is caught.

Two exclusions are principled rather than convenient, and are worth
knowing about. A literal containing a newline escape is skipped,
because `shell_text()` paints one line and nothing it draws contains
one -- that is also how a developer `--help` block is recognised, since
the `stderr` that gives it away sits a dozen lines below the string.
And bare "swap" was removed from the list: it is jargon only in "swap
partition", which "partition" already catches, and on its own it is an
ordinary English verb. Flagging workbench's "swap pane" would have
taught people to ignore the check, which is the only way a check like
this really dies.

`launchtest` answers the first question a desktop has to answer: can she
start a program by clicking on it? This product answered "no" for its
entire life and nothing noticed.

`shell_launch()` -- the only function that asks the host to run
anything -- had zero callers. All six archetypes hand-rolled a window
slot instead: filled in a title and a subtitle and stopped, so every
icon opened a rectangle with a name in it and nothing behind the
rectangle. `tiles.c` carried a comment explaining that the shell
contract had no hook for starting a program; the hook was added later
and the comment, and the code under it, stayed.

Two harnesses looked straight at it and passed. `hittest` proves a
click is *consumed* and that the frame changes afterwards -- both were
true, because a placeholder window is a visible change. And
`aurshell --with-app CMD` was used as the end-to-end proof, but it
calls `aurwl_spawn()` directly from `main.c`: it exercises the
compositor and bypasses the entire click-to-launch path, which was
exactly the missing piece. **A test that starts the program itself
cannot discover that nothing else does.**

So `launchtest` never spawns anything. It installs a recording hook in
`shell_ctx.spawn`, sweeps clicks across the screen, and asks whether
any of them reached the hook -- with nothing running, with three windows
already open, and at 1024x600, because an archetype that only launches
from an empty desktop, or only once something is already there, or
loses the control when the panel gets small, is broken in a way one
sweep would miss.

`targets` enforces docs/EASY.md rule 4: everything she has to press is
at least 44 pixels on its shorter side **at 1024x600**, the bottom of
the range, because that is where the old machines are. A target
measured at 1920x1080 and allowed to shrink with the panel fails
exactly where it matters.

It caught the rule's own author within the hour. The always-present
band at the bottom of the screen -- the one piece of furniture in this
product that exists specifically for a person whose hands are not
steady -- was sized at 46 pixels tall and then had padding taken out of
it, leaving 27-pixel buttons. Nobody had to be careless: the two
numbers were three lines apart.

It measures across three resolutions and all four text sizes she can
choose, because the band's height follows the type and a control that
is fine at one size can be squeezed out of the rule at another. It also
checks the buttons do not overlap and are on the screen, which are the
other two ways a row of controls becomes unpressable.

It only measures controls that publish their geometry -- today, the
band. Archetype internals (dock cells, window buttons, rail rows) do
not, so it says so in its own output rather than implying coverage it
does not have.

`filetypes.sh` checks that every kind of file opens something that
exists. `build/forge` writes an `/etc/xdg/mimeapps.list` naming a
`.desktop` for each file type, and if that `.desktop` does not exist the
line does nothing at all -- silently. The file just opens with whatever
the database happens to rank first, or with nothing.

Two of the first six entries were wrong: ristretto ships
`org.xfce.ristretto.desktop` and mousepad ships
`org.xfce.mousepad.desktop`, not the names anybody would assume. A
`.deb` that opens an archive manager instead of an installer is how a
person ends up with a folder full of files and no program.

It reads the table out of `build/forge` rather than keeping a second
copy, checks each `.desktop` exists AND that its `Exec` names something
installed, and calls out the `.deb` association on its own -- that one
is the whole "download things like on any other Linux distro" path. It
exits 2, rather than passing, when the packages are not installed
locally, because an empty search is not a clean bill of health.

## The installer's tests, and what they cost to run

```sh
# the whole round trip, ~25 minutes, needs root and QEMU with OVMF
sudo sh tools/installtest.sh

# every geometry, through the dry run, which writes nothing, ~20 minutes
sudo sh tools/matrixtest.sh

# what AurBridge writes against what reads it. Seconds, no root, no QEMU.
sh tools/bridgetest.sh

# R4/R6. Needs a fault-injection image first, and takes about an hour.
sudo AURSTAGE_FAULT=1 ./build/staging
sudo sh tools/powercuttest.sh
```

`build/staging` refuses to pack an ordinary image that contains the
fault-injection flag, and refuses to pack a fault-injection one that
does not — the check is on the linked binary, because what ships is the
binary.
