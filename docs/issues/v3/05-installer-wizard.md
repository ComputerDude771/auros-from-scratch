# 5. The installer (AurBridge wizard and staging): missing pieces

**Source:** `docs/RELEASE.md` §7 and `HANDOFF.md` §6 on
`claude/laughing-cray-ayao6i`, checked against the code on 2026-09-26.
**Severity:** each one is marked below.

## Missing features a person runs into

### 5.1 "Put Windows back" can't be started by a person
**Severity: high.** The restore exists and is tested (`aurstage.restore`,
from the memory stick or from the copy on the disk). But no menu entry
and no button in AurOS starts it. The wizard no longer promises one.
The website still does: `website/safety.html` and
`website/download.html` describe a "one-button Put Windows back". See
[10-docs-and-website.md](10-docs-and-website.md).
**Needed:** a start-up (GRUB) menu entry and a settings button in AurOS.

### 5.2 No page to choose a memory stick (R11)
**Severity: high.** The wizard can only install in no-stick mode, which
keeps the only copy of the way back on the disk it's changing. Stick
mode works in the engine and in every end-to-end test, but a person
can't get to it. R11's typed confirmation of the target drive belongs on
the same page and doesn't exist either.

### 5.3 Screen readers see nothing
**Severity: blocker for a public release.** The wizard is one
owner-drawn window with no accessibility tree. It needs a UI Automation
(or MSAA) provider.

### 5.4 No profile picker
**Severity: medium.** The wizard always writes the constant `desktop`
(`src/aurbridge/wizard.c` ~line 1217). The other four profiles
(`office`, `revive`, `school-kiosk`, `multilingual`) can't be installed
from the wizard. The journal already carries the profile, ready for a
picker.

### 5.5 The staging environment has no screen of its own
**Severity: medium.** After the restart the person sees console text
("Making room on the Windows drive", `verdict=...`). It can be read, but
it isn't the progress display the design calls for. `docs/TRY-IT.md` has
to warn testers that "the screen shows text, not a desktop".

### 5.6 AurOS's own words are English only
**Severity: medium.** The language picked in the installer sets the
locale and applies to apps that ship their own translations. The
shell's own text, and apps whose translations are in Ubuntu language
packs, stay English until those packs are installed.

## Machines that are refused

### 5.7 PCs that trust only Microsoft's 2023 third-party key
**Severity: medium, and growing over time.** Ubuntu's shim is signed
with the **2011** key only. Preflight correctly refuses these PCs
("installer too old for this PC"), and there's no setting a person can
change. It's fixed when a 2023-signed shim exists. `build/aurbridge`
reads the key off whatever shim it embeds, so nothing else needs to
change.

### 5.8 Secured-core PCs with the third-party CA off
**Severity: low. It's handled, but it's friction.** Preflight stops and
draws the one firmware setting to switch on. That breaks the "no firmware
screens" rule for those machines, and there's no way around it with
Secure Boot on.

## Risks noticed and not fixed

### 5.9 The staging `grub.cfg` finds its files by searching for them
**Severity: low.** It uses `search --file /EFI/AurOS/staging.efi`. On a
PC with a second EFI partition that holds an old `\EFI\AurOS\staging.efi`,
it could pick the wrong one. It would then refuse safely at the journal
check. It would be more exact to use the device GRUB was loaded from.

### 5.10 Closing the window cancels everything
**Severity: low. It's by design, but a trap.** After the last step, closing
the window instead of pressing *Restart now* takes the restart setting
back. `docs/TRY-IT.md` warns about this. The wizard should make it
impossible to miss, for example by asking before it closes.
