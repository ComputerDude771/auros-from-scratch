#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  bootupdatetest — after a kernel, grub and shim update, does AurOS
#  still start, with Secure Boot on?
#
#  bootchaintest proves auros-bootchain's gates on fixtures. This boots
#  the REAL image under OVMF with Microsoft's keys and Secure Boot
#  enforcing, and lets Ubuntu's own packaging drive it, the way
#  unattended-upgrades will on somebody's PC:
#
#    before  the image's EFI partition is made one shim behind: the
#            single-signed shimx64.efi.signed.latest where the build put
#            the dual-signed one. (Same code, different signatures, so
#            the files differ and there is something to update. No newer
#            shim/grub pair is installable on 24.04 today: shim-signed
#            1.59 needs a grub from a later series.)
#    boot 1  AurOS. A stand-in for unattended-upgrades runs, and each
#            of the two ways a new shim reaches the EFI partition is
#            made to work ALONE:
#              dpkg -i shim-signed grub-efi-amd64-signed     (the real
#                 .debs and their real maintainer scripts, which call
#                 grub-multi-install -> grub-install; no apt, so no
#                 apt hook)
#              the partition put one shim behind again, the postinsts'
#                 trigger file (core.efi) moved away, and
#                 apt-get install --reinstall shim-signed: now only
#                 DPkg::Post-Invoke (52auros-bootchain) can sync it
#              a new kernel, and /etc/kernel/postinst.d/zz-update-grub
#                 with DEB_MAINT_PARAMS=configure, exactly as the
#                 linux-image postinst runs it
#            Afterwards, from the outside: the EFI partition has the
#            dual-signed shim, nothing was written to \EFI\ubuntu, no
#            "ubuntu" firmware entry exists, BootOrder still starts with
#            AurOS then Windows (OVMF appends its own entries on every
#            boot; that is the firmware, docs/handoff/TRAPS.md), the menu
#            starts the new kernel and falls back to the old one, and Put
#            Windows back is still in it.
#    boot 2  the firmware starts AurOS's entry, through the shim the
#            update installed, and grub starts the NEW kernel. The
#            desktop comes up.
#    boot 3  the new kernel is damaged on disk (its signature no longer
#            verifies). grub must fall back to the previous kernel by
#            itself, and the desktop must still come up.
#
#  The "new kernel" is the image's own signed vmlinuz under a higher
#  version number: what matters to the boot chain is a second signed
#  kernel in /boot and the hook that rewrites the menu, and that is
#  real. A kernel from the archive would add a 100 MB download and test
#  nothing more about the menu.
#
#    sudo sh tools/bootupdatetest.sh [profile]     (default: desktop)
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

PROFILE="${1:-desktop}"
E=tools/efivarstore.py
CODE=/usr/share/OVMF/OVMF_CODE_4M.ms.fd
VARS=/usr/share/OVMF/OVMF_VARS_4M.ms.fd
BOOT_TIMEOUT="${BOOTUPDATE_TIMEOUT:-1200}"
NEWK=6.8.0-999-generic

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-64s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-64s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

for t in qemu-system-x86_64 sgdisk python3 losetup mount mcopy mdir apt-get dpkg-deb; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ -f "$CODE" ] && [ -f "$VARS" ] || { echo "need OVMF with Microsoft's keys"; exit 2; }
[ -f "out/auros-$PROFILE.img" ] || { echo "no out/auros-$PROFILE.img"; exit 2; }

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another end-to-end test is running"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/bootupdate.XXXXXX")
QP=""; LP=""
cleanup() {
    [ -n "$QP" ] && kill -9 "$QP" 2>/dev/null
    mountpoint -q "$T/m" 2>/dev/null && umount "$T/m"
    [ -n "$LP" ] && losetup -d "$LP" 2>/dev/null
    rm -rf "$T" "$LOCK"
}
trap cleanup EXIT

echo
echo "After a kernel, grub and shim update, does AurOS still start with Secure Boot on?"
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

