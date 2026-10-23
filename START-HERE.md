# START HERE (for an agent who has only this repository)

You are taking over AurOS from a session that ran out of usage on
2026-10-23. Everything you need is in this repository; nothing else
carries over (no transcripts, no scratch files, no running agents). Read
this file fully, then the files it points to, then act. The work was
designed to be resumed exactly from here.

## 0. If these files are missing, the session ended before they landed

- `docs/handoff/CURRENT-STATE.md` (newest / problems / plan / owner actions):
  run `tools/workflows/current-state.js` first (Workflow tool), or write it
  yourself from the files below.
- `docs/security/FINDINGS.md`: run `tools/workflows/security-audit.js`.
- `docs/LAUNCH-READINESS.md`: run `tools/workflows/launch-readiness.js`.
- A stream with no `docs/handoff/streams/<ID>.md`: its brief is in
  `docs/handoff/STREAMS.md` or §5 below, its progress in the Progress log.

## 1. What AurOS is, in one paragraph

A free Linux desktop (Ubuntu 24.04 base, our own shell `aurshell`) that a
non-technical person — the owner's test is *his great-grandmother* —
installs on her Windows PC by opening a website, downloading one file
(`AurOS-Setup.exe`), clicking through three screens, and restarting once.
Windows and her files stay; until she says "it works", Windows is kept
as it was; "Put Windows back" removes AurOS. Target: under 10 minutes,
on (nearly) every Windows 10/11 PC, then installing programs from "Get
more programs". The owner wants it secure ("not the hackable OS"), never
crashing ("not like Windows' blue screens"), lightweight, and good enough
that people will pay for it.

## 2. The owner and how to work with him

- Aaroh, a builder (hardware, startups). He doesn't read code; he wants
  results, blunt plain answers, a recommendation not a menu, and to be
  asked only high-impact questions (one line each).
- **Standing orders**: full speed, many parallel agents/workflows, but
  agents must not trip over each other (file ownership, `docs/handoff/STREAMS.md`);
  **save constantly** (push every green step); write progress down
  (`docs/handoff/SESSION-2026-10-23.md` Progress log); adversarial review of
  important work; prove every check fails when broken; "be 100% sure"
  before anything is sold.
- **Decisions he made (do not relitigate)** — full text in
  `docs/handoff/SESSION-2026-10-23.md`, D1-D11:
  - D1 the slow install work happens in Windows during the download ("fast path");
  - D2/D10 her Wi-Fi comes across, with a visible pre-ticked box;
  - D3 three-screen wizard, no typing; she picks a look (Like Windows / Like a Mac / Big and simple / One thing at a time);
  - D4 only real dangers stop an install;
  - D5 Flathub in "Get more programs", no password to install from the store;
  - D6 build/test/publish on GitHub Actions; GitHub Release + Pages;
  - D7 **no code signing, no keys from Microsoft, no Microsoft services, ever**;
  - D8 "other OSes" = the official Ubuntu/Mint/Fedora installed unattended (phase 2);
  - D9 **encrypted disk + short PIN** (TPM);
  - D11 **BitLocker is paused for one restart, never decrypted**.
  - Also: downloaded `.deb` installers will need her PIN (no passwordless
    root-equivalent permission, ever).
- He will move to another repository later; the code must not hard-code
  this one (`docs/MOVING.md`, stream M).

## 3. Read next, in this order

1. `docs/handoff/RESUME-NOW.md` — state, CI, release flow, unverified review findings.
2. `docs/handoff/STREAMS.md` — every workstream, its files, common rules, briefs.
   `docs/handoff/METHODOLOGY.md` — HOW the work was run (lead + 14 stream agents,
   worktrees, contracts, adversarial workflows, CI/container/owner lessons). Copy it.
3. `docs/handoff/streams/*.md` — each stream's own latest state and next steps
   (written by the stream agents at the end; trust these over older notes).
4. The tail of `docs/handoff/SESSION-2026-10-23.md` (Progress log; newest last).
5. `docs/OWNER-SETUP.md` (what only the owner can click), `docs/MOVING.md`.
6. If they exist: `docs/security/FINDINGS.md` (security backlog),
   `docs/LAUNCH-READINESS.md` (go/no-go before charging money). If they don't,
   their audits didn't finish: re-run them (`tools/workflows/README.md`).
7. Reference: `docs/AURBRIDGE.md` (installer design + "Fast path: status"),
   `docs/EASY.md` (usability law), `docs/security/PLAN.md`, `docs/handoff/TRAPS.md`,
   `docs/HANDOFF-RUNBOOK.md`, `tools/README.md`.

## 4. Your environment, and what you need

