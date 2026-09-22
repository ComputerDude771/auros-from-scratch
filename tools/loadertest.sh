#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  loadertest — can the computer start AurOS by itself?
#
#  Every other end-to-end test in this directory hands QEMU a kernel
#  with -kernel, which is the right thing when what is being tested is
#  the staging environment. It also means not one of them has ever
#  asked the question this product actually turns on: with the power
#  off and the memory stick out, does the machine come back up in
#  AurOS.
#
#  For a long time the answer was no, and nothing said so. The
#  installer shrank Windows, wrote AurOS, verified it, committed the
#  table and handed over in the same boot -- and wrote no bootloader
#  and no boot entry. installtest passed all thirty-two of its checks
#  on a machine that would have lost AurOS at the next restart.
#
#  So this one installs, and then starts the machine the way its owner
#  would: firmware, one disk, no -kernel, no stick.
#
#    sudo sh tools/loadertest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-58s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-58s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

. tools/machine.sh
mach_need
[ -f out/auros-staging.img ] || { echo "run ./build/staging first"; exit 2; }

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another end-to-end test is running"; exit 2; }
MTMP=$(mktemp -d "${TMPDIR:-/tmp}/loadertest.XXXXXX")
[ -n "$MTMP" ] && [ -d "$MTMP" ] || { rmdir "$LOCK"; echo "no scratch dir"; exit 2; }
trap 'mountpoint -q "$MTMP/m" 2>/dev/null && umount "$MTMP/m"; rm -rf "$MTMP" "$LOCK"' EXIT

echo
echo "With the power off and the stick out, does it come back up in AurOS?"
echo

mach_disk
mach_image
mach_stick
mach_journal
echo "  a 3 GiB machine: ESP, a 1 GiB Windows with $WINFILES files, WinRE at the end"
echo "  an AurOS image with a real signed boot chain in its $((ELEN/1048576)) MiB EFI partition"

# The NVRAM this machine starts with already holds the entry the
# Windows half made to get here, so that the installer has something
# real to tidy away.
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$MTMP/start-vars.fd"
python3 tools/efivarstore.py "$MTMP/start-vars.fd" plant Boot0009 \
    "AurOS Installer" '\EFI\AurOS\staging.efi' \
    || { echo "  could not prepare the machine's NVRAM"; exit 2; }
MACH_VARS="$MTMP/start-vars.fd"; export MACH_VARS
echo "  and firmware that already has an \"AurOS Installer\" entry in it"
echo

# ── 0. the instrument ───────────────────────────────────────────────
# EVERY FIRMWARE CLAIM BELOW IS THIS DECODER'S WORD. It reads OVMF's
# variable store with offsets taken from EDK2's headers, and it got
# them wrong once already -- NameSize and DataSize read from +48
# instead of +36 and +40, which yielded one plausible-looking variable
# and then garbage. A decoder that silently finds nothing turns "the
# installer left BootOrder alone" and "the installer's own entry was
# tidied away" into two checks that pass by not looking.
#
# So it is made to read a store it did not write -- the untouched
# OVMF_VARS the distribution ships, whose PK, KEK, db and dbx were put
# there by EDK2 -- before anything here trusts it.
echo "  the instrument"
if python3 tools/efivarstore.py --self-test >"$MTMP/st.txt" 2>&1
then ok "the variable-store reader still reads a store it did not write"
else bad "the variable-store reader still reads a store it did not write" \
         "$(tail -4 "$MTMP/st.txt")"
     echo; echo "  $checked checked, $fail failed"; exit 1
fi
echo

# ── 1. install it ───────────────────────────────────────────────────
echo "  the install"
if mach_boot out/auros-staging.img "$DISK" "$STICK" \
             "aurstage.install aurstage.min_gb=1" "INSTALLTEST-AUROS-STARTED" 1800
then ok "it installs and hands over in the same boot"
else bad "it installs and hands over in the same boot" \
         "$(grep -a 'aurstage' "$MTMP/out.txt" | tail -6)"
     echo; echo "  $checked checked, $fail failed"; exit 1
fi
DISKAFTER="$MTMP/run.img"
VARS="$MTMP/vars.fd"

grep -aq 'startup  AurOS is in this computer' "$MTMP/out.txt" \
  && ok "and says it put AurOS in the start-up menu" \
  || bad "and says it put AurOS in the start-up menu" \
         "$(grep -a 'startup\|start-up' "$MTMP/out.txt" | tail -4)"

# ── 2. what it left on the disk ─────────────────────────────────────
echo
echo "  the partition it starts from"
SET=$(mach_part "$DISKAFTER" AUROS-BOOT)
if [ -n "$SET" ]; then
    BNUM=$(echo "$SET" | cut -d' ' -f1)
    BFIRST=$(echo "$SET" | cut -d' ' -f2)
    BLAST=$(echo "$SET" | cut -d' ' -f3)
    BGUID=$(echo "$SET" | cut -d' ' -f4)
    BTYPE=$(echo "$SET" | cut -d' ' -f5)
    ok "there is an AUROS-BOOT partition"
