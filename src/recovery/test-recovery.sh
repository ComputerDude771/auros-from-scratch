#!/bin/bash
# Destroy-and-restore test for mkrecovery.
#
# Builds a synthetic disk with a Windows-like layout, captures it,
# deliberately destroys the partition table the way a failed install
# would, restores, and requires the result to be byte-identical.
# "We have a backup" is worth nothing until the restore has been run.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK=$(mktemp -d)
IMG="$WORK/disk.img"
DEST="$WORK/recovery"
pass=0; fail=0
ok()   { printf '  \033[38;2;125;211;192mPASS\033[0m %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[38;2;242;120;141mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
cleanup() { losetup -D 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

echo "== building a synthetic Windows-like disk =="
truncate -s 2G "$IMG"
sgdisk --zap-all "$IMG" >/dev/null
sgdisk -n 1:2048:+100M  -t 1:ef00 -c 1:"EFI system partition" "$IMG" >/dev/null
sgdisk -n 2:0:+16M      -t 2:0c01 -c 2:"Microsoft reserved"   "$IMG" >/dev/null
sgdisk -n 3:0:+1500M    -t 3:0700 -c 3:"Basic data"           "$IMG" >/dev/null
sgdisk -n 4:0:0         -t 4:2700 -c 4:"Recovery"             "$IMG" >/dev/null
sgdisk -p "$IMG" | tail -5

# Put identifiable content in the ESP so a restore can be checked.
P1_OFF=$(( $(sgdisk -i 1 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p') * 512 ))
P1_SZ=$((  ( $(sgdisk -i 1 "$IMG" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p') - $(sgdisk -i 1 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p') + 1 ) * 512 ))
L1=$(losetup --find --show --offset "$P1_OFF" --sizelimit "$P1_SZ" "$IMG")
mkfs.vfat -F32 -n "SYSTEM" "$L1" >/dev/null 2>&1
export MTOOLS_SKIP_CHECK=1
mmd -i "$L1" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot 2>/dev/null || true
printf 'WINDOWS BCD SENTINEL' > "$WORK/BCD"
mcopy -i "$L1" -o "$WORK/BCD" ::/EFI/Microsoft/Boot/BCD 2>/dev/null
losetup -d "$L1"

ORIG_GPT_SHA=$(dd if="$IMG" bs=512 count=2048 status=none | sha256sum | cut -d' ' -f1)
ORIG_ESP_SHA=$(dd if="$IMG" bs=512 skip=$((P1_OFF/512)) count=$((P1_SZ/512)) status=none | sha256sum | cut -d' ' -f1)
echo "  original GPT sha: ${ORIG_GPT_SHA:0:16}…"

echo
echo "== capture =="
LOOP=$(losetup --find --show "$IMG")
"$HERE/mkrecovery" capture "$LOOP" "$DEST" 2>&1 | sed 's/^/  /'
[ -f "$DEST/gpt-primary.bin" ] && ok "primary GPT captured" || bad "primary GPT missing"
[ -f "$DEST/gpt-backup.bin" ]  && ok "backup GPT captured"  || bad "backup GPT missing"
[ -f "$DEST/SHA256SUMS" ]      && ok "hashes written"       || bad "no hashes"
[ -s "$DEST/layout.txt" ]      && ok "human-readable layout kept" || bad "no layout.txt"
grep -q "esp" "$DEST/recovery.json" && ok "manifest written" || bad "no manifest"

echo
echo "== verify =="
"$HERE/mkrecovery" verify "$DEST" >/dev/null 2>&1 && ok "verify passes on intact capture" || bad "verify failed on intact capture"

# A corrupted capture must be REFUSED, not used.
cp "$DEST/gpt-primary.bin" "$WORK/keep.bin"
printf 'X' | dd of="$DEST/gpt-primary.bin" bs=1 seek=600 conv=notrunc status=none
if "$HERE/mkrecovery" verify "$DEST" >/dev/null 2>&1; then
    bad "verify ACCEPTED a corrupted capture"
else
    ok "verify refuses a corrupted capture"
fi
cp "$WORK/keep.bin" "$DEST/gpt-primary.bin"

echo
echo "== destroy the disk the way a failed install would =="
# Wipe the partition table and the backup: the classic unbootable machine.
dd if=/dev/zero of="$IMG" bs=512 count=2048 conv=notrunc status=none
SZ=$(stat -c %s "$IMG")
dd if=/dev/zero of="$IMG" bs=512 seek=$(( SZ/512 - 34 )) count=34 conv=notrunc status=none
sync
if sgdisk -p "$IMG" 2>&1 | grep -qiE "Basic data|Recovery"; then
    bad "disk was not actually destroyed — test is meaningless"
else
    ok "disk destroyed (no partitions readable)"
fi

echo
echo "== restore =="
"$HERE/mkrecovery" restore "$LOOP" "$DEST" 2>&1 | sed 's/^/  /'
sync

NEW_GPT_SHA=$(dd if="$IMG" bs=512 count=2048 status=none | sha256sum | cut -d' ' -f1)
[ "$NEW_GPT_SHA" = "$ORIG_GPT_SHA" ] && ok "GPT restored byte-identical" \
    || bad "GPT differs after restore"

sgdisk -p "$IMG" 2>/dev/null | grep -q "Basic data" && ok "partitions readable again" \
    || bad "partitions still unreadable"

NEW_ESP_SHA=$(dd if="$IMG" bs=512 skip=$((P1_OFF/512)) count=$((P1_SZ/512)) status=none | sha256sum | cut -d' ' -f1)
[ "$NEW_ESP_SHA" = "$ORIG_ESP_SHA" ] && ok "ESP untouched by the destroy+restore cycle" \
    || bad "ESP content changed"

# The BCD sentinel must still be findable — this is what Windows needs.
L1=$(losetup --find --show --offset "$P1_OFF" --sizelimit "$P1_SZ" "$IMG")
if mtype -i "$L1" ::/EFI/Microsoft/Boot/BCD 2>/dev/null | grep -q "WINDOWS BCD SENTINEL"; then
    ok "Windows BCD intact and readable"
else
    bad "BCD lost"
fi
losetup -d "$L1"
losetup -d "$LOOP"

echo
printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
