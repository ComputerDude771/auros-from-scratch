# AurOS security and reliability program

Started 2026-10-23 at the owner's request: "make a very secure OS so ours
isn't known as the hackable one, and not like Windows which keeps
BSOD'ing. Every version of our OS a user could want, completely secure,
without hurting performance."

Honest framing: no OS is "completely secure". The goal is to be the
hardest target in its class and to recover by itself: **nothing reachable
from the network, every attack contained where it lands, fixes delivered
fast and automatically, and a machine that can always get itself back
into a working state.** Every control must earn its performance cost on
a 2013 laptop with 4 GB of RAM.

## Who attacks a great-grandmother's computer (threat model, short form)

| Attacker | How they get in | What we do |
|---|---|---|
| Scammers (the #1 real threat) | fake "your PC is infected" pages, phone calls, getting her to install remote-access tools, fake apps in stores | browser content blocking and policies, warnings when a remote-access program is installed, verified-only or curated store options, plain "AurOS will never call you" messaging, scam pages that can't trap the screen |
| Malicious downloads and apps | Flatpak apps, `.deb` files, browser downloads | sandboxed Flatpak, no `.exe` execution, AppArmor confinement, polkit grants scoped to install/remove only |
| The network (café Wi-Fi, a hostile router) | listening services (cups-browsed RCE 2024, avahi), DNS spoofing | zero listening ports by default, firewall default-deny inbound, patched or disabled discovery, MAC randomisation |
| Browser exploits | malicious pages and ads | Firefox from Mozilla, auto-updated, sandboxed, HTTPS-only, safe browsing, ad and malware blocking |
| Privilege escalation | bugs in our own root-running code (aurfirst, answer.sh, the installer's helpers), polkit rules, setuid binaries | least privilege, input validation, fuzzing our parsers, compiler hardening, AppArmor for our daemons |
| Kernel attacks | unprivileged BPF, user namespaces, debugfs, kernel pointer leaks | lockdown (already, via Secure Boot), sysctl hardening, module signature enforcement |
| Theft and lost laptops | reading the disk, booting it | disk encryption tied to the TPM (no password to remember) plus a lock: decision pending with the owner |
| Boot chain | bootkits (BlackLotus-class), revoked shims, SBAT | Secure Boot stays on, the `bootchain` verifier, dbx/SBAT updates via fwupd, no MOK keys |
| Supply chain | our build, our update channel, Actions | pinned actions by SHA, least-privilege tokens, reproducible hashes, signed update channel for AurOS's own packages |
| The Windows-side installer | DLL hijacking next to a downloaded `.exe`, an elevated scheduled task pointing at a user-writable file, swapped pieces between check and use, the Wi-Fi password on the ESP | safe DLL search, protected copies, verify at use, minimal secret exposure |
| Physical USB | BadUSB keyboards while she's away | lock-screen policy for new input devices (cheap version) |
| Future | AI-written phishing, faster exploit turnaround, post-quantum | faster updates, scam-focused UX, crypto agility in our update channel |

## Reliability: "never the blue screen"

| Failure | What we do |
|---|---|
| A bad kernel or update | grub falls back to the previous kernel (bootchain, exists); boot counting so a failed boot rolls back automatically; apt interrupted → repaired at boot |
| Running out of memory (4 GB + Firefox) | zram swap (compressed, cheap) + systemd-oomd/earlyoom so the machine slows instead of freezing |
| The desktop crashing | systemd restarts aurshell; crash loop → safe mode with the plain home |
| Kernel panic | reboot by itself after 10 s and fall back |
| Full disk | warnings well before; logs and caches capped |
| Power cut during an update | ext4 journal, dpkg repair at boot, atomic boot-chain writes (exists) |
| A broken GPU driver | simpledrm fallback |

## How it runs (coordinated, adversarial)

1. **Audit** (workflow `security-audit`): one auditor per domain (Windows installer, staging, boot chain, kernel/sysctl, network/services, privilege paths and polkit, our C code's attack surface, browser/scams/apps, updates and supply chain, encryption/theft, reliability) → each finding challenged by skeptics → `docs/security/FINDINGS.md`, a prioritised backlog with a performance cost for each control.
2. **Implement** (workflow `security-implement`): one engineer per domain in its own worktree, small green commits, a test that proves each control is on and fails when it's removed.
3. **Red team** (workflow `security-redteam`): attackers per domain try to break each control on a built image in a KVM VM on Actions (open ports scan, polkit escalation attempts, fuzzing our parsers with sanitizers, kernel-hardening-checker, lynis, scam-page trap tests, supply-chain tampering) → findings back to step 2 until a round finds nothing new.
4. **Performance gate**: boot time, idle RAM, Firefox start and memory on a 4 GB VM, before and after; no control ships if it costs more than its budget.
5. **Every version**: all five AurOS profiles (desktop, office, revive, school-kiosk, multilingual) get the same baseline; phase-2 official distros keep their publishers' security and our install path must never weaken it (Secure Boot on, no MOK, BitLocker untouched).

## Status

See the Progress log in `docs/handoff/SESSION-2026-10-23.md` and
`docs/security/FINDINGS.md` once the audit writes it.
