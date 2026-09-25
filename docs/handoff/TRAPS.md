# Traps: things that already went wrong here

Every one of these happened on this project and cost time. Most look
reasonable until they bite.

## Testing

- **Never rebuild anything a running test reads.** Twice, `./build/staging`
  ran while `installtest` was reading `out/auros-staging.img`. The
  failures that produced were then blamed on real bugs, and later a real
  bug was blamed on the race. Freeze the tree, and checksum the
  artifacts before and after the run.
- **Prove a check fails before believing it passes.** Break the thing it
  checks (revert the fix, stub the function) and watch it go red. Checks
  in this project have passed while testing nothing:
  - a `sed` that matched nothing
  - a loop that printed "ok" unconditionally
  - a fixture that firmware quietly removed
  - a file that pre-dated the build
- **A test that passes because the environment tidied up is not a
  test.** OVMF deletes or replaces some planted boot entries.
  `firstboottest` case C now checks its own fixture first for exactly
  this reason.
- **Check the failure path too.** Check what a test does when the
  fixture is wrong, the tool is missing, or the read fails. `2>/dev/null
  || ok` has turned "could not read" into "passed" here more than once.
- **Warnings need a check.** The second copy of the "way back" was not
  written to any machine for a whole day. The install only printed a
  warning, since that step is deliberately non-fatal. Nothing read the
  warning until a restore case six checks later failed with a
  misleading message.

## Shell and build

- **`[ -e link ]` follows the link.** systemd's enable links are
  absolute and root-relative (`/etc/systemd/system/x.service`). From
  outside the chroot, `-e` resolves them against the build host and
  says they are missing. Use `-L`. This bug was written twice.
- **`pkill -f pattern` matches the shell running it** when the pattern
  is in its own command line, and kills it (exit 144). Kill by PID.
- **Under `set -e`, `[ test ] && action` ends the script silently** when
  the test is false. Use `if`.
- **`forge build`'s stage 7 is a deliberate no-op.** Images come from
  `build/all` (build, `mkimage`, hash, compress, verify by decompressing,
  write the manifest, reclaim). A script that called `forge build` and
  deleted the old images first destroyed all five and made none.
- **`build/all` keeps `work/forge/desktop/rootfs` on purpose.**
  `build/staging` and every end-to-end test take tools out of it.
  Cleaning it breaks them all.
- **`systemctl enable` inside a chroot needs the unit file to exist
  first.** The overlay that installs the units once ran after the
  enables, each ending `|| true`. It only worked because work trees were
  reused between builds.

## Firmware and boot

- **A bare `File()` device path does not boot.** Firmware answers
  `EFI_NOT_FOUND`. Boot entries need `HD(partition, GPT, GUID, start,
  size)/File(...)`.
- **The staging kernel is Canonical-signed, not Microsoft-signed.**
  Firmware with Secure Boot on refuses it unless it is loaded through
  shim. Tests using `-kernel` bypass this completely.
- **OVMF prints which entry it tries and starts**, for example `BdsDxe:
  starting Boot0002 "AurOS"`, on the serial console. That is how a test
  tells "started from its own entry" from "rescued by the removable-media
  fallback".
- **OVMF appends its own entries** to `BootOrder`, as vendor firmware
  does. Assert on what comes first, not on exact equality.
- **`/sys` reports partition offsets in 512-byte units** whatever the
  disk's sector size. GPT LBAs are in logical-sector units, so a 4Kn
  disk is 8x off if you mix them.

## Environment

- The cloud container is **ephemeral**. `out/` and `work/` are not in
  git, so push anything worth keeping.
- **Disk is tight.** Roughly 8–16 GB free; one image build peaks at
  about 9.5 GB. Build one profile at a time.
- **No KVM.** QEMU runs in software emulation, so booting the full
  desktop image takes several minutes.
- **Network:** every Mozilla and Launchpad host returns 403, so there is
  no Firefox. Most other hosts, including the Ubuntu archive, work.
