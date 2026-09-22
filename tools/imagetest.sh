#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  imagetest — finding the AurOS image, and putting it on the disk
#
#  Phases 5 and 6. The doc's power-loss table says an abort anywhere in
#  them leaves the machine bootable into Windows, and the property that
#  makes that true is that they write ONLY into space the shrink has
#  already freed, with the old partition table still in force.
#
#  The interesting check here is the ordering one. "Write the first
#  megabyte last" is borrowed from eos-installer: zero it, write
#  everything else, verify, and only then lay it down, so that a
#  partially written install is never a bootable-LOOKING install. The
#  way to test that is to make the write fail partway and then look at
#  what is on the disk -- if the first megabyte holds a superblock at
#  that moment, the property does not hold.
#
#    sudo sh tools/imagetest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

for t in sgdisk mkfs.ext4 python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
TMP=$(mktemp -d "${TMPDIR:-/tmp}/imagetest.XXXXXX")
trap 'mountpoint -q "$TMP/m" 2>/dev/null && umount "$TMP/m"; rm -rf "$TMP"' EXIT

echo
echo "Does the image get found, checked, and written the right way round?"
echo

# ── a small AurOS image, the same shape as the real one ─────────────
IMG="$TMP/auros.img"
truncate -s 96M "$IMG"
sgdisk --zap-all "$IMG" >/dev/null 2>&1
sgdisk -n 1:2048:+16M -t 1:ef00 -c 1:"AUROS-ESP"  "$IMG" >/dev/null 2>&1
sgdisk -n 2:0:0       -t 2:8304 -c 2:"AUROS-ROOT" "$IMG" >/dev/null 2>&1
RS=$(sgdisk -i 2 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
RE=$(sgdisk -i 2 "$IMG" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
ROFF=$((RS * 512)); RLEN=$(((RE - RS + 1) * 512))
ES=$(sgdisk -i 1 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
EE=$(sgdisk -i 1 "$IMG" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
EOFF=$((ES * 512)); ELEN=$(((EE - ES + 1) * 512))
# Something recognisable in the EFI partition, so that "the boot chain
# is checked" is a check about bytes rather than about zeroes.
dd if=/dev/urandom of="$IMG" bs=512 seek=$ES count=64 conv=notrunc status=none
dd if=/dev/zero of="$TMP/root.img" bs=1M count=$((RLEN / 1048576)) status=none
mkfs.ext4 -q -L AUROS-ROOT "$TMP/root.img"
mkdir -p "$TMP/m"
mount -o loop "$TMP/root.img" "$TMP/m" 2>/dev/null && {
    echo "AurOS was here" > "$TMP/m/MARKER"
    mkdir -p "$TMP/m/etc"; echo "nocturne" > "$TMP/m/etc/auros-release"
    sync; umount "$TMP/m"
}
dd if="$TMP/root.img" of="$IMG" bs=1M seek=$((ROFF / 1048576)) conv=notrunc status=none
echo "  a ${RLEN}-byte root filesystem inside a 96 MiB image"

# ── the stick: our type GUID, a manifest, then the image ────────────
mkstick() { # out  profile  [corrupt-root] [lie-about-offsets]
    S="$1"; PROF="$2"; CORRUPT="${3:-}"; LIE="${4:-}"
    rm -f "$S"; truncate -s 160M "$S"
    sgdisk --zap-all "$S" >/dev/null 2>&1
    sgdisk -n 1:2048:0 -t 1:A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41 \
           -c 1:"AUROS-IMAGE" "$S" >/dev/null 2>&1
    PS=$(sgdisk -i 1 "$S" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    python3 - "$S" "$IMG" "$((PS * 512))" "$ROFF" "$RLEN" "$PROF" \
                 "$CORRUPT" "$LIE" "$EOFF" "$ELEN" <<'EOPY'
import sys, struct, hashlib
stick, img, pstart, roff, rlen, prof, corrupt, lie, eoff, elen = sys.argv[1:11]
pstart, roff, rlen = int(pstart), int(roff), int(rlen)
eoff, elen = int(eoff), int(elen)
data = bytearray(open(img, 'rb').read())
# HASH FIRST, THEN CORRUPT. The first version corrupted the image and
# then hashed it, so the manifest described the damage perfectly and
# the check had nothing to catch -- a fixture that agreed with itself,
# which is the same mistake this whole test exists to prevent in the
# product.
sha  = hashlib.sha256(bytes(data[roff:roff + rlen])).digest()
esha = hashlib.sha256(bytes(data[eoff:eoff + elen])).digest()
if corrupt == 'esp':
    data[eoff + 16] ^= 0xFF
elif corrupt:
    data[roff + rlen // 2] ^= 0xFF

man = bytearray(4096)
man[0:8] = b'AURIMG01'
struct.pack_into('<Q', man, 8, len(data))
struct.pack_into('<Q', man, 16, roff if not lie else roff + 4096)
struct.pack_into('<Q', man, 24, rlen)
struct.pack_into('<I', man, 32, 512)
man[36:68] = sha
man[68:68+len(prof)] = prof.encode()
struct.pack_into('<Q', man, 132, eoff)
struct.pack_into('<Q', man, 140, elen)
man[148:180] = esha

f = open(stick, 'r+b')
f.seek(pstart);            f.write(man)
f.seek(pstart + 4096);     f.write(bytes(data))
f.close()
EOPY
}

# TWO IMAGES ON ONE STICK, which is the whole reason image_find takes
# a profile at all -- and which no fixture in this repository could
# produce until now, so "given two, pick the one the journal names" was
# tested by nothing. Two AurOS sticks in the same machine reach
# image_find the same way: it walks every disk, and every AUROS-IMAGE
# partition on each.
#
# The wanted one is deliberately SECOND, so a search that stops at the
# first candidate fails this. `$3` damages the first partition's
# manifest, because a check that aborts on a sibling it was not asked
# about is the other half of the same bug.
mkstick2() { # out  first-profile  second-profile  [break-first]
    S="$1"; P1="$2"; P2="$3"; BREAK="${4:-}"
    rm -f "$S"; truncate -s 300M "$S"
    sgdisk --zap-all "$S" >/dev/null 2>&1
    sgdisk -n 1:2048:+140M -t 1:A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41 \
           -c 1:"AUROS-IMAGE" "$S" >/dev/null 2>&1
    sgdisk -n 2:0:+140M     -t 2:A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41 \
           -c 2:"AUROS-IMAGE" "$S" >/dev/null 2>&1
    for nn in 1 2; do
        PS=$(sgdisk -i $nn "$S" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
        if [ "$nn" = 1 ]; then PP="$P1"; BB="$BREAK"; else PP="$P2"; BB=""; fi
        python3 - "$S" "$IMG" "$((PS * 512))" "$ROFF" "$RLEN" "$PP" \
                     "$BB" "$EOFF" "$ELEN" <<'EOPY'
import sys, struct, hashlib
stick, img, pstart, roff, rlen, prof, brk, eoff, elen = sys.argv[1:10]
pstart, roff, rlen = int(pstart), int(roff), int(rlen)
eoff, elen = int(eoff), int(elen)
data = bytearray(open(img, 'rb').read())
sha  = hashlib.sha256(bytes(data[roff:roff + rlen])).digest()
esha = hashlib.sha256(bytes(data[eoff:eoff + elen])).digest()
man = bytearray(4096)
man[0:8] = b'AURIMG01'
# `brk` makes this manifest claim an image larger than the partition
# holding it -- one of the two checks that used to `return -1` for any
# candidate, before anybody asked whose it was.
struct.pack_into('<Q', man, 8, (1 << 40) if brk else len(data))
struct.pack_into('<Q', man, 16, roff)
struct.pack_into('<Q', man, 24, rlen)
struct.pack_into('<I', man, 32, 512)
man[36:68] = sha
man[68:68+len(prof)] = prof.encode()
struct.pack_into('<Q', man, 132, eoff)
struct.pack_into('<Q', man, 140, elen)
man[148:180] = esha
f = open(stick, 'r+b')
f.seek(pstart);        f.write(man)
f.seek(pstart + 4096); f.write(bytes(data))
f.close()
EOPY
    done
}

cat > "$TMP/i.c" <<'EOC'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "wr.h"
static void pct(int p) { (void)p; }
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    /* The survey normally supplies the disks. Here they are named on
     * the command line, because a loop device in this container has
     * max_part=0 and never produces partition nodes. */
    stage_machine m; memset(&m, 0, sizeof m);
    char why[240] = {0};

    if (!strcmp(argv[1], "write")) {
        /* write <stick-part> <target> <dst_off> <root_len> [win_bytes] */
        /* write <disk> <part_off> <target> <dst_off> [win] */
        image_src s; memset(&s, 0, sizeof s);
        snprintf(s.dev, sizeof s.dev, "%s", argv[2]);
        s.part_off = strtoull(argv[3], NULL, 10);
        FILE *f = fopen(argv[2], "rb");
        unsigned char man[4096];
        if (!f || fseek(f, (long)s.part_off, SEEK_SET) != 0 ||
            fread(man, 1, sizeof man, f) != sizeof man) return 2;
        fclose(f);
        s.image_bytes = *(uint64_t *)(man + 8);
        s.root_off    = *(uint64_t *)(man + 16);
        s.root_len    = *(uint64_t *)(man + 24);
        s.image_sector= *(uint32_t *)(man + 32);
        memcpy(s.root_sha, man + 36, 32);
        s.have_sha = 1;

        wr_target t;
        if (wr_open(&t, argv[4], why, sizeof why) != 0) {
            printf("open=failed\n"); printf("why=%s\n", why); return 2;
        }
        uint64_t dst = strtoull(argv[5], NULL, 10);
        uint64_t win = argc > 6 ? strtoull(argv[6], NULL, 10) : s.root_len;
        if (wr_arm(&t, WR_ROOT, dst, dst + win, why, sizeof why) != 0) {
            printf("arm=failed\n"); printf("why=%s\n", why); return 2;
        }
        int r = image_write_root(&t, &s, dst, pct, why, sizeof why);
        printf("write=%d\n", r);
        printf("why=%s\n", why);
        printf("written=%llu\n", (unsigned long long)wr_written(&t));
        wr_close(&t);
        return r ? 1 : 0;
    }

    /* find/verify <disk> <profile> */
    snprintf(m.disk[0].name, sizeof m.disk[0].name, "%s", argv[2]);
    m.disk[0].logical_sector = 512;
    FILE *f = fopen(argv[3], "rb");
    if (f) { fseek(f, 0, SEEK_END); m.disk[0].bytes = ftell(f); fclose(f); }
    m.n_disks = 1;

    image_src s;
    int r = image_find(&m, argv[4], &s, why, sizeof why);
    printf("find=%d\n", r);
    printf("why=%s\n", why);
    if (r != 0) return 1;
    printf("profile=%s\n", s.profile);
    printf("root_len=%llu\n", (unsigned long long)s.root_len);
    if (!strcmp(argv[1], "verify")) {
        int v = image_verify(&s, pct, why, sizeof why);
        printf("verify=%d\n", v);
        printf("why=%s\n", why);
        return v ? 1 : 0;
    }
    return 0;
}
EOC
gcc -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I src/aurstage -o "$TMP/i" "$TMP/i.c" \
    src/aurstage/image.c src/aurstage/gpt.c src/aurstage/wr.c \
    src/aurstage/sha256.c src/aurstage/boot.c src/aurstage/disks.c \
    src/aurstage/ntfs.c src/aurstage/fde.c \
    || { echo "  did not build"; exit 2; }

# The container's loop module has max_part=0, so partition nodes never
# appear. The whole stick is attached and a second loop device is
# pointed at the partition by hand.
attach() { losetup --find --show "$1"; }
attach_part() { losetup --find --show -o "$2" "$1"; }

G() { sed -n "s/^$1=//p"; }

echo
echo "  finding it"
mkstick "$TMP/stick.img" "desktop"
L=$(attach "$TMP/stick.img") || { echo "  no loop devices"; exit 2; }
NAME=$(basename "$L")
OUT=$("$TMP/i" find "$NAME" "$TMP/stick.img" desktop 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G find)" = "0" ] && ok "the stick is found by what is on it" \
    || bad "the stick is found by what is on it" "$(printf '%s\n' "$OUT" | G why)"
[ "$(printf '%s\n' "$OUT" | G profile)" = "desktop" ] && ok "...and names its profile" \
    || bad "...and names its profile"
[ "$(printf '%s\n' "$OUT" | G root_len)" = "$RLEN" ] && ok "...and the root extent's length" \
    || bad "...and the root extent's length" "got $(printf '%s\n' "$OUT" | G root_len)"

OUT=$("$TMP/i" find "$NAME" "$TMP/stick.img" kiosk 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G find)" = "-1" ] && ok "a stick for another profile is not used" \
    || bad "a stick for another profile is not used"
case "$(printf '%s\n' "$OUT" | G why)" in
  *"different version"*) ok "...and says so" ;;
  *) bad "...and says so" "it said: $(printf '%s\n' "$OUT" | G why)" ;;
esac

# AND A JOURNAL THAT NAMES NOTHING TAKES WHAT IT FINDS, which is the
# only protection every stick made before the journal carried a profile
# has, and which was claimed in four files and tested in none.
OUT=$("$TMP/i" find "$NAME" "$TMP/stick.img" "" 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G find)" = "0" ] \
  && ok "a journal from before profiles were recorded still installs" \
  || bad "a journal from before profiles were recorded still installs" \
        "$(printf '%s\n' "$OUT" | G why)"
losetup -d "$L"

# ── two images on one stick ─────────────────────────────────────────
echo
echo "  and when the stick holds more than one"
mkstick2 "$TMP/two.img" office desktop
L=$(attach "$TMP/two.img"); NAME=$(basename "$L")
OUT=$("$TMP/i" find "$NAME" "$TMP/two.img" desktop 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G find)" = "0" ] \
  && ok "the one the journal names is the one that is found" \
  || bad "the one the journal names is the one that is found" \
        "$(printf '%s\n' "$OUT" | G why)"
[ "$(printf '%s\n' "$OUT" | G profile)" = "desktop" ] \
  && ok "...and it is the second one, so the search did not stop at the first" \
  || bad "...and it is the second one, so the search did not stop at the first" \
        "got $(printf '%s\n' "$OUT" | G profile)"
losetup -d "$L"

# A BROKEN MANIFEST ON THE ONE NOBODY ASKED ABOUT. The two structural
# checks above the profile comparison used to `return -1` for any
# candidate, so a damaged sibling refused the whole machine and the
# image the journal named was never reached.
mkstick2 "$TMP/twobad.img" office desktop break-first
L=$(attach "$TMP/twobad.img"); NAME=$(basename "$L")
OUT=$("$TMP/i" find "$NAME" "$TMP/twobad.img" desktop 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G find)" = "0" ] \
  && ok "a damaged image nobody asked for does not refuse the one they did" \
  || bad "a damaged image nobody asked for does not refuse the one they did" \
        "$(printf '%s\n' "$OUT" | G why)"
# ...and it is still caught when it IS the one asked for.
OUT=$("$TMP/i" find "$NAME" "$TMP/twobad.img" office 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G find)" = "-1" ] \
  && ok "...and is still refused when it is" \
  || bad "...and is still refused when it is"
losetup -d "$L"
L=$(attach "$TMP/stick.img"); NAME=$(basename "$L")

echo
echo "  checking it"
OUT=$("$TMP/i" verify "$NAME" "$TMP/stick.img" desktop 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verify)" = "0" ] && ok "an intact image verifies" \
    || bad "an intact image verifies" "$(printf '%s\n' "$OUT" | G why)"
losetup -d "$L"

mkstick "$TMP/bad.img" "desktop" corrupt
L2=$(attach "$TMP/bad.img"); N2=$(basename "$L2")
OUT=$("$TMP/i" verify "$N2" "$TMP/bad.img" desktop 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verify)" = "-1" ] && ok "one flipped byte is caught" \
    || bad "one flipped byte is caught"
case "$(printf '%s\n' "$OUT" | G why)" in
  *damaged*) ok "...and is called damaged, not missing" ;;
  *) bad "...and is called damaged, not missing" "it said: $(printf '%s\n' "$OUT" | G why)" ;;
esac
losetup -d "$L2"

# AND A FLIP IN THE PART THAT STARTS A COMPUTER, which is a different
# extent and used to be checked by nothing: the installer copies it
# onto the machine and reads it back against the stick -- the same
# bytes it just wrote -- so rot in the shim installed cleanly, said so,
# and left a machine that starts nothing.
mkstick "$TMP/badesp.img" "desktop" esp
LE=$(attach "$TMP/badesp.img")
NE=$(basename "$LE")
OUT=$("$TMP/i" verify "$NE" "$TMP/badesp.img" desktop 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G verify)" = "-1" ] \
  && ok "a flipped byte in the boot chain is caught too" \
  || bad "a flipped byte in the boot chain is caught too"
case "$(printf '%s\n' "$OUT" | G why)" in
  *"starts a computer"*) ok "...and is named as the start-up files" ;;
  *) bad "...and is named as the start-up files" \
         "it said: $(printf '%s\n' "$OUT" | G why)" ;;
