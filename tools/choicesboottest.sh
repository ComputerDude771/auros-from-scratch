#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  choicesboottest — the real image, started for the first time, with
#  what somebody chose in the installer waiting on the EFI partition
#
#  tools/choicestest.sh proves choices.sh against a directory. This
#  proves the thing that ships: out/auros-desktop.img, booted by
#  firmware in QEMU the way a PC starts it after the install, with a
#  second disk standing in for the machine's own EFI partition -- the
#  place AurBridge writes \EFI\AurOS\choices.conf. After first boot has
#  run, the image's root is read (read-only) and asked whether it is
#  Spanish, has a Spanish keyboard, keeps Paris time, looks like Moss
#  and works like the taskbar desktop -- and whether the Spanish locale
#  was actually generated, which no image could do before `locales`.
#
#    sudo sh tools/choicesboottest.sh      (needs out/auros-desktop.img)
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-60s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-60s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

IMG=out/auros-desktop.img
[ -f "$IMG" ] || { echo "no $IMG; build it first"; exit 2; }
for t in qemu-system-x86_64 sgdisk mkfs.vfat mcopy mmd losetup; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
TMP=$(mktemp -d "${TMPDIR:-/tmp}/choicesboot.XXXXXX")
[ -n "$TMP" ] && [ -d "$TMP" ] || { echo "no scratch dir"; exit 2; }
trap 'mountpoint -q "$TMP/m" && umount "$TMP/m"; [ -n "${L:-}" ] && losetup -d "$L" 2>/dev/null; rm -rf "$TMP"' EXIT

echo
echo "Does the real image start up the way the installer was told to?"
echo

cp --sparse=always "$IMG" "$TMP/disk.img"

# The machine's EFI partition, as AurBridge leaves it.
E="$TMP/esp.img"
truncate -s 64M "$E"
sgdisk --zap-all "$E" >/dev/null 2>&1
sgdisk -n 1:2048:0 -t 1:ef00 -c 1:"EFI" "$E" >/dev/null 2>&1
truncate -s 62M "$TMP/fat.img"
mkfs.vfat -F32 -n EFI "$TMP/fat.img" >/dev/null 2>&1
export MTOOLS_SKIP_CHECK=1
mmd -i "$TMP/fat.img" ::/EFI ::/EFI/AurOS
cat > "$TMP/choices.conf" <<'EOF'
# What was chosen in the AurOS installer. Applied once, at
# AurOS's first start (/usr/lib/auros/choices.sh).
language=es_ES.UTF-8
keyboard=klid:0000040A
timezone=windows:Romance Standard Time
theme=moss
shell=taskbar
EOF
mcopy -i "$TMP/fat.img" "$TMP/choices.conf" ::/EFI/AurOS/choices.conf
dd if="$TMP/fat.img" of="$E" bs=1M seek=1 conv=notrunc status=none

echo "  booting the image from firmware (this is slow under emulation)"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$TMP/vars.fd"
: > "$TMP/out.txt"
qemu-system-x86_64 -machine q35,accel=tcg -m 3072 -smp 2 \
    -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,unit=1,file="$TMP/vars.fd" \
    -drive file="$TMP/disk.img",format=raw,if=none,id=d0 \
    -device virtio-blk-pci,drive=d0,bootindex=0 \
    -drive file="$E",format=raw,if=none,id=d1 \
    -device virtio-blk-pci,drive=d1 \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -display none -serial stdio > "$TMP/out.txt" 2>&1 &
