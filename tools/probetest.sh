#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  probetest — phase 7, and the difference between two refusals
#
#  The probe exists to catch one thing: a machine that would boot into
#  a working-looking AurOS desktop with no way to get online. That
#  person cannot search for the answer, cannot ask us, and cannot get
#  back. It is worth an abort, and the abort is free -- the old
#  partition table is still in force and Windows still boots.
#
#  But "this computer has no WiFi" and "we failed to load any drivers
#  at all" are DIFFERENT ANSWERS, and confusing them tells a person
#  their laptop is at fault when the fault is ours. That distinction
#  is what this file mostly tests.
#
#    sh tools/probetest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/probetest.XXXXXX")
trap 'mountpoint -q /probe 2>/dev/null && umount /probe; rm -rf "$TMP"' EXIT

echo
echo "Would it refuse a machine that cannot get online -- and blame the"
echo "right thing when it cannot tell?"
echo

cat > "$TMP/p.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"
int main(int argc, char **argv)
{
    probe_result r;
    if (argc >= 2 && !strcmp(argv[1], "mount")) {
        probe_run(argv[2], &r);
    } else {
        /* look <sysroot> <modules_loaded> <kernel_matches> */
        memset(&r, 0, sizeof r);
        probe_look(argv[2], &r);
        r.modules_loaded = atoi(argv[3]);
        r.kernel_matches = atoi(argv[4]);
        probe_judge(&r);
    }
    printf("verdict=%s\n", probe_verdict_name(r.verdict));
    printf("wifi=%d\n", r.wifi_devices);
    printf("wired=%d\n", r.wired_up);
    printf("backlight=%d\n", r.backlight);
    printf("sound=%d\n", r.sound_cards);
    printf("why=%s\n", r.why);
    printf("remedy=%s\n", r.remedy);
    return r.verdict == PROBE_OK ? 0 : 1;
}
EOC
gcc -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I src/aurstage -o "$TMP/p" \
    "$TMP/p.c" src/aurstage/probe.c src/aurstage/boot.c \
    src/aurstage/disks.c src/aurstage/sha256.c src/aurstage/ntfs.c \
    src/aurstage/fde.c || { echo "  did not build"; exit 2; }

# A synthetic /sys, because what hardware this container has is not
# something a test gets to choose.
mksys() { # dir  wifi wired-carrier backlight sound
    R="$TMP/$1"; rm -rf "$R"; mkdir -p "$R/class/net/lo"
    [ "$2" = 1 ] && mkdir -p "$R/class/ieee80211/phy0"
    mkdir -p "$R/class/net/eth0"; printf '%s\n' "$3" > "$R/class/net/eth0/carrier"
    # A wireless interface has a `wireless` directory; it must not be
    # counted as a wired link, or a laptop with WiFi hardware and no
    # network chosen would look like it has a cable in.
    mkdir -p "$R/class/net/wlan0/wireless"
    printf '1\n' > "$R/class/net/wlan0/carrier"
    [ "$4" = 1 ] && mkdir -p "$R/class/backlight/intel_backlight"
    [ "$5" = 1 ] && mkdir -p "$R/class/sound/card0"
    echo "$R"
}
G() { sed -n "s/^$1=//p"; }

echo "  a machine that works"
S=$(mksys ok 1 0 1 1)
OUT=$("$TMP/p" look "$S" 40 1 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verdict)" = "ok" ] && ok "wireless present: allowed" \
    || bad "wireless present: allowed" "$(printf '%s\n' "$OUT" | G why)"
[ "$(printf '%s\n' "$OUT" | G wifi)" = "1" ] && ok "...and the wireless is counted" \
    || bad "...and the wireless is counted"

S=$(mksys wired 0 1 0 0)
OUT=$("$TMP/p" look "$S" 40 1 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verdict)" = "ok" ] && ok "no wireless but a cable: allowed" \
    || bad "no wireless but a cable: allowed"

echo
echo "  a machine that cannot get online"
S=$(mksys dark 0 0 1 1)
OUT=$("$TMP/p" look "$S" 40 1 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verdict)" = "no-network" ] && ok "no wireless, no cable: refused" \
    || bad "no wireless, no cable: refused" "$(printf '%s\n' "$OUT" | G verdict)"
case "$(printf '%s\n' "$OUT" | G remedy)" in
  *"Windows will start as usual"*) ok "...and she is told Windows still works" ;;
  *) bad "...and she is told Windows still works" \
         "it said: $(printf '%s\n' "$OUT" | G remedy)" ;;
esac

# A wireless interface with a carrier is NOT a cable. A laptop with a
# WiFi card that has not joined a network would otherwise pass the
# wired test and skip the abort this phase exists for.
S=$(mksys onlywl 0 0 0 0)
OUT=$("$TMP/p" look "$S" 40 1 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G wired)" = "0" ] && ok "a wireless interface is not mistaken for a cable" \
    || bad "a wireless interface is not mistaken for a cable" "wired=$(printf '%s\n' "$OUT" | G wired)"

echo
echo "  and the fault that is OURS, not the machine's"
# The image and the running kernel disagree: every modprobe fails, no
# device ever appears. Reporting that as "this computer's WiFi does
# not work" is a lie that sends her to buy an adapter she does not
# need.
S=$(mksys nodrv 0 0 0 0)
OUT=$("$TMP/p" look "$S" 0 1 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verdict)" = "no-drivers" ] && ok "zero drivers loaded is not a hardware verdict" \
    || bad "zero drivers loaded is not a hardware verdict" "$(printf '%s\n' "$OUT" | G verdict)"
case "$(printf '%s\n' "$OUT" | G remedy)" in
  *"not in this computer"*) ok "...and the blame is put where it belongs" ;;
  *) bad "...and the blame is put where it belongs" \
         "it said: $(printf '%s\n' "$OUT" | G remedy)" ;;
esac

OUT=$("$TMP/p" look "$S" 40 0 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verdict)" = "no-drivers" ] && ok "an image without this kernel's modules is our fault too" \
    || bad "an image without this kernel's modules is our fault too"

# And the order matters: a machine with no network AND no drivers must
# be reported as no-drivers, because we never got far enough to know
# anything about its network.
S=$(mksys both 0 0 0 0)
OUT=$("$TMP/p" look "$S" 0 0 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verdict)" = "no-drivers" ] && ok "our fault is reported before the machine's" \
    || bad "our fault is reported before the machine's"

echo
echo "  and what we just wrote not mounting at all"
dd if=/dev/urandom of="$TMP/garbage.img" bs=1M count=8 status=none
OUT=$("$TMP/p" mount "$TMP/garbage.img" 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verdict)" = "unmountable" ] && ok "a root that will not mount is caught" \
    || bad "a root that will not mount is caught" "$(printf '%s\n' "$OUT" | G verdict)"
case "$(printf '%s\n' "$OUT" | G remedy)" in
  *"Nothing on this computer has been changed"*) ok "...and nothing has been changed" ;;
  *) bad "...and nothing has been changed" ;;
esac

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "This phase is the last chance to stop before anything is committed."
    exit 1
fi
echo "$checked checks: it refuses a machine that cannot get online, and it"
echo "does not call our own fault a fault of hers."
exit 0
