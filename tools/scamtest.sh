#!/bin/sh
# scamtest.sh -- the scam protections, checked from the source tree.
#
# Stream S2 (docs/handoff/streams/S2.md). Each part proves its own
# checks go red when the protection they check is removed (--prove).
#
#   sh tools/scamtest.sh            from anywhere in the repository
#
# Needs python3 with jsonschema (apt install python3-jsonschema).
# Exit 0 only if every part passes.
set -u
cd "$(dirname "$0")/.."
fail=0

python3 -I tools/scamtest/firefoxpolicy.py --prove || fail=$((fail + 1))

# The image must carry the file where Firefox looks first. build/forge
# lays rootfs/ down whole (stage_overlay); a build step that later wrote
# its own /etc/firefox/policies would silently replace every protection.
if grep -q 'cp -a --no-preserve=ownership "$ROOT/rootfs/." "$RFS/"' build/forge; then
    echo "    build/forge lays rootfs/ down whole                         ok"
else
    echo "    build/forge lays rootfs/ down whole                         FAIL"
    fail=$((fail + 1))
fi
if grep -n 'etc/firefox/policies' build/forge build/mkimage 2>/dev/null | grep -v '^\s*#' | grep -q .; then
    echo "    nothing in build/ writes its own /etc/firefox/policies      FAIL"
    grep -n 'etc/firefox/policies' build/forge build/mkimage
    fail=$((fail + 1))
else
    echo "    nothing in build/ writes its own /etc/firefox/policies      ok"
fi

if [ "$fail" -eq 0 ]; then echo "scamtest: ok"; else echo "scamtest: FAIL ($fail)"; fi
[ "$fail" -eq 0 ]
