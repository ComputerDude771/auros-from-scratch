#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  installtest — the whole thing, on a synthetic machine
#
#  Everything else in tools/ tests a part. This boots the staging
#  environment on a machine shaped like the ones this product is for,
#  with a real NTFS Windows volume with real files in it and a
#  recovery stick, and lets it INSTALL -- shrink, write, verify,
#  probe, commit, grow, hand over.
#
#  Then it asks the questions that matter:
#
#    - did the installed system actually start?
#    - is every file in Windows still exactly what it was?
#    - is Windows' partition still bootable, and smaller?
#    - does another tool agree the new table is valid?
#
#  And for every refusal case: is the disk byte-for-byte unchanged?
#
#    sudo sh tools/installtest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-58s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-58s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

# THE MACHINE COMES FROM ONE FILE, shared with the power-cut matrix.
#
# Both tests used to build the same synthetic computer from their own
# copy of the same seventy lines. Two fixtures meant to be identical and
# maintained separately are two fixtures that eventually are not, and an
# audit of the power-cut matrix found exactly that: its copy was missing
# the check this one added after a real bug, so it was passing a restore
# that never grew Windows back.
. tools/machine.sh
mach_need
[ -f out/auros-staging.img ] || { echo "run ./build/staging first"; exit 2; }

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another end-to-end test is running"; exit 2; }
MTMP=$(mktemp -d "${TMPDIR:-/tmp}/installtest.XXXXXX")
[ -n "$MTMP" ] && [ -d "$MTMP" ] || { rmdir "$LOCK"; echo "no scratch dir"; exit 2; }
TMP="$MTMP"
trap 'mountpoint -q "$MTMP/m" 2>/dev/null && umount "$MTMP/m"; rm -rf "$MTMP" "$LOCK"' EXIT

echo
echo "Does it actually install AurOS, and leave Windows intact?"
echo

mach_disk
mach_image
mach_stick
mach_journal
echo "  a 3 GiB machine: ESP, a 1 GiB Windows with $WINFILES files, WinRE at the end"
echo "  a stick with a $((RLEN/1048576)) MiB AurOS image, a record area and room for the way back"

# The stick, remade -- optionally with the image on it damaged.
mkstick() { mach_stick "${1:-}"; }

# Which disk each run starts from. The install runs start from the
# pristine machine; the restore runs start from the machine as the
# install left it, which is the only honest way to ask whether the way
# back works.
SRCDISK="$DISK"
run() { # name  kargs  expect  stick-file  want-unchanged
    cp --sparse=always "$SRCDISK" "$TMP/run.img"
    cp --sparse=always "${4:-$STICK}" "$TMP/stk.img"
    cat out/auros-staging.img "$JNL" > "$TMP/initrd.img"
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$TMP/vars.fd"
    before=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    : > "$TMP/out.txt"
    qemu-system-x86_64 -machine q35,accel=tcg -m 1536 -smp 2 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,unit=1,file="$TMP/vars.fd" \
        -no-reboot -kernel out/auros-staging-vmlinuz \
        -initrd "$TMP/initrd.img" -append "console=ttyS0 $2" \
        -drive file="$TMP/run.img",format=raw,if=none,id=d0 \
        -device virtio-blk-pci,drive=d0,serial=AUROSTEST \
        -drive file="$TMP/stk.img",format=raw,if=none,id=d1 \
        -device virtio-blk-pci,drive=d1,serial=AUROSSTICK \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -display none -serial stdio > "$TMP/out.txt" 2>&1 &
    qp=$!
    seen=0; i=0
    while [ "$i" -lt 900 ]; do
        kill -0 "$qp" 2>/dev/null || break
        if [ "$seen" -eq 0 ] && grep -aq "$3" "$TMP/out.txt" 2>/dev/null; then
            seen=1; i=880
        fi
        sleep 1; i=$((i+1))
    done
    kill -9 "$qp" 2>/dev/null; wait "$qp" 2>/dev/null
    after=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    if grep -aq "$3" "$TMP/out.txt"; then ok "$1"
    else bad "$1" "expected: $3"
         grep -a "aurstage:" "$TMP/out.txt" | tail -6 | sed 's/^/        /'; fi
    if [ "${5:-}" = "unchanged" ]; then
        [ "$before" = "$after" ] && ok "...and the disk is byte-for-byte unchanged" \
                                 || bad "...and the disk is byte-for-byte unchanged"
    fi
}

# An empty second disk, for the case where the stick is not there.
truncate -s 64M "$TMP/blank.img"

