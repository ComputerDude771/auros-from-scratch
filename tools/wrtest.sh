#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  wrtest — the one file in src/aurstage that is allowed to write
#
#  Two things are being tested and only one of them is the code.
#
#  The code: a write outside its named window must be REFUSED, not
#  clamped. A clamped write is a write that went somewhere else and
#  said nothing.
#
#  The arrangement: build/staging must refuse to build an image in
#  which any file OTHER than wr.c opens a device writably. That gate is
#  what makes wr.c worth having -- not the bounds checks, which anybody
#  can write, but the fact that there is nowhere else to write from.
#
#    sh tools/wrtest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/wrtest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

echo
echo "Can anything write to a disk except the one file that may?"
echo

cat > "$TMP/w.c" <<'EOC'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wr.h"

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *cmd = argv[1], *dev = argv[2];
    wr_target t; char why[240] = {0};
    if (wr_open(&t, dev, why, sizeof why) != 0) {
        printf("open=failed\n"); printf("why=%s\n", why); return 2;
    }
    printf("untouched=%d\n", wr_untouched(&t));

    /* One window over [1 MiB, 2 MiB), which is what every case below
     * is measured against. */
    int a = wr_arm(&t, WR_ROOT, 1u<<20, 2u<<20, why, sizeof why);
    printf("arm=%d\n", a);

    static unsigned char buf[65536];
    memset(buf, 0xA5, sizeof buf);

    if (!strcmp(cmd, "inside")) {
        int r = wr_bytes(&t, WR_ROOT, 1u<<20, buf, sizeof buf, why, sizeof why);
        printf("write=%d\n", r); printf("why=%s\n", why);
        printf("untouched=%d\n", wr_untouched(&t));
        printf("written=%llu\n", (unsigned long long)wr_written(&t));
        uint64_t bad = 0;
        printf("check=%d\n", wr_check(&t, 1u<<20, buf, sizeof buf, &bad));
        printf("verified=%llu\n", (unsigned long long)wr_verified(&t));
    } else if (!strcmp(cmd, "before")) {
        int r = wr_bytes(&t, WR_ROOT, (1u<<20) - 1, buf, 16, why, sizeof why);
        printf("write=%d\n", r); printf("why=%s\n", why);
        printf("untouched=%d\n", wr_untouched(&t));
    } else if (!strcmp(cmd, "after")) {
        int r = wr_bytes(&t, WR_ROOT, (2u<<20) - 8, buf, 16, why, sizeof why);
        printf("write=%d\n", r); printf("why=%s\n", why);
        printf("untouched=%d\n", wr_untouched(&t));
    } else if (!strcmp(cmd, "unarmed")) {
        int r = wr_bytes(&t, WR_RECOVERY, 1u<<20, buf, 16, why, sizeof why);
        printf("write=%d\n", r); printf("why=%s\n", why);
        printf("untouched=%d\n", wr_untouched(&t));
    } else if (!strcmp(cmd, "overlap")) {
        int r = wr_arm(&t, WR_RECOVERY, (1u<<20) + 4096, 3u<<20, why, sizeof why);
        printf("arm2=%d\n", r); printf("why=%s\n", why);
    } else if (!strcmp(cmd, "adjacent")) {
        int r = wr_arm(&t, WR_RECOVERY, 2u<<20, 3u<<20, why, sizeof why);
        printf("arm2=%d\n", r); printf("why=%s\n", why);
    } else if (!strcmp(cmd, "wrap")) {
        /* off + len wraps. A wrapped sum compares as inside any
         * window, so the overflow check has to come first. */
        int r = wr_bytes(&t, WR_ROOT, 0xFFFFFFFFFFFFFFF0ull, buf, 64,
                         why, sizeof why);
        printf("write=%d\n", r); printf("why=%s\n", why);
        printf("untouched=%d\n", wr_untouched(&t));
    } else if (!strcmp(cmd, "disarm")) {
        wr_disarm(&t, WR_ROOT);
        int r = wr_bytes(&t, WR_ROOT, 1u<<20, buf, 16, why, sizeof why);
        printf("write=%d\n", r); printf("why=%s\n", why);
        printf("untouched=%d\n", wr_untouched(&t));
    } else if (!strcmp(cmd, "mismatch")) {
        wr_bytes(&t, WR_ROOT, 1u<<20, buf, 4096, why, sizeof why);
        unsigned char other[4096]; memset(other, 0xA5, sizeof other);
        other[1234] = 0x5A;
        uint64_t bad = 0;
        printf("check=%d\n", wr_check(&t, 1u<<20, other, sizeof other, &bad));
        printf("first_bad=%llu\n", (unsigned long long)bad);
    }
    wr_flush(&t);
    wr_close(&t);
    return 0;
}
EOC
gcc -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I src/aurstage -o "$TMP/w" \
    "$TMP/w.c" src/aurstage/wr.c || { echo "  did not build"; exit 2; }

