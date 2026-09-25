#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  AurOS first boot
#
#  Runs once, before the user sees a desktop. Everything here is
#  idempotent and every step is allowed to fail without taking the boot
#  with it: a machine that reaches a usable desktop with an unfinished
#  migration is recoverable, one that fails to boot is not.
# ═══════════════════════════════════════════════════════════════════
set -u
STAMP=/var/lib/auros/firstboot.done
LOG=/var/log/auros-firstboot.log
mkdir -p /var/lib/auros
exec >>"$LOG" 2>&1
echo "=== AurOS first boot $(date -Is) ==="

[ -f "$STAMP" ] && { echo "already done"; exit 0; }

# ── 1. Grow the root filesystem to fill its partition.
#
# The installer writes a fixed-size image, so the filesystem is smaller
# than the partition it landed in. Growing happens here, where a real
# kernel and resize2fs exist, rather than in the Windows-side installer
# where neither does.
root_src=$(findmnt -n -o SOURCE / 2>/dev/null)
case "$root_src" in
    /dev/*)
        echo "growing $root_src"
        # growpart needs the disk and partition number split out.
        disk=$(lsblk -no PKNAME "$root_src" 2>/dev/null | head -1)
        part=$(echo "$root_src" | grep -o '[0-9]*$')
        if [ -n "$disk" ] && [ -n "$part" ]; then
            growpart "/dev/$disk" "$part" 2>&1 || echo "growpart: nothing to do"
        fi
        resize2fs "$root_src" 2>&1 || echo "resize2fs: nothing to do"
        ;;
    *) echo "root is not a block device ($root_src); skipping grow" ;;
esac

# ── 1b. What the person chose in the installer: language, keyboard,
# time zone, look and desktop. Before the theme step, because the look
# they chose is what that step applies. See choices.sh.
if [ -x /usr/lib/auros/choices.sh ]; then
    /usr/lib/auros/choices.sh 2>&1 || echo "choices: did not finish; defaults kept"
fi

# ── 2. Re-apply the theme.
# The image was themed at build time, but the display resolution is only
# known now, so the wallpaper is regenerated at the real size.
if command -v aurora >/dev/null 2>&1; then
    aurora set "$(cat /etc/auros/theme 2>/dev/null || echo nocturne)" 2>&1 || true
fi

# ── 3. Offer migration if a Windows installation is still on the disk.
# Ferry is not run automatically: it copies personal data and the user
# consents to that in the desktop, not in a boot script. This only
# records what was found so the welcome flow can offer it.
if [ -x /usr/lib/ferry/ferry-detect ]; then
    echo "scanning for a Windows installation"
    /usr/lib/ferry/ferry-detect > /var/lib/auros/windows-found.json 2>/dev/null \
        && echo "scan written to /var/lib/auros/windows-found.json" \
        || echo "no Windows installation found"
fi

touch "$STAMP"
echo "=== first boot complete ==="
