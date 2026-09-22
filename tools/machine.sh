# ═══════════════════════════════════════════════════════════════════
#  machine.sh — the synthetic machine every end-to-end test runs on
#
#  Sourced, never executed. It builds a computer shaped like the ones
#  this product is for -- an EFI partition, a real NTFS Windows volume
#  with real files in it, and an OEM recovery partition parked at the
#  far end so that the space a shrink creates is a GAP and not room at
#  the end of the disk -- plus the AurOS memory stick that goes with
#  it, and the record AurBridge would have written before the restart.
#
#  It is one file because there is now more than one test that needs
#  the same machine, and two fixtures that are meant to be identical
#  and are maintained separately are two fixtures that eventually are
#  not. The power-cut matrix in particular only means anything if the
#  machine it interrupts is the same machine the ordinary install test
#  says works.
#
#  The caller sets MTMP to a scratch directory and then calls, in
#  order: mach_need, mach_disk, mach_image, mach_stick, mach_journal.
#  Everything is exported in shell variables named at the top of each
#  function.
# ═══════════════════════════════════════════════════════════════════

RFS="${RFS:-work/forge/desktop/rootfs}"

# The machine's layout, in 512-byte sectors. WinRE sits at the very
# end on purpose: on the OEM layout docs/AURBRIDGE.md calls typical,
# the space a shrink creates is between C: and the recovery partition,
# and a planner that computes from the end of the disk gets every one
# of those machines wrong.
P1S=2048;    P1E=206847       # ESP, 100 MiB
P2S=206848;  P2E=2303999      # Windows, 1 GiB
P3S=5500000; P3E=6291422      # WinRE, at the very end

mach_need() {
    for t in qemu-system-x86_64 sgdisk mkfs.ext4 cpio python3; do
        command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
    done
    [ -d "$RFS" ] || { echo "no rootfs at $RFS"; exit 2; }
    [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] || { echo "need OVMF"; exit 2; }
    LD=$(ls "$RFS"/lib64/ld-linux-x86-64.so.2 2>/dev/null || \
         ls "$RFS"/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 2>/dev/null)
    LP="$RFS/lib/x86_64-linux-gnu:$RFS/lib64:$RFS/usr/lib/x86_64-linux-gnu"
}

# ntfs-3g and mkntfs out of the forged rootfs rather than the build
# host's, so the test uses the same versions the product ships.
nt() { p="$RFS/usr/sbin/$1"; [ -x "$p" ] || p="$RFS/usr/bin/$1"; shift
       "$LD" --library-path "$LP" "$p" "$@"; }

# → DISK, WINFILES, ESPMD5, $MTMP/win.before
mach_disk() {
    DISK="$MTMP/disk.img"
    truncate -s 3G "$DISK"
    sgdisk --zap-all "$DISK" >/dev/null 2>&1
    sgdisk -n 1:$P1S:$P1E -t 1:ef00 -c 1:"EFI"     "$DISK" >/dev/null 2>&1
    sgdisk -n 2:$P2S:$P2E -t 2:0700 -c 2:"Windows" "$DISK" >/dev/null 2>&1
    sgdisk -n 3:$P3S:$P3E -t 3:2700 -c 3:"WinRE"   "$DISK" >/dev/null 2>&1

    _mkpart() {
        off=$(( $1 * 512 )); len=$(( ($2 - $1 + 1) * 512 ))
        l=$(losetup --find --show -o "$off" --sizelimit "$len" "$DISK") || return 1
        case "$3" in
          ntfs) nt mkntfs -Q -F -L WINDOWS "$l" >/dev/null 2>&1 ;;
          vfat) mkfs.vfat -n EFI "$l" >/dev/null 2>&1 ;;
          ext4) mkfs.ext4 -q -L WINRE "$l" >/dev/null 2>&1 ;;
        esac
        rc=$?; losetup -d "$l"; return $rc
    }
    _mkpart $P1S $P1E vfat || { echo "  no ESP"; exit 2; }
    _mkpart $P2S $P2E ntfs || { echo "  no NTFS"; exit 2; }
    _mkpart $P3S $P3E ext4 || { echo "  no WinRE"; exit 2; }

    # Real files in Windows, so "intact" means something.
    L=$(losetup --find --show -o $((P2S*512)) \
        --sizelimit $(((P2E-P2S+1)*512)) "$DISK")
    mkdir -p "$MTMP/m"
    if nt ntfs-3g "$L" "$MTMP/m" >/dev/null 2>&1; then
        mkdir -p "$MTMP/m/Users/auros/Pictures"
        for i in 1 2 3 4 5 6; do
            dd if=/dev/urandom of="$MTMP/m/Users/auros/Pictures/p$i.jpg" \
               bs=64k count=1 status=none
        done
        dd if=/dev/urandom of="$MTMP/m/Users/auros/thesis.odt" \
           bs=1M count=2 status=none
        ( cd "$MTMP/m" && find . -type f -exec md5sum {} \; | sort ) \
            > "$MTMP/win.before"
        sync; umount "$MTMP/m"
    else
        echo "  cannot write to the NTFS volume (no FUSE?)"; losetup -d "$L"
        exit 2
    fi
    losetup -d "$L"
    WINFILES=$(wc -l < "$MTMP/win.before")
    # AND THERE HAD BETTER BE SOME.
    #
    # Every "is every file in Windows still exactly what it was" check
    # in every one of these tests is `cmp -s` of this file against a
    # later one. If the dd's above failed -- no room, a degraded fuse
    # mount, a cd that did not happen -- win.before is empty, both
    # sides are empty, and the single property the whole suite exists
    # to prove passes vacuously. The only clue would be the word "0" in
    # a banner line.
    [ "$WINFILES" -ge 7 ] || {
        echo "  only $WINFILES files went into Windows; the fixture is broken"
        exit 2
    }
    # What the EFI partition held before any of this. The restore has to
    # put these bytes back or Windows does not start, and nothing else
    # would notice if it put back something almost right.
    ESPMD5=$(dd if="$DISK" bs=512 skip=$P1S count=$((P1E-P1S+1)) status=none \
             | md5sum | cut -d' ' -f1)
    NTFSTOT=$(ntfs_total "$DISK" $P2S | cut -d' ' -f1)
    NTFSSPC=$(ntfs_total "$DISK" $P2S | cut -d' ' -f2)
    [ -n "$NTFSTOT" ] && [ "$NTFSTOT" -gt 0 ] || {
        echo "  the NTFS volume does not state a size; the fixture is broken"
        exit 2
    }
}

