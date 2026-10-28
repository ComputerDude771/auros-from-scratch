#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  putbacktest — does "Put Windows back" in AurOS reach the restore?
#
#  The restore itself (aurstage.restore) is installtest's and
#  nosticktest's: install, put Windows back, every Windows file
#  byte-identical. What they boot it with is QEMU's -kernel. What a
#  person has is a Settings page in a running AurOS, and a line in
#  AurOS's own start-up menu. This boots the REAL image, with Secure
#  Boot on and Microsoft's keys, and follows the button all the way:
#
#    boot 1  AurOS, on a machine whose person said "it does not work"
#            (so nothing would bring it back to AurOS by itself). A
#            stand-in presses "Put Windows back" the way Settings does:
#            the bare word, as the desktop user, O_EXCL. answer.sh must
#            answer ok, set the menu's next_entry, arm BootNext at
#            AurOS's entry, and restart.
#    boot 2  the firmware starts AurOS's menu, the menu takes the
#            restore entry by itself, and grub starts the installer's
#            Canonical-signed staging kernel from the OTHER disk's EFI
#            partition with aurstage.restore -- under Secure Boot. The
#            restore reports. next_entry is gone afterwards.
#    boot 3  an ordinary start: AurOS, not the restore again.
#
#  The "Windows" disk here holds an EFI partition with the installer's
#  files in \EFI\AurOS and nothing to restore, so the restore's verdict
#  is restore-nothing-saved and the disk must come out unchanged. That
#  is the point where this test stops and installtest's begins.
#
#  WHAT IS NOT REAL: the stand-in for the button (as in firstboottest),
#  and one grubenv variable, aurstage_extra, which adds a serial console
#  to the restore so this can read it. Nothing a person does sets it.
#
#    sudo sh tools/putbacktest.sh [profile]     (default: desktop)
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

PROFILE="${1:-desktop}"
E=tools/efivarstore.py
CODE=/usr/share/OVMF/OVMF_CODE_4M.ms.fd
VARS=/usr/share/OVMF/OVMF_VARS_4M.ms.fd
BOOT_TIMEOUT="${PUTBACK_TIMEOUT:-1200}"

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-62s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-62s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

for t in qemu-system-x86_64 sgdisk python3 losetup mount mkfs.vfat mmd mcopy \
         grub-editenv; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ -f "$CODE" ] && [ -f "$VARS" ] || { echo "need OVMF with Microsoft's keys"; exit 2; }
[ -f "out/auros-$PROFILE.img" ] || { echo "no out/auros-$PROFILE.img"; exit 2; }
for f in out/auros-staging-vmlinuz out/auros-staging.img; do
    [ -f "$f" ] || { echo "no $f: run ./build/staging"; exit 2; }
done

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another end-to-end test is running"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/putback.XXXXXX")
QP=""; LP=""
cleanup() {
    [ -n "$QP" ] && kill -9 "$QP" 2>/dev/null
    mountpoint -q "$T/m" 2>/dev/null && umount "$T/m"
    [ -n "$LP" ] && losetup -d "$LP" 2>/dev/null
    rm -rf "$T" "$LOCK"
}
trap cleanup EXIT

echo
echo "Does \"Put Windows back\" in AurOS reach the restore, with Secure Boot on?"
echo

part_of() { sgdisk -p "$1" 2>/dev/null | awk -v n="$2" '$NF==n {print $1, $2, $3}'; }
mount_root() {
    set -- $(part_of "$DISK" AUROS-ROOT)
    [ -n "${2:-}" ] || { echo "  no AUROS-ROOT"; return 1; }
    LP=$(losetup --find --show -o $(( $2 * 512 )) \
                 --sizelimit $(( ($3 - $2 + 1) * 512 )) "$DISK") || return 1
    mkdir -p "$T/m"; mount "$LP" "$T/m" || { losetup -d "$LP"; LP=""; return 1; }
}
umount_root() { sync; umount "$T/m" 2>/dev/null; losetup -d "$LP" 2>/dev/null; LP=""; }

