#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  bootchaintest — does auros-bootchain refuse what would not start?
#
#  rootfs/usr/lib/auros/bootchain is what an installed AurOS runs as
#  update-grub and grub-install. It is the reason the kernel, grub and
#  shim may now take security updates by themselves, so the checks
#  that matter are the refusals: each one is made to fire, and each
#  time the EFI partition must come out byte-for-byte unchanged.
#
#    menu   newest kernel first, the previous one as grub's fallback,
#           a half-installed kernel left out, "Put Windows back" and the
#           Windows entry kept; no kernel at all -> refused, old menu
#           untouched; a template that lost its restore entry -> refused.
#    sync   with OVMF's own Microsoft db:
#             trusted pair                      -> installed, then current
#             db with the 2011 key removed      -> refused (the 2023 change)
#             the 2011 CA in dbx                -> refused
#             shim's own hash in dbx            -> refused
#             NVRAM SBAT level revokes the grub -> refused
#             a stop between grub and shim      -> new grub + old shim,
#                                                  the order that starts
#             no AurOS EFI partition            -> nothing, not an error
#    hash   the Authenticode hash dbx is compared with, against
#           osslsigncode's.
#
#  The shim and grub are the real signed binaries out of a forged
#  rootfs, never stand-ins: the gates read their signatures and SBAT
#  sections, and a fixture would only test the fixture.
#
#    sh tools/bootchaintest.sh [ROOTFS]   (default work/forge/desktop/rootfs)
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
RFS="${1:-work/forge/desktop/rootfs}"
BC=rootfs/usr/lib/auros/bootchain
E=tools/efivarstore.py
VARS=/usr/share/OVMF/OVMF_VARS_4M.ms.fd

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-64s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-64s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }
check() { # name, then a command whose success is the pass
    n=$1; shift
    if "$@" >/dev/null 2>&1; then ok "$n"; else bad "$n"; fi
}

for t in python3 openssl osslsigncode; do
    command -v $t >/dev/null || { echo "need $t"; exit 2; }
done
[ -f "$VARS" ] || { echo "need $VARS (ovmf)"; exit 2; }
SHIM="$RFS/usr/lib/shim/shimx64.efi.dualsigned"
OLDSHIM="$RFS/usr/lib/shim/shimx64.efi.signed.latest"
GRUB="$RFS/usr/lib/grub/x86_64-efi-signed/grubx64.efi.signed"
for f in "$SHIM" "$OLDSHIM" "$GRUB"; do
    [ -f "$f" ] || { echo "no $f: forge a rootfs first"; exit 2; }
done

T=$(mktemp -d "${TMPDIR:-/tmp}/bootchain.XXXXXX")
trap 'rm -rf "$T"' EXIT
G_SEC=d719b2cb-3d3a-4596-a3bc-dad00e67656f
G_GLB=8be4df61-93ca-11d2-aa0d-00e098032b8c
G_SHIM=605dab50-e046-4300-abb6-3dd810dd8b23

echo
echo "auros-bootchain: the menu, and the three gates in front of the EFI partition"

# ── menu ────────────────────────────────────────────────────────────
echo
echo "  menu"
M="$T/menu"
mkdir -p "$M/boot/grub" "$M/etc" "$M/usr/lib/auros"
cp rootfs/usr/lib/auros/grub.cfg.in "$M/usr/lib/auros/"
U=0b1c2d3e-4f50-6172-8394-a5b6c7d8e9f0
printf 'UUID=%s  /  ext4  defaults  0 1\nUUID=AAAA-BBBB  /boot/efi  vfat  umask=0077  0 2\n' "$U" > "$M/etc/fstab"
for v in 6.8.0-9-generic 6.8.0-10-generic 6.8.0-11-generic; do
    : > "$M/boot/vmlinuz-$v"
done
: > "$M/boot/initrd.img-6.8.0-9-generic"
: > "$M/boot/initrd.img-6.8.0-10-generic"
# -11 has no initrd yet: dpkg is between the two
python3 "$BC" menu --root "$M" >/dev/null 2>&1
C="$M/boot/grub/grub.cfg"
grep -A1 -- "--id auros {" "$C" 2>/dev/null | grep -q "vmlinuz-6.8.0-10-generic root=UUID=$U " \
  && ok "the newest COMPLETE kernel is the default (10, not 9 or 11)" \
  || bad "the newest COMPLETE kernel is the default (10, not 9 or 11)" "$(grep vmlinuz "$C" 2>/dev/null | head -3)"
