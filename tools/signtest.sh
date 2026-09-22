#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  signtest — is the thing a stranger double-clicks actually signed?
#
#  The certificate is a purchase and cannot be in a test. Everything
#  else about signing can be, and the parts that go wrong are not the
#  parts people expect:
#
#    - a signature that was never verified after being applied. The
#      first person to find out it did not take is somebody looking at
#      a blue full-screen SmartScreen panel.
#    - signing twice, which appends rather than replaces and produces
#      a binary some verifiers refuse.
#    - SHA-1, which Windows has refused since 2016 and which is still
#      what a tool's default was in some versions.
#    - a signature that survives a byte being changed afterwards --
#      which would mean it is not a signature.
#    - a release build that shipped unsigned because the key was not
#      configured and nothing checked.
#
#  A throwaway CA is made here, so the chain is real and ours.
#
#    sh tools/signtest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

command -v osslsigncode >/dev/null 2>&1 || { echo "need osslsigncode"; exit 2; }
command -v openssl      >/dev/null 2>&1 || { echo "need openssl"; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/signtest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

echo
echo "Is the thing a stranger double-clicks actually signed?"
echo

# ── a certificate of our own, so the chain is real ──────────────────
openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
    -subj "/CN=AurOS Test CA" -keyout "$TMP/ca.key" -out "$TMP/ca.pem" \
    >/dev/null 2>&1 || { echo "  could not make a CA"; exit 2; }
openssl req -newkey rsa:2048 -nodes \
    -subj "/CN=AurOS Test Signer" -keyout "$TMP/code.key" -out "$TMP/code.csr" \
    >/dev/null 2>&1
cat > "$TMP/ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature
extendedKeyUsage=codeSigning
EOF
openssl x509 -req -in "$TMP/code.csr" -CA "$TMP/ca.pem" -CAkey "$TMP/ca.key" \
    -CAcreateserial -days 2 -extfile "$TMP/ext" -out "$TMP/code.pem" \
    >/dev/null 2>&1 || { echo "  could not make a signing certificate"; exit 2; }

# ── something to sign ───────────────────────────────────────────────
#
# The real binary if this machine can build one, and a minimal PE if it
# cannot -- because what is being tested is the pipeline, and a build
# host with no mingw should still be able to find out that the pipeline
# is broken.
EXE=""
if [ -f out/aurbridge-wizard.exe ]; then
    EXE="$TMP/subject.exe"; cp out/aurbridge-wizard.exe "$EXE"
elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    printf 'int main(void){return 0;}\n' > "$TMP/t.c"
    x86_64-w64-mingw32-gcc -O0 -o "$TMP/subject.exe" "$TMP/t.c" 2>/dev/null \
        && EXE="$TMP/subject.exe"
fi
[ -n "$EXE" ] || { echo "  nothing to sign here (no out/*.exe and no mingw)"; exit 2; }
cp "$EXE" "$TMP/pristine.exe"
echo "  signing $(basename "$EXE"), $(du -h "$EXE" | cut -f1)"
echo

export AUROS_SIGN_CERT="$TMP/code.pem"
export AUROS_SIGN_KEY="$TMP/code.key"
export AUROS_SIGN_CAFILE="$TMP/ca.pem"
# No network to a timestamp server from a test machine, so the test
# signs without one -- and checks that doing so is loud.
export AUROS_SIGN_NO_TS=1

echo "  before anything is signed"
sh build/sign --check "$EXE" >/dev/null 2>&1 \
  && bad "an unsigned binary is reported as unsigned" "it said it was signed" \
  || ok "an unsigned binary is reported as unsigned"

sh build/sign --configured && ok "it knows it has something to sign with" \
                           || bad "it knows it has something to sign with"
( unset AUROS_SIGN_CERT AUROS_SIGN_KEY; sh build/sign --configured ) \
  && bad "and knows when it has not" "it claimed a key it does not have" \
  || ok "and knows when it has not"

echo
echo "  signing it"
if sh build/sign "$EXE" >"$TMP/out.txt" 2>&1; then
    ok "it signs"
else
    bad "it signs" "$(tail -6 "$TMP/out.txt")"
fi
sh build/sign --check "$EXE" >/dev/null 2>&1 \
  && ok "and the signature verifies against our own CA" \
  || bad "and the signature verifies against our own CA" \
         "$(osslsigncode verify -CAfile "$TMP/ca.pem" -ignore-timestamp \
            -in "$EXE" 2>&1 | tail -6)"

grep -qi 'NOT timestamped' "$TMP/out.txt" \
  && ok "and says out loud that it was not timestamped" \
  || bad "and says out loud that it was not timestamped" "$(tail -4 "$TMP/out.txt")"

# SHA-1 HAS BEEN REFUSED BY WINDOWS SINCE 2016, and a tool's default is
# not a promise.
osslsigncode verify -CAfile "$TMP/ca.pem" -ignore-timestamp -in "$EXE" 2>&1 \
  | grep -qi 'sha256\|SHA-256' \
  && ok "with a SHA-256 digest, not SHA-1" \
  || bad "with a SHA-256 digest, not SHA-1" \
         "$(osslsigncode verify -CAfile "$TMP/ca.pem" -ignore-timestamp -in "$EXE" 2>&1 | grep -i 'message digest\|algorithm' | head -3)"

# THE PROGRAM MUST STILL BE THE PROGRAM. A signature is appended to a
# PE's certificate table; getting that wrong produces a file Windows
# refuses to run at all, which is worse than an unsigned one.
cmp -s "$TMP/pristine.exe" "$EXE" \
  && bad "the file changed, so something was actually added" "it is byte-identical" \
  || ok "the file changed, so something was actually added"
head -c 2 "$EXE" | grep -q 'MZ' \
  && ok "and it is still a Windows program" \
  || bad "and it is still a Windows program"

echo
echo "  and the things that would make it worthless"
# Signing twice must REPLACE, not stack.
sh build/sign "$EXE" >/dev/null 2>&1
N=$(osslsigncode verify -CAfile "$TMP/ca.pem" -ignore-timestamp -in "$EXE" 2>&1 \
    | grep -ci 'Signer #\|Number of signers' || true)
sh build/sign --check "$EXE" >/dev/null 2>&1 \
  && ok "signing twice leaves a binary that still verifies" \
  || bad "signing twice leaves a binary that still verifies" "signers: $N"

# A byte changed after signing must break it. If it does not, the
# signature is not covering the program.
cp "$EXE" "$TMP/tampered.exe"
python3 - "$TMP/tampered.exe" <<'EOPY'
import sys
f = open(sys.argv[1], 'r+b')
# 4 KiB in: inside the code, never inside the PE header or the
# certificate table at the end.
f.seek(4096); b = f.read(1)
f.seek(4096); f.write(bytes([b[0] ^ 0xFF]))
f.close()
EOPY
sh build/sign --check "$TMP/tampered.exe" >/dev/null 2>&1 \
  && bad "a byte changed afterwards breaks the signature" "it still verified" \
  || ok "a byte changed afterwards breaks the signature"

echo
echo "  and a release that has nothing to sign with"
OUT=$( unset AUROS_SIGN_CERT AUROS_SIGN_KEY
       AUROS_RELEASE=1 sh build/aurbridge 2>&1 || true )
case "$OUT" in
  *"not signed"*|*"nothing to sign with"*|*"refusing to publish"*)
    ok "a release build refuses to publish an unsigned installer" ;;
  *)
    bad "a release build refuses to publish an unsigned installer" \
        "$(printf '%s' "$OUT" | tail -4)" ;;
esac

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "An unsigned installer is a full-screen warning whose only visible"
    echo "button says Don't run."
    exit 1
fi
echo "$checked checks: it signs, it verifies what it signed, a changed byte"
echo "breaks it, and a release with no key does not get built."
exit 0
