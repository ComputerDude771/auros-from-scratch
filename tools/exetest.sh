#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  exetest — one file somebody double-clicks
#
#  The product is a download. For a long time it was three downloads
#  that had to end up in the same folder, and the message a person got
#  for not knowing that was "the copy of AurOS to install could not be
#  found" -- a true sentence about a mistake we made.
#
#  Two things now make it one file, and both of them are the kind that
#  fail silently:
#
#    the staging environment lives INSIDE the executable, as a PE
#    resource. Whether it is really in there is a question about the
#    resource directory, and it is answered here by a second
#    implementation in python that does not ask the program that wrote
#    it. A resource that is present and truncated boots nothing.
#
#    the image is fetched, and RESUMED. What goes wrong with a resume
#    is not the resume: it is a server that ignores the Range header,
#    answers 200 with the whole file where a 206 was asked for, and
#    gets its first byte written four gigabytes into the file. That
#    server is every misconfigured CDN edge, and the failure it makes
#    is a five gigabyte download that hashes wrong after forty minutes
#    with nothing to say about why.
#
#    sh tools/exetest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-58s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-58s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/exetest.XXXXXX")
trap 'kill %1 2>/dev/null; rm -rf "$TMP"' EXIT

echo
echo "Is it one file, and does it carry what it says it does?"
echo

# ── what the executable carries ─────────────────────────────────────
EXE=out/aurbridge-wizard.exe
if [ -f "$EXE" ] && [ -f out/auros-staging-vmlinuz ] && [ -f out/auros-staging.img ]; then
    echo "  what the installer carries"
    python3 tools/pe_payload.py list "$EXE" > "$TMP/res.txt" 2>/dev/null
    grep -q '^1 ' "$TMP/res.txt" && grep -q '^2 ' "$TMP/res.txt" \
      && ok "both halves of the staging environment are in the .exe" \
      || bad "both halves of the staging environment are in the .exe" \
             "$(cat "$TMP/res.txt")"

    # BYTE FOR BYTE. A resource of the right size that is not the right
    # bytes is an installer that arranges a restart into nothing.
    for pair in "1 out/auros-staging-vmlinuz kernel" \
                "2 out/auros-staging.img initramfs"; do
        set -- $pair
        python3 tools/pe_payload.py get "$EXE" "$1" "$TMP/got.$1" 2>/dev/null
        if cmp -s "$TMP/got.$1" "$2"; then
            ok "the $3 in it is the one this build made"
        else
            bad "the $3 in it is the one this build made" \
                "$(ls -l "$TMP/got.$1" "$2" 2>&1 | tr '\n' ' ')"
        fi
    done

    head -c 2 "$EXE" | grep -q MZ \
      && ok "and it is still a Windows program" \
      || bad "and it is still a Windows program"

    # THE TWO MUST NOT FIGHT. A payload inside the image and a
    # signature at the end of it are the reason the payload is a
    # resource rather than bytes appended to the file, and that is
    # only true if it survives being signed.
    if command -v osslsigncode >/dev/null 2>&1 && command -v openssl >/dev/null 2>&1; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
            -subj "/CN=AurOS Test CA" -keyout "$TMP/ca.key" -out "$TMP/ca.pem" \
            >/dev/null 2>&1
        cp "$EXE" "$TMP/signed.exe"
        AUROS_SIGN_CERT="$TMP/ca.pem" AUROS_SIGN_KEY="$TMP/ca.key" \
        AUROS_SIGN_CAFILE="$TMP/ca.pem" AUROS_SIGN_NO_TS=1 \
            sh build/sign "$TMP/signed.exe" >/dev/null 2>&1
        python3 tools/pe_payload.py get "$TMP/signed.exe" 2 "$TMP/after.img" 2>/dev/null
        cmp -s "$TMP/after.img" out/auros-staging.img \
          && ok "and signing it does not disturb what it carries" \
          || bad "and signing it does not disturb what it carries"
    else
        echo "    (no osslsigncode; the signing interaction was not checked)"
    fi
