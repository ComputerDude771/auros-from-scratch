#!/bin/sh
# Build a fake Windows volume under a directory, for testing Ferry
# without an NTFS image or a spare laptop.
#
# It is deliberately awkward in the ways real machines are awkward:
#   - Documents is redirected into OneDrive (the single most common
#     real-world redirection, and the one that breaks naive importers);
#   - the OneDrive folder mixes hydrated files with cloud placeholders;
#   - casing does not match what the registry says ("users" vs "Users");
#   - one WiFi profile is DPAPI-protected, one is in the clear, one is
#     enterprise, one is open;
#   - there is a file already present at the destination, to exercise
#     the no-overwrite rule.
#
#   tests/mkfakewin.sh /tmp/fakewin [--hibernated]
set -eu

ROOT=${1:?usage: mkfakewin.sh DIR [--hibernated]}
HIB=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)

rm -rf "$ROOT"
mkdir -p "$ROOT"

# ── Directory skeleton. Note the lowercase 'users': NTFS is
#    case-preserving and the registry says "C:\Users\Bob", so anything
#    that does not resolve case-insensitively finds nothing here.
mkdir -p "$ROOT/Windows/System32/config" \
         "$ROOT/users/Bob/Desktop" \
         "$ROOT/users/Bob/Downloads" \
         "$ROOT/users/Bob/Pictures/2019" \
         "$ROOT/users/Bob/Music" \
         "$ROOT/users/Bob/Videos" \
         "$ROOT/users/Bob/Documents" \
         "$ROOT/users/Bob/OneDrive/Documents/Taxes" \
         "$ROOT/users/Bob/AppData/Roaming/Mozilla/Firefox/Profiles/a1b2c3d4.default-release/storage" \
         "$ROOT/users/Bob/AppData/Roaming/Microsoft/Windows/Themes" \
         "$ROOT/users/Bob/AppData/Local/Google/Chrome/User Data/Default" \
         "$ROOT/users/Bob/Documents/Outlook Files" \
         "$ROOT/users/Public/Documents" \
         "$ROOT/ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/{11111111-2222-3333-4444-555555555555}"

# ── Hives.
python3 "$HERE/mkhive.py" "$ROOT/Windows/System32/config/SOFTWARE" '{
  "Microsoft\\Windows NT\\CurrentVersion": {
    "ProductName": ["sz", "Windows 10 Pro"],
    "DisplayVersion": ["sz", "22H2"],
    "CurrentBuild": ["sz", "19045"],
    "EditionID": ["sz", "Professional"],
    "RegisteredOwner": ["sz", "Bob"]
  },
  "Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\S-1-5-21-1111111111-2222222222-3333333333-1001": {
    "ProfileImagePath": ["exp", "C:\\Users\\Bob"]
  },
  "Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\S-1-5-18": {
    "ProfileImagePath": ["exp", "C:\\Windows\\system32\\config\\systemprofile"]
  }
}'

python3 "$HERE/mkhive.py" "$ROOT/Windows/System32/config/SYSTEM" '{
  "Select": {"Current": ["dword", 1]},
  "ControlSet001\\Control\\ComputerName\\ComputerName": {"ComputerName": ["sz", "BOB-LAPTOP"]},
  "ControlSet001\\Control\\TimeZoneInformation": {
     "TimeZoneKeyName": ["sz", "GMT Standard Time"],
     "StandardName": ["sz", "GMT Standard Time"]
  }
}'

python3 "$HERE/mkhive.py" "$ROOT/Windows/System32/config/SECURITY" '{
  "Policy\\Secrets\\DPAPI_SYSTEM\\CurrVal": {"@": ["binary", "not a real secret"]}
}'

python3 "$HERE/mkhive.py" "$ROOT/users/Bob/NTUSER.DAT" '{
  "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders": {
    "Desktop":      ["exp", "%USERPROFILE%\\Desktop"],
    "Personal":     ["exp", "%OneDrive%\\Documents"],
    "{374DE290-123F-4565-9164-39C4925E467B}": ["exp", "%USERPROFILE%\\Downloads"],
    "My Pictures":  ["exp", "%USERPROFILE%\\Pictures"],
    "My Music":     ["exp", "%USERPROFILE%\\Music"],
    "My Video":     ["exp", "D:\\Media\\Videos"]
  },
  "Software\\Microsoft\\OneDrive\\Accounts\\Personal": {
    "UserFolder": ["sz", "C:\\Users\\Bob\\OneDrive"],
    "UserEmail":  ["sz", "bob@example.com"]
  },
  "Control Panel\\Desktop": {
    "Wallpaper": ["sz", "C:\\Users\\Bob\\Pictures\\wall.jpg"],
    "WallpaperStyle": ["sz", "10"],
    "TileWallpaper": ["sz", "0"],
    "LogPixels": ["dword", 120]
  },
  "Control Panel\\International": {"LocaleName": ["sz", "en-GB"]},
  "Keyboard Layout\\Preload": {"1": ["sz", "00000809"], "2": ["sz", "0000040C"]},
  "Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice": {
    "ProgId": ["sz", "FirefoxURL-308046B0AF4A39CB"]
  },
  "Network\\Z": {"RemotePath": ["sz", "\\\\nas\\shared"]}
}'

