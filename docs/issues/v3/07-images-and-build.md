# 7. Images, build and hosting

## 7.1 No Firefox: the images ship Epiphany
**Severity: medium.** Every profile asks for Firefox. The build network
answers 403 for `packages.mozilla.org`, `ppa.launchpadcontent.net`,
`ftp.mozilla.org` and `download.mozilla.org`, so the images are built
with `ALLOW_BROWSER_FALLBACK=1` and ship Epiphany. It's recorded in
each manifest and in `/etc/auros/build-warnings`.
- Ferry's best "clean win" is the Firefox profile transplant (bookmarks,
  history, tabs, cookies), and it has **nothing to transplant into** on
  these images.
- None of `build/forge`'s three Firefox routes has ever been run.
- **Fix:** build on a network that can reach Mozilla. That's an
  environment setting, not code. Then rebuild without the override and
  check that `/etc/auros/build-warnings` has no browser lines.

## 7.2 All five images need rebuilding
**Severity: high for v3.** Rebuild after the merge in [06](06-first-boot-aurfirst.md):
```
ALLOW_BROWSER_FALLBACK=1 ./build/all desktop office revive school-kiosk multilingual
```
(Drop the override if 7.1 is fixed.) About 9.5 GB peak each, one at a
time, roughly 1.5 h each without KVM. Then run
`tools/firstboottest.sh desktop` and expect `entry_by=partition`. On
the original branch's container only the desktop image survived, and
it was built before the fix.

## 7.3 The image is hosted on a git branch
**Severity: blocker for public release. Fine for tests.** The installer
downloads 20 pieces from `raw.githubusercontent.com`, branch
`image-desktop`.
- It's a git host, not a download service, and there's no promise the
  address will still answer in a year.
- v1 and v2 together add about 3.6 GB to the repository until the branch
  is deleted. A v3 in pieces adds another 1.8 GB.
- During the check on 2026-09-26, 5 of 20 pieces were cut off partway
  through the first download attempt and completed on retry. The
  installer resumes, so this worked out. It was probably this sandbox's
  proxy, not GitHub, but it shows the resume path matters.
- **Fix** (`docs/RELEASE.md` §3): one GitHub Releases asset (1.84 GB, under
  the 2 GiB limit), or Cloudflare R2 / Backblaze B2 behind a CDN. The host
  must answer `Range` requests with 206.

## 7.4 No update channel
**Severity: blocker for release.** `docs/PLAN.md` W6: "certificate, image
host and update channel are not" done. Nothing in the repository updates
an installed AurOS, whether security updates or new releases. A desktop
OS for non-technical people without automatic security updates can't
ship.

## 7.5 Azure Artifact Signing isn't wired into the build
**Severity: medium.** `build/sign` supports a PKCS#11 token and PKCS#12
(through `osslsigncode`). Azure Artifact Signing, the cheapest option in
`docs/RELEASE.md` §2, needs `signtool` with a dlib. That step isn't written.

## 7.6 The manifest isn't checked by the build
The build compiles `src/aurbridge/aurbridge.manifest` without ever
parsing it, which is how [01](01-sxs-manifest.md) happened. It's listed
here too because it's a build fix.
