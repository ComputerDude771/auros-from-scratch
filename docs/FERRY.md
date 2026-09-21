# Ferry — the first-boot migration engine

> The moment AurOS earns or loses the user's trust. They agreed to
> replace Windows because we promised their files would be waiting on
> the other side. Ferry is the code that keeps that promise — and, just
> as importantly, the code that tells the truth about the parts of it
> nobody can keep.

Ferry runs once, on the first boot of a freshly installed AurOS, while
the user's old Windows partition is still sitting intact on the same
disk. It mounts that partition **read-only**, reads the user's data and
settings off it, and lands them in the new home directory. Then it shows
a report of everything that came across and everything that could not —
because an honest "we could not bring your Chrome passwords without your
old password" is worth more than a silent gap the user discovers a week
later.

The authoritative specification is
[`docs/research/migration.md`](research/migration.md); this document is
the design and the honest limitations. Where they disagree, the spec
wins and this file is the bug.

---

## Governing rules

These are not aspirations. Each is enforced in code and checked by the
test suite, and each maps to a hard requirement of the product.

1. **The Windows partition is mounted read-only, always.** There is no
   code path in Ferry that mounts it any other way. `mount_ro` passes
   `ro` as both a driver option and a mount flag and then re-reads
   `/proc/mounts` to confirm the kernel agrees, unmounting again if it
   does not. The test suite snapshots every file on the source volume
   before the stages run and asserts not one byte, size or mtime changed
   after.

2. **A hibernated or dirty volume is refused, out loud, with the fix.**
   Not because we might corrupt it — we never write to it — but because
   a hibernated volume is *stale*: the newest copy of the user's work is
   inside `hiberfil.sys`, not in the files. Importing from it copies
   yesterday's spreadsheet and calls it today's. Windows 8+ "Fast
   Startup" makes an ordinary shutdown produce exactly this state, so it
   is the common case, not the rare one. Every importing stage calls
   `require_clean_volume` first and exits `3` with a plain-language
   remedy if the volume is not clean. `ferry detect` is the one
   exception: it *reports* the condition instead of refusing, because
   its whole job is to give the UI the sentence that explains why.

3. **Every operation is idempotent and resumable.** First boot can be
   interrupted by a flat battery or an impatient user. A re-run must not
   duplicate, must not skip, must not overwrite. Two mechanisms provide
   this: stages record their own completion (`ferry run` skips finished
   ones), and within a stage `safe_copy` is content-aware (same size and
   mtime ⇒ already done) and writes through a `.ferrypart` temp so a
   half-copy never exists at the destination.

4. **Never overwrite a destination without recording it.** If something
   already sits where an imported file would go, Ferry keeps *both* — the
   Windows copy lands beside it as `name (from Windows).ext` — and writes
   a collision row to the manifest and the report. We never choose which
   of two files the user meant to keep. `safe_copy` is the only function
   allowed to create a file in the user's home, so this rule cannot be
   bypassed by a careless stage.

5. **Progress is reportable.** Ferry runs behind a UI. Each stage writes
   a JSON progress line to `$FERRY_STATE/state/<stage>.progress` (and
   appends to `$FERRY_PROGRESS` when set), so the UI polls a file rather
   than speaking a protocol to a daemon.

6. **What could not migrate is a feature, not an apology.** The final
   report reads from a ledger every stage writes as it works, and states
   the unconditional truths (Chromium secrets, EFS, DRM, drivers…)
   whether or not this particular machine hit them — because "we didn't
   try" and "it can't be done" are different sentences and the user
   deserves the true one.

---

## The three findings that shape everything

From the research (`migration.md` §"The three findings"), because they
explain why Ferry is shaped the way it is:

- **DPAPI is the gatekeeper.** Windows encrypts user secrets — browser
  passwords, cookies, saved credentials — with a key derived from the
  user's *Windows logon password*. Without that password, those secrets
  are mathematically unrecoverable from an offline disk. A Windows Hello
  PIN does not help; it is a different protector.

