# The usability law

> "I need installation and every basic thing to be very easy to do for
> the common man and 68 yr old."

That is the product owner's constraint, stated twice, and it outranks
the rest of this repository's opinions. Where it conflicts with
elegance, with completeness, or with what other Linux desktops do, it
wins.

This document exists because "make it easy" is not actionable and
therefore does not survive contact with a deadline. What follows is the
same sentence, rewritten as things a person can check and a harness can
fail.

---

## Who she is

Not a persona invented to flatter a design. The specific, ordinary
person this product is for:

She is 68. She has used Windows since about 1998, so she has twenty-odd
years of habits and exactly one mental model of installing software:
download a file, double-click it, click Next until it stops. She is not
stupid — she is unpractised, which is a completely different thing and
is fixed by different means.

She is slightly afraid of breaking something that cost money. That fear
is the single most important fact about her. It means she does not
explore, does not experiment, and does not click things to find out what
they do. When she gets stuck she stops, and she may not come back.

Her eyes have presbyopia, which is near-universal by her age: small text
is not merely annoying, it is unreadable at the distance she actually
sits. Her hands may be less precise than they were; a small target is
not a small inconvenience, it is a miss.

Somebody else — a nephew, a neighbour, a son — set up her last computer.
That person is not available now. Everything in this product has to work
without them.

---

## The rules

Each of these is written so that it can be violated observably. A rule
nobody can check is a preference.

### 1. No terminal. Not ever, not as a fallback.

There is no task in this product whose answer is "open a terminal". Not
for wifi, not for installing something, not for fixing something, not
"for advanced users". The moment a path bottoms out in a command line,
that path is unfinished.

`allow_tty="no"` already exists as a policy flag and is enforced down to
refusing the VT-switch chord. That flag is about lockdown. This rule is
about the other builds: even where a terminal *exists*, no supported
task may require it.

### 2. Every user-facing word is one she uses.

A word she does not use is a word that stops her. The product may not
say, in anything she can read: *SSID, WPA, authenticate, credentials,
interface, adapter, driver, daemon, mount, unmount, partition,
repository, package, dependency, runtime, sandbox, sudo, root, binary,
executable, directory, path, shell, kernel, PID, process, buffer,
render, compositor, Wayland, X11, Wine, Flatpak, snap, deb, initialise,
configure, parameter, verify, validate, instance, session, allocate.*

Some of those are unavoidable in *code comments* and in developer
output. None of them is acceptable on a screen she is looking at.

The replacement is not a simpler synonym for the same idea. It is
usually a different sentence about what happens next.

> Not: "Failed to authenticate with the wireless network."
> Not: "Could not connect. Please check your credentials."
> But: "That password didn't work. Want to try typing it again?"

### 3. She can always make the text bigger.

Presbyopia is not an accessibility edge case for this product; it is the
median user. A machine whose text she cannot read is not a machine with
an accessibility problem, it is a machine she does not use.

The type scale lives in the theme (`themes/*.theme`, resolved by
`src/aurora/aurora` into `/etc/auros/shell.conf`), and the shell already
reloads its theme on `SIGHUP`. The mechanism exists. What must exist is
a way for *her* to reach it, in every archetype, without knowing what a
theme is.

### 4. Targets are big enough to hit.

Every interactive element is at least 44 pixels on its shorter side at
1024×600, the smallest panel this product supports. Not 44 at 1920×1080
and smaller when it scales down — 44 at the bottom of the range, because
that is where the old machines are.

`tools/hittest.c` already proves that what lights up can be clicked. It
does not yet prove that it can be *hit*. That is a harness waiting to be
written.

### 5. Nothing is time-limited, and nothing requires a gesture.

No element disappears on its own. No task requires a double-click, a
drag, a hover-then-move, a press-and-hold, or a chord. Each of those is
a specific failure mode for slower or less precise hands, and every one
of them has a single-click alternative that is no worse for anyone else.

Where the shell already dispatches on press rather than release, that is
correct and must stay.

### 6. Every state she can get into, she can get out of.

The most dangerous screen in this product is the one she reaches by
accident and cannot leave. This includes the ones we would not think of
as states: text set so large the buttons no longer fit, a theme whose
contrast she cannot read, a window moved somewhere strange, a setting
changed by a mis-click.

Anything she can change must be reversible by someone who does not
remember what they changed.

### 7. Help is one thing, always in the same place.

She will not search for help, will not read a manual, and will not
remember a keyboard shortcut. There is one thing, it is always visible,
it is in the same position in every archetype, and pressing it is never
destructive.

### 8. Failure states never blame her and never name a component.

The rule for every message she can see:

- Say what happened, in terms of what she was trying to do.
- Never name a program, a service, a file path or an error code.
- Always say what she can do next, even when the answer is "nothing —
  this needs someone to look at it".
- Never imply she did something wrong.

### 9. Tell her before, not after.

Anything that will not work after the migration should be said on the
Windows side, in AurBridge, before anything is destroyed — not
discovered afterwards. A person who finds out late feels lied to, and
they are right to.

This is the rule that makes the honest answer about Microsoft Office and
about Fortnite a feature rather than a disappointment.

---

## What this does not mean

It does not mean the product is for her *only*. The archetype system
exists precisely so that one build can be a locked-down school kiosk and
another can be a tiling workbench for someone who lives in a keyboard.

It means the *default* build is for her, that the basic tasks are
finished for her, and that no path that serves an expert is allowed to
be the only path.

It also does not mean hiding things. Hiding a capability she needs is
not simplicity, it is a different failure. Simplicity is fewer
decisions, not fewer abilities.

---

## Why this is a document and not a commit message

Because the pressure to violate it is constant and reasonable-sounding.
Every one of these rules will, at some point, be the thing standing
between a feature and shipping, and the argument against it will be
sensible: *advanced users expect a terminal; everyone knows what a
network is; nobody really sits that far from the screen.*

The counter-argument is that the product has exactly one proposition —
that a nervous person can hand us the computer with all their photos on
it — and every one of these rules is that proposition, restated for one
specific part of the machine.