# ── the machine ─────────────────────────────────────────────────────
DISK="$T/auros.img"
cp --sparse=always "out/auros-$PROFILE.img" "$DISK"

# The "Windows" disk: GPT, one EFI partition, the installer's files in
# \EFI\AurOS exactly where AurBridge leaves them. Nothing to restore.
WIN="$T/windows.img"
truncate -s 256M "$WIN"
sgdisk -n1:2048:+200M -t1:EF00 -c1:"EFI system partition" "$WIN" >/dev/null
EO=$((2048 * 512))
dd if=/dev/zero of="$T/esp.fat" bs=1M count=200 2>/dev/null
mkfs.vfat -F 32 "$T/esp.fat" >/dev/null
mmd -i "$T/esp.fat" ::/EFI ::/EFI/AurOS ::/EFI/Microsoft ::/EFI/Microsoft/Boot
mcopy -i "$T/esp.fat" out/auros-staging-vmlinuz ::/EFI/AurOS/staging.efi
mcopy -i "$T/esp.fat" out/auros-staging.img    ::/EFI/AurOS/staging.img
dd if="$T/esp.fat" of="$WIN" bs=512 seek=2048 conv=notrunc 2>/dev/null
rm -f "$T/esp.fat"
WIN_BEFORE=$(md5sum "$WIN" | cut -d' ' -f1)

# Declined: the stamp aurfirst writes when she says "it does not work",
# so the hold re-arms nothing and only "Put Windows back" can bring the
# next start back to AurOS. The stand-in for the button, as firstboottest.
mount_root || exit 2
mkdir -p "$T/m/var/lib/auros" "$T/m/usr/lib/auros-e2e" "$T/m/etc/auros-e2e"
printf 'declined by putbacktest\n' > "$T/m/var/lib/auros/converted.declined"
cat > "$T/m/usr/lib/auros-e2e/run.sh" <<'EOS'
#!/bin/sh
# NOT PART OF AUROS. tools/putbacktest.sh puts this into a scratch copy
# of the image. It stands in for one press of "Put Windows back".
OUT=/var/lib/auros-e2e
mkdir -p "$OUT"
mode=$(cat /etc/auros-e2e/mode 2>/dev/null || echo none)
n=$(cat /etc/auros-e2e/boot 2>/dev/null || echo 0)
log() { printf '%s %s\n' "$n" "$*" >> "$OUT/log"; sync; }
log "up mode=$mode"
i=0
while [ ! -d /run/auros ] && [ "$i" -lt 300 ]; do sleep 1; i=$((i+1)); done
[ -d /run/auros ] && log "desktop after ${i}s" || log "desktop NEVER"
if [ "$mode" = putback ]; then
    u=$(stat -c %U /run/auros 2>/dev/null)
    if runuser -u "$u" -- sh -c "umask 077; set -C; printf %s putback > /run/auros/answer"
    then log "pressed putback as $u"; else log "could not press putback as $u"; fi
    i=0
    while [ "$i" -lt 180 ]; do
        grep -qx "request=putback" /run/auros-answer/result 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    cp /run/auros-answer/result "$OUT/result.$n" 2>/dev/null \
        && log "answered after ${i}s" || log "NO ANSWER after ${i}s"
    cp /var/log/auros-answer.log "$OUT/answer.log.$n" 2>/dev/null
    sync
    # answer.sh restarts the machine itself. If it has not within a
    # minute, it is not going to, and the test is told so by the log.
    sleep 60
    log "NO RESTART"
fi
sync
systemctl poweroff
EOS
chmod 755 "$T/m/usr/lib/auros-e2e/run.sh"
cat > "$T/m/etc/systemd/system/auros-e2e.service" <<'EOU'
[Unit]
Description=NOT PART OF AUROS: putbacktest's stand-in for one button press
After=auros-hold.service aurshell.service
[Service]
Type=oneshot
ExecStart=/usr/lib/auros-e2e/run.sh
TimeoutStartSec=900
[Install]
WantedBy=multi-user.target
EOU
ln -sf /etc/systemd/system/auros-e2e.service \
    "$T/m/etc/systemd/system/multi-user.target.wants/auros-e2e.service"