- **Two things escape DPAPI's user layer.** WiFi PSKs are encrypted
  under the *machine* DPAPI key, whose protector lives in on-disk hives,
  so they are recoverable offline with no user password. And Firefox
  does not use DPAPI at all — its login key is self-contained in
  `key4.db` — so the entire Firefox profile migrates verbatim.

- **Chromium passwords need the old Windows password.** Chrome/Edge/Brave
  wrap their key in user-context DPAPI (plus an App-Bound SYSTEM layer on
  recent versions). The SYSTEM layer strips offline; the user layer does
  not. Migrating them requires prompting for the previous password, and
  the UX must be honest that it is optional.

---

## Architecture

Ferry is POSIX shell plus two small C helpers, matching the house style
(`src/aurora/aurora`, `src/common/theme.c`): dense comments that explain
*why*, `_`-prefixed locals because busybox `ash` has no lexical scope,
no dependency beyond busybox and awk.

```
  ferry                orchestrator: mounts ro, runs stages in spec order,
                       resumes, prints the report
  ferry-detect         READ-ONLY discovery -> JSON for the UI
  ferry-wifi       (1) WiFi profiles  -> NetworkManager keyfiles
  ferry-settings   (2) timezone/locale/keyboard/wallpaper/…
  ferry-files      (3) known folders  -> XDG dirs (placeholder-aware)
  ferry-firefox    (4) whole Firefox profile -> ~/.mozilla
  ferry-report         the "what we could NOT migrate" report
  ferry-hive           registry access: hivex if present, else our reader
  ferry-dpapi          the offline-DPAPI call-out (interface only; see below)
  ferry-common.sh      the shared rules: read-only mount, volume health,
                       idempotent copy, path/registry/known-folder logic
  hivepeek.c           a from-scratch read-only `regf` hive parser
  winattr.c            NTFS-attribute / OneDrive-placeholder classifier
  data/windowsZones.tab  CLDR Windows-timezone -> IANA mapping
  data/klid.tab          Windows keyboard-layout-id -> XKB mapping
```

The order of the stages is load-bearing and comes straight from the spec
(§8): **WiFi first**, because getting online is what makes every later
re-sync — mail, browser accounts, OneDrive — possible at all.

### Why two C helpers, and only two

Everything that *can* be shell *is* shell, because shell is auditable by
the people who audit this project. Two jobs cannot be:

- **Parsing a registry hive.** A `regf` file is a binary B-tree. `hivex`
  (`hivexget`/`hivexsh`) is the mature tool and Ferry prefers it when it
  is installed — but AurOS is built from source and cannot assume
  libhivex is present on a first boot, so `hivepeek.c` is a dependency-
  free fallback that produces identical output. It is deliberately
  minimal: read-only, no LOG replay, no checksum enforcement (a hive
  lifted from a machine that used Fast Startup has stale sequence numbers
  and is still perfectly readable). It parses hostile input as root, so
  every access is bounds-checked and every list walk has a visit budget;
  the test suite fuzzes it with thousands of corrupted hives under
  AddressSanitizer without a crash.

- **Classifying an NTFS file as real data or a cloud placeholder.** This
  needs the NTFS attribute word and the reparse tag, which ntfs-3g and
  the ntfs3 driver expose as extended attributes. `winattr.c` reads them
  and applies the decision below. See "The OneDrive trap".

Both helpers accept a test seam (`FERRY_WINATTR_MAP`, `user.*` xattrs,
`FERRY_HIVE_BACKEND`) so the whole engine can be tested on an ordinary
ext4 tree with no NTFS image and no root — which is exactly what
`tests/run.sh` does.

---

## What migrates

### WiFi (`ferry-wifi`) — the automatic win

Parses every `ProgramData/Microsoft/Wlansvc/.../*.xml`, maps
authentication to a NetworkManager `key-mgmt` (open → `none`, WPA/WPA2-PSK
→ `wpa-psk`, WPA3 → `sae`), and writes a `.nmconnection` keyfile at mode
`0600 root:root` (NetworkManager ignores anything looser) with a
deterministic UUID so a re-run updates rather than duplicates. Handles
hidden networks, the hex SSID form, and open networks. Fully implemented
and tested.

