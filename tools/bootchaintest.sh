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
python3 "$BC" menu --root "$M" --running none >/dev/null 2>&1
C="$M/boot/grub/grub.cfg"
grep -A1 -- "--id auros {" "$C" 2>/dev/null | grep -q "vmlinuz-6.8.0-10-generic root=UUID=$U " \
  && ok "the newest COMPLETE kernel is the default (10, not 9 or 11)" \
  || bad "the newest COMPLETE kernel is the default (10, not 9 or 11)" "$(grep vmlinuz "$C" 2>/dev/null | head -3)"
grep -q "11-generic" "$C" && bad "a kernel without its initrd is left out" || ok "a kernel without its initrd is left out"
grep -A1 -- "--id auros-previous" "$C" | grep -q "vmlinuz-6.8.0-9-generic" \
  && ok "the previous kernel has its own entry" || bad "the previous kernel has its own entry"
# grub's fallback is an entry NUMBER (Ubuntu's 2.12 unsets anything
# else). Count the top-level entries the way grub does, independently
# of the program that wrote the number.
want=$(awk '/^(menuentry|submenu) /{ if ($0 ~ /--id auros-previous/) {print n; exit} n++ }' "$C")
got=$(sed -n 's/^set fallback=\([0-9]*\)$/\1/p' "$C")
[ -n "$want" ] && [ "$got" = "$want" ] \
  && ok "...and grub falls back to it, by number ($got), if the newest will not start" \
  || bad "...and grub falls back to it, by number, if the newest will not start" "fallback=$got, entry is number $want"
: > "$M/boot/vmlinuz-6.8.0-8-generic"; : > "$M/boot/initrd.img-6.8.0-8-generic"
python3 "$BC" menu --root "$M" --running 6.8.0-8-generic >/dev/null 2>&1
grep -A1 -- "--id auros-previous" "$C" | grep -q "vmlinuz-6.8.0-8-generic" \
  && ok "the fallback is the kernel running now, when it is not the newest" \
  || bad "the fallback is the kernel running now, when it is not the newest" "$(grep -A1 -- '--id auros-previous' "$C")"
rm -f "$M/boot/vmlinuz-6.8.0-8-generic" "$M/boot/initrd.img-6.8.0-8-generic"
python3 "$BC" menu --root "$M" --running none >/dev/null 2>&1
mv "$M/etc/fstab" "$T/fstab"
python3 "$BC" menu --root "$M" --running none >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && grep -q "root=UUID=$U " "$C" \
  && ok "fstab without a / line: the UUID yesterday's menu booted" \
  || bad "fstab without a / line: the UUID yesterday's menu booted" "rc=$rc"
mv "$T/fstab" "$M/etc/fstab"
grep -q -- "--id put-windows-back-yes" "$C" && grep -q "bootmgfw.efi" "$C" \
  && ok "Put Windows back and the Windows entry are still there" \
  || bad "Put Windows back and the Windows entry are still there"
grep -q "@[A-Z_]*@" "$C" && bad "no placeholder left unfilled" "$(grep -o '@[A-Z_]*@' "$C" | sort -u)" \
  || ok "no placeholder left unfilled"
rm -f "$M/boot/vmlinuz-6.8.0-9-generic"
python3 "$BC" menu --root "$M" --running none >/dev/null 2>&1
if grep -q "fallback\|auros-previous" "$C"; then bad "one kernel: no fallback to a kernel that is not there"
else ok "one kernel: no fallback to a kernel that is not there"; fi
cp "$C" "$T/good.cfg"
mv "$M/boot/initrd.img-6.8.0-10-generic" "$T/"
python3 "$BC" menu --root "$M" --running none >/dev/null 2>&1; rc=$?
[ "$rc" != 0 ] && cmp -s "$C" "$T/good.cfg" \
  && ok "no kernel at all: refused, and yesterday's menu kept" \
  || bad "no kernel at all: refused, and yesterday's menu kept" "rc=$rc"
mv "$T/initrd.img-6.8.0-10-generic" "$M/boot/"
sed -i '/put-windows-back-yes/d' "$M/usr/lib/auros/grub.cfg.in"
python3 "$BC" menu --root "$M" --running none >/dev/null 2>&1; rc=$?
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