# THE FILESYSTEM'S OWN SIZE, not the partition entry's.
#
# Checking only the partition table let a restore pass while the NTFS
# inside was still its shrunken size -- Windows would start and show a
# smaller C: than it had, which is exactly the thing "put Windows back"
# promises not to do. total_sectors lives at offset 0x28 of the boot
# sector and sectors_per_cluster at 0x0d; the second is the tolerance,
# because mkntfs sets total_sectors to one less than the partition,
# which is not a multiple of the cluster size, and ntfsresize can only
# land on (floor(size/cluster) - 1) x sectors_per_cluster. The original
# number is not reachable by any resize at all.
ntfs_total() { # image  first_sector  ->  "total_sectors sectors_per_cluster"
    python3 - "$1" "$2" <<'EOPY'
import sys, struct
f = open(sys.argv[1], 'rb'); f.seek(int(sys.argv[2]) * 512)
b = f.read(512); f.close()
print(struct.unpack_from('<Q', b, 0x28)[0], b[0x0d])
EOPY
}

# → AIMG, ROFF, RLEN
mach_image() {
    AIMG="$MTMP/auros.img"
    truncate -s 320M "$AIMG"
    sgdisk --zap-all "$AIMG" >/dev/null 2>&1
    sgdisk -n 1:2048:+16M -t 1:ef00 -c 1:"AUROS-ESP"  "$AIMG" >/dev/null 2>&1
    sgdisk -n 2:0:0       -t 2:8304 -c 2:"AUROS-ROOT" "$AIMG" >/dev/null 2>&1
    RS=$(sgdisk -i 2 "$AIMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    RE=$(sgdisk -i 2 "$AIMG" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
    ROFF=$((RS*512)); RLEN=$(((RE-RS+1)*512))

    cat > "$MTMP/init.c" <<'EOC'
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
int main(void){ puts("\nINSTALLTEST-AUROS-STARTED"); fflush(stdout);
                sync(); sleep(2); reboot(RB_POWER_OFF); for(;;) pause(); }
EOC
    cc -static -O2 -o "$MTMP/init" "$MTMP/init.c" 2>/dev/null \
        || { echo "  no cc"; exit 2; }

    dd if=/dev/zero of="$MTMP/root.img" bs=1M count=$((RLEN/1048576)) status=none
    mkfs.ext4 -q -L AUROS-ROOT "$MTMP/root.img"
    # A REAL MODULE TREE. Phase 7 mounts this filesystem and loads ITS
    # drivers, and an image with an empty /lib/modules is correctly
    # reported as our fault rather than the machine's.
    KVER=$(ls work/staging/lib/modules 2>/dev/null | head -1)
    [ -n "$KVER" ] || { echo "  no built module tree to put in the image"; exit 2; }
    mount -o loop "$MTMP/root.img" "$MTMP/m" && {
        mkdir -p "$MTMP/m/sbin" "$MTMP/m/proc" "$MTMP/m/sys" "$MTMP/m/dev" \
                 "$MTMP/m/run" "$MTMP/m/lib/modules"
        cp "$MTMP/init" "$MTMP/m/sbin/init"
        cp -a "work/staging/lib/modules/$KVER" "$MTMP/m/lib/modules/" 2>/dev/null
        sync; umount "$MTMP/m"
    }
    dd if="$MTMP/root.img" of="$AIMG" bs=1M seek=$((ROFF/1048576)) \
       conv=notrunc status=none
}

# → STICK.  mach_stick [corrupt]
mach_stick() {
    STICK="$MTMP/stick.img"
    rm -f "$STICK"; truncate -s 768M "$STICK"
    sgdisk --zap-all "$STICK" >/dev/null 2>&1
    sgdisk -n 1:2048:+400M -t 1:A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41 \
           -c 1:"AUROS-IMAGE" "$STICK" >/dev/null 2>&1
    sgdisk -n 2:0:+4M      -t 2:7E1C3B90-4D2A-4F16-8B77-2C6E5A9D0E33 \
           -c 2:"AUROS-RECORD" "$STICK" >/dev/null 2>&1
    # Room for a copy of this machine's Windows startup. Dominated by
    # the EFI partition, which is 100 MiB here and a gigabyte on some
    # OEM laptops -- AurBridge sizes this from the machine it looked at.
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

# The hash of a disk's partition table, computed the way aurstage.h
# defines it -- independently, in another language, which is the only
# way the definition is worth anything.
mach_gpthash() { python3 - "$1" <<'EOPY'
import sys, struct, hashlib
ss=512; f=open(sys.argv[1],'rb'); f.seek(ss); h=f.read(ss)
hs=struct.unpack_from('<I',h,12)[0]; pl=struct.unpack_from('<Q',h,72)[0]
n=struct.unpack_from('<I',h,80)[0]; e=struct.unpack_from('<I',h,84)[0]
f.seek(pl*ss); print(hashlib.sha256(h[:hs]+f.read(n*e)).hexdigest())
EOPY
}

# → JNL, the cpio AurBridge would have left in the initramfs
mach_journal() {
    GPT=$(mach_gpthash "$DISK")
    JNL="$MTMP/j.cpio"
    mkdir -p "$MTMP/j/aurbridge"
    J="$MTMP/j/aurbridge/journal.json"
    printf '{"disk_serial":"%s","disk_model":"QEMU",' AUROSTEST > "$J"
    printf '"disk_bytes":3221225472,"logical_sector":512,' >> "$J"
    printf '"win_part":"2","win_start_lba":%s,"win_sectors":%s,' \
           "$P2S" "$((P2E-P2S+1))" >> "$J"
    printf '"win_ntfs_serial":0,"gpt_sha256":"%s","stage":"armed",' "$GPT" >> "$J"
    printf '"boot_from":"esp","run_id":%s,' "$(date +%s)" >> "$J"
    printf '"written_unix":%s}\n' "$(date +%s)" >> "$J"
    ( cd "$MTMP/j" && find . -print0 | cpio --null -o --format=newc --quiet ) \
        | gzip -9 > "$JNL"
    rm -rf "$MTMP/j"
}

# Boot the staging environment on a copy of a disk and a copy of a
# stick, and wait for a marker on the serial console.
#
#   mach_boot IMAGE DISKSRC STICKSRC KARGS MARKER [TIMEOUT]
#
# leaves the machine afterwards in $MTMP/run.img and the console in
# $MTMP/out.txt, and returns 0 if the marker appeared.
mach_boot() {
    _img=$1; _disk=$2; _stick=$3; _kargs=$4; _marker=$5; _to=${6:-900}
    cp --sparse=always "$_disk"  "$MTMP/run.img"
    cp --sparse=always "$_stick" "$MTMP/stk.img"
    cat "$_img" "$JNL" > "$MTMP/initrd.img"
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$MTMP/vars.fd"
    : > "$MTMP/out.txt"
    _kern="${_img%.img}-vmlinuz"
    qemu-system-x86_64 -machine q35,accel=tcg -m 1536 -smp 2 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,unit=1,file="$MTMP/vars.fd" \
        -no-reboot -kernel "$_kern" \
        -initrd "$MTMP/initrd.img" -append "console=ttyS0 $_kargs" \
        -drive file="$MTMP/run.img",format=raw,if=none,id=d0 \
        -device virtio-blk-pci,drive=d0,serial=AUROSTEST \
        -drive file="$MTMP/stk.img",format=raw,if=none,id=d1 \
        -device virtio-blk-pci,drive=d1,serial=AUROSSTICK \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -display none -serial stdio > "$MTMP/out.txt" 2>&1 &
    _qp=$!
    _seen=0; _i=0
    while [ "$_i" -lt "$_to" ]; do
        kill -0 "$_qp" 2>/dev/null || break
        if [ "$_seen" -eq 0 ] && grep -aq "$_marker" "$MTMP/out.txt" 2>/dev/null; then
            _seen=1; _i=$(( _to - 20 ))
        fi
        sleep 1; _i=$((_i+1))
    done
    kill -9 "$_qp" 2>/dev/null; wait "$_qp" 2>/dev/null
    grep -aq "$_marker" "$MTMP/out.txt"
}
