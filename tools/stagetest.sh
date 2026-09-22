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
#  Stages A and B are the part of that which can be tested before
#  anything destructive exists: the environment boots, looks at a real
#  disk with a real NTFS filesystem on it, says what it would do to
#  the machine and why it would refuse -- and changes NOT ONE BYTE.
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

# ONE AT A TIME. Two of these running together share the log, fight
# over the CPU, and -- the way I found out -- a stale "clean up the
# old fixtures" command deletes the live one's disk image out from
# under it. The result reads like eleven real failures. A suite whose
# failures cannot be trusted is worse than no suite.
LOCK="${TMPDIR:-/tmp}/stagetest.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "another stagetest is already running ($LOCK)"
    echo "wait for it, or remove that directory if it is stale."
    exit 2
fi
TMP=$(mktemp -d "${TMPDIR:-/tmp}/stagetest.XXXXXX")
trap 'rm -rf "$TMP" "$LOCK"' EXIT

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
# Written down once, because the journal fixtures below have to state
# the same numbers and a partition table that disagrees with the
# journal by a typo would look exactly like the attack the journal is
# there to stop.
# One gibibyte of Windows, not four. The surface test reads every
# sector of the reclaimable space and that is almost the whole cost of
# this suite under emulation; a bigger volume buys nothing, because
# what is being tested is that the read happens and reports honestly,
# not how long a real disk takes.
P1S=2048;    P1E=206847        # ESP, 100 MiB
P2S=206848;  P2E=2303999       # Windows, 1 GiB
P3S=2304000; P3E=6291422       # AurOS, the rest of 3 GiB
SERIAL=AUROSTEST
DISKSECT=6291456               # 3 GiB in 512-byte sectors
truncate -s 3G "$DISK"
sgdisk --zap-all "$DISK" >/dev/null 2>&1
sgdisk -n 1:2048:+100M -t 1:ef00 -c 1:"EFI"     "$DISK" >/dev/null 2>&1
sgdisk -n 2:0:+1G      -t 2:0700 -c 2:"Windows" "$DISK" >/dev/null 2>&1
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
mkpart $P1S $P1E vfat || { echo "  could not make the ESP"; exit 2; }
mkpart $P2S $P2E ntfs || { echo "  could not make the NTFS volume"; exit 2; }
mkpart $P3S $P3E ext4 || { echo "  could not make the root"; exit 2; }

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
L=$(losetup --find --show -o $((P3S*512)) --sizelimit $(((P3E-P3S+1)*512)) "$DISK")
mkdir -p "$TMP/mnt"
mount "$L" "$TMP/mnt" && mkdir -p "$TMP/mnt/sbin" "$TMP/mnt/proc" "$TMP/mnt/sys" \
                                  "$TMP/mnt/dev" "$TMP/mnt/run" &&
    cp "$TMP/fakeinit" "$TMP/mnt/sbin/init" && sync && umount "$TMP/mnt"
losetup -d "$L"

