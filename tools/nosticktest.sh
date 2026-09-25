#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  nosticktest — the whole install with NO memory stick at all
#
#  installtest.sh proves the product as designed: a stick holds the
#  image, the installer's notes and the first copy of the way back.
#  This proves the no-stick mode, where none of that exists:
#
#    - AurBridge's own phase engine (the simulation, out/aurbridge-sim)
#      runs in no-stick mode against this machine and writes the
#      manifest and the journal. Nothing here writes them by hand.
#    - The image and its manifest go into \AurOS\ on the NTFS Windows
#      volume, where the wizard puts them.
#    - The staging environment boots with ONE disk and nothing else
#      plugged in, reads the image through a read-only mount, keeps the
#      way back in memory until the shrink has made room, writes it
#      there first, installs, and hands over.
#
#  Then: every file in Windows (the AurOS folder included) is what it
#  was; the way back is on the disk; and putting Windows back from it,
#  still with no stick, gives back the machine we started with.
#
#  And the refusals, each with the disk byte-for-byte unchanged -- which
#  is also the proof that the read-only mount of Windows wrote nothing.
#
#    sudo sh tools/nosticktest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-60s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-60s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

. tools/machine.sh
mach_need
[ -f out/auros-staging.img ] || { echo "run ./build/staging first"; exit 2; }
[ -x out/aurbridge-sim ]     || { echo "run ./build/aurbridge first"; exit 2; }

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another end-to-end test is running"; exit 2; }
MTMP=$(mktemp -d "${TMPDIR:-/tmp}/nosticktest.XXXXXX")
[ -n "$MTMP" ] && [ -d "$MTMP" ] || { rmdir "$LOCK"; echo "no scratch dir"; exit 2; }
TMP="$MTMP"
trap 'mountpoint -q "$MTMP/m" 2>/dev/null && umount "$MTMP/m"; rm -rf "$MTMP" "$LOCK"' EXIT

echo
echo "Does it install AurOS with no memory stick, and leave Windows intact?"
echo

mach_disk
mach_image
echo "  a 3 GiB machine: ESP, a 1 GiB Windows with $WINFILES files, WinRE at the end"
echo "  and a $((RLEN/1048576)) MiB AurOS image, which is going to live inside Windows"

# ── AurBridge, in no-stick mode, against this machine ───────────────
SIM="$TMP/sim"
mkdir -p "$SIM/esp" "$TMP/win/AurOS"
: > "$SIM/efivars.txt"
printf '0\t%s\t%s\t512\tAUROSTEST\tQEMU\t0\n' "$DISK" "$(stat -c%s "$DISK")" \
    > "$SIM/disks.txt"
cp "$AIMG" "$TMP/win/AurOS/auros-desktop.img"
if out/aurbridge-sim "$SIM" desktop none "$TMP/win/AurOS/auros-desktop.img" \
       out/auros-staging-vmlinuz out/auros-staging.img \
       > "$TMP/sim.out" 2> "$TMP/sim.err" && grep -q 'verdict=armed' "$TMP/sim.out"; then
    ok "AurBridge (no stick) arms the machine"
else
    bad "AurBridge (no stick) arms the machine" "$(tail -3 "$TMP/sim.err")"
    exit 1
fi
[ -f "$TMP/win/AurOS/auros-desktop.img.manifest" ] \
    && ok "...and wrote the image's manifest beside it" \
    || bad "...and wrote the image's manifest beside it"
if grep -q 'stick' "$SIM/ran.log" 2>/dev/null; then :; fi
INITRD="$SIM/esp/EFI/AurOS/staging.img"
if gzip -dc "$INITRD" 2>/dev/null | cpio -i --to-stdout aurbridge/journal.json 2>/dev/null \
       | grep -q '"image_on":"windows"'; then
    ok "...and a journal that says the image is on Windows"
else
    # The journal is the LAST segment; cpio on the concatenation reads
    # only the first. Ask the tail instead.
    if tail -c 16384 "$INITRD" | python3 -c '
import sys, zlib
d = sys.stdin.buffer.read()
i = d.rfind(b"\x1f\x8b\x08")
out = zlib.decompress(d[i:], 16 + zlib.MAX_WBITS)
sys.exit(0 if b"\"image_on\":\"windows\"" in out else 1)'; then
        ok "...and a journal that says the image is on Windows"
    else
        bad "...and a journal that says the image is on Windows"
    fi