else
    bad "there is an AUROS-BOOT partition" "$(sgdisk -p "$DISKAFTER" | tail -8)"
    BNUM=0; BFIRST=0; BLAST=0; BGUID=; BTYPE=
    # THE TWO BELOW ARE SKIPPED, AND SAYING SO IS THE POINT. They used
    # to vanish with no ok, no FAIL and no output -- only the final
    # count moved, and only if somebody was counting.
    bad "and it is the image's EFI partition, byte for byte" \
        "skipped: there is no AUROS-BOOT partition to look at"
    bad "and it was made big enough for it" \
        "skipped: there is no AUROS-BOOT partition to look at"
fi

[ "$BTYPE" = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b" ] \
  && ok "and the firmware will recognise it (EFI System)" \
  || bad "and the firmware will recognise it (EFI System)" "type is $BTYPE"

# THE BYTES, not a file listing. loader.c copies the image's EFI
# partition whole and claims to read every byte back; this is the
# independent check of that claim, and it is one md5 each side.
if [ "$BFIRST" -gt 0 ]; then
    WANT=$(dd if="$AIMG" bs=512 skip=$((EOFF/512)) count=$((ELEN/512)) \
           status=none | md5sum | cut -d' ' -f1)
    GOT=$(dd if="$DISKAFTER" bs=512 skip="$BFIRST" count=$((ELEN/512)) \
          status=none | md5sum | cut -d' ' -f1)
    [ "$WANT" = "$GOT" ] \
      && ok "and it is the image's EFI partition, byte for byte" \
      || bad "and it is the image's EFI partition, byte for byte" \
             "image $WANT" "disk  $GOT"
    # ...and it is big enough to hold it, which is the refusal the
    # planner is supposed to make impossible.
    HAVE=$(( (BLAST - BFIRST + 1) * 512 ))
    [ "$HAVE" -ge "$ELEN" ] \
      && ok "and it was made big enough for it" \
      || bad "and it was made big enough for it" "$HAVE < $ELEN"
fi

# ── 3. what it did NOT do ───────────────────────────────────────────
echo
echo "  and the computer's own EFI partition"
NOWESP=$(dd if="$DISKAFTER" bs=512 skip=$P1S count=$((P1E-P1S+1)) status=none \
         | md5sum | cut -d' ' -f1)
[ "$NOWESP" = "$ESPMD5" ] \
  && ok "is byte-for-byte what it was (R12: never mounted, never written)" \
  || bad "is byte-for-byte what it was (R12: never mounted, never written)" \
         "before $ESPMD5" "after  $NOWESP"

# ── 4. the firmware's menu ──────────────────────────────────────────
echo
echo "  the boot entry"
E=$(python3 tools/efivarstore.py "$VARS" entry AurOS 2>/dev/null)
if [ -n "$E" ]; then
    ENUM=$(echo "$E" | sed -n 's/^num=//p')
    EPATH=$(echo "$E" | sed -n 's/^path=//p')
    EGUID=$(echo "$E" | sed -n 's/^part_guid=//p')
    EPNUM=$(echo "$E" | sed -n 's/^part_number=//p')
    EFIRST=$(echo "$E" | sed -n 's/^part_first=//p')
    EBLK=$(echo "$E" | sed -n 's/^part_blocks=//p')
    ENODES=$(echo "$E" | sed -n 's/^nodes=//p')
    ok "there is one, and it is called AurOS"
else
    bad "there is one, and it is called AurOS" \
        "$(python3 tools/efivarstore.py "$VARS" list | grep -i boot)"
    ENUM=; EPATH=; EGUID=; EPNUM=; EFIRST=; EBLK=; ENODES=
fi

[ "$EPATH" = '\EFI\AurOS\shimx64.efi' ] \
  && ok "it starts the signed shim" \
  || bad "it starts the signed shim" "path is $EPATH"

# THE SHORT FORM UEFI REQUIRES. A bare File() node is not one of the
# two forms 2.10 s10.3.5 makes the boot manager expand, so LoadImage
# would return EFI_NOT_FOUND on every machine -- and the machine would
# come back to Windows, which is at least the safe direction and is
# also a product that never installs.
[ "$ENODES" = "04/01,04/04,7f/ff" ] \
  && ok "as a Hard Drive node, then a File node, then End" \
  || bad "as a Hard Drive node, then a File node, then End" "nodes: $ENODES"

[ -n "$BGUID" ] && [ "$EGUID" = "$BGUID" ] \
  && ok "and the partition it names is the one on the disk" \
  || bad "and the partition it names is the one on the disk" \
         "entry $EGUID" "disk  $BGUID"

[ "$EPNUM" = "$BNUM" ] && [ "$EFIRST" = "$BFIRST" ] \
  && [ "$EBLK" = "$((BLAST - BFIRST + 1))" ] \
  && ok "with the number, the start and the length it really has" \
  || bad "with the number, the start and the length it really has" \
         "entry: $EPNUM $EFIRST $EBLK" \
         "disk:  $BNUM $BFIRST $((BLAST - BFIRST + 1))"

BN=$(python3 tools/efivarstore.py "$VARS" bootnext 2>/dev/null)
[ -n "$ENUM" ] && [ "$BN" = "$ENUM" ] \
  && ok "BootNext is armed at it -- one-shot, so a failure self-reverts" \
  || bad "BootNext is armed at it -- one-shot, so a failure self-reverts" \
         "BootNext=$BN entry=$ENUM"

# R3 AND THE WHOLE OF PHASE 10: Windows stays this machine's default
# until somebody has seen AurOS work and said so.
ORDER=$(python3 tools/efivarstore.py "$VARS" bootorder 2>/dev/null)
# NEITHER MAY BE EMPTY. With no entry number the pattern became `*"  "*`
# and matched nothing; with no BootOrder there was nothing to search.
# Both read as "left alone" while nothing had been looked at.
if [ -z "$ENUM" ] || [ -z "$ORDER" ]; then
    bad "and BootOrder was left alone -- Windows is still the default" \
        "entry='$ENUM' BootOrder='$ORDER' -- one of them could not be read"
else
    case " $ORDER " in
      *" $ENUM "*) bad "and BootOrder was left alone -- Windows is still the default" \
                       "BootOrder is '$ORDER' and our entry $ENUM is in it" ;;
      *) ok "and BootOrder was left alone -- Windows is still the default" ;;
    esac
