#!/bin/sh
# wlstress.sh — does the compositor survive applications behaving badly?
#
# The polite tests answer "does a browser work". This one answers the
# question that decides whether the machine is trustworthy: when one
# application crashes, does everything else on the screen survive?
#
# It exists because the answer was no. A SIGKILLed terminal segfaulted
# the compositor, because libwayland destroys a dead client's resources
# in an order the compositor does not choose -- it freed the wl_surface
# and then ran the xdg_surface destructor, which wrote through the freed
# pointer. Not a corner case: that is every crash of every program.
#
#   sh tools/wlstress.sh [path-to-wltest]
#
# Build wltest first (see tools/README.md). Run it against an
# AddressSanitizer build too -- the plain build happened to survive two
# of these cases while corrupting the heap.
set -u
WLTEST="${1:-/tmp/wltest}"
: "${XDG_RUNTIME_DIR:=/tmp/wlrun}"
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

pass=0; fail=0
run() {
    name="$1"; shift
    printf '%-46s ' "$name"
    out=$(timeout 90 "$WLTEST" "$@" 2>&1)
    rc=$?
    # A crash is the failure this script exists to catch, and it does not
    # announce itself in the output -- it just stops producing any.
    case $rc in
      139|134|136) printf 'CRASH (signal %d)\n' $((rc - 128)); fail=$((fail+1)); return ;;
    esac
    if printf '%s' "$out" | grep -q '^PASS'; then
        printf 'ok\n'; pass=$((pass+1))
    else
        printf 'FAILED (exit %d)\n' "$rc"
        printf '%s\n' "$out" | sed -n '1,12p' | sed 's/^/    /'
        fail=$((fail+1))
    fi
}

# Each case ends with a client that must still be alive and drawing, so
# "PASS" means the compositor not only survived but kept working.
run "a client SIGKILLed while mapped" \
    -s 12 -o /tmp/wlst-kill -- \
    'weston-terminal & P=$!; sleep 4; kill -9 $P; sleep 2; weston-simple-shm'

run "a client SIGKILLed before it maps" \
    -s 10 -o /tmp/wlst-early -- \
    'weston-terminal & P=$!; kill -9 $P; sleep 1; weston-simple-shm'

run "three clients at once" \
    -s 10 -o /tmp/wlst-three -- \
    'weston-simple-shm & weston-simple-shm & weston-terminal; wait'

run "a client that connects and leaves, twenty times" \
    -s 16 -o /tmp/wlst-churn -- \
    'i=0; while [ $i -lt 20 ]; do weston-simple-shm & p=$!; sleep 0.15; kill -9 $p 2>/dev/null; i=$((i+1)); done; weston-simple-shm'

run "a client that binds globals and exits cleanly" \
    -s 8 -o /tmp/wlst-info -- \
    'weston-info >/dev/null 2>&1; weston-simple-shm'

echo
if [ "$fail" -eq 0 ]; then
    echo "$pass/$((pass+fail)) — the compositor outlives its clients"
    exit 0
fi
echo "$fail of $((pass+fail)) FAILED — one application can take down the desktop"
exit 1
