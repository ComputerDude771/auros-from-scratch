# Trust & Signing Prerequisites

Research date 2026-09-20. **[V]** = verified against a primary source.

## The four findings that drive the plan

1. **The Secure Boot signing window for new entrants closed on 27 June
   2026.** **[V]** Microsoft can no longer sign shim with the old
   *Microsoft Corporation UEFI CA 2011* key — only *Microsoft UEFI CA
   2023*. Distros that obtained dual-signed shims did so ~Oct 2025 –
   Jun 2026. A new distro applying today gets a **2023-only** shim, which
   will not boot on firmware whose `db` lacks the 2023 CA — i.e. exactly
   the old PCs this product targets. **We missed the dual-signing window.**

2. **A from-scratch distro with a custom bootloader is close to
   un-signable.** **[V]** The shim review board states it only has
   experience with **GRUB2 or systemd-boot**; anything else needs
   convincing justification. Its template asks *"Why are you unable to
   reuse shim from another distro that is already signed?"* — reuse is
   the expected default for a project this size. **syslinux is not a
   viable path.**

3. **EV certificates no longer buy SmartScreen reputation.** **[V]**
   Microsoft's own docs: *"EV certificates no longer bypass SmartScreen"*
   and *"Paying a premium for EV solely to avoid SmartScreen warnings is
   no longer justified."* Budgeting ~$600/yr of EV for a clean first run
   buys something that no longer exists.

4. **No kernel-mode Windows driver is needed.** **[V from Rufus source]**
   Rufus performs partition-table rewrites, volume lock/dismount and raw
   sector writes entirely from an elevated **user-mode** process. This
   removes EV + Partner Center + attestation signing from AurBridge's
   critical path.

## Secure Boot — decision for v1

**Option A (adopted): reuse an already-signed shim + MOK enrollment.**
Ship a third party's Microsoft-signed `shimx64.efi` (Fedora's or
Debian's), our own `grubx64.efi` signed with our key, our own signed
kernel, and enroll our key as a MOK.

- Reuse is the review board's assumed default. **[V]**
- shim is permissively licensed and Microsoft's signature travels with
  the binary. *(Verify the exact SPDX id in `rhboot/shim/COPYRIGHT`
  before shipping.)*
- **Decisive advantage:** picking a shim **dual-signed** during the Oct
  2025 – Jun 2026 window (Red Hat / AlmaLinux / Debian builds from that
  period) gets 2011 **and** 2023 coverage — something no longer
  obtainable by applying ourselves. Verify with `sbverify --list` before
  shipping.
- Costs: one-time MOK enrollment UX; we inherit the upstream vendor's
  SBAT generation and revocation fate; our loader must be named
  `grubx64.efi` to match shim's `DEFAULT_LOADER`.

Rejected: **B** own shim via shim-review (3-6 months, EV cert, legal
entity, GRUB2 rewrite, ongoing CVE obligation — and the result is
2023-CA-only, failing on our target hardware; a v2/v3 goal).
**C** ship with Secure Boot disabled (a serious product defect, and the
BitLocker interaction makes it actively dangerous).
**D** `sbctl` with own keys in firmware Setup Mode (worse UX than MOK).

**Immediate consequence: GRUB2 is a prerequisite.** The syslinux
bootloader template has been replaced.

## Windows code signing — signing is not optional

Three independent gates; conflating them is the common planning error:

| | UAC prompt | SmartScreen (MOTW) | Smart App Control |
|---|---|---|---|
| **Unsigned** | "Unknown publisher" | "Windows protected your PC", primary button **Don't run**; managed environments **cannot** bypass | **Hard blocked**, no "Run anyway" |
| **OV, no reputation** | Verified org name | Same warning until hash/publisher reputation accrues | Blocked unless recognized |
| **EV, no reputation** | Verified org name | **Identical to OV since 2024** | Same as OV |
| **Established cert** | Verified org name | No warning | Passes |

**Smart App Control is the gate that actually stops us.** **[V]** It
blocks unsigned code by default, starts in evaluation mode (~a month),
then auto-enables for typical consumers and auto-disables for
developer/enterprise-looking machines. Our target user — non-technical,
clean-installed Windows 11, Microsoft account — is **exactly** the
profile where it auto-enables.

> An unsigned AurBridge is not merely scary for that user. It is
> **unrunnable**.

Implications: buy **OV, not EV** (EV's SmartScreen advantage is gone);
keys must live on a hardware token/cloud HSM (June 2023 requirement);
budget for reputation build-up time; submit binaries to Microsoft to
pre-empt Defender false positives — an installer that repartitions disks
is inherently heuristic-suspicious.

## Legal

A real legal entity, OV identity validation, a lawyer-drafted consent
flow, and an explicit EU decision are prerequisites to the first public
download. The EU Product Liability Directive (2024/2853) applies from
**9 December 2026** and software liability under it cannot be disclaimed
away by EULA.