echo
echo "  refusals, with the disk untouched"
run "no stick in the computer: refused" \
    "aurstage.install aurstage.min_gb=1" "memory stick is not in this computer" \
    "$TMP/blank.img" unchanged
mkstick corrupt
run "a damaged image on the stick: refused" \
    "aurstage.install aurstage.min_gb=1" "copy of AurOS on the memory stick is damaged" \
    "$STICK" unchanged
# AND DAMAGE TO THE PART THAT STARTS A COMPUTER, which is a different
# extent and used to be checked by nothing at all: the installer copied
# it onto the machine and read it back against the stick -- the same
# bytes it had just written -- so rot in the shim or in grub installed
# cleanly, said so, and left a machine that starts nothing.
mkstick corrupt-esp
run "a damaged boot chain on the stick: refused" \
    "aurstage.install aurstage.min_gb=1" \
    "part of the memory stick that starts a computer is damaged" \
    "$STICK" unchanged
# THE STICK IS PUT BACK FIRST, and it matters: the case below must
# refuse for the profile and for nothing else, and the stick it would
# otherwise have used is the one with a damaged boot chain.
mkstick

# AND A STICK FOR A DIFFERENT AUROS. image_find() walks every disk in
# the machine and every AUROS-IMAGE partition on each, so a second
# AurOS stick left plugged in is a second candidate and the staging
# environment used to install whichever it reached first. It has taken
# a wanted profile since the day it was written; the one caller passed
# NULL, written as `j.stage[0] ? NULL : NULL` so that it looked like a
# decision. The journal names it now.
#
# The stick here holds one image, so what this case proves is the
# comparison and the refusal. That the RIGHT one of several is chosen
# is tools/imagetest.sh's two-image fixture, which is where a stick
# with two AUROS-IMAGE partitions can actually be built.
mach_journal office
run "a stick for a different AurOS: refused" \
    "aurstage.install aurstage.min_gb=1" \
    "for a different version of AurOS" \
    "$STICK" unchanged
mach_journal

echo
echo "  and then installing, for real"
run "it installs and the new system starts" \
    "aurstage.install aurstage.min_gb=1" "INSTALLTEST-AUROS-STARTED"
# AND THE SECOND COPY OF THE WAY BACK, ASSERTED HERE.
#
# Failing to keep it is deliberately a warning and not a give_up --
# AurOS is installed and working by then, and the copy that matters is
# on the stick. So when rescue_mirror started writing through a window
# nobody had armed, the install still said it had succeeded, and what
# noticed was a restore case six checks further down whose message was
# about a memory stick that was not even plugged in.
#
# The install says whether it kept the copy. This reads that sentence.
if grep -aq "a second copy is on this computer" "$TMP/out.txt"; then
    ok "...and kept a second copy of the way back on the computer"
else
    bad "...and kept a second copy of the way back on the computer" \
        "$(grep -a 'way back' "$TMP/out.txt" | tail -2)"
fi

echo
echo "  and what it left behind"
if sgdisk -v "$TMP/run.img" 2>&1 | grep -q "No problems found"; then
    ok "another tool agrees the new table is valid"
else
    bad "another tool agrees the new table is valid" \
        "$(sgdisk -v "$TMP/run.img" 2>&1 | head -3)"
fi
N=$(sgdisk -p "$TMP/run.img" 2>/dev/null | sed -n '/^Number/,$p' | tail -n +2 | grep -c .)
[ "$N" = "6" ] && ok "the disk now has 6 partitions" \
               || bad "the disk now has 6 partitions" "it has $N"
sgdisk -p "$TMP/run.img" 2>/dev/null | sed -n '/^Number/,$p' | tail -n +2 |
    awk 'NF{printf "      p%s %s..%s %s\n",$1,$2,$3,$7}'

