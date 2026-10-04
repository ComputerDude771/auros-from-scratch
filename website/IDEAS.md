# Website: the page is the drive

The first redesign read like every other product site: a hero, three
cards, a features grid, a theme switcher. This one starts from a single
idea and lets everything else follow from it.

**The old PC is talking, and the page is its hard drive.**

You scroll down a disk the way the disk is addressed, LBA 0 to the last
sector. The machine narrates in the first person ("I'm 11 years old. I
still work."), because it is the one with something to lose.

| on the page | what it is on a disk | where |
|---|---|---|
| the top line | LBA 0, protective MBR | `index.html` |
| the partition table under the drive | the primary GPT at LBA 1, and also the menu | `#gpt-rows`, drawn by `drive.js` |
| each section, with a coloured edge and its sector range | a partition: EFI, Windows (C:), the gap / AUROS-ROOT, AUROS-BOOT, Recovery | `[data-part]` |
| the head in the left margin, sliding along a to-scale map | the read/write head; the LBA you are reading | `.ruler`, `moveHead()` |
| the test results, as a SMART table | drive firmware; the head *parks* while you read it | `#smart` |
| the known problems, as bad sectors (open) and remapped ones (fixed) | the grown defect list | `#defects` |
| the footer and the download | the backup GPT header in the last sector | `#backup` |

Things you can do to it:

- **Change its age.** The sentence about why Windows 11 won't install
  changes with it, including the honest one past 14: "I may be too old
  for AurOS too" (BIOS-only PCs are turned away).
- **Peel its stickers** off the case. It complains.
- **Restart it into AurOS.** The page plays the real restart: the real
  step labels and sentences from `src/aurscreen` and `src/aurstage`, the
  bar shrinking Windows and filling AurOS, the table at LBA 1 flipping
  at the end, then AurOS's real first-start question.
- **Pull its plug** at any moment with the power button ("Ow."). The
  outcome is the one `docs/results/powercut.txt` records for the named
  instant the cut landed in, with the caveat that the run used a memory
  stick (v3).
- **Put Windows back.** Before the restart the page saved Windows'
  section and took its SHA-256; putting Windows back restores it from
  that copy and shows both fingerprints side by side.

Every claim is still checked against the repository: test counts are the
transcripts' own, the restart's sentences are the programs' own strings,
and the one number that matters next (real PCs resized: 0) is red.

Kept from the first redesign: `desktop.html` (click around AurOS),
`wallpaper.html` (press your own wallpaper) and `guide.html` (one page to
print). The other pages redirect into the matching partition.
