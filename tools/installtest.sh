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
RFS="${RFS:-work/forge/desktop/rootfs}"

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-58s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-58s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

for t in qemu-system-x86_64 sgdisk mkfs.ext4 cpio python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ -f out/auros-staging.img ] || { echo "run ./build/staging first"; exit 2; }
[ -d "$RFS" ] || { echo "no rootfs at $RFS"; exit 2; }
[ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] || { echo "need OVMF"; exit 2; }

LD=$(ls "$RFS"/lib64/ld-linux-x86-64.so.2 2>/dev/null || \
     ls "$RFS"/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 2>/dev/null)
LP="$RFS/lib/x86_64-linux-gnu:$RFS/lib64:$RFS/usr/lib/x86_64-linux-gnu"
nt() { p="$RFS/usr/sbin/$1"; [ -x "$p" ] || p="$RFS/usr/bin/$1"; shift
       "$LD" --library-path "$LP" "$p" "$@"; }

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another installtest is running"; exit 2; }
TMP=$(mktemp -d "${TMPDIR:-/tmp}/installtest.XXXXXX")
trap 'mountpoint -q "$TMP/m" 2>/dev/null && umount "$TMP/m"; rm -rf "$TMP" "$LOCK"' EXIT

echo
echo "Does it actually install AurOS, and leave Windows intact?"
echo

# ── the machine ─────────────────────────────────────────────────────
P1S=2048;    P1E=206847       # ESP, 100 MiB
P2S=206848;  P2E=2303999      # Windows, 1 GiB
P3S=5500000; P3E=6291422      # WinRE, at the very end
DISK="$TMP/disk.img"
truncate -s 3G "$DISK"
sgdisk --zap-all "$DISK" >/dev/null 2>&1
sgdisk -n 1:$P1S:$P1E -t 1:ef00 -c 1:"EFI"     "$DISK" >/dev/null 2>&1
sgdisk -n 2:$P2S:$P2E -t 2:0700 -c 2:"Windows" "$DISK" >/dev/null 2>&1
sgdisk -n 3:$P3S:$P3E -t 3:2700 -c 3:"WinRE"   "$DISK" >/dev/null 2>&1

mkpart() {
    off=$(( $1 * 512 )); len=$(( ($2 - $1 + 1) * 512 ))
    l=$(losetup --find --show -o "$off" --sizelimit "$len" "$DISK") || return 1
    case "$3" in
      ntfs) nt mkntfs -Q -F -L WINDOWS "$l" >/dev/null 2>&1 ;;
      vfat) mkfs.vfat -n EFI "$l" >/dev/null 2>&1 ;;
      ext4) mkfs.ext4 -q -L WINRE "$l" >/dev/null 2>&1 ;;
    esac
    rc=$?; losetup -d "$l"; return $rc
}
mkpart $P1S $P1E vfat || { echo "  no ESP"; exit 2; }
mkpart $P2S $P2E ntfs || { echo "  no NTFS"; exit 2; }
mkpart $P3S $P3E ext4 || { echo "  no WinRE"; exit 2; }

