# RESUME NOW

> **If you have only the repository: read `START-HERE.md` (repo root) first.**

> **04:35: every parallel stream, its files and its standing brief are in
> `docs/handoff/STREAMS.md`. Respawn any stream not marked DONE from there.**

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
| A | **fast path** (install engine) | `src/aurbridge/{phases,plat*,main,simmain,report,format,inflate}.*`, `src/aurstage/**`, `build/staging`, install tests, the install-thread glue in `wizard.c`, `docs/AURBRIDGE.md` | FIRST the low-risk legacy-path fixes: staging probe loads the new root's firmware and never requires a network; shrink only what AurOS needs (min(40 GiB, leave Windows max(20 GiB,15%) free), floor 16 GiB) not ntfsresize's minimum; surface-test only that region with progress; hash the image once; `SetThreadExecutionState`; an early-download API the wizard calls when the check passes. THEN D1: Windows shrinks C: itself during the download, the installer writes AurOS raw into the unallocated gap (fenced: only the gap, re-checked against the live layout; Windows refuses writes inside volumes), staging "prepared" mode only verifies + commits GPT + boots; D2 Wi-Fi key carried (ESP file → 0600 NM keyfile, deleted); BitLocker suspended (`-RebootCount 2`), never decrypted; Put Windows back via a Windows startup task that extends C:. New `gapselftest` job in windows.yml on a VHDX. **Rule: the wizard selects the fast path only once it is proven; never ship an unproven destructive path.** | **STOPPED** 03:55 (usage). SHIPS: legacy path fixed — `71b3ff5` probe (firmware via `/lib/firmware`→new root symlink; network recorded not required; probetest 13), `7e874e7` shrink only what AurOS needs + surface-test only that (shrinktargettest 6; big-disk win proven by unit test only — the KVM suite uses a 1 GiB disk), `c47eac1` keep-awake + early download (navtest 62/0). NOT WIRED (safe): `154d409` gap-write fence + Windows online shrink (`plat_gap_*`, `plat_shrink_system`; gapfencetest 6). Hash-once deliberately deferred (would weaken a tested guarantee). LEFT for the fast path, in order (also in `docs/AURBRIDGE.md` "Fast path: status"): gapselftest on real Windows (VHDX job in windows.yml); shared planner + journal `image_on=gap`; engine prepared-mode phases; staging prepared mode + putback task; Wi-Fi carry-over; BitLocker suspend; sim online-shrink + `-FVE-FS-` no-mount test. Until then BitLocker PCs auto-decrypt (slow). Copy fact: AurOS takes up to 40 GiB; Windows keeps max(20 GiB,15%) free. |
| B | **wizard + preflight** | `src/aurbridge/{wizard,preflight,fix*,sbdb}.*`, `docs/shots/wizard/` | ≤3 screens, zero typing: Welcome+automatic check → "How would you like it to look?" (big picture cards → `shell_archetype`/theme in `ab_choice`, reaching `choices.conf`) → Ready (≤80 words, honest time per SSD/HDD, one "Install AurOS" button) → progress (time left, "Restart now" + 60 s auto-restart countdown). Contrast ≥4.5:1, text ≥18 px, targets ≥44 px. Policy D4: ignore empty card readers, eMMC is internal, USB stick = warning only, AC required but never battery %, refuse Intel RST/VMD remap and ARM64 in Windows. VERSIONINFO + icon (unsigned exe identity). Primary button stays teal (#7DD3C0), Enter presses it (the e2e driver relies on both). | `ed5381f` 3-screen flow, no typing, look picker. Mapping (final): Like Windows→taskbar (preselected), Like a Mac→dock, Big and simple→rail, One thing at a time→tiles (tiles.shell's own name); all theme=sandstone (light). Buttons: Next → Next → **Install AurOS** → (progress) → **Restart now** / Not yet (60 s countdown); stop page: Fix these for me / Check again / Close; failure: Try again / Close. Titles e2e uses: "Setting up AurOS", "Ready to restart". Next: D4 policy fixes, VERSIONINFO+icon. Website follows the mapping (`ffc4f6f`). |
| C | **AurOS desktop + software** | `src/aurshell/**`, `build/forge`, `profiles/**`, `rootfs/**`, shell tests | "Get more programs" reachable ≤2 presses on every shell's home; "All my programs" list; inotify rescan so new programs appear at once; Flathub (flatpak + gnome-software plugin + remote at build or first online) + polkit (no password); catalogue ready; unattended-upgrades held off the first hour; style choice applied at first boot; Firefox from Mozilla apt (works on Actions); bloat cut (snapd + Ubuntu's snap firefox were installed despite exclude; datacenter firmware ~170 MB; /var/cache/apt 165 MB; 2nd WebKit). | **DONE** (agent finished 03:50, last `dd39389`; built/booted: NOT yet). Store "Get more programs" on every home (rail row 6 + "All my programs" row 5; tiles last tile p1; taskbar last row of All Programs, open on landing; dock button beside Find; workbench last row); inotify rescan + "X is ready" card; O_NOFOLLOW symlink bug fixed (Flathub apps could never appear); forge: flatpak + plugin + portal, Flathub at build or first-online (`/etc/auros/software-sources`), polkit no-password installs, snapd/snap firefox blocked (build refuses), Mozilla apt with key check, updates wait 1 h, alsa-ucm-conf + SOF firmware; apps follow the light theme. Tests: `tools/storetest.c` 108, `choicestest` 42, all shell tests pass. ~650 MB smaller on disk. **Not done:** .deb → gnome-software (needs a polkit grant forge withholds; gdebi still fails via pkexec), portal env for Flatpak Open/Save dialogs, Wi-Fi keyfile autoconnect unchecked, storetest/choicestest not in tests.yml. **e2e store clicks (1280x800):** Home's "Get more programs" (bag icon) → store (Explore/Installed/Updates) → Search "VLC" → result "VLC" → "Install" (no password) → "Open" or the top-right card "VLC media player is ready – Press here to open it". The wizard preselects Like Windows = **taskbar**: there, All Programs is open on landing and "Get more programs" is its last row. Catalogue may take 1-2 min after first online. |
| D | **website** | `website/**`, `.github/workflows/pages.yml`, `tools/site/` | Front door: one sentence, big "Get AurOS" → `https://github.com/ComputerDude771/auros-from-scratch/releases/latest/download/AurOS-Setup.exe`, platform detection (ARM/Mac/phone → "open this on your Windows computer"), permanent picture steps for the unsigned-file warnings + a Smart App Control path, "pick the look" cards, honest small print + FAQ; old "page is the drive" moved to how-it-works.html; Playwright check `node tools/site/check.mjs`; Pages deploy. | **DONE** (agent finished 03:25): `998057f`..`8b1572a`. `node tools/site/check.mjs` 80/80 (old page: 60 failures). Screens `docs/shots/website/`. **Blocked on the owner**: Pages is off (`has_pages:false`; deploy fails at configure-pages "Not Found"). Owner clicks: (1) Settings → Pages → Source: GitHub Actions; (2) Settings → Environments → github-pages → Deployment branches: add `claude/brave-hypatia-x72g1m` (or No restriction); (3) Actions → pages → re-run. Button 404s until a non-pre release with `AurOS-Setup.exe` is marked latest. Nit: Atkinson Hyperlegible draws slashed zeros ("1Ø minutes"). |
| E | **release pipeline** | `.github/workflows/{build,release,tests}.yml`, `build/sign`, `docs/{SIGNING,RELEASE,HANDOFF-RUNBOOK}.md` | Build the desktop image on Actions without the browser fallback (Firefox), staging, `AurOS-Setup.exe` with the release's piece list baked in; KVM test job runs the simulation suite on fresh artifacts; release job on `[release]` in the commit subject makes prerelease `v<ver>-<sha>` (`[latest]` makes it the latest) with `AurOS-Setup.exe`, ~90 MiB gz pieces, `pieces.txt`, `SHA256SUMS`. Unsigned by design. | `139f536` build+KVM tests+publish; `e626fd2` release workflow, trigger words in subject; `42433ce` docs unsigned |
| F | **real-Windows e2e** | `.github/workflows/e2e.yml`, `tools/e2e/**` | Windows 11 Enterprise eval (fwlink resolved at run time), unattended, OVMF Secure Boot + MS keys, swtpm, KVM; Edge opens the real website (served from the runner at 10.0.2.2), clicks the real download, Keep anyway / Run anyway / UAC Yes, drives the wizard by real input (teal button by colour, boxes by shape, typed word only if asked), follows the restart through staging to AurOS, answers "it works", then (M3) installs a program from "Get more programs" and checks the disk offline. Windows image cached after first good run. | harness landed (`bc26dbe` ... `a550cb2`); run `39891768538` installing Windows at 03:00 |
| G | **phase-2 research** | `docs/research/other-systems-2026-10.md` | Chose route B (D8 in the session log). | **DONE** `96e397c` |
| H | **other OSes P1** | `src/othersys/**`, `tools/othersys/**`, `.github/workflows/othersys.yml` (trigger: its paths or `[othersys]`; evidence in prerelease `othersys-<run_id>`), `docs/OTHER-SYSTEMS.md` | Official Ubuntu 26.04 ISO in a temp partition in the gap → BootNext → ISO's shim → grub loopback → autoinstall formatting only our partitions → post-install restores BootOrder, checks no MOK queued, removes temp partition. Prove in KVM VM on Actions: Ubuntu boots; Windows (and a BitLocker-shaped variant) byte-identical; BootOrder Windows-first; no blue MOK screen; timings. Then Put Windows back, Xubuntu/Cinnamon, integration plan for the wizard picker. | started 03:45 |

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


