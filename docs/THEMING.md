# Theming AurOS

> **If you are here to reskin AurOS, you only need to do one thing:
> write a `.theme` file and run `aurora set <name>`.** Everything else on
> this page is reference.

Nothing in the system hardcodes a colour, a corner radius, a gap or a
font. Every surface — wallpaper, bar, command palette, terminal, the
Linux console, GTK apps, the bootloader menu — renders from the active
theme.

---

## The 60-second version

```sh
aurora list                       # what is installed
aurora show synthwave             # preview a palette in the terminal
aurora new midnight --from nocturne
$EDITOR /usr/share/auros/themes/midnight.theme
aurora validate midnight          # refuses to ship a half-written theme
aurora set midnight               # applies it everywhere, live
```

`aurora set` re-renders every template, regenerates the wallpaper at the
display's resolution, repaints the TTY palette, and signals the running
shell to reload. No rebuild, no restart.

---

## The theme file

Plain `key="value"`. No parser, no dependencies — it is sourced by a
POSIX shell and read by a small C parser. Comments start with `#`.

```sh
# ── Identity ───────────────────────────────────────────────────────
theme_name="Midnight"
theme_author="you"
theme_variant="dark"            # dark | light  (light is fully supported)
theme_description="One line describing the mood."

# ── Core palette: layered deepest → brightest ──────────────────────
bg="#0B0E14"          # desktop void, deepest background
bg_alt="#10151F"      # bar background, panels
surface="#161C28"     # cards, windows, menus
surface_hi="#1F2735"  # hover / selected row
overlay="#2B3542"     # borders, dividers
muted="#55606E"       # disabled text, decoration
subtle="#8793A4"      # secondary text, captions
fg="#D4DCEA"          # primary text
fg_hi="#F3F7FD"       # emphasis, active title

# ── Accents ────────────────────────────────────────────────────────
accent="#7DD3C0"      # focus rings, active workspace, cursor
accent_alt="#A78BFA"  # secondary highlights
accent_warm="#F2B880" # badges, warm emphasis

# ── Semantic ───────────────────────────────────────────────────────
ok="#7DD3C0"   warn="#F2B880"   err="#F2788D"   info="#82AAFF"

# ── ANSI 16 (terminal + Linux console) ─────────────────────────────
ansi0="#161C28"   ansi8="#55606E"      # black / bright black
ansi1="#F2788D"   ansi9="#FF9AAB"      # red
ansi2="#7DD3C0"   ansi10="#9BE8D8"     # green
ansi3="#F2B880"   ansi11="#FFD2A3"     # yellow
ansi4="#82AAFF"   ansi12="#A5C4FF"     # blue
ansi5="#A78BFA"   ansi13="#C4ADFF"     # magenta
ansi6="#6FD8DC"   ansi14="#93EEF2"     # cyan
ansi7="#D4DCEA"   ansi15="#F3F7FD"     # white

# ── Geometry (pixels) ──────────────────────────────────────────────
radius="14"  radius_sm="8"  gap="12"  margin="14"
border="2"   bar_height="38"  padding="14"

# ── Typography ─────────────────────────────────────────────────────
font_sans="Inter"          # family name; forge resolves it to a file
font_mono="JetBrainsMono"
font_size="14"  font_size_sm="12"  font_size_lg="19"  line_height="1.45"

# ── Effects ────────────────────────────────────────────────────────
blur="1"  blur_radius="20"
shadow="1"  shadow_opacity="0.50"  shadow_radius="28"
opacity_panel="0.88"  opacity_inactive="0.94"
animation_ms="170"  animation_curve="spring"

# ── Wallpaper (generated, never a file) ────────────────────────────
wall_style="aurora"    # aurora | mesh | waves | gradient | noise | solid
wall_c1="#080B11"      # deep base
wall_c2="#10303A"      # mid wash
wall_c3="#7DD3C0"      # ribbon / accent 1
wall_c4="#A78BFA"      # ribbon / accent 2
wall_intensity="0.55"  # 0.0–1.0
wall_grain="0.05"      # film grain; hides banding on large displays
wall_vignette="0.45"

# ── Branding ───────────────────────────────────────────────────────
brand_glyph="◆"  brand_text="AurOS"  cursor_size="24"
```

### Inheritance

A variant states only what it changes:

```sh
inherit="nocturne"
theme_name="Midnight"
accent="#8AB4F8"
wall_c3="#8AB4F8"
```

Parents are resolved iteratively, parent-first, so a child's assignments
win. Cycles and chains deeper than 16 are refused.

---

## Wallpaper styles

Generated from `wall_c1..c4`, so they follow the palette automatically.

| `wall_style` | What it draws | Suits |
|---|---|---|
| `aurora` | Sinusoidal ribbons with a gaussian core over layered ridge silhouettes and stars | dark, atmospheric |
| `mesh` | Inverse-square weighted colour blobs, fbm-warped | light themes, calm |
| `waves` | Horizon, banded sun, anti-aliased perspective grid | retro, high contrast |
| `gradient` | Diagonal two-stop with two radial glows | minimal |
| `noise` | fbm mapped through the palette | textured |
| `solid` | Flat `wall_c1` + vignette | kiosk, maximum legibility |