NEWEND=$(sgdisk -i 2 "$TMP/run.img" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
if [ -n "$NEWEND" ] && [ "$NEWEND" -lt "$P2E" ]; then
    ok "Windows is smaller than it was"
else
    bad "Windows is smaller than it was" "was $P2E, now $NEWEND"
fi

L=$(losetup --find --show -o $((P2S*512)) --sizelimit $(((NEWEND-P2S+1)*512)) "$TMP/run.img" 2>/dev/null)
if [ -n "$L" ] && nt ntfs-3g "$L" "$TMP/m" >/dev/null 2>&1; then
    ok "the Windows volume still mounts"
    ( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$TMP/win.after"
    umount "$TMP/m"
    if cmp -s "$TMP/win.before" "$TMP/win.after"; then
        ok "and all $WINFILES files in it are byte-for-byte what they were"
    else
        bad "and all $WINFILES files in it are byte-for-byte what they were" \
            "$(diff "$TMP/win.before" "$TMP/win.after" | head -4)"
    fi
else
    bad "the Windows volume still mounts" "it does not"
fi
[ -n "$L" ] && losetup -d "$L"

# The record on the stick should say the install finished.
if grep -aq "verdict=installed" "$TMP/out.txt"; then
    ok "the installer reported success"
else
    bad "the installer reported success" "$(grep -a 'aurstage-report' "$TMP/out.txt" | tail -1)"
fi

# ════════════════════════════════════════════════════════════════════
#  And now the other direction: put Windows back.
#
#  This is the half of the product that decides whether the whole thing
#  is honest. Rule 2 of docs/AURBRIDGE.md says the machine can always
#  go back; every sentence in the wizard rests on it. So the machine
#  the installer just changed is handed to the restore, and afterwards
#  it has to be the machine we started with -- the same three
#  partitions at the same sectors, the same EFI partition byte for
#  byte, and the same files in Windows with the same md5sums as the
#  ones taken before anything was touched.
# ════════════════════════════════════════════════════════════════════
cp --sparse=always "$TMP/run.img" "$TMP/installed.img"
# AND THE STICK AS THE INSTALL LEFT IT, which is not the same object.
#
# run() copies the fixture stick for every boot, so the first version of
# this handed the restore a FRESH stick with nothing saved on it -- and
# the restore quietly fell back to the copy on the computer and passed.
# Two checks that said "from the stick" were both testing the same thing
# as the two below them, and the stick path was not exercised at all.
cp --sparse=always "$TMP/stk.img" "$TMP/stick-after.img"
SRCDISK="$TMP/installed.img"

check_back() { # label
    if sgdisk -v "$TMP/run.img" 2>&1 | grep -q "No problems found"; then
        ok "$1: the table is valid again"
    else
        bad "$1: the table is valid again" \
            "$(sgdisk -v "$TMP/run.img" 2>&1 | head -3)"
    fi
    N=$(sgdisk -p "$TMP/run.img" 2>/dev/null | sed -n '/^Number/,$p' |
        tail -n +2 | grep -c .)
    [ "$N" = "3" ] && ok "$1: the three original partitions are back" \
                   || bad "$1: the three original partitions are back" "it has $N"
    E=$(sgdisk -i 2 "$TMP/run.img" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
    S=$(sgdisk -i 2 "$TMP/run.img" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    if [ "$S" = "$P2S" ] && [ "$E" = "$P2E" ]; then
        ok "$1: Windows is its full size again"
    else
        bad "$1: Windows is its full size again" "$S..$E, was $P2S..$P2E"
    fi
    M=$(dd if="$TMP/run.img" bs=512 skip=$P1S count=$((P1E-P1S+1)) status=none |
        md5sum | cut -d' ' -f1)
    [ "$M" = "$ESPMD5" ] && ok "$1: the EFI partition is byte-for-byte what it was" \
                         || bad "$1: the EFI partition is byte-for-byte what it was"
    # WITHIN ONE CLUSTER, and not exactly.
    #
    # mkntfs sets total_sectors to one less than the partition, which
    # is not a multiple of the cluster size; ntfsresize can only land
    # on (floor(size/cluster) - 1) x sectors_per_cluster. So the
    # original number is not reachable by any resize at all, and the
    # best a correct restore can do is come back up to one cluster
    # short. Windows shows C: at its full size either way. Demanding
    # equality here would report every correct restore as broken --
    # and accepting anything looser would have hidden the real bug
    # this check was added for, which left the filesystem at 16408
    # sectors out of 2097151.
    T2=$(ntfs_total "$TMP/run.img" "$S" | cut -d' ' -f1)
    D=$((NTFSTOT - T2))
    if [ "$D" -ge 0 ] && [ "$D" -le "$NTFSSPC" ]; then
        ok "$1: the Windows FILESYSTEM is its full size again"
    else
        bad "$1: the Windows FILESYSTEM is its full size again" \
            "it claims $T2 sectors, it had $NTFSTOT (one cluster is $NTFSSPC)"
    fi
    if grep -aq "verdict=restored .*grown=1 small=0" "$TMP/out.txt"; then
        ok "$1: and the installer says so itself"
    else
        bad "$1: and the installer says so itself" \
            "$(grep -a 'aurstage-report' "$TMP/out.txt" | tail -1)"
    fi
    L=$(losetup --find --show -o $((P2S*512)) \
        --sizelimit $(((P2E-P2S+1)*512)) "$TMP/run.img" 2>/dev/null)
    if [ -n "$L" ] && nt ntfs-3g "$L" "$TMP/m" >/dev/null 2>&1; then
        ( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$TMP/win.back"
        umount "$TMP/m"
        if cmp -s "$TMP/win.before" "$TMP/win.back"; then
            ok "$1: all $WINFILES files in Windows are exactly what they were"
        else
            bad "$1: all $WINFILES files in Windows are exactly what they were" \
                "$(diff "$TMP/win.before" "$TMP/win.back" | head -4)"
        fi
    else
        bad "$1: the Windows volume mounts again" "it does not"
    fi
    [ -n "$L" ] && losetup -d "$L"
}

echo
echo "  and then putting Windows back, from the memory stick"
run "it restores and says so" "aurstage.restore" "verdict=restored" \
    "$TMP/stick-after.img"
if grep -aq "using the saved copy on /dev/vdb" "$TMP/out.txt"; then
    ok "and it used the copy on the stick, not the one on the computer"
else
    bad "and it used the copy on the stick, not the one on the computer" \
        "$(grep -a 'using the saved copy' "$TMP/out.txt" | tail -1)"
fi
check_back "from the stick"

echo
echo "  and again with no stick at all, from the copy on the computer"
run "it restores from the copy it left on this computer" \
    "aurstage.restore" "verdict=restored" "$TMP/blank.img"
if grep -aq "using the saved copy on /dev/vda" "$TMP/out.txt"; then
    ok "and it used the copy on the computer, the stick being absent"
else
    bad "and it used the copy on the computer, the stick being absent" \
        "$(grep -a 'using the saved copy' "$TMP/out.txt" | tail -1)"
fi
check_back "from the computer"

echo
echo "  and a refusal, with the disk untouched"
# A stick whose saved copy has been damaged, and no second copy to fall
# back on: the restore must refuse rather than write half a table.
cp --sparse=always "$TMP/installed.img" "$TMP/wiped.img"
python3 - "$TMP/wiped.img" <<'EOPY'
import sys, subprocess, re
# Blank the AUROS-SAVED partition on the installed disk, so the only
# copy left is the damaged one on the stick.
p = subprocess.run(["sgdisk","-p",sys.argv[1]],capture_output=True,text=True).stdout
for line in p.splitlines():
    f = line.split()
    if len(f) >= 7 and f[0].isdigit() and "AUROS-SAVED" in line:
        f_ = open(sys.argv[1],"r+b"); f_.seek(int(f[1])*512)
        f_.write(b"\0" * 4096); f_.close()
EOPY
cp --sparse=always "$TMP/stick-after.img" "$TMP/badstick.img"
python3 - "$TMP/badstick.img" <<'EOPY'
import sys, subprocess
p = subprocess.run(["sgdisk","-p",sys.argv[1]],capture_output=True,text=True).stdout
for line in p.splitlines():
    f = line.split()
    if len(f) >= 7 and f[0].isdigit() and "AUROS-SAVED" in line:
        fh = open(sys.argv[1],"r+b"); fh.seek(int(f[1])*512 + 4096 + 1024)
        fh.write(b"\xff" * 512); fh.close()
EOPY
SRCDISK="$TMP/wiped.img"
# THE MARKER MUST NOT BE A PREFIX OF SUCCESS.
#
# This waited for "aurstage-report v1 verdict=restore", which is a
# prefix of "verdict=restored record=done ..." -- so a restore that
# BELIEVED the corrupted copy printed a line matching it and the row
# said "refused ok". The disk-unchanged check would usually have
# caught it, but the restore is idempotent by design, so a run that
# wrote back exactly what was already there would have passed both.
run "a damaged saved copy: refused, disk untouched" \
    "aurstage.restore" "aurstage-report v1 verdict=restore-" \
    "$TMP/badstick.img" unchanged
if grep -aq "verdict=restored" "$TMP/out.txt"; then
    bad "...and it did not quietly restore anyway" \
        "$(grep -a 'aurstage-report' "$TMP/out.txt" | tail -1)"
else
    ok "...and it did not quietly restore anyway"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "This is the whole product, on one synthetic machine."
    exit 1
fi
echo "$checked checks: it installs AurOS, starts it, puts Windows back"
echo "again, and every file in Windows is exactly what it was."
exit 0
