#!/bin/bash
# Destroy-and-restore test for mkrecovery.
#
# Builds a synthetic disk with a Windows-like layout, captures it,
# deliberately destroys the partition table the way a failed install
# would, restores, and requires the result to be byte-identical.
# "We have a backup" is worth nothing until the restore has been run.
#
# Then the failures that matter more than the happy path, because a
# backup tool is judged on what it does when something is wrong:
#   - a capture that cannot see the partition table must REFUSE, not
#     write a smaller capture and hash it;
#   - a capture with a hole in it must fail verification even though
#     every file it does contain is intact;
#   - a disk whose geometry has moved under the capture must not be
#     allowed into a destructive phase;
#   - a BitLocker volume's partition entry must never be resized;
#   - and a completed shrink must be grown back, or "Put Windows back"
#     hands the user a partition bigger than the filesystem in it.
#
# ntfsprogs is not installed in this container. NTFS volumes here are
# therefore synthesised boot sectors — a real BPB, a real sector count, a
# real backup boot sector in the volume's last sector — and ntfsresize is
# a stub that does the two things mkrecovery can observe the real one
# doing: rewrite the sector count and move the backup boot sector.
# Everything mkrecovery itself reads and writes is real.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MK="$HERE/mkrecovery"
WORK=$(mktemp -d)
IMG="$WORK/disk.img"
DEST="$WORK/recovery"
pass=0; fail=0
ok()   { printf '  \033[38;2;125;211;192mPASS\033[0m %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[38;2;242;120;141mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
cleanup() { losetup -D 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

# refuses "description" CMD...  — the command MUST fail. A recovery tool
# that says yes when it should say no is the bug this suite exists for.
refuses() {
    local what=$1; shift
    if "$@" >/dev/null 2>&1; then bad "$what — IT WENT AHEAD"; else ok "$what"; fi
}
allows() {
    local what=$1; shift
    if "$@" >/dev/null 2>&1; then ok "$what"; else bad "$what — it refused"; fi
}

# ── synthetic NTFS, because ntfsprogs is not here ──────────────────────
put_le() {  # VALUE WIDTH FILE BYTE-OFFSET
    local v=$1 w=$2 f=$3 off=$4 i b out=""
    for ((i=0;i<w;i++)); do b=$(( (v >> (8*i)) & 255 )); out+=$(printf '\\%03o' "$b"); done
    printf '%b' "$out" | dd of="$f" bs=1 seek="$off" conv=notrunc status=none
}
get_le() { od -A n -t u"$1" -j "$3" -N "$1" "$2" | tr -d ' \n'; }
oem_at() { dd if="$1" bs="${3:-512}" skip="$2" count=1 status=none | dd bs=1 skip=3 count=8 status=none; }

# A volume whose boot sector says what a real NTFS boot sector says, with
# the bootstrap sectors filled with identifiable content so a $Boot
# restore can be checked for all 16 sectors and not just the first.
fake_ntfs() {  # IMG FIRST_LBA LAST_LBA [SECTOR_SIZE] [BPB_SECTOR_SIZE]
    local img=$1 first=$2 tot=$(( $3 - $2 )) ss=${4:-512} bpb=${5:-${4:-512}}
    local bs="$WORK/bs.bin" i
    dd if=/dev/zero of="$bs" bs="$ss" count=1 status=none
    printf '\353\122\220' | dd of="$bs" bs=1 seek=0   conv=notrunc status=none
    printf 'NTFS    '     | dd of="$bs" bs=1 seek=3   conv=notrunc status=none
    put_le "$bpb" 2 "$bs" 11          # bytes per sector
    put_le 8      1 "$bs" 13          # sectors per cluster
    put_le "$tot" 8 "$bs" 40          # total sectors: NTFS does not count
                                      # its own backup boot sector
    put_le 4      8 "$bs" 48          # $MFT cluster
    printf '\125\252'     | dd of="$bs" bs=1 seek=510 conv=notrunc status=none
    dd if="$bs" of="$img" bs="$ss" seek="$first" conv=notrunc status=none
    for ((i=1;i<16;i++)); do
        printf 'AUROS-NTFS-BOOTSTRAP-SENTINEL-%02d' "$i" \
            | dd of="$img" bs="$ss" seek=$(( first + i )) conv=notrunc status=none
    done
    dd if="$bs" of="$img" bs="$ss" seek=$(( first + tot )) conv=notrunc status=none
}
# A volume that is BitLocker-encrypted: the signature at offset 3 is the
# whole of the detection, in mkrecovery and in aurbridge's preflight.
fake_bitlocker() {  # IMG FIRST_LBA
    local bs="$WORK/fve.bin"
    dd if=/dev/zero of="$bs" bs=512 count=1 status=none
    printf '\353\130\220' | dd of="$bs" bs=1 seek=0   conv=notrunc status=none
    printf -- '-FVE-FS-'  | dd of="$bs" bs=1 seek=3   conv=notrunc status=none
    printf '\125\252'     | dd of="$bs" bs=1 seek=510 conv=notrunc status=none
    dd if="$bs" of="$1" bs=512 seek="$2" conv=notrunc status=none
}
first_of() { sgdisk -i "$1" "$2" | sed -n 's/^First sector: \([0-9]*\).*/\1/p'; }
last_of()  { sgdisk -i "$1" "$2" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p'; }

# The stand-in for ntfsresize. Real ntfsresize moves file data; nothing
# mkrecovery does depends on that, and everything it does depend on —
# the sector count in the boot sector and the position of the backup
# boot sector — is here.
mkdir -p "$WORK/stub"
cat > "$WORK/stub/ntfsresize" <<'STUB'
#!/bin/bash
set -eu
size=""; dev=""; noact=0
while [ $# -gt 0 ]; do
  case "$1" in
    --size)   size=$2; shift ;;
    --size=*) size=${1#--size=} ;;
    -n|--no-action) noact=1 ;;
    -*) ;;
    *) dev=$1 ;;
  esac
  shift