# Real files in Windows, so "intact" means something.
L=$(losetup --find --show -o $((P2S*512)) --sizelimit $(((P2E-P2S+1)*512)) "$DISK")
mkdir -p "$TMP/m"
if nt ntfs-3g "$L" "$TMP/m" >/dev/null 2>&1; then
    mkdir -p "$TMP/m/Users/auros/Pictures"
    for i in 1 2 3 4 5 6; do
        dd if=/dev/urandom of="$TMP/m/Users/auros/Pictures/p$i.jpg" \
           bs=64k count=1 status=none
    done
    dd if=/dev/urandom of="$TMP/m/Users/auros/thesis.odt" bs=1M count=2 status=none
    ( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$TMP/win.before"
    sync; umount "$TMP/m"
else
    echo "  cannot write to the NTFS volume (no FUSE?)"; losetup -d "$L"; exit 2
fi
losetup -d "$L"
WINFILES=$(wc -l < "$TMP/win.before")
# What the EFI partition held before any of this. The restore has to
# put these bytes back or Windows does not start, and nothing else in
# this test would notice if it put back something almost right.
ESPMD5=$(dd if="$DISK" bs=512 skip=$P1S count=$((P1E-P1S+1)) status=none | md5sum | cut -d" " -f1)
# THE FILESYSTEM'S OWN SIZE, not the partition entry's.
#
# Checking only the partition table let a restore pass while the NTFS
# inside was still its shrunken size -- Windows would start and show a
# smaller C: than it had, which is exactly the thing "put Windows back"
# promises not to do. total_sectors lives at offset 0x28 of the boot
# sector and is the number the filesystem itself claims.
ntfs_total() { # image  first_sector  ->  "total_sectors sectors_per_cluster"
    python3 - "$1" "$2" <<'EOPY'
import sys, struct
f = open(sys.argv[1], 'rb'); f.seek(int(sys.argv[2]) * 512)
b = f.read(512); f.close()
print(struct.unpack_from('<Q', b, 0x28)[0], b[0x0d])
EOPY
}
NTFSTOT=$(ntfs_total "$DISK" $P2S | cut -d' ' -f1)
NTFSSPC=$(ntfs_total "$DISK" $P2S | cut -d' ' -f2)
echo "  a 3 GiB machine: ESP, a 1 GiB Windows with $WINFILES files, WinRE at the end"

# ── a small AurOS image ─────────────────────────────────────────────
AIMG="$TMP/auros.img"
truncate -s 320M "$AIMG"
sgdisk --zap-all "$AIMG" >/dev/null 2>&1
sgdisk -n 1:2048:+16M -t 1:ef00 -c 1:"AUROS-ESP"  "$AIMG" >/dev/null 2>&1
sgdisk -n 2:0:0       -t 2:8304 -c 2:"AUROS-ROOT" "$AIMG" >/dev/null 2>&1
RS=$(sgdisk -i 2 "$AIMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
RE=$(sgdisk -i 2 "$AIMG" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
ROFF=$((RS*512)); RLEN=$(((RE-RS+1)*512))

cat > "$TMP/init.c" <<'EOC'
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
int main(void){ puts("\nINSTALLTEST-AUROS-STARTED"); fflush(stdout);
                sync(); sleep(2); reboot(RB_POWER_OFF); for(;;) pause(); }
EOC
cc -static -O2 -o "$TMP/init" "$TMP/init.c" 2>/dev/null || { echo "  no cc"; exit 2; }

dd if=/dev/zero of="$TMP/root.img" bs=1M count=$((RLEN/1048576)) status=none
mkfs.ext4 -q -L AUROS-ROOT "$TMP/root.img"
# A REAL MODULE TREE. Phase 7 mounts this filesystem and loads ITS
# drivers -- that is the whole point of probing after the write -- and
# an image with an empty /lib/modules is, correctly, reported as our
# fault rather than the machine's. So the fixture carries the same
# modules the staging image does, which is what a real AurOS image
# carries: the ones for the kernel it ships with.
KVER=$(ls work/staging/lib/modules 2>/dev/null | head -1)
mount -o loop "$TMP/root.img" "$TMP/m" && {
    mkdir -p "$TMP/m/sbin" "$TMP/m/proc" "$TMP/m/sys" "$TMP/m/dev" "$TMP/m/run"
    cp "$TMP/init" "$TMP/m/sbin/init"
    if [ -n "$KVER" ]; then
        mkdir -p "$TMP/m/lib/modules"
        cp -a "work/staging/lib/modules/$KVER" "$TMP/m/lib/modules/" 2>/dev/null
    fi
    sync; umount "$TMP/m"
}
[ -n "$KVER" ] || { echo "  no built module tree to put in the image"; exit 2; }
dd if="$TMP/root.img" of="$AIMG" bs=1M seek=$((ROFF/1048576)) conv=notrunc status=none

# ── the recovery stick: an image partition and a record partition ───
STICK="$TMP/stick.img"
mkstick() { # [corrupt]
    rm -f "$STICK"; truncate -s 768M "$STICK"
    sgdisk --zap-all "$STICK" >/dev/null 2>&1
    sgdisk -n 1:2048:+400M -t 1:A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41 \
           -c 1:"AUROS-IMAGE" "$STICK" >/dev/null 2>&1
    sgdisk -n 2:0:+4M      -t 2:7E1C3B90-4D2A-4F16-8B77-2C6E5A9D0E33 \
           -c 2:"AUROS-RECORD" "$STICK" >/dev/null 2>&1
    # Room for a copy of this machine's Windows startup. Dominated by
    # the ESP, which is 100 MiB here and is a gigabyte on some OEM
    # laptops -- AurBridge sizes this from the machine it looked at.
    sgdisk -n 3:0:+180M    -t 3:7E1C3B90-4D2A-4F16-8B77-2C6E5A9D0E34 \
           -c 3:"AUROS-SAVED" "$STICK" >/dev/null 2>&1
    IS=$(sgdisk -i 1 "$STICK" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    python3 - "$STICK" "$AIMG" "$((IS*512))" "$ROFF" "$RLEN" "${1:-}" <<'EOPY'
import sys, struct, hashlib
stick, img, pstart, roff, rlen, corrupt = sys.argv[1:7]
pstart, roff, rlen = int(pstart), int(roff), int(rlen)
data = bytearray(open(img,'rb').read())
sha = hashlib.sha256(bytes(data[roff:roff+rlen])).digest()
if corrupt: data[roff + rlen//2] ^= 0xFF
man = bytearray(4096)
man[0:8] = b'AURIMG01'
struct.pack_into('<Q', man, 8, len(data))
struct.pack_into('<Q', man, 16, roff)
struct.pack_into('<Q', man, 24, rlen)
struct.pack_into('<I', man, 32, 512)
man[36:68] = sha
man[68:75] = b'desktop'
f = open(stick,'r+b'); f.seek(pstart); f.write(man)
f.seek(pstart+4096); f.write(bytes(data)); f.close()
EOPY
}
mkstick
echo "  a stick with a $((RLEN/1048576)) MiB AurOS image, a record area and room for the way back"

# ── the journal AurBridge would have written ────────────────────────
journal() { # out  serial  start  sectors  hash  boot_from
    mkdir -p "$TMP/j/aurbridge"
    J="$TMP/j/aurbridge/journal.json"
    printf '{"disk_serial":"%s","disk_model":"QEMU",' "$2" > "$J"
    printf '"disk_bytes":3221225472,"logical_sector":512,' >> "$J"
    printf '"win_part":"2","win_start_lba":%s,"win_sectors":%s,' "$3" "$4" >> "$J"
    printf '"win_ntfs_serial":0,"gpt_sha256":"%s","stage":"armed",' "$5" >> "$J"
    printf '"boot_from":"%s","run_id":%s,' "$6" "$(date +%s)" >> "$J"
    printf '"written_unix":%s}\n' "$(date +%s)" >> "$J"
    ( cd "$TMP/j" && find . -print0 | cpio --null -o --format=newc --quiet ) \
        | gzip -9 > "$1"
    rm -rf "$TMP/j"
}
gpthash() { python3 - "$1" <<'EOPY'
import sys, struct, hashlib
ss=512; f=open(sys.argv[1],'rb'); f.seek(ss); h=f.read(ss)
hs=struct.unpack_from('<I',h,12)[0]; pl=struct.unpack_from('<Q',h,72)[0]
n=struct.unpack_from('<I',h,80)[0]; e=struct.unpack_from('<I',h,84)[0]
f.seek(pl*ss); print(hashlib.sha256(h[:hs]+f.read(n*e)).hexdigest())
EOPY
}
GPT=$(gpthash "$DISK")
JNL="$TMP/j.cpio"; journal "$JNL" AUROSTEST $P2S $((P2E-P2S+1)) "$GPT" esp

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
mkstick

echo
echo "  and then installing, for real"
run "it installs and the new system starts" \
    "aurstage.install aurstage.min_gb=1" "INSTALLTEST-AUROS-STARTED"

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
run "it restores and says so" "aurstage.restore" "verdict=restored"
check_back "from the stick"

echo
echo "  and again with no stick at all, from the copy on the computer"
run "it restores from the copy it left on this computer" \
    "aurstage.restore" "verdict=restored" "$TMP/blank.img"
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
cp --sparse=always "$STICK" "$TMP/badstick.img"
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
run "a damaged saved copy: refused, disk untouched" \
    "aurstage.restore" "aurstage-report v1 verdict=restore" \
    "$TMP/badstick.img" unchanged

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "This is the whole product, on one synthetic machine."
    exit 1
fi
echo "$checked checks: it installs AurOS, starts it, puts Windows back"
echo "again, and every file in Windows is exactly what it was."
exit 0