qp=$!
# First boot is done when its stamp is on the disk. Asked of the file
# through a read-only loop device while the machine runs: a stale read
# only makes this wait longer.
R=$(python3 - "$TMP/disk.img" <<'EOPY'
import sys, struct
f = open(sys.argv[1], 'rb'); f.seek(512); h = f.read(512)
pl = struct.unpack_from('<Q', h, 72)[0]; n = struct.unpack_from('<I', h, 80)[0]
e = struct.unpack_from('<I', h, 84)[0]; f.seek(pl * 512); arr = f.read(n * e)
for i in range(n):
    ent = arr[i*e:(i+1)*e]
    if ent[56:128].decode('utf-16-le', 'replace').rstrip('\0') == 'AUROS-ROOT':
        first, last = struct.unpack_from('<QQ', ent, 32)
        print(first, last); break
EOPY
)
RS=${R% *}; RE=${R#* }
[ -n "$RS" ] || { echo "  the image has no AUROS-ROOT partition"; kill -9 $qp; exit 2; }
done_at=""
i=0
while [ $i -lt 2400 ]; do
    kill -0 $qp 2>/dev/null || break
    if [ $((i % 30)) -eq 0 ] && [ $i -gt 0 ]; then
        if command -v debugfs >/dev/null 2>&1; then
            L=$(losetup -r --find --show -o $((RS*512)) --sizelimit $(((RE-RS+1)*512)) "$TMP/disk.img" 2>/dev/null)
            if [ -n "$L" ]; then
                debugfs -R 'stat /var/lib/auros/firstboot.done' "$L" 2>/dev/null | grep -q Inode \
                    && done_at=$i
                losetup -d "$L"; L=""
            fi
        fi
        [ -n "$done_at" ] && break
    fi
    sleep 1; i=$((i+1))
done
# A little longer, so what first boot wrote reaches the disk image.
sleep 20
kill -9 $qp 2>/dev/null; wait $qp 2>/dev/null
[ -n "$done_at" ] && ok "first boot finished (after about ${done_at}s)" \
                  || bad "first boot finished" "$(tail -5 "$TMP/out.txt")"

L=$(losetup -r --find --show -o $((RS*512)) --sizelimit $(((RE-RS+1)*512)) "$TMP/disk.img")
mkdir -p "$TMP/m"
if ! mount -o ro,noload "$L" "$TMP/m" 2>/dev/null; then
    bad "the image's root can be read afterwards"
    exit 1
fi
M="$TMP/m"
echo
echo "  what the installed system now says"
grep -qx 'LANG=es_ES.UTF-8' "$M/etc/default/locale" \
    && ok "its language is Spanish" \
    || bad "its language is Spanish" "$(cat "$M/etc/default/locale" 2>/dev/null)"
if ls "$M/usr/lib/locale/" 2>/dev/null | grep -qi 'es_ES' ||
   { [ -f "$M/usr/lib/locale/locale-archive" ] &&
     strings "$M/usr/lib/locale/locale-archive" | grep -q 'es_ES'; }; then
    ok "...and the Spanish locale was generated, not just named"
else
    bad "...and the Spanish locale was generated, not just named" \
        "$(ls "$M/usr/lib/locale/" 2>/dev/null | tr '\n' ' ')"
fi
grep -qx 'XKBLAYOUT="es"' "$M/etc/default/keyboard" \
    && ok "its keyboard is Spanish (from Windows' KLID 0000040A)" \
    || bad "its keyboard is Spanish" "$(grep XKB "$M/etc/default/keyboard" 2>/dev/null)"
[ "$(readlink "$M/etc/localtime")" = /usr/share/zoneinfo/Europe/Paris ] \
    && ok "its clock keeps Paris time (from 'Romance Standard Time')" \
    || bad "its clock keeps Paris time" "$(readlink "$M/etc/localtime")"
grep -qx moss "$M/etc/auros/theme" \
    && ok "it looks like Moss" || bad "it looks like Moss" "$(cat "$M/etc/auros/theme")"
[ "$(readlink "$M/etc/auros/shell/active.shell")" = /usr/share/auros/shells/taskbar.shell ] \
    && ok "it works like the taskbar desktop" \
    || bad "it works like the taskbar desktop" "$(readlink "$M/etc/auros/shell/active.shell")"
[ "$(grep -c '=' "$M/etc/auros/installer-choices.conf" 2>/dev/null)" = 5 ] \
    && ok "and all five are recorded, so Ferry leaves them alone" \
    || bad "and all five are recorded" "$(cat "$M/etc/auros/installer-choices.conf" 2>/dev/null)"
grep -q 'choices: desktop: taskbar' "$M/var/log/auros-firstboot.log" 2>/dev/null \
    && ok "first boot's own log says so" \
    || bad "first boot's own log says so" "$(grep choices "$M/var/log/auros-firstboot.log" 2>/dev/null | tail -3)"

echo
if [ "$fail" -gt 0 ]; then echo "$fail of $checked wrong."; exit 1; fi
echo "$checked checks: the real image starts up in the language, keyboard, time"
echo "zone, look and desktop the installer was told."
exit 0