done
[ -n "$dev" ] || { echo "ntfsresize: no device given" >&2; exit 1; }
[ "$(dd if="$dev" bs=1 skip=3 count=8 status=none)" = "NTFS    " ] \
    || { echo "ntfsresize: not an NTFS volume" >&2; exit 1; }
bps=$(od -A n -t u2 -j 11 -N 2 "$dev" | tr -d ' \n')
cur=$(od -A n -t u8 -j 40 -N 8 "$dev" | tr -d ' \n')
if [ -b "$dev" ]; then devsz=$(blockdev --getsize64 "$dev"); else devsz=$(stat -c %s "$dev"); fi
new=$(( size / bps ))
[ "$size" -le "$devsz" ] || { echo "ntfsresize: $size bytes will not fit in $devsz" >&2; exit 1; }
echo "ntfsresize (stub): volume is $cur sectors, asked for $new"
[ "$noact" -eq 0 ] || { echo "ntfsresize (stub): --no-action, nothing changed"; exit 0; }
b=$(mktemp); dd if="$dev" bs="$bps" count=1 status=none of="$b"
out=""; for ((i=0;i<8;i++)); do out+=$(printf '\\%03o' $(( (new >> (8*i)) & 255 ))); done
printf '%b' "$out" | dd of="$b" bs=1 seek=40 conv=notrunc status=none
dd if="$b" of="$dev" bs="$bps" seek=0      conv=notrunc status=none
dd if="$b" of="$dev" bs="$bps" seek="$new" conv=notrunc status=none
rm -f "$b"; sync
echo "ntfsresize (stub): successfully resized to $new sectors"
STUB
chmod +x "$WORK/stub/ntfsresize"
# Two reduced PATHs: one with nothing but the tools mkrecovery needs,
# and the same again plus sgdisk. Shadowing a tool is not possible, so
# absence has to be built rather than arranged.
mkdir -p "$WORK/nosgdisk" "$WORK/nosgdisk-with-sgdisk"
for t in sh dd stat od cmp sha256sum find xargs wc du cut date cat sed awk tr \
         mktemp mkdir rm sync printf blockdev losetup grep mdir mcopy; do
    if p=$(command -v "$t" 2>/dev/null); then
        ln -sf "$p" "$WORK/nosgdisk/$t"
        ln -sf "$p" "$WORK/nosgdisk-with-sgdisk/$t"
    fi
done
ln -sf "$(command -v sgdisk)" "$WORK/nosgdisk-with-sgdisk/sgdisk"

