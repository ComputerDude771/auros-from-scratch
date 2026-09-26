# 6. First boot (aurfirst): the fix that isn't in any image

**Severity:** high for v3. Every image built without it carries the bug.
**Status:** fixed in code on one branch, merged nowhere, built into
nothing.

## 6.1 The `aurfirst` fix is on the wrong branch

Commit `2d85777` on `claude/linux-distro-from-scratch-tt5dx1` fixes
"which AurOS is this AurOS". Before the fix, `aurfirst` promoted the
first firmware entry called "AurOS". After "Put Windows back" and a
second install, that could be the dead entry from the first install, so
the PC went back to Windows right after the person said AurOS works.
The fix picks the entry by the partition it names. `aurfirsttest`
covers it with 54 checks.

- The development branch `claude/laughing-cray-ayao6i` **doesn't have
  it**.
- The published v2 image **doesn't have it**.
- **The two handoffs disagree about the merge.** `docs/handoff/BRANCHES.md`
  on the original branch says it's clean. `HANDOFF.md` §6 on the
  development branch says it "will conflict in `tools/efivarstore.py` and
  `docs/AURBRIDGE.md`". A `git merge-tree --write-tree` run on 2026-09-26
  between the two current tips reported **no conflicts**. Merge it, then
  re-run `tools/aurfirsttest.sh` (expect 54).

Today this only bites after "Put Windows back", and nothing a person can
press starts that yet ([05](05-installer-wizard.md) §5.1). It becomes
live the moment that button exists, so the merge has to come first.

## 6.2 `tools/firstboottest.sh` case C isn't a real test

Case C plants a second firmware entry called "AurOS" with a lower number,
to check that the booted AurOS picks its own. On OVMF the planted entry
**doesn't survive the boot**: OVMF removes it and reuses its number for
one of its own entries. This happens both for an entry pointing at a
partition that doesn't exist and for one pointing at a real `AUROS-BOOT`
on a second disk. The case now detects this and fails honestly. Nobody
knows why OVMF does it.

- A one-minute experiment: boot OVMF with no OS and two disks, then
  read the variable store with `tools/efivarstore.py`.
- Where to look: OVMF's platform boot manager and `QemuBootOrderLib`.
- Until then, the proof is `aurfirsttest.sh`'s eight "two entries
  called AurOS" checks.

## 6.3 "Put Windows back" leaves a dead "AurOS" entry

**Accepted, documented.** The restore path writes nothing to firmware
variables on purpose, so a stale "AurOS" entry stays in the firmware's
boot menu. It's harmless once 6.1 is merged. A person will still see it
and wonder what it is. Decide whether AurOS or the restore should
explain or remove it.
