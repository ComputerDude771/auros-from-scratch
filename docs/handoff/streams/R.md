# Stream R — Ferry: her files come across from Windows

Owner of `src/ferry/**` (and this file). Brief (docs/handoff/STREAMS.md):
when she says "it works", her Documents, Pictures, Desktop, Downloads,
Music, Videos and her browser bookmarks come across from Windows,
plainly and safely: read-only on the Windows side, **never her
passwords** (lead's rule: never copy or decrypt saved passwords). Fuzz
the hive/registry parsers, time a large fixture, make the progress she
sees honest.

## How Ferry is reached today (read this first)

- Welcome panel (`src/aurshell/welcome.c`) → "Bring my files across" →
  writes `import` to `/run/auros/answer` → `rootfs/usr/lib/auros/answer.sh`
  (root) → `aurfirst ferry` gate (only after "Yes, it works") →
  `/usr/bin/ferry run --auto >> /run/auros-answer/import.log 2>&1` →
  `ferry report > /var/lib/auros/import-report.txt`.
- **What she sees while it runs is the last line of import.log**
  (welcome.c `read_import_line`). Today that is developer chatter
  ("-> mounted /dev/sda3 read-only at ..."): EASY.md words she must not see.
- Installed into the image by `build/forge` ~line 909 (explicit list of
  scripts; helpers compiled in the chroot with gcc). A new helper or a
  removed script must be added/removed there too.
- Tests: `cd src/ferry && make && sh tests/run.sh` (43 checks, green at
  `6ce278d`); run in `.github/workflows/tests.yml` step "Ferry".

## Findings (verified 2026-10-23, before any change)

1. **Passwords are copied.** `ferry-firefox` copies the whole profile,
   including `logins.json` + `key4.db` (saved passwords), and the suite
   *asserts* it. `ferry-wifi` writes cleartext Wi-Fi keys into
   NetworkManager files and has a DPAPI decryption hook
   (`FERRY_DPAPI_HELPER` / `ferry-dpapi`). All three break the rule.
2. **Too slow for a 2013 laptop.** `ferry-files` copies in a shell loop:
   ~26 ms per file here (5,000 x 4 KB files = 131 s). 40,000 photos ≈ 17 min
   of overhead before any bytes.
3. **Progress is per file, not per byte, and only in a JSON file the
   screen never reads.** She sees the last log line.
4. **Root writes into her home through paths she controls**
   (`mkdir -p`, `cp`, `> user-dirs.dirs`, `chown -R` as root). A symlink
   she or a program running as her plants (e.g. `~/Pictures -> /etc/cron.d`)
   makes root write Windows-side content there: a local root escalation.
5. **Edge/Chrome bookmarks are not brought across at all** (most of
   these users use Edge). Chromium's `Bookmarks` file is plain JSON,
   not encrypted.
6. **Copying Windows `installs.ini`/`profiles.ini`** pins the profile to
   the Windows install hash; Linux Firefox then likely ignores it and
   makes an empty profile ("my bookmarks are gone"). Needs proof with a
   real Firefox (Actions).
7. `hivepeek.c`: `cell()`/`cell_size()` negate `INT32_MIN` (UB) on a
   hostile cell size. FERRY.md claims a fuzz run that does not exist.
8. `safe_copy` collision bug: `"(from Windows)"` name chosen from the
   whole path (`a/dir.v2/file` → wrong folder), and a second collision
   silently overwrites the first `(from Windows)` file.

## Plan (in order; each step = one pushed commit with tests)

1. **Never passwords**: firefox stage drops logins.json, logins-backup.json,
   key4.db, key3.db, signons.sqlite, autofill-profiles.json; wifi stage
   writes no keyfile and never decrypts (lists the networks by name: "AurOS
   will ask for each password once"; her current Wi-Fi comes via the
   installer's D10 tick box, not Ferry). Tests flip to assert absence.
2. **`ferry-copy` (C)**: one process for all six folders: walk without
   following links, classify with the winattr rules, plan totals first,
   copy with temp + `renameat2(RENAME_NOREPLACE)`, keep both on collision,
   mtime preserved, periodic `syncfs`; opens state files as root then
   **drops to her uid** for every read/write; byte-weighted progress and
   a plain line ("Copying your pictures: 1200 of 5000 files. About 4
   minutes left.") about once a second.
3. **Edge/Chrome bookmarks**: `chromemarks.c` (JSON → Netscape
   bookmarks.html, scheme allow-list http/https/ftp). New Firefox profile
   seeded with `bookmarks.html` (Firefox imports it when places.sqlite is
   new), made default in profiles.ini/installs.ini (backups kept). If a
   Windows Firefox profile is primary, the Edge file is saved in her Home
   with plain steps.
4. **Fuzz**: libFuzzer + ASan/UBSan harnesses in `src/ferry/fuzz/` for
   hivepeek, chromemarks, collision naming; seed corpora from mkhive.py;
   fix finds; regression inputs in the suite.
5. **Plain words for her**: `ferry run --plain` (chatter to
   /var/lib/ferry/ferry.log, plain lines on stdout), a plain report
   "What came from Windows.txt" in her Home, one-line change in
   answer.sh; exit 2 when something she'd expect did not come (welcome
   then says "Some things could not be brought across.").
6. **Proof on Actions**: new `.github/workflows/ferry.yml` (concurrency,
   one job): suite, ASan build, short fuzz, and real Firefox (Mozilla apt)
   started headless on the migrated home, checking places.sqlite has the
   bookmarks and no logins exist.
7. Update `docs/FERRY.md` (it currently promises passwords migrate).

## State

- Done and pushed: plan step 1 (`fb90bcd`): no saved password crosses
  (Firefox password/card files skipped, Wi-Fi only listed, ferry-dpapi
  deleted); `src/ferry/install.sh` is now the one install list (forge
  calls it); suite 49/49, mutations of both protections caught.
- In progress: step 2, `src/ferry/copy.c` (`ferry-copy`). Nothing of it
  is pushed yet; if this line is still here, start step 2 from scratch.

## Traps

- Disk in the container is nearly full (2.8 GB free): keep fixtures
  < 150 MB, delete `r-tmp` in the scratchpad when done.
- `sh tests/run.sh` must run with `TMPDIR` on the scratchpad.
- `build/forge` lists Ferry's files by name: removing `ferry-dpapi` or
  adding a helper without editing forge breaks the image build.
- POSIX sh: `_`-prefixed locals collide across functions (see ferry-firefox).

## How to verify

```
cd src/ferry && make && sh tests/run.sh      # every check "ok", 0 failed
```
