# tools — proofs, not demos

Each of these exists because a change here was too easy to get subtly
wrong to accept on inspection alone. They are meant to be run, and they
exit non-zero when they fail.

| | What it proves |
|---|---|
| `recip_proof.c` | The blur's multiply-and-shift replacement for integer division is **exact** — checked over every (window, sum) pair a blur can produce, all 4.2 million of them, for radius 1..128. Not a spot check. |
| `blur_equiv.c` | The rewritten blur matches the one it replaced pixel for pixel, on adversarial full-contrast noise across 9 radii × 8 region shapes including off-screen, 1×1 and 1-pixel-wide slivers. Also times both. |
| `hittest.c` | Two things. **Clicks land where the archetype paints** — sweeps `click()` across four resolutions and checks each consumed click against a mask of what was actually drawn. **Nothing highlights that cannot be clicked** — wherever hovering changes the frame, clicking must change it further. |

```sh
cc -O2 -o /tmp/recip_proof tools/recip_proof.c && /tmp/recip_proof
cc -O2 -std=gnu11 -o /tmp/blur_equiv tools/blur_equiv.c src/aurshell/draw.c -lm && /tmp/blur_equiv
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
