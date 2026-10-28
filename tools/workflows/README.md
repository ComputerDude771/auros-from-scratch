# Agent workflows (Claude Code "Workflow" tool scripts)

These are the multi-agent orchestration scripts the 2026-10-23 session ran.
They are plain JavaScript for Claude Code's Workflow tool (not GitHub
Actions). A new agent can run one with the Workflow tool:
`Workflow({scriptPath: "tools/workflows/<name>.js", args: ...})`.

Before running, edit the paths at the top of each script: they assume the
repository at `/home/user/auros-from-scratch` and a scratch directory
under `/tmp/claude-0/...scratchpad`; change `REPO` and the scratch paths
to wherever you are. The branch is `claude/brave-hypatia-x72g1m`.

| Script | What it does | Output |
|---|---|---|
| `security-audit.js` | 11 domain auditors (Windows installer, staging, boot chain, kernel/OS, network, privilege paths, our C attack surface, browser/scams/apps, updates/supply chain, theft/data, reliability) → 2 skeptics per finding → prioritised backlog | `docs/security/FINDINGS.md` |
| `launch-readiness.js` | 7 dimensions (data safety, every machine, journey, daily use, commercial/legal, security, test evidence) → 2 skeptics per gap → go/no-go + real-PC test campaign | `docs/LAUNCH-READINESS.md` |
| `review-changes.js` | one reviewer per area over a commit range (`args.base`) → 3 skeptics per finding → confirmed findings sent to the owning agents (`args.owners` = agent ids; with no running agents, change the last stage to write the findings to a file instead) | messages / a findings list |

If `docs/security/FINDINGS.md` or `docs/LAUNCH-READINESS.md` already exist
on the branch, the run finished; read them instead of re-running.
| `current-state.js` | six readers (install path, CI/release/e2e, desktop+security, theft/updates/portability, other OSes/profiles/Ferry, launch/business) → one writer | `docs/handoff/CURRENT-STATE.md` (newest, problems, plan, owner actions) |