[ -f "$T/m/boot/grub/grubenv" ] && ok "the image carries grub's environment block" \
    || bad "the image carries grub's environment block"
grep -q "put-windows-back-yes" "$T/m/boot/grub/grub.cfg" \
    && ok "...and a Put Windows back entry in its menu" \
    || bad "...and a Put Windows back entry in its menu"
umount_root

# Firmware as an install leaves it, with Secure Boot on: Windows first
# in BootOrder, AurOS's entry, and the one restart armed so boot 1 is
# AurOS. After that nothing re-arms: she declined.
set -- $(part_of "$DISK" AUROS-ESP)
NV="$T/vars.fd"; cp "$VARS" "$NV"
python3 "$E" "$NV" plant Boot0000 "Windows Boot Manager" '\EFI\Microsoft\Boot\bootmgfw.efi' &&
python3 "$E" "$NV" plant-hd Boot0002 "AurOS" '\EFI\AurOS\shimx64.efi' "$DISK" "$1" &&
python3 "$E" "$NV" set-order 0000 &&
python3 "$E" "$NV" set-next 0002 || { echo "  could not make the firmware"; exit 2; }

BOOTN=0
power_on() { # mode
    BOOTN=$((BOOTN + 1))
    mount_root || return 1
    printf '%s\n' "$1" > "$T/m/etc/auros-e2e/mode"
    printf '%s\n' "$BOOTN" > "$T/m/etc/auros-e2e/boot"
    umount_root
    SER="$T/serial.$BOOTN"; : > "$SER"
    qemu-system-x86_64 -machine q35,smm=on,accel=tcg -m 2048 -smp 2 -no-reboot \
        -global driver=cfi.pflash01,property=secure,value=on \
        -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
        -drive if=pflash,format=raw,unit=1,file="$NV" \
        -drive file="$DISK",format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0 \
        -drive file="$WIN",format=raw,if=none,id=d1 -device virtio-blk-pci,drive=d1 \
        -device virtio-vga -display none -serial file:"$SER" -net none \
        >/dev/null 2>&1 &
    QP=$!
    i=0
    while kill -0 "$QP" 2>/dev/null && [ "$i" -lt "$BOOT_TIMEOUT" ]; do
        # The restore stops and waits for the power button; its report
        # line is the end of what there is to read.
        grep -aq 'aurstage-report v1 verdict=restore' "$SER" && { sleep 3; break; }
        sleep 2; i=$((i + 2))
    done
    kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=""
}
e2e_log() { mount_root && cat "$T/m/var/lib/auros-e2e/log" 2>/dev/null; umount_root; }
grubenv_list() { mount_root && grub-editenv "$T/m/boot/grub/grubenv" list 2>/dev/null; umount_root; }

# ── boot 1: AurOS, and the button ───────────────────────────────────
echo "  boot 1: AurOS, where she presses Put Windows back"
power_on putback
L=$(e2e_log)
case "$L" in *"1 pressed putback"*) ok "the desktop came up and the button was pressed" ;;
    *) bad "the desktop came up and the button was pressed" "$(echo "$L" | tail -4)" ;; esac
mount_root; R=$(cat "$T/m/var/lib/auros-e2e/result.1" 2>/dev/null)
AL=$(tail -5 "$T/m/var/lib/auros-e2e/answer.log.1" 2>/dev/null); umount_root
case "$R" in *"request=putback"*"result=ok"*) ok "answer.sh answered ok" ;;
    *) bad "answer.sh answered ok" "$(echo "$R" | tr '\n' ' ')" "$AL" ;; esac