Preview without applying:

```sh
aurwall --theme themes/midnight.theme --out /tmp/w.png --width 2560 --height 1440
aurwall --theme themes/midnight.theme --style waves --out /tmp/w2.png
```

---

## Templates: adding a new surface

A template is any file with an `@!out=` header. `aurora set` renders
every `.tpl` in the template directory. **Adding a themed surface means
adding a template — never editing C.**

```
themes/templates/myapp.conf.tpl
─────────────────────────────────────────────
#@!out=/etc/myapp/colors.conf mode=0644
background = @bg@
text       = @fg@
highlight  = @accent@
border     = @accent|mix:bg:60@
dim_text   = @fg|mix:bg:45@
```

### Colour algebra

Derived shades are computed, never hand-written — which is what keeps a
theme to ~40 meaningful values instead of 300.

| Token | Result |
|---|---|
| `@accent@` | `#7DD3C0` |
| `@accent\|hex@` | `7DD3C0` |
| `@accent\|0x@` | `0x7DD3C0` |
| `@accent\|rgb@` | `125,211,192` |
| `@accent\|rgbsp@` | `125 211 192` |
| `@accent\|rgba:0.4@` | `rgba(125,211,192,0.40)` |
| `@accent\|lighten:20@` | 20% toward white |
| `@accent\|darken:35@` | 35% toward black |
| `@accent\|mix:bg:70@` | 70% blended toward `bg` |
| `@accent\|on@` | whichever of `bg`/`fg_hi` reads better **on** the accent |
| `@accent\|lum@` | relative luminance, `0.000`–`1.000` |
| `@radius\|int@` | `14` |

`@key|on@` is the one worth remembering: it picks a readable foreground
for any background, so an accent can change from pale yellow to deep
navy without anyone having to revisit the text colour.

Unresolved tokens are emitted as `@@MISSING:key@@` **and warned about**,
so a typo is loud rather than silent.

### Surfaces already templated

`shell.conf` (the desktop), `colors.sh`, `colors.json`, `console.sh`
(Linux TTY palette via OSC `P`), `terminal.conf`, `gtk.css` (GTK3/4),
`prompt.sh`, `motd`, `grub-theme.cfg` and `grub/theme.txt`.

---

## Checks that stop a bad theme shipping

- `aurora validate NAME` requires every key in the schema, including all
  16 ANSI entries. `aurora set` runs it first and **refuses to apply an
  incomplete theme** — a missing `fg` would otherwise mean black text on
  a black desktop with no way back.
- `forge` verifies the named font actually contains TrueType `glyf`
  outlines before adopting it. Many modern fonts (Ubuntu's Inter among
  them) ship as OTF with CFF outlines; selecting one of those would give
  a desktop with no text at all. It falls back with a warning.
- Rendering warns on every unresolved token.

---

## Writing a good theme

- **Keep the layer order monotonic.** `bg` → `bg_alt` → `surface` →
  `surface_hi` should step steadily in one direction. If two are too
  close, hover states vanish; if `surface` overshoots `fg`, text drowns.
- **One accent does the work.** `accent` marks focus, the active
  workspace and the caret. A second accent competing for attention reads
  as noise. `accent_alt` is for a *different kind* of thing (notifications),
  not for variety.
- **Check contrast, don't eyeball it.** `@fg|lum@` and `@bg|lum@` should
  differ by roughly 0.5 or more. `muted` is for borders and decoration;
  text at `muted` on `bg` will fail accessibility.
- **Light themes need different effects, not just inverted colours.**
  Drop `shadow_opacity` to ~0.15 and `blur_radius` to ~14; a dark theme's
  heavy shadow looks like dirt on paper. See `sandstone.theme`.
- **Tie the wallpaper to the palette.** `wall_c3`/`wall_c4` echoing
  `accent`/`accent_alt` is what makes the desktop feel designed rather
  than assembled.
- **Preview before applying**: `aurora show NAME`, then
  `aurwall --theme ... --out /tmp/w.png`.

---

## Enforcing a theme on a fleet

Themes are per-image via the profile, and can be made non-negotiable:

```sh
# profiles/school-kiosk.profile
theme="sandstone"
allow_theme_change="no"     # written to /etc/auros/policy.conf
```

The shell and settings read `/etc/auros/policy.conf`; lockdown is
enforced by consulting policy, not by hiding menu items.

---

## How it fits together

```
themes/midnight.theme                    the only file you edit
        │
        ├─ aurora set midnight
        │     ├─ resolve inherit chain (parent-first)
        │     ├─ validate every required key
        │     ├─ render every templates/*.tpl  ──►  /etc/auros/*
        │     ├─ aurwall  ──►  wallpaper at display resolution
        │     ├─ console.sh  ──►  repaint the TTY palette
        │     └─ SIGHUP aurshell  ──►  live reload, no restart
        │
        └─ forge (build time) bakes the same result into the image
```