# Firmware whose variables cannot be read: no SecureBoot variable.
FWN="$T/fwn"; mkfw "$FWN" "$VARS"; rm -f "$FWN/SecureBoot-$G_GLB"

# Canonical's own signing certificate (2022 v1) -- one of the two
# signatures on the dual-signed shim -- in dbx. The other (Microsoft
# 2011) is still trusted; firmware refuses the image anyway.
pyc() { python3 -c "
src=open('$BC').read(); m={}
exec(compile(src.replace('if __name__ == \"__main__\":', 'if False:'), 'bc', 'exec'), m)
$1"; }
FWC="$T/fwc"; mkfw "$FWC" "$VARS"
pyc "
import struct
pe=m['PE'](open('$SHIM','rb').read())
s=m['Signature'](pe.signatures()[0], pe.authenticode_sha256())
d=s.signer.public_bytes(m['_DER'])
lst=bytes.fromhex('a159c0a5e494a74a87b5ab155c2bf072')+struct.pack('<III',28+16+len(d),0,16+len(d))+bytes(16)+d
open('$FWC/dbx-$G_SEC','ab').write(lst)
print(s.signer.subject.rfc4514_string())" > "$T/canon.subject"

# A shim signed by a key nobody trusts, carrying Microsoft's real 2011
# UEFI CA certificate in its bag -- the case a check that only looks
# at which certificates a signature CONTAINS lets through.
pyc "
d=open('$T/db.bin','rb').read(); import struct
X=bytes.fromhex('a159c0a5e494a74a87b5ab155c2bf072'); o=0
while o+28<=len(d):
    ls,hs,ss=struct.unpack_from('<III',d,o+16); p=o+28+hs
    while p+ss<=o+ls:
        b=d[p+16:p+ss]
        if d[o:o+16]==X and b'Corporation UEFI CA 2011' in b: open('$T/ms2011.der','wb').write(b)
        p+=ss
    o+=ls"
openssl x509 -inform DER -in "$T/ms2011.der" -out "$T/ms2011.pem" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/k.pem" -out "$T/self.pem" \
    -subj "/CN=Microsoft Windows UEFI Driver Publisher" -days 30 2>/dev/null
FORGED="$T/forged.efi"
osslsigncode sign -certs "$T/self.pem" -key "$T/k.pem" -ac "$T/ms2011.pem" -h sha256 \
    -in "$RFS/usr/lib/shim/shimx64.efi" -out "$FORGED" >/dev/null 2>&1
TAMPERED="$T/tampered.efi"; cp "$SHIM" "$TAMPERED"
printf 'X' | dd of="$TAMPERED" bs=1 seek=8192 conv=notrunc 2>/dev/null
# and a kernel signed by that key, for "will the new shim start it"
KREAL=$(ls "$RFS"/boot/vmlinuz-*-generic | head -1)
cp "$KREAL" "$T/k.unsigned"; sbattach --remove "$T/k.unsigned" 2>/dev/null
sbsign --key "$T/k.pem" --cert "$T/self.pem" --output "$T/k.forged" "$T/k.unsigned" >/dev/null 2>&1

# ── the verifier itself ─────────────────────────────────────────────
echo
echo "  the signature check, against OVMF's Microsoft db"
v() { pyc "
fw=m['firmware']('$1')
print(m['judge_image'](open('$2','rb').read(), fw['db'], fw['dbx'], fw['dbx_hashes'], fw['dbx_tbs']))"; }
[ "$(v "$FW" "$SHIM")" = None ] && ok "the real dual-signed shim: trusted" || bad "the real dual-signed shim: trusted" "$(v "$FW" "$SHIM")"
[ -s "$FORGED" ] && grep -q "UEFI CA 2011" "$T/ms2011.pem" 2>/dev/null || openssl x509 -in "$T/ms2011.pem" -noout -subject | grep -q "UEFI CA 2011" \
  && ok "fixture: a forged shim carrying Microsoft's 2011 CA was made" \
  || bad "fixture: a forged shim carrying Microsoft's 2011 CA was made"
case "$(v "$FW" "$FORGED")" in None) bad "the forged shim: refused" ;;
    *) ok "the forged shim: refused ($(v "$FW" "$FORGED" | cut -c1-40)...)" ;; esac
