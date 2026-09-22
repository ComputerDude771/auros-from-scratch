#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  shrinktest — the one step in this product that cannot be undone
#
#  Phase 4 resizes a real NTFS filesystem. Everything before it is a
#  refusal that costs a restart; everything after it writes only into
#  space that is already free. This is the single non-restartable
#  window, and the way to have confidence in it is to run it, for
#  real, on a real filesystem with real files in it, and then check
#  the files are still there.
#
#  The volume is made by the mkntfs this product ships and read back
#  by the ntfs-3g it ships, so the thing under test is the thing that
#  will run on a stranger's disk.
#
#    sudo sh tools/shrinktest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
RFS="${RFS:-work/forge/desktop/rootfs}"

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

[ -d "$RFS" ] || { echo "no built rootfs at $RFS"; exit 2; }
LD=$(ls "$RFS"/lib64/ld-linux-x86-64.so.2 2>/dev/null || \
     ls "$RFS"/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 2>/dev/null)
LP="$RFS/lib/x86_64-linux-gnu:$RFS/lib64:$RFS/usr/lib/x86_64-linux-gnu"
[ -n "$LD" ] || { echo "no loader in the image"; exit 2; }
nt() { p="$RFS/usr/sbin/$1"; [ -x "$p" ] || p="$RFS/usr/bin/$1"; shift
       "$LD" --library-path "$LP" "$p" "$@"; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/shrinktest.XXXXXX")
trap 'mountpoint -q "$TMP/m" 2>/dev/null && umount "$TMP/m"; rm -rf "$TMP"' EXIT

echo
echo "Does the one irreversible step leave the files where they were?"
echo

# A wrapper that runs ntfsresize out of the rootfs, so shrink.c's fixed
# path points at the copy this product ships rather than one installed
# on the build host. The argument vector is still fixed in the C.
#
# Absolute paths, built one at a time. The first version interpolated a
# bash parameter expansion into a /bin/sh heredoc and produced an empty
# file, which shrink.c then correctly reported as "could not run the
# tool" -- the right answer to the wrong question.
HERE=$(pwd)
ALD="$HERE/$LD"
ALP="$HERE/$RFS/lib/x86_64-linux-gnu:$HERE/$RFS/lib64:$HERE/$RFS/usr/lib/x86_64-linux-gnu"
ANT="$HERE/$RFS/usr/sbin/ntfsresize"
mkdir -p "$TMP/sbin"
{
  echo '#!/bin/sh'
  echo "exec \"$ALD\" --library-path \"$ALP\" \"$ANT\" \"\$@\""
} > "$TMP/sbin/ntfsresize"
chmod +x "$TMP/sbin/ntfsresize"
"$TMP/sbin/ntfsresize" --version >/dev/null 2>&1 \
    || { echo "  the ntfsresize wrapper does not run"; exit 2; }

gcc -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I src/aurstage \
    -DNTFSRESIZE_PATH="\"$TMP/sbin/ntfsresize\"" \
    -o "$TMP/s" -x c - src/aurstage/shrink.c src/aurstage/ntfs.c \
    src/aurstage/fde.c src/aurstage/boot.c src/aurstage/disks.c \
    src/aurstage/sha256.c <<'EOC' || { echo "  did not build"; exit 2; }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shrink.h"
#include "ntfs.h"
static void pct(int p) { fprintf(stderr, " %d%%", p); }
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "size")) {
        uint64_t b = 0;
        int r = ntfs_volume_bytes(argv[2], &b);
        printf("rc=%d\n", r);
        printf("bytes=%llu\n", (unsigned long long)b);
        return r;
    }
    if (!strcmp(argv[1], "ask")) {
        shrink_plan p; shrink_ask(argv[2], &p);
        printf("ok=%d\n", p.ok);
        printf("current=%llu\n", (unsigned long long)p.current_bytes);
        printf("smallest=%llu\n", (unsigned long long)p.smallest_bytes);
        printf("why=%s\n", p.why);
        return p.ok ? 0 : 1;
    }
    if (!strcmp(argv[1], "do")) {
        shrink_result r;
        shrink_do(argv[2], strtoull(argv[3], NULL, 10), pct, &r);
        fprintf(stderr, "\n");
        printf("ok=%d\n", r.ok);
        printf("started=%d\n", r.started);
        printf("achieved=%llu\n", (unsigned long long)r.achieved_bytes);
        printf("why=%s\n", r.why);
        return r.ok ? 0 : 1;
    }
    return 2;
}
EOC

