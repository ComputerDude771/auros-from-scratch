# HANDOFF — start here

Written 2026-09-25 by the agent working on branch
`claude/linux-distro-from-scratch-tt5dx1`, for whoever picks the project
up next. Nothing else in the repository was changed to write it.

## What this is

**AurOS** turns a Windows PC into a Linux one without the person ever
seeing a firmware screen. They download one `.exe`, a wizard explains and
asks, the machine restarts **exactly once**, and it comes up in AurOS
with their files. Until they say "it works", every power-on can still
reach Windows, and "Put Windows back" restores it.

Two halves:

- **AurBridge** (`src/aurbridge/`): the Windows wizard. It checks the
  machine, asks for consent, prepares, and arms one restart.
- **aurstage** (`src/aurstage/`): the staging environment that runs
  after the restart. It shrinks Windows, writes AurOS, verifies it,
  commits the new partition table in one sector, installs the boot
  chain, and hands over.

After that, **aurfirst** (`src/aurfirst/`) and the desktop's welcome
panel ask "does this work?" and only then make AurOS the default.

## Read in this order

1. **`docs/handoff/BRANCHES.md`**: there are three branches and two
   chats' worth of history. Read this before touching git.
2. **`docs/handoff/STATE.md`**: what is proven, by which test, with what
   numbers, and what is not proven.
3. **`docs/handoff/OPEN.md`**: what is left, in priority order.
4. **`docs/handoff/TRAPS.md`**: mistakes that already happened on this
   project. Most of them look reasonable until they bite.
5. The project's own docs:
   - `docs/PLAN.md`: plan and risk register
   - `docs/AURBRIDGE.md`: installer design, phase by phase
   - `tools/README.md`: what every test proves
   - `docs/results/`: raw test transcripts
   - on the active branch only: `docs/TRY-IT.md` and `docs/RELEASE.md`

## The one-paragraph status

The whole install path works end to end on **simulated** machines under
real UEFI firmware (OVMF), including Secure Boot with Microsoft's keys,
power cuts at 17 named instants, and putting Windows back. The real
desktop image has been booted through first-boot "it works" / "it does
not" with real systemd and real firmware variables.

**Nothing has run on a real PC yet.** That is the main remaining risk.
The other chat has built a test installer you can download (see
BRANCHES.md) so a real PC can be tried.

## What the user has asked for, standing

- **Push to GitHub often.**
- **Lots of adversarial review, using several independent reviewers.**
- **At every step, ask "why?", because humans make errors.** Prove a
  check fails when the thing it checks is broken before believing it.
- **Keep it original.** Look at how others solved something when stuck,
  but don't copy it.
- **Spending credits is fine; raising the credit limit is not.**
- **"Genuinely just a wizard."** As easy as possible for the end user.
  They explicitly do not want users sent into firmware settings.
  Secure Boot stays on; the program cannot turn it off, and shouldn't
  need to.
- **One chat at a time**, because it is easier to track. This handoff
  exists because the user is switching agents.
- **No pull requests unless asked.**

## Product rules the code is built around

- Refuse first: anything uncertain is a refusal, made before anything
  is changed.
- The machine can always go back to Windows.
- Nothing irreversible until AurOS has booted and the person has said
  it works.
- One atomic commit point per destructive phase: LBA 1.
- The machine's own EFI partition is never mounted or reformatted (R12).
  AurOS writes only under `\EFI\AurOS\`.
- `BootOrder` is written only by `aurfirst confirm`. Before that, only
  the one-shot `BootNext`.
