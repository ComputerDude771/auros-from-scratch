export const meta = {
  name: 'current-state',
  description: 'Write docs/handoff/CURRENT-STATE.md: newest, problems, plans, owner questions, from what is on the branch now',
  phases: [
    { title: 'Read', detail: 'six readers, one area each' },
    { title: 'Write', detail: 'merge, cross-check, push' },
  ],
}

const REPO = '/home/user/auros-from-scratch'
const BR = 'origin/claude/brave-hypatia-x72g1m'
const HOW = `Repository ${REPO}, branch ${BR}. Read-only: run \`git -C ${REPO} fetch -q origin claude/brave-hypatia-x72g1m\` first, then use \`git -C ${REPO} show ${BR}:<path>\`, \`git -C ${REPO} log --format='%h %ad %s' --date=format:'%m-%d %H:%M' ${BR} -- <paths>\`, \`git -C ${REPO} ls-tree -r --name-only ${BR} <dir>\`. Never edit or commit. Start with \`git -C ${REPO} show ${BR}:START-HERE.md\` (context) and the tail (last 60 lines) of docs/handoff/SESSION-2026-10-23.md. You may read GitHub Actions run status for the public repo (R=\$(sh ${REPO}/tools/repo.sh), its owner/name) via \`curl -sS "https://api.github.com/repos/$R/actions/runs?branch=claude/brave-hypatia-x72g1m&per_page=40"\` and releases via \`curl -sS https://api.github.com/repos/$R/releases\`. Report FACTS with commit hashes, file paths, run ids, and say plainly when something is unknown or unverified. Output plain markdown, under 700 words, with exactly these headings: "Newest (last ~6 hours)", "Works and is proven (how)", "Broken, unproven or risky", "In flight right now (may be half-done)", "Next steps, in order", "Needs the owner".`

const AREAS = [
  { k: 'install-path', s: 'Streams A2 (install engine / fast path) and B (wizard + preflight): docs/handoff/streams/A2.md and B.md if present, docs/AURBRIDGE.md "Fast path: status", src/aurbridge, src/aurstage, docs/shots/wizard, tools/*test.sh results in docs/results, windows.yml.' },
  { k: 'ci-release-e2e', s: 'Streams E (build/tests/release workflows) and F (real-Windows e2e): docs/handoff/streams/E.md and F.md, docs/HANDOFF-RUNBOOK.md, .github/workflows/*.yml, the Actions runs and releases (is there a prerelease with AurOS-Setup.exe? did any e2e run reach AurOS? M1/M2/M3 status, evidence tags e2e-*).' },
  { k: 'desktop-software-security', s: 'The desktop and software work (stream C, finished) and security streams S1 (OS hardening + reliability), S2 (browser + scams), S3 (fuzzing + compiler hardening): docs/handoff/streams/S1.md S2.md S3.md, docs/security/*, src/aurshell, build/forge, rootfs/, profiles/desktop.profile, tools/storetest.c nmtest.sh etc.' },
  { k: 'theft-updates-portability', s: 'Streams S4 (encrypted disk + PIN, docs/security/THEFT.md), S5 (AurOS update channel, docs/security/UPDATES.md), M (no hard-coded repository, docs/handoff/streams/M.md, tools/portabilitytest.sh), plus docs/OWNER-SETUP.md and docs/MOVING.md.' },
  { k: 'othersys-profiles-ferry', s: 'Streams H (other OSes, phase 2: docs/OTHER-SYSTEMS.md, src/othersys, tools/othersys, othersys.yml, docs/handoff/streams/H.md), P (every profile builds and boots: profiles.yml, docs/handoff/streams/P.md), R (Ferry: src/ferry, docs/handoff/streams/R.md).' },
  { k: 'launch-business', s: 'Launch readiness and the owner side: docs/LAUNCH-READINESS.md and docs/security/FINDINGS.md if present (if absent, say their audits did not finish and must be re-run with tools/workflows/), docs/research/journey-audit-2026-10-23.md vs today, docs/RELEASE.md, stream K (help pages rootfs/usr/share/auros/help, SECURITY.md, docs/SUPPORT.md, docs/legal drafts, website/advanced.html), the website (website/, tools/site/check.mjs), the unverified review-findings table in docs/handoff/RESUME-NOW.md (which are fixed now?), and what must be true before people can be charged money.' },
]

const parts = await parallel(AREAS.map(a => () => agent(`${HOW}\n\nYour area: ${a.k}. ${a.s}`, { label: `read:${a.k}`, phase: 'Read' })))

const result = await agent(`You write docs/handoff/CURRENT-STATE.md for AurOS and push it. A new agent will receive ONLY this repository (no other context) and must be able to start exactly where the previous session stopped. ${HOW}

Here are six area reports (some may be null if a reader failed — then read that area yourself):
${parts.map((p, i) => `\n\n=== ${AREAS[i].k} ===\n${p}`).join('')}

Write the file with these sections: (0) a dated one-paragraph summary: where AurOS is right now, in plain words, and the single most important next action; (1) Newest — what landed in the last ~6 hours, grouped by area, with commit hashes; (2) What works and how it is proven (real Windows / VM / simulation / unit), honestly; (3) Problems — broken, unproven, risky, contradictory (cross-check the reports against each other and the code; resolve contradictions by reading the code); (4) In flight — work that may be half-done when the session ended, and how to tell; (5) The plan — next steps in strict priority order (a working published version first: prerelease → e2e pass → promote → website live; then fast path; then security backlog + encrypted disk/PIN; then launch-readiness blockers incl. a real-PC test campaign; then phase 2, profiles, Ferry, translations, accessibility), each with files and the command or workflow to run; (6) Needs the owner — every click, secret, decision or real-world action only he can do, one line each; (7) Where to look — an index of the handoff files. Plain, specific, no filler; under 1800 words.

Then: \`git -C ${REPO} worktree remove --force /tmp/claude-0/-home-user-auros-from-scratch/0f9a7adc-af00-5b1e-9166-5e932ada07e6/scratchpad/wt-state 2>/dev/null; git -C ${REPO} worktree add --detach /tmp/claude-0/-home-user-auros-from-scratch/0f9a7adc-af00-5b1e-9166-5e932ada07e6/scratchpad/wt-state ${BR}\`, write the file there, add one line near the top of START-HERE.md under its title: "> **What's newest, what's broken, and the plan: [docs/handoff/CURRENT-STATE.md](docs/handoff/CURRENT-STATE.md).**", append to docs/handoff/SESSION-2026-10-23.md "- <HH:MM UTC> CURRENT-STATE.md written (newest, problems, plan, owner actions).", commit with subject "handoff: current state — newest, problems, plan, owner actions" and a one-line body, ending with exactly:\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_017qioTeikWexSpAiM6y9oSA\nthen \`git fetch origin claude/brave-hypatia-x72g1m && git rebase origin/claude/brave-hypatia-x72g1m && git push origin HEAD:refs/heads/claude/brave-hypatia-x72g1m\` (retry 4x, 2/4/8/16 s), and remove the worktree. Return the commit hash and section (0) verbatim.`, { label: 'write', phase: 'Write', effort: 'high' })

return result