## 6. 04:00 UTC: FULL SPEED AGAIN (owner has more capacity; the 92% note only meant "save progress")

All streams resumed at 04:00: A (finish + wire the fast path once every piece is green), C (.deb installs, portal env, Wi-Fi keyfile check, theme flip, evince), D (three review fixes: truthful "until Yes" wording, 32-bit Firefox, pages deploy waits for a real release), E (prerelease + promote-after-checks flow + storetest/choicestest in tests.yml + e2e as gate), F (M2, M3, then Windows variants: BitLocker on, small disk, HDD-speed throttle, Fast Startup + pending reboot, Windows 10), B (VERSIONINFO, fresh shots, honest sizes/times, screen-reader basics, plain can't-install page), H (phase 2 Ubuntu P1). Workflows: `tonight-review` resumed (wf_f84f4f83-bfc; finders cached, verifying + routing now) and **`launch-readiness`** (wf_c7f2f6d7-f78): seven dimensions (data safety, every machine, journey, daily use, commercial/legal, security, test evidence) → two skeptics per gap → `docs/LAUNCH-READINESS.md` go/no-go + real-PC test campaign. **Owner's bar: "be 100% sure before people spend money on this" — publishing is not the finish line; LAUNCH-READINESS.md's launch-blockers are.**

### Earlier note (03:40): owner at 92% of plan usage

Stopped to save usage: the phase-2 agent (Stream H; it had read the
Ubuntu installer's behaviour, pushed NOTHING — restart it from its brief
in §2 when usage allows) and the review workflow `tonight-review`
(partial results below). Streams A, B, C were told to land what's green,
document, and stop. Streams E (release) and F (e2e) continue,
economically, toward: a prerelease with `AurOS-Setup.exe`, then the e2e
run on it.

Trigger words changed (release agent, `d433874`/`948fdac`): the build
job queues per job; the tests/release gates skip unless the commit
subject contains `[test]` / `[publish]` (and `[latest]` to make it the
latest). The authoritative list is in `docs/HANDOFF-RUNBOOK.md` and the
header comments of `.github/workflows/{build,tests,release}.yml` —
check there, not here.


### The release flow (release agent, `a3ba7c0`) — follow exactly

1. A green `build` run (any push touching build/src/rootfs/profiles/themes/shells). Its KVM tests run when the subject has `[test]` (see runbook).
2. Push a commit whose subject contains `[publish]` (or `[publish RUN-ID]` to name the build run, e.g. after re-running a flaky job with rerun-failed-jobs) → a **prerelease** tag with `AurOS-Setup.exe`, pieces, `pieces.txt`, `SHA256SUMS`, read-back + real-Windows-runner checks.
3. Point `e2e.yml` env `INSTALLER_URL=https://github.com/ComputerDude771/auros-from-scratch/releases/download/<tag>/AurOS-Setup.exe` and `INSTALLER_SHA256=<that file's sha>`, push → the e2e run must pass (website → install → AurOS → program).
4. Push `[publish RUN-ID] [latest]` → the `promote` job checks a passing e2e run whose e2e.yml pins THIS exe's SHA-256, then `gh release edit --prerelease=false --latest`, then re-downloads `releases/latest/download/AurOS-Setup.exe` and compares. Only then does the website button work.
5. Update `windows.yml` `PUBLISHED_*`/`IMAGE_*` to the promoted release in a follow-up commit.
6. The website deploys from release.yml's `pages` job after `promote` (asked of the release agent 04:05; check it exists). `pages.yml` alone won't re-run after a release: GITHUB_TOKEN-made releases trigger no workflows, and dispatch only works on the default branch. pages.yml skips its deploy while the download 404s (`dd99d82`). Site check: 96/96.

### First 30 minutes for a fresh session

1. `git fetch origin claude/brave-hypatia-x72g1m`; read §0-§4 above and the
   newest lines of the Progress log in `SESSION-2026-10-23.md`.
2. List Actions runs on the branch (queued ones first — the free plan's 20
   job slots fill up). Find the newest green `build` and whether a
   prerelease exists (`mcp__github__list_releases`).
3. If no prerelease: push an empty-ish docs commit whose subject carries
   the publish word (see runbook). If one exists: point `e2e.yml`
   (`INSTALLER_URL`, `INSTALLER_SHA256`) at it and push; watch
   `releases/download/e2e-<RUNID>/live.png`.
4. Work the "unverified review findings" list below: verify each (1 min),
   fix the real ones (owners' files are now free — the agents stopped).
5. When e2e passes website → AurOS → program installed: publish as
   latest, update `windows.yml` `PUBLISHED_*`/`IMAGE_*`, confirm the
   website button resolves, tell the owner the two links (§4.7).

### Unverified review findings (from `tonight-review`, stopped before its 3-skeptic check)

Verify each before fixing; some may already be fixed by later commits.

| Sev | Area | Where | Finding → fix |
|---|---|---|---|
| ~~blocker~~ FIXED | shell | src/aurshell/apps.c:404 | rail Home shows 6; store ranked 7th → rank PackageManager right after WebBrowser (storetest `a2b5dba` may already cover this) |
| ~~high~~ FIXED | shell | src/aurshell/apps.c:124 | `open_entry()` O_NOFOLLOW rejects flatpak's symlinked .desktop files → Flathub apps never appear; drop O_NOFOLLOW |
| ~~high~~ FIXED | shell | src/aurshell/main.c:790 | app list read once → new programs invisible until reboot; inotify rescan |
| high | shell | build/forge:1623 / :476 | gdebi re-execs via pkexec without WAYLAND_DISPLAY → .deb install fails; map .deb to gnome-software-local-file-packagekit |
| ~~high~~ FIXED | shell | profiles/desktop.profile:112 | no `alsa-ucm-conf alsa-topology-conf` → no sound on most Intel SOF laptops |
| medium | shell | src/aurshell/main.c:1344 | portal backend lacks WAYLAND_DISPLAY → Flatpak Open/Save dialogs fail; dbus-update-activation-environment --systemd ... |
| high | fastpath | build/staging:515 | preflight now accepts eMMC (D4) but staging has no MMC driver → add `kernel/drivers/mmc` to MOD_DIRS, load mmc_block |
| medium | fastpath | src/aurstage/probe.c:337 | firmware path dangles after unchecked umount → settle 3 s before umount, retry EBUSY, reset firmware_class.path |
| medium | e2e/ci | .github/workflows/tests.yml:151 | shrink-to-target + probe change not exercised in CI → add shrinktargettest/probetest steps and a ≥40 GiB sparse-NTFS installtest case |
| high | release | .github/workflows/release.yml:193 | `[publish][latest]` can't promote an existing prerelease → if tag exists, verify exe and `gh release edit --prerelease=false --latest` |
| high | release | .github/workflows/release.yml:192 | `[latest]` goes public before read-back/real-Windows checks → always create as prerelease; a final `promote` job after checks |
| medium | release | .github/workflows/release.yml:85 | a flaky test can't be cured by `[test]` → use rerun-failed-jobs on the build run |
| medium | release | .github/workflows/build.yml:206 | warns "browser problem" on every good Firefox build → only warn on `browser=MISSING` or `(fallback;` |
| high | website | website/index.html:287,360 | promises "until Yes, switching on starts Windows" but auros-hold re-arms BootNext → reword: Windows kept as it was; if AurOS fails to start, Windows starts; "No" brings Windows back |
| high | website | .github/workflows/pages.yml:56 | deploys while the only download link 404s → add a check that `releases/latest/download/AurOS-Setup.exe` resolves |
| medium | website | website/assets/door.js:94 | 32-bit Windows on Firefox gets the x64 button → if no UA-CH and no Win64/WOW64/x64/ARM64 in UA, show "this computer can't" |

## 5. Traps already hit tonight

- Pushing races: always `git fetch && git rebase origin/claude/brave-hypatia-x72g1m` before `git push origin HEAD:refs/heads/claude/brave-hypatia-x72g1m`; retry.
- Never edit `build/*` in a checkout while a build runs from it (bash reads scripts as it goes).
- `docs/research/every-machine-2026-10.md` and the audit were committed in `a100d30`; don't re-add.
- The e2e workflow queues (concurrency, no cancel); only the newest pending run survives.
- The older `docs/handoff/TRAPS.md` still applies (prove checks fail; `[ -e link ]`; `pkill -f`; OVMF quirks).
