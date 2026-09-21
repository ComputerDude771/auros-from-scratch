# tools — proofs, not demos

Each of these exists because a change here was too easy to get subtly
wrong to accept on inspection alone. They are meant to be run, and they
exit non-zero when they fail.

| | What it proves |
|---|---|
| `recip_proof.c` | The blur's multiply-and-shift replacement for integer division is **exact** — checked over every (window, sum) pair a blur can produce, all 4.2 million of them, for radius 1..128. Not a spot check. |
| `blur_equiv.c` | The rewritten blur matches the one it replaced pixel for pixel, on adversarial full-contrast noise across 9 radii × 8 region shapes including off-screen, 1×1 and 1-pixel-wide slivers. Also times both. |
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

cc -O2 -std=gnu11 -o /tmp/contrast tools/contrast.c src/common/theme.c -lm
/tmp/contrast /tmp/*.conf            # add --strict if the design uses rules

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
