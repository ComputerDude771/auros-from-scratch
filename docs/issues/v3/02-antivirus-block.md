# 2. Avast (and probably others) block the installer

**Severity:** blocker for the intended audience. Testers can work around it.
**Status:** not fixed. Partly covered by the code-signing plan in
`docs/RELEASE.md` §2 and `docs/SIGNING.md`, but third-party antivirus
isn't mentioned anywhere in the repository.

## What happens

When `AurOS-Installer-test.exe` is downloaded or opened, Avast shows:

1. **"Suspicious file detected".** "We're scanning
   AurOS-Installer-test.exe to make sure it's safe to open. We've also
   sent it to our Threat Labs for a closer look." This is Avast's
   CyberCapture: unknown files get uploaded and held.
2. **"Hmm… This needs a closer look".** "It should take only a few
   hours. In the meantime, we'll keep blocking it." The buttons are
   *Close* and *More options*.

`docs/SIGNING.md` and `docs/RELEASE.md` only plan for **Windows
SmartScreen**. Avast, AVG (same engine), Norton, McAfee, Bitdefender,
Kaspersky and ESET are all common on exactly the old home PCs AurOS is
for. None of them appear anywhere in the repository.

## Why this installer looks suspicious (likely reasons, not confirmed)

Avast doesn't publish why it flagged a file. These are the traits that
antivirus heuristics usually weigh, and this file has all of them:

- **It isn't signed** (no Authenticode signature; its security directory is empty).
- **It has no reputation.** It's a brand-new file hash that no one else has run.
- **About 32 MB of opaque data sits in its resources.** Five RCDATA blobs
  (15.0 MB, 13.6 MB, 2.7 MB, 0.97 MB and 0.86 MB: the staging kernel,
  initramfs, shim, GRUB and so on) inside a ~200 KB program. That's the
  shape of a "dropper".
- **It asks for administrator rights** as soon as it starts (`requireAdministrator`).
- **What it does looks like a bootkit's behaviour:** reading and writing
  the raw disk, writing UEFI firmware variables (`BootNext`), writing files
  to the EFI system partition, and downloading from the internet (WinHTTP)
  from a `raw.githubusercontent.com` URL.
- **It's hosted on GitHub raw**, which malware uses a lot.

## What needs doing

### Now, for testers (documentation only)

- **Add an antivirus section to `docs/TRY-IT.md`:**
  - Say that third-party antivirus may hold the file.
  - Say what to do: wait for the Threat Labs verdict, or use *More options*
    to allow the file / add an exception. **The exact wording in Avast
    hasn't been checked.** Get a screenshot from the tester and write the
    real steps down.
  - Say whether to **turn off the antivirus for the length of the install**.
    Real-time protection may also block the raw-disk and EFI writes
    halfway through (see below). A test build that is stopped halfway
    through arming the restart is a state the failure tests should already
    cover, and that should be confirmed.

### Before any public release

1. **Code signing** (already `docs/RELEASE.md` §2). It helps with
   SmartScreen and with most antivirus reputation systems, but it doesn't
   switch off heuristics.
2. **Submit the file as a false positive to each major vendor for every
   release:** Avast/AVG, Microsoft (Defender's file submission portal),
   Norton, McAfee, Bitdefender, Kaspersky, ESET. Add this to the release
   checklist in `docs/RELEASE.md`.
3. **Check every release on VirusTotal before publishing** and record the
   result in `docs/results/`. A detection count above zero is a release
   blocker, or at least a known issue that is written down.
4. **Consider shrinking the "dropper" shape:** download the staging
   environment (kernel/initrd) with its hash, the same way the image pieces
   are already downloaded, instead of embedding 30 MB in the `.exe`. This
   is a design trade-off. The embedded payload is what lets the installer
   work offline up to the image download. Weigh it; don't just do it.
5. **Test with antivirus running during the install, not only at
   download.** Behaviour blockers (Avast Behavior Shield, Bitdefender
   Advanced Threat Defense, Kaspersky System Watcher, Defender's
   Controlled Folder Access and ASR rules) may kill the process while it
   writes to the disk or firmware. The installer has to fail safely and
   say so in plain words when that happens. Nobody has tried it yet.
6. **Hosting:** a download from the project's own domain, with a stable
   URL and a stable signer, builds reputation. A branch on GitHub raw
   doesn't (already `docs/RELEASE.md` §3, "image host").

## The bigger point

The product promise is "download one file, double-click it". Right now a
real person has to:

1. get past antivirus
2. get past SmartScreen ("More info" → "Run anyway")
3. turn off Fast Startup
4. turn off BitLocker/Device encryption if it's on
5. unplug USB drives

All of that happens before the wizard does anything. Items 3–5 are
refusals the wizard explains, which is fine. Items 1–2 happen before the
wizard exists on screen, so the wizard can't explain them. For this
audience that's where most people will give up.
