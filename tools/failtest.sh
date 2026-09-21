#!/bin/sh
# failtest.sh — when the desktop does not start, does she see words?
#
# The one screen in this product that only appears when everything else
# has gone wrong, checked the only way it can be: by making everything
# else go wrong on a real machine and looking at the screen.
#
# It took three attempts to get right, and not one of the three
# failures was visible in the unit file.
#
#   1. aurshell.service said OnFailure=getty@tty1.service -- a login
#      prompt, on a machine whose owner has never been told the account
#      name or the password, and whose password the image expires on
#      purpose. To her that is the same screen as a black one.
#   2. A second unit drawing a message was killed two seconds in by the
#      shell's own TTYVHangup=yes. SIGHUP.
#   3. Adding Conflicts= to stop the shell restarting underneath it
#      changed the signal to SIGTERM and nothing else. Conflicts is
#      symmetric; something always starts the shell again.
#
# The answer was to stop having two units. The message is the shell's
# own ExecStopPost. This checks that it stays that way.
#
#   sh tools/failtest.sh                 # needs out/auros-<profile>.img
#   IMG=path/to.img sh tools/failtest.sh
#
# It does NOT copy the image -- they are several gigabytes. It adds a
# drop-in that makes the shell fail, boots with -snapshot so the guest
# writes nothing back, and removes the drop-in again on the way out,
# including if it is interrupted. Nothing it adds is a permission or a
# binary; if it is killed at the worst possible moment, `rm` the file
# it names and the image is as it was.
set -u
cd "$(dirname "$0")/.."

PROFILE="${PROFILE:-desktop}"
IMG="${IMG:-out/auros-$PROFILE.img}"
DROPIN=/etc/systemd/system/aurshell.service.d/99-failtest.conf
WAIT="${WAIT:-420}"

for t in qemu-system-x86_64 qemu-img sgdisk losetup; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ -f "$IMG" ] || { echo "no image at $IMG -- ./build/mkimage $PROFILE"; exit 2; }
[ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] || { echo "need OVMF"; exit 2; }
[ "$(id -u)" = 0 ] || { echo "needs root: it loop-mounts the image"; exit 2; }

WORK=$(mktemp -d)
LOOP=""
MNT="$WORK/mnt"

cleanup() {
    set +e
    if [ -n "$LOOP" ]; then
        mount | grep -q "$MNT" && {
            rm -f "$MNT$DROPIN"
            rmdir "$MNT$(dirname "$DROPIN")" 2>/dev/null
            umount "$MNT"
        }
        losetup -d "$LOOP"
    fi
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

echo "when the desktop does not start, does she see words?"
echo

OFF=$(sgdisk -i 2 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
[ -n "$OFF" ] || { echo "cannot read the partition table of $IMG"; exit 2; }
LOOP=$(losetup -o $((OFF * 512)) --find --show "$IMG") || exit 2
mkdir -p "$MNT"
mount "$LOOP" "$MNT" || exit 2

[ -f "$MNT/usr/bin/aursorry" ] || {
    echo "  this image has no /usr/bin/aursorry, so a failed desktop"
    echo "  has nothing to say. See src/aurshell/sorry.c."
    exit 1
}
grep -q "aursorry" "$MNT/etc/systemd/system/aurshell.service" || {
    echo "  aurshell.service never runs aursorry, so a failed desktop"
    echo "  shows her nothing. See the ExecStopPost in that file."
    exit 1
}
grep -q "OnFailure=getty" "$MNT/etc/systemd/system/aurshell.service" && {
    echo "  aurshell.service falls back to a login prompt, which is a"
    echo "  screen she cannot use: she has never been told the account"
    echo "  name, and this image expires the password on purpose."
    exit 1
}

# Break the desktop the gentlest way there is: a drop-in, not a chmod.
mkdir -p "$MNT$(dirname "$DROPIN")"
cat > "$MNT$DROPIN" <<EOL
# Added by tools/failtest.sh and removed by it. If this file is still
# here, that run was interrupted -- delete it.
[Service]
ExecStart=
ExecStart=/bin/false
EOL
sync
umount "$MNT"; losetup -d "$LOOP"; LOOP=""
echo "  the desktop in this image is now set to fail on purpose"

cp /usr/share/OVMF/OVMF_VARS_4M.fd "$WORK/vars.fd"
qemu-system-x86_64 -machine q35,accel=tcg -m 2048 -smp "$(nproc)" \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file="$WORK/vars.fd" \
  -drive file="$IMG",format=raw,if=virtio,snapshot=on \
  -device virtio-vga -display none \
  -serial file:"$WORK/serial.log" \
  -qmp unix:"$WORK/qmp.sock",server,nowait \
  -no-reboot > "$WORK/qemu.out" 2>&1 &
QPID=$!
echo "  booting it (this is emulated, so it is slow)"

# The screen is the answer. Sample it until something is drawn on it.
best=0
i=0
while [ "$i" -lt "$WAIT" ]; do
    sleep 15; i=$((i + 15))
    kill -0 "$QPID" 2>/dev/null || { echo "  the machine stopped"; break; }
    [ -S "$WORK/qmp.sock" ] || continue
    python3 - "$WORK/qmp.sock" "$WORK/f.ppm" <<'PY' >/dev/null 2>&1 || continue
import socket, json, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
f = s.makefile('rw'); f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
f.write(json.dumps({"execute": "screendump",
                    "arguments": {"filename": sys.argv[2]}}) + "\n"); f.flush()
f.readline(); s.close()
PY
    [ -f "$WORK/f.ppm" ] || continue
    n=$(tail -c +20 "$WORK/f.ppm" | tr -d '\000-\060' | wc -c)
    [ "$n" -gt "$best" ] && { best=$n; cp "$WORK/f.ppm" "$WORK/best.ppm"; }
    # A screen with this much light on it is text, not a cursor.
    if [ "$n" -gt 40000 ]; then
        echo "  at t+${i}s there are words on the screen"
        [ -n "${KEEP:-}" ] && cp "$WORK/best.ppm" "$KEEP"
        echo
        echo "a machine whose desktop cannot start says so, in words,"
        echo "on the screen the person is looking at."
        exit 0
    fi
done

echo
echo "the screen stayed dark for ${i}s (brightest sample: $best)."
echo "A person whose desktop failed is looking at nothing. The journal"
echo "of the booted machine is the place to start:"
echo "  journalctl -b -u aurshell"
[ -n "${KEEP:-}" ] && [ -f "$WORK/best.ppm" ] && cp "$WORK/best.ppm" "$KEEP"
exit 1
