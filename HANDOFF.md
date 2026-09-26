# HANDOFF — start here

Rewritten 2026-09-26. This replaces the two handoffs the project had
(one per branch, each saying its own branch was the one to read). There
is one line of work now.

## What this is

**AurOS** turns a Windows PC into a Linux one without the person ever
seeing a firmware screen. They download one `.exe`, a wizard explains
and asks, the machine restarts **exactly once**, and it comes up in
AurOS. Until they say "it works", every power-on can still reach
Windows, and "Put Windows back" restores it.

| Part | Where | What it does |
|---|---|---|
| **AurBridge** | `src/aurbridge/` | The Windows installer. Checks the PC (read-only), asks, downloads the image, arms one restart. |
| **aurstage** | `src/aurstage/`, `build/staging` | What the restart boots: shrinks Windows, writes AurOS, verifies, commits the partition table in one sector, hands over. Also the restore (`aurstage.restore`). |
| **aurfirst** | `src/aurfirst/` | AurOS's first boot: "does it work?" Yes makes AurOS the default; no goes back to Windows; `putback` restarts into the restore. |
| **aurshell** | `src/aurshell/` | The desktop, painting to DRM/KMS. Its welcome panel and Settings ask aurfirst for things through one word each. |
| **Ferry** | `src/ferry/` | Moves files and settings across from Windows. |
| **Forge / mkimage** | `build/forge`, `build/mkimage`, `build/all` | Build the images from Ubuntu 24.04 packages. |

## Branches

| Branch | What it is |
|---|---|
| `claude/confident-johnson-hxevk4` | **The development branch.** Everything is here. |
| `image-desktop` | No source. The published test installer and the desktop image in gzip pieces (`v1` top level, `v2/`, `v3/`), with a README of hashes and of what was tested where. |
| `claude/laughing-cray-ayao6i` | History: merged into the development branch at `eed8beb`. Do not work here. |
| `claude/linux-distro-from-scratch-tt5dx1` | History: the original line, and the repository's default branch. Everything on it is in the development branch. |

`docs/handoff/BRANCHES.md` has how the branches came to be.

## Read in this order

1. **`docs/issues/v3/STATUS.md`**: what is done, what is partly done,
   what is not, item by item. `docs/issues/v3/` has the full write-up of
   each problem.
2. `docs/HANDOFF-RUNBOOK.md`: the exact commands to build, test and
   publish.
3. `docs/handoff/TRAPS.md`: mistakes that already happened here. Most
   look reasonable until they bite.
4. `docs/handoff/STATE.md`, `docs/PLAN.md`, `docs/AURBRIDGE.md`,
   `docs/RELEASE.md`, `tools/README.md`, `docs/results/`.
5. `docs/TRY-IT.md`: what a person testing on a spare PC is told.

## Where it stands

**Proven on real Windows** (`.github/workflows/windows.yml`, a GitHub
Actions VM, on every push): the installer starts, its preflight runs
read-only against a real machine, its navigation and selftest pass,
WinHTTP downloads and checks the whole image, the published file is
started from its public link, and the build that failed on the first
real PC fails there too.

**Proven in simulation, under real UEFI firmware** (OVMF, Microsoft's
keys, Secure Boot on): install and put back, with and without a memory
stick (`installtest` 37, `nosticktest` 45), power cuts at 17 named
moments (`powercuttest`), the real image's first boot and its choices
(`firstboottest`, `choicesboottest`), and "Put Windows back" from the
button in AurOS through AurOS's own menu to the restore
(`putbacktest`).

**Not proven: a real PC's disk being resized and written.** The first
real-PC attempt (2026-09-26) could not start the installer at all; that
is fixed, and the tester has the fixed build. Their result is the next
piece of evidence. A photo of the staging screen's `verdict=` line is
what to ask for if anything stops.

## Published right now

See the `image-desktop` branch's README for the hashes. The link people
are given:

https://github.com/ComputerDude771/auros-from-scratch/raw/image-desktop/AurOS-Installer-test.exe

Right now: installer `c70045fb` (built from `1bef5d7`), which downloads
the v3 image `5dee5f44` from `image-desktop/v3/`. `windows.yml`'s
`PUBLISHED_SHA256` and `IMAGE_*` are pinned to them; change them in the
same commit as the files.

## What the user has asked for, standing

- **Push to GitHub often.** No pull requests unless asked.
- **Lots of adversarial review; ask "why?" at every step.** Prove a check
  fails when the thing it checks is broken before believing it passes.
- **Keep it original.**
- **"Genuinely just a wizard."** No firmware screens; Secure Boot stays on.
- **Do not hand them an installer that has not been built and tested end
  to end** — and "tested" now includes starting it on real Windows.
- They write informally and want plain answers: what works, what does
  not, and a link.
- **Spending credits is fine; raising the credit limit is not.**

## Product rules the code is built around

- Refuse first: anything uncertain is a refusal, made before anything
  is changed.
- The machine can always go back to Windows.
- Nothing irreversible until AurOS has booted and the person has said it
  works.
- One atomic commit point per destructive phase: LBA 1.
- The machine's own EFI partition is written only under `\EFI\AurOS`,
  and only from Windows. AurOS never writes it; it reads it to find the
  restore.
- `BootOrder` is written only by `aurfirst confirm` (and demoted by
  `decline`). Everything else uses the one-shot `BootNext`.
