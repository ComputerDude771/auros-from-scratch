# Website ideas

The brief was "hella creative ideas for the website". This file lists
all of them, says which ones are built and where, and ranks the ones
worth building next. Two rules applied to every idea:

- **Every claim is checked against the repository.** The project's
  brand is that it does not round up. Where the docs disagree with each
  other, the primary evidence wins (a test transcript beats a summary).
- **No template look.** `docs/DESIGN.md` applies to the site as it does
  to the desktop: one accent that means "you are here / press this",
  hierarchy carried by type, hairlines instead of shadows, one
  deliberately off-axis element (the 28% margin column, the same
  fraction the letterpress wallpaper puts its tint block at).

Static HTML/CSS/JS, no build step, no CDN, no tracker. The site loads
nothing from anywhere but its own server.

## Built

| # | Idea | Where |
|---|---|---|
| 1 | **A live model of the AurOS desktop.** The "Everything in a row" shell the desktop image ships with, at 1024×600 (the smallest screen AurOS is designed for), scaled to fit. Real app names and hints (`shellcommon.c`), real Settings rows and the real two-press, 1.5-second "Put Windows back" rule (`settings.c`), the real first-boot question and every one of its follow-up panels (`welcome.c`), and the restore's real screen text (`rescue.c`). Six themes switchable from outside and four from inside Settings, as in AurOS. A working calculator, a letter pad, Photos drawn by the wallpaper code, a chord at the Settings volume, and a Files app with the real "what could not come across" list. Keyboard (arrows, Enter, Esc, Tab), swipe, Fit / Actual size, full screen. | `desktop.html`, `assets/desktop.js` |
| 2 | **The theme file as text**, updating live as you switch themes in the model. | `desktop.html` |
| 3 | **The wallpaper press.** A line-by-line port of `src/common/wall.c` (all seven styles and the post-processing). At seed 0 it was checked against the compiled C renderer for all six themes and four more styles: at most one level off in one channel. A seed (labelled as this site's addition: AurOS has none) gives each visitor their own picture by the same rules. Edit the `wall_*` numbers and colours, download a PNG at this screen's size or common sizes, copy the result as theme-file lines, or share a link (`#theme=moss&seed=42`). Renders in a Web Worker. | `wallpaper.html`, `assets/wall.js`, `assets/wall-worker.js`, `assets/wallpaper.js` |
| 4 | **The home page is a press sheet drawn for you.** The hero is a full-bleed letterpress wallpaper with a fresh seed per visit; the headline is set on it, the 28% tint block holds a colophon (what it is, restarts, Secure Boot, "proven on a real PC's disk: not yet"). "Draw another" and "Keep this one" (opens the press on that seed). Sandstone's wallpaper in light mode. | `index.html`, `assets/site.js` |
| 5 | **The one restart, scrubbable.** Every step of `install.c` in order, with the screen as it looks at that moment (installer window, the accumulating text console, AurOS's question) and the disk drawn twice: **the map** (what the partition table says) above **the ground** (what is actually written). The whole design is visible at a glance: the ground is written first, then the map changes. Three cells show the three table writes (entries, LBA 1, spare). The firmware's start-up order, the one-time entry and where the way back lives are shown at each step. | `restart.html`, `assets/restart.js` |
| 6 | **Pull the plug anywhere.** The screen collapses, and the outcome is told twice: what a person would see when switching on again, and, at each of the 18 named instants, the actual check lines from `docs/results/powercut.txt` ticking in. Steps with no named instant say their answer comes from the design, not a test. The middle of the shrink is shown as the one bad row. | `restart.html` |
| 7 | **Putting Windows back, scrubbable too**, with its five instants ("running it again finishes the job"). | `restart.html` |
| 8 | **Stick / no-stick switch.** The power-cut run used a memory stick; the downloadable build has none. The toggle shows both orders (capture to stick vs. into memory; second copy after the commit vs. the way back first), and the page says plainly that the power cuts were not run in no-stick mode. | `restart.html` |
| 9 | **The 18 instants as buttons**: press `commit-sector` and land there with the plug already pulled. | `restart.html` |
| 10 | **"Will my PC make it?"**, honestly. What a browser can see (Windows 10 vs 11 via UA client hints where offered, ARM vs Intel/AMD, threads, rounded memory, screen size against 1024×600), each with a verdict and a plain note, and "disk, firmware, Secure Boot, encryption: invisible to web pages". Then the table of what only *Check this PC* sees, what the installer does about each, and how to look yourself. Includes the Secured-core stop page screenshot. | `check.html`, `assets/check.js` |
| 11 | **The Windows 10 calendar.** Days until 12 October 2027 (the extended end of free consumer ESU), a month strip from regular end of support (14 Oct 2025), framed as "a date, not a fault". | `index.html`, `check.html` |
| 12 | **The Secure Boot chain, walked.** Firmware → Microsoft-signed shim → Canonical-signed GRUB → Canonical-signed kernel → aurshell, each link pressable (arrow keys too), with what signed it and what it checks. Walks itself once when scrolled into view (not with reduced motion). Says outright that aurshell is *not* in the signature chain. Plus the table of what happens on PCs that do not trust the key. | `how-it-works.html`, `assets/chain.js` |
| 13 | **The honesty page as a ledger.** A tally (7 proven on real Windows, 6 on simulated PCs, 3 not proven, 10 not done), every test with its check count, and what is not done, including the uncomfortable ones (no Firefox in the published image; kernel/grub/shim updates held back; image hosted on a git branch). | `safety.html` |
| 14 | **Errata.** The site's own past claims that were wrong ("two restarts", "approve our key", "type back your recovery key", "your Wi-Fi passwords come with you", teal Nocturne), struck through, with what is true now. | `safety.html#errata` |
| 15 | **A one-page guide for the person you are helping.** Large type, blanks for a name and phone number, the one moment not to switch off boxed. Prints on one sheet of A4 or Letter (checked). | `guide.html` |
| 16 | **The site is themed by AurOS's own theme files.** The Nocturne/Sandstone switch uses the values from `themes/nocturne.theme` and `themes/sandstone.theme`, and is labelled with the theme's name. Follows the system setting until pressed. | `assets/nocturne.css`, `assets/site.js` |
| 17 | **The download gate, rewritten for the test build** that exists: five boxes from `docs/TRY-IT.md`, the real link, the SHA-256 of the published file (checked against the `image-desktop` branch), how to check it, and "the README beside the file is the one to trust over this page". | `download.html` |
| 18 | **A colophon instead of a cookie banner**: what the site loads (nothing external), what it stores (the theme choice, only if pressed), how it is coloured. | every page |

Every interactive piece works with a keyboard, respects
`prefers-reduced-motion`, and fits a 375px-wide phone without
horizontal scrolling. Pages read fine with JavaScript off; only the
models need it, and say so.

## Next, in the order worth building

1. **Hash checker, in the browser.** Drop the downloaded `.exe` on the
   download page; SubtleCrypto computes its SHA-256 locally and compares
   it to the published one. Nothing uploaded. Small, and it turns
   "check the checksum" from homework into one gesture.
2. **Verdict decoder.** "It stopped and said `verdict=no-network`."
   Type the word from the photo, get the plain sentence, whether
   anything changed, and what to do. The words are already in
   `install.c` / `rescue.c` (`give_up(..., "no-room")` etc.); this is a
   table and a text box.
3. **Fresh screenshots.** Every screendump in `docs/shots/` predates
   the brass Nocturne. `aurshell --png` and the wizard's own screenshot
   mode can re-shoot them; then the site can show the real thing in
   every theme instead of captioning old pictures.
4. **The installer, page by page.** A clickable replica of the wizard
   (Welcome → Check this PC → Fix these for me → Ready → Restart now),
   once there are current screenshots or the wizard's page text is
   pulled out of `wizard.c`.
5. **Theme forge.** Edit a `.theme` in the browser with `aurora`'s
   colour algebra (`@accent|mix:bg:70@`, `|on`, `|lighten:20`), see the
   desktop model and the wallpaper change, and check every rule against
   the 3:1 floor `tools/contrast.c --strict` enforces. Download the file.
6. **Profile builder for organizations.** A form for the policy keys
   with the profile file written beside it, showing only the lines that
   differ from `desktop` (inheritance made visible).
7. **All six shell layouts in the model.** Only "Everything in a row" is
   built; "One thing at a time", "The familiar one", the dock, the
   workbench tiler and the locked kiosk each have a `.shell` file and a
   tagline already.
8. **Field reports.** When real PCs start reporting, a static table:
   maker, model, year, the `verdict=` line, what happened. The single
   most persuasive page this site could have, and it cannot be faked.
9. **"First megabyte last", animated.** A ten-second explainer of why a
   half-written AurOS can never be mistaken for a filesystem.
10. **Text size that follows "How big the words are".** Let the site's
    own type scale follow the slider in the model.
11. **A slow-PC mode for the model**: a 2013 laptop's 1366×768 panel
    and its colours, to show the target honestly.
12. **Translations of the guide**, starting with the languages the
    multilingual profile ships.
13. **A changelog feed of the ledger** (static Atom file), so "is it
    ready yet?" can be subscribed to instead of asked.
14. **An audio version of the guide**, read slowly.

## Considered and rejected

- **An e-waste calculator with tonnes and CO₂.** The repository has no
  sourced numbers, and invented ones would be the first false claim on
  the site. The home page makes the argument in words instead.
- **A countdown framed as fear ("your PC becomes unsafe in…").** The
  PC does not change on that day; the copy says so.
- **Testimonials.** There are no users yet.
- **A "download" button without the gate.** The test build is for spare
  PCs; the five boxes stay.

## Things found while building this, outside the website

- `docs/results/powercut.txt` lists **18** named instants (13 install +
  5 restore), and its 138 checks add up only with 18. `HANDOFF.md`,
  `docs/PLAN.md`, `docs/handoff/STATE.md` and `docs/results/README.md`
  say 17 ("twelve during the install"). The site says 18.
- `README.md` says the shim is "dual-signed (Microsoft UEFI CA 2011 and
  2023)"; `docs/AURBRIDGE.md`, `docs/SIGNING.md` and STATUS 5.7 say it
  carries only the 2011 signature and PCs trusting only 2023 are
  refused. The site follows the latter.
- `themes/nocturne.theme`'s `theme_description` still says "aurora-teal
  glow"; the palette is brass. AurOS's own Settings shows that line.
- `src/aurstage/probe.c`'s remedy for `no-network` and `no-drivers` says
  "Nothing has been changed", but the probe runs after the shrink and the
  write, so Windows is already smaller.