command -v ntfsresize >/dev/null 2>&1 \
    && echo "note: the real ntfsresize is installed and will NOT be used; the stub shadows it" \
    || echo "note: ntfsprogs is absent — NTFS volumes are synthesised, ntfsresize is stubbed"

echo "== building a synthetic Windows-like disk =="
truncate -s 2G "$IMG"
sgdisk --zap-all "$IMG" >/dev/null
sgdisk -n 1:2048:+100M  -t 1:ef00 -c 1:"EFI system partition" "$IMG" >/dev/null
sgdisk -n 2:0:+16M      -t 2:0c01 -c 2:"Microsoft reserved"   "$IMG" >/dev/null
sgdisk -n 3:0:+1500M    -t 3:0700 -c 3:"Basic data"           "$IMG" >/dev/null
sgdisk -n 4:0:0         -t 4:2700 -c 4:"Recovery"             "$IMG" >/dev/null
sgdisk -p "$IMG" | tail -5

# Put identifiable content in the ESP so a restore can be checked.
P1_OFF=$(( $(first_of 1 "$IMG") * 512 ))
P1_SZ=$((  ( $(last_of 1 "$IMG") - $(first_of 1 "$IMG") + 1 ) * 512 ))
L1=$(losetup --find --show --offset "$P1_OFF" --sizelimit "$P1_SZ" "$IMG")
mkfs.vfat -F32 -n "SYSTEM" "$L1" >/dev/null 2>&1
export MTOOLS_SKIP_CHECK=1
mmd -i "$L1" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot 2>/dev/null || true
printf 'WINDOWS BCD SENTINEL' > "$WORK/BCD"
mcopy -i "$L1" -o "$WORK/BCD" ::/EFI/Microsoft/Boot/BCD 2>/dev/null
losetup -d "$L1"

# And a Windows volume, because the partition table alone cannot put
# Windows back: the filesystem inside it has its own idea of its size.
P3_FIRST=$(first_of 3 "$IMG"); P3_LAST=$(last_of 3 "$IMG")
fake_ntfs "$IMG" "$P3_FIRST" "$P3_LAST"
ORIG_FS_SECTORS=$(get_le 8 "$IMG" $(( P3_FIRST * 512 + 40 )))
echo "  C: is partition 3, LBA $P3_FIRST-$P3_LAST, filesystem $ORIG_FS_SECTORS sectors"

ORIG_GPT_SHA=$(dd if="$IMG" bs=512 count=2048 status=none | sha256sum | cut -d' ' -f1)
ORIG_ESP_SHA=$(dd if="$IMG" bs=512 skip=$((P1_OFF/512)) count=$((P1_SZ/512)) status=none | sha256sum | cut -d' ' -f1)
ORIG_BOOT_SHA=$(dd if="$IMG" bs=512 skip="$P3_FIRST" count=16 status=none | sha256sum | cut -d' ' -f1)
echo "  original GPT sha: ${ORIG_GPT_SHA:0:16}…"

echo
echo "== capture =="
LOOP=$(losetup --find --show "$IMG")
"$MK" capture "$LOOP" "$DEST" 2>&1 | sed 's/^/  /'
[ -f "$DEST/gpt-primary.bin" ] && ok "primary GPT captured" || bad "primary GPT missing"
[ -f "$DEST/gpt-backup.bin" ]  && ok "backup GPT captured"  || bad "backup GPT missing"
[ -f "$DEST/SHA256SUMS" ]      && ok "hashes written"       || bad "no hashes"
[ -s "$DEST/layout.txt" ]      && ok "human-readable layout kept" || bad "no layout.txt"
grep -q "esp" "$DEST/recovery.json" && ok "manifest written" || bad "no manifest"

# The layout, machine-readable: this is what proves later that nothing
# has moved, so every partition has to be in it.
[ "$(grep -vc '^#' "$DEST/parts.tab")" -eq 4 ] \
    && ok "all four partitions recorded in parts.tab" || bad "parts.tab is incomplete"
grep -q '^3 .* ntfs$' "$DEST/parts.tab" \
    && ok "partition 3 recorded as holding NTFS" || bad "the Windows volume was not identified"

# Finding 3: none of this is in the GPT, and without it a restore after a
# successful shrink cannot know how big C: used to be.
awk -v n="$ORIG_FS_SECTORS" '$1=="3" && $4==n {f=1} END{exit !f}' "$DEST/ntfs.tab" \
    && ok "original filesystem sector count recorded ($ORIG_FS_SECTORS)" \
    || bad "ntfs.tab does not record the original filesystem size"
