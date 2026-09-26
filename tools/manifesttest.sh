#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  manifesttest — will Windows start the wizard at all?
#
#  The first real Windows PC answered "no": "The application has failed
#  to start because its side-by-side configuration is incorrect." The
#  manifest linked into the wizard had "--" inside an XML comment. XML
#  forbids that, Windows' manifest parser enforces it, and Windows then
#  refuses to create the process -- before a line of AurBridge runs.
#  Wine loaded the same file happily, so every test was green.
#
#  This proves the gate that now stands in the build, including that it
#  goes red: a manifest with "--" in a comment, one with no manifest at
#  all, one that forgot requireAdministrator, and the build itself
#  refusing a broken manifest end to end.
#
#    sh tools/manifesttest.sh [BUILT-WIZARD.exe] [KNOWN-BAD.exe]
#
#  With a built wizard it also checks the manifest inside it, and that
#  it is the source manifest byte for byte. A known-bad .exe (the test
#  build published on 2026-09-25, SHA-256 3e32e929...) must be refused.
#
#  It cannot prove that Windows loads the file: only Windows can.
#  .github/workflows/windows.yml is where that is asked.
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-60s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-60s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/manifesttest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
CHECK="python3 tools/pe_payload.py check"
MAN=src/aurbridge/aurbridge.manifest

echo
echo "Will Windows start the wizard at all?"
echo

# ── the source ──────────────────────────────────────────────────────
if $CHECK "$MAN" >/dev/null 2>&1; then ok "the source manifest is one Windows loads"
else bad "the source manifest is one Windows loads" "$($CHECK "$MAN" 2>&1)"; fi

if grep -q -- '--' "$MAN" && ! grep -q -- '<!--' "$MAN"; then
    bad "no '--' anywhere in the manifest" "$(grep -n -- '--' "$MAN")"
elif grep -q -- '--' "$MAN"; then
    bad "no comments in the manifest (they ship, and bite)" "$(grep -n -- '--' "$MAN")"
else ok "no comments and no '--' in the manifest"; fi

# ── the gate goes red ───────────────────────────────────────────────
# The exact shape that broke: "--" inside a comment.
sed '1a <!-- right-click and choose Run as administrator -- a step -->' "$MAN" > "$TMP/dash.xml"
if $CHECK "$TMP/dash.xml" >/dev/null 2>&1; then
    bad "refuses '--' inside a comment (the real failure)" "the check passed it"
else ok "refuses '--' inside a comment (the real failure)"; fi

sed 's/requireAdministrator/asInvoker/' "$MAN" > "$TMP/invoker.xml"
if $CHECK "$TMP/invoker.xml" >/dev/null 2>&1; then
    bad "refuses a manifest without requireAdministrator" "the check passed it"
else ok "refuses a manifest without requireAdministrator"; fi

head -n 5 "$MAN" > "$TMP/cut.xml"
if $CHECK "$TMP/cut.xml" >/dev/null 2>&1; then
    bad "refuses a truncated manifest" "the check passed it"
else ok "refuses a truncated manifest"; fi

# The build refuses too: a copy of the tree with the broken manifest.
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    mkdir -p "$TMP/tree/src" "$TMP/tree/tools"
    cp -r build "$TMP/tree/"
    cp -r src/aurbridge "$TMP/tree/src/"
    cp tools/pe_payload.py "$TMP/tree/tools/"
    cp "$TMP/dash.xml" "$TMP/tree/src/aurbridge/aurbridge.manifest"
    if OUT="$TMP/tree/out" sh "$TMP/tree/build/aurbridge" > "$TMP/build.log" 2>&1; then
        bad "build/aurbridge refuses a manifest Windows would refuse" \
            "the build finished"
    elif grep -q "manifest is one Windows would refuse" "$TMP/build.log"; then
        ok "build/aurbridge refuses a manifest Windows would refuse"
    else
        bad "build/aurbridge refuses a manifest Windows would refuse" \
            "it failed, but not at the manifest:" "$(tail -n 3 "$TMP/build.log")"
    fi
else
    echo "    (no mingw-w64: the end-to-end build refusal was not run)"
fi

# ── inside a built wizard ───────────────────────────────────────────
if [ -n "${1:-}" ]; then
    if $CHECK "$1" >/dev/null 2>&1; then ok "the manifest inside $(basename "$1") loads"
    else bad "the manifest inside $(basename "$1") loads" "$($CHECK "$1" 2>&1)"; fi
    python3 tools/pe_payload.py manifest "$1" > "$TMP/inside.xml" 2>/dev/null
    if cmp -s "$TMP/inside.xml" "$MAN"; then ok "...and is the source manifest, byte for byte"
    else bad "...and is the source manifest, byte for byte" \
             "$(wc -c < "$TMP/inside.xml") bytes inside, $(wc -c < "$MAN") in the source"; fi

    # A resource directory the check must refuse: the same program with
    # a '--' written into its manifest, the size unchanged.
    python3 - "$1" "$TMP/dashed.exe" <<'EOF'
import sys
sys.path.insert(0, 'tools')
import pe_payload
pe = pe_payload.PE(sys.argv[1])
off, ln = pe.resources(pe_payload.RT_MANIFEST)[1]
b = bytearray(pe.b)
m = bytes(b[off:off + ln])
i = m.index(b'<description>')
bad = b'<!-- a -- b -->'
b[off + i:off + i + len(bad)] = bad
open(sys.argv[2], 'wb').write(b)
EOF
    if $CHECK "$TMP/dashed.exe" >/dev/null 2>&1; then
        bad "refuses the same .exe with '--' written into it" "the check passed it"
    else ok "refuses the same .exe with '--' written into it"; fi
fi
if [ -n "${2:-}" ]; then
    if $CHECK "$2" >/dev/null 2>&1; then
        bad "refuses the known-bad $(basename "$2")" "the check passed it"
    else ok "refuses the known-bad $(basename "$2")"; fi
fi

echo
if [ "$fail" -eq 0 ]; then echo "  $checked checks, all ok"
else echo "  $checked checks, $fail FAILED"; fi
exit "$fail"
