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