The **PSK** is the one caveat. A cleartext key (`<protected>false`) is
imported directly. A DPAPI-encrypted key is handed to a single isolated
call-out, `dpapi_decrypt` → `ferry-dpapi`/`$FERRY_DPAPI_HELPER`. **That
decryption is not implemented in this build** (see below). When no helper
answers, Ferry still creates the connection — correctly, with the right
security type — but *without* a saved password, and reports the network
so the user types the password once on first connect. It never writes a
connection with a wrong or empty PSK and pretends it works.

Enterprise **802.1X/EAP** networks are reported, never faked: their
credentials are per-user DPAPI and the user re-enters them.

### User files (`ferry-files`) — the one that must not lie

Copies Desktop, Documents, Downloads, Pictures, Music and Videos to the
XDG directories. Two things make this more than a `cp -r`:

**It resolves redirection.** It never assumes `\Users\<name>\Documents`.
It reads each folder's real location from
`HKCU\…\Explorer\User Shell Folders` in `NTUSER.DAT` (falling back to
`Shell Folders`), expands the `%TOKENS%`, and — critically — handles the
OneDrive "back up your folders" case, where Documents/Desktop/Pictures
have been silently moved into the sync root. A folder redirected onto a
drive letter that is not part of the migration is reported as
`off-volume`, never quietly replaced with an empty default.

> Token expansion is done with shell string surgery, not `sed`, on
> purpose: the replacement is a Windows path full of backslashes, and
> `%USERPROFILE%` → `C:\Users\Bob` through `sed` yields `C:SERSBOB`
> because `sed` reads the `\U` as an uppercase command. That bug would
> have silently emptied every imported folder. The comment in
> `ferry-common.sh` exists so nobody reintroduces it.

**The OneDrive trap.** See its own section below.

Idempotent, resumable, and collision-safe as described in the governing
rules. Windows junk (`desktop.ini`, `Thumbs.db`) is filtered so the "what
came across" count stays trustworthy.

### The OneDrive trap — the #1 invisible data-loss path

OneDrive Files-On-Demand leaves files that look completely normal from
Linux: right name, right size in `ls`, right modification time. They
contain **nothing** — the bytes are on a server, and the directory entry
is an NTFS reparse point with tag `IO_REPARSE_TAG_CLOUD (0x9000001A)` and
the `OFFLINE` / `RECALL_ON_DATA_ACCESS` attributes set. Copying one
yields a stub or an I/O error, *not* the file. On a migration that is
silent, invisible data loss: the filename appears in the new Documents
folder and the user believes the photo is there.

So **Ferry never copies a file it has not classified.** `winattr.c` is
the only thing allowed to say "hydrated", and it is built to refuse the
optimistic answer:

```
  reparse tag is a cloud tag ............... placeholder   (report, don't copy)
  OFFLINE / RECALL_* / UNPINNED attr set ... placeholder   (report, don't copy)
  EFS-encrypted attr set ................... encrypted     (report, can't read)
  other reparse point (junction/symlink) ... reparse       (report, don't follow)
  size > 0 but zero blocks allocated ....... suspect       (copy AND flag to verify)
  size == 0 ................................ hydrated       (legitimately empty)
  blocks allocated ......................... hydrated       (bytes are on disk)
  no attribute source, can't tell .......... unknown        (report, don't assume)
```