mkdisk() { rm -f "$1"; truncate -s 8M "$1"; }
run() { D="$TMP/d.img"; mkdisk "$D"; "$TMP/w" "$1" "$D" 2>/dev/null; }
F() { run "$1" | sed -n "s/^$2=//p" | tail -1; }

echo "  a write inside its window"
[ "$(F inside write)" = "0" ] && ok "lands" || bad "lands" "write=$(F inside write)"
[ "$(F inside check)" = "0" ] && ok "...and reads back identical" \
                              || bad "...and reads back identical"
[ "$(F inside verified)" = "65536" ] && ok "...and the verified count is honest" \
                                     || bad "...and the verified count is honest" \
                                            "verified=$(F inside verified)"

echo
echo "  and every write that is not"
for c in before after unarmed wrap disarm; do
    case "$c" in
      before)   n="one byte before the window is refused" ;;
      after)    n="eight bytes past the end is refused" ;;
      unarmed)  n="a window nobody armed is refused" ;;
      wrap)     n="an offset that would wrap the address is refused" ;;
      disarm)   n="a window that has been disarmed is refused" ;;
    esac
    if [ "$(F "$c" write)" = "-1" ]; then ok "$n"; else bad "$n" "write=$(F "$c" write)"; fi
    # THE PROPERTY THAT MATTERS: a refused write must not have written.
    if [ "$(run "$c" | sed -n 's/^untouched=//p' | tail -1)" = "1" ]; then
        ok "...and not one byte was written"
    else
        bad "...and not one byte was written" "the refusal wrote something"
    fi
done

echo
echo "  and windows that would tread on each other"
[ "$(F overlap arm2)" = "-1" ] && ok "two overlapping windows will not arm" \
                               || bad "two overlapping windows will not arm"
case "$(F overlap why)" in
  *overlap*) ok "...and it says which two" ;;
  *) bad "...and it says which two" "it said: $(F overlap why)" ;;
esac
[ "$(F adjacent arm2)" = "0" ] && ok "two windows that merely touch are fine" \
                              || bad "two windows that merely touch are fine"

echo
echo "  and a read-back that disagrees"
[ "$(F mismatch check)" = "-1" ] && ok "a mismatch is caught" || bad "a mismatch is caught"
EXP=$((1048576 + 1234))
[ "$(F mismatch first_bad)" = "$EXP" ] && ok "...and the first wrong byte is named" \
                                       || bad "...and the first wrong byte is named" \
                                              "got $(F mismatch first_bad), wanted $EXP"

echo
echo "  and nothing outside the window moved"
D="$TMP/whole.img"; mkdisk "$D"
dd if=/dev/urandom of="$D" bs=1M count=8 conv=notrunc status=none
cp "$D" "$TMP/before.img"
"$TMP/w" inside "$D" >/dev/null 2>&1
# Everything below 1 MiB and above 2 MiB must be byte-identical.
a1=$(dd if="$TMP/before.img" bs=1M count=1 status=none | md5sum)
a2=$(dd if="$D"              bs=1M count=1 status=none | md5sum)
b1=$(dd if="$TMP/before.img" bs=1M skip=2 status=none | md5sum)
b2=$(dd if="$D"              bs=1M skip=2 status=none | md5sum)
[ "$a1" = "$a2" ] && ok "the megabyte before the window is untouched" \
                  || bad "the megabyte before the window is untouched"
[ "$b1" = "$b2" ] && ok "the six megabytes after it are untouched" \
                  || bad "the six megabytes after it are untouched"

echo
echo "  and the build refuses a second writer"
# A gate that has never fired is a gate nobody should trust.
cp src/aurstage/disks.c "$TMP/disks.bak"
# Just the flag name. The first version matched on the whole
# "O_RDONLY | O_CLOEXEC" and used a pipe as sed's delimiter as well,
# so it silently matched nothing and the gate test reported "could not
# stage the fault" instead of testing the gate -- a test that was not
# testing, which is the only kind worse than no test.
sed -i 's/O_RDONLY/O_RDWR/' src/aurstage/disks.c
if grep -q 'O_RDWR' src/aurstage/disks.c; then
    if ./build/staging desktop 2>&1 | grep -q 'only wr.c may write'; then
        ok "a writable open in another file stops the build"
    else
        bad "a writable open in another file stops the build" "the build allowed it"
    fi
else
    bad "a writable open in another file stops the build" "could not stage the fault"
fi
cp "$TMP/disks.bak" src/aurstage/disks.c
./build/staging desktop >/dev/null 2>&1 && ok "...and the real tree still builds" \
                                        || bad "...and the real tree still builds"

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "Everything stage C does to a stranger's disk goes through this file."
    exit 1
fi
echo "$checked checks: writes land only where they were armed to, refusals"
echo "write nothing, and nothing else in the directory can write at all."
exit 0
