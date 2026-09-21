# Shell archetypes — how this computer should work

AurOS asks one question during setup that no other operating system
asks: **when you want to get to a different thing, what do you do?**

That single answer determines almost everything else about how a
computer feels. So instead of shipping one answer and calling it "the
desktop", AurOS ships six, and you pick.

> **For builders:** an archetype is a `.shell` file. Colours are the
> *theme*. What a user is permitted to do is the *policy*. Those three
> files build a whole distribution without anyone writing code — which
> is the entire point, because bespoke shell work is the expensive part.

---

## The six

| | Called | In one line | Windows |
|---|---|---|---|
| **Rail** | Everything in a row | Everything open sits side by side; nothing ever hides | one at a time |
| **Tiles** | One thing at a time | A page of big buttons; press one, it fills the screen | one at a time |
| **Locked** | Just these apps | Only what the owner chose. Nothing else exists | one at a time |
| **Taskbar** | The familiar one | A bar along the bottom listing everything you have open | overlapping |
| **Dock** | Favourites along the edge | Your regular programs always in the same place | overlapping |
| **Workbench** | Panes and keyboard | Windows divide the screen automatically; keyboard-driven | tiled |

They differ on the axis that actually changes how a computer feels —
**how you reach another thing, and whether things can hide** — not on
colour or icon shape. Any of them can wear any theme.

---

## Rail — "Everything in a row"

Everything you have open sits side by side in a row. What is next to you
is always visible at the edges of the screen; click it to go there. The
first card is always Home, so you cannot get lost.

- **Best for:** someone who loses windows, or has never been comfortable
  with a computer.
- **The catch:** you see fewer things at once than a normal desktop, and
  you cannot put two windows side by side.
- **Feels like:** flipping through photos on a phone.

*Why it exists:* every other desktop splits the screen into the thing
you are looking at and the machinery for reaching other things — a bar,
a dock, an overview. That second layer is where "it disappeared" comes
from. The Rail has no second layer.

This is the default, because it is the only one where **a thing cannot
be hidden.**

## Tiles — "One thing at a time"

You start on a page of large labelled buttons. Press one and it takes
over the whole screen. A single Home button brings the page back.

- **Best for:** anyone who already uses a phone or tablet and wants the
  computer to behave the same way.
- **The catch:** you cannot see two things at once, and switching means
  going via Home.
- **Feels like:** a phone or tablet home screen.

## Locked — "Just these apps"

The computer does the handful of jobs it was set up for and nothing
else. No desktop, no settings, no way to install anything, no file
system. If only one app is allowed, it simply starts and stays.

- **Best for:** schools, libraries, reception desks, clinics — or a
  relative who only needs a couple of things and must not be able to
  break them.
- **The catch:** the person using it cannot change anything at all. That
  is the entire point, and it will frustrate anyone who wants more.
- **Feels like:** a self-checkout or a library catalogue terminal.

*Note:* restriction should read as focus, not as poverty. A locked
machine gets the same themes and the same visual care as every other
archetype. A school laptop that looks like a prison is a school laptop
children learn to hate.

## Taskbar — "The familiar one"

A bar runs along the bottom. Every window gets a button on it, so you
click a button to come back to something. A button at the left lists all
your programs. Windows overlap, move and resize.

- **Best for:** someone who has used a computer for years and does not
  want to learn anything new.
- **The catch:** windows end up on top of each other and get lost behind
  one another — the oldest complaint in computing, and we implement it
  faithfully rather than pretending otherwise.
- **Feels like:** every office PC of the last thirty years.

## Dock — "Favourites along the edge"

A strip of your most-used programs sits at the bottom, always in the
same order, whether running or not — so the thing you want is always in
the same spot. Anything else, you find by typing its name.

- **Best for:** someone who uses the same handful of programs constantly
  and wants them in a fixed, muscle-memory place.
- **The catch:** programs that are open but not in your favourites are
  harder to find again, and typing to search is a habit some people
  never form.
- **Feels like:** a shelf of the tools you reach for most.

**Taskbar or Dock?** They look similar and are not. *A taskbar lists what
is **open**. A dock lists what you **use**.* "Which of my windows" and
"which of my favourites" are different questions, and people have strong
preferences once they notice which one they are asking.

## Workbench — "Panes and keyboard"

Windows never overlap — they divide the screen between them
automatically, so everything open is visible at once. You move around
mostly with the keyboard, across several separate screens.

- **Best for:** someone technical, or anyone who spends all day at the
  machine and will invest an afternoon in learning it.
- **The catch:** **it expects you to learn keyboard shortcuts. It will
  feel hostile on day one**, and it is a poor choice for anyone who
  wanted the computer to be simpler.
- **Feels like:** nothing, honestly. This one you learn.

This is the only archetype allowed to require learning, and the chooser
says so in those words, so nobody picks it by accident.

---

## Choosing

Three questions settle it for almost everyone:

