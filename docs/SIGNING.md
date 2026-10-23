# Signing: AurOS-Setup.exe ships unsigned, by the owner's decision

> **2026-10-23, the owner: no code signing, no certificates, no Azure,
> no Microsoft services, ever.** AurOS-Setup.exe is published unsigned
> on purpose (`docs/handoff/SESSION-2026-10-23.md`, D7). Nothing in the
> build or the release workflow asks for a key, and `AUROS_RELEASE=1`
> builds an unsigned installer without complaint. What this costs the
> person is a few extra clicks, written out below so that the website,
> the release notes and anyone helping her say the same thing.

---

## What she sees, and what to click

Unsigned, a downloaded `.exe` meets up to four questions. None of them
is an error.

| Where | What appears | What to click |
|---|---|---|
| **Edge, while downloading** | "AurOS-Setup.exe isn't commonly downloaded. Make sure you trust AurOS-Setup.exe before you open it." | **…** then **Keep**; in the box that follows, **Show more** then **Keep anyway** |
| **Chrome, while downloading** | "Suspicious download blocked" / "This file isn't commonly downloaded" | **Download suspicious file** |
| **Opening it** (any browser) | The blue "Windows protected your PC" panel. Only *Don't run* is visible | **More info**, then **Run anyway** |
| **Permission** | "Do you want to allow this app from an unknown publisher to make changes to your device?" | **Yes** |

**Smart App Control.** Some Windows 11 PCs (clean installs since 22H2)
have Smart App Control on. It blocks an unsigned program outright, with
no *Run anyway*: "Smart App Control blocked an app that may be unsafe".
On those PCs it has to be switched off first: **Start → Settings →
Privacy & security → Windows Security → App & browser control → Smart
App Control settings → Off**. Since the April 2026 Windows update it
can be switched back on later from the same page (on some PCs the
October 2026 update broke switching it back on;
`docs/research/every-machine-2026-10.md` §5). Windows 10 has no Smart
App Control.

The blue panel and Edge's warning fade as more people download the same
file (SmartScreen reputation is per file hash for an unsigned program),
so a new `AurOS-Setup.exe` starts again from nothing. Publishing fewer,
longer-lived installers helps; see "What would help" below.

---

## What the build does

- `build/aurbridge` signs only if signing material is configured (the
  optional paths `build/sign` has always had: a PKCS#11 token, a
  PKCS#12 file, or a PEM key pair). Nothing configures any, so the
  installer is unsigned, and the build says *"unsigned, by design"*.
  `AUROS_RELEASE=1` no longer refuses that; it still refuses an
  installer without its staging environment, its signed shim or its
  image address.
- `tools/signtest.sh` keeps the optional path honest: with a throwaway
  CA it proves `build/sign` signs with SHA-256, verifies what it
  signed, replaces rather than stacks a second signature, and is broken
  by one changed byte; and that `build/aurbridge` signs what it builds
  when a key is configured and, without one, finishes, says it is
  unsigned, and is.
- The release workflow (`.github/workflows/release.yml`) publishes the
  unsigned file and says so in one neutral line of the release notes,
  with the clicks above. Its Windows job prints what
  `Get-AuthenticodeSignature` says (`NotSigned`) and checks the file
  starts.

## What would help without a certificate

- **One installer for longer.** Every rebuild of `AurOS-Setup.exe` is a
  new hash with no reputation. Today each build bakes the address and
  hash of its own image in, so a new image means a new installer. An
  installer that could take a newer image without being rebuilt would
  keep its reputation; that is a design change for the installer's
  owners, not a build setting.
- **The website shows the clicks**, with pictures, before she meets
  them.

---

## The other signature: Secure Boot

This is a separate question with a much happier answer, and it is worth
saying why it is already solved.

AurOS boots through **Canonical's `shimx64.efi.dualsigned`** and their
signed `grubx64.efi`, copied out of the image by `build/mkimage` — not
rebuilt. Those binaries carry Microsoft's signature already, so AurOS
boots on a machine with Secure Boot enabled without anybody submitting
anything to anybody. `tools/loadertest.sh` proves it against
`OVMF_CODE_4M.ms.fd` with Microsoft's own keys enrolled and Secure Boot
enforcing.

**What "dualsigned" means, because this file used to get it wrong.**
Ubuntu's `shimx64.efi.dualsigned` carries two signatures: Canonical's
own and **Microsoft's, through the Microsoft Corporation UEFI CA 2011**
(`sbverify --list` shows exactly those two). It is *not* signed with
Microsoft's 2023 third-party key. The 2011 CA is the one old firmware
trusts, which is the whole reason the chain is borrowed rather than
built: a distribution applying to Microsoft on its own today gets a
2023-signed shim, which would not boot on exactly the old PCs this
product exists for.

The other side of the same fact: **a PC that lists Microsoft's 2023
third-party key and not the 2011 one refuses this shim**, and so does a
Secured-core PC that ships with third-party keys switched off. Neither
is left to find out at the restart. The installer reads the firmware's
`db` (and `dbx`) from Windows before it changes anything and asks
whether it trusts the key the carried shim is signed with -- a name
`build/aurbridge` reads off the shim itself, so a newer shim is checked
for its own key. See `docs/AURBRIDGE.md`, "Secure Boot stays on".

If AurOS ever needs its own shim — to carry its own vendor certificate,
so that kernel modules could be signed by us — that is a submission to
the Microsoft UEFI signing service through a shim review, it takes
months, and it does not remove the 2011 problem. There is no reason to
start it, and this paragraph exists so that nobody starts it by
accident.

---
