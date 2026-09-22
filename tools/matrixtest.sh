#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  matrixtest — the hardware we cannot buy, built out of QEMU
#
#  This product will meet ten-year-old laptops with firmware nobody
#  has a copy of and disk layouts no current tool produces. We have
#  one machine. So the machines are synthesised: the geometries and
#  layouts that the risk register says break installers, each one
#  built, booted, and asked the one question the dry run exists to
#  answer -- WOULD YOU TOUCH THIS COMPUTER?
#
#  Every row here is a real failure mode from docs/research/red-team.md
#  or from a bug this tree has already had:
#
#    4Kn            /sys reports partition starts in 512-byte units on
#                   every disk, including one whose blocks are 4096.
#                   Confusing the two makes every partition eight
#                   times too small. It is the single most expensive
#                   arithmetic mistake available here.
#    a 1 GiB ESP    OEM EFI partitions are not 100 MiB. The saved copy
#                   of the startup is sized from this, and a constant
#                   would be wrong on most real laptops.
#    LBA 34         Disks partitioned before 2010 do not start the
#                   first partition at 1 MiB. A recovery tool that
#                   restores "the first 2048 sectors" destroys them.
#    MBR, BIOS      Unconditional refusals. The one-restart design
#                   rests on an EFI variable; a machine with none has
#                   no safe way to fail.
#    BitLocker      THE MUST NOT.
#    hibernated     A session is still in there; resizing loses it.
#    two Windows    Every OEM laptop has a WinRE partition, so "more
#                   than one NTFS" is the norm, not a corner case.
#
#  It uses the DRY RUN, which writes nothing, so the whole matrix can
#  be run on a laptop without risk and without a spare disk.
#
#    sudo sh tools/matrixtest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
RFS="${RFS:-work/forge/desktop/rootfs}"

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-52s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-52s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

for t in qemu-system-x86_64 sgdisk python3 losetup; do
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

LOCK="${TMPDIR:-/tmp}/matrixtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another matrixtest is running"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/matrix.XXXXXX")
trap 'mountpoint -q "$T/m" 2>/dev/null && umount "$T/m"; rm -rf "$T" "$LOCK"' EXIT
mkdir -p "$T/m"

echo
echo "Which computers would AurOS touch, and which would it refuse?"
echo

# ── building a machine ──────────────────────────────────────────────
#
# SECTOR SIZE IS A PROPERTY OF THE LOOP DEVICE, not of the file. sgdisk
# on a plain file always writes a table in 512-byte LBAs, so a 4Kn
# machine cannot be made by partitioning the file directly -- it has to
# go through a loop device opened with --sector-size 4096, which is
# also the only way to find out whether our own reader gets it right.
build() { # out  sector  esp_start  esp_mib  win_mib  extra
    _o=$1; _ss=$2; _es=$3; _emib=$4; _wmib=$5; _extra=${6:-}
    rm -f "$_o"; truncate -s 4G "$_o"
    _l=$(losetup --find --show --sector-size "$_ss" "$_o") || return 1
    sgdisk --zap-all "$_l" >/dev/null 2>&1
    _eb=$(( _emib * 1024 * 1024 / _ss ))
    _wb=$(( _wmib * 1024 * 1024 / _ss ))
    _ws=$(( _es + _eb ))
    sgdisk -n 1:$_es:$(( _ws - 1 ))            -t 1:ef00 -c 1:"EFI" \
           "$_l" >/dev/null 2>&1 || { losetup -d "$_l"; return 1; }
    sgdisk -n 2:$_ws:$(( _ws + _wb - 1 ))      -t 2:0700 -c 2:"Windows" \
           "$_l" >/dev/null 2>&1 || { losetup -d "$_l"; return 1; }
    case "$_extra" in
      winre) sgdisk -n 3:$(( _ws + _wb + 2048 )):+200M -t 3:2700 \
                    -c 3:"WinRE" "$_l" >/dev/null 2>&1 ;;
    esac
    losetup -d "$_l"
    # The filesystems, by offset into the file.
    _eoff=$(( _es * _ss )); _elen=$(( _eb * _ss ))
    _woff=$(( _ws * _ss )); _wlen=$(( _wb * _ss ))
    _rc=0
    _l=$(losetup --find --show -o "$_eoff" --sizelimit "$_elen" "$_o")
    mkfs.vfat -n EFI "$_l" >/dev/null 2>&1 || _rc=1
    losetup -d "$_l"
    _l=$(losetup --find --show -o "$_woff" --sizelimit "$_wlen" \
         --sector-size "$_ss" "$_o")
    nt mkntfs -Q -F -L WINDOWS "$_l" >/dev/null 2>&1 || _rc=1
    losetup -d "$_l"
    # THE RETURN VALUE IS THE FILESYSTEMS', not just the table's. This
    # used to `return 0` unconditionally, so `build ... || exit 2` could
    # not catch a failed mkntfs and two rows below ran against a disk
    # with no filesystem on it -- passing, for the wrong reason.
    WOFF=$_woff; WLEN=$_wlen
    return $_rc
}