else
    echo "  (no out/aurbridge-wizard.exe with a staging environment; skipped)"
fi

# ── the release gates ───────────────────────────────────────────────
echo
echo "  and what a release is not allowed to be"
O=$( AUROS_RELEASE=1 AUROS_STAGING_KERNEL=/nonexistent \
     AUROS_STAGING_INITRD=/nonexistent sh build/aurbridge 2>&1 || true )
case "$O" in
  *"carries no staging"*)
    ok "a release with nothing inside it is refused" ;;
  *) bad "a release with nothing inside it is refused" \
         "$(printf '%s' "$O" | tail -3)" ;;
esac
O=$( AUROS_RELEASE=1 sh build/aurbridge 2>&1 || true )
case "$O" in
  *"no AUROS_IMAGE_URL"*)
    ok "a release that cannot say where the image is, too" ;;
  *) bad "a release that cannot say where the image is, too" \
         "$(printf '%s' "$O" | tail -3)" ;;
esac
O=$( AUROS_RELEASE=1 AUROS_IMAGE_URL=http://x/y.img sh build/aurbridge 2>&1 || true )
case "$O" in
  *"AUROS_IMAGE_SHA256 is not"*)
    ok "and one that cannot say what it should be" ;;
  *) bad "and one that cannot say what it should be" \
         "$(printf '%s' "$O" | tail -3)" ;;
esac

# ── the download ────────────────────────────────────────────────────
[ -x out/aurbridge-sim ] || { echo; echo "  (no simulator; the download was not tested)";
                             echo; echo "  $checked checked, $fail failed"
                             [ "$fail" -eq 0 ] || exit 1; exit 0; }

echo
echo "  the download, against a server that answers Range"
dd if=/dev/urandom of="$TMP/payload.bin" bs=1k count=900 status=none
WANT=$(sha256sum "$TMP/payload.bin" | cut -d' ' -f1)

# A server small enough to read, because the thing being tested is how
# the client behaves when a server misbehaves -- so the misbehaviour
# has to be ours to choose.
cat > "$TMP/srv.py" <<'EOPY'
import http.server, os, re, socketserver, sys, threading
FILE, MODE = sys.argv[1], sys.argv[2]
BODY = open(FILE, 'rb').read()

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        rng = self.headers.get('Range')
        start = 0
        if rng:
            m = re.match(r'bytes=(\d+)-', rng)
            if m: start = int(m.group(1))
        if MODE == 'ignore-range' or not rng or start == 0:
            # A server that answers 200 where a 206 was asked for.
            body = BODY if MODE != 'ignore-range' or start == 0 else BODY
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if MODE == 'cut':
            body = BODY[start:start + 100 * 1024]
        else:
            body = BODY[start:]
        self.send_response(206)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Content-Range',
                         'bytes %d-%d/%d' % (start, start + len(body) - 1, len(BODY)))
        self.end_headers()
        self.wfile.write(body)

class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
# PORT ZERO, AND THE ANSWER WRITTEN DOWN. A hardcoded port is a
# test that fails because a previous run of itself leaked a server,
# which is what happened the first time this was written and cost
# twenty minutes of looking at the client instead.
srv = S(('127.0.0.1', 0), H)
# A FILE, NOT A PORT PROBE. The caller is /bin/sh, which has no
# /dev/tcp -- that is a bash feature, and reaching for it made this
# whole section report "the test server would not start" while the
# server was running perfectly.
open(sys.argv[3], 'w').write('%d\n' % srv.server_address[1])
srv.serve_forever()
EOPY

PORT=""
serve() { # mode   -> sets PORT
    rm -f "$TMP/ready"
    python3 "$TMP/srv.py" "$TMP/payload.bin" "$1" "$TMP/ready" \
        >"$TMP/srv.log" 2>&1 &
    SRV=$!
    i=0
    while [ $i -lt 100 ]; do
        if [ -s "$TMP/ready" ]; then PORT=$(cat "$TMP/ready"); return 0; fi
        i=$((i+1)); sleep 0.1
    done
    return 1
}
stopserve() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }

if serve whole; then
    rm -f "$TMP/dl.bin"
    out/aurbridge-sim fetch http://127.0.0.1:$PORT/x.img "$TMP/dl.bin" >/dev/null 2>&1
    [ "$(sha256sum "$TMP/dl.bin" | cut -d' ' -f1)" = "$WANT" ] \
      && ok "a whole download arrives intact" \
      || bad "a whole download arrives intact"
    stopserve
else
    bad "a whole download arrives intact" "the test server would not start" \
        "$(head -5 "$TMP/srv.log" 2>/dev/null)"
fi

# A HALF-DOWNLOADED FILE IS CONTINUED, NOT RESTARTED. This is the
# difference between somebody finishing and somebody giving up.
if serve whole; then
    head -c 400000 "$TMP/payload.bin" > "$TMP/part.bin"
    out/aurbridge-sim fetch http://127.0.0.1:$PORT/x.img "$TMP/part.bin" >/dev/null 2>&1
    [ "$(sha256sum "$TMP/part.bin" | cut -d' ' -f1)" = "$WANT" ] \
      && ok "a half-finished one is continued and comes out right" \
      || bad "a half-finished one is continued and comes out right" \
             "$(ls -l "$TMP/part.bin")"
    stopserve
fi

echo
echo "  and against one that ignores it"
if serve ignore-range; then
    head -c 400000 "$TMP/payload.bin" > "$TMP/ign.bin"
    out/aurbridge-sim fetch http://127.0.0.1:$PORT/x.img "$TMP/ign.bin" >/dev/null 2>&1
    # The right behaviour is to notice the 200 and start the file
    # again, so the result is still correct. The wrong behaviour --
    # appending the whole body at 400000 -- gives a file that is too
    # long and hashes wrong.
    if [ "$(sha256sum "$TMP/ign.bin" | cut -d' ' -f1)" = "$WANT" ]; then
        ok "a server that ignores Range does not corrupt the file"
    else
        bad "a server that ignores Range does not corrupt the file" \
            "got $(stat -c%s "$TMP/ign.bin") bytes, wanted $(stat -c%s "$TMP/payload.bin")"
    fi
    stopserve
fi

echo
echo "  and one that stops partway"
if serve cut; then
    head -c 200000 "$TMP/payload.bin" > "$TMP/cut.bin"
    out/aurbridge-sim fetch http://127.0.0.1:$PORT/x.img "$TMP/cut.bin" >/dev/null 2>&1
    SZ=$(stat -c%s "$TMP/cut.bin")
    [ "$SZ" -gt 200000 ] \
      && ok "what did arrive is kept, so the next try carries on" \
      || bad "what did arrive is kept, so the next try carries on" "$SZ bytes"
    # ...and trying again finishes it.
    stopserve
    if serve whole; then
        out/aurbridge-sim fetch http://127.0.0.1:$PORT/x.img "$TMP/cut.bin" >/dev/null 2>&1
        [ "$(sha256sum "$TMP/cut.bin" | cut -d' ' -f1)" = "$WANT" ] \
          && ok "and the try after that finishes it" \
          || bad "and the try after that finishes it"
        stopserve
    fi
fi

echo
echo "  and a copy that is already here"
cp "$TMP/payload.bin" "$TMP/local.bin"
out/aurbridge-sim fetch "file://$TMP/local.bin" "$TMP/fromfile.bin" >/dev/null 2>&1
[ "$(sha256sum "$TMP/fromfile.bin" | cut -d' ' -f1)" = "$WANT" ] \
  && ok "file:// works, so a machine that is offline is not stuck" \
  || bad "file:// works, so a machine that is offline is not stuck"

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "A product that is three downloads is a product most people do not"
    echo "finish installing."
    exit 1
fi
echo "$checked checks: the installer carries the staging environment, signing"
echo "does not disturb it, and a download resumes rather than starting again."
exit 0
