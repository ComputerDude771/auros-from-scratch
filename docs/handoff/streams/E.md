# Stream E: build, test and publish AurOS on GitHub Actions

Owner of: `.github/workflows/{build,tests,release}.yml`, `build/sign`,
`docs/{SIGNING,RELEASE,HANDOFF-RUNBOOK}.md`, this file.
Updated 2026-10-23 04:20 UTC.

## Goal

Every change is built on GitHub's `ubuntu-24.04` runners (KVM, open
network; the development container has neither), tested under KVM, and
published as a GitHub Release that `AurOS-Setup.exe` downloads its image
from. The website's button is
`https://github.com/ComputerDude771/auros-from-scratch/releases/latest/download/AurOS-Setup.exe`,
which only a **full** (non-pre) release moves.

## Trigger words: in the commit SUBJECT only (first line)

| Subject contains | What runs |
|---|---|
| `[build]`, or any change under `build/ src/ rootfs/ profiles/ themes/ shells/` or to `build.yml`/`tests.yml` | `build.yml`: build, then `tests.yml` |
| `[test]` | `tests.yml` alone, against the newest build of the branch with artifacts (14 days) |
| `[release]` | build + tests + prerelease `v<os_version>-<sha7>` if all passed |
| `[publish]` or `[publish RUN-ID]` | `release.yml` alone: prerelease of the newest build run whose build job and every test passed (or that run); nothing rebuilt |
| add `[latest]` to either | after the read-back and Windows checks, `promote` makes it the full release (only if a passing `e2e.yml` run pinned that AurOS-Setup.exe's SHA-256), then `pages` deploys `website/` |

Empty commits are fine: `git commit --allow-empty -m "[publish 39895845920] ..."`.
A body that mentions the words does nothing (the first commit's body did,
and nearly published a full release; that is why).

## Done and pushed

- `build.yml` (gate → build → tests → release). Build job 9-15 min:
  `build/all desktop --keep=raw` **without** `ALLOW_BROWSER_FALLBACK`
  (Firefox arrives from Mozilla's apt repo: build-warnings say
  `browser=firefox (mozilla-apt)`, `flathub=build (...)`), `build/staging`,
  `pigz -9` + `split` into 90 MiB pieces, `pieces.txt` with
  `base https://github.com/ComputerDude771/auros-from-scratch/releases/download/<tag>/`,
  `AUROS_RELEASE=1 build/aurbridge`, checks of the `.exe` (resources 1-5,
  baked address, shim key, manifesttest), and `aurbridge-sim getimage`
  fetching every piece over HTTP and checking the image. debootstrap is
  cached (release pocket only, safe). Artifacts: `auros-installer`
  (AurOS-Setup.exe, aurbridge.exe, aurbridge-sim, staging files,
  `staging-work.tar.zst`, pieces.txt, hashes, build-warnings, timings,
  build-info.txt), `auros-pieces`, `auros-rootfs`.
- `tests.yml`: `unit` (aurfirsttest, choicestest with the image's own
  aurshell, nmtest, sessionenvtest, looktest, storetest,
  shrinktargettest, probetest, nvramtest, bridgetest, manifesttest,
  Ferry, sim selftests, `aurbridge.exe selftest` under Wine, signtest) and
  `e2e` matrix (installtest, nosticktest, firstboottest,
  choicesboottest, putbacktest), max-parallel 2. A `qemu-system-x86_64`
  wrapper in `/usr/local/bin` turns the scripts' `accel=tcg` into
  `accel=kvm` (or `$AUROS_QEMU_ACCEL`); no test script is edited.
- `release.yml`: `which` → `publish` (always a prerelease; re-reads every
  file through GitHub's 302, a Range request must be a 206 from the right
  byte) → `windows` (downloads AurOS-Setup.exe, `--navtest`, starts it;
  `aurbridge.exe getimage` fetches the image from the release with
  WinHTTP) → `promote` (`[latest]` only, e2e-gated) → `pages`.
- Unsigned by the owner's decision: `build/aurbridge` no longer refuses
  an unsigned `AUROS_RELEASE=1` build; `tools/signtest.sh` 13/13;
  `docs/SIGNING.md` has the clicks she sees.
- **First fully green build: run 39895845920** (commit `dd39389`):
  https://github.com/ComputerDude771/auros-from-scratch/actions/runs/39895845920
  build 529 s; unit green; installtest 399 s, nosticktest 1338 s,
  firstboottest 430 s, putbacktest 170 s, choicesboottest 208 s.
  Image 4,855,955,456 bytes, SHA-256 `9e3e7b9d461f2bbd1095f5cb8a82a5b07f77cdef449145fca65c51d0b7acce52`;
  download 1,477,945,274 bytes in 16 pieces;
  AurOS-Setup.exe 35,001,391 bytes, SHA-256 `89b49e4bb7538abf56fee31399b4f71f74d1fa5f245484bed9e317e1a8a02a3d`.

## Half-done (check first)

- Prerelease `v0.1.0-dd39389` of run 39895845920: the first `[publish]`
  (release run 39899061212) failed with HTTP 403 creating the release
  (a GITHUB_TOKEN may tag only its own run's commit). Fixed in `89b79c7`,
  which also re-requests the publish. Check
  `https://github.com/ComputerDude771/auros-from-scratch/releases/tag/v0.1.0-dd39389`
  and the newest `release.yml` run. If it failed again, read its
  `publish` job log tail.

## Next steps

1. Confirm the prerelease exists with 19 assets (AurOS-Setup.exe,
   pieces.txt, SHA256SUMS, 16 pieces) and that `release / windows` passed.
2. Ask stream F (e2e) to pin `INSTALLER_URL` to
   `https://github.com/ComputerDude771/auros-from-scratch/releases/download/v0.1.0-dd39389/AurOS-Setup.exe`
   and `INSTALLER_SHA256` to its hash in `e2e.yml`; when that run's `e2e`
   job passes, push `[publish 39895845920] [latest]`.
3. Pages needs the owner's click once: Settings → Pages → Source: GitHub
   Actions. The `pages` job says so if not.
4. Tell whoever owns `windows.yml` the new `PUBLISHED_URL`/`PUBLISHED_SHA256`
   and `IMAGE_PIECES_URL`/`IMAGE_SHA256`/`IMAGE_BYTES` (above) once the
   release is the one people get.
5. Optional: an `apt-cacher-ng` cache for the chroot's apt (forge cleans
   its archive, so actions/cache cannot hold it); downloads are a minute
   of the build today, so it is low value.

## Traps

- Unnamed `run:` shell is `bash -e` **without pipefail**; every workflow
  sets `defaults.run.shell: bash`. Then `strings | grep -q`, `tail | head`
  and similar pipes fail on SIGPIPE: read files instead.
- Concurrency belongs on the **build job**, not the workflow: at workflow
  level a docs-only push replaces a queued build (GitHub keeps one
  pending run per group). A job whose `if` is false never joins a group.
- `workflow_dispatch` and `workflow_run` only work for workflows on the
  default branch (`claude/linux-distro-from-scratch-tt5dx1`): hence the
  subject words.
- A called workflow cannot have more permissions than its caller job;
  build.yml's `release` job grants what release.yml's jobs ask for
  (contents, actions, pages, id-token).
- Artifacts cannot be downloaded into the development container (blob
  storage 403). Read job logs (`get_job_logs` tail; each job ends with a
  summary) or publish to a release.
- The free plan runs 20 jobs at once for the whole repository; every
  job has a timeout so a hang cannot hold a queue.
- `tools/machine.sh` needs `work/staging/lib/modules` as well as the
  rootfs; tests.yml restores it from `staging-work.tar.zst` (or rebuilds
  it for older artifacts).

## How to verify

```sh
curl -s https://api.github.com/repos/ComputerDude771/auros-from-scratch/actions/workflows/build.yml/runs?per_page=5
curl -s https://api.github.com/repos/ComputerDude771/auros-from-scratch/releases/tags/v0.1.0-dd39389
curl -sSL -o x.exe https://github.com/ComputerDude771/auros-from-scratch/releases/download/v0.1.0-dd39389/AurOS-Setup.exe && sha256sum x.exe
```
