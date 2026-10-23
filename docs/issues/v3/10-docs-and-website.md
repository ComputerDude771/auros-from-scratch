# 10. Documentation and website: out of date or contradicting each other

## 10.1 The website describes a project from before the installer existed
**Severity: medium. It's wrong in both directions.**
- `website/download.html` (~line 217): "AurBridge's destructive phases
  are specified and not yet written." They are written: shrink, write,
  verify, commit, boot handoff, restore.
- `website/safety.html` (~line 597), under "Not built yet": lists every
  destructive installer phase, the rescue environment and the "Put
  Windows back" path, **the migration engine**, and the Secure Boot
  chain. All of these exist now.
- The website also promises a **"one-button 'Put Windows back'"**, which
  still doesn't exist ([05](05-installer-wizard.md) §5.1).
- The download page names `aurbridge-check-0.1.0.exe` as the future file.
  The real test file is `AurOS-Installer-test.exe`.

## 10.2 Two handoffs on two branches that disagree
**Severity: medium. It will confuse the next agent.**
- `HANDOFF.md` differs between `claude/linux-distro-from-scratch-tt5dx1`
  and `claude/laughing-cray-ayao6i`. Each says its own branch is the one
  to read.
- On the merge of `2d85777`: one says "clean", the other says "will
  conflict". A dry run on 2026-09-26 says clean
  ([06](06-first-boot-aurfirst.md)).
- The development branch's `HANDOFF.md` §2 still lists
  `claude/linux-distro-from-scratch-tt5dx1` at `ef2300a`. It's at
  `787e598` (the handoff commit).
- **Fix:** after the merge, keep one `HANDOFF.md` and delete or redirect
  the other.

## 10.3 "Tested" doesn't say where
**Severity: high. It's how [01](01-sxs-manifest.md) got to a tester.**
The `image-desktop` README and `HANDOFF.md` §5 list test counts that
read as though they cover Windows. They should say which ran under
Wine, which ran in QEMU, and which on real Windows (none, today).
See [03](03-testing-gap.md).

## 10.4 `docs/TRY-IT.md` is missing things a tester hit or will hit
- Third-party antivirus ([02](02-antivirus-block.md)).
- What to do when the installer doesn't open at all.
- That the file that was published doesn't start
  ([01](01-sxs-manifest.md)). Until it's replaced, the doc sends
  people to a broken file.

## 10.5 The promise and the reality of "one file"
**Severity: high. It's the product's headline.** The README's contract
is: "downloads one file, double-clicks it, reads and agrees…". The tester
asked for exactly that: one download and no other changes to their PC.
What it actually takes today:

1. Get past antivirus ([02](02-antivirus-block.md)).
2. Get past SmartScreen: *More info* → *Run anyway*, and it's impossible
   under Smart App Control.
3. Turn off Fast Startup in Control Panel.
4. Turn off BitLocker or Device encryption and wait for it to finish.
5. Unplug every USB drive.
6. Have about 50 GB free on C:.
7. Install waiting Windows updates and restart.
8. Make OneDrive files local, or lose access to them in AurOS
   ([08](08-ferry-migration.md) §8.9). Nothing asks for this.

Items 3–7 are refusals the wizard explains, which is acceptable. Items
1–2 happen before the wizard exists. For each item, decide whether the
wizard can do it for the person (with consent), walk them through it,
or whether it's a real limit to state on the download page.

## 10.6 A theme template placeholder
**Severity: trivial.** `src/aurora/aurora` line 330 scaffolds new themes
with `theme_description="TODO: one line describing the mood."`. That's
intended as a prompt to the theme author. Check that nothing ships
with it unfilled.