case "$(v "$FW" "$TAMPERED")" in None) bad "the real shim with one byte changed: refused" ;;
    *) ok "the real shim with one byte changed: refused" ;; esac
case "$(v "$FWC" "$SHIM")" in None) bad "one of two signatures revoked in dbx: the whole shim refused" "$(cat "$T/canon.subject")" ;;
    *) ok "one of two signatures revoked in dbx: the whole shim refused" ;; esac
case "$(v "$FWC" "$OLDSHIM")" in None) ok "...while the shim without that signature is still trusted" ;;
    *) bad "...while the shim without that signature is still trusted" "$(v "$FWC" "$OLDSHIM")" ;; esac
L=$(pyc "print(m['shim_levels'](m['PE'](open('$SHIM','rb').read())))")
[ "$L" = "({'shim': 2, 'grub': 3, 'grub.debian': 4}, {'shim': 4, 'grub': 3, 'grub.debian': 4})" ] \
  && ok "the shim's own SBAT levels are read (a '/N' section name)" \
  || bad "the shim's own SBAT levels are read (a '/N' section name)" "$L"
VC=$(pyc "v,_=m['shim_vendor'](m['PE'](open('$SHIM','rb').read())); print([c.subject.rfc4514_string() for c in m['certs_of'](v)])")
case "$VC" in *"Canonical Ltd. Master Certificate Authority"*) ok "...and its vendor certificate: Canonical's master CA" ;;
    *) bad "...and its vendor certificate: Canonical's master CA" "$VC" ;; esac

# ── sync ────────────────────────────────────────────────────────────
echo
echo "  sync"
# "yesterday's grub": the real one with a byte after its end, so it is
# a different file to replace (there is only one signed grub to hand).
cp "$GRUB" "$T/oldgrub.efi"; printf '\0' >> "$T/oldgrub.efi"
newesp() { # dir: an installed AurOS's EFI partition, one shim and one grub behind
    rm -rf "$1"; mkdir -p "$1/EFI/AurOS" "$1/EFI/BOOT"
    cp "$OLDSHIM" "$1/EFI/AurOS/shimx64.efi"; cp "$T/oldgrub.efi" "$1/EFI/AurOS/grubx64.efi"
    cp "$OLDSHIM" "$1/EFI/BOOT/BOOTX64.EFI";  cp "$T/oldgrub.efi" "$1/EFI/BOOT/grubx64.efi"
    ( cd "$1" && find . -type f -exec md5sum {} + | sort ) > "$1.md5"
}
same() { ( cd "$1" && find . -type f -exec md5sum {} + | sort ) | cmp -s - "$1.md5"; }
SR="$T/sroot"; mkdir -p "$SR/usr/lib/shim" "$SR/usr/lib/grub/x86_64-efi-signed" "$SR/var/lib/auros" "$SR/boot"
cp "$SHIM" "$OLDSHIM" "$SR/usr/lib/shim/"
cp "$RFS/usr/lib/shim/mmx64.efi" "$RFS/usr/lib/shim/fbx64.efi" "$SR/usr/lib/shim/" 2>/dev/null
cp "$GRUB" "$SR/usr/lib/grub/x86_64-efi-signed/"
# the restore kernel, where the installer leaves it on Windows' partition
STG="$T/winesp"; mkdir -p "$STG/EFI/AurOS"; cp "$KREAL" "$STG/EFI/AurOS/staging.efi"
ESP="$T/esp"
run() { python3 "$BC" sync --root "$SR" --esp "$ESP" --efivars "$1" --esp-roots "$STG"; }
candidate() { # file: what the archive offers as the newest shim
    cp "$1" "$SR/usr/lib/shim/shimx64.efi.dualsigned"
    cp "$1" "$SR/usr/lib/shim/shimx64.efi.signed.latest"
}
refused() { # name fw
    newesp "$ESP"
    out=$(run "$2" 2>&1); rc=$?
    if [ "$rc" = 3 ] && same "$ESP"; then ok "$1: refused, partition unchanged"
    else bad "$1: refused, partition unchanged" "rc=$rc" "$out"; fi
}
refused "the 2011 key removed from db (the 2023 change)" "$FW23"
refused "the 2011 CA revoked in dbx" "$FWX"
refused "the shim's own hash in dbx" "$FWH"
refused "the NVRAM SBAT level above this grub's generation" "$FWS"
refused "Canonical's signing certificate in dbx (one of two signatures)" "$FWC"
refused "Secure Boot variables that cannot be read" "$FWN"
grep -q "^result=refused" "$SR/var/lib/auros/bootchain.state" \
  && ok "...and the refusal is written down for status" \
  || bad "...and the refusal is written down for status"
