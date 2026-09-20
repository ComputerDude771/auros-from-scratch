# Windows → Linux Data-Migration Map (Ferry's spec)

Ferry mounts the intact NTFS partition **read-only** (`ntfs3` kernel driver
or `ntfs-3g`) and imports data. All paths below are on the Windows volume.
"Offline" = reading NTFS from Linux, no running Windows, and (unless
stated) no user password.

## The three findings that shape everything

1. **DPAPI is the gatekeeper.** Windows encrypts user secrets (browser
   passwords, cookies, saved credentials) with the Data Protection API.
   The user's DPAPI master key is encrypted with a key derived from the
   **Windows logon password** (`SHA1(UTF16LE(pw))` for local accounts, the
   NTLM hash for MS/domain accounts) + SID. **Without that password,
   user-scoped DPAPI secrets are mathematically unrecoverable offline.**
   A Windows Hello **PIN is not sufficient** — it's an NGC protector, not
   the DPAPI password key.

2. **Two asymmetries:**
   - **WiFi PSKs = recoverable offline WITHOUT the user password.**
     Encrypted under SYSTEM/machine DPAPI, whose master key is protected
     by the `DPAPI_SYSTEM` LSA secret in the on-disk `SECURITY`+`SYSTEM`
     hives — both on the partition. (What `secretsdump -system -security`
     does, run offline.)
   - **Firefox passwords = recoverable offline WITHOUT the Windows
     password.** Firefox uses NSS, not DPAPI; the key lives in
     `key4.db`, self-contained (only blocked by a Primary Password).

3. **Chromium passwords require the user's OLD Windows password.** To
   migrate Chrome/Edge/Brave passwords, Ferry must prompt for the previous
   Windows account password and derive the master key from it. Be honest
   in the UX.

## 1. User files

Never assume `\Users\<name>\Documents`. Read redirection from
`NTUSER.DAT` (load offline with `hivex`/`chntpw`):
`HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders`
(authoritative; `%USERPROFILE%` tokens), falling back to `Shell Folders`.

Value names: `Desktop`, `Personal`=Documents,
`{374DE290-123F-4565-9164-39C4925E467B}`=Downloads, `My Pictures`,
`My Music`, `My Video`. Resolve the token, verify the target exists on the
mounted volume, then copy. Map to XDG dirs via `~/.config/user-dirs.dirs`.

**OneDrive Files-On-Demand trap (critical):** online-only files are NTFS
**reparse points, not real files** — attrs `FILE_ATTRIBUTE_OFFLINE
(0x1000)` / `RECALL_ON_DATA_ACCESS (0x400000)`, reparse tag
`IO_REPARSE_TAG_CLOUD (0x9000001A)`. Copying them yields a stub or I/O
error, **not the file**. Ferry must classify hydrated vs placeholder and
**never silently skip or silently copy stubs** — this is the #1 invisible
data-loss path. OneDrive root: `HKCU\Software\Microsoft\OneDrive\Accounts\
Personal\UserFolder` (and `Business1`).

## 2. Browsers

Profile roots: Chrome `%LOCALAPPDATA%\Google\Chrome\User Data`, Edge
`%LOCALAPPDATA%\Microsoft\Edge\User Data`, Brave
`%LOCALAPPDATA%\BraveSoftware\Brave-Browser\User Data`, Firefox
`%APPDATA%\Mozilla\Firefox\Profiles\<hash>.default-release`. Enumerate
**all** profiles.

**Chromium (Chrome/Edge/Brave, identical schema):**
- Bookmarks (`Bookmarks`, JSON) — plaintext, easy
- History (`History`, SQLite) — plaintext
- Open tabs (`Sessions/`, SNSS binary) — parse, re-open URLs
- Extensions — list migratable; extensions themselves reinstall from store
- Passwords (`Login Data` SQLite + key in `Local State` JSON) — AES-256-GCM,
  key DPAPI-wrapped in **user** context (+SYSTEM App-Bound layer on
  Chrome/Edge 127+). **Offline without password: NO.** The ABE SYSTEM
  layer is strippable offline but the underlying user layer is not.
  **With old Windows password: YES** (derive user master key → unwrap
  `Local State` key → AES-GCM decrypt).
- Cookies (`Network/Cookies`) — same key, same constraint; best-effort.

**Firefox (the exception):** best strategy is **copy the whole profile
folder** to `~/.mozilla/firefox/<profile>` — bookmarks, history, tabs,
cookies, and saved logins all work verbatim, offline, no Windows password.
(`logins.json`+`key4.db`; only blocked if a Primary Password was set.)

## 3. WiFi profiles (high value, fully automatable, no prompt)