- **Git**: branch `claude/brave-hypatia-x72g1m` (PR
  https://github.com/ComputerDude771/auros-from-scratch/pull/1 tracks it). Push
  with `git fetch && git rebase origin/<branch> && git push origin HEAD:<branch>`;
  never another branch; commit trailers per `docs/handoff/STREAMS.md` rule 5
  (if your harness gives different attribution lines, use those).
- **Heavy work runs on GitHub Actions** (public repo; KVM on Linux runners;
  20 concurrent jobs max). Image builds need Linux (they can't run on a Mac);
  a Mac can edit, push, and watch CI. Workflows and their trigger words:
  `docs/HANDOFF-RUNBOOK.md` and `docs/handoff/streams/E.md`.
- You need a way to read Actions runs/logs and releases (GitHub API, `gh`, or
  a GitHub MCP server). Evidence is published as (pre)release assets, e.g.
  `releases/download/e2e-<run_id>/e2e-evidence.zip`.
- Owner-only clicks still pending (ask him once, plainly):
  Pages → Source: GitHub Actions; Environments → github-pages → No restriction;
  default branch → the working branch; later one secret for the update channel.

## 5. How to resume the work

For each stream in `docs/handoff/STREAMS.md` not marked DONE, read
`docs/handoff/streams/<ID>.md` (if present) and respawn one background agent
with: *"You are stream <ID> of AurOS. Read START-HERE.md,
docs/handoff/STREAMS.md (common rules + your brief) and
docs/handoff/streams/<ID>.md, then continue from the branch's current state.
Save constantly."* Run them in parallel; they own disjoint files.

Briefs for the streams whose briefs are not spelled out in STREAMS.md:

- **A2 install engine (fast path).** Finish D1/D2/D10/D11 per `docs/AURBRIDGE.md`
  "Fast path: status": gapselftest on real Windows (windows.yml VHDX job), shared
  planner + journal `image_on=gap`, engine prepared-mode phases (Windows online
  shrink during download → streamed gap write → verify), staging prepared mode
  (never mount the BitLocker NTFS), Wi-Fi keyfile handoff, BitLocker protector
  suspend (`-RebootCount 2`), Put-Windows-back startup task (admin-only path),
  simulation + power-cut tests. Wire into the wizard only when all green, by
  defining `bring_wifi`, `bitlocker_pause`, `AB_CHOICE_BRING_WIFI`,
  `AB_CHOICE_BITLOCKER_PAUSE` in `src/aurbridge/phases.h` (the wizard switches on
  automatically). Never ship an unproven destructive path.
- **B wizard + preflight.** Three screens, no typing, look picker, honest sizes and
  times, D4 policy, VERSIONINFO/icon, MSAA names, plain "can't install" page
  (links `advanced.html`). Keep windows.yml green; screenshots in `docs/shots/wizard/`.
- **E release pipeline.** `build.yml` (image with Firefox on Actions, staging,
  installer), `tests.yml` (KVM simulation suite + unit tests), `release.yml`
  (prerelease → e2e-gated `promote` to latest → deploy Pages). Unsigned by design.
- **F real-Windows e2e.** Windows 11 eval VM on Actions (Secure Boot, swtpm, KVM):
  Edge opens the website → "Get AurOS" → Keep anyway / Run anyway / UAC Yes → wizard
  by real input → restart → staging → AurOS "does it work?" → yes → install VLC
  from "Get more programs" → open it; then variants (BitLocker on, small disk,
  HDD-speed throttle, Fast Startup + pending reboot, Windows 10). Point it at a
  release with `INSTALLER_URL`/`INSTALLER_SHA256` in `e2e.yml`.
- **H other OSes (phase 2).** Official Ubuntu 26.04 installed unattended beside
  Windows via the same safe path (`docs/OTHER-SYSTEMS.md`, D8), proven in a KVM VM;
  then Xubuntu/Cinnamon, Mint 22.3, Fedora.

## 6. Priority order (what "done" means)

1. A **prerelease** from a green build, then the **e2e run passing end to end** on
   it (website → AurOS → program installed), then **promote** to latest (website
   goes live). This is the owner's "working version".
2. The fast path wired (BitLocker PCs without decryption; ~10 minutes).
3. Security backlog critical/high items (`docs/security/FINDINGS.md`) and the
   encrypted disk + PIN.
4. Launch-readiness blockers (`docs/LAUNCH-READINESS.md`), including a small
   real-PC test campaign the owner runs — a VM can't prove a physical PC.
5. Phase 2 other OSes; every profile; Ferry; translations; accessibility.

Keep `docs/handoff/RESUME-NOW.md` and this file current as you go, so the
next agent after you can do the same.
