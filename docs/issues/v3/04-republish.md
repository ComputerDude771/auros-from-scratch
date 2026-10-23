# 4. What to redo once the manifest is fixed

**Severity:** follow-up. None of this is useful until issue 1 is fixed.

The broken `.exe` is still the file on branch `image-desktop`, and the
link given to testers points at it. Every download of it fails the same
way.

## Checklist

- [ ] **Decide what happens to the current download right now.** Either
      replace `AurOS-Installer-test.exe` on `image-desktop` with a note, or
      leave it and put a "known broken, do not use" line at the top of
      that branch's README. Otherwise the next tester hits the same wall.
- [ ] **Merge `claude/linux-distro-from-scratch-tt5dx1` into
      `claude/laughing-cray-ayao6i` first** (`docs/handoff/OPEN.md` item
      1). A dry run found it clean. Rebuilding the installer is the
      natural moment to pick up the `aurfirst` "which AurOS is this AurOS"
      fix (`2d85777`), and a rebuilt image would pick it up too.
- [ ] Fix `src/aurbridge/aurbridge.manifest` and add the build check
      ([01-sxs-manifest.md](01-sxs-manifest.md)).
- [ ] Rebuild `aurbridge-wizard.exe` → `AurOS-Installer-test.exe`.
- [ ] Re-run `tools/installtest.sh`, `tools/nosticktest.sh` and
      `tools/choicesboottest.sh`, and record the new transcripts in
      `docs/results/`.
- [ ] **Start it on real Windows** ([03-testing-gap.md](03-testing-gap.md))
      and record that as well. At minimum: the UAC prompt appears, the
      wizard opens, and *Check this PC* completes.
- [ ] Run it through VirusTotal and record the detections
      ([02-antivirus-block.md](02-antivirus-block.md)).
- [ ] Update the `image-desktop` README: new SHA-256, "built from"
      commit, what was tested *on Windows* versus under Wine, and add
      `3e32e929…` to the "earlier test installers" list with the note
      "did not start on Windows: malformed manifest".
- [ ] If the image is rebuilt as well (for the `aurfirst` fix), publish
      it as `v3/` next to `v2/`, the same way `v2/` was added next to v1,
      so no installer that is already out breaks. Update the piece list
      the installer embeds.
- [ ] Add third-party antivirus to `docs/TRY-IT.md`
      ([02-antivirus-block.md](02-antivirus-block.md), "Now, for testers").
- [ ] Update `HANDOFF.md` / `docs/handoff/OPEN.md` so "the first real PC"
      records this attempt: it got as far as the download and stopped at
      Windows' program loader.
