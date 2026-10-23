# RESUME NOW — if you are a new agent taking over, start here

Kept current by the lead agent of session 2026-10-23 (it may run out of
credits mid-flight). Read this file top to bottom (10 minutes), then
`docs/handoff/SESSION-2026-10-23.md` (decisions D1-D7 and the progress
log). Everything else is reference.

Last updated: see `git log -1 -- docs/handoff/RESUME-NOW.md`.

---

## 0. The owner's orders (verbatim intent; do not relitigate)

1. **A working version TONIGHT; at the latest tomorrow morning.** Working =
   a great-grandmother opens the website, downloads one file, and in under
   10 minutes is using AurOS and can install a program. On "every machine"
   (every Windows 10/11 x64 UEFI PC we can make safe; refusals must be rare
   and explained in plain words).
2. **No code signing, no keys, no Microsoft services.** Unsigned installer
   by design. Website teaches the clicks (Keep anyway / More info / Run
   anyway / Yes; Smart App Control PCs: switch SAC off, one-way, said
   plainly). Never add signing back.
3. **"An OS she wants" = both:** phase 1 AurOS where she picks the style
   (Like Windows / Like a Mac / Big and simple / One thing at a time);
   phase 2 other OSes (Ubuntu, Mint, ...). Phase 1 ships first.