awk -v s="$P3_FIRST" '$1=="3" && $2==s {f=1} END{exit !f}' "$DEST/ntfs.tab" \
    && ok "original volume start LBA recorded ($P3_FIRST)" \
    || bad "ntfs.tab does not record where the volume started"
if [ "$(sha256sum < "$DEST/ntfs-3-boot.bin" | cut -d' ' -f1)" = "$ORIG_BOOT_SHA" ]; then
    ok "NTFS \$Boot captured, all 16 sectors, byte-identical"
else
    bad "\$Boot not captured (or not all 16 sectors of it)"
fi
BAK_LBA=$(awk '$1=="3"{print $7}' "$DEST/ntfs.tab")
if [ "$BAK_LBA" = "$P3_LAST" ] && [ "$(oem_at "$DEST/ntfs-3-backup-boot.bin" 0)" = "NTFS    " ]; then
    ok "NTFS backup boot sector captured from the volume's last sector (LBA $BAK_LBA)"
else
    bad "the backup boot sector was not captured from the end of the volume"
fi
# It used to be written after the hashes, so the one file describing the
# capture was the one file nobody checked.
awk '$2=="./recovery.json"{f=1} END{exit !f}' "$DEST/SHA256SUMS" \
    && ok "recovery.json is itself hashed" || bad "recovery.json is outside SHA256SUMS"

echo
echo "== verify =="
"$MK" verify "$DEST" >/dev/null 2>&1 && ok "verify passes on intact capture" || bad "verify failed on intact capture"

# A corrupted capture must be REFUSED, not used.
cp "$DEST/gpt-primary.bin" "$WORK/keep.bin"
printf 'X' | dd of="$DEST/gpt-primary.bin" bs=1 seek=600 conv=notrunc status=none
if "$MK" verify "$DEST" >/dev/null 2>&1; then
    bad "verify ACCEPTED a corrupted capture"
else
    ok "verify refuses a corrupted capture"
fi
cp "$WORK/keep.bin" "$DEST/gpt-primary.bin"

# And an INCOMPLETE one: every file present, every hash correct, and the
# ESP simply not there. This is the shape of the bug that matters —
# intact is not the same as complete.
cp -a "$DEST" "$WORK/holey"; rm -f "$WORK/holey/esp.img"
refuses "verify refuses a capture with the ESP missing" "$MK" verify "$WORK/holey"
cp -a "$DEST" "$WORK/holey2"; rm -f "$WORK/holey2/ntfs-3-boot.bin"
refuses "verify refuses a capture with no NTFS \$Boot" "$MK" verify "$WORK/holey2"
refuses "capture refuses to write over an existing capture" "$MK" capture "$LOOP" "$DEST"

# The journal is the one file that legitimately changes after the
# capture, because the installer writes its progress into it. If it were
# hashed, the gate below would refuse every run from phase 2 onward.
printf '{"phase":"PREPARE"}\n' > "$DEST/journal.json"
allows "verify still passes after the installer updates journal.json" \
       "$MK" verify "$DEST"

echo
echo "== the gate, which is what makes 'verify before destruction' real =="
allows  "gate passes on the disk the capture came from" "$MK" gate "$LOOP" "$DEST"
cp --sparse=always "$IMG" "$WORK/moved.img"
P4_FIRST=$(first_of 4 "$IMG")
sgdisk -d 4 "$WORK/moved.img" >/dev/null
sgdisk -n 4:$(( P4_FIRST + 2048 )):0 -t 4:2700 "$WORK/moved.img" >/dev/null
refuses "gate refuses once a captured partition has moved" \
        "$MK" gate "$WORK/moved.img" "$DEST"
cp --sparse=always "$IMG" "$WORK/clobbered.img"
dd if=/dev/zero of="$WORK/clobbered.img" bs=512 seek="$P3_FIRST" count=1 conv=notrunc status=none
refuses "gate refuses once something has been written over the start of C:" \
        "$MK" gate "$WORK/clobbered.img" "$DEST"
truncate -s 3G "$WORK/other.img"; sgdisk --zap-all "$WORK/other.img" >/dev/null 2>&1
refuses "gate refuses a disk that is not the one captured" \
        "$MK" gate "$WORK/other.img" "$DEST"

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
"$MK" restore "$LOOP" "$DEST" 2>&1 | sed 's/^/  /'
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

