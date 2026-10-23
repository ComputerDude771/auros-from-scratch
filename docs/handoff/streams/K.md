# Stream K: help, support and legal drafts

Brief: `docs/handoff/STREAMS.md` (row K and "K help, support, legal drafts").
Branch `claude/brave-hypatia-x72g1m` only; push recipe in STREAMS.md rule 1.

## Goal

1. `website/advanced.html`: "for someone who knows computers". The installer's
   can't-install page gives its address (`src/aurbridge/fix_win.c`, `ADVANCED`,
   commit b4ab898) for BIOS-mode PCs and Intel RST "RAID On"/VMD NVMe drives.
2. In-OS help pages in `rootfs/usr/share/auros/help/`: getting online, installing a
   program, printing, video calls, getting back to Windows, "AurOS will never call you".
3. `SECURITY.md` (how to report a vulnerability), `docs/SUPPORT.md` (how a person gets help).
4. `docs/legal/`: DRAFTS of terms of use, privacy policy, disclaimer, each marked
   "Draft: must be reviewed by a lawyer before use". Plain language. Not legal advice.

## Done and pushed

- c01a63c `website/advanced.html` (BIOS to UEFI with mbr2gpt; RAID On / VMD to AHCI via
  one Safe Mode start; copy files + pause BitLocker first; when NOT to do it; the way back).
  Footer link in `website/index.html`. `tools/site/check.mjs` now also measures
  advanced.html (18px, 4.5:1, 320px no sideways scroll, footer link). `node tools/site/check.mjs`
  = 105 passed, 0 failed; shown to fail with the footer link removed and 16px command text.

- 8c4bfff `SECURITY.md` (GitHub private vulnerability reporting; owner must enable it once in
  Settings -> Code security) and `docs/SUPPORT.md` (Help button, help pages, website pages,
  public issues, no phone line).
- `docs/legal/{README,TERMS,PRIVACY,DISCLAIMER}.md`: drafts, every file banner-marked
  "DRAFT: must be reviewed by a lawyer before use"; `[OWNER: ...]` placeholders, `[LAWYER: ...]`
  questions, `[CHECK: ...]` facts to re-verify (privacy host list: do a packet capture of an idle
  AurOS VM and make the table match). README.md lists what the owner must do.

- ced6642 help pages `rootfs/usr/share/auros/help/` (index, getting-online, programs, printing,
  video-calls, windows, never-call, help.css). 17ffb48 `tools/helptest.py` (33 checks, each shown
  to fail on a broken copy). 2045c6f helptest step in `.github/workflows/tests.yml` (stream E's file,
  3 lines).

## Half-done / not pushed

- **Nothing opens the help pages yet.** Next step 5 below (the Help-panel button) is the entry point.
- When S2's "Get me out of this page" control lands, mention it in `never-call.html` (today it
  says Close this / Turn off, which exist).

## Next steps

1. (done) Help pages as self-contained HTML (no network, no scripts, 22px+ text, sandstone-like
   light colours, "Back to all help" links): `index.html` + `getting-online.html`,
   `installing-programs.html`, `printing.html`, `video-calls.html`, `back-to-windows.html`,
   `never-call.html`, shared `help.css`. `build/forge` copies all of `rootfs/` into the image
   (stage "THE WHOLE rootfs/ TREE"), so no forge change is needed.
2. (done, as tools/helptest.py) A test (new file): every page exists and links resolve, no http(s)
   requests, none of docs/EASY.md rule 2's forbidden words in visible text, font sizes.
   Prove it fails on a broken copy.
3. (done) SECURITY.md, SUPPORT.md. 4. (done) legal drafts.
5. Help-panel hook (shared aurshell files; do last, smallest edit): a button in the band's
   Help panel ("Show me how to do things") that runs `xdg-open /usr/share/auros/help/index.html`
   via a flag in `shell_ctx` handled in `main.c` with `run_detached`, shown only when the file
   exists; geometry in ONE function used by paint, click and `tools/targets.c`.

## Facts the pages must match (checked in code, 2026-10-23)

- Band at the bottom (`src/aurshell/foot.c`): Help, Internet (= wifi panel), Settings,
  Close this; right side Turn off, Bigger, Smaller. Turn off opens: "Turn it off",
  "Start it again", "Let it sleep", "Never mind".
- Wifi panel (`net.c`): "Press the name of your wifi." / "Type the password for X" /
  "Joining X. This can take a moment." / "You are connected to X." / wrong password:
  "That password did not work. Try typing it again?" / hint "It is usually printed on the
  bottom of your internet box." / "A cable from your internet box to this computer will work."
- The home "Internet" tile is the web browser (Firefox from Mozilla; fallback Epiphany).
  Two things are called Internet: say "the Internet button on the bar at the bottom" for wifi.
- Store: "Get more programs" (bag icon) on every look's home; gnome-software
  (Explore / Installed / Updates); Install needs no password; card "X is ready - Press here
  to open it". Downloaded installer files (.deb) do NOT install (lead decision 04:30): programs
  come only from Get more programs.
- First start question (`welcome.c`): "Yes, it all works" / "Let me look first" /
  "No, go back to Windows" / "Bring my files across".
- Settings rows: "How bright the screen is", "How loud the sound is", "How big the words are",
  "Battery", "Headphones and mice", "The time and date", "How this computer works", "How it looks",
  "Put Windows back" (-> "Remove AurOS and put Windows back", press again within the page;
  "First copy anything you want to keep onto a memory stick.").
- Boot menu (`rootfs/usr/lib/auros/grub.cfg.in`): 3 s, entries AurOS, Windows (if found),
  "Put Windows back (remove AurOS)" submenu with "No, keep AurOS" first.
- Printing: cups, cups-filters, avahi, gutenprint, printer-driver-hpcups, system-config-printer
  ("Print Settings" appears in the program list). S1 may remove cups-browsed (remote discovery).

## Traps

- A program-list entry whose Exec is `xdg-open` leaves a "Starting..." slot that turns into
  "This did not start" (session.c START_TIMEOUT_MS): the browser window's app_id never matches
  the entry. Don't use a .desktop entry + want_launch for help; open the pages directly.
- Adding a Documentation entry with default rank 6 named "Help" can push "Get more programs"
  off the rail home (rank ties sort by name). Another reason not to add one.
- docs/EASY.md rule 2 word list applies to the help pages (no "session", "verify", "process"...).
- `node tools/site/check.mjs` needs Playwright: global install at `npm root -g` works here.
- The main checkout `/home/user/auros-from-scratch` is shared: work in your own worktree.
