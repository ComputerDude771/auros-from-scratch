# v3: everything that needs fixing

This folder lists every known problem with AurOS that should be dealt
with before, or in, the next build. **v3** comes after the v2 image and
test installer currently on branch `image-desktop`. Written 2026-09-26.
Nothing else in the repository was changed to write it.

Sources: the first real-PC attempt (below), plus the project's own
lists: `docs/RELEASE.md`, `HANDOFF.md` on both branches,
`docs/handoff/OPEN.md`, `STATE.md`, `docs/FERRY.md`, `docs/PLAN.md`,
`docs/results/README.md`. Each was checked against the code on
`claude/laughing-cray-ayao6i` where possible.

## The first real PC (2026-09-26)

`AurOS-Installer-test.exe` (SHA-256 `3e32e929…3e875`, built from
`cd437e1`) was downloaded onto a real Windows PC. What happened:

1. **Avast** "Suspicious file detected", then "This needs a closer look".
   It uploaded the file to Threat Labs and blocked it for "a few hours".
2. **Windows:** "The application has failed to start because its
   side-by-side configuration is incorrect."

The installer never started. Nothing on the PC was changed.

## The list

| # | File | Area | Worst item |
|---|------|------|------------|
| 1 | [01-sxs-manifest.md](01-sxs-manifest.md) | Installer won't start | **Blocker.** The manifest has `--` inside an XML comment. Root cause confirmed. |
| 2 | [02-antivirus-block.md](02-antivirus-block.md) | Antivirus | **Blocker for users.** Avast blocks it. Nobody planned for third-party antivirus. |
| 3 | [03-testing-gap.md](03-testing-gap.md) | Testing | Nothing has ever started the `.exe` on real Windows. Wine hid 1. |
| 4 | [04-republish.md](04-republish.md) | Release steps | The broken `.exe` is still the download. A checklist for v3. |
| 5 | [05-installer-wizard.md](05-installer-wizard.md) | Installer features | No "Put Windows back" button, no memory-stick page, no screen reader support, no profile picker. |
| 6 | [06-first-boot-aurfirst.md](06-first-boot-aurfirst.md) | First boot | The `aurfirst` fix isn't merged or in any image. Test case C isn't real. |
| 7 | [07-images-and-build.md](07-images-and-build.md) | Images, build, hosting | No Firefox, images need rebuilding, image hosted on a git branch, **no update channel**. |
| 8 | [08-ferry-migration.md](08-ferry-migration.md) | Moving from Windows | Wi-Fi and Chrome/Edge passwords don't move. Never tested on real data. |
| 9 | [09-real-hardware.md](09-real-hardware.md) | Real hardware | `plat_win.c` and WinHTTP have never run on a real PC. Only one firmware (OVMF) tested. |
| 10 | [10-docs-and-website.md](10-docs-and-website.md) | Docs and website | The website is out of date both ways. The two handoffs disagree. The "one file" promise vs 8 manual steps. |
| 11 | [11-release-blockers.md](11-release-blockers.md) | Non-code | Legal entity, code signing, host, licence, insurance. |

## Suggested order for v3

1. Fix 1 (the one-line manifest fix plus a build check), and put a
   "don't use" note on the current download (4).
2. Merge the `aurfirst` fix (6.1), then rebuild the installer and the
   images (7.2).
3. Start the new `.exe` on real Windows before publishing it (3), and
   check it on VirusTotal (2).
4. Run the console `aurbridge.exe` read-only on the test PC, which is
   the first real run of `plat_win.c` (9), then a spare-PC install.
5. Update `TRY-IT.md` and the website (10), and document antivirus (2).
6. Then the feature gaps (5, 8), in the order real testers hit them.
7. The non-code blockers (11) run alongside all of this. The legal
   entity comes first.