[ "$(get_le 8 "$IMG" $(( P3_FIRST * 512 + 40 )))" = "$ORIG_FS_SECTORS" ] \
    && ok "C: filesystem still its original size after the cycle" \
    || bad "the filesystem size changed"

echo
echo "== a COMPLETED shrink, then put Windows back =="
# Phase 4 shrinks the filesystem and leaves the partition entry alone;
# phase 8 commits a new table over the freed space. That is the state a
# real "Put Windows back" has to undo, and restoring the old GPT alone
# leaves a 1500MB partition with a 750MB filesystem in it.
SHRUNK=$(( ORIG_FS_SECTORS / 2 ))
L3=$(losetup --find --show --offset $(( P3_FIRST * 512 )) \
        --sizelimit $(( (P3_LAST - P3_FIRST + 1) * 512 )) "$IMG")
"$WORK/stub/ntfsresize" --force --size $(( SHRUNK * 512 )) "$L3" | sed 's/^/  /'
losetup -d "$L3"
[ "$(get_le 8 "$IMG" $(( P3_FIRST * 512 + 40 )))" = "$SHRUNK" ] \
    && ok "shrink simulated: filesystem is now $SHRUNK sectors" \
    || bad "the shrink did not take — the rest of this section is meaningless"
# The gate has to pass HERE. Phase 8 runs after the shrink, and a gate
# that refused a legitimately shrunken filesystem would block the
# install rather than protect it.
allows "gate still passes after the shrink, before the commit" \
       "$MK" gate "$LOOP" "$DEST"

# And if the filesystem cannot be grown back, saying so is the only
# honest outcome: a restore that silently leaves C: half its size has
# lied to someone who is already having a bad day.
cp --sparse=always "$IMG" "$WORK/nogrow.img"
refuses "restore refuses to finish silently when ntfsresize is missing" \
        env PATH="$WORK/nosgdisk-with-sgdisk" "$MK" restore "$WORK/nogrow.img" "$DEST"

sgdisk -d 4 -d 3 "$IMG" >/dev/null
sgdisk -n 3:"$P3_FIRST":$(( P3_FIRST + SHRUNK )) -t 3:0700 -c 3:"Basic data" "$IMG" >/dev/null
sgdisk -n 5:0:0 -t 5:8300 -c 5:"AurOS" "$IMG" >/dev/null
sgdisk -p "$IMG" | grep -q "AurOS" && ok "new partition table committed over the freed space" \
    || bad "the fake install did not repartition"

PATH="$WORK/stub:$PATH" "$MK" restore "$LOOP" "$DEST" 2>&1 | sed 's/^/  /'
sync
[ "$(get_le 8 "$IMG" $(( P3_FIRST * 512 + 40 )))" = "$ORIG_FS_SECTORS" ] \
    && ok "filesystem grown back to its original $ORIG_FS_SECTORS sectors" \
    || bad "C: is still the shrunken size — the user's drive shrank permanently"
[ "$(oem_at "$IMG" "$P3_LAST")" = "NTFS    " ] \
    && ok "backup boot sector back in the volume's last sector (LBA $P3_LAST)" \
    || bad "no NTFS backup boot sector at the end of the restored volume"
sgdisk -p "$IMG" 2>/dev/null | grep -q "Recovery" \
    && ok "the original partition table is back, AurOS's partition gone" \
    || bad "the partition table was not restored"

echo
echo "== the head of C: overwritten =="
# The other half of the same problem: a failed install that wrote over
# the start of the volume. $Boot is the only copy left.
cp --sparse=always "$IMG" "$WORK/head.img"
dd if=/dev/zero of="$WORK/head.img" bs=512 seek="$P3_FIRST" count=16 conv=notrunc status=none
PATH="$WORK/stub:$PATH" "$MK" restore "$WORK/head.img" "$DEST" >/dev/null 2>&1 \
    && ok "restore completes with the head of C: destroyed" \
    || bad "restore could not handle a destroyed volume header"
if [ "$(dd if="$WORK/head.img" bs=512 skip="$P3_FIRST" count=16 status=none | sha256sum | cut -d' ' -f1)" = "$ORIG_BOOT_SHA" ]; then
    ok "\$Boot restored byte-identical, bootstrap sectors and all"