The block-count fallback is the safety net for when no attribute is
readable: a file that claims bytes but owns zero disk blocks is holding
none, which is exactly the shape of a placeholder. A file *with* blocks
cannot be online-only, so its bytes are genuinely present. Every
placeholder is listed in the report **by name and by the size it would
have been**, with the one-line fix ("right-click in OneDrive → Always
keep on this device, then re-run"). Nothing is ever silently skipped and
nothing is ever silently copied as a stub.

### Firefox (`ferry-firefox`) — the clean win

Firefox migrates completely and offline with no Windows password, so the
strategy is not to parse anything — it is to copy the whole profile
folder to `~/.mozilla/firefox/`. Bookmarks, history, open tabs, cookies
*and saved passwords* all work verbatim, because Firefox on Linux reads
the identical files. Every profile in `profiles.ini` is enumerated (a
second profile is a second person's bookmarks). Caches, stale locks and
`compatibility.ini` (which pins the profile to the old binary's path) are
dropped from the copy; everything else is byte-for-byte. The only
possible caveat, noted in the report, is a Primary Password — the logins
still migrate, and Firefox asks for it once.

### Settings (`ferry-settings`)

Timezone, locale, keyboard layout, display scale, wallpaper, default
browser, and mapped-drive reporting. Everything is *suggested* — written
where the desktop reads it and echoed in the report so the welcome screen
can offer "we set your timezone to Europe/London — change it?" — never
silently forced. Display scale in particular is only ever a hint.

The hard part is the timezone, and it is the reason `data/windowsZones.tab`
ships with Ferry. Windows stores a `TimeZoneKeyName` of its own invention
("GMT Standard Time"); everything on Linux wants an IANA id
("Europe/London"). The table is the CLDR `windowsZones.xml` territory-001
default mapping, so the conversion works with no network. Keyboard layout
maps the Windows KLID to XKB via `data/klid.tab`, with a language-id
fallback so an unlisted KLID still lands on the right language.

### Discovery (`ferry-detect`)

Read-only, unmounts everything it mounts, and emits JSON for the UI: the
Windows version and build (correctly calling build ≥ 22000 "Windows 11"
even though the registry still says "Windows 10"), the volume health, the
registry backend in use, the WiFi count, and per-profile the resolved
known folders (with file counts and sizes), the browsers found, and the
mail/credential inventory. A `--human` mode prints the same facts for a
person at a terminal. A locked BitLocker volume is a finding, not a
silent skip.

---

## What cannot migrate — stated honestly

This list is printed at the end of *every* run, whether or not the
machine hit each item, because the honesty is the product.

| Not migrated | Why | What the user can do |
|---|---|---|
| Chromium (Chrome/Edge/Brave) passwords & cookies | wrapped in your Windows-login DPAPI key | re-enter, or sign back into the browser and re-sync; a future build can prompt for the old password |
| Windows Credential Manager, RDP creds, app tokens | encrypted to your Windows login, unrecoverable offline | re-enter in each app |
| OneDrive online-only files | live on the server, not the disk | hydrate in OneDrive, or sign in to OneDrive on Linux |
| EFS-encrypted files | need your Windows encryption certificate | decrypt in Windows first, then re-run |
| Files on a locked BitLocker volume | unreadable until unlocked | unlock in Windows, then re-run |
| DRM media (Kindle, some iTunes/Audible) | licensed to the old machine | re-download from the vendor |
| Installed Windows programs | are Windows binaries | reinstall the Linux equivalent (Forge's app map) |
| Print & device drivers | hardware/OS-specific | Linux supplies its own |
| Windows Hello PIN / biometrics | sealed to this PC's TPM | set up again |
| Outlook `.ost` / Windows Mail store | server caches | re-add the account; it re-syncs |
| WiFi PSKs (this build) | DPAPI decryption not yet implemented | enter each password once on connect |

---

## The DPAPI seam — honest about what is not built

WiFi PSKs are, per the research, recoverable offline **without** the
user's password, because they are machine-DPAPI encrypted and the
protecting `DPAPI_SYSTEM` LSA secret is in the on-disk `SECURITY` and
`SYSTEM` hives. Ferry implements everything up to the crypto — finding
the profiles, parsing the XML, mapping to NetworkManager, writing the
keyfiles — and isolates the decryption itself behind one call-out so it
can be filled in without touching any of that.

`ferry-dpapi` is currently a **stub that fails loudly** (exit 75) rather
than a helper that fails silently. Its header documents exactly what a
real implementation must do (read the SYSKEY from `SYSTEM`, decrypt the
LSA secrets in `SECURITY` to get `DPAPI_SYSTEM`, unwrap the SYSTEM DPAPI
master key from `Windows\System32\Microsoft\Protect\S-1-5-18\…`, then
decrypt the blob — what `secretsdump -system -security` does offline).
Until it exists, WiFi networks import without their saved password and
are flagged in the report. An external, complete implementation can be
dropped in via `$FERRY_DPAPI_HELPER` with no other change; the helper
seam is tested end-to-end with a mock.

This is the one place the design deliberately ships an interface instead
of an implementation, and it is called out here, in the code comments,
and in the report, rather than hidden.

---

## State, resume, and the manifest

Everything Ferry knows lives under `$FERRY_STATE` (default
`/var/lib/ferry`) and survives a reboot on purpose:

```
  state/<stage>.status     running | done | failed (+ timestamp)
  state/<stage>.progress   latest JSON progress line, for the UI
  report.d/<stage>.tsv     the ledger: category, severity, item, detail
  manifest.tsv             every destination byte written, incl. collisions
  suggested.conf           settings suggestions for the welcome UI
```

`ferry run` after a crash reads the status files, skips finished stages,
and re-runs a partial one — which copies only what is missing, because
`safe_copy` is content-aware. The manifest is the single source of truth
for "what did we create", including the files we declined to overwrite;
"never overwrite without recording" is enforced by making the recording
the only way to write.

---

## Usage

```
  ferry detect [--device DEV | --root DIR] [--human]   find Windows (read-only)
  ferry run                                             mount, import, report
  ferry wifi | files | firefox | settings [opts]        one stage
  ferry report [--json | --brief]                       the report
  ferry status                                          where a resume picks up
  ferry unmount                                         release the ro mount
```

Common options (every stage): `--root DIR` (an already-mounted volume),
`--device DEV`, `--user NAME` (which Windows profile; Ferry refuses to
guess between two real accounts), `--home DIR`, `--owner USER`,
`--dry-run`, `--json`, `--progress FILE`, `--force`.

---

## Testing

Ferry cannot be tested against a real Windows disk in CI, so
`tests/mkfakewin.sh` builds a synthetic NTFS-like tree containing every
trap the real world has — a Documents folder redirected into OneDrive,
cloud placeholders mixed with hydrated files, a Videos folder on a second
drive, a wrong-case `users` directory, a DPAPI-protected WiFi PSK next to
a cleartext and an enterprise one, and a destination collision — and
`tests/mkhive.py` writes real binary `regf` hives. `tests/run.sh` asserts
Ferry does the honest thing with each (42 checks, all passing), including
the read-only guarantee (a before/after snapshot of the whole source
volume) and the safety gate (a hibernated volume is refused). The C
helpers build warning-clean under `-Wall -Wextra` and pass
AddressSanitizer against thousands of corrupted hives.

```
  make            build the two C helpers into build/
  make check      shellcheck-syntax every script + run the suite
```

---

## Known limitations

- **SYSTEM-DPAPI WiFi PSK decryption is not implemented** (the interface
  is; see the DPAPI seam). This is the single biggest gap versus the
  spec's "fully automatable, no prompt" ambition for WiFi.
- **Chromium password migration is not implemented.** The spec describes
  the old-password prompt path; this build reports the limitation and
  does not prompt.
- **Mail (.pst → MBOX) and the application inventory are not
  implemented** as importing stages. `ferry-detect` counts `.pst`/`.ost`
  and reports them; conversion and the app-equivalence list are Forge's
  data and a future stage.
- **The timezone table uses CLDR territory-001 defaults.** Windows keeps
  one name where IANA splits many zones by country ("Central Standard
  Time" is Chicago in the US but Mexico City in Mexico); the default is
  right for the large majority, and the value is shown for the user to
  confirm, never forced.
- **The hive reader skips LOG replay and checksum verification.** By
  design (a Fast-Startup hive is readable and Ferry refuses dirty volumes
  anyway), but it means a setting changed in the final seconds before an
  unclean shutdown, and living only in the transaction LOG, is not seen.
