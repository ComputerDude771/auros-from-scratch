#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  shrinktargettest — how much of Windows the install takes
#
#  shrink_pick_target() decides how small to make the Windows volume.
#  The 2026-10-23 audit found the old code shrank it to ntfsresize's
#  absolute floor, so AurOS swallowed every free byte -- hours of
#  surface test and shrink on a hard disk, and a Windows left full. The
#  rule now is "only what AurOS needs, with headroom", and this is the
#  rule, on its own, against named cases -- including the ones that
#  must still take everything so the end-to-end tests on a tiny
#  synthetic disk keep shrinking.
#
#    sh tools/shrinktargettest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-58s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-58s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/shrinktgt.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# A harness that links the real shrink.c. It only calls the pure
# shrink_pick_target(); the resize/surface code is dead-linked, so the
# number under test is the number the install uses, not a copy.
cat > "$TMP/t.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include "shrink.h"
int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    unsigned long long cur = strtoull(argv[1], NULL, 10);
    unsigned long long sml = strtoull(argv[2], NULL, 10);
    unsigned long long need = strtoull(argv[3], NULL, 10);
    printf("%llu\n", (unsigned long long)shrink_pick_target(cur, sml, need));
    return 0;
}
EOC
cc -O2 -std=gnu11 -Wall -Wextra -I src/aurstage -o "$TMP/t" "$TMP/t.c" \
   src/aurstage/shrink.c src/aurstage/plan.c src/aurstage/gpt.c \
   src/aurstage/ntfs.c src/aurstage/sha256.c src/aurstage/fde.c \
   2>"$TMP/cc.log" || { echo "  harness did not build:"; sed -n 1,20p "$TMP/cc.log"; exit 2; }

GIB=$((1024*1024*1024))
pick() { "$TMP/t" "$1" "$2" "$3"; }

echo
echo "How much of Windows does the install take?"
echo

# A 500 GiB Windows with 300 GiB reclaimable, image needs ~26 GB.
# Expect: take 40 GiB (min(40, headroom)); Windows left ~460 GiB.
CUR=$((500*GIB)); SML=$((200*GIB)); NEED=$((26*1000*1000*1000))
T=$(pick $CUR $SML $NEED); SHARE=$((CUR-T))
[ "$SHARE" = "$((40*GIB))" ] \
    && ok "big disk: AurOS takes exactly 40 GiB, not everything free" \
    || bad "big disk: AurOS takes exactly 40 GiB, not everything free" "took $SHARE"

# A 128 GiB Windows, 50 GiB reclaimable. reserve=max(20,15%*128=19.2)=20.
# headroom=30 -> share=30 GiB (leaves Windows 20 GiB free).
CUR=$((128*GIB)); SML=$((78*GIB)); NEED=$((26*1000*1000*1000))
T=$(pick $CUR $SML $NEED); SHARE=$((CUR-T)); FREE_LEFT=$((T-SML))
[ "$SHARE" = "$((30*GIB))" ] \
    && ok "medium disk: AurOS takes the headroom (30 GiB), not 40" \
    || bad "medium disk: AurOS takes the headroom (30 GiB), not 40" "took $SHARE"
[ "$FREE_LEFT" -ge "$((20*GIB))" ] \
    && ok "...and Windows keeps at least its 20 GiB reserve free" \
    || bad "...and Windows keeps at least its 20 GiB reserve free" "left $FREE_LEFT"

# A disk where the need exceeds the comfortable headroom: the install
# must still fit, so the share rises to the need (dipping into reserve).
CUR=$((60*GIB)); SML=$((28*GIB)); NEED=$((26*1000*1000*1000))   # reclaimable 32 GiB
T=$(pick $CUR $SML $NEED); SHARE=$((CUR-T))
[ "$SHARE" -ge "$NEED" ] \
    && ok "tight disk: the share rises to meet the install's need" \
    || bad "tight disk: the share rises to meet the install's need" "took $SHARE need $NEED"

# A tiny synthetic test volume: 1 GiB Windows, ~50 MiB floor, need 1.3 GB.
# Everything reclaimable is less than the need, so it takes it all and
# returns `smallest` -- the end-to-end tests depend on a real shrink.
CUR=$((1*GIB)); SML=$((50*1024*1024)); NEED=$((1300*1000*1000))
T=$(pick $CUR $SML $NEED)
[ "$T" = "$SML" ] \
    && ok "tiny volume: takes it all (= the floor), so the shrink still runs" \
    || bad "tiny volume: takes it all (= the floor), so the shrink still runs" \
        "target $T, floor $SML"

# Never below the floor: a target that would be smaller than `smallest`
# is clamped to `smallest`.
T=$(pick $((2*GIB)) $((1*GIB)) $((999*GIB)))
[ "$T" = "$((1*GIB))" ] \
    && ok "never shrinks below ntfsresize's floor" \
    || bad "never shrinks below ntfsresize's floor" "target $T"

echo
if [ "$fail" -gt 0 ]; then echo "$fail of $checked wrong."; exit 1; fi
echo "$checked checks: the install takes only what AurOS needs, with headroom,"
echo "and still takes everything on a disk too small to leave any."
exit 0