DISK="$T/auros.img"
cp --sparse=always "out/auros-$PROFILE.img" "$DISK"
set -- $(part_of "$DISK" AUROS-ESP)
ESPN=$1; ESPOFF=$(( $2 * 512 ))
export MTOOLS_SKIP_CHECK=1
espget() { mcopy -n -i "$DISK@@$ESPOFF" "::$1" "$2" 2>/dev/null; }
espput() { mcopy -o -i "$DISK@@$ESPOFF" "$1" "::$2"; }

# ── the packages, at exactly the versions in the image ──────────────
mount_root || exit 2
R="$T/m"
SHIMV=$(chroot "$R" dpkg-query -W -f '${Version}' shim-signed 2>/dev/null)
GRUBV=$(chroot "$R" dpkg-query -W -f '${Version}' grub-efi-amd64-signed 2>/dev/null)
# grub2-common owns /usr/sbin/update-grub and grub-install: reinstalling
# it is what would put Ubuntu's back over the diverted ones.
COMMONV=$(chroot "$R" dpkg-query -W -f '${Version}' grub2-common 2>/dev/null)
mkdir -p "$T/debs"
( cd "$T/debs" && apt-get download -q "shim-signed=$SHIMV" "grub-efi-amd64-signed=$GRUBV" \
      "grub2-common=$COMMONV" ) \
    >/dev/null 2>&1
ls "$T/debs"/shim-signed_*.deb "$T/debs"/grub-efi-amd64-signed_*.deb "$T/debs"/grub2-common_*.deb >/dev/null 2>&1 \
  && ok "the image's own shim-signed, grub-efi-amd64-signed and grub2-common" \
  || { bad "the image's own shim-signed, grub-efi-amd64-signed and grub2-common" "apt-get download failed"; exit 1; }