fi

# The image and its manifest go INTO the Windows volume, where the
# wizard downloads them. Then the "before" picture is taken again, so
# that the AurOS folder is part of what must survive.
put_in_windows() { # extra-step: "" | corrupt | missing
    L=$(losetup --find --show -o $((P2S*512)) --sizelimit $(((P2E-P2S+1)*512)) "$1")
    nt ntfs-3g "$L" "$TMP/m" >/dev/null 2>&1 || { losetup -d "$L"; return 1; }
    rm -rf "$TMP/m/AurOS"
    if [ "$2" != missing ]; then
        mkdir -p "$TMP/m/AurOS"
        cp "$TMP/win/AurOS/auros-desktop.img" "$TMP/win/AurOS/auros-desktop.img.manifest" \
           "$TMP/m/AurOS/"
        if [ "$2" = corrupt ]; then
            python3 - "$TMP/m/AurOS/auros-desktop.img" "$ROFF" "$RLEN" <<'EOPY'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(int(sys.argv[2]) + int(sys.argv[3]) // 2)
b = f.read(1); f.seek(-1, 1); f.write(bytes([b[0] ^ 0xFF])); f.close()
EOPY
        fi
    fi
    ( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$3"
    sync; umount "$TMP/m"; losetup -d "$L"
}
cp --sparse=always "$DISK" "$TMP/clean.img"
put_in_windows "$TMP/clean.img" "" "$TMP/win.before" \
    || { echo "  cannot write into the NTFS volume"; exit 2; }
cp --sparse=always "$DISK" "$TMP/corrupt.img"
put_in_windows "$TMP/corrupt.img" corrupt "$TMP/x.before"
cp --sparse=always "$DISK" "$TMP/missing.img"
put_in_windows "$TMP/missing.img" missing "$TMP/y.before"
WINFILES=$(wc -l < "$TMP/win.before")
grep -q 'AurOS/auros-desktop.img$' "$TMP/win.before" \
    && ok "the image is inside Windows ($WINFILES files there now)" \
    || bad "the image is inside Windows"

# ── one boot: ONE disk, no stick, nothing else plugged in ───────────
SRCDISK="$TMP/clean.img"
run() { # name  kargs  expect  [unchanged]
    cp --sparse=always "$SRCDISK" "$TMP/run.img"
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$TMP/vars.fd"
    before=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    : > "$TMP/out.txt"
    qemu-system-x86_64 -machine q35,accel=tcg -m 1536 -smp 2 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,unit=1,file="$TMP/vars.fd" \
        -no-reboot -kernel out/auros-staging-vmlinuz \
        -initrd "$INITRD" -append "console=ttyS0 $2" \
        -drive file="$TMP/run.img",format=raw,if=none,id=d0 \
        -device virtio-blk-pci,drive=d0,serial=AUROSTEST \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -display none -serial stdio > "$TMP/out.txt" 2>&1 &
    qp=$!
    seen=0; i=0
    while [ "$i" -lt 1200 ]; do
        kill -0 "$qp" 2>/dev/null || break
        if [ "$seen" -eq 0 ] && grep -aq "$3" "$TMP/out.txt" 2>/dev/null; then
            seen=1; i=1180
        fi
        sleep 1; i=$((i+1))
    done
    kill -9 "$qp" 2>/dev/null; wait "$qp" 2>/dev/null
    after=$(md5sum "$TMP/run.img" | cut -d' ' -f1)
    if grep -aq "$3" "$TMP/out.txt"; then ok "$1"
    else bad "$1" "expected: $3"
         grep -aE "aurstage:|aurstage-report" "$TMP/out.txt" | tail -8 | sed 's/^/        /'; fi
    if [ "${4:-}" = "unchanged" ]; then
        [ "$before" = "$after" ] && ok "...and the disk is byte-for-byte unchanged" \
                                 || bad "...and the disk is byte-for-byte unchanged"
    fi
}

echo
echo "  refusals, with the disk untouched (so the read-only mount wrote nothing)"
SRCDISK="$TMP/missing.img"
run "no image in the AurOS folder: refused" \
    "aurstage.install aurstage.min_gb=1" "is not there any more" unchanged
SRCDISK="$TMP/corrupt.img"
run "a damaged image in the AurOS folder: refused" \
    "aurstage.install aurstage.min_gb=1" "on the Windows drive is damaged" unchanged

echo
echo "  and then installing, for real, with no stick"
SRCDISK="$TMP/clean.img"
run "it installs and the new system starts" \
    "aurstage.install aurstage.min_gb=1" "INSTALLTEST-AUROS-STARTED"
grep -aq "Keeping the way back on this computer, before anything" "$TMP/out.txt" &&
grep -aq "the way back is on this computer, read back and" "$TMP/out.txt" \
    && ok "...the way back went onto the disk before AurOS did" \
    || bad "...the way back went onto the disk before AurOS did" \
           "$(grep -a 'way back' "$TMP/out.txt" | tail -2)"
W1=$(grep -an "the way back is on this computer" "$TMP/out.txt" | head -1 | cut -d: -f1)
W2=$(grep -an "Copying AurOS onto this computer" "$TMP/out.txt" | head -1 | cut -d: -f1)
[ -n "$W1" ] && [ -n "$W2" ] && [ "$W1" -lt "$W2" ] \
    && ok "...in that order" || bad "...in that order" "way back at $W1, copy at $W2"
grep -aq "verdict=installed" "$TMP/out.txt" \
    && ok "the installer reported success" \
    || bad "the installer reported success" "$(grep -a 'aurstage-report' "$TMP/out.txt" | tail -1)"

echo
echo "  and what it left behind"
if sgdisk -v "$TMP/run.img" 2>&1 | grep -q "No problems found"; then
    ok "another tool agrees the new table is valid"
else
    bad "another tool agrees the new table is valid" "$(sgdisk -v "$TMP/run.img" 2>&1 | head -3)"
fi
sgdisk -p "$TMP/run.img" 2>/dev/null | sed -n '/^Number/,$p' | tail -n +2 |
    awk 'NF{printf "      p%s %s..%s %s\n",$1,$2,$3,$7}'
[ -n "$(mach_part "$TMP/run.img" AUROS-SAVED)" ] \
    && ok "the way back has a partition of its own on this disk" \
    || bad "the way back has a partition of its own on this disk"
NEWEND=$(sgdisk -i 2 "$TMP/run.img" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
[ -n "$NEWEND" ] && [ "$NEWEND" -lt "$P2E" ] \
    && ok "Windows is smaller than it was" || bad "Windows is smaller than it was"
L=$(losetup --find --show -o $((P2S*512)) --sizelimit $(((NEWEND-P2S+1)*512)) "$TMP/run.img" 2>/dev/null)
if [ -n "$L" ] && nt ntfs-3g "$L" "$TMP/m" >/dev/null 2>&1; then
    ( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$TMP/win.after"
    umount "$TMP/m"
    cmp -s "$TMP/win.before" "$TMP/win.after" \
        && ok "all $WINFILES files in Windows, the AurOS folder too, are unchanged" \
        || bad "all $WINFILES files in Windows are unchanged" \
               "$(diff "$TMP/win.before" "$TMP/win.after" | head -4)"
else
    bad "the Windows volume still mounts"
fi
[ -n "$L" ] && losetup -d "$L"

# ── and back, with no stick: from the copy on the disk ──────────────
echo
echo "  and putting Windows back, still with no stick"
cp --sparse=always "$TMP/run.img" "$TMP/installed.img"
SRCDISK="$TMP/installed.img"
run "it restores from the copy on this computer" "aurstage.restore" "verdict=restored"
grep -aq "using the saved copy on /dev/vda" "$TMP/out.txt" \
    && ok "...using the copy on this computer's own disk" \
    || bad "...using the copy on this computer's own disk" \
           "$(grep -a 'using the saved copy' "$TMP/out.txt" | tail -1)"
N=$(sgdisk -p "$TMP/run.img" 2>/dev/null | sed -n '/^Number/,$p' | tail -n +2 | grep -c .)
[ "$N" = "3" ] && ok "the three original partitions are back" \
               || bad "the three original partitions are back" "it has $N"
S=$(sgdisk -i 2 "$TMP/run.img" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
E=$(sgdisk -i 2 "$TMP/run.img" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
[ "$S" = "$P2S" ] && [ "$E" = "$P2E" ] && ok "Windows' partition is its full size again" \
    || bad "Windows' partition is its full size again" "$S..$E, was $P2S..$P2E"
M=$(dd if="$TMP/run.img" bs=512 skip=$P1S count=$((P1E-P1S+1)) status=none | md5sum | cut -d' ' -f1)
[ "$M" = "$ESPMD5" ] && ok "the EFI partition is byte-for-byte what it was" \
                     || bad "the EFI partition is byte-for-byte what it was"
T2=$(ntfs_total "$TMP/run.img" "$P2S" | cut -d' ' -f1)
D=$((NTFSTOT - T2))
[ "$D" -ge 0 ] && [ "$D" -le "$NTFSSPC" ] \
    && ok "the Windows filesystem is its full size again" \
    || bad "the Windows filesystem is its full size again" "claims $T2, had $NTFSTOT"
L=$(losetup --find --show -o $((P2S*512)) --sizelimit $(((P2E-P2S+1)*512)) "$TMP/run.img" 2>/dev/null)
if [ -n "$L" ] && nt ntfs-3g "$L" "$TMP/m" >/dev/null 2>&1; then
    ( cd "$TMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$TMP/win.back"
    umount "$TMP/m"
    cmp -s "$TMP/win.before" "$TMP/win.back" \
        && ok "and every file in Windows is exactly what it was" \
        || bad "and every file in Windows is exactly what it was" \
               "$(diff "$TMP/win.before" "$TMP/win.back" | head -4)"
else
    bad "the Windows volume mounts again"
fi
[ -n "$L" ] && losetup -d "$L"

# ════════════════════════════════════════════════════════════════════
#  And the restart itself, the way a PC does it: firmware, Secure Boot
#  ON with Microsoft's keys, NO -kernel. Everything above hands QEMU the
#  kernel directly, which is why nothing ever noticed that the old boot
#  entry -- the kernel itself, signed by Canonical -- is a file no
#  firmware enforcing Secure Boot will start.
# ════════════════════════════════════════════════════════════════════
echo
echo "  and the restart itself, from firmware, with Secure Boot on"
MSCODE=/usr/share/OVMF/OVMF_CODE_4M.ms.fd
MSVARS=/usr/share/OVMF/OVMF_VARS_4M.ms.fd
if [ -f "$MSCODE" ] && [ -f "$MSVARS" ]; then
    SIM2="$TMP/sim2"
    mkdir -p "$SIM2/esp" "$SIM2/payload"
    : > "$SIM2/efivars.txt"
    # AND THE SERIAL AS WINDOWS WRITES IT FOR NVME, which is not the one
    # Linux reads: an identifier like "0025_3886_81B7_8F17." where the
    # kernel reports the drive's own serial (here, AUROSTEST). Matched
    # exactly, as it used to be, this machine is refused after its
    # restart about the right disk; the staging environment has to know
    # the disk by its partition table instead, and say so.
    printf '0\t%s\t%s\t512\t0025_3886_81B7_8F17.\tQEMU\t0\n' "$DISK" \
        "$(stat -c%s "$DISK")" > "$SIM2/disks.txt"
    cp out/auros-staging-shimx64.efi "$SIM2/payload/staging-shim"
    cp out/auros-staging-grubx64.efi "$SIM2/payload/staging-grub"
    cp out/auros-staging-mmx64.efi   "$SIM2/payload/staging-mokmgr" 2>/dev/null
    AURBRIDGE_STAGING_KARGS="console=ttyS0,115200 aurstage.min_gb=1" \
        out/aurbridge-sim "$SIM2" desktop none "$TMP/win/AurOS/auros-desktop.img" \
        out/auros-staging-vmlinuz out/auros-staging.img \
        > "$TMP/sim2.out" 2> "$TMP/sim2.err"
    grep -q 'verdict=armed' "$TMP/sim2.out" \
        && ok "AurBridge arms it through shim and grub" \
        || bad "AurBridge arms it through shim and grub" "$(tail -3 "$TMP/sim2.err")"
    grep -q 'AurOS Installer|\\EFI\\AurOS\\shimx64.efi||' "$SIM2/efivars.txt" \
        && ok "...the entry starts shim, with no command line of its own" \
        || bad "...the entry starts shim, with no command line of its own" \
               "$(grep -a Boot "$SIM2/efivars.txt" | head -2)"
    head -c 11 "$SIM2/esp/EFI/ubuntu/grub.cfg" 2>/dev/null | grep -q '# AurBridge' \
        && ok "...and grub reads a configuration that says whose it is" \
        || bad "...and grub reads a configuration that says whose it is"

    # What AurBridge put on the EFI partition, onto this disk's EFI
    # partition -- which on a real machine is where it wrote it.
    put_esp() { # disk  esp-tree
        export MTOOLS_SKIP_CHECK=1
        ( cd "$2" && find . -type d ! -name . -printf '%P\n' ) | while read -r d; do
            mmd -i "$1@@$((P1S*512))" "::/$d" >/dev/null 2>&1
        done
        ( cd "$2" && find . -type f -printf '%P\n' ) | while read -r f; do
            mcopy -o -i "$1@@$((P1S*512))" "$2/$f" "::/$f" || exit 1
        done
    }
    cp --sparse=always "$TMP/clean.img" "$TMP/sb.img"
    put_esp "$TMP/sb.img" "$SIM2/esp"
    mdir -i "$TMP/sb.img@@$((P1S*512))" ::/EFI/AurOS 2>/dev/null | grep -qi shimx64 \
        && ok "the start-up files are on the machine's EFI partition" \
        || bad "the start-up files are on the machine's EFI partition"

    # The NVRAM the Windows half would have left: the entry, and BootNext.
    sbboot() { # disk  loader  marker  timeout  -> 0 if marker seen
        cp "$MSVARS" "$TMP/sbvars.fd"
        python3 tools/efivarstore.py "$TMP/sbvars.fd" plant Boot0009 \
            "AurOS Installer" "$2" || return 2
        python3 -c "
import sys; sys.path.insert(0, 'tools'); import efivarstore as e
e.plant(sys.argv[1], 'BootNext', '8be4df61-93ca-11d2-aa0d-00e098032b8c', b'\x09\x00')
" "$TMP/sbvars.fd" || return 2
        # WITH A NETWORK CARD. Phase 7 refuses a machine AurOS would have
        # no network on (R13), and the first run of this section proved
        # the whole signed chain and then stopped there, correctly, with
        # verdict=no-network -- on a virtual machine given no card at all.
        mach_boot_firmware "$1" "$3" "$4" "$MSCODE" "$TMP/sbvars.fd" \
            "-global driver=cfi.pflash01,property=secure,value=on -netdev user,id=n0 -device virtio-net-pci,netdev=n0"
    }

    # FIRST, THE OLD WAY, to show the bug was real: the entry names the
    # kernel itself. The firmware must refuse it, and the installer
    # must never start.
    cp --sparse=always "$TMP/sb.img" "$TMP/sbold.img"
    sbboot "$TMP/sbold.img" '\EFI\AurOS\staging.efi' "aurstage:" 240
    if grep -aq "aurstage:" "$TMP/fout.txt"; then
        bad "the old entry (the kernel itself) is refused under Secure Boot" \
            "the firmware started it; this check no longer shows anything"
    else
        ok "the old entry (the kernel itself) is refused under Secure Boot"
    fi

    # AND THE NEW WAY: shim, grub, kernel, install, hand over.
    sbboot "$TMP/sb.img" '\EFI\AurOS\shimx64.efi' "INSTALLTEST-AUROS-STARTED" 1500
    if grep -aq "INSTALLTEST-AUROS-STARTED" "$TMP/fout.txt"; then
        ok "through shim and grub it installs, and AurOS starts"
    else
        bad "through shim and grub it installs, and AurOS starts" \
            "$(grep -aE 'aurstage|Security|shim|grub|error' "$TMP/fout.txt" | tail -8)"
    fi
    grep -aq "known by its partition table" "$TMP/fout.txt" \
        && ok "...knowing the disk by its partition table, not the serial" \
        || bad "...knowing the disk by its partition table, not the serial" \
               "$(grep -a 'record ' "$TMP/fout.txt" | tail -2)"
    grep -aq "verdict=installed" "$TMP/fout.txt" \
        && ok "...and the installer reported success" \
        || bad "...and the installer reported success" \
               "$(grep -a 'aurstage-report' "$TMP/fout.txt" | tail -1)"
else
    echo "    (no Microsoft-keyed OVMF here; skipped)"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    exit 1
fi
echo "$checked checks: with no memory stick, it installs AurOS, starts it,"
echo "puts Windows back from the copy on the disk, and every file in Windows"
echo "is exactly what it was."
exit 0
