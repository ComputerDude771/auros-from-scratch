#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  Ferry offline test suite.
#
#  Ferry cannot be tested against a real Windows disk in CI, so the
#  suite builds a synthetic one (tests/mkfakewin.sh) with every trap the
#  real world contains — a Documents folder redirected into OneDrive,
#  cloud placeholders mixed with real files, a Videos folder on a second
#  drive, a wrong-case Users directory, a DPAPI-protected WiFi PSK, a
#  destination collision — and asserts Ferry does the right, honest
#  thing with each. No network, no NTFS mount, no root beyond writing
#  the xattrs the fake tree uses to imitate NTFS attributes.
#
#    sh tests/run.sh
# ═══════════════════════════════════════════════════════════════════
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
FERRY=$(dirname "$HERE")
export PATH="$FERRY:$FERRY/build:$PATH"
export FERRY_HIVE_BACKEND=hivepeek
export NO_COLOR=1

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ferrytest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
WIN="$TMP/win"
export FERRY_STATE="$TMP/state"

pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad() { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; }
# assert LABEL: runs the rest as a command; pass iff it exits 0.
assert() { _l=$1; shift; if "$@" >/dev/null 2>&1; then ok "$_l"; else bad "$_l"; fi; }
refute() { _l=$1; shift; if "$@" >/dev/null 2>&1; then bad "$_l"; else ok "$_l"; fi; }
eq()     { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

echo "building fake Windows volume..."
sh "$HERE/mkfakewin.sh" "$WIN" >/dev/null
NT="$WIN/users/Bob/NTUSER.DAT"

# Snapshot the source volume's every file (path, size, mtime) BEFORE any
# stage touches it, so the read-only guarantee is checked against the
# tree as Ferry first saw it — not against the source-file timestamps,
# which say nothing about whether Ferry wrote to the volume.
snapshot() { find "$WIN" -type f -exec stat -c '%n %s %Y' {} + 2>/dev/null | sort; }
snapshot > "$TMP/win.before"

echo "hive reader:"
eq "reads REG_EXPAND_SZ" \
   "$(ferry-hivepeek "$NT" 'Control Panel\Desktop' Wallpaper)" \
   'C:\Users\Bob\Pictures\wall.jpg'
eq "reads REG_DWORD as number" \
   "$(ferry-hivepeek "$NT" 'Control Panel\Desktop' LogPixels)" 120
eq "value lookup is case-insensitive" \
   "$(ferry-hivepeek "$NT" 'control panel\desktop' wallpaperstyle)" 10
refute "missing key exits nonzero" ferry-hivepeek "$NT" 'No\Such\Key' X
assert "lists subkeys" sh -c "ferry-hivepeek -k '$NT' 'Control Panel' | grep -qx Desktop"

echo "attribute classifier:"
STUB="$WIN/users/Bob/OneDrive/Documents/Taxes/2022.pdf"
REAL="$WIN/users/Bob/Desktop/notes.txt"
CLASS_STUB=$(ferry-winattr "$STUB" | cut -f1)
CLASS_REAL=$(ferry-winattr "$REAL" | cut -f1)
case "$CLASS_STUB" in
    placeholder|suspect) ok "cloud placeholder is not classified as real data ($CLASS_STUB)" ;;
    *) bad "cloud placeholder misclassified as '$CLASS_STUB' — DATA LOSS RISK" ;;
esac
eq "a real file is hydrated" "$CLASS_REAL" hydrated
refute "a missing path exits nonzero" ferry-winattr "$TMP/nope"

echo "detect:"
ferry-detect --root "$WIN" --quick > "$TMP/detect.json" 2>/dev/null
assert "detect emits valid JSON" python3 -m json.tool "$TMP/detect.json"
assert "detect identifies Windows 10" \
    grep -q '"product": "Windows 10 Pro"' "$TMP/detect.json"
assert "detect counts 4 WiFi profiles" \
    grep -q '"wifi_profiles": 4' "$TMP/detect.json"
# The redirection trap: Documents must resolve into OneDrive, never the
# stale local Users\Bob\Documents.
DOCPATH=$(python3 - "$TMP/detect.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for p in d["installs"][0]["profiles"]:
    if p["kind"] != "user": continue
    for f in p["folders"]:
        if f["label"] == "Documents": print(f["path"]); break
PY
)
case "$DOCPATH" in
    *OneDrive*) ok "Documents resolved into OneDrive (redirection honoured)" ;;
    *) bad "Documents resolved to '$DOCPATH' — redirection NOT honoured" ;;
