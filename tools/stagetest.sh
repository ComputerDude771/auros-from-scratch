#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  stagetest — does the staging environment come up, and does it
#  really leave the disk alone?
#
#  The staging environment is where the one restart lands and where
#  every destructive step will happen. docs/AURBRIDGE.md makes one
#  requirement of it above all others:
#
#    "Abort at any phase <= 7 leaves the machine bootable into
#     Windows. That is a hard requirement, tested, not a goal."
#
#  Stage A is the part of that which can be tested before anything
#  destructive exists: the environment boots, looks at a real disk
#  with a real NTFS filesystem on it, and changes NOT ONE BYTE.
#
#  So every case below hashes the whole disk before and after. A
#  staging environment that is merely *intended* not to write is one
#  nobody can ship; the hash is what makes it a fact.
#
#    sudo sh tools/stagetest.sh        # needs qemu, losetup, the image
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
RFS="${RFS:-work/forge/desktop/rootfs}"

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        [ $# -gt 1 ] && printf '      %s\n' "$2"; }

for t in qemu-system-x86_64 losetup sgdisk mkfs.ext4 mkfs.vfat; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ -d "$RFS" ] || { echo "no built rootfs at $RFS"; exit 2; }
[ -f out/auros-staging.img ] || { echo "run ./build/staging first"; exit 2; }

LD=$(ls "$RFS"/lib64/ld-linux-x86-64.so.2 2>/dev/null || \
     ls "$RFS"/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 2>/dev/null)
LP="$RFS/lib/x86_64-linux-gnu:$RFS/lib64:$RFS/usr/lib/x86_64-linux-gnu"
[ -n "$LD" ] || { echo "no loader in the image"; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/stagetest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

echo
echo "Does the staging environment leave the disk alone?"
echo

# ── a machine shaped like the ones this product is for ──────────────
#
# An ESP, a Windows volume with a REAL NTFS filesystem on it -- made by
# the same mkntfs the image ships, so the thing being read is the thing
# that will be read on a customer's disk -- and a root partition with
# something that will run as init.
echo "  building a synthetic machine"
DISK="$TMP/disk.img"
truncate -s 8G "$DISK"
sgdisk --zap-all "$DISK" >/dev/null 2>&1
sgdisk -n 1:2048:+100M -t 1:ef00 -c 1:"EFI"     "$DISK" >/dev/null 2>&1
sgdisk -n 2:0:+4G      -t 2:0700 -c 2:"Windows" "$DISK" >/dev/null 2>&1
sgdisk -n 3:0:0        -t 3:8304 -c 3:"AurOS"   "$DISK" >/dev/null 2>&1

mkpart() { # start-sector end-sector kind
    off=$(( $1 * 512 )); len=$(( ($2 - $1 + 1) * 512 ))
    l=$(losetup --find --show -o "$off" --sizelimit "$len" "$DISK") || return 1
    case "$3" in
      ntfs) "$LD" --library-path "$LP" "$RFS/usr/sbin/mkntfs" -Q -F -L WINDOWS "$l" >/dev/null 2>&1 ;;
      vfat) mkfs.vfat -n EFI "$l" >/dev/null 2>&1 ;;
      ext4) mkfs.ext4 -q -L AurOS "$l" >/dev/null 2>&1 ;;
    esac
    rc=$?
    losetup -d "$l"
    return $rc
}
mkpart 2048 206847 vfat     || { echo "  could not make the ESP"; exit 2; }
mkpart 206848 8595455 ntfs  || { echo "  could not make the NTFS volume"; exit 2; }
mkpart 8595456 16777182 ext4|| { echo "  could not make the root"; exit 2; }

# Something to hand over TO. A whole AurOS is not needed to prove the
# handover; a static binary that says it is running is.
cat > "$TMP/fakeinit.c" <<'EOC'
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
int main(void){ puts("\nSTAGETEST-INSTALLED-SYSTEM-RAN"); fflush(stdout);
                sync(); sleep(2); reboot(RB_POWER_OFF); for(;;) pause(); }
EOC
cc -static -O2 -o "$TMP/fakeinit" "$TMP/fakeinit.c" 2>/dev/null || \
    { echo "  could not build the stand-in init"; exit 2; }
L=$(losetup --find --show -o $((8595456*512)) --sizelimit $(((16777182-8595456+1)*512)) "$DISK")
mkdir -p "$TMP/mnt"
mount "$L" "$TMP/mnt" && mkdir -p "$TMP/mnt/sbin" "$TMP/mnt/proc" "$TMP/mnt/sys" \
                                  "$TMP/mnt/dev" "$TMP/mnt/run" &&
    cp "$TMP/fakeinit" "$TMP/mnt/sbin/init" && sync && umount "$TMP/mnt"
losetup -d "$L"

run_case() { # name  kernel-args  expect-in-output
    cp "$DISK" "$TMP/run.img"
    before=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    timeout 300 qemu-system-x86_64 -machine q35,accel=tcg -m 1024 -smp 2 \
        -no-reboot -kernel out/auros-staging-vmlinuz \
        -initrd out/auros-staging.img -append "console=ttyS0 $2" \
        -drive file="$TMP/run.img",format=raw,if=virtio \
        -display none -serial stdio > "$TMP/out.txt" 2>&1
    after=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    if grep -aq "$3" "$TMP/out.txt"; then ok "$1"
    else bad "$1" "expected to see: $3"; tail -4 "$TMP/out.txt" | sed 's/^/        /'; fi
    # THE CHECK THIS FILE EXISTS FOR.
    if [ "$before" = "$after" ]; then
        ok "...and the disk is byte-for-byte unchanged"
    else
        bad "...and the disk is byte-for-byte unchanged" \
            "stage A wrote to the disk. Nothing in it may."
    fi
}

echo
echo "  look and do not touch"
run_case "it finds the Windows volume"        "aurstage.dry" "vda2 .*ntfs"
echo
echo "  handing over to the installed system"
run_case "the installed system runs, same boot" "aurstage.root=/dev/vda3" \
         "STAGETEST-INSTALLED-SYSTEM-RAN"
echo
echo "  and when something is wrong"
# Nobody said which system to start. It must say so and stop -- not
# guess, and not panic. Guessing is how a machine with two disks gets
# the wrong one.
run_case "no root named: it says so and stops" "aurstage.quiet" \
         "nobody said which system to start"
# A root that is not there. Same rule.
run_case "a root that is not there: it says so" "aurstage.root=/dev/vda9" \
         "could not be started"

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "The staging environment is where the one restart lands. If it"
    echo "writes to the disk before it is supposed to, there is no way back."
    exit 1
fi
echo "$checked checks: the staging environment comes up, finds the disk,"
echo "hands over in the same boot, and changes nothing."
exit 0