esac
losetup -d "$LE"

mkstick "$TMP/lie.img" "desktop" "" lie
L3=$(attach "$TMP/lie.img"); N3=$(basename "$L3")
OUT=$("$TMP/i" find "$N3" "$TMP/lie.img" desktop 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G find)" = "-1" ] && ok "a manifest that disagrees with the image is caught" \
    || bad "a manifest that disagrees with the image is caught"
case "$(printf '%s\n' "$OUT" | G why)" in
  *"do not agree"*) ok "...which is our own build being wrong, and it says so" ;;
  *) bad "...which is our own build being wrong, and it says so" \
         "it said: $(printf '%s\n' "$OUT" | G why)" ;;
esac
losetup -d "$L3"

echo
echo "  writing it"
PS=$(sgdisk -i 1 "$TMP/stick.img" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
LW=$(attach "$TMP/stick.img")
TGT="$TMP/target.img"; truncate -s 256M "$TGT"
dd if=/dev/urandom of="$TGT" bs=1M count=256 conv=notrunc status=none
DST=$((32 * 1024 * 1024))
OUT=$("$TMP/i" write "$LW" $((PS * 512)) "$TGT" "$DST" 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G write)" = "0" ] && ok "the root extent is written and verified" \
    || bad "the root extent is written and verified" "$(printf '%s\n' "$OUT" | G why)"

# The written region must equal the source root extent exactly.
dd if="$TGT" bs=1M skip=$((DST / 1048576)) count=$((RLEN / 1048576)) \
   status=none > "$TMP/out.bin"
if cmp -s "$TMP/out.bin" "$TMP/root.img"; then
    ok "...byte for byte the same as the source"
else
    bad "...byte for byte the same as the source"
fi
if mount -o loop,ro "$TMP/out.bin" "$TMP/m" 2>/dev/null; then
    [ -f "$TMP/m/MARKER" ] && ok "...and it mounts, with its files in it" \
                           || bad "...and it mounts, with its files in it"
    umount "$TMP/m"
else
    bad "...and it mounts, with its files in it" "it does not mount"
fi

echo
echo "  and the first megabyte is written LAST"
# Arm a window that is too small, so the write fails partway. At that
# moment the first megabyte must still be zeroes: a partially written
# install must never be a bootable-LOOKING install.
TGT2="$TMP/target2.img"; truncate -s 256M "$TGT2"
dd if=/dev/urandom of="$TGT2" bs=1M count=256 conv=notrunc status=none
SHORT=$((RLEN / 2))
OUT=$("$TMP/i" write "$LW" $((PS * 512)) "$TGT2" "$DST" "$SHORT" 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G write)" = "-1" ] && ok "a write that runs out of room is refused" \
    || bad "a write that runs out of room is refused"
NZ=$(dd if="$TGT2" bs=1M skip=$((DST / 1048576)) count=1 status=none | tr -d '\0' | wc -c)
[ "$NZ" = "0" ] && ok "...and the first megabyte is all zeroes, not a filesystem" \
                || bad "...and the first megabyte is all zeroes, not a filesystem" \
                       "$NZ non-zero bytes"
if mount -o loop,ro,offset=$DST "$TGT2" "$TMP/m" 2>/dev/null; then
    umount "$TMP/m"
    bad "...and nothing will mount it" "it mounted, which is the whole problem"
else
    ok "...and nothing will mount it"
fi
losetup -d "$LW"

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    exit 1
fi
echo "$checked checks: the image is found by what is on it, a single flipped"
echo "byte is caught, and an interrupted write leaves nothing that will mount."
exit 0