V="$TMP/v.img"
truncate -s 512M "$V"
nt mkntfs -Q -F -L WINDOWS "$V" >/dev/null 2>&1 || { echo "  no mkntfs"; exit 2; }

# Put real files in it, at the far end as well as the near, so the
# resize has to relocate something rather than just trimming empty
# space -- which is the whole difficulty of a real shrink.
mkdir -p "$TMP/m"
if ! nt ntfs-3g "$V" "$TMP/m" >/dev/null 2>&1; then
    echo "  cannot mount NTFS here (no FUSE?); the real shrink is untested"
    exit 2
fi
mkdir -p "$TMP/m/Users/auros/Pictures"
for i in 1 2 3 4 5 6 7 8; do
    dd if=/dev/urandom of="$TMP/m/Users/auros/Pictures/p$i.jpg" \
       bs=1M count=6 status=none
done
# And a file written last, so it lands high in the volume.
dd if=/dev/urandom of="$TMP/m/Users/auros/thesis.odt" bs=1M count=20 status=none
( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$TMP/before.sums"
sync; umount "$TMP/m"
BEFORE=$(wc -l < "$TMP/before.sums")
echo "  a 512 MiB Windows volume with $BEFORE files in it"

F() { sed -n "s/^$1=//p"; }
BYTES=$("$TMP/s" size "$V" 2>/dev/null | F bytes)
echo "    it says it is $((BYTES / 1024 / 1024)) MiB"

echo
echo "  what it says it could do"
OUT=$("$TMP/s" ask "$V" 2>/dev/null)
SMALL=$(printf '%s\n' "$OUT" | F smallest)
[ "$(printf '%s\n' "$OUT" | F ok)" = "1" ] && ok "ntfsresize answers" \
                                           || bad "ntfsresize answers" "$OUT"
[ -n "$SMALL" ] && [ "$SMALL" -gt 0 ] && ok "...with a floor of $((SMALL/1024/1024)) MiB" \
                                      || bad "...with a floor" "smallest=$SMALL"

echo
echo "  and then doing it"
TARGET=$((300 * 1024 * 1024))
OUT=$("$TMP/s" do "$V" "$TARGET" 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | F ok)" = "1" ] && ok "the volume resizes" \
    || bad "the volume resizes" "$(printf '%s\n' "$OUT" | F why)"
[ "$(printf '%s\n' "$OUT" | F started)" = "1" ] && ok "...and it reports having started" \
    || bad "...and it reports having started"

ACH=$(printf '%s\n' "$OUT" | F achieved)
NOW=$("$TMP/s" size "$V" 2>/dev/null | F bytes)
[ -n "$ACH" ] && [ "$ACH" = "$NOW" ] && ok "the size it reports is the size on the volume" \
    || bad "the size it reports is the size on the volume" "achieved=$ACH boot=$NOW"
if [ -n "$ACH" ] && [ "$ACH" -le "$TARGET" ]; then
    ok "...and it is not bigger than what was asked for"
else
    bad "...and it is not bigger than what was asked for" "achieved=$ACH target=$TARGET"
fi

echo
echo "  and the files"
if nt ntfs-3g "$V" "$TMP/m" >/dev/null 2>&1; then
    ok "the volume still mounts"
    ( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$TMP/after.sums"
    umount "$TMP/m"
    if cmp -s "$TMP/before.sums" "$TMP/after.sums"; then
        ok "all $BEFORE files are byte-for-byte what they were"
    else
        bad "all $BEFORE files are byte-for-byte what they were" \
            "$(diff "$TMP/before.sums" "$TMP/after.sums" | head -4)"
    fi
else
    bad "the volume still mounts" "it does not"
fi

echo
echo "  and a shrink that cannot be done"
V2="$TMP/v2.img"; truncate -s 256M "$V2"
nt mkntfs -Q -F -L SMALL "$V2" >/dev/null 2>&1
OUT=$("$TMP/s" do "$V2" 1048576 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | F ok)" = "0" ] && ok "a target below the floor is refused" \
    || bad "a target below the floor is refused"
if nt ntfs-3g "$V2" "$TMP/m" >/dev/null 2>&1; then
    umount "$TMP/m"; ok "...and the volume is still mountable afterwards"
else
    bad "...and the volume is still mountable afterwards" "the refusal damaged it"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "This is the only step in the product with no way back."
    exit 1
fi
echo "$checked checks: a real volume shrinks, reports the size it actually"
echo "reached, and every file in it is still exactly what it was."
exit 0