fi

# EXIT 1 AND NOTHING ON STDERR. The decoder exits 1 for "no such
# entry" AND for a missing file, a store it cannot parse, a traceback,
# or no python3 at all -- and 2>/dev/null hid which. Every one of those
# read as "it was tidied away".
python3 tools/efivarstore.py "$VARS" entry "AurOS Installer" \
    >/dev/null 2>"$MTMP/ev.err"
evrc=$?
if [ "$evrc" -eq 1 ] && [ ! -s "$MTMP/ev.err" ]; then
    ok "the installer's own entry was tidied away"
elif [ "$evrc" -eq 0 ]; then
    bad "the installer's own entry was tidied away" \
        "\"AurOS Installer\" is still in the menu"
else
    bad "the installer's own entry was tidied away" \
        "the variable store could not be read: exit $evrc" \
        "$(head -3 "$MTMP/ev.err")"
fi

# ── 5. the question ─────────────────────────────────────────────────
echo
echo "  starting it the way its owner would"
if mach_boot_firmware "$DISKAFTER" "AUROS-STARTED-FROM-FIRMWARE" 900; then
    ok "firmware, one disk, no stick, no -kernel: AurOS starts"
else
    bad "firmware, one disk, no stick, no -kernel: AurOS starts" \
        "$(tail -14 "$MTMP/fout.txt")"
fi

# ── 6. and with Secure Boot actually on ─────────────────────────────
#
# A DIFFERENT FIRMWARE AND FRESH NVRAM, because the install above ran
# with -kernel and OVMF will not load an unsigned kernel that way.
# Nothing in NVRAM points at AurOS here, so what is being tested is the
# other half of the chain: the removable path -- /EFI/BOOT/BOOTX64.EFI
# and the BOOTX64.CSV beside it, both copied out of the image -- and
# whether the shim and grub in it are signatures this firmware trusts.
echo
echo "  and with Secure Boot on, against Microsoft's own keys"
if [ -f /usr/share/OVMF/OVMF_CODE_4M.ms.fd ] && \
   [ -f /usr/share/OVMF/OVMF_VARS_4M.ms.fd ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd "$MTMP/msvars.fd"
    if mach_boot_firmware "$DISKAFTER" "AUROS-STARTED-FROM-FIRMWARE" 900 \
         /usr/share/OVMF/OVMF_CODE_4M.ms.fd "$MTMP/msvars.fd" \
         "-global driver=cfi.pflash01,property=secure,value=on"; then
        ok "the signed chain boots on firmware that enforces signatures"
    else
        bad "the signed chain boots on firmware that enforces signatures" \
            "$(tail -14 "$MTMP/fout.txt")"
    fi
else
    echo "    (no Microsoft-keyed OVMF here; skipped)"
fi

echo
echo "  $checked checked, $fail failed"
[ "$fail" -eq 0 ] || exit 1