grep -q "11-generic" "$C" && bad "a kernel without its initrd is left out" || ok "a kernel without its initrd is left out"
grep -A1 -- "--id auros-previous" "$C" | grep -q "vmlinuz-6.8.0-9-generic" \
  && ok "the previous kernel has its own entry" || bad "the previous kernel has its own entry"
grep -q "^set fallback=auros-previous" "$C" \
  && ok "...and grub falls back to it if the newest will not start" \
  || bad "...and grub falls back to it if the newest will not start"
grep -q -- "--id put-windows-back-yes" "$C" && grep -q "bootmgfw.efi" "$C" \
  && ok "Put Windows back and the Windows entry are still there" \
  || bad "Put Windows back and the Windows entry are still there"
grep -q "@[A-Z_]*@" "$C" && bad "no placeholder left unfilled" "$(grep -o '@[A-Z_]*@' "$C" | sort -u)" \
  || ok "no placeholder left unfilled"
rm -f "$M/boot/vmlinuz-6.8.0-9-generic"
python3 "$BC" menu --root "$M" >/dev/null 2>&1
if grep -q "fallback\|auros-previous" "$C"; then bad "one kernel: no fallback to a kernel that is not there"
else ok "one kernel: no fallback to a kernel that is not there"; fi
cp "$C" "$T/good.cfg"
mv "$M/boot/initrd.img-6.8.0-10-generic" "$T/"
python3 "$BC" menu --root "$M" >/dev/null 2>&1; rc=$?
[ "$rc" != 0 ] && cmp -s "$C" "$T/good.cfg" \
  && ok "no kernel at all: refused, and yesterday's menu kept" \
  || bad "no kernel at all: refused, and yesterday's menu kept" "rc=$rc"
mv "$T/initrd.img-6.8.0-10-generic" "$M/boot/"
sed -i '/put-windows-back-yes/d' "$M/usr/lib/auros/grub.cfg.in"
python3 "$BC" menu --root "$M" >/dev/null 2>&1; rc=$?
[ "$rc" != 0 ] && cmp -s "$C" "$T/good.cfg" \
  && ok "a template that lost the restore entry: refused" \
  || bad "a template that lost the restore entry: refused" "rc=$rc"

# ── firmware fixtures ───────────────────────────────────────────────
# efivarfs files: four bytes of attributes, then the data. db and dbx
# come out of OVMF's own variable store -- Microsoft's real keys.
efi() { # dir name guid file-with-data
    { printf '\047\0\0\0'; cat "$4"; } > "$1/$2-$3"
}
mkfw() { # dir varsfile
    mkdir -p "$1"
    python3 "$E" "$2" get db "$T/db.bin"   >/dev/null || return 1
    python3 "$E" "$2" get dbx "$T/dbx.bin" >/dev/null || return 1
    efi "$1" db  $G_SEC "$T/db.bin"
    efi "$1" dbx $G_SEC "$T/dbx.bin"
    printf '\001' > "$T/sb.bin"; efi "$1" SecureBoot $G_GLB "$T/sb.bin"
}
FW="$T/fw"; mkfw "$FW" "$VARS" || { echo "could not read $VARS"; exit 2; }

python3 "$E" "$VARS" certs db | grep -qx "Microsoft Corporation UEFI CA 2011" \
  && ok "fixture: OVMF's db has the 2011 UEFI CA" \
  || bad "fixture: OVMF's db has the 2011 UEFI CA"

cp "$VARS" "$T/v23.fd"
python3 "$E" "$T/v23.fd" db-without "Microsoft Corporation UEFI CA 2011" >/dev/null
FW23="$T/fw23"; mkfw "$FW23" "$T/v23.fd"
python3 "$E" "$T/v23.fd" certs db | grep -qx "Microsoft Corporation UEFI CA 2011" \
  && bad "fixture: the 2023-only db really lacks the 2011 CA" \
  || ok "fixture: the 2023-only db really lacks the 2011 CA"

# The 2011 CA, moved from db into dbx: a db that still lists it, and a
# dbx that revokes it. dbx wins, as it does in firmware.
FWX="$T/fwx"; mkfw "$FWX" "$VARS"
python3 - "$FWX/db-$G_SEC" "$FWX/dbx-$G_SEC" <<'EOF'
import struct, sys
db = open(sys.argv[1], 'rb').read()
attrs, data = db[:4], db[4:]
X509 = bytes.fromhex("a159c0a5e494a74a87b5ab155c2bf072")
o, keep = 0, None
while o + 28 <= len(data):
    ls, hs, ss = struct.unpack_from("<III", data, o + 16)
    if data[o:o+16] == X509 and b"Corporation UEFI CA 2011" in data[o:o+ls]:
        keep = data[o:o+ls]
    o += ls
