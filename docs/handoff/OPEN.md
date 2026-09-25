# What is left, in priority order

## 1. Merge `claude/linux-distro-from-scratch-tt5dx1` into the active branch

It holds the `aurfirst` fix (`2d85777`) and the first-boot test, which
the active branch lacks. A dry run found no conflicts. Do this before
building any image. Afterwards, re-run `tools/aurfirsttest.sh` (expect
54) on the merged tree.

## 2. Fix `tools/firstboottest.sh` case C

Case C plants a second firmware entry called "AurOS" with a lower
number, to check that the booted AurOS picks its own entry. On OVMF the
planted entry **does not survive the boot**: it is gone and its number
reused for one of OVMF's own entries.

- An entry for a partition that doesn't exist is replaced.
- So is one for a real `AUROS-BOOT` on a second attached disk with a
  file at the path.

The case now checks for this first and FAILS, so it cannot pass by
accident. Nobody has yet worked out *why* OVMF does this.

- A one-minute experiment is to boot OVMF with no OS and two disks, then
  read the variable store with `tools/efivarstore.py`.
- A starting point is OVMF's platform boot manager and
  `QemuBootOrderLib`, which may rewrite options.
- Until case C works, `tools/aurfirsttest.sh`'s eight "two entries
  called AurOS" checks are the proof. Seven of them fail under the old
  rule.

## 3. Rebuild all five images, after the merge

```
ALLOW_BROWSER_FALLBACK=1 ./build/all desktop office revive school-kiosk multilingual
```

- **Disk:** each profile peaks at about 9.5 GB, one at a time.
- **Browser:** Firefox cannot be fetched from this environment (see
  item 7), so the images ship Epiphany. `ALLOW_BROWSER_FALLBACK=1` is
  the deliberate override; it is recorded in each manifest and in
  `/etc/auros/build-warnings`.
- **Check:** stage 6 refuses to seal a rootfs without phases 9 and 10
  (`aurfirst`, `answer.sh`, the units and their enable links).
- **Then:** run `tools/firstboottest.sh desktop` on the new image and
  expect everything green, including `entry_by=partition`.

## 4. The first real PC

This is the main remaining risk. The active branch has `docs/TRY-IT.md`,
and branch `image-desktop` has a downloadable test installer.

- Start with the zero-risk step: `aurbridge.exe` run as administrator,
  which only reports what would block.
- Then install on a PC whose data doesn't matter.
- Keep the USB stick if one is used; it holds the installer's record.
- Send back photos of every screen.

## 5. Verify the Secure Boot fix on the active branch

The active branch reports that the installer now starts through shim
and grub and that preflight checks the firmware trusts shim's key. Two
things still need checking:

- The staging environment itself boots on Microsoft-keys OVMF with
  Secure Boot enforced and no `-kernel`.
- It still works under kernel lockdown: raw disk writes, efivarfs
  writes, and module loading.

## 6. Non-code blockers to a public release

- **Code-signing certificate.** Unsigned `.exe` files trigger
  SmartScreen. See `docs/SIGNING.md`.
- **Hosting for the image.** Branch `image-desktop` is a stopgap;
  `AUROS_IMAGE_URL` must be set for a release build.
- **A legal entity, EULA and support path.** This repartitions
  strangers' disks.

## 7. Known, accepted, documented

- **No Firefox** from this build environment. The network policy
  answers 403 for `packages.mozilla.org`, `ppa.launchpadcontent.net`,
  `ftp.mozilla.org` and `download.mozilla.org`. The fix is the
  environment's network policy, not code.
- **"Put Windows back" leaves a dead "AurOS" entry** in the firmware
  menu. This is deliberate: the restore path writes nothing to NVRAM.
  It's harmless now that `aurfirst` picks its own entry. See
  `docs/AURBRIDGE.md`, "Which AurOS is this AurOS".
- **The wizard has no profile picker yet.** It writes the constant
  `desktop`. The journal carries the profile, ready for a picker.
- **Ferry's migration breadth is thin.** It is tested against fixtures,
  never a real user's data.