# ── User data. Sizes are small; what matters is the classification.
echo "shopping list"            > "$ROOT/users/Bob/Desktop/notes.txt"
echo "report"                   > "$ROOT/users/Bob/Desktop/report.docx"
echo "installer"                > "$ROOT/users/Bob/Downloads/setup.exe"
echo "JPEGDATA-wallpaper"       > "$ROOT/users/Bob/Pictures/wall.jpg"
echo "JPEGDATA-holiday"         > "$ROOT/users/Bob/Pictures/2019/holiday.jpg"
echo "song"                     > "$ROOT/users/Bob/Music/track.mp3"
echo "[.ShellClassInfo]"        > "$ROOT/users/Bob/Pictures/desktop.ini"
printf 'thumbcache'             > "$ROOT/users/Bob/Pictures/Thumbs.db"
echo "TAXRETURN-2023-real"      > "$ROOT/users/Bob/OneDrive/Documents/Taxes/2023.pdf"
echo "letter"                   > "$ROOT/users/Bob/OneDrive/Documents/letter.docx"
echo "NOT the redirected one"   > "$ROOT/users/Bob/Documents/stale-local.txt"
echo "PSTDATA"                  > "$ROOT/users/Bob/Documents/Outlook Files/archive.pst"
echo "JPEGDATA-transcoded"      > "$ROOT/users/Bob/AppData/Roaming/Microsoft/Windows/Themes/TranscodedWallpaper"

# Firefox profile: the clean win. Cache dirs exist to prove they are
# skipped; compatibility.ini exists to prove it is dropped.
FF="$ROOT/users/Bob/AppData/Roaming/Mozilla/Firefox"
cat > "$FF/profiles.ini" <<'INI'
[Install308046B0AF4A39CB]
Default=Profiles/a1b2c3d4.default-release
Locked=1

[Profile0]
Name=default-release
IsRelative=1
Path=Profiles/a1b2c3d4.default-release
Default=1

[General]
StartWithLastProfile=1
Version=2
INI
P="$FF/Profiles/a1b2c3d4.default-release"
echo "SQLite format 3 places"   > "$P/places.sqlite"
echo "SQLite format 3 key4"     > "$P/key4.db"
echo '{"logins":[]}'            > "$P/logins.json"
echo "user_pref()"              > "$P/prefs.js"
echo "LastVersion=115.0"        > "$P/compatibility.ini"
mkdir -p "$P/cache2/entries" "$P/storage/default"
echo "cachejunk"                > "$P/cache2/entries/DEADBEEF"
echo "lock"                     > "$P/.parentlock"

echo '{"roots":{}}'             > "$ROOT/users/Bob/AppData/Local/Google/Chrome/User Data/Default/Bookmarks"
echo 'prefs'                    > "$ROOT/users/Bob/AppData/Local/Google/Chrome/User Data/Default/Preferences"
echo 'SQLite Login Data'        > "$ROOT/users/Bob/AppData/Local/Google/Chrome/User Data/Default/Login Data"
echo '{"os_crypt":{"encrypted_key":"RFBBUEkAAAA="}}' > "$ROOT/users/Bob/AppData/Local/Google/Chrome/User Data/Local State"

# ── OneDrive placeholders: the invisible-data-loss case. Real size in
#    the directory entry, no blocks, cloud reparse tag.
for f in "Taxes/2022.pdf" "vacation-video.mp4" "scan.pdf"; do
    p="$ROOT/users/Bob/OneDrive/Documents/$f"
    mkdir -p "$(dirname "$p")"
    : > "$p"
done
python3 - "$ROOT" <<'PY'
import os, sys
root = sys.argv[1]
ARCH = 0x20
CLOUD = (0x400 | 0x1000 | 0x400000 | ARCH)   # reparse|offline|recall|archive
base = os.path.join(root, "users/Bob/OneDrive/Documents")
stubs = {"Taxes/2022.pdf": 812_345, "vacation-video.mp4": 1_900_000_000,
         "scan.pdf": 45_000}
