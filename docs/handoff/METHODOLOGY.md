# METHODOLOGY — how the 2026-10-23 session ran, so you don't re-think it

Written by the lead agent at the end of the session. Read after
`START-HERE.md`. This is the operating playbook: copy it, don't redesign it.

## 1. Shape: one lead, many owners

- **The lead (you) does not write product code.** It reads, decides, writes the
  briefs, routes findings, unblocks CI, reviews landings, keeps the handoff files
  current, and talks to the owner. Every product change is made by a stream agent.
- **One background agent per stream**, each owning a disjoint set of files
  (`docs/handoff/STREAMS.md`). At peak: **14 stream agents + 3-4 workflows**
  (each workflow ran 10-60 short-lived agents). Streams that ran:
  A/A2 install engine · B wizard+preflight · C desktop+software (done) · D website (done) ·
  E build/test/release · F real-Windows e2e · H other OSes · S1 OS hardening+reliability ·
  S2 browser+scams · S3 fuzzing+compiler hardening · S4 encrypted disk+PIN · S5 update channel ·
  P all profiles · R Ferry · K help/legal drafts · M portability.
- **Research and audits are separate one-shot agents** that only read and write a
  doc (journey audit, every-machine research, other-OS research). They gave the
  facts that drove every decision; do this before building anything big.

## 2. How each agent is set up (the brief template)

Every brief contains: the goal in the owner's terms (the great-grandmother,
10 minutes, every machine); files to read first; **exactly which files it owns**
and that others are editing other files concurrently; its own git worktree
(`git worktree add -b wt-<id> <scratch>/wt-<id> origin/<branch>`); the push loop
(`fetch → rebase → push HEAD:<branch>`, retry 2/4/8/16 s); commit-message rules;
container limits; CI facts; the culture (refuse first, prove each check fails
when broken, never ship an unproven destructive path, no passwordless
root-equivalent permissions); milestones in order; **"save constantly"** (push
every green step; a Progress-log line before any step > 10 min; a
`docs/handoff/streams/<ID>.md` state file); and a word-capped final report.
Since STREAMS.md exists, a new stream's prompt can be one paragraph: *"You are
stream X. Read START-HERE.md, docs/handoff/STREAMS.md (rules + your brief) and
docs/handoff/streams/X.md, continue from the branch."*

## 3. Coordination rules that prevented collisions

- Disjoint file ownership; cross-stream edits are minimal, called out in the commit
  body, and preferred as "new file + one-line hook".
- Interfaces between streams are written as **contracts** relayed by the lead
  (e.g. the wizard shows the Wi-Fi box only when `phases.h` defines
  `AB_CHOICE_BRING_WIFI`; the engine defines it only when it works end to end).
- The lead relays one stream's findings to the owner of the file (SendMessage),
  never edits their file while they run.
- Shared names that the website, wizard and e2e driver depend on (button texts,
  look names, asset names like `AurOS-Setup.exe`) are fixed by the lead and sent to
  every consumer at once.

## 4. Workflows (fan-out with adversarial checking)

Pattern used every time (scripts in `tools/workflows/`):
**N finders, one per domain → 2-3 skeptics per finding told to REFUTE it →
keep only what survives → one synthesizer writes a doc → one writer pushes.**
- `security-audit.js`: 11 domains → `docs/security/FINDINGS.md`.
- `launch-readiness.js`: 7 dimensions → `docs/LAUNCH-READINESS.md`.
- `review-changes.js`: per-area review of a commit range → confirmed findings to owners.
- `current-state.js`: 6 readers → `docs/handoff/CURRENT-STATE.md`.
Finder stages ran at high effort; skeptics at default; the writer low. Results are
docs in the repo, never only in a transcript.

## 5. CI lessons (each cost time once)

- The container has no KVM and blocks Microsoft/Mozilla/Flathub/Google: **build and
  VM-test on GitHub Actions** (public repo = free minutes; `/dev/kvm` after a udev rule).
- **20 concurrent jobs** on the free plan: a per-push workflow with 5 jobs filled it.
  Use `concurrency` with `cancel-in-progress: false` (queue; newest pending wins) —
  cancelling means nothing finishes when agents push every few minutes. Put the
  concurrency on the *job* when docs-only pushes would otherwise replace a queued build.
- **Every job needs `timeout-minutes`**: a hung job held the windows.yml queue for ~35 minutes. **Bound every `apt-get` too** (`timeout 150 apt-get update`, `timeout 300 apt-get install`, 3 tries; done in windows.yml `e6a888b`): a stalled mirror hung a build step for 20 minutes. build.yml, tests.yml, profiles.yml and othersys.yml still had unbounded apt calls at handoff — fix them.
- Actions **artifacts can't be downloaded** from the container; publish evidence as
  **prerelease assets** (`releases/download/<tag>/<file>` works with curl).
- `workflow_dispatch` only works for workflows on the **default branch** (an old
  branch here) → triggers are path filters and words in the commit subject.
- Releases made with `GITHUB_TOKEN` trigger no other workflow → the release
  workflow deploys Pages itself.
- The e2e Windows VM: cache the installed Windows (Actions cache) so later runs
  start at the install; move the mouse 1 px every 2 min (display timeout); Edge's
  download flyout is a web document — don't filter it out as "the website".

## 6. Container lessons

- Disk is ~25 GB total with ~5-12 GB free: keep scratch < 300 MB per agent; never
  download ISOs locally (6 GB filled the disk once); delete big temporaries.
- **The container can restart and kill every agent.** Files survive; agents can be
  resumed by id; anything only in a running process is lost. Hence "push every
  green step". A later owner may give a new agent ONLY the repository, so the
  repository is the only save that counts.
- The main checkout is shared: agents never edit it; they work in worktrees.

## 7. Owner lessons (how to work with Aaroh)

- He reports plan usage ("92%", "95%", "99%"): that means **save more**, not stop.
  Stopping agents at 92% was a mistake he corrected.
- Ask only decisions that are genuinely his, batched, with a recommendation first
  (AskUserQuestion): the 3 asked this session (OS choice, publishing; theft/Wi-Fi/
  BitLocker) each changed the design. Everything else: decide and say so in one line.
- Sensitive features (reading Wi-Fi keys, suspending BitLocker, SYSTEM tasks) need
  his explicit consent AND visible, consented UX — a safety system halted an agent
  building them silently, and antivirus would flag them too.
- Status messages: what landed, what's running, what he must click; plain words;
  no jargon; one blunt line for each real risk.
- He can't open Settings via the agents: Pages, environments, default branch,
  secrets are his clicks (`docs/OWNER-SETUP.md`).

## 8. Order that worked

1. Read everything; find the newest branch (five branches existed; the newest
   contained all of them).
2. Research + a stringent journey audit (measured, with file:line evidence)
   before deciding anything.
3. Decisions written down (D1-D11) before spawning builders.
4. Parallel streams with disjoint files; the lead keeps RESUME-NOW, STREAMS,
   the session log and START-HERE current after every event.
5. Adversarial workflows over what landed; findings routed to owners.
6. Ship gate: build → prerelease → real-Windows e2e on that exact file → promote.