esac
VIDSTATUS=$(python3 - "$TMP/detect.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for p in d["installs"][0]["profiles"]:
    if p["kind"] != "user": continue
    for f in p["folders"]:
        if f["label"] == "Videos": print(f["status"]); break
PY
)
eq "Videos on drive D: reported off-volume" "$VIDSTATUS" off-volume

echo "files:"
HOME1="$TMP/home"
mkdir -p "$HOME1/Desktop"
echo "my own newer notes" > "$HOME1/Desktop/notes.txt"   # collision seed
ferry-files --root "$WIN" --home "$HOME1" --owner "$(id -un)" >/dev/null 2>&1
assert "real Desktop file imported" test -f "$HOME1/Desktop/report.docx"
assert "collision kept BOTH files (no overwrite)" \
    test -f "$HOME1/Desktop/notes (from Windows).txt"
eq "the pre-existing file was not overwritten" \
   "$(cat "$HOME1/Desktop/notes.txt")" "my own newer notes"
assert "Documents imported from the OneDrive redirect" \
    test -f "$HOME1/Documents/letter.docx"
refute "a OneDrive placeholder was NOT silently copied" \
    test -f "$HOME1/Documents/Taxes/2022.pdf"
assert "each placeholder is reported by name" \
    grep -q 'Taxes/2022.pdf' "$FERRY_STATE/report.d/files.tsv"
assert "junk (desktop.ini/Thumbs.db) filtered out" \
    sh -c "! find '$HOME1' -iname 'thumbs.db' -o -iname 'desktop.ini' | grep -q ."
assert "user-dirs.dirs written" test -f "$HOME1/.config/user-dirs.dirs"
# idempotency: re-run copies nothing new
BEFORE=$(find "$HOME1" -type f | wc -l)
ferry-files --root "$WIN" --home "$HOME1" --owner "$(id -un)" >/dev/null 2>&1
AFTER=$(find "$HOME1" -type f | wc -l)
eq "re-run is idempotent (no new files)" "$AFTER" "$BEFORE"

echo "firefox:"
ferry-firefox --root "$WIN" --home "$HOME1" --owner "$(id -un)" >/dev/null 2>&1
FP="$HOME1/.mozilla/firefox/Profiles/a1b2c3d4.default-release"
assert "places.sqlite (bookmarks/history) copied" test -f "$FP/places.sqlite"
assert "prefs.js (settings) copied" test -f "$FP/prefs.js"
refute "key4.db (the password key) NOT copied" test -e "$FP/key4.db"
refute "logins.json (saved passwords) NOT copied" test -e "$FP/logins.json"
refute "a password file in odd case NOT copied" test -e "$FP/LOGINS-BACKUP.JSON"
assert "the report says passwords were left on Windows" \
    grep -q 'firefox/passwords' "$FERRY_STATE/report.d/firefox.tsv"
assert "profiles.ini copied" test -f "$HOME1/.mozilla/firefox/profiles.ini"
refute "installs.ini (claims the profile for Windows) NOT copied" \
    test -e "$HOME1/.mozilla/firefox/installs.ini"
refute "cache NOT copied" test -e "$FP/cache2"
refute "stale lock NOT copied" test -e "$FP/.parentlock"
refute "compatibility.ini NOT copied" test -e "$FP/compatibility.ini"

echo "wifi (never a password):"
NMDIR="$TMP/nm"
# A helper planted where the old decryption call-out looked. It must
# never run: Ferry does not decrypt saved passwords.
printf '#!/bin/sh\ntouch "%s"\necho FilledSecret\n' "$TMP/helper-ran" > "$TMP/helper"
chmod +x "$TMP/helper"
FERRY_DPAPI_HELPER="$TMP/helper" \
    ferry-wifi --root "$WIN" --to "$NMDIR" --no-reload >/dev/null 2>&1
eq "wifi stage succeeds" "$?" 0
refute "no decryption helper was ever run" test -e "$TMP/helper-ran"
refute "no network file written at all" \
    sh -c "find '$NMDIR' /etc/NetworkManager/system-connections -name '*.nmconnection' -newer '$TMP/helper' 2>/dev/null | grep -q ."
assert "secured networks listed by name" \
    sh -c "grep -q 'HomeNet' '$FERRY_STATE/report.d/wifi.tsv' && grep -q 'Phone Hotspot' '$FERRY_STATE/report.d/wifi.tsv'"
