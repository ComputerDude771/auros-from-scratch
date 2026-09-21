# Design law

This document exists because the first version of AurOS looked like
software that designed itself.

It was not ugly. It was *generic* — and generic in a specific,
recognisable way, which is worse, because it tells a person that nobody
was really at the controls. For a product whose entire proposition is
"trust us with the computer that has all your photos on it", that is not
a cosmetic problem.

---

## The rules

**Forbidden**

1. **No blue-to-purple or pink-to-purple neon gradients.** No gradient
   used as decoration at all.
2. **No centred hero followed by a row of identical rounded cards.**
3. **No glassmorphism, glowing borders, or arbitrary drop shadows.**
4. **No walls of undifferentiated text.** Rhythm, deliberate
   whitespace, and real weight contrast instead.

**Mandated**

5. **Typography carries the hierarchy.** A distinct display face for
   headings and a clean, highly readable face for text — and *uneven*
   weights. If the hierarchy would survive being printed in one colour,
   it is real.
6. **A limited, intentional palette.** Subtle variation between
   surfaces, not loud gradients. One accent that means something,
   rather than three that mean nothing.
7. **Asymmetry and generous space.** Hand-composed, not stamped from a
   grid generator.

---

## What we did wrong, so it is not done again

Every item below was in the shipped tree. They are listed with their
evidence because "make it less generic" is not actionable and this is.

**The accent pair.** `accent=#7DD3C0` with `accent_alt=#A78BFA` is the
canonical teal-to-violet AI pair. It was restated verbatim as the two
wallpaper ribbon colours, so the desktop *was* the gradient; then again
in the installer's per-pixel backdrop, the website's blurred glow
layers, the login banner, and as a swatch row on the homepage. One
colour decision, repeated on every surface the product has.

**Colours hardcoded in C.** Nine Nocturne hexes were baked into the app
table in `main.c`, and again in three other files. `docs/SHELLS.md` says
in writing: *no archetype may hardcode a colour*. The one rule the
codebase wrote down to protect itself was broken in the file that seeds
every icon — which is why Sandstone's warm-paper desktop still glowed
mint and lavender.

**Glassmorphism as the declared house style.** `draw.h` said it outright:
"rounded rectangles, soft shadows and translucency over a procedural
wallpaper". Nineteen blur sites, 28px pure-black shadows at 50% behind
every panel. It was also the most expensive thing the shell did, on the
Atom it was built for.

**One face, four sizes, one weight.** That was the entire typographic
system of an operating system. `font_load()` had no weight parameter, so
weight contrast was not merely absent — it was unrepresentable. The
theme file advertised `font_size_sm`, `font_size_lg` and `line_height`;
nothing read them, and the sizes came from `2.6 / 1.7 / 1.2` multipliers
hardcoded in C.

**The number 14.** `radius`, `margin`, `padding` and `font_size` were all
14. The corner radius, the screen inset, the internal padding and the
base type size were one number. There was no spacing scale, which is
exactly why nothing in the system could be intentionally uneven.

**The halo disc.** A pastel tinted circle behind a thin outline icon,
twenty times, in six archetypes. All twelve icons shared one stroke
width, one corner radius and one fill opacity, so Mail, Photos, Window
and Terminal were the same box with different lines in it.

**Nine centring decisions and no off-axis element.** Perfect bilateral
symmetry everywhere, argued for in comments.

**The same composition six times.** Halo disc, centred icon, centred
name, centred hint — reimplemented independently in all six archetypes.
It is the reason six genuinely different interaction models — a row, a
page, a kiosk, a taskbar, a dock, a tiler — all read as the same
product.

---

## Structural rules that outlive any particular look

These are not taste. A future reskin must not break them.

- **Nothing hardcodes a colour.** Not a layout, not the app table, not a
  shadow. If a value is not in the theme schema, add it to the schema.
- **The theme file is the whole surface.** `themes/<id>.theme` →
  `src/aurora/aurora` → `/etc/auros/shell.conf`, read at runtime. A
  reskin must never require a recompile. `tools/theme-test.sh` proves
  every template resolves against every theme.
- **One function owns geometry**, called by both painting and
  hit-testing. `tools/hittest.c` proves it, at four resolutions.
- **What lights up can be clicked.** An element that reacts to the
  pointer has promised something. `tools/hittest.c` proves that too.
- **The type scale lives in the theme**, not in multipliers in C.
- **It has to work at 1024×600**, in four themes, in a language whose
  labels are 40% longer, on a machine from 2013.
- **If rules replace shadows, the rules must be visible.** Every theme
  written before this had `col_overlay` around 1.4:1 against its
  surface, which was defensible while shadows and fills did the
  separating and is not defensible once a hairline *is* the structure.
  `tools/contrast.c --strict` is the check; 3:1 is the floor. Two
  independent redesigns both deleted the shadows and both left the rule
  below 2.4:1 — it is not an obvious mistake, which is why it is
  written down here.

---

## Judging a redesign

A direction is finished when someone can answer these without hedging:

1. Point at the single accent and say what it means. If there are three,
   which one does the eye go to first, and why?
2. Cover the colour. Is the hierarchy still legible? If not, the colour
   was doing the typography's job.
3. Find the one element that is deliberately off-axis. If there isn't
   one, the composition was generated rather than made.
4. Show it to someone who is not a designer and ask what it reminds them
   of. "A nice app" is a failure. "The instructions that came with
   something" is not.
