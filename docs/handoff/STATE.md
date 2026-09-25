# State: what is proven, and what is not

These numbers are for branch `claude/linux-distro-from-scratch-tt5dx1`.
The active branch (`claude/laughing-cray-ayao6i`) has its own, newer
results in its `docs/results/`. Every check below was run on this
branch's tree; raw transcripts are in `docs/results/`.

## Proven on simulated machines (QEMU + OVMF UEFI firmware)

| test | checks | what it proves |
|---|---|---|
| `tools/installtest.sh` | 37 | Installs for real on a synthetic Windows machine (real NTFS, 7 files with known md5s), starts AurOS, puts Windows back twice (from the stick, then from the copy on the computer), refuses a damaged image, a damaged boot chain and a stick for another profile, and keeps every Windows file identical. |
| `tools/loadertest.sh` | 18 | With the stick out and no `-kernel`, firmware alone starts the installed AurOS, including **under Secure Boot with Microsoft's keys**. |
| `tools/powercuttest.sh` | 138 | Power cut at each of 17 named instants of install and restore; Windows always comes back with every file intact. |
| `tools/matrixtest.sh` | 10 | 512-byte and 4Kn disks, a 1 GiB OEM ESP, first partition at LBA 34 are accepted; BitLocker, hibernated Windows, MBR, legacy BIOS, two Windows drives and no room are refused. |
| `tools/imagetest.sh` | 23 | The image is found by its content, including the right one of two on a stick; a flipped byte is caught; an interrupted write leaves nothing mountable. |
| `tools/bridgetest.sh` | 13 | The Windows half and the staging half agree byte-for-byte on the journal, cpio, gzip and GPT. |
| `tools/aurfirsttest.sh` | 54 | `BootOrder` is untouched until someone says AurOS works; nothing is ever dropped from it; AurOS picks its own entry out of several called "AurOS". |
| `tools/nvramtest.sh` | 25 | The installer's boot entry decodes correctly, takes a free slot, and cannot touch `BootOrder`. |
| `tools/wrtest.sh` | 23 | Disk writes land only where they were armed; the build refuses any other file that could write a disk. |
| `tools/exetest.sh` | 17 | One `.exe` carries the staging environment; a download resumes after a cut connection, including when the server ignores `Range`. |
| `tools/signtest.sh` | 12 | Signing works, a changed byte breaks the signature, and a release build refuses to ship unsigned. |
| `tools/welcometest.c` | 27 | The "does this work?" panel appears only while the question is open, and each button asks for exactly one word. |
| `tools/modaltest.c` | 53 | Panels are modal and can always be left without a mouse. |
| `tools/targets.c` | 10021 | Everything clickable is at least 44 px, at every text size. |

## The real desktop image, booted: `tools/firstboottest.sh`

This test boots `out/auros-desktop.img.zst` itself (not a synthetic
root) under OVMF with NVRAM that persists between boots, set up the way
an install leaves it. On the image built *before* the `aurfirst` fix it
passed **29 of 30**:

- AurOS starts from its own firmware entry via `BootNext`.
- `auros-hold` re-arms `BootNext` under real systemd against real
  efivarfs.
- "It works" makes root put AurOS first in `BootOrder` and keep Windows
  in it.
- "Go back to Windows" leaves Windows first, and the next power-on goes
  to Windows.

The one failure was expected: that `aurfirst` has no `entry_by` field.

**Case C of that test does not yet test anything**; see OPEN.md item 2.

## Not proven

- **Any real PC.** Every result above is QEMU/OVMF.
- The **staging environment under Secure Boot with no `-kernel`**. It
  was broken on this branch (the installer kernel is Canonical-signed
  and was booted directly by firmware). The active branch reports a
  fix. Verify it.
- **Ferry** (moving files from Windows) against a real user's data.
- The five images **with** the `aurfirst` fix. None has been built yet.

## Artifacts: not in git

`out/` and `work/` are build output and git-ignored. Anything built in a
cloud container disappears when the container is reclaimed.

- On this branch's container at handoff: only `out/auros-desktop.img.zst`
  (built before the `aurfirst` fix) plus the staging images and
  `.exe`s. The other four profile images were deleted and not rebuilt.
- Downloadable: branch `image-desktop` (test installer plus the desktop
  image in pieces, with SHA-256s). See BRANCHES.md.

## How to build and test

```
./build/staging                          # staging environment (needs work/forge/desktop/rootfs)
AURSTAGE_FAULT=1 ./build/staging         # fault-injection build, for powercuttest
./build/aurbridge                        # the Windows .exe files (mingw)
ALLOW_BROWSER_FALLBACK=1 ./build/all desktop office revive school-kiosk multilingual
                                         # images, one at a time, ~1.5 h each, ~9.5 GB peak
sudo sh tools/installtest.sh             # and the other tools/*.sh; see tools/README.md
```

`build/all` keeps `work/forge/desktop/rootfs` by default.
`build/staging` and every end-to-end test take tools out of it, so do
not delete it.