1. **Should someone be able to change this machine?**
   No → **Locked**. Done.
2. **Do you want to see two things side by side?**
   No → **Rail** or **Tiles**. Rail if you switch often, Tiles if you do
   one thing for a long stretch.
   Yes → question 3.
3. **How do you want to switch — by picking from what is open, by
   picking from your favourites, or with the keyboard?**
   Open → **Taskbar**. Favourites → **Dock**. Keyboard → **Workbench**.

The choice is not permanent. `aurshell --layout <id>` switches at any
time, and an administrator can pin it with the policy file.

---

## The band, which belongs to none of them

Along the bottom of every archetype is a strip that is not part of any
of them. It is always there, it is in the same place in all six, and
nothing on the screen can cover it.

| | |
|---|---|
| **Help** | one thing, in one place, in every archetype |
| **Internet** | the wifi in range, and joining one |
| **Smaller** / **Bigger** | the size of every word on the machine |
| **Turn off** | shutting the computer down properly |

Those four exist because each was a thing a person could not do at all.
There was no way to shut the machine down except holding the power
button in. There was no way to make the words bigger, on a product
whose median user has presbyopia. And there was no way to get onto
wifi, on a machine whose whole promise is that she can hand us her
computer — NetworkManager was installed and running from the first
image, and there was not one pixel between it and her.

**How it reaches six archetypes without six implementations.** It does
not draw into them and they cannot draw into it. The host hands the
archetype a surface that is shorter than the screen — same pixels, same
stride, fewer rows — and reduces `screen_h` to match. Every primitive in
`draw.c` and `font.c` already clips to the surface's height, so the band
is unreachable from a layout by construction rather than by agreement,
and not one archetype needed editing to gain it.

**Its colours are fixed**, which is a deliberate exception to the rule
below that nothing hardcodes a colour. A theme may restyle every pixel
in this product except the one that leads out of it: the control she
reaches for when the screen has become unreadable must not be styled by
the thing that made it unreadable. Its type has a floor for the same
reason — and the floor is a floor, not a ceiling, because the person who
picks the largest text is the person who needs the way out largest of
all.

**What a kiosk gets.** Help and nothing else. A kiosk has no way to turn
the machine off and no network of its own to choose: an administrator
sets a kiosk's network, and a public terminal whose users can point it
at any nearby wifi is a different product. `allow_network_change` in the
profile is the switch for the ordinary builds, and it is separate from
`allow_settings_change` — a build that pins the theme has not thereby
said the owner must retype the wifi password.

`src/aurshell/foot.c` is the band; `src/aurshell/net.c` is the wifi
panel behind its Internet button.

---

## For builders

```
shells/<id>.shell     the interaction model  (this document)
themes/<id>.theme     colours, type, geometry  (docs/THEMING.md)
profiles/<id>.profile packages, locale, policy
```

A build is one of each. To make a new product:

```sh
cp shells/tiles.shell shells/clinic.shell    # start from the closest fit
$EDITOR shells/clinic.shell
./build/forge build clinic
```

Each layout is its own translation unit exposing one `shell_layout`
vtable (`src/aurshell/layouts/*.c`) against the contract in
`src/aurshell/shell.h`. Adding a seventh archetype means adding one
file and one `.shell`; it does not mean touching the other six.

Preview any combination without booting anything:

```sh
shellpreview shells/dock.shell themes/sandstone.theme /tmp/x.png 1600 900 3
```

### Deliberate constraints

- **No archetype may hardcode a colour.** Everything comes from the
  theme, so all six work in light and dark.
- **One function owns geometry**, called by both painting and
  hit-testing. Deriving them separately is how a UI ends up off by the
  width of a shadow and feeling haunted.
- **A geometry function takes what it needs, not what the machine
  happens to be doing.** `foot_buttons()` and `net_layout()` are pure
  functions of the policy, the panel size and the text size, so
  `tools/targets.c` can ask them about every screen at every size
  without a radio, a daemon, or a person pressing things. A geometry
  function that can only be asked about the current state is one that
  nothing checks — and the 44-pixel floor in docs/EASY.md is then an
  intention rather than a rule. It has already caught three real
  defects that are invisible at the default text size.
- **Icons and the clock are shared helpers.** Six independently written
  renderers must not drift into six icon sets. A mark is a solid
  silhouette with its detail knocked out in the PAPER colour the
  caller just filled, so `shell_icon_draw()` takes both inks; passing
  the wrong paper shows, which is the point.
- **The six archetypes must not share a composition.** They shared one
  for a while — halo disc, centred mark, centred name, centred hint,
  reimplemented six times — and the result was that six deliberately
  different interaction models all read as the same product. Differing
  behaviour deserves differing layout: an index, a broken grid, a
  strip, a bar, a shelf, a tiler.
- **Every archetype states its tradeoff** in its `.shell` file, and the
  chooser shows it. A menu that only lists upsides is useless for
  choosing.