for rel, size in stubs.items():
    p = os.path.join(base, rel)
    os.truncate(p, size)                     # size, but zero blocks
    try:
        os.setxattr(p, "user.ntfs_attrib", CLOUD.to_bytes(4, "little"))
        os.setxattr(p, "user.ntfs_reparse_data",
                    (0x9000001A).to_bytes(4, "little") + b"\0" * 12)
    except OSError:
        pass                                 # the map-file seam covers this
for rel in ["letter.docx", "Taxes/2023.pdf"]:
    p = os.path.join(base, rel)
    try:
        os.setxattr(p, "user.ntfs_attrib", ARCH.to_bytes(4, "little"))
    except OSError:
        pass
PY

# ── WiFi profiles, one of each kind that exists in the wild.
W="$ROOT/ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/{11111111-2222-3333-4444-555555555555}"
cat > "$W/{aaaaaaaa-0000-0000-0000-000000000001}.xml" <<'XML'
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
	<name>HomeNet</name>
	<SSIDConfig>
		<SSID>
			<hex>486F6D654E6574</hex>
			<name>HomeNet</name>
		</SSID>
	</SSIDConfig>
	<connectionType>ESS</connectionType>
	<connectionMode>auto</connectionMode>
	<MSM>
		<security>
			<authEncryption>
				<authentication>WPA2PSK</authentication>
				<encryption>AES</encryption>
				<useOneX>false</useOneX>
			</authEncryption>
			<sharedKey>
				<keyType>passPhrase</keyType>
				<protected>true</protected>
				<keyMaterial>01000000D08C9DDF0115D1118C7A00C04FC297EB0100000042DEADBEEF</keyMaterial>
			</sharedKey>
		</security>
	</MSM>
</WLANProfile>
XML
cat > "$W/{aaaaaaaa-0000-0000-0000-000000000002}.xml" <<'XML'
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
	<name>CoffeeShop</name>
	<SSIDConfig>
		<SSID><hex>436F6666656553686F70</hex><name>CoffeeShop</name></SSID>
		<nonBroadcast>false</nonBroadcast>
	</SSIDConfig>
	<connectionType>ESS</connectionType>
	<connectionMode>manual</connectionMode>
	<MSM>
		<security>
			<authEncryption>
				<authentication>open</authentication>
				<encryption>none</encryption>
				<useOneX>false</useOneX>
			</authEncryption>
		</security>
	</MSM>
</WLANProfile>
XML
cat > "$W/{aaaaaaaa-0000-0000-0000-000000000003}.xml" <<'XML'
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
	<name>eduroam</name>
	<SSIDConfig>
		<SSID><hex>656475726F616D</hex><name>eduroam</name></SSID>
	</SSIDConfig>
	<connectionType>ESS</connectionType>
	<connectionMode>auto</connectionMode>
	<MSM>
		<security>
			<authEncryption>
				<authentication>WPA2</authentication>
				<encryption>AES</encryption>
				<useOneX>true</useOneX>
			</authEncryption>
			<OneX xmlns="http://www.microsoft.com/networking/OneX/v1">
				<EAPConfig>PEVhcEhvc3RDb25maWc+</EAPConfig>
			</OneX>
		</security>
	</MSM>
</WLANProfile>
XML
cat > "$W/{aaaaaaaa-0000-0000-0000-000000000004}.xml" <<'XML'
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
	<name>Phone Hotspot</name>
	<SSIDConfig>
		<SSID><hex>50686F6E6520486F7473706F74</hex><name>Phone Hotspot</name></SSID>
		<nonBroadcast>true</nonBroadcast>
	</SSIDConfig>
	<connectionType>ESS</connectionType>
	<connectionMode>auto</connectionMode>
	<MSM>
		<security>
			<authEncryption>
				<authentication>WPA3SAE</authentication>
				<encryption>AES</encryption>
				<useOneX>false</useOneX>
			</authEncryption>
			<sharedKey>
				<keyType>passPhrase</keyType>
				<protected>false</protected>
				<keyMaterial>hunter2hunter2</keyMaterial>
			</sharedKey>
		</security>
	</MSM>
</WLANProfile>
XML

# ── Volume-level files. A zeroed hiberfil is the "hibernation enabled
#    but not used" case and must NOT trip the refusal; --hibernated
#    writes a real image signature, which must.
python3 - "$ROOT" "$HIB" <<'PY'
import os, sys
root, hib = sys.argv[1], sys.argv[2]
with open(os.path.join(root, "pagefile.sys"), "wb") as f:
    f.truncate(1 << 20)
with open(os.path.join(root, "hiberfil.sys"), "wb") as f:
    f.write(b"HIBR" if hib == "--hibernated" else b"\0\0\0\0")
    f.truncate(1 << 20)
PY

printf '%s\n' "$ROOT"
