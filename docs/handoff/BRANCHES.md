# Branches, and the two chats

> **Update, 2026-09-26.** The two lines below were merged into
> `claude/confident-johnson-hxevk4` (merge commit `eed8beb`), which is
> now the only development branch; the `aurfirst` fix `2d85777` is in
> it and in the v3 image. The merge had one conflict, `HANDOFF.md`
> itself (both lines had added one). What follows is how things stood
> on 2026-09-25, kept as history.

Repository: `ComputerDude771/auros-from-scratch`. As of 2026-09-25 there
are three branches on GitHub.

## `claude/laughing-cray-ayao6i`: the ACTIVE line of work

This is where the most recent work is. A second chat ("Repository
progress check") branched off `claude/linux-distro-from-scratch-tt5dx1`
at commit `4170f0a` and has since made 13 commits. Judging by their
commit messages (read the branch itself for detail):

- a **no-stick mode**: the installer downloads the image in pieces
  instead of needing a memory stick, with a decompressor and a piece
  downloader
- **the installer's choices reach AurOS**: language, keyboard, time
  zone, look, desktop
- **Secure Boot stays on**: preflight asks the firmware whether it
  trusts shim's key
- `docs/TRY-IT.md` (trying the test build on a spare PC) and
  `docs/RELEASE.md`
- results including `nosticktest 45/45` and a real-image first-boot
  run for the choices

Tip at handoff time: `a93f468` ("results: nosticktest 45/45 on the
Secure Boot commit").

## `claude/linux-distro-from-scratch-tt5dx1`: the original line

The first chat worked here from the start of the project. It is
**stopped** and holds nothing in flight. Tip: `ef2300a`, plus the
commit that adds these handoff files.

It has **two commits the active branch does not have**:

| commit | what |
|---|---|
| `2d85777` | `src/aurfirst/`: AurOS picks **its own** firmware entry by the partition it names, not the first entry called "AurOS". Fixes "try AurOS, go back to Windows, try again", where a dead entry was re-armed and promoted and the machine went back to Windows right after the person said AurOS works. `aurfirsttest` goes to 54 checks. Also adds `tools/firstboottest.sh` and new `tools/efivarstore.py` operations (real `HD()` entries, `BootOrder`, `BootNext`, stale entries). |
| `ef2300a` | `tools/firstboottest.sh` results and state; `docs/AURBRIDGE.md` section "Which AurOS is this AurOS"; `tools/README.md` row for the test. |

**Merging this branch into the active one is clean.** A `git merge-tree`
dry run on 2026-09-25 reported no conflicts. Do that merge before
building images, or the images will lack the `aurfirst` fix.

## `image-desktop`: a download, not source

Created by the other chat. It holds no source code: a test installer
(`AurOS-Installer-test.exe`) and the desktop image, gzip-compressed and
cut into pieces under GitHub's 100 MB file limit so the no-stick
installer can download it. Its `README.md` lists the SHA-256 of the
image and of every piece.

That image was built from the active branch, so it **does not have the
`aurfirst` fix** from `2d85777`. Fine for a first real-PC test; rebuild
after the merge.

## What happened between the chats

The user started the second chat while the first was working, then
asked for **one chat only**, the second one. The first chat relayed
three messages into the second through one-time Routines, all now
deleted:

1. "stand down"
2. a correction ("no, YOU continue; the first chat is stopping")
3. the Secure Boot task: keep Secure Boot on, and start the installer
   through Microsoft-signed shim, then grub, then the kernel

The first message was a mistake that the second corrected about a
minute later. If the second chat's history shows it pausing briefly,
that is why.
