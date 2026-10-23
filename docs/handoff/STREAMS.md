# STREAMS — every parallel workstream, its files, and its standing brief

If you are a new session: each stream below can be respawned as one
background agent with the prompt "You are stream <ID> of the AurOS
project. Read docs/handoff/STREAMS.md (the common rules and your
stream's brief), docs/handoff/RESUME-NOW.md and the Progress log in
docs/handoff/SESSION-2026-10-23.md, then continue your stream from the
branch's current state." Check the Progress log first: a stream marked
DONE needs no respawn.

## Common rules (every stream)

1. **Branch** `claude/brave-hypatia-x72g1m`, pushed with
   `git fetch origin claude/brave-hypatia-x72g1m && git rebase origin/claude/brave-hypatia-x72g1m && git push origin HEAD:refs/heads/claude/brave-hypatia-x72g1m`
   (retry 2/4/8/16 s on races or network). Never another branch, never a PR.
2. **Own worktree**: `git -C /home/user/auros-from-scratch worktree add -b wt-<id> <scratchpad>/wt-<id> origin/claude/brave-hypatia-x72g1m`
   (scratchpad = `/tmp/claude-0/-home-user-auros-from-scratch/0f9a7adc-af00-5b1e-9166-5e932ada07e6/scratchpad`).
   The main checkout `/home/user/auros-from-scratch` is shared: never edit there.
   If you need the built rootfs, symlink ONLY `work/forge/desktop/rootfs` read-only into your own `work/`.
3. **Save constantly — the owner's usage can run out at any minute.** Commit and push every
   green step (at least every 30 minutes of work). Before any step longer than 10 minutes,
   push what you have and append one line to the Progress log in
   `docs/handoff/SESSION-2026-10-23.md` (append only; others write there too) saying what you
   are about to do and what's left. Unpushed work is lost work.
4. **Stay in your files** (table below). If you need a change in another stream's file, make
   the smallest possible edit, say so in the commit body, and mention it in your final report.
   Prefer new files plus a one-line hook over editing shared files.
5. **Commit messages**: plain subject; a body that says why; end with exactly
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
   `Claude-Session: https://claude.ai/code/session_017qioTeikWexSpAiM6y9oSA`. No model names in files.
6. **Container limits**: 4 CPUs shared by ~15 agents, ~5 GB disk free (keep your scratch under
   300 MB, delete it when done), no KVM, network blocks Microsoft/Mozilla/Flathub/Google. Heavy
   builds and VM tests run on GitHub Actions (public repo, KVM after a udev rule); the free plan
   allows 20 concurrent jobs, so any new workflow must use `concurrency` (one run per branch,
   `cancel-in-progress: false`) and `max-parallel` ≤ 2 for matrices. Actions artifacts can't be
   downloaded from the container; publish evidence as prerelease assets
   (`releases/download/<tag>/<file>` works with curl).
7. **Culture**: refuse first; prove each check fails when the thing it checks is broken; tests
   for everything; plain words for anything a person sees (`docs/EASY.md`); performance budget
   for a 2013 laptop with 4 GB; never ship an unproven destructive change; never add a
   passwordless root-equivalent permission (owner's security bar).
8. **Owner's standing decisions**: `docs/handoff/SESSION-2026-10-23.md` D1-D11 (no code signing,
   no Microsoft services, publish via GitHub Release + Pages, other OSes = official ones,
   encrypted disk + PIN, Wi-Fi tick box, pause BitLocker).

## Stream table

| ID | Stream | Owns | Status |
|---|---|---|---|
| A2 | install engine fast path | `src/aurbridge/{phases,plat*,main,simmain,report,format,inflate}.*`, `src/aurstage/**`, `build/staging`, install tests, `docs/AURBRIDGE.md`, engine glue in wizard.c | running (since 04:25) |
| B | wizard + preflight | `src/aurbridge/{wizard,preflight,fix*,sbdb}.*`, `docs/shots/wizard/` | running |
| C | desktop + software | (finished; files now free: `src/aurshell/**`, `build/forge`, `profiles/**`, `rootfs/**`) | DONE |
| D | website | (finished; `website/**`, `pages.yml`, `tools/site/`) | DONE |
| E | release pipeline | `.github/workflows/{build,release,tests}.yml`, `build/sign`, `docs/{SIGNING,RELEASE,HANDOFF-RUNBOOK}.md` | running |
| F | real-Windows e2e | `.github/workflows/e2e.yml`, `tools/e2e/**` | running |
| H | other OSes (phase 2) | `src/othersys/**`, `tools/othersys/**`, `.github/workflows/othersys.yml`, `docs/OTHER-SYSTEMS.md` | running |
| S1 | OS hardening + reliability | new files under `rootfs/etc/**` (sysctl.d, modprobe.d, nftables, systemd drop-ins, zram, oomd, cups/avahi config), a `packages_security` line + one stage in `build/forge`, kernel cmdline in `rootfs/usr/lib/auros/grub.cfg.in` (cmdline only), `tools/hardentest*` | new 04:35 |
| S2 | browser + scam protection | `rootfs/etc/firefox/**`, new `src/aurshell/guard*.c` + minimal hooks in aurshell, store remote-access warning, `tools/scamtest*` | new 04:35 |
| S3 | memory safety of our C | `tools/fuzz/**`, `src/common/**`, `src/aurwl/**`, desktop-entry parsing in `src/aurshell/apps.c`, compiler-hardening flags in build scripts (minimal edits) | new 04:35 |
| S4 | theft: LUKS2 + TPM2 + PIN (D9) | `docs/security/THEFT.md`, new `src/aurstage/luks.*`, new `rootfs/usr/lib/auros/lock*`, initramfs hooks under `rootfs/etc/initramfs-tools/**`, new `src/aurshell/lock*.c` | new 04:35 (design first) |
| S5 | AurOS update channel | `docs/security/UPDATES.md`, `.github/workflows/aptrepo.yml`, `rootfs/etc/apt/**` for our repo, `tools/updatetest*` | new 04:35 |
| P | every profile builds and boots | `.github/workflows/profiles.yml`, `profiles/{office,revive,school-kiosk,multilingual}.profile`, `tools/profiletest*` | new 04:35 |
| R | Ferry: her files come across | `src/ferry/**` | new 04:35 |
| K | help, support and legal drafts | `rootfs/usr/share/auros/help/**`, `docs/legal/**`, `SECURITY.md`, `docs/SUPPORT.md`, `website/advanced.html` | new 04:35 |
| M | portability: no hard-coded repository | replaces `ComputerDude771/auros-from-scratch` / `computerdude771.github.io/auros-from-scratch` outside docs with values derived from the repository (workflows: `${{ github.repository }}`, `${{ github.repository_owner }}`; build scripts: `AUROS_REPO` defaulting to `$GITHUB_REPOSITORY` or the git remote; website: derived from its own Pages address, else one constant; wizard/installer strings: baked at build time) — minimal edits in other streams' files, tests that grep for the literal | running (since 05:12) |
| W | workflows (lead) | scripts in `/root/.claude/projects/-home-user-auros-from-scratch/0f9a7adc-af00-5b1e-9166-5e932ada07e6/workflows/scripts/` (container-local; if lost, re-author from the descriptions in RESUME-NOW §4b/§6 and docs/security/PLAN.md): `security-audit` run wf_fc726b57-c9a (resumed 05:15) → writes docs/security/FINDINGS.md; `launch-readiness` run wf_0e3f5766-e02 (fresh 05:15; old journal corrupt) → docs/LAUNCH-READINESS.md; `tonight-review` run wf_f84f4f83-bfc (resumed 05:15) | running |

## Briefs for the streams started at 04:35

**S1 OS hardening + reliability.** Make a stock AurOS image unreachable from the network and
hard to escalate on, with near-zero performance cost, and make it recover from the failures that
make Windows blue-screen. Network: firewall default-deny inbound (nftables), no listening
sockets by default; cups-browsed (CVE-2024-47176 chain) — remote discovery off or the package
gone while local/USB printing still works; avahi publishing off. Kernel: sysctl hardening
(kptr_restrict=2, dmesg_restrict=1, unprivileged_bpf_disabled=1, bpf_jit_harden=2,
yama.ptrace_scope=1, kexec_load_disabled=1, fs.protected_*=1/2, suid_dumpable=0, sysrq
limited) and cheap cmdline options (init_on_alloc=1, slab_nomerge, randomize_kstack_offset=on,
vsyscall=none, debugfs=off; NOT init_on_free); keep Flatpak/bubblewrap working (noble's
AppArmor userns rules). Reliability: zram swap, systemd-oomd (or earlyoom) tuned for 4 GB,
kernel.panic=10 + panic_on_oops=0, aurshell restart with crash-loop → safe mode, dpkg repair at
boot, journald size caps, fsck on errors. Each control: a test (static on the rootfs, plus a
boot check on Actions where needed) that fails when removed; measure boot time/idle RAM before
and after (KVM VM on Actions) and record it in `docs/security/HARDENING.md`. Read the
`security-audit` output (`docs/security/FINDINGS.md`) when it lands and take its kernel-os,
network and reliability items.

**S2 browser + scam protection.** The #1 real threat to her is scams. Firefox enterprise policy
(`/etc/firefox/policies/policies.json`): HTTPS-only, safe browsing on, block dangerous
downloads, no telemetry, extension policy that preinstalls uBlock Origin from
addons.mozilla.org (the end user's machine can reach it) and blocks others unless she chooses,
sane defaults for an elderly user. In aurshell: a fullscreen page can never trap her (a big,
always-available "Get me out of this page" path; Esc/Home always works; beeping audio can be
silenced). In "Get more programs": when a remote-access program (AnyDesk, TeamViewer, RustDesk,
etc.) is about to be installed, a plain warning: "Scammers ask people to install this. Only
continue if YOU called someone you know." Tests for each.

**S3 memory safety.** Fuzz every parser of untrusted input we wrote: the TrueType rasterizer and
PNG code in `src/common`, `.desktop` parsing (apps.c), theme/wall parsers, the Wayland compositor's
request handling in `src/aurwl` (malicious clients), and coordinate with A2 for
`src/aurbridge/inflate.c` (send them reproducers, don't edit their file). Build with
ASan+UBSan, libFuzzer harnesses under `tools/fuzz/`, seed corpora, run each for a bounded time
locally (CPU is shared: one fuzzer at a time, `nice`), fix every crash with a regression test.
Then add hardening compiler flags to every C build (`-O2 -D_FORTIFY_SOURCE=3
-fstack-protector-strong -fstack-clash-protection -fcf-protection=full -fPIE -pie
-Wl,-z,relro,-z,now`) and prove them with a checksec-style test on the built binaries.

**S4 theft: encrypted disk + PIN (owner decision D9).** Design first, in
`docs/security/THEFT.md`, then implement: AurOS root as LUKS2; unlocked by TPM 2.0 (PCR 7)
plus a 4-6 digit PIN (`systemd-cryptenroll --tpm2-with-pin`, noble's systemd 255) with a
friendly full-screen PIN prompt at start (not a console) and the same PIN on wake (aurshell lock
screen via PAM); PCs without TPM 2.0 (common in the target market) — the honest fallback and its
strength; re-sealing before dbx/firmware updates change PCR 7; a recovery key she can actually
keep (printed sheet, shown once, stored where?); Adiantum on CPUs without AES-NI; how staging
writes into the opened LUKS device (coordinate with A2 who owns `src/aurstage`; prefer new files
and a hook). Prove in a KVM VM with swtpm on Actions: PIN unlock, wrong PIN, TPM change →
recovery, performance cost.

**S5 update channel.** AurOS's own components (aurshell, aurfirst, bootchain, themes) have no
update path, so a security fix can't reach her. Design and build one with no Microsoft service:
an apt repository published on GitHub Pages or Releases, signed with an OpenPGP key that lives
only in a repository secret (the owner adds it once: write the exact steps), `signed-by` pinned
in the image, built and published by `.github/workflows/aptrepo.yml` from tagged builds, with
key rotation and expiry documented. Test with a throwaway key in CI: a signed update installs,
an unsigned or tampered one is refused.

**P every profile.** "Every version of our OS a user could want" must build and boot: add
`.github/workflows/profiles.yml` building office, revive, school-kiosk and multilingual
(max-parallel 1, triggered by `[profiles]` in the subject or changes to `profiles/**`), boot
each in a KVM VM to its desktop (screenshot), and fix profile-specific breakage in those four
profile files. Report each profile's image size and boot result.

**R Ferry.** When she says "it works", her Documents, Pictures, Desktop, Downloads, Music,
Videos and her browser bookmarks come across from Windows, plainly and safely (read-only on the
Windows side; never her passwords). Extend `src/ferry` and its tests (`src/ferry/tests`), fuzz
its hive/registry parsers, measure time on a large fixture, and make the progress she sees
honest.

**K help, support, legal drafts.** Also add `website/advanced.html` ("for someone who knows computers": switching a BIOS-mode PC to UEFI with mbr2gpt, and Intel RST "RAID On" → AHCI safely), linked from the wizard's can't-install page text and the site footer; run `node tools/site/check.mjs`. Write the in-OS help pages (`rootfs/usr/share/auros/help/`:
getting online, installing a program, printing, video calls, getting back to Windows, "AurOS
will never call you"), `SECURITY.md` (how to report a vulnerability), `docs/SUPPORT.md` (how a
person gets help), and **drafts** of terms of use, privacy policy and disclaimer in
`docs/legal/` clearly marked "draft — must be reviewed by a lawyer before use". Plain language.