assert keep, "no 2011 UEFI CA list in db"
dbx = open(sys.argv[2], 'rb').read()
open(sys.argv[2], 'wb').write(dbx + keep)
EOF

# shim's own Authenticode hash, as a dbx entry (EFI_CERT_SHA256).
H=$(python3 -c "
src=open('$BC').read(); m={}
exec(compile(src.replace('if __name__ == \"__main__\":', 'if False:'), 'bc', 'exec'), m)
print(m['PE'](open('$SHIM','rb').read()).authenticode_sha256().hex())")
FWH="$T/fwh"; mkfw "$FWH" "$VARS"
python3 - "$FWH/dbx-$G_SEC" "$H" <<'EOF'
import struct, sys
p, h = sys.argv[1], bytes.fromhex(sys.argv[2])
SHA = bytes.fromhex("2616c4c14c509240aca941f936934328")
owner = bytes(16)
lst = SHA + struct.pack("<III", 28 + 48, 0, 48) + owner + h
open(p, 'ab').write(lst)
EOF

# A revocation level in NVRAM above this grub's generation -- what
# Windows Update writes when it revokes a grub.
GG=$(python3 -c "
src=open('$BC').read(); m={}
exec(compile(src.replace('if __name__ == \"__main__\":', 'if False:'), 'bc', 'exec'), m)
pe=m['PE'](open('$GRUB','rb').read()); print(m['sbat_csv'](pe.sections['.sbat'].decode('latin-1'))['grub'])")
FWS="$T/fws"; mkfw "$FWS" "$VARS"
printf 'sbat,1,2099010100\nshim,4\ngrub,%d\n' $((GG + 1)) > "$T/lvl"
efi "$FWS" SbatLevelRT $G_SHIM "$T/lvl"

# ── sync ────────────────────────────────────────────────────────────
echo
echo "  sync"
newesp() { # dir: an installed AurOS's EFI partition, one shim behind
    rm -rf "$1"; mkdir -p "$1/EFI/AurOS" "$1/EFI/BOOT"
    cp "$OLDSHIM" "$1/EFI/AurOS/shimx64.efi"; cp "$GRUB" "$1/EFI/AurOS/grubx64.efi"
    cp "$OLDSHIM" "$1/EFI/BOOT/BOOTX64.EFI";  cp "$GRUB" "$1/EFI/BOOT/grubx64.efi"
    ( cd "$1" && find . -type f -exec md5sum {} + | sort ) > "$1.md5"
}
same() { ( cd "$1" && find . -type f -exec md5sum {} + | sort ) | cmp -s - "$1.md5"; }
SR="$T/sroot"; mkdir -p "$SR/usr/lib/shim" "$SR/usr/lib/grub/x86_64-efi-signed" "$SR/var/lib/auros"
cp "$SHIM" "$OLDSHIM" "$SR/usr/lib/shim/"
cp "$RFS/usr/lib/shim/mmx64.efi" "$RFS/usr/lib/shim/fbx64.efi" "$SR/usr/lib/shim/" 2>/dev/null
cp "$GRUB" "$SR/usr/lib/grub/x86_64-efi-signed/"
run() { python3 "$BC" sync --root "$SR" --esp "$ESP" --efivars "$1"; }

ESP="$T/esp"
for case in "2023-only db:$FW23:the 2011 key removed from db (the 2023 change)" \
            "2011 CA in dbx:$FWX:the 2011 CA revoked in dbx" \
            "shim hash in dbx:$FWH:shim's own hash in dbx" \
            "SBAT revokes grub:$FWS:NVRAM SBAT level above this grub's generation"; do
    fw=$(echo "$case" | cut -d: -f2); what=$(echo "$case" | cut -d: -f3)
    newesp "$ESP"
    out=$(run "$fw" 2>&1); rc=$?
    if [ "$rc" = 3 ] && same "$ESP"; then ok "$what: refused, partition unchanged"
    else bad "$what: refused, partition unchanged" "rc=$rc" "$out"; fi
done
grep -q "^result=refused" "$SR/var/lib/auros/bootchain.state" \
  && ok "...and the refusal is written down for status" \
  || bad "...and the refusal is written down for status"

newesp "$ESP"
out=$(run "$FW" 2>&1); rc=$?
if [ "$rc" = 0 ] && cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$SHIM" && \
   cmp -s "$ESP/EFI/BOOT/BOOTX64.EFI" "$SHIM" && cmp -s "$ESP/EFI/AurOS/grubx64.efi" "$GRUB"; then
    ok "trusted pair on OVMF's Microsoft db: installed, both directories"
else bad "trusted pair on OVMF's Microsoft db: installed, both directories" "rc=$rc" "$out"; fi
[ -f "$ESP/EFI/AurOS/mmx64.efi" ] && ok "...with MokManager beside it" || bad "...with MokManager beside it"
out=$(run "$FW" 2>&1); rc=$?
case "$out" in *"is current"*) [ "$rc" = 0 ] && ok "run again: nothing to do" || bad "run again: nothing to do" "rc=$rc";;
    *) bad "run again: nothing to do" "$out";; esac