else
    bad "\$Boot was not restored"
fi

echo
echo "== BitLocker: the MUST-NOT =="
BL="$WORK/bl.img"; BLDEST="$WORK/bl-recovery"
truncate -s 256M "$BL"
sgdisk --zap-all "$BL" >/dev/null
sgdisk -n 1:2048:+16M -t 1:ef00 -c 1:"EFI system partition" "$BL" >/dev/null
sgdisk -n 2:0:+100M   -t 2:0700 -c 2:"Basic data"           "$BL" >/dev/null
sgdisk -n 3:0:0       -t 3:0700 -c 3:"Encrypted data"       "$BL" >/dev/null
BL1S=$(first_of 1 "$BL"); BL1E=$(last_of 1 "$BL")
L1=$(losetup --find --show --offset $(( BL1S * 512 )) --sizelimit $(( (BL1E-BL1S+1)*512 )) "$BL")
mkfs.vfat -F32 "$L1" >/dev/null 2>&1
mmd -i "$L1" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot 2>/dev/null || true
mcopy -i "$L1" -o "$WORK/BCD" ::/EFI/Microsoft/Boot/BCD 2>/dev/null
losetup -d "$L1"
fake_ntfs "$BL" "$(first_of 2 "$BL")" "$(last_of 2 "$BL")"
BL3S=$(first_of 3 "$BL")
fake_bitlocker "$BL" "$BL3S"

"$MK" capture "$BL" "$BLDEST" >/dev/null 2>&1 \
    && ok "capture completes on a disk with an encrypted volume" \
    || bad "capture failed on a BitLocker disk"
grep -q '^3 .* bitlocker$' "$BLDEST/parts.tab" \
    && ok "the encrypted volume is recorded as BitLocker in parts.tab" \
    || bad "BitLocker volume not identified"
refuses "fve-check refuses the BitLocker volume's geometry" \
        "$MK" fve-check "$BL" "$BL3S"
allows  "fve-check allows an ordinary NTFS volume's geometry" \
        "$MK" fve-check "$BL" "$(first_of 2 "$BL")"
allows  "restore proceeds while the encrypted partition is untouched" \
        "$MK" restore "$BL" "$BLDEST"

# Now the two-line mistake: something truncates the encrypted partition.
cp --sparse=always "$BL" "$WORK/bl-bad.img"
sgdisk -d 3 "$WORK/bl-bad.img" >/dev/null
sgdisk -n 3:"$BL3S":$(( BL3S + 40000 )) -t 3:0700 "$WORK/bl-bad.img" >/dev/null
refuses "gate refuses once a BitLocker partition entry has been resized" \
        "$MK" gate "$WORK/bl-bad.img" "$BLDEST"
refuses "restore refuses to resize a BitLocker partition entry, even back" \
        "$MK" restore "$WORK/bl-bad.img" "$BLDEST"
# Nothing may have been written before that refusal.
if sgdisk -p "$WORK/bl-bad.img" 2>/dev/null | grep -qE '^ +3 +'"$BL3S"' +'$(( BL3S + 40000 )); then
    ok "the refused restore wrote nothing to the disk"
else
    bad "the refused restore had already changed the partition table"
fi

echo
echo "== a capture that cannot see the partition table =="
# sgdisk missing used to mean "no ESP found (legacy BIOS layout?)", a
# capture with no ESP in it, and a SHA256SUMS over the hole.
NOSG="$WORK/nosgdisk-recovery"
if PATH="$WORK/nosgdisk" "$MK" capture "$IMG" "$NOSG" >/dev/null 2>&1; then
    bad "capture SUCCEEDED with sgdisk unavailable"
else
    ok "capture refuses when sgdisk is unavailable"
fi
if [ -f "$NOSG/SHA256SUMS" ] && "$MK" verify "$NOSG" >/dev/null 2>&1; then
    bad "a partial capture was left behind AND it passes verify"
else
    ok "no passing SHA256SUMS was left behind"
fi

