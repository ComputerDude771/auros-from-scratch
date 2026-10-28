# Every machine, October 2026: what the PCs out there look like

Research date **2026-10-23**. Point-in-time; re-check before betting user
data on any single line.

How each claim was checked:

- **[V]** inspected the primary artifact myself today: the Ubuntu archive's
  `Packages.gz`, a `.deb` and its `sbverify --list`, Chromium or other
  source code, an action's `action.yml`.
- **[P]** a primary source (Microsoft, Canonical, GitHub, Flathub). Every
  `*.microsoft.com` host refused WebFetch from here, so Microsoft pages
  were read **through search-engine summaries**, not in full.
- **[2]** a secondary source (press, forum, vendor blog).

Already settled elsewhere and not repeated here: EV no longer skips
SmartScreen (`docs/SIGNING.md`), Rufus-style user-mode disk writes
(`signing-trust.md`), the 2 GiB release-asset fit (`docs/RELEASE.md` §3).

---

## 1. Secure Boot: the 2011 to 2023 certificate change

| Fact | Status 2026-10-23 | Source |
|---|---|---|
| Expiry dates | KEK CA 2011: 2026-06-24. **UEFI CA 2011** (third-party, the one that signs shim): **2026-06-27**. Windows Production PCA 2011: 2026-10-19. | [P] [MS support, CA updates](https://support.microsoft.com/en-us/servicing/os/secure-boot/2025/06/windows-secure-boot-certificate-expiration-and-ca-updates) |
| Microsoft signs new shims with | **2023 only, since 2026-06-27**: "It is no longer possible to get your shim signed by the 'old' Microsoft Corporation UEFI CA 2011 key." | [P] [rhboot/shim-review README](https://github.com/rhboot/shim-review) |
| Noble `shim-signed` | **1.58+15.8-0ubuntu1**, release pocket only (published 2024-04-08). **Nothing in noble-updates, -security or -proposed.** | [V] `archive.ubuntu.com/.../noble*/main/binary-amd64/Packages.gz`; [Launchpad publishing history](https://launchpad.net/ubuntu/+source/shim-signed/+publishinghistory) |
| Its signatures | `shimx64.efi.dualsigned` = Canonical (2022 v1) + **Microsoft Corporation UEFI CA 2011**. `.signed.latest` and `.signed.previous` = MS UEFI CA 2011 only. **No 2023 signature.** | [V] `sbverify --list` on the 1.58 `.deb` |
| Newest in the archive, any series | 1.59+15.8-0ubuntu2 (built 2024-10-02; in resolute, stonking, tenacious). Still shim 15.8 and 2011. No 1.60 in the pool. | [V] `pool/main/s/shim-signed/` |
| A 2023-signed Ubuntu shim | **Not released.** `shim 16.1-0ubuntu1` and `shim-signed 1.60~ppa1` (stonking) are in the Ubuntu UEFI team's build PPA, uploaded 2026-06-29 and 2026-07-06. I could not read the changes file, so whether it carries a Microsoft signature is **unconfirmed**. Any Microsoft signature obtained after 27 June can only be 2023. **Dual-signed (2011+2023) is therefore unlikely for Ubuntu**, unlike Fedora's shim 16.1, which is dual-signed. | [V] [PPA](https://launchpad.net/~ubuntu-uefi-team/+archive/ubuntu/build); [2] [rhboot/shim#825](https://github.com/rhboot/shim/issues/825) |
| Canonical's stated plan | "Shim bootloader updates delivered by Canonical after the June 2026 expiry will be signed with the new 2023 Microsoft CA." 2023 db/KEK updates go out through fwupd/LVFS. Rollout to 24.04 was paused 06-11 and completed in -updates 06-23. | [P] [Ubuntu Discourse, juliank, 2026-05-21, updated 06-23](https://discourse.ubuntu.com/t/microsoft-uefi-ca-rotation-what-it-means-for-ubuntu-users-and-vendors/82652) |
| 2011 UEFI CA in dbx? | **No, and no date announced.** Microsoft's Linux guidance: "no current plan to revoke the Microsoft UEFI CA 2011", and don't remove it from db. Canonical: "We do not anticipate the 2011 code signing CA being revoked in its entirety, but selective security-related revocations of old code are likely." | [P] [MS Linux distro guidance](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/linux-distribution-guidance-handling-2023keys?view=windows-11) (via search); Discourse above |
| Does Windows Update remove 2011 from db? | No. It adds Windows UEFI CA 2023. It adds **Microsoft UEFI CA 2023 and Option ROM CA 2023 only if MS UEFI CA 2011 is already in db.** So a PC that boots our shim today keeps booting it after the update. | [P] [MS IT-pro guidance](https://support.microsoft.com/en-us/topic/secure-boot-certificate-updates-guidance-for-it-professionals-and-organizations-e2b43f9f-b424-42df-bc6a-8476db65ab2f) (via search) |
| New PCs with only 2023 certificates | **Yes, some.** Debian: "typically concerns new (2024+) hardware that instead contains the 'Windows UEFI CA 2023' certificate" with no MS UEFI CA 2011 (Debian bug 1112197). fwupd says the same. Dell: ships **both** on new platforms since late 2024 and on all by end 2025, with no end date. One field report: an MSI Crosshair 16 Max HX that had completed the transition refuses Ubuntu 26.04's 1.59 shim ("Invalid signature detected"); open, unanswered (2026-08-06). | [P] [Debian wiki](https://wiki.debian.org/SecureBoot); [P] [Dell FAQ](https://www.dell.com/support/kbdoc/en-us/000390990/secure-boot-transition-faq); [2] [shim#825](https://github.com/rhboot/shim/issues/825) |
| Share of PCs with the 2023 db | **No public figure (could not be confirmed).** Microsoft self-reports 97% of its own ~500k-device fleet [2]. Consumer rollout is phased by "high confidence" buckets and was still expanding in Sept 2026 [2]. **Windows 10 without ESU does not get it**, and consumer ESU ended **2026-10-13**. Legacy-BIOS and Secure-Boot-off PCs are skipped. | [2] [PCWorld](https://www.pcworld.com/article/3201751/still-dont-have-updated-windows-secure-boot-certificates-dont-panic.html); [2] [Help Net Security on ESU dates](https://www.helpnetsecurity.com/2025/06/25/windows-10-how-to-get-security-updates-for-free-until-2026/) |
| Ubuntu OVMF in our tests | `edk2 2024.02-2ubuntu0.9` (2026-06-02): "Expand EnrollDefaultKeys with Microsoft 2023 keys" (LP #2146560). noble-updates is now 0.10. So `OVMF_VARS_4M.ms.fd` probably trusts both generations (I did not dump the VARS). To simulate a 2023-only PC, build VARS with `virt-fw-vars` (`python3-virt-firmware` 24.1.1, noble universe). | [V] [edk2 changelog](https://changelogs.ubuntu.com/changelogs/pool/main/e/edk2/edk2_2024.02-2ubuntu0.10/changelog) |

**So what for AurOS:** the 2011-signed noble shim is still right for the
main market. Windows 10 PCs never get the 2023 db, and Windows 11 PCs keep
2011 alongside it. The gap stays as it is: 2024+ PCs with only the 2023
certificates, and Secured-core PCs, are refused. Plan for **two shims**:
when `shim-signed` ≥ 1.60 reaches noble-updates (probably 2023-only),
carry both and pick one per machine from the `db` that preflight already
reads. Add a test VARS file with no 2011 certificate, because the stock
`.ms.fd` no longer represents an old PC on its own.

---

## 2. BitLocker / Device Encryption

| Windows | Encrypted by default? | Conditions | Source |
|---|---|---|---|
| Windows 10, Home and Pro: **Device encryption** | Yes, on qualifying hardware | TPM with PCR7 binding (TPM 2.0 in practice), UEFI + Secure Boot, **Modern Standby or HSTI-compliant**, no external DMA ports. **Activated only when an admin signs in with a Microsoft or Entra account** (key escrowed). With a local account it stays inactive. | [P] [MS BitLocker overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/data-protection/bitlocker/) (via search); [P] [OEM BitLocker](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/oem-bitlocker) |
| Windows 10/11 Pro: full **BitLocker** | No, manual | Turned on by the owner or by an employer's policy | same |
| Windows 11 ≤ 23H2 | as Windows 10 | Modern Standby/HSTI still required | same |
| **Windows 11 24H2 and later** (25H2, 26H2), clean install or OOBE | **Yes, Home and Pro** | "the prerequisites of DMA and HSTI/Modern Standby are removed"; TPM + Secure Boot are still needed. **An in-place upgrade to 24H2 does not newly enable it.** With a local account the drive is **encrypted with a clear key** ("waiting for activation"): BitLocker-formatted, protection off. | [P] MS overview (via search); [2] [Windows Latest 2024-08-11](https://www.windowslatest.com/2024/08/11/windows-11-24h2-reduces-bitlocker-eligibility-turns-on-automatic-encryption-for-more-pcs/); [2] [Thurrott field guide](https://www.thurrott.com/books/windows-11-field-guide/archived-22h2/312958/device-encryption-24h2) |

- **Share of consumer PCs encrypted: no data found (could not be
  confirmed).** Microsoft publishes nothing on this. The only percentages
  are enterprise or 2015-era (Statista/OPSWAT).
- **Windows 10 PCs that cannot run Windows 11: usually unencrypted.**
  This is inference from the requirements above, not measured. Most
  2010–2017 desktops and laptops have no Modern Standby and often no TPM
  2.0, so Device encryption never qualified. The exceptions are Modern
  Standby tablets, 2-in-1s and ultrabooks (Surface Pro 3/4, Atom and
  Core m devices) signed in with a Microsoft account, and Pro machines
  where someone turned BitLocker on.

**So what for AurOS:** for the Windows 10 market, expect mostly
unencrypted disks. For any Windows 11 24H2+ clean install, expect
encryption, **including local-account PCs where "protection" reads Off**.
Detection must keep looking at the volume (`-FVE-FS-`), never at the
protection status. The installer's "turn it off and wait" path will be the
normal path on new PCs, so its wait (hours on a hard disk) needs a
progress line.

---

## 3. Legacy BIOS / MBR, and 32-bit UEFI

- **Share of Windows 10 PCs booting legacy BIOS/CSM from MBR: no
  authoritative data found.** The nearest numbers:
  - Lansweeper's 2021–22 audits (about 30 million enterprise devices)
    report CPU and TPM failures only. They infer UEFI from a GPT boot
    disk and give no BIOS share. [2] [Lansweeper report](https://www.lansweeper.com/report/windows-11-requirements-audit/)
  - FirmGuard says "up to 20% of endpoints had Secure Boot disabled";
    its methodology is unknown. [2] [FirmGuard](https://firmguard.com/3-key-findings-from-our-securecheck-audits/)
  - Microsoft's certificate rollout skips legacy-BIOS and Secure-Boot-off
    PCs, which is consistent with a non-trivial population, but gives no
    number. [2]
- **Where the MBR machines are** (qualitative): Windows 7 installs done
  in legacy mode, even on UEFI-capable 2012–2015 hardware, and then
  upgraded in place to 10; and pre-2012 machines. Windows 8-logo PCs
  (2012 and later) shipped UEFI with Secure Boot on.
- **32-bit UEFI**: Bay Trail and Cherry Trail tablets and cheap 2-in-1s
  (2013–2016). They usually run 32-bit Windows 8.1/10 with 1–2 GB of RAM
  and 32 GB of eMMC. [2] [LP #1341944](https://launchpad.net/bugs/1341944)
  Noble ships `grub-efi-ia32` **unsigned only**, with no signed ia32 shim
  or grub [V archive], so these machines need Secure Boot off. A 32-bit
  Windows also cannot start our x64 `aurbridge.exe` at all
  (`build/aurbridge` uses `x86_64-w64-mingw32-gcc`).

**So what for AurOS:** keep refusing legacy-BIOS, MBR and 32-bit UEFI
machines. Each fix (MBR2GPT plus a firmware switch, or Secure Boot off)
needs a firmware screen. The download page should catch 32-bit Windows
**before** the download (§4: `bitness` reports `"32"`), so nobody gets
the "This app can't run on your PC" error. The dry-run matrix is the only
way to learn the real BIOS share for this audience.

---

## 4. Windows on ARM, and detecting it

- **Share:** TrendForce, as reported by ITdaily, puts Windows-on-Arm at
  **≈1.2% of laptop shipments in 2025 and ≈3.2% in 2026**. That is
  shipments; the installed base is lower. [2] [ITdaily](https://itdaily.com/blogs/workplace/qualcomm-snapdragon-x-pc-windows-on-arm/)
  There are effectively **no ARM PCs among Windows 10 machines that
  cannot upgrade**: Windows 10 ARM devices could take 11, and Windows 10
  on ARM never shipped x64 emulation (it is Windows 11 only). [P] [Windows Insider blog](https://blogs.windows.com/windows-insider/2020/12/10/introducing-x64-emulation-in-preview-for-windows-10-on-arm-pcs/)
- **Website detection, Chrome and Edge** (both use Chromium's
  `components/embedder_support/user_agent_utils.cc`) [V source,
  main@2026-10-23]:
  - `navigator.userAgentData.getHighEntropyValues(["architecture","bitness","platformVersion"])`.
  - `architecture` is `"arm"` when the **OS** is ARM64, **including an x64
    or x86 Chrome running under emulation**. The code checks
    `IsWowX86OnARM64() || IsWowAMD64OnARM64()`, because "the OS lies".
  - `bitness` is `"32"` only on 32-bit Windows, otherwise `"64"`.
  - `platformVersion` ≥ `13.0.0` means Windows 11 ([P] Microsoft Edge
    "detect Windows 11" docs, via search).
  - The legacy UA string is frozen at `Windows NT 10.0; Win64; x64` on
    every Windows, ARM included, so it cannot be used.
  - **Firefox has no `userAgentData`** [P] [MDN](https://developer.mozilla.org/en-US/docs/Web/API/NavigatorUAData/getHighEntropyValues).
    I could not confirm Firefox's UA string on ARM64. Treat Firefox as
    unknown and let the exe decide.
- **Inside an x64 exe running under emulation on ARM64:**
  - `IsWow64Process2` returns `processMachine = IMAGE_FILE_MACHINE_UNKNOWN`
    (x64 emulation is not WOW64) and **`nativeMachine = 0xAA64`
    (ARM64)**. `nativeMachine` is the reliable signal (Windows 10 1709
    and later). [P] [IsWow64Process2](https://learn.microsoft.com/en-us/windows/desktop/api/wow64apiset/nf-wow64apiset-iswow64process2); [P] [MS Q&A](https://learn.microsoft.com/en-us/answers/questions/232802/detecting-an-x64-app-running-on-arm64)
  - `GetNativeSystemInfo` from an x64 process reports AMD64, so do not
    use it.
  - `GetProcessInformation(ProcessMachineTypeInfo)` and
    `GetMachineTypeAttributes` need **build 22000 (Windows 11)**.
    [P] [PROCESS_MACHINE_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-process_machine_information)

**So what for AurOS:** the website should send `arch=arm` and
`bitness=32` visitors to a "not for this PC" page before the download.
AurBridge should refuse on `IsWow64Process2` `nativeMachine != AMD64` as
its first check, before anything else. Neither is in the repository today.

---

## 5. SmartScreen and Smart App Control

**What she sees for an unsigned `.exe`:**

| Step | What appears (button texts) | Source |
|---|---|---|
| Edge, download | Bubble: "*X* isn't commonly downloaded. Make sure you trust *X* before you open it." Buttons **Delete** and **…**; under **…** is **Keep**. Then a dialog "Make sure you trust *X* before you open it" (Publisher: Unknown), with **Delete** and **Show more**; **Show more** reveals **Keep anyway**. | [2] [gHacks](https://www.ghacks.net/2022/07/02/how-to-deal-with-microsoft-edges-isnt-commonly-downloaded-warning); [2] [Fuji Electric FAQ](https://monitouch.fujielectric.com/site/faq-e/qa/354) |
| Chrome, download (Safe Browsing "uncommon") | Status "**Suspicious download blocked**". Subpage "This file isn't commonly downloaded and it may be dangerous", buttons **Delete** and **Download suspicious file**. `chrome://downloads` says "*X* is not commonly downloaded and may be dangerous." | [V] Chromium `generated_resources.grd`, `download_ui_model.cc`, `download_bubble_info_utils.cc` (main@2026-10-23) |
| Launch, any browser (mark of the web) | Full-screen "**Windows protected your PC** / Microsoft Defender SmartScreen prevented an unrecognised app from starting…". Only **Don't run** is visible; the **More info** link shows "Publisher: Unknown publisher" and **Run anyway**. | `docs/SIGNING.md` (already established) |
| UAC | Unsigned: "Do you want to allow this app from an unknown publisher…", Publisher: Unknown. Signed: "Verified publisher: *name*". | standard Windows behaviour |

**Smart App Control (SAC):**

- **Where it is on:** Windows 11 only, on clean installs or resets
  (22H2 and later). It starts in **Evaluation** and then switches itself
  On or Off. **Windows 10 has no SAC.** Since the April 2026 update
  (KB5083769) it can be switched On, Evaluation or Off in Windows
  Security without reinstalling. The **2026-10-13 update KB5127648
  broke turning it back on** on some 24H2/25H2/26H2 PCs. No share of
  PCs with SAC on is published. [2] [Topedia 2026-04](https://blog-en.topedia.com/2026/04/smart-app-control-in-windows-11-can-now-be-re-enabled-without-reinstalling/); [2] [WindowsDigitals](https://www.windowsdigitals.com/smart-app-control-grayed-out-kb5127648/)
- **Does it block unsigned exes outright?** **Yes.** "If the security
  service can't make a confident prediction about the app, and the app
  doesn't have a valid signature, it's considered untrusted", and it is
  blocked. There is **no per-app bypass and no Run anyway**; the dialogs
  are "Smart App Control blocked an app that may be unsafe" and "Part of
  this app has been blocked". [P] [SAC FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions) (via search); [P] [blocked part of this app](https://support.microsoft.com/en-US/Windows/Security/Threat-Malware-Protection/smart-app-control-has-blocked-part-of-this-app)
- **Signed exes:** "If the app has a valid signature, Smart App Control
  will let it run" when the cloud has no verdict. [P] SAC FAQ

**With a new OV certificate or Artifact Signing:**

| | Changes **immediately** | Changes only **after reputation builds** |
|---|---|---|
| SAC | **Passes** (valid signature, no malicious verdict) | — |
| UAC | Shows the verified publisher name instead of "Unknown" | — |
| SmartScreen at launch | "More info" shows the publisher name | The blue panel stops appearing |
| Edge "isn't commonly downloaded" and Chrome "Suspicious download" | Probably nothing | These fade with download volume |
| Every new build | — | File-hash reputation starts again; the established publisher helps |

Sources: [P] [MS SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation) (via search). Microsoft's Artifact Signing page claims "base reputation on smart screen" [P] [product page](https://azure.microsoft.com/en-us/products/artifact-signing). Q&A reports from 2026 still show warnings on each release after Microsoft rotated intermediate CAs ("Microsoft ID Verified CS AOC CA 03", from March 2026) [2] [MS Q&A](https://learn.microsoft.com/en-us/answers/questions/5996190/artifact-signing-smartscreen-unrecognized-app-for).

**So what for AurOS:**

- Signing turns the SAC hard block into a pass **on day one**. That is
  the strongest argument for buying it now, before any reputation exists.
- The SmartScreen panel stays until volume builds. The release plan
  should stop rebuilding the exe for every image change (the image
  already has its own piece list), so that one hash accumulates
  reputation.
- Anything AurBridge itself launches must be Microsoft-signed (today it
  launches `schtasks.exe` and the like) or signed by us. It must never
  write out and run scripts.

---

## 6. Azure Artifact Signing (formerly Trusted Signing), individual in the US

**Eligibility and price**

- Individuals: **USA and Canada only**. Organisations: US, Canada, EU, UK.
  [P] [MS code-signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options) (via search)
- The rename is in name only. [P] product page
- Basic **USD 9.99/month** (5,000 signatures); Premium USD 99.99/month
  (100,000); USD 0.005 per signature over the quota. [2] summaries of the
  [pricing page](https://azure.microsoft.com/en-us/pricing/details/artifact-signing/)
- Needs a paid Azure subscription, with no free or trial credit (Q&A,
  low confidence).

**Identity validation (individual, Public Trust)**

1. Use a pay-as-you-go subscription whose **billing account type is
   Individual**. The **legal name and sold-to address must match the
   government ID**.
2. Register the `Microsoft.CodeSigning` provider and create an Artifact
   Signing account.
3. Grant yourself the Identity Verifier role.
4. In the **Azure portal only** (not the CLI), submit Identity
   validation, Individual. Verification is a Microsoft Entra **Verified
   ID**: an **AU10TIX** ID scan plus a **selfie** on a phone, and the
   card presented from **Microsoft Authenticator**.
5. Create a certificate profile.
6. Assign **Artifact Signing Certificate Profile Signer** to the
   identity that will sign.

[P] [Quickstart](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart) (via search); [P] the action's README.

**How long:** Microsoft says to plan for **a few business days**. 2026
Q&A threads report AU10TIX selfie hangs on iPhone Safari, Authenticator
QR loops, and requests stuck at "Action Required" for **days to weeks**;
a stuck session needs Microsoft support to reset it. [P/2] [Q&A 1](https://learn.microsoft.com/en-us/answers/questions/6009243/artifact-signing-individual-identity-validation-st), [Q&A 2](https://learn.microsoft.com/en-us/answers/questions/5955409/azure-artifact-signing-individual-identity-validat)

The certificate subject for an individual is the person's legal name,
which is what UAC will show.

**GitHub Actions, official action:** `azure/artifact-signing-action@v2`
(tags `v2`, `v2.0.0`; older `v1.2.0`, `v0.5.x`). [V] `action.yml` and tags of [Azure/artifact-signing-action](https://github.com/Azure/artifact-signing-action)

- It runs on **Windows runners only** (`windows-2025` or `windows-2022`;
  no ARM).
- Inputs:
  - `endpoint` (required, regional, e.g. `https://eus.codesigning.azure.net/`)
  - `signing-account-name` (the old `trusted-signing-account-name` still
    accepted)
  - `certificate-profile-name`
  - `files` or `files-folder` with `files-folder-filter`
  - `file-digest: SHA256`
  - `timestamp-rfc3161: http://timestamp.acs.microsoft.com`
  - `timestamp-digest: SHA256`
- Authentication, either way:
  - **OIDC (recommended):** `permissions: id-token: write`, then
    `azure/login@v3` with secrets `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`,
    `AZURE_SUBSCRIPTION_ID`, plus a federated credential on the app
    registration.
  - **Client secret:** inputs `azure-tenant-id`, `azure-client-id`,
    `azure-client-secret` (the `DefaultAzureCredential` chain;
    `exclude-*` inputs prune it).
- "The files must be signed with timestamping enabled in order for the
  signatures to be valid for longer than 3 days."
- **Linux alternative, which fits `build/sign`:** jsign 7.x supports it,
  for example
  `jsign --storetype TRUSTEDSIGNING --keystore eus.codesigning.azure.net --storepass "$(az account get-access-token --resource https://codesigning.azure.net --query accessToken -o tsv)" --alias <account>/<profile> aurbridge.exe`.
  Or omit `--storepass` and set `AZURE_TENANT_ID`, `AZURE_CLIENT_ID` and
  `AZURE_CLIENT_SECRET`. [V] [jsign docs](https://github.com/ebourg/jsign/blob/master/docs/index.html)

**Resources and appended data:** an Authenticode signature is a
certificate table at the end of the PE.

- Bytes appended **after** signing change the hash, so the signature
  stops verifying.
- Bytes appended **before** signing are covered, but are fragile.
- RCDATA resources are inside the image, which is what `build/aurbridge`
  already does (`tools/pe_payload.py`).
- Signing hashes **locally** and sends only the digest, so a large exe
  costs no upload.
- Never pad data into the certificate table: that is the
  MS13-098/`EnableCertPaddingCheck` case.

**So what for AurOS:**

- Eligible today as a US individual, at USD 9.99 a month.
- Budget **one to three weeks** of calendar time for identity
  validation, and do it from an Android phone or desktop Chrome rather
  than iPhone Safari.
- Add a jsign `TRUSTEDSIGNING` branch to `build/sign` rather than moving
  the build to a Windows runner. The existing verify-after-sign step
  still applies.

---

## 7. A Windows evaluation VM from CI (Linux runner, QEMU)

**Getting the ISO non-interactively.** dockur/windows (HEAD `013ea27`,
read 2026-10-23) uses three routes [V `src/mido.sh`, `src/define.sh`]:

1. **Evaluation Center scrape (Enterprise eval, LTSC, Server):**
   - GET `https://www.microsoft.com/en-us/evalcenter/download-windows-11-enterprise`,
     or `-windows-10-enterprise`.
   - Extract the `https://go.microsoft.com/fwlink/?linkid=…` links and
     pick the one for the right locale and architecture by its position
     and by the redirect's filename. **The fwlink IDs are not stable**;
     they are re-scraped every time.
   - `HEAD` it to resolve to `software-static.download.prss.microsoft.com/...iso`
     and check that the filename says x64.
2. **Consumer ISO (Home/Pro), the Fido/Rufus method:**
   - Make up a session UUID, register it at
     `vlscppe.microsoft.com/tags?org_id=y6jn8c31&session_id=…`, and
     complete an `ov-df.microsoft.com` token challenge.
   - Then `software-download-connector/api/getskuinformationbyproductedition`
     and `GetProductDownloadLinksBySku` (profile `606624d44113`) return a
     24-hour link.
   - Microsoft answers "715-123130 … Some users, entities and locations
     are banned" to IPs it rate-limits or blocks. [2] [Rufus #2130](https://github.com/pbatard/rufus/issues/2130)
   - **Cloud runner IPs are likely to be blocked; unconfirmed.**
3. **ESD catalog:** `go.microsoft.com/fwlink?linkid=2156292` (Windows 11)
   and `/?LinkId=841361` (Windows 10) are Media Creation Tool
   `products.cab` catalogs, which point to ESDs to convert to an ISO.
   These two IDs have been stable for years.

dockur falls back to mirrors (`dl.bobpony.com`, archive.org) when
Microsoft fails.

- **Sizes** (dockur README, 2026-10-23): Windows 11 Enterprise ≈ **6.6
  GB**, 11 LTSC ≈ 4.7 GB, Windows 10 Enterprise ≈ 5.2 GB, 10 LTSC ≈ 4.6
  GB. The Evaluation Center now serves **26H2 build 26300.9457** (since
  about 2026-09-18) [2] [Windows Report](https://windowsreport.com/windows-11-26h2-is-getting-closer-as-microsoft-releases-evaluation-isos/).
- **Licence:** 90 days, no product key. When it expires the background
  turns black and the PC **shuts down every hour** [P evalcenter via
  search]. Evaluation editions are for "testing, training and
  demonstration, not commercial use" [2]. **I could not read the EULA
  text itself.** Whether automated CI testing on GitHub's machines fits
  "evaluation" is **unconfirmed**. Read `sources\license\en-us\license.rtf`
  in the ISO before relying on it. A fresh install every run never
  reaches day 90.

**The unattended install, essentials:**

- **`autounattend.xml`**: the file name Windows Setup searches for at
  the root of every removable or optical drive. Ship it on a second
  small ISO or a FAT image attached as USB; no ISO rebuild needed.
- **Hardware checks**:
  - **Preferred:** `swtpm socket --tpm2` (noble `swtpm` 0.7.3) plus
    `OVMF_CODE_4M.ms.fd`/`OVMF_VARS_4M.ms.fd` with Secure Boot on, 4 GB
    or more of RAM, `-cpu host` on KVM. Then nothing needs bypassing, and
    the VM is a realistic Windows 11 PC: Device encryption, the Secure
    Boot db, BitLocker.
  - **Fallback:** in the windowsPE pass, `RunSynchronous` `reg add
    HKLM\SYSTEM\Setup\LabConfig` with `BypassTPMCheck`,
    `BypassSecureBootCheck` and `BypassRAMCheck` = 1 (dockur's
    `assets/win11x64.xml`).
- **"Press any key to boot from CD or DVD"**: rebuild the ISO with
  `efi/microsoft/boot/efisys_noprompt.bin` (and `cdboot_noprompt.efi`) as
  the El Torito EFI image, which is what dockur does with xorriso. Or skip
  CD boot by copying the ISO contents to a FAT/NTFS disk and booting
  `\efi\boot\bootx64.efi`. Or send `sendkey ret` through the QEMU monitor
  for the first ~10 s. [2] [Proxmox thread](https://forum.proxmox.com/threads/fully-unattended-windows-11-installation-avoid-press-any-key-to-boot-from-cd-or-dvd.162252/)
- **Local account and autologon:**
  - In `oobeSystem`: `UserAccounts/LocalAccounts/LocalAccount` (with a
    password), `AutoLogon` (`Enabled`, `Username`, `Password`,
    `LogonCount`), and `OOBE` (`HideEULAPage`, `HideOnlineAccountScreens`,
    `HideWirelessSetupInOOBE`, `ProtectYourPC=3`).
  - **`BypassNRO` and `start ms-cxh:localonly` are gone** from builds
    26120/26220.6772 onward (late 2025). [2] [Pureinfotech](https://pureinfotech.com/microsoft-blocks-windows-11-microsoft-account-bypass/)
  - Answer-file local accounts **still work**, and Enterprise never
    required a Microsoft account.
- **Edge first run:** `reg add "HKLM\SOFTWARE\Policies\Microsoft\Edge" /v HideFirstRunExperience /t REG_DWORD /d 1 /f`.
  It works on unmanaged PCs, and it does not cover sign-in, sync, import
  or the new tab page, which have their own policies (`BrowserSignin=0`,
  `SyncDisabled=1`, `AutoImportAtFirstRun=4`). [P] [Edge policy](https://learn.microsoft.com/en-us/DeployEdge/microsoft-edge-policies/hidefirstrunexperience) (via search)

**So what for AurOS:**

- A Linux-runner job can install Windows 11 Enterprise eval under OVMF
  with swtpm, run AurBridge, **really restart into staging**, and come
  back. `windows-latest` cannot do that last step.
- Use the Evaluation Center scrape, with a pinned SHA-256 and a cache.
  Treat a Microsoft refusal as "skip, not fail".
- Prefer swtpm and Secure Boot to the LabConfig bypasses, so the VM is
  encrypted the way a real 24H2+ PC is.

---

## 8. GitHub Actions hosted runners

| Item | Value | Source |
|---|---|---|
| `ubuntu-24.04`, public repository | **4 vCPU, 16 GB RAM, 14 GB SSD** (the guaranteed figure), x64, free and unlimited minutes | [V] [github/docs `supported-github-runners.md`](https://github.com/github/docs/blob/main/data/reusables/actions/supported-github-runners.md) |
| `ubuntu-latest` | **Moves to Ubuntu 26.04 in November 2026.** Pin `ubuntu-24.04`. | [V] [runner-images Ubuntu2404 README](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md) (image 20261012) |
| `/dev/kvm` | Present on standard Linux runners (since 2024-04-02; earlier only on 4+ vCPU). It needs: `echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' \| sudo tee /etc/udev/rules.d/99-kvm4all.rules && sudo udevadm control --reload-rules && sudo udevadm trigger --name-match=kvm`. GitHub: nested virtualisation is "not officially supported … experimental". | [P] [changelog 2024-04-02](https://github.blog/changelog/2024-04-02-github-actions-hardware-accelerated-android-virtualization-now-available/); [V] github/docs `concepts/runners/github-hosted-runners.md` |
| Real free disk | About 25–29 GB on `/` by default. Removing Android (~14 GiB), .NET, GHC, large apt packages, Docker images and the tool cache frees up to ~31 GB more (several minutes). The `/mnt` temp disk can be merged in with LVM for **60 GB or more**. **Do not remove swap**: jobs die silently. | [2] [jlumbroso/free-disk-space](https://github.com/jlumbroso/free-disk-space), [easimon/maximize-build-space](https://github.com/easimon/maximize-build-space) |
| Job time limit | **6 hours** per job (hosted); a workflow run can last 35 days | [V] github/docs `reference/limits.md` |
| Artifacts | 90-day default retention, settable from 1 to 90 days; 500 artifacts per job; v4 artifacts are immutable. Storage quotas apply to private repositories (public: free). No per-artifact size cap is documented. | [V] [upload-artifact README](https://github.com/actions/upload-artifact) (v7.0.2); github/docs billing |
| Release assets | **Each file under 2 GiB**, up to **1,000 assets per release**, "no limit on the total size of a release, nor bandwidth usage" | [V] github/docs `about-releases.md` and `data/variables` |
| `GITHUB_TOKEN`, releases | Yes, with `permissions: contents: write` (`gh release create`, upload assets) | standard; github/docs |
| `GITHUB_TOKEN`, Pages | Deploy **yes** (`actions/deploy-pages@v5` needs `pages: write` + `id-token: write` and the `github-pages` environment). **Enabling** Pages: **no**. `configure-pages`' `enablement` "requires a token other than `GITHUB_TOKEN`". An admin or maintainer must once set **Settings → Pages → Source: GitHub Actions** (or use a PAT). | [V] [configure-pages action.yml](https://github.com/actions/configure-pages), [deploy-pages README](https://github.com/actions/deploy-pages) |

**So what for AurOS:**

- `docs/RELEASE.md` §3 and STATUS 7.3 ("needs a release made from the
  GitHub page") can be closed by a workflow with `contents: write`: one
  1.84 GB asset fits.
- KVM is available, so full Windows-VM tests fit within 6 hours. The
  "No KVM" in `docs/handoff/TRAPS.md` is about the development sandbox,
  not the hosted runners.
- Pin `ubuntu-24.04` before November.
- Pages needs one click by the owner.

---

## 9. Installing apps on 24.04 without snap

**Packages (noble)** [V archive]:

- `gnome-software` 46.0-1ubuntu2 and `gnome-software-plugin-flatpak`
  46.0-1ubuntu2, both in **universe**. Do not install
  `gnome-software-plugin-snap`.
- `flatpak` and `libflatpak0` 1.14.6-1ubuntu0.1 (universe, -updates).
- Also needed: `xdg-desktop-portal` 1.18.4, `xdg-desktop-portal-gnome`
  46.2 or `-gtk`, `packagekit` 1.2.8, `appstream`.
- Universe security fixes come from Canonical only through Ubuntu Pro;
  flatpak did get a noble-updates fix.

**Flathub without network at build time:**

- flatpak reads statically configured remotes from
  **`/etc/flatpak/remotes.d/*.flatpakrepo`** or
  `/usr/share/flatpak/remotes.d/` ("intended for OS vendors") and
  imports them on first use. Offline `flatpak remote-add --system
  flathub ./flathub.flatpakrepo` inside the chroot also works. [V] [flatpak(1) docs and NEWS](https://github.com/flatpak/flatpak/blob/main/doc/flatpak.xml)
- The `.flatpakrepo` is a small INI file that carries the **public**
  `GPGKey=` inline, so **vendoring it is fine**. Fedora, Mint and
  elementary ship Flathub preconfigured.
- Fetch it once from `https://dl.flathub.org/repo/flathub.flatpakrepo`
  on a machine that can reach it, and pin its SHA-256 in the repository.
  This environment's proxy refuses `dl.flathub.org`, so I could not
  check the fingerprint.
- AppStream data downloads at first online refresh. That refresh needs
  no password (policy `appstream-update` = yes).

**Polkit:**

- What noble already allows [V, from the noble `.deb`s]:
  - `flatpak`'s shipped rule grants `app-install`, `runtime-install`,
    `app-uninstall`, `runtime-uninstall` and `modify-repo` **without a
    password** to an **active, local** user in the **`sudo`** group.
  - Updates (`app-update`, `runtime-update`) are allowed for everyone.
  - PackageKit's `package-install` is `auth_admin_keep` and
    `package-remove` is `auth_admin`. Noble's rule only waives
    `upgrade-system` and `trigger-offline-update` for `sudo`.
- To make deb installs and removals passwordless too, add for example
  `/etc/polkit-1/rules.d/60-auros-software.rules`: `polkit.addRule`
  returning `YES` for `org.freedesktop.packagekit.package-install`,
  `package-remove` and `system-update` when
  `subject.active && subject.local && subject.isInGroup("sudo")`.
  Do not waive `package-install-untrusted` or
  `system-sources-configure`. polkitd 124 reads JS rules.
- Or keep the person out of `sudo` and match a dedicated group in both
  rules.

**The apps on Flathub** (repository state read 2026-10-23; the verified
badge could not be read directly because `flathub.org` refuses this
environment):

| App | On Flathub | Verified / official | Notes |
|---|---|---|---|
| **Google Chrome** `com.google.Chrome` | Yes, **stable** 156.0.8078.47 (2026-10-21) | **No**: community wrapper. It downloads Google's `.deb` from `dl.google.com` at install time (extra-data). Google says Chromium's new Flatpak tooling is "not a commitment" to ship one. | [V] [manifest](https://github.com/flathub/com.google.Chrome); [2] [OMG! Ubuntu 2026-08](https://www.omgubuntu.co.uk/2026/08/chromium-adds-flatpak-packaging-support) |
| Firefox `org.mozilla.firefox` | Yes | **Yes**, published by Mozilla (no `flathub/` build repository: direct upload) | [V] no repository; [2] VideoLAN forum 2023 (blue check) |
| LibreOffice `org.libreoffice.LibreOffice` | Yes | **Yes** (blue check since 2023) | [2] |
| VLC `org.videolan.VLC` | Yes | **Probably not** (reported unverified, 2026-05) | [2] [linuxcapable](https://linuxcapable.com/install-vlc-media-player-on-fedora-linux/) |
| Zoom `us.zoom.Zoom` | Yes, x86_64 only | **No**: "not verified by, affiliated with, or supported by Zoom" | [2] [linuxcapable](https://linuxcapable.com/install-zoom-on-fedora-linux/) |
| Spotify `com.spotify.Client` | Yes, x86_64 only | **No**: "community flatpak packaging of official spotify binaries" | [V] repository README |
| Skype `com.skype.Client` | The repository still exists | Skype itself was **retired by Microsoft in May 2025** | don't offer it |
| Microsoft Teams | **No official Linux client**; `com.github.IsmaelMartinez.teams_for_linux` is unofficial | — | offer teams.microsoft.com in the browser |
| WhatsApp | **No official Linux client**; unofficial wrappers (e.g. ZapZap `com.rtosta.zapzap`) | — | offer web.whatsapp.com as a web app |
| Microsoft Edge `com.microsoft.Edge` | Yes | No (community) | [V] repository |

**So what for AurOS:**

- Vendor the `.flatpakrepo` into `/etc/flatpak/remotes.d/`.
- Put the grandma user in `sudo`, or a dedicated group, plus one
  PackageKit rule. Flatpak installs are then passwordless already.
- Ship Firefox and LibreOffice from Flathub (verified). That also closes
  `docs/RELEASE.md` §4: no Mozilla APT host is needed at build time, only
  Flathub at first boot.
- Chrome, Zoom and Spotify are unverified wrappers that each need their
  vendor's CDN at install time. Label them, or offer web apps (WhatsApp,
  Teams, and Skype's replacement).
- Consider `--subset=verified` only if the unverified wrappers are
  dropped.