assert "open network listed" grep -q 'CoffeeShop' "$FERRY_STATE/report.d/wifi.tsv"
assert "enterprise network listed" grep -q 'eduroam' "$FERRY_STATE/report.d/wifi.tsv"
refute "no Wi-Fi password in the report" \
    grep -rq 'hunter2hunter2' "$FERRY_STATE"
# An SSID only in <hex> (no <name>): mawk has no strtonum, the image's
# awk is mawk, so the decode must not need it.
W2="$TMP/w2"; mkdir -p "$W2/Windows/System32" \
    "$W2/ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/{x}"
printf '<WLANProfile><SSIDConfig><SSID><hex>4F6C6473</hex></SSID></SSIDConfig><MSM><security><authEncryption><authentication>WPA2PSK</authentication></authEncryption></security></MSM></WLANProfile>' \
    > "$W2/ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/{x}/a.xml"
FERRY_STATE="$TMP/state-w2" ferry-wifi --root "$W2" >/dev/null 2>&1
assert "a hex-only SSID is decoded with the image's awk" \
    grep -q 'Olds' "$TMP/state-w2/report.d/wifi.tsv"

echo "settings (timezone mapping is the point):"
rm -rf "$TMP/state3"
TZ=$(FERRY_STATE="$TMP/state3" ferry-settings --root "$WIN" --home "$TMP/h3" \
       --owner "$(id -un)" --only timezone --dry-run 2>/dev/null; \
     grep -h timezone "$TMP/state3/report.d/settings.tsv" 2>/dev/null)
case "$TZ" in
    *Europe/London*) ok "GMT Standard Time -> Europe/London (CLDR mapping)" ;;
    *) bad "timezone mapping failed: $TZ" ;;
esac

# WHAT SHE CHOSE IN THE INSTALLER WINS. First boot records it in
# /etc/auros/installer-choices.conf (rootfs/usr/lib/auros/choices.sh);
# the Windows value is still suggested, but not applied over it.
rm -rf "$TMP/state3b"
printf 'timezone=America/New_York\n' > "$TMP/choices.conf"
OUT=$(FERRY_CHOICES="$TMP/choices.conf" FERRY_STATE="$TMP/state3b" \
      ferry-settings --root "$WIN" --home "$TMP/h3b" --owner "$(id -un)" \
      --only timezone --dry-run 2>&1)
case "$OUT" in
    *"kept as chosen in the installer"*) ok "a time zone chosen in the installer is not overwritten" ;;
    *) bad "installer choice overwritten by the Windows timezone: $OUT" ;;
esac

echo "safety gate:"
sh "$HERE/mkfakewin.sh" "$TMP/hib" --hibernated >/dev/null
rm -rf "$TMP/state4"
FERRY_STATE="$TMP/state4" ferry-files --root "$TMP/hib" --home "$TMP/h4" \
    --owner "$(id -un)" >/dev/null 2>&1
eq "hibernated volume is refused (exit 3)" "$?" 3
FERRY_STATE="$TMP/state4b" ferry-detect --root "$TMP/hib" --quick >/dev/null 2>&1
eq "detect still succeeds on a hibernated volume" "$?" 0

echo "no saved password anywhere on the AurOS side:"
refute "no Wi-Fi key, no Firefox login, anywhere under the new home or state" \
    sh -c "grep -rqs -e hunter2hunter2 -e bank.example '$HOME1' '$FERRY_STATE' '$TMP/nm'"
refute "no password file anywhere under the new home" \
    sh -c "find '$HOME1' -iname 'logins*.json' -o -iname 'key[34].db' -o -iname 'signons*' | grep -q ."

echo "install list:"
for _f in $(sh "$FERRY/install.sh" list); do
    [ -f "$FERRY/$_f" ] || bad "install.sh names $_f, which does not exist"
done
_missing=""
for _f in "$FERRY"/ferry*; do
    _b=$(basename "$_f")
    sh "$FERRY/install.sh" list | grep -qx "$_b" || _missing="$_missing $_b"
done
eq "every ferry tool in the tree is installed by install.sh" "$_missing" ""

echo "read-only guarantee:"
# Every import stage above ran against $WIN. Not one byte, size or mtime
# on it may have changed: Ferry mounts read-only and must never write.
snapshot > "$TMP/win.after"
if diff -q "$TMP/win.before" "$TMP/win.after" >/dev/null 2>&1; then
    ok "source Windows tree left byte-for-byte unchanged by every stage"
else
    bad "source Windows tree was modified:"
    diff "$TMP/win.before" "$TMP/win.after" | head
fi

echo
printf 'ferry tests: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
