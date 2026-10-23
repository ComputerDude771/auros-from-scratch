# Stream E: build, test and release pipeline (lead's snapshot, 2026-10-23 05:40 UTC)

Written by the lead because the stream's own file had not landed when the
session ended. If the release agent later replaced this file, trust its version.

## Goal
Build AurOS on GitHub Actions (the dev container can't: no KVM, blocked
hosts), test it under KVM, and publish it as a GitHub Release whose
`AurOS-Setup.exe` the website's "Get AurOS" button downloads
(`releases/latest/download/AurOS-Setup.exe`). Unsigned by the owner's decision.

## The pieces (read their header comments for the exact trigger words)
- `.github/workflows/build.yml`: on pushes touching `build/ src/ rootfs/ profiles/ themes/ shells/`
  (or a trigger word): forge + mkimage the desktop image WITHOUT the browser
  fallback (Firefox from Mozilla's apt repo), build staging and the installer
  with the release's piece list baked in. First green: run 39893191009
  (forge+mkimage ~5.5 min, job ~15 min, 21 image pieces). The build job queues
  one at a time per branch (`concurrency` on the job, never cancelled).
- `.github/workflows/tests.yml`: KVM simulation suite (installtest, nosticktest,
  firstboottest, choicesboottest, putbacktest, …) + unit tests (probetest,
  shrinktargettest, gapfencetest, storetest, …) on a build's artifacts;
  runs when the commit subject has the test word.
- `.github/workflows/release.yml`: the publish word creates a **prerelease**
  `v<version>-<sha>` (AurOS-Setup.exe, ~90 MiB gz pieces, pieces.txt,
  SHA256SUMS), reads it back, starts it on a real Windows runner. With the
  latest word too, a final `promote` job runs ONLY if an e2e run passed whose
  `e2e.yml` pins THIS exe's SHA-256, then `gh release edit --latest`,
  re-downloads `releases/latest/download/AurOS-Setup.exe` and compares, then a
  `pages` job deploys the website (needs the owner's Pages click).
- Flaky test: re-run the failed jobs of the build run, then push the publish
  word naming that run id (it reads each job's latest attempt).

## State at handoff
- Builds: green at least once; later builds carry every stream's changes.
- **No prerelease with AurOS-Setup.exe existed yet** when the lead stopped
  (only `e2e-*` evidence prereleases). That is the first thing to do.
- Every job should have `timeout-minutes` (a 90-minute hang once blocked the
  windows.yml queue; windows.yml now has them — check build/tests/release).

## Next steps
1. Find the newest green `build` run on the branch (GitHub API or `gh run list`).
2. Push a docs-only commit whose subject carries the publish word (see the
   header of `release.yml`) → prerelease. Note its tag and AurOS-Setup.exe SHA-256.
3. Point `.github/workflows/e2e.yml` `INSTALLER_URL`/`INSTALLER_SHA256` at it,
   push, and watch `releases/download/e2e-<run_id>/live.png` (stream F).
4. When e2e passes: publish + latest words → promote → website deploys.
5. Update `windows.yml` `PUBLISHED_*`/`IMAGE_*` to the promoted release.