candidate "$FORGED";    refused "a forged shim in the archive (Microsoft's CA in its bag)" "$FW"
candidate "$TAMPERED";  refused "a damaged shim in the archive" "$FW"
candidate "$SHIM"
cp "$T/k.forged" "$SR/boot/vmlinuz-6.8.0-1-generic"; : > "$SR/boot/initrd.img-6.8.0-1-generic"
refused "a kernel in the menu the new shim would refuse" "$FW"
rm -f "$SR/boot/vmlinuz-6.8.0-1-generic" "$SR/boot/initrd.img-6.8.0-1-generic"
cp "$T/k.forged" "$STG/EFI/AurOS/staging.efi"
refused "a Put Windows back kernel the new shim would refuse" "$FW"
cp "$KREAL" "$STG/EFI/AurOS/staging.efi"

cp "$KREAL" "$SR/boot/vmlinuz-6.8.0-1-generic"; : > "$SR/boot/initrd.img-6.8.0-1-generic"
newesp "$ESP"
out=$(run "$FW" 2>&1); rc=$?
if [ "$rc" = 0 ] && cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$SHIM" && \
   cmp -s "$ESP/EFI/BOOT/BOOTX64.EFI" "$SHIM" && cmp -s "$ESP/EFI/AurOS/grubx64.efi" "$GRUB"; then
    ok "trusted pair, real kernel, real restore kernel: installed, both directories"
else bad "trusted pair, real kernel, real restore kernel: installed, both directories" "rc=$rc" "$out"; fi
[ -f "$ESP/EFI/AurOS/mmx64.efi" ] && ok "...with MokManager beside it" || bad "...with MokManager beside it"
out=$(run "$FW" 2>&1); rc=$?
case "$out" in *"is current"*) [ "$rc" = 0 ] && ok "run again: nothing to do" || bad "run again: nothing to do" "rc=$rc";;
    *) bad "run again: nothing to do" "$out";; esac

newesp "$ESP"
out=$(AUROS_BOOTCHAIN_FAIL_AFTER=grubx64.efi run "$FW" 2>&1); rc=$?
if [ "$rc" = 9 ] && cmp -s "$ESP/EFI/AurOS/grubx64.efi" "$GRUB" && \
   cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$OLDSHIM"; then
    ok "stopped half way: grub went first, the old shim is still there"
else bad "stopped half way: grub went first, the old shim is still there" "rc=$rc" "$out"; fi
newesp "$ESP"
AUROS_BOOTCHAIN_FAIL_AFTER=shimx64.efi run "$FW" >/dev/null 2>&1
cmp -s "$ESP/EFI/BOOT/BOOTX64.EFI" "$OLDSHIM" && run "$FW" >/dev/null 2>&1 && \
    cmp -s "$ESP/EFI/BOOT/BOOTX64.EFI" "$SHIM" \
  && ok "stopped after \\EFI\\AurOS: the next run finishes \\EFI\\BOOT" \
  || bad "stopped after \\EFI\\AurOS: the next run finishes \\EFI\\BOOT"
newesp "$ESP"; : > "$ESP/EFI/AurOS/.bootchain.abc123"
run "$FW" >/dev/null 2>&1
[ ! -e "$ESP/EFI/AurOS/.bootchain.abc123" ] && ok "a power cut's half-written file is cleared" \
  || bad "a power cut's half-written file is cleared"