# Boot the staging environment on a machine and return its report line.
# `$1` is the image, `$2` extra -device properties, `$3` bios|uefi.
look() { # image  blockprops  firmware
    _img=$1; _props=$2; _fw=$3
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$T/vars.fd"
    : > "$T/out.txt"
    # THE ARGUMENTS ARE COPIED OUT FIRST, and the firmware flags go in
    # a file rather than into "$@".
    #
    # This used to build the pflash arguments with `set --`, which
    # REPLACES the function's positional parameters -- so $1 became
    # "-drive" and QEMU was handed `-drive file=-drive`. Every row in
    # this file failed with "no report line at all", which is also what
    # a machine with no QEMU prints, so the whole matrix read as an
    # environment problem and had never verified anything at all.
    : > "$T/fw.args"
    if [ "$_fw" = uefi ]; then
        printf '%s\n' \
            "-drive" \
            "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
            "-drive" \
            "if=pflash,format=raw,unit=1,file=$T/vars.fd" > "$T/fw.args"
    fi
    _fwargs=""
    while IFS= read -r a; do _fwargs="$_fwargs $a"; done < "$T/fw.args"
    # shellcheck disable=SC2086
    qemu-system-x86_64 -machine q35,accel=tcg -m 1024 -smp 2 $_fwargs \
        -no-reboot -kernel out/auros-staging-vmlinuz \
        -initrd out/auros-staging.img -append "console=ttyS0 aurstage.dry aurstage.min_gb=1" \
        -drive file="$_img",format=raw,if=none,id=d0 \
        -device "virtio-blk-pci,drive=d0,serial=AUROSTEST$_props" \
        -display none -serial stdio > "$T/out.txt" 2>&1 &
    _qp=$!
    _i=0
    while [ "$_i" -lt 420 ]; do
        kill -0 "$_qp" 2>/dev/null || break
        grep -aq 'aurstage-report v1' "$T/out.txt" 2>/dev/null && { _i=415; }
        sleep 1; _i=$((_i+1))
    done
    kill -9 "$_qp" 2>/dev/null; wait "$_qp" 2>/dev/null
    sed -n 's/.*\(aurstage-report v1 .*\)/\1/p' "$T/out.txt" | tail -1
}

row() { # label  want-verdict  image  blockprops  firmware
    _line=$(look "$3" "$4" "$5")
    _v=$(printf '%s' "$_line" | sed -n 's/.*verdict=\([^ ]*\).*/\1/p')
    if [ "$_v" = "$2" ]; then ok "$1"
    else bad "$1" "wanted verdict=$2, got: ${_line:-no report line at all}"
    fi
}

# ── the rows ────────────────────────────────────────────────────────
echo "  ordinary machines"
build "$T/base.img" 512 2048 100 1024 winre || { echo "  cannot build"; exit 2; }
row "512-byte blocks, 100 MiB EFI, WinRE at the end" could-convert \
    "$T/base.img" "" uefi

build "$T/k4.img" 4096 256 100 1024 winre || { echo "  cannot build 4Kn"; exit 2; }
row "4Kn: blocks are 4096 bytes, not 512" could-convert \
    "$T/k4.img" ",logical_block_size=4096,physical_block_size=4096" uefi

build "$T/bigesp.img" 512 2048 1024 1024 winre || { echo "  cannot build"; exit 2; }
row "an OEM 1 GiB EFI partition" could-convert "$T/bigesp.img" "" uefi

build "$T/old.img" 512 34 100 1024 winre || { echo "  cannot build"; exit 2; }
row "the first partition at LBA 34, as disks were made before 2010" \
    could-convert "$T/old.img" "" uefi

