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
    for t in qemu-system-x86_64 sgdisk mkfs.ext4 mkfs.vfat cpio python3 blkid; do
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

# → AIMG, ROFF, RLEN, EOFF, ELEN, AUROS_UUID
#
# A WHOLE IMAGE, INCLUDING THE PART THAT STARTS A COMPUTER.
#
# This used to make a 16 MiB EFI partition and put nothing in it,
# which was fine while the installer only ever copied the root extent.
# loader.c copies the image's ESP onto the machine and the firmware
# starts from it, so a fixture with an empty ESP would let the whole
# of that pass while proving nothing. What goes in here is what
# build/mkimage puts in the real one, out of the same rootfs: the
# dual-signed shim, Canonical's signed grub, the fallback CSV, and a
# grub.cfg that finds the root by UUID.
mach_image() {
    AIMG="$MTMP/auros.img"
    truncate -s 384M "$AIMG"
    sgdisk --zap-all "$AIMG" >/dev/null 2>&1
    sgdisk -n 1:2048:+48M -t 1:ef00 -c 1:"AUROS-ESP"  "$AIMG" >/dev/null 2>&1
    sgdisk -n 2:0:0       -t 2:8304 -c 2:"AUROS-ROOT" "$AIMG" >/dev/null 2>&1
    ES=$(sgdisk -i 1 "$AIMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    EE=$(sgdisk -i 1 "$AIMG" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
    RS=$(sgdisk -i 2 "$AIMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    RE=$(sgdisk -i 2 "$AIMG" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
    EOFF=$((ES*512)); ELEN=$(((EE-ES+1)*512))
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
    # The one that says the FIRMWARE started this system -- a different
    # sentence from the one switch_root prints, because the two runs
    # prove different things and a shared marker would let either of
    # them pass for the other.
    sed 's/INSTALLTEST-AUROS-STARTED/AUROS-STARTED-FROM-FIRMWARE/' \
        "$MTMP/init.c" > "$MTMP/finit.c"
    cc -static -O2 -o "$MTMP/finit" "$MTMP/finit.c" 2>/dev/null \
        || { echo "  no cc"; exit 2; }
    # ...packed as an initramfs, so that what grub loads is a kernel
    # and an initrd rather than a kernel and a promise. The image's
    # root has no /etc and no shared libraries; a generic kernel's ext4
    # is a module, so a root= boot would not mount anything.
    mkdir -p "$MTMP/fi"; cp "$MTMP/finit" "$MTMP/fi/init"
    ( cd "$MTMP/fi" && find . -print0 \
        | cpio --null -o --format=newc --quiet ) | gzip -9 > "$MTMP/finitrd.img"
    rm -rf "$MTMP/fi"

    dd if=/dev/zero of="$MTMP/root.img" bs=1M count=$((RLEN/1048576)) status=none
    mkfs.ext4 -q -L AUROS-ROOT "$MTMP/root.img"
    AUROS_UUID=$(blkid -o value -s UUID "$MTMP/root.img")
    [ -n "$AUROS_UUID" ] || { echo "  the image root has no UUID"; exit 2; }
    # A REAL MODULE TREE. Phase 7 mounts this filesystem and loads ITS
    # drivers, and an image with an empty /lib/modules is correctly
    # reported as our fault rather than the machine's.
    KVER=$(ls work/staging/lib/modules 2>/dev/null | head -1)
    [ -n "$KVER" ] || { echo "  no built module tree to put in the image"; exit 2; }
    KERN=$(ls out/auros-staging-vmlinuz 2>/dev/null) \
        || { echo "  no kernel to put in the image"; exit 2; }
    mount -o loop "$MTMP/root.img" "$MTMP/m" && {
        mkdir -p "$MTMP/m/sbin" "$MTMP/m/proc" "$MTMP/m/sys" "$MTMP/m/dev" \
                 "$MTMP/m/run" "$MTMP/m/lib/modules" "$MTMP/m/boot/grub"
        cp "$MTMP/init" "$MTMP/m/sbin/init"
        cp -a "work/staging/lib/modules/$KVER" "$MTMP/m/lib/modules/" 2>/dev/null
        cp "$KERN" "$MTMP/m/boot/vmlinuz"
        cp "$MTMP/finitrd.img" "$MTMP/m/boot/initrd.img"
        # The same two-step build/mkimage uses: the ESP's grub.cfg
        # finds the root and hands over to THIS file, which is where
        # the menu actually lives.
        cat > "$MTMP/m/boot/grub/grub.cfg" <<EOG
set default=0
set timeout=1
serial --unit=0 --speed=115200
terminal_input  console serial
terminal_output console serial
menuentry 'AurOS' --id auros {
    linux  /boot/vmlinuz console=ttyS0,115200n8
    initrd /boot/initrd.img
}
EOG
        sync; umount "$MTMP/m"
    }
    dd if="$MTMP/root.img" of="$AIMG" bs=1M seek=$((ROFF/1048576)) \
       conv=notrunc status=none
    mach_image_esp
}

# The image's EFI partition, built the way build/mkimage builds the
# real one and out of the same files.
#
# mtools, NOT mount. The container these tests run in has no vfat in
# its kernel, which is a property of the test machine and not of the
# product -- the machines AurOS installs on never mount this
# filesystem either, because loader.c copies it as bytes. mkfs.vfat
# and mcopy both work on a plain file, so the fixture needs neither a
# loop device nor a mount for it.
mach_image_esp() {
    SHIM=""
    for c in shimx64.efi.dualsigned shimx64.efi.signed.latest shimx64.efi.signed; do
        [ -f "$RFS/usr/lib/shim/$c" ] && { SHIM="$RFS/usr/lib/shim/$c"; break; }
    done
    [ -n "$SHIM" ] || { echo "  no signed shim in $RFS"; exit 2; }
    GRUBEFI="$RFS/usr/lib/grub/x86_64-efi-signed/grubx64.efi.signed"
    [ -f "$GRUBEFI" ] || { echo "  no signed grub in $RFS"; exit 2; }

    E="$MTMP/esp.img"
    rm -f "$E"; truncate -s "$ELEN" "$E"
    mkfs.vfat -F32 -n AUROS-ESP "$E" >/dev/null 2>&1 \
        || { echo "  the image ESP would not format"; exit 2; }

    D="$MTMP/espdir"
    rm -rf "$D"; mkdir -p "$D/EFI/BOOT" "$D/EFI/AurOS" "$D/EFI/ubuntu"
    cp "$SHIM"    "$D/EFI/BOOT/BOOTX64.EFI"
    cp "$GRUBEFI" "$D/EFI/BOOT/grubx64.efi"
    cp "$RFS/usr/lib/shim/mmx64.efi" "$D/EFI/BOOT/mmx64.efi" 2>/dev/null
    cp "$RFS/usr/lib/shim/fbx64.efi" "$D/EFI/BOOT/fbx64.efi" 2>/dev/null
    cp "$SHIM"    "$D/EFI/AurOS/shimx64.efi"
    cp "$GRUBEFI" "$D/EFI/AurOS/grubx64.efi"
    cp "$RFS/usr/lib/shim/mmx64.efi" "$D/EFI/AurOS/mmx64.efi" 2>/dev/null
    printf 'shimx64.efi,AurOS,,AurOS\n' | iconv -f UTF-8 -t UTF-16LE \
        > "$D/EFI/AurOS/BOOTX64.CSV"
    for d in ubuntu AurOS BOOT; do
        cat > "$D/EFI/$d/grub.cfg" <<EOG
search --no-floppy --fs-uuid --set=root $AUROS_UUID
set prefix=(\$root)/boot/grub
configfile (\$root)/boot/grub/grub.cfg
EOG
    done

    export MTOOLS_SKIP_CHECK=1
    ( cd "$D" && find . -type d ! -name . -printf '%P\n' ) | while read -r d; do
        mmd -i "$E" "::/$d" >/dev/null 2>&1
    done
    ( cd "$D" && find . -type f -printf '%P\n' ) | while read -r f; do
        mcopy -i "$E" -o "$D/$f" "::/$f" || { echo "  mcopy failed for $f"; exit 2; }
    done
    # AND IT HAD BETTER BE IN THERE. mcopy inside a `while read` is in
    # a subshell, so its exit does not reach this function; without
    # this the fixture would go on with an empty ESP and the loader
    # test would fail somewhere it cannot explain.
    mdir -i "$E" ::/EFI/AurOS 2>/dev/null | grep -q 'shimx64' \
        || { echo "  nothing was copied into the image ESP"; exit 2; }

    dd if="$E" of="$AIMG" bs=1M seek=$((EOFF/1048576)) conv=notrunc status=none
    rm -rf "$D"
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

# One partition of an image, by the name in its GPT entry.
#   mach_part IMAGE NAME  ->  "number first_lba last_lba unique_guid type_guid"
# Nothing is printed if there is no such partition, which is how a
# caller tells "not there" from "there and wrong".
mach_part() { python3 - "$1" "$2" <<'EOPY'
import sys, struct, uuid
ss = 512
f = open(sys.argv[1], 'rb'); f.seek(ss); h = f.read(ss)
if h[:8] != b'EFI PART': raise SystemExit(0)
pl = struct.unpack_from('<Q', h, 72)[0]
n  = struct.unpack_from('<I', h, 80)[0]
e  = struct.unpack_from('<I', h, 84)[0]
f.seek(pl * ss); arr = f.read(n * e); f.close()
for i in range(n):
    ent = arr[i*e:(i+1)*e]
    if ent[:16] == b'\0' * 16: continue
    if ent[56:128].decode('utf-16-le', 'replace').rstrip('\0') != sys.argv[2]:
        continue
    first, last = struct.unpack_from('<QQ', ent, 32)
    print(i + 1, first, last,
          str(uuid.UUID(bytes_le=ent[16:32])), str(uuid.UUID(bytes_le=ent[0:16])))
    break
EOPY
}

# Start the machine the way its owner would.
#
# NO -kernel AND NO -initrd. Everything else in these tests hands QEMU
# a kernel directly, which is the right thing when what is being tested
# is the staging environment -- and which skips, entirely, the question
# of whether the machine can start anything by itself. This one gives
# the firmware a disk and the NVRAM the installer left in
# $MTMP/vars.fd, and nothing else. There is no memory stick plugged in
# either: a machine that only starts with the stick in it has not been
# installed.
#
#   mach_boot_firmware DISKIMG MARKER [TIMEOUT] [CODE.fd] [VARS.fd] [EXTRA]
#
# leaves the console in $MTMP/fout.txt and returns 0 if the marker
# appeared.
mach_boot_firmware() {
    _fdisk=$1; _fmarker=$2; _fto=${3:-900}
    _fcode=${4:-/usr/share/OVMF/OVMF_CODE_4M.fd}
    _fvars=${5:-$MTMP/vars.fd}
    _fextra=${6:-}
    : > "$MTMP/fout.txt"
    # shellcheck disable=SC2086
    qemu-system-x86_64 -machine q35,accel=tcg -m 1536 -smp 2 $_fextra \
        -drive if=pflash,format=raw,unit=0,readonly=on,file="$_fcode" \
        -drive if=pflash,format=raw,unit=1,file="$_fvars" \
        -no-reboot \
        -drive file="$_fdisk",format=raw,if=none,id=d0 \
        -device virtio-blk-pci,drive=d0,serial=AUROSTEST \
        -display none -serial stdio > "$MTMP/fout.txt" 2>&1 &
    _fqp=$!
    _fseen=0; _fi=0
    while [ "$_fi" -lt "$_fto" ]; do
        kill -0 "$_fqp" 2>/dev/null || break
        if [ "$_fseen" -eq 0 ] && grep -aq "$_fmarker" "$MTMP/fout.txt" 2>/dev/null; then
            _fseen=1; _fi=$(( _fto - 5 ))
        fi
        sleep 1; _fi=$((_fi+1))
    done
    kill -9 "$_fqp" 2>/dev/null; wait "$_fqp" 2>/dev/null
    grep -aq "$_fmarker" "$MTMP/fout.txt"
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
    # The NVRAM this machine starts with. Pristine unless a caller has
    # arranged otherwise -- the loader test starts from one that
    # already holds the entry the Windows half would have written, so
    # that "the installer tidied it away" is a thing a test can see
    # rather than a claim in a comment.
    cp "${MACH_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}" "$MTMP/vars.fd"
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