`%SystemDrive%\ProgramData\Microsoft\Wlansvc\Profiles\Interfaces\
{GUID}\{GUID}.xml` — SSID in `<SSIDConfig>`, PSK in
`<sharedKey><keyMaterial>` (hex, **SYSTEM-DPAPI**). Recoverable offline
from the `SECURITY`+`SYSTEM` hives without the user password. Write to
NetworkManager keyfiles at `/etc/NetworkManager/system-connections/
<SSID>.nmconnection`, **mode 0600 root:root** (NM ignores otherwise),
then `nmcli connection reload`:

```ini
[connection]
id=<SSID>
type=wifi
[wifi]
mode=infrastructure
ssid=<SSID>
[wifi-security]
key-mgmt=wpa-psk
psk=<cleartext PSK>
[ipv4]
method=auto
```

Enterprise 802.1X/EAP creds are usually per-user DPAPI → not recoverable;
user re-enters.

## 4. Other settings (HKCU in NTUSER.DAT, HKLM in SOFTWARE/SYSTEM hives)

| Setting | Source | Linux destination |
|---|---|---|
| Wallpaper | `HKCU\Control Panel\Desktop\Wallpaper`; fallback `%APPDATA%\Microsoft\Windows\Themes\TranscodedWallpaper` | copy + set via shell |
| Wallpaper fit | `WallpaperStyle` (10=Fill,6=Fit,2=Stretch,0=Center) | map |
| Timezone | `HKLM\SYSTEM\...\TimeZoneInformation\TimeZoneKeyName` | **map Windows→IANA via CLDR windowsZones.xml**, `timedatectl` |
| Locale | `HKCU\Control Panel\International\LocaleName` | `/etc/locale.conf` |
| Keyboard | `HKCU\Keyboard Layout\Preload` (KLID) | map KLID→XKB |
| Display scale | `HKCU\Control Panel\Desktop\LogPixels` (96/120/144) | text-scaling; suggest, don't force |
| Printers | `HKLM\SYSTEM\...\Control\Print\Printers` | CUPS `lpadmin` by IP/IPP; **drivers don't migrate** |
| Mapped drives | `HKCU\Network\<Letter>\RemotePath` | fstab cifs; creds DPAPI → prompt |
| Default browser | `HKCU\...\UrlAssociations\https\UserChoice\ProgId` | `xdg-settings` |
| Avatar | `%APPDATA%\Microsoft\Windows\AccountPictures\` | AccountsService icon |

## 5. Mail/PIM

- Outlook **.pst** (local archives) — Thunderbird has no native PST import;
  convert PST→MBOX (`readpst`/`libpff pffexport`), then import. Medium.
- Outlook **.ost** and Windows Mail store — server caches / opaque; **re-add
  the IMAP/Exchange account and re-sync**, don't file-migrate.
- Legacy `.eml` — imports cleanly. Low.

Key point: **server-backed mail should be re-synced, not migrated.**

## 6. Application inventory

Enumerate offline from `HKLM\SOFTWARE\...\Uninstall\*` (+ `Wow6432Node`),
`HKCU\...\Uninstall\*`, and `%ProgramFiles%\WindowsApps\*\AppxManifest.xml`
(Store apps). Filter out drivers/redistributables/updates. Present a
"we found X → Linux equivalent, install it?" list. **Never auto-install,
never claim in-app data auto-migrates** unless covered above. (Full
~35-app equivalence table lives in Forge's app-map data file.)

## 7. What cannot migrate (state this honestly)

Windows apps themselves (PE binaries); Chromium passwords/cookies without
the old password; any user-DPAPI secret without the password (Credential
Manager `%APPDATA%\Microsoft\Credentials\`, RDP creds, tokens); OneDrive
online-only placeholders; EFS-encrypted files; a locked BitLocker volume;
DRM content (Kindle, DRM'd iTunes/Audible, PlayReady); licensed/activated
software and product keys; Outlook .ost / Windows Mail store; print &
hardware drivers; Hello biometrics / TPM-sealed keys; passkeys/FIDO
platform credentials.

## 8. Import order & UX

**Automatic (safe, no prompts):** (1) WiFi → NM (gets online first,
enabling re-syncs); (2) locale/timezone/keyboard (shown for confirm);
(3) known-folder files → XDG (hydrated only); (4) Firefox profile;
(5) wallpaper/avatar/default-browser/accessibility.

**Ask first:** (6) OneDrive — show hydrated-vs-placeholder counts, offer
re-sync, never silently skip; (7) **single old-Windows-password prompt**
for Chromium passwords/cookies — skippable, and if skipped say so plainly;
(8) mail — prefer re-add IMAP & re-sync; (9) printers/mapped drives —
reconstruct IPP queues, prompt for creds; (10) app inventory — user picks.

**Always at the end:** show a **"What we could NOT migrate"** report. This
honesty is a feature, not an apology.

---
*Source: research agent, web-verified against Microsoft Learn, CLDR,
NetworkManager docs, and forensics sources (HackTricks DPAPI, SharpDPAPI,
xaitax ABE research, firefox_decrypt). Verify before betting user data on
any single line.*