case "$L" in *"NO RESTART"*) bad "...and restarted the computer itself" ;;
    *) ok "...and restarted the computer itself" ;; esac
[ "$(grubenv_list)" = "next_entry=put-windows-back>put-windows-back-yes" ] \
  && ok "the menu will take the restore entry next" \
  || bad "the menu will take the restore entry next" "$(grubenv_list)"
[ "$(python3 "$E" "$NV" bootnext)" = "0002" ] \
  && ok "and the firmware will start AurOS's menu, though she declined" \
  || bad "and the firmware will start AurOS's menu, though she declined" \
         "BootNext=$(python3 "$E" "$NV" bootnext)"
[ "$(python3 "$E" "$NV" bootorder | awk '{print $1}')" = "0000" ] \
  && ok "...with BootOrder untouched: Windows is still the default" \
  || bad "...with BootOrder untouched: Windows is still the default" \
         "BootOrder=$(python3 "$E" "$NV" bootorder)"

# ── boot 2: the menu takes the restore by itself ────────────────────
echo
echo "  boot 2: the next start, with nobody touching anything"
mount_root && grub-editenv "$T/m/boot/grub/grubenv" set \
    "aurstage_extra=console=ttyS0,115200n8"; umount_root
power_on none
S2=$(tr -d '\r' < "$T/serial.2")
echo "$S2" | grep -aq 'starting Boot0002 "AurOS"' \
  && ok "the firmware started AurOS's entry" \
  || bad "the firmware started AurOS's entry" "$(echo "$S2" | grep -ao 'BdsDxe:.*' | head -3)"
echo "$S2" | grep -aq 'Command line:.*aurstage.restore' \
  && ok "grub started the staging kernel with aurstage.restore" \
  || bad "grub started the staging kernel with aurstage.restore" \
         "$(echo "$S2" | grep -a 'Command line' | head -2)"
echo "$S2" | grep -aqi 'secure boot.*enabled\|Secure boot enabled\|secure   Secure Boot on' \
  && ok "...with Secure Boot on" \
  || bad "...with Secure Boot on" "$(echo "$S2" | grep -ai 'secure' | head -3)"
V=$(echo "$S2" | grep -ao 'aurstage-report v1 verdict=restore[a-z-]*' | head -1)
[ "$V" = "aurstage-report v1 verdict=restore-nothing-saved" ] \
  && ok "the restore ran, and found nothing on this test disk to put back" \
  || bad "the restore ran, and found nothing on this test disk to put back" "${V:-no report}" \
         "$(echo "$S2" | grep -a 'aurstage:' | tail -5)"
[ "$(md5sum "$WIN" | cut -d' ' -f1)" = "$WIN_BEFORE" ] \
  && ok "...and changed nothing on that disk" || bad "...and changed nothing on that disk"
GL=$(grubenv_list)
case "$GL" in *next_entry=put*) bad "the menu's one-time choice was used up" "$GL" ;;
    *) ok "the menu's one-time choice was used up" ;; esac

# ── boot 3: an ordinary start ───────────────────────────────────────
echo
echo "  boot 3: the start after that"
mount_root && grub-editenv "$T/m/boot/grub/grubenv" unset aurstage_extra; umount_root
python3 "$E" "$NV" set-next 0002 >/dev/null
power_on none
S3=$(tr -d '\r' < "$T/serial.3")
echo "$S3" | grep -aq 'aurstage.restore' \
  && bad "it does not restore again" "$(echo "$S3" | grep -a 'Command line' | head -2)" \
  || ok "it does not restore again"
L=$(e2e_log)
case "$L" in *"3 up mode=none"*) ok "...it starts AurOS" ;;
    *) bad "...it starts AurOS" "$(echo "$L" | tail -3)" ;; esac

echo
if [ "$fail" -gt 0 ]; then echo "$fail of $checked wrong."; exit 1; fi
echo "$checked checks: the button in AurOS restarts into the restore, once,"
echo "through AurOS's own menu, with Secure Boot on."
exit 0