run_case() { # name  kernel-args  expect-in-output  [prepare-fn]  [extra-cpio]
    cp --sparse=always "$DISK" "$TMP/run.img"
    # A hook that breaks the disk on purpose BEFORE the boot, so that
    # every refusal below is a refusal about a real volume in a real
    # state rather than a string we arranged to see.
    [ $# -ge 4 ] && [ -n "$4" ] && "$4" "$TMP/run.img"
    IRD=out/auros-staging.img
    if [ $# -ge 5 ] && [ -n "$5" ]; then
        # The kernel unpacks concatenated initramfs archives in order,
        # so a second one can drop a file into the first without the
        # image being rebuilt for every case.
        cat out/auros-staging.img "$5" > "$TMP/initrd.img"; IRD="$TMP/initrd.img"
    fi
    before=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    : > "$TMP/out.txt"
    # BOOTED THROUGH OVMF, NOT SeaBIOS.
    #
    # Stage B refuses a machine that has no EFI variables, because the
    # one-restart design is built on BootNext and a machine with no
    # BootNext has no one-shot self-reverting boot to fail safely
    # into. A BIOS-booted test machine is therefore not a machine this
    # product supports, and testing on one tests a path no customer is
    # on. QEMU's direct kernel boot works through OVMF, so the only
    # cost is a copy of the variable store per case.
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$TMP/vars.fd" 2>/dev/null
    qemu-system-x86_64 -machine q35,accel=tcg -m 1024 -smp 2 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,unit=1,file="$TMP/vars.fd" \
        -no-reboot -kernel out/auros-staging-vmlinuz \
        -initrd "$IRD" -append "console=ttyS0 $2" \
        -drive file="$TMP/run.img",format=raw,if=none,id=d0 \
        -device virtio-blk-pci,drive=d0,serial=$SERIAL \
        -display none -serial stdio > "$TMP/out.txt" 2>&1 &
    qp=$!
    # WATCH FOR THE ANSWER RATHER THAN WAITING OUT THE CLOCK.
    #
    # Half the cases here end in stop_here(), which is a deliberate
    # forever-pause: the staging environment must never panic and must
    # never reboot-loop, so when it gives up it sits there with its
    # message on the screen. Waiting for the timeout on each of those
    # was twenty minutes of the suite's runtime spent watching a
    # machine do nothing on purpose.
    #
    # Ten seconds are still given after the answer appears, so a case
    # that DOES power itself off is still seen to do it -- that is the
    # evidence the dry run reboots back to Windows, and it should not
    # be thrown away to save ten seconds.
    #
    # Stopping QEMU early cannot hide a write: the guest's writes have
    # already reached the host's page cache, which is what md5sum
    # reads, so an unchanged hash after an early kill means the same
    # thing it meant before.
    seen=0; i=0
    # 240 seconds, not ten minutes. The slowest case that PASSES is the
    # full dry run at about a minute; the deadline only ever bounds a
    # case that is going to fail, and four minutes of watching a failed
    # case is four minutes nobody learns anything in.
    while [ "$i" -lt 240 ]; do
        kill -0 "$qp" 2>/dev/null || break
        if [ "$seen" -eq 0 ] && grep -aq "$3" "$TMP/out.txt" 2>/dev/null; then
            seen=1; i=230
        fi
        sleep 1; i=$((i + 1))
    done
    kill -9 "$qp" 2>/dev/null
    wait "$qp" 2>/dev/null
    after=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    if grep -aq "$3" "$TMP/out.txt"; then ok "$1"
    else bad "$1" "expected to see: $3"; tail -4 "$TMP/out.txt" | sed 's/^/        /'; fi
    # THE CHECK THIS FILE EXISTS FOR.
    if [ "$before" = "$after" ]; then
        ok "...and the disk is byte-for-byte unchanged"
    else
        bad "...and the disk is byte-for-byte unchanged" \
            "stage B wrote to the disk. Nothing in it may."
    fi
}

# The hash the journal carries, computed the way aurstage.h defines it
# -- by a different implementation, in a different language, from the
# same written-down rule. That is the whole value of it: AurBridge
# will be a third implementation on the other side of a restart, and
# if the definition is not precise enough for two to agree, it is not
# precise enough to ship.
gpthash() { python3 - "$1" <<'EOPY'
import sys, struct, hashlib
ss = 512
f = open(sys.argv[1], 'rb')
f.seek(ss); h = f.read(ss)
assert h[0:8] == b'EFI PART', "no GPT on the fixture"
hsize = struct.unpack_from('<I', h, 12)[0]
plba  = struct.unpack_from('<Q', h, 72)[0]
num   = struct.unpack_from('<I', h, 80)[0]
esz   = struct.unpack_from('<I', h, 84)[0]
f.seek(plba * ss)
print(hashlib.sha256(h[:hsize] + f.read(num * esz)).hexdigest())
EOPY
}

# A journal in its own little initramfs, dropped in beside the image.
journal() { # file  serial  start-lba  sectors  age-seconds  [gpt-hash]
    mkdir -p "$TMP/j/aurbridge"
    J="$TMP/j/aurbridge/journal.json"
    printf '{"disk_serial":"%s","disk_model":"QEMU HARDDISK",' "$2" > "$J"
    printf '"disk_bytes":3221225472,"logical_sector":512,' >> "$J"
    printf '"win_part":"2","win_start_lba":%s,"win_sectors":%s,' "$3" "$4" >> "$J"
    printf '"win_ntfs_serial":0,"gpt_sha256":"%s","stage":"armed",' "${6:-}" >> "$J"
    printf '"written_unix":%s}\n' "$(( $(date +%s) - $5 ))" >> "$J"
    # GZIPPED, LIKE THE IMAGE IT IS APPENDED TO.
    #
    # The kernel unpacks a sequence of initramfs archives, and it
    # decompresses each one it recognises. A plain cpio appended after
    # a gzipped one did not arrive -- every journal case failed with
    # "No installer record was found", which looks exactly like a
    # parser bug and is not one. Compressing this the same way the
    # image is compressed makes the two halves the same kind of thing.
    ( cd "$TMP/j" && find . -print0 | cpio --null -o --format=newc --quiet ) \
        | gzip -9 > "$1"
    rm -rf "$TMP/j"
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

# ═══ stage B ═══════════════════════════════════════════════════════
#
# Everything above proves the environment comes up and leaves the disk
# alone. Below is what it is FOR: looking at a real volume and saying
# whether this machine could be converted -- and saying no, with a
# reason a person can act on, when it could not.
#
# docs/AURBRIDGE.md calls the dry run "the first shippable artifact"
# and says it is worth shipping on its own to build a hardware matrix
# before anyone's disk is at risk. These are the cases that decide
# whether it is honest enough to ship.

echo
echo "  the dry run, all the way through"
# The whole chain: journal, NTFS state, ntfsresize --info for the real
# floor, and every sector of the reclaimable space read back. On a
# two-gigabyte volume under emulation this is the slowest case here,
# and that is the point -- it is doing the reading.
# aurstage.min_gb=1 because this fixture's Windows volume is a
# gibibyte: the real floor is twenty-four, and a twenty-five gigabyte
# fixture would be a twenty-five gigabyte surface test under emulation
# for no extra confidence. The floor itself gets its own case below.
run_case "a healthy machine: it says it could be converted" \
         "aurstage.dry aurstage.min_gb=1" \
         "This computer could be converted"
# And the floor, at its shipped value, on the same machine.
run_case "too small to be worth converting: refused" "aurstage.dry" \
         "not enough room"

echo
echo "  and the states a resize must never start from"
bitlocker() { printf '\055FVE-FS\055' | \
              dd of="$1" bs=1 seek=$((P2S*512+3)) conv=notrunc status=none; }
run_case "BitLocker: refused, and told how to turn it off" "aurstage.dry" \
         "encrypted with BitLocker" bitlocker

# Somebody else's encryption, by name, where the product writes it.
# docs/AURBRIDGE.md makes this an unconditional abort: under a
# sector-level filter driver we do not control, the bytes we read are
# not the bytes Windows sees.
#
# LBA 100 -- in the gap between the GPT and the first partition, which
# is where boot code lives and where nothing legitimate is. The first
# version of this wrote at byte 16384, which is inside the GPT's
# partition entry array: it destroyed the partition table, the kernel
# found no partitions, and the case failed with "AurOS cannot see the
# drive on this computer yet" instead of the refusal it was testing.
# A fixture that breaks something other than the thing under test
# tests that other thing.
veracrypt() { printf 'VeraCrypt Boot Loader' | \
              dd of="$1" bs=1 seek=51200 conv=notrunc status=none; }
run_case "somebody else's encryption: refused by name" "aurstage.dry" \
         "protected by VeraCrypt" veracrypt

notntfs() { dd if=/dev/zero of="$1" bs=512 count=1 seek=$P2S \
               conv=notrunc status=none; }
run_case "nothing that looks like Windows: it says so" "aurstage.dry" \
         "no Windows filesystem found" notntfs

echo
echo "  is this still the machine the installer was prepared for?"
# BootNext is one-shot and self-reverting, which is what makes a
# failed first boot a non-event. What it is not is immediate: a
# restart can be cancelled or blocked for days, and Windows updates,
# defragments and grows its pagefile in the meantime. Every number the
# shrink would be given was measured before that.
GPT=$(gpthash "$DISK")
JM="$TMP/jm.cpio"; journal "$JM" "$SERIAL" $P2S $((P2E-P2S+1)) 60 "$GPT"
run_case "the right machine: it recognises it" "aurstage.dry aurstage.min_gb=1" \
         "the computer the installer was prepared for" "" "$JM"

JW="$TMP/jw.cpio"; journal "$JW" "SOMEOTHERDISK" $P2S $((P2E-P2S+1)) 60
run_case "another machine's record: refused" "aurstage.dry" \
         "not the disk the installer was prepared for" "" "$JW"

JV="$TMP/jv.cpio"; journal "$JV" "$SERIAL" $((P2S + 4096)) $((P2E-P2S+1)) 60
run_case "Windows has moved since: refused" "aurstage.dry" \
         "not where it was" "" "$JV"

JR="$TMP/jr.cpio"; journal "$JR" "$SERIAL" $P2S $((P2E-P2S-4096)) 60
run_case "Windows is a different size since: refused" "aurstage.dry" \
         "not the size it was" "" "$JR"

JS="$TMP/js.cpio"; journal "$JS" "$SERIAL" $P2S $((P2E-P2S+1)) 345600 "$GPT"
run_case "armed four days ago: refused as stale" "aurstage.dry aurstage.min_gb=1" \
         "more than three days ago" "" "$JS"

# The narrow checks above all pass on this one: right disk, right
# start, right length, written a minute ago. Only the hash of the
# whole table disagrees -- which is what a partition added after
# Windows, or a recovery tool having rewritten the table, looks like.
JT="$TMP/jt.cpio"
journal "$JT" "$SERIAL" $P2S $((P2E-P2S+1)) 60 \
        "0000000000000000000000000000000000000000000000000000000000000000"
run_case "the disk is divided up differently now: refused" "aurstage.dry" \
         "divided up has changed" "" "$JT"

echo
echo "  and when it cannot see a disk at all"
# This one was an accident. An earlier fixture wrote its marker into
# the GPT's partition entry array by mistake, the kernel found no
# partitions, and the environment took the no-disk path -- which
# nothing had ever exercised. It printed the PCI storage controllers
# it could see and stopped without panicking, which is exactly what
# that path is for, so the accident is now a test.
#
# The message deliberately does NOT tell her to change a BIOS setting.
# The commonest cause is Intel RST, and switching that to AHCI stops
# Windows booting until somebody does the safe-mode dance afterwards.
# BOTH copies of the table. Zeroing only the primary proves nothing:
# Linux falls back to the backup GPT at the end of the disk and finds
# the partitions anyway, and the case would pass for the wrong reason
# or fail for a confusing one.
nodisk() { dd if=/dev/zero of="$1" bs=1M count=1 conv=notrunc status=none
           dd if=/dev/zero of="$1" bs=512 seek=$((DISKSECT - 40)) count=40 \
              conv=notrunc status=none; }
run_case "no disk it can see: it says what hardware is there" \
         "aurstage.dry" "what this computer has instead" nodisk

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "The staging environment is where the one restart lands. If it"
    echo "writes to the disk before it is supposed to, there is no way back."
    exit 1
fi
echo "$checked checks: the staging environment comes up, finds the disk,"
echo "hands over in the same boot, says what it would and would not do"
echo "to this machine, and changes nothing."
exit 0