newesp "$ESP"
out=$(AUROS_BOOTCHAIN_FAIL_AFTER=grub run "$FW" 2>&1); rc=$?
if [ "$rc" = 9 ] && cmp -s "$ESP/EFI/AurOS/grubx64.efi" "$GRUB" && \
   cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$OLDSHIM"; then
    ok "stopped half way: grub went first, the old shim is still there"
else bad "stopped half way: grub went first, the old shim is still there" "rc=$rc" "$out"; fi

rm -rf "$T/none"; mkdir -p "$T/none"
out=$(python3 "$BC" sync --root "$SR" --esp "$T/none" --efivars "$FW" 2>&1); rc=$?
[ "$rc" = 0 ] && ok "no AurOS EFI partition (forge's chroot): nothing, and not an error" \
  || bad "no AurOS EFI partition (forge's chroot): nothing, and not an error" "rc=$rc" "$out"

# The wrapper dpkg runs: a refusal must not fail the package.
newesp "$ESP"
W="$T/grub-install"
sed "s#/usr/lib/auros/bootchain sync#python3 $PWD/$BC sync --root $SR --esp $ESP --efivars $FW23#" \
    rootfs/usr/lib/auros/grub-install > "$W"
sh "$W" --target=x86_64-efi >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && same "$ESP" \
  && ok "as grub-install under dpkg: a refusal exits 0, changes nothing" \
  || bad "as grub-install under dpkg: a refusal exits 0, changes nothing" "rc=$rc"

# ── proving the gates are not decorative ────────────────────────────
# Each refusal above must come from its gate: with the gate's check
# stubbed out, the same case has to go through.
echo
echo "  the gates, switched off one at a time, must let the bad case through"
mutant() { # name, python replace-from, replace-to, fw
    sed "s|$2|$3|" "$BC" > "$T/mut"
    cmp -s "$T/mut" "$BC" && { bad "$1" "the mutation matched nothing"; return; }
    newesp "$ESP"
    python3 "$T/mut" sync --root "$SR" --esp "$ESP" --efivars "$4" >/dev/null 2>&1
    if cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$SHIM"; then ok "$1"
    else bad "$1" "the bad case was still refused: the test does not test the gate"; fi
}
mutant "without the db check, the 2023-only firmware gets the shim" \
       'if not trusted:' 'if False:' "$FW23"
mutant "without the dbx-hash check, a revoked hash gets through" \
       'in fw\["dbx_hashes"\]:' 'in ():' "$FWH"
mutant "without the dbx-certificate check, a revoked CA's shim gets through" \
       'if anchored(certs, fw\["dbx_certs"\], tmp):' 'if False:' "$FWX"
mutant "without the SBAT check, a revoked grub gets through" \
       'r = revoked_by(grub_sbat, level)' 'r = []' "$FWS"

# ── the hash dbx is compared with ───────────────────────────────────
echo
echo "  Authenticode"
O=$(osslsigncode verify -in "$OLDSHIM" 2>&1 | sed -n 's/^Calculated message digest *: *\([0-9A-F]*\).*/\1/p' | head -1 | tr 'A-F' 'a-f')
M2=$(python3 -c "
src=open('$BC').read(); m={}
exec(compile(src.replace('if __name__ == \"__main__\":', 'if False:'), 'bc', 'exec'), m)
print(m['PE'](open('$OLDSHIM','rb').read()).authenticode_sha256().hex())")
[ -n "$O" ] && [ "$O" = "$M2" ] && ok "matches osslsigncode's ($O)" \
  || bad "matches osslsigncode's" "osslsigncode=$O" "bootchain=$M2"

echo
echo "bootchaintest: $((checked - fail))/$checked"
[ "$fail" = 0 ]