mkdir -p "$R/var/cache/auros-e2e" "$R/usr/lib/auros-e2e" "$R/etc/auros-e2e" "$R/var/lib/auros"
cp "$T/debs"/*.deb "$R/var/cache/auros-e2e/"
DUAL="$T/dual.efi"; cp "$R/usr/lib/shim/shimx64.efi.dualsigned" "$DUAL"
cp "$R/usr/lib/shim/shimx64.efi.signed.latest" "$T/older.efi"
printf 'confirmed by bootupdatetest\n' > "$R/var/lib/auros/converted.confirmed"

cat > "$R/usr/lib/auros-e2e/run.sh" <<EOS
#!/bin/sh
# NOT PART OF AUROS. tools/bootupdatetest.sh puts this into a scratch
# copy of the image. It stands in for one night of unattended-upgrades.
OUT=/var/lib/auros-e2e
mkdir -p "\$OUT"
mode=\$(cat /etc/auros-e2e/mode 2>/dev/null || echo none)
n=\$(cat /etc/auros-e2e/boot 2>/dev/null || echo 0)
log() { printf '%s %s\n' "\$n" "\$*" >> "\$OUT/log"; sync; }
sb=\$(od -An -t u1 /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c 2>/dev/null | awk '{print \$NF}')
log "up mode=\$mode sb=\$sb cmdline=\$(cat /proc/cmdline)"
i=0
while [ ! -d /run/auros ] && [ "\$i" -lt 300 ]; do sleep 1; i=\$((i+1)); done
[ -d /run/auros ] && log "desktop after \${i}s" || log "desktop NEVER"
if [ "\$mode" = update ]; then
    DEBIAN_FRONTEND=noninteractive dpkg -i /var/cache/auros-e2e/*.deb \
        > "\$OUT/dpkg.log" 2>&1
    log "dpkg exit \$?"
    sha256sum /boot/efi/EFI/AurOS/shimx64.efi | cut -c1-64 > "\$OUT/shim.after-dpkg"
    cp /var/lib/auros/bootchain.state "\$OUT/state.after-dpkg" 2>/dev/null
    # one shim behind again, and the postinsts' own trigger out of the way
    cp /usr/lib/shim/shimx64.efi.signed.latest /boot/efi/EFI/AurOS/shimx64.efi
    cp /usr/lib/shim/shimx64.efi.signed.latest /boot/efi/EFI/BOOT/BOOTX64.EFI
    sha256sum /boot/efi/EFI/AurOS/shimx64.efi | cut -c1-64 > "\$OUT/shim.reaged"
    mv /boot/grub/x86_64-efi/core.efi /boot/grub/x86_64-efi/core.efi.away
    DEBIAN_FRONTEND=noninteractive apt-get install -y --reinstall \
        /var/cache/auros-e2e/shim-signed_*.deb > "\$OUT/apt.log" 2>&1
    log "apt exit \$?"
    mv /boot/grub/x86_64-efi/core.efi.away /boot/grub/x86_64-efi/core.efi
    sha256sum /boot/efi/EFI/AurOS/shimx64.efi | cut -c1-64 > "\$OUT/shim.after-apt"
    cp /boot/vmlinuz-\$(uname -r) /boot/vmlinuz-$NEWK
    cp /boot/initrd.img-\$(uname -r) /boot/initrd.img-$NEWK
    DEB_MAINT_PARAMS="'configure'" /etc/kernel/postinst.d/zz-update-grub $NEWK \
        > "\$OUT/kernel-hook.log" 2>&1
    log "kernel hook exit \$?"
    /usr/sbin/auros-bootchain status > "\$OUT/status" 2>&1
    log "status exit \$?"
    dpkg-divert --list > "\$OUT/diversions" 2>&1
    grep -l /usr/lib/auros/bootchain /usr/sbin/update-grub /usr/sbin/grub-install \
        > "\$OUT/ours" 2>&1
fi
sync
systemctl poweroff
EOS
chmod 755 "$R/usr/lib/auros-e2e/run.sh"
cat > "$R/etc/systemd/system/auros-e2e.service" <<'EOU'
[Unit]
Description=NOT PART OF AUROS: bootupdatetest's stand-in for unattended-upgrades
After=auros-hold.service aurshell.service
[Service]
Type=oneshot
ExecStart=/usr/lib/auros-e2e/run.sh
TimeoutStartSec=1200
[Install]
WantedBy=multi-user.target
EOU
ln -sf /etc/systemd/system/auros-e2e.service \
    "$R/etc/systemd/system/multi-user.target.wants/auros-e2e.service"
umount_root

# One shim behind: what an AurOS installed before an update has.
espput "$T/older.efi" /EFI/AurOS/shimx64.efi && espput "$T/older.efi" /EFI/BOOT/BOOTX64.EFI \
  && espget /EFI/AurOS/shimx64.efi "$T/check.efi" && cmp -s "$T/check.efi" "$T/older.efi" \
  && ! cmp -s "$T/older.efi" "$DUAL" \
  && ok "the EFI partition starts one shim behind" \
  || bad "the EFI partition starts one shim behind"
ESP_UBUNTU_BEFORE=$(mdir -b -i "$DISK@@$ESPOFF" ::/EFI/ubuntu 2>/dev/null | sort)

# The "Windows" disk: an EFI partition holding the Put Windows back
# kernel where the installer leaves it. A new shim is installed only if
# it would start that kernel, so AurOS has to find it -- by mounting
# this partition read-only -- and judge it. Its bytes must not change.
WIN="$T/windows.img"
truncate -s 256M "$WIN"
sgdisk -n1:2048:+200M -t1:EF00 -c1:"EFI system partition" "$WIN" >/dev/null
dd if=/dev/zero of="$T/wesp.fat" bs=1M count=200 2>/dev/null
mkfs.vfat -F 32 "$T/wesp.fat" >/dev/null
mmd -i "$T/wesp.fat" ::/EFI ::/EFI/AurOS ::/EFI/Microsoft ::/EFI/Microsoft/Boot
if [ -f out/auros-staging-vmlinuz ]; then
    mcopy -i "$T/wesp.fat" out/auros-staging-vmlinuz ::/EFI/AurOS/staging.efi
else
    mount_root && cp "$(ls "$T"/m/boot/vmlinuz-*-generic | head -1)" "$T/stg.efi"; umount_root
    mcopy -i "$T/wesp.fat" "$T/stg.efi" ::/EFI/AurOS/staging.efi
fi
dd if="$T/wesp.fat" of="$WIN" bs=512 seek=2048 conv=notrunc 2>/dev/null
rm -f "$T/wesp.fat"
WIN_BEFORE=$(md5sum "$WIN" | cut -d' ' -f1)

# Firmware as a confirmed install leaves it: AurOS first, Windows' entry
# still in the list.
NV="$T/vars.fd"; cp "$VARS" "$NV"
python3 "$E" "$NV" plant Boot0000 "Windows Boot Manager" '\EFI\Microsoft\Boot\bootmgfw.efi' &&
python3 "$E" "$NV" plant-hd Boot0002 "AurOS" '\EFI\AurOS\shimx64.efi' "$DISK" "$ESPN" &&
python3 "$E" "$NV" set-order 0002 0000 || { echo "  could not make the firmware"; exit 2; }
ORDER_BEFORE=$(python3 "$E" "$NV" bootorder)

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
        -monitor unix:"$T/mon.sock",server,nowait \
        >/dev/null 2>&1 &
    QP=$!
    i=0
    while kill -0 "$QP" 2>/dev/null && [ "$i" -lt "$BOOT_TIMEOUT" ]; do
        sleep 2; i=$((i + 2))
    done
    # Still running at the timeout: what is on the screen is the only
    # account of why, when the menu or an error is not on the serial
    # line. Kept outside $T, which the EXIT trap removes.
    if kill -0 "$QP" 2>/dev/null; then
        SHOT="${TMPDIR:-/tmp}/bootupdatetest.boot$BOOTN.ppm"
        python3 - "$T/mon.sock" "$SHOT" <<'EOP' 2>/dev/null && echo "  boot $BOOTN timed out; its screen: $SHOT"
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.settimeout(3); s.connect(sys.argv[1]); time.sleep(0.3)
try: s.recv(4096)
except Exception: pass
s.send(("screendump %s\n" % sys.argv[2]).encode()); time.sleep(2); s.close()
EOP
    fi
    kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=""
}
e2e_log() { mount_root && cat "$T/m/var/lib/auros-e2e/log" 2>/dev/null; umount_root; }

# ── boot 1: the update ──────────────────────────────────────────────
echo
echo "  boot 1: AurOS, and a night of updates"
power_on update
L=$(e2e_log)
case "$L" in *"1 desktop after"*) ok "AurOS started on the old shim, desktop up" ;;
    *) bad "AurOS started on the old shim, desktop up" "$(echo "$L" | tail -3)" ;; esac
mount_root
DL=$(tail -15 "$T/m/var/lib/auros-e2e/dpkg.log" 2>/dev/null)
ST=$(cat "$T/m/var/lib/auros-e2e/status" 2>/dev/null)
DV=$(cat "$T/m/var/lib/auros-e2e/diversions" 2>/dev/null)
CFG=$(cat "$T/m/boot/grub/grub.cfg" 2>/dev/null)
umount_root
case "$L" in *"1 dpkg exit 0"*) ok "dpkg -i shim-signed grub-efi-amd64-signed: exit 0" ;;
    *) bad "dpkg -i shim-signed grub-efi-amd64-signed: exit 0" "$DL" ;; esac
case "$DL" in *"Installing grub to"*) bad "grub-multi-install did not enumerate EFI partitions" "$DL" ;;
    *) ok "grub-multi-install did not enumerate EFI partitions" ;; esac
mount_root; OURS=$(cat "$T/m/var/lib/auros-e2e/ours" 2>/dev/null); umount_root
case "$OURS" in *update-grub*grub-install*)
    ok "after reinstalling grub2-common, update-grub and grub-install are still ours" ;;
    *) bad "after reinstalling grub2-common, update-grub and grub-install are still ours" "$OURS" "$DV" ;; esac
DUALSHA=$(sha256sum "$DUAL" | cut -c1-64); OLDSHA=$(sha256sum "$T/older.efi" | cut -c1-64)
mount_root
A1=$(cat "$T/m/var/lib/auros-e2e/shim.after-dpkg" 2>/dev/null)
A0=$(cat "$T/m/var/lib/auros-e2e/shim.reaged" 2>/dev/null)
A2=$(cat "$T/m/var/lib/auros-e2e/shim.after-apt" 2>/dev/null)
SD=$(cat "$T/m/var/lib/auros-e2e/state.after-dpkg" 2>/dev/null)
AL=$(tail -8 "$T/m/var/lib/auros-e2e/apt.log" 2>/dev/null)
umount_root
[ "$A1" = "$DUALSHA" ] \
  && ok "the packages' own scripts installed the new shim (dpkg, no apt hook)" \
  || bad "the packages' own scripts installed the new shim (dpkg, no apt hook)" "$SD" "$DL"
case "$SD" in *"judged="*"the Put Windows back kernel on /dev/"*) 
    ok "...after finding and judging the Put Windows back kernel on Windows' disk" ;;
    *) bad "...after finding and judging the Put Windows back kernel on Windows' disk" "$SD" ;; esac
case "$L" in *"1 apt exit 0"*) ok "apt-get install --reinstall shim-signed: exit 0" ;;
    *) bad "apt-get install --reinstall shim-signed: exit 0" "$AL" ;; esac
[ "$A0" = "$OLDSHA" ] && [ "$A2" = "$DUALSHA" ] \
  && ok "...and with the scripts' trigger gone, the apt hook alone did it" \
  || bad "...and with the scripts' trigger gone, the apt hook alone did it" "reaged=$A0" "after=$A2" "$AL"
case "$ST" in *"result=updated"*|*"is current"*) ok "auros-bootchain status: current" ;;
    *) bad "auros-bootchain status: current" "$ST" ;; esac
espget /EFI/AurOS/shimx64.efi "$T/after.efi"; espget /EFI/BOOT/BOOTX64.EFI "$T/afterb.efi"
cmp -s "$T/after.efi" "$DUAL" && cmp -s "$T/afterb.efi" "$DUAL" \
  && ok "the EFI partition has the dual-signed shim now, both places" \
  || bad "the EFI partition has the dual-signed shim now, both places"
[ "$(mdir -b -i "$DISK@@$ESPOFF" ::/EFI/ubuntu 2>/dev/null | sort)" = "$ESP_UBUNTU_BEFORE" ] \
  && ok "nothing was written to \\EFI\\ubuntu (Ubuntu's grub-install did not run)" \
  || bad "nothing was written to \\EFI\\ubuntu (Ubuntu's grub-install did not run)" \
         "$(mdir -b -i "$DISK@@$ESPOFF" ::/EFI/ubuntu 2>/dev/null)"
case "$(python3 "$E" "$NV" bootorder)" in
    "$ORDER_BEFORE"|"$ORDER_BEFORE "*) ok "BootOrder still starts $ORDER_BEFORE: AurOS, then Windows" ;;
    *) bad "BootOrder still starts $ORDER_BEFORE: AurOS, then Windows" "now: $(python3 "$E" "$NV" bootorder)" ;; esac
if python3 "$E" "$NV" entry ubuntu >/dev/null 2>&1 || python3 "$E" "$NV" entry Ubuntu >/dev/null 2>&1; then
    bad "no \"ubuntu\" firmware entry was made"
else ok "no \"ubuntu\" firmware entry was made"; fi
case "$L" in *"1 kernel hook exit 0"*) ok "the kernel's zz-update-grub hook: exit 0" ;;
    *) bad "the kernel's zz-update-grub hook: exit 0" "$L" ;; esac
echo "$CFG" | grep -A1 -- "--id auros {" | grep -q "vmlinuz-$NEWK " \
  && ok "the menu starts the new kernel" \
  || bad "the menu starts the new kernel" "$(echo "$CFG" | grep vmlinuz | head -3)"
# grub's fallback is an entry NUMBER; counted here the way grub does.
FB_WANT=$(echo "$CFG" | awk '/^(menuentry|submenu) /{ if ($0 ~ /--id auros-previous/) {print n; exit} n++ }')
FB_GOT=$(echo "$CFG" | sed -n 's/^set fallback=\([0-9]*\)$/\1/p')
[ -n "$FB_WANT" ] && [ "$FB_GOT" = "$FB_WANT" ] \
  && ok "...and falls back, by entry number ($FB_GOT), to the one before it" \
  || bad "...and falls back, by entry number, to the one before it" "fallback=$FB_GOT, entry $FB_WANT"
echo "$CFG" | grep -q -- "--id put-windows-back-yes" \
  && ok "...and still has Put Windows back" || bad "...and still has Put Windows back"

# ── boot 2: the new chain ───────────────────────────────────────────
echo
echo "  boot 2: the next start, through the shim the update installed"
power_on none
S2=$(tr -d '\r' < "$T/serial.2")
echo "$S2" | grep -aq 'starting Boot0002 "AurOS"' \
  && ok "the firmware started AurOS's entry" \
  || bad "the firmware started AurOS's entry" "$(echo "$S2" | grep -ao 'BdsDxe:.*' | head -3)"
L=$(e2e_log)
case "$L" in *"2 up mode=none sb=1 "*) ok "...with Secure Boot on (SecureBoot=1, read inside AurOS)" ;;
    *) bad "...with Secure Boot on (SecureBoot=1, read inside AurOS)" "$(echo "$L" | grep '^2 up')" ;; esac
case "$L" in *"2 up "*"BOOT_IMAGE=/boot/vmlinuz-$NEWK "*) ok "grub started the new kernel" ;;
    *) bad "grub started the new kernel" "$(echo "$L" | grep '^2 up')" ;; esac
echo "$S2" | grep -aq "prohibited by secure boot policy" \
  && bad "no Secure Boot refusal on the way (grub's loadfont)" \
  || ok "no Secure Boot refusal on the way (grub's loadfont)"
case "$L" in *"2 desktop after"*) ok "the desktop came up" ;;
    *) bad "the desktop came up" "$(echo "$L" | tail -3)" ;; esac

# ── boot 3: a kernel that will not verify ───────────────────────────
echo
echo "  boot 3: the new kernel damaged; grub must fall back by itself"
mount_root
K="$T/m/boot/vmlinuz-$NEWK"
SZ=$(stat -c %s "$K")
# Flip bytes in the middle of the image: the PE still parses, the
# signature no longer matches, and shim refuses it.
printf 'AUROS-BOOTUPDATETEST-DAMAGE' | dd of="$K" bs=1 seek=$((SZ / 2)) conv=notrunc 2>/dev/null
umount_root
power_on none
S3=$(tr -d '\r' < "$T/serial.3")
L=$(e2e_log)
U3=$(echo "$L" | grep '^3 up')
case "$U3" in *"BOOT_IMAGE=/boot/vmlinuz-$NEWK "*|"") 
        bad "grub fell back to the previous kernel" "${U3:-no boot 3 at all}" \
            "$(echo "$S3" | sed 's/\x1b\[[0-9;=?]*[a-zA-Z]//g' | grep -a 'error\|shim\|Boot' | head -4)" ;;
    *"BOOT_IMAGE=/boot/vmlinuz-6.8.0-"*) ok "grub fell back to the previous kernel" ;;
    *) bad "grub fell back to the previous kernel" "$U3" ;; esac
case "$L" in *"3 desktop after"*) ok "...and the desktop came up" ;;
    *) bad "...and the desktop came up" "$(echo "$L" | tail -3)" ;; esac

[ "$(md5sum "$WIN" | cut -d' ' -f1)" = "$WIN_BEFORE" ] \
  && ok "Windows' disk: byte-for-byte what it was, after three starts and an update" \
  || bad "Windows' disk: byte-for-byte what it was, after three starts and an update"

echo
echo "bootupdatetest: $((checked - fail))/$checked"
[ "$fail" = 0 ]