echo
echo "== a disk with no ESP =="
NOESP="$WORK/noesp.img"
truncate -s 64M "$NOESP"
sgdisk --zap-all "$NOESP" >/dev/null
sgdisk -n 1:2048:0 -t 1:0700 -c 1:"Basic data" "$NOESP" >/dev/null
fake_ntfs "$NOESP" "$(first_of 1 "$NOESP")" "$(last_of 1 "$NOESP")"
refuses "capture refuses a disk with no EFI System Partition" \
        "$MK" capture "$NOESP" "$WORK/noesp-recovery"

echo
echo "== a 4Kn disk, where assuming 512 is wrong by a factor of eight =="
# AURBRIDGE.md's warning, as a test: shrink takes sectors, partition
# structures take bytes, and a capture that guesses 512 on a 4Kn disk
# writes the backup GPT eight times too close to the front of the disk.
K4="$WORK/4kn.img"; K4DEST="$WORK/4kn-recovery"
truncate -s 256M "$K4"
K4LOOP=$(losetup --find --show -b 4096 "$K4")
sgdisk --zap-all "$K4LOOP" >/dev/null
sgdisk -n 1:256:+16M -t 1:ef00 -c 1:"EFI system partition" "$K4LOOP" >/dev/null
sgdisk -n 2:0:0      -t 2:0700 -c 2:"Basic data"           "$K4LOOP" >/dev/null
K4_1S=$(first_of 1 "$K4LOOP"); K4_1E=$(last_of 1 "$K4LOOP")
K4_2S=$(first_of 2 "$K4LOOP"); K4_2E=$(last_of 2 "$K4LOOP")
L1=$(losetup --find --show -b 4096 --offset $(( K4_1S * 4096 )) \
        --sizelimit $(( (K4_1E - K4_1S + 1) * 4096 )) "$K4")
mkfs.vfat -F32 "$L1" >/dev/null 2>&1
mmd -i "$L1" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot 2>/dev/null || true
mcopy -i "$L1" -o "$WORK/BCD" ::/EFI/Microsoft/Boot/BCD 2>/dev/null
losetup -d "$L1"
fake_ntfs "$K4" "$K4_2S" "$K4_2E" 4096
K4_GPT_SHA=$(dd if="$K4" bs=4096 count=2048 status=none | sha256sum | cut -d' ' -f1)
allows "capture completes on a 4Kn disk" "$MK" capture "$K4LOOP" "$K4DEST"
[ "$(cat "$K4DEST/sector-size" 2>/dev/null)" = "4096" ] \
    && ok "4096-byte sectors recorded, not assumed to be 512" \
    || bad "the capture recorded the wrong sector size"
[ "$(stat -c %s "$K4DEST/gpt-primary.bin" 2>/dev/null)" = "$(( 2048 * 4096 ))" ] \
    && ok "the head capture is 2048 sectors of 4096 bytes" \
    || bad "the head capture was sized in 512-byte sectors"
awk '$1=="2" && $5=="4096" {f=1} END{exit !f}' "$K4DEST/ntfs.tab" \
    && ok "the filesystem size is recorded in the disk's own sectors" \
    || bad "ntfs.tab did not record 4096-byte sectors"
dd if=/dev/zero of="$K4" bs=4096 count=2048 conv=notrunc status=none
dd if=/dev/zero of="$K4" bs=4096 seek=$(( 65536 - 34 )) count=34 conv=notrunc status=none
sync
"$MK" restore "$K4LOOP" "$K4DEST" >/dev/null 2>&1 || true
[ "$(dd if="$K4" bs=4096 count=2048 status=none | sha256sum | cut -d' ' -f1)" = "$K4_GPT_SHA" ] \
    && ok "4Kn GPT restored byte-identical" || bad "4Kn GPT differs after restore"
losetup -d "$K4LOOP"

# And the trap itself: a volume whose BPB disagrees with the disk.
K4B="$WORK/4kn-bad.img"; cp --sparse=always "$WORK/4kn.img" "$K4B"
fake_ntfs "$K4B" "$K4_2S" "$K4_2E" 4096 512
K4BLOOP=$(losetup --find --show -b 4096 "$K4B")
refuses "capture refuses a volume whose BPB disagrees with the disk's sectors" \
        "$MK" capture "$K4BLOOP" "$WORK/4kn-bad-recovery"
losetup -d "$K4BLOOP"

echo
echo "== restoring onto the wrong disk =="
refuses "restore refuses a disk that is not the one captured" \
        "$MK" restore "$WORK/other.img" "$DEST"

losetup -d "$LOOP"

echo
printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