rm -rf "$T/none"; mkdir -p "$T/none"
out=$(python3 "$BC" sync --root "$SR" --esp "$T/none" --efivars "$FW" 2>&1); rc=$?
[ "$rc" = 0 ] && ok "no AurOS EFI partition (forge's chroot): nothing, and not an error" \
  || bad "no AurOS EFI partition (forge's chroot): nothing, and not an error" "rc=$rc" "$out"

# The wrapper dpkg runs: it must reach bootchain (a trusted pair goes
# in), and a refusal must not fail the package.
W="$T/grub-install"
wrap() { sed "s#/usr/lib/auros/bootchain sync#python3 $PWD/$BC sync --root $SR --esp $ESP --efivars $1 --esp-roots $STG#" \
    rootfs/usr/lib/auros/grub-install > "$W"; }
newesp "$ESP"; wrap "$FW"
sh "$W" --target=x86_64-efi >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$SHIM" \
  && ok "as grub-install under dpkg: it reaches bootchain, which installs" \
  || bad "as grub-install under dpkg: it reaches bootchain, which installs" "rc=$rc"
newesp "$ESP"; wrap "$FW23"
sh "$W" --target=x86_64-efi >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && same "$ESP" \
  && ok "...and a refusal exits 0, changing nothing" \
  || bad "...and a refusal exits 0, changing nothing" "rc=$rc"
newesp "$ESP"; wrap "$FW"
sh "$W" --help >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] && same "$ESP" \
  && ok "...and grub-install --help (a postinst's probe) syncs nothing" \
  || bad "...and grub-install --help (a postinst's probe) syncs nothing" "rc=$rc"

# ── proving the checks are not decorative ───────────────────────────
echo
echo "  each check, switched off, must let its bad case through"
mutant() { # name, from, to, fw, candidate
    sed "s|$2|$3|" "$BC" > "$T/mut"
    cmp -s "$T/mut" "$BC" && { bad "$1" "the mutation matched nothing"; return; }
    candidate "${5:-$SHIM}"
    newesp "$ESP"
    python3 "$T/mut" sync --root "$SR" --esp "$ESP" --efivars "$4" --esp-roots "$STG" >/dev/null 2>&1
    if cmp -s "$ESP/EFI/AurOS/shimx64.efi" "${5:-$SHIM}"; then ok "$1"
    else bad "$1" "the bad case was still refused: the test does not test the check"; fi
    candidate "$SHIM"
}
mutant "no db chain: the 2023-only firmware gets the shim" \
       'if path_to(s.signer, s.bag, anchors):' 'if True:' "$FW23"
mutant "no digest check: a damaged shim gets through" \
       'if inner(p7, dig) != image_digest:' 'if False:' "$FW" "$TAMPERED"
mutant "signer = any certificate in the bag: the forged shim gets through" \
       'if path_to(s.signer, s.bag, anchors):' 'if any(path_to(c, s.bag, anchors) for c in s.bag):' "$FW" "$FORGED"
mutant "no dbx-hash check: a revoked hash gets through" \
       'if digest in deny_hashes:' 'if False:' "$FWH"
mutant "no dbx-certificate check: Canonical revoked, shim through anyway" \
       'if path_to(s.signer, s.bag, deny_certs, deny_tbs) is not None:' 'if False:' "$FWC"
mutant "no SBAT check: a revoked grub gets through" \
       'r = revoked_by(sb, level)' 'r = []' "$FWS"

# ── the hash dbx is compared with ───────────────────────────────────
echo
echo "  Authenticode"
O=$(osslsigncode verify -in "$OLDSHIM" 2>&1 | sed -n 's/^Calculated message digest *: *\([0-9A-F]*\).*/\1/p' | head -1 | tr 'A-F' 'a-f')
M2=$(pyc "print(m['PE'](open('$OLDSHIM','rb').read()).authenticode_sha256().hex())")
[ -n "$O" ] && [ "$O" = "$M2" ] && ok "matches osslsigncode's ($O)" \
  || bad "matches osslsigncode's" "osslsigncode=$O" "bootchain=$M2"

echo
echo "bootchaintest: $((checked - fail))/$checked"
[ "$fail" = 0 ]