echo
echo "  machines it must refuse"
# BOTH FIXTURES ARE REBUILT, NOT COPIED, and the offsets come back from
# the build that made them.
#
# They used to be copies of base.img patched at $WOFF -- a global left
# over from the LAST build() call, which by then was old.img with its
# first partition at LBA 34. So the BitLocker signature went into the
# middle of base.img's EFI partition, the hibernation fixture's loop
# device would not mount, the `if` body never ran, and neither disk was
# corrupted at all. Both rows then ran against a perfectly healthy
# machine, and the obvious way to make them green would have been to
# relax the expectation -- which is how the MUST NOT stops being
# guarded.
build "$T/fve.img" 512 2048 100 1024 winre || { echo "  cannot build"; exit 2; }
python3 - "$T/fve.img" "$WOFF" <<'EOPY'
import sys
f = open(sys.argv[1], 'r+b'); off = int(sys.argv[2])
f.seek(off); head = bytearray(f.read(512))
assert head[3:11] == b'NTFS    ', "the fixture is not where the test thinks"
head[3:11] = b'-FVE-FS-'
f.seek(off); f.write(bytes(head)); f.close()
EOPY
python3 - "$T/fve.img" "$WOFF" <<'EOPY'
import sys
f = open(sys.argv[1], 'rb'); f.seek(int(sys.argv[2]))
assert f.read(512)[3:11] == b'-FVE-FS-', "the BitLocker fixture did not take"
EOPY
row "a BitLocker-encrypted Windows drive" ntfs-refused "$T/fve.img" "" uefi

# A session still in there. hiberfil.sys with the header Windows writes.
build "$T/hib.img" 512 2048 100 1024 winre || { echo "  cannot build"; exit 2; }
L=$(losetup --find --show -o "$WOFF" --sizelimit "$WLEN" "$T/hib.img")
if [ -n "$L" ] && nt ntfs-3g "$L" "$T/m" >/dev/null 2>&1; then
    dd if=/dev/zero of="$T/m/hiberfil.sys" bs=1M count=4 status=none
    printf 'hibr' | dd of="$T/m/hiberfil.sys" bs=1 conv=notrunc status=none
    sync; umount "$T/m"
    hib_made=yes
else
    hib_made=no
fi
[ -n "$L" ] && losetup -d "$L"
# AND THE FIXTURE IS CHECKED BEFORE THE ROW RUNS. A corruption that
# silently did nothing is the whole reason these two moved.
[ "$hib_made" = yes ] || { echo "  could not make the hibernation fixture"; exit 2; }
row "Windows is asleep, not shut down" ntfs-refused "$T/hib.img" "" uefi

# An MBR disk. parted rather than sgdisk, which only speaks GPT.
rm -f "$T/mbr.img"; truncate -s 4G "$T/mbr.img"
parted -s "$T/mbr.img" mklabel msdos mkpart primary ntfs 1MiB 1025MiB \
    >/dev/null 2>&1
L=$(losetup --find --show -o $((1048576)) --sizelimit $((1024*1024*1024)) "$T/mbr.img")
nt mkntfs -Q -F -L WINDOWS "$L" >/dev/null 2>&1; losetup -d "$L"
row "a disk divided up the old way (MBR)" not-gpt "$T/mbr.img" "" uefi

row "a computer that starts up the old way (no EFI)" not-uefi \
    "$T/base.img" "" bios

# Two NTFS volumes and nothing to say which is which. Every OEM laptop
# has a WinRE partition, so this is the normal case, not a strange one
# -- and picking one by readdir() order is how a 500 MB recovery
# partition gets reported as convertible.
build "$T/two.img" 512 2048 100 1024 "" || { echo "  cannot build"; exit 2; }
L=$(losetup --find --show "$T/two.img")
sgdisk -n 3:$((2048 + 204800 + 2097152)):+400M -t 3:0700 -c 3:"Data" \
    "$L" >/dev/null 2>&1
losetup -d "$L"
L=$(losetup --find --show -o $(( (2048 + 204800 + 2097152) * 512 )) \
    --sizelimit $((400*1024*1024)) "$T/two.img")
nt mkntfs -Q -F -L DATA "$L" >/dev/null 2>&1; losetup -d "$L"
row "two Windows drives and nothing saying which" ambiguous-windows \
    "$T/two.img" "" uefi

# A machine with nowhere near enough room.
build "$T/small.img" 512 2048 100 400 winre || { echo "  cannot build"; exit 2; }
row "a computer with no room to spare" no-room "$T/small.img" "" uefi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "Each row is a shape of computer somebody actually owns."
    exit 1
fi
echo "$checked machines: every geometry it should take, it takes, and"
echo "every one it must refuse, it refuses."
exit 0