4. **Publish: yes.** GitHub Release (installer + image) + GitHub Pages.
   The owner must click: Settings → Pages → Source: "GitHub Actions", and
   Settings → Environments → github-pages → allow branch
   `claude/brave-hypatia-x72g1m` (the token can't). Then re-run `pages`.
5. Test end to end in a VM: website → download → install → AurOS → install
   a program. (Done on GitHub Actions with KVM; see §3.)
6. Push to `claude/brave-hypatia-x72g1m` only. No PRs. Commits end with
   the two trailer lines the harness gives you (Co-Authored-By /
   Claude-Session). No model names in files.
7. Use many agents/workflows; credits are fine; don't waste them.
8. Write progress down constantly (this file + the session log).

## 1. Where things are (state at last update)

Branch `claude/brave-hypatia-x72g1m` contains ALL earlier work (it was
fast-forwarded to `claude/admiring-shannon-xm1g0e`, the newest of five
older branches). Older handoffs (`HANDOFF.md`, `docs/handoff/*.md`,
`docs/issues/v3/STATUS.md`) describe the system before tonight.

The local container has no KVM, 4 CPUs, ~23 GB free disk, and a network
that blocks Microsoft, Mozilla, Flathub, Google, Debian, Fedora hosts.
**Anything needing those runs on GitHub Actions** (public repo, free
minutes, KVM after a udev rule). GitHub MCP tools (load with ToolSearch:
`select:mcp__github__actions_list,mcp__github__actions_get,mcp__github__get_job_logs,mcp__github__list_releases`)
read runs/logs. Actions *artifact* downloads are blocked from the
container; **GitHub Releases downloads work** (`curl -sSL
https://github.com/ComputerDude771/auros-from-scratch/releases/download/TAG/FILE`),
so evidence is published as (pre)release assets.

A local desktop image was built at 02:54 in the main checkout
(`/home/user/auros-from-scratch/out/auros-desktop.img`,
`work/forge/desktop/rootfs`, with `ALLOW_BROWSER_FALLBACK=1`, so no
Firefox). It exists only in this container.

## 2. Workstreams (one background agent each, own git worktree under the scratchpad)

Worktrees live in `/tmp/claude-0/.../scratchpad/wt-*` and DIE with the
container. A new agent should not look for them; work from the branch.

| # | Stream | Owns (files) | Brief, in one paragraph | Landed so far |
|---|---|---|---|---|
| A | **fast path** (install engine) | `src/aurbridge/{phases,plat*,main,simmain,report,format,inflate}.*`, `src/aurstage/**`, `build/staging`, install tests, the install-thread glue in `wizard.c`, `docs/AURBRIDGE.md` | FIRST the low-risk legacy-path fixes: staging probe loads the new root's firmware and never requires a network; shrink only what AurOS needs (min(40 GiB, leave Windows max(20 GiB,15%) free), floor 16 GiB) not ntfsresize's minimum; surface-test only that region with progress; hash the image once; `SetThreadExecutionState`; an early-download API the wizard calls when the check passes. THEN D1: Windows shrinks C: itself during the download, the installer writes AurOS raw into the unallocated gap (fenced: only the gap, re-checked against the live layout; Windows refuses writes inside volumes), staging "prepared" mode only verifies + commits GPT + boots; D2 Wi-Fi key carried (ESP file → 0600 NM keyfile, deleted); BitLocker suspended (`-RebootCount 2`), never decrypted; Put Windows back via a Windows startup task that extends C:. New `gapselftest` job in windows.yml on a VHDX. **Rule: the wizard selects the fast path only once it is proven; never ship an unproven destructive path.** | `71b3ff5` probe: network recorded, never required; new root's firmware loaded |
| B | **wizard + preflight** | `src/aurbridge/{wizard,preflight,fix*,sbdb}.*`, `docs/shots/wizard/` | ≤3 screens, zero typing: Welcome+automatic check → "How would you like it to look?" (big picture cards → `shell_archetype`/theme in `ab_choice`, reaching `choices.conf`) → Ready (≤80 words, honest time per SSD/HDD, one "Install AurOS" button) → progress (time left, "Restart now" + 60 s auto-restart countdown). Contrast ≥4.5:1, text ≥18 px, targets ≥44 px. Policy D4: ignore empty card readers, eMMC is internal, USB stick = warning only, AC required but never battery %, refuse Intel RST/VMD remap and ARM64 in Windows. VERSIONINFO + icon (unsigned exe identity). Primary button stays teal (#7DD3C0), Enter presses it (the e2e driver relies on both). | (nothing yet at last update) |
| C | **AurOS desktop + software** | `src/aurshell/**`, `build/forge`, `profiles/**`, `rootfs/**`, shell tests | "Get more programs" reachable ≤2 presses on every shell's home; "All my programs" list; inotify rescan so new programs appear at once; Flathub (flatpak + gnome-software plugin + remote at build or first online) + polkit (no password); catalogue ready; unattended-upgrades held off the first hour; style choice applied at first boot; Firefox from Mozilla apt (works on Actions); bloat cut (snapd + Ubuntu's snap firefox were installed despite exclude; datacenter firmware ~170 MB; /var/cache/apt 165 MB; 2nd WebKit). | `cc60b77` forge: Flathub, no snap, smaller image; `9ebd6af` XDG_DATA_DIRS |
| D | **website** | `website/**`, `.github/workflows/pages.yml`, `tools/site/` | Front door: one sentence, big "Get AurOS" → `https://github.com/ComputerDude771/auros-from-scratch/releases/latest/download/AurOS-Setup.exe`, platform detection (ARM/Mac/phone → "open this on your Windows computer"), permanent picture steps for the unsigned-file warnings + a Smart App Control path, "pick the look" cards, honest small print + FAQ; old "page is the drive" moved to how-it-works.html; Playwright check `node tools/site/check.mjs`; Pages deploy. | **DONE** (agent finished 03:25): `998057f`..`8b1572a`. `node tools/site/check.mjs` 80/80 (old page: 60 failures). Screens `docs/shots/website/`. **Blocked on the owner**: Pages is off (`has_pages:false`; deploy fails at configure-pages "Not Found"). Owner clicks: (1) Settings → Pages → Source: GitHub Actions; (2) Settings → Environments → github-pages → Deployment branches: add `claude/brave-hypatia-x72g1m` (or No restriction); (3) Actions → pages → re-run. Button 404s until a non-pre release with `AurOS-Setup.exe` is marked latest. Nit: Atkinson Hyperlegible draws slashed zeros ("1Ø minutes"). |
| E | **release pipeline** | `.github/workflows/{build,release,tests}.yml`, `build/sign`, `docs/{SIGNING,RELEASE,HANDOFF-RUNBOOK}.md` | Build the desktop image on Actions without the browser fallback (Firefox), staging, `AurOS-Setup.exe` with the release's piece list baked in; KVM test job runs the simulation suite on fresh artifacts; release job on `[release]` in the commit subject makes prerelease `v<ver>-<sha>` (`[latest]` makes it the latest) with `AurOS-Setup.exe`, ~90 MiB gz pieces, `pieces.txt`, `SHA256SUMS`. Unsigned by design. | `139f536` build+KVM tests+publish; `e626fd2` release workflow, trigger words in subject; `42433ce` docs unsigned |
| F | **real-Windows e2e** | `.github/workflows/e2e.yml`, `tools/e2e/**` | Windows 11 Enterprise eval (fwlink resolved at run time), unattended, OVMF Secure Boot + MS keys, swtpm, KVM; Edge opens the real website (served from the runner at 10.0.2.2), clicks the real download, Keep anyway / Run anyway / UAC Yes, drives the wizard by real input (teal button by colour, boxes by shape, typed word only if asked), follows the restart through staging to AurOS, answers "it works", then (M3) installs a program from "Get more programs" and checks the disk offline. Windows image cached after first good run. | harness landed (`bc26dbe` ... `a550cb2`); run `39891768538` installing Windows at 03:00 |
| G | **phase-2 research** | writes `docs/research/other-systems-2026-10.md` (uncommitted until the lead commits) | Route A (rebuild flavours with forge) vs Route B (official ISOs installed unattended); trademarks; Secure Boot; recommendation. | running |

## 3. CI: what runs, how to trigger, where the evidence is

| Workflow | Trigger | What it proves |
|---|---|---|
| `windows.yml` | every push to `claude/**` | the installer starts on a real Windows Server VM; preflight read-only; navtest; Fast Startup switch; space fix; WinHTTP download of the published image pieces (pinned env `PUBLISHED_*`, `IMAGE_*` — update when a new release is the one people get) |
| `build.yml` | push touching build/src/rootfs/profiles/themes/shells, or `[build]` in the commit subject | image + staging + exe built on Actions; KVM simulation suite |
| `release.yml` | `[release]` in the commit subject (`[latest]` too → latest) | publishes the GitHub (pre)release |
| `e2e.yml` | push touching `tools/e2e/**` or `e2e.yml`, or `[e2e]` in the subject | the whole journey on real Windows 11 in a VM. Point it at a build by editing `INSTALLER_URL` + `INSTALLER_SHA256` in `e2e.yml` env and pushing. Evidence: `https://github.com/ComputerDude771/auros-from-scratch/releases/download/e2e-<RUNID>/e2e-evidence.zip`, live `live.png`/`live.txt` every 3 min while running |
| `pages.yml` | push touching `website/**` | deploys the site (needs the owner's one click) |

`workflow_dispatch` cannot be used (only works on the default branch,
which is `claude/linux-distro-from-scratch-tt5dx1`).

## 4. The finish line, in order (what "done tonight" means)

1. Stream A legacy fixes + Stream B wizard + Stream C forge/shell landed and
   `windows.yml` green.
2. `build.yml` green on the branch head (image with Firefox + Flathub).
3. A **prerelease** from that build (`[release]`).
4. `e2e.yml` pointed at the prerelease's `AurOS-Setup.exe`: website →
   download → install → AurOS "does it work?" → yes → install a program →
   it opens. Look at every screenshot yourself.
5. Fix what the e2e finds; repeat 2-4.
6. Fast path (D1) proven (gapselftest on real Windows + simulation + e2e)
   → wizard selects it → repeat 2-4 and measure the wall clock.
7. Release `[release][latest]`; update `windows.yml` pins; website live;
   tell the owner the link: `https://computerdude771.github.io/auros-from-scratch/`
   and the installer link
   `https://github.com/ComputerDude771/auros-from-scratch/releases/latest/download/AurOS-Setup.exe`.
8. Phase 2 (other OSes) per the research recommendation.

## 4b. Review workflow

At 03:20 the lead launched workflow `tonight-review` (run `wf_f84f4f83-bfc`): one reviewer per area (forge-shell, staging, ci, website, e2e, integration) over `c358a32..HEAD`, three skeptics per finding, confirmed findings SendMessage'd to the owning agent. Script: `/root/.claude/projects/-home-user-auros-from-scratch/0f9a7adc-af00-5b1e-9166-5e932ada07e6/workflows/scripts/tonight-review-wf_f84f4f83-bfc.js` (container-local). Re-run it after each big landing with a new `base`.

## 5. Traps already hit tonight

- Pushing races: always `git fetch && git rebase origin/claude/brave-hypatia-x72g1m` before `git push origin HEAD:refs/heads/claude/brave-hypatia-x72g1m`; retry.
- Never edit `build/*` in a checkout while a build runs from it (bash reads scripts as it goes).
- `docs/research/every-machine-2026-10.md` and the audit were committed in `a100d30`; don't re-add.
- The e2e workflow queues (concurrency, no cancel); only the newest pending run survives.
- The older `docs/handoff/TRAPS.md` still applies (prove checks fail; `[ -e link ]`; `pkill -f`; OVMF quirks).
