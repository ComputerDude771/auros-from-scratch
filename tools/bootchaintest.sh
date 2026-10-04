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
awk '/set default="\$\{next_entry\}"/{d=1} d && /^fi/{exit} d && /unset fallback/{f=1} END{exit !f}' "$C" \
  && awk '/^set fallback=/{s=NR} /set default="\$\{next_entry\}"/{n=NR} END{exit !(s && n && s < n)}' "$C" \
  && ok "...but not for Put Windows back: that start unsets it" \
  || bad "...but not for Put Windows back: that start unsets it"
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
if grep -q "^set fallback=\|--id auros-previous" "$C"; then bad "one kernel: no fallback to a kernel that is not there"
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
src=open('${B:-$BC}').read(); m={}
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
FB=$(pyc "
pe=m['PE'](open('$FORGED','rb').read()); s=m['Signature'](pe.signatures()[0], pe.authenticode_sha256())
print(s.signer is not None, any('UEFI CA 2011' in c.subject.rfc4514_string() for c in s.bag))" 2>&1)
[ "$FB" = "True True" ] \
  && ok "fixture: a forged shim, validly signed, carrying Microsoft's 2011 CA" \
  || bad "fixture: a forged shim, validly signed, carrying Microsoft's 2011 CA" "$FB"
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
shim_kept() { # name fw: the shim refused, the grub the old shim starts goes in alone
    newesp "$ESP"
    out=$(run "$2" 2>&1); rc=$?
    if [ "$rc" = 0 ] && cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$OLDSHIM" && \
       cmp -s "$ESP/EFI/BOOT/BOOTX64.EFI" "$OLDSHIM" && \
       cmp -s "$ESP/EFI/AurOS/grubx64.efi" "$GRUB" && cmp -s "$ESP/EFI/BOOT/grubx64.efi" "$GRUB" && \
       grep -q "^result=updated-grub-only" "$SR/var/lib/auros/bootchain.state"; then
        ok "$1: shim refused, the new grub alone installed"
    else bad "$1: shim refused, the new grub alone installed" "rc=$rc" "$out"; fi
}
candidate "$FORGED";    shim_kept "a forged shim in the archive (Microsoft's CA in its bag)" "$FW"
candidate "$TAMPERED";  shim_kept "a damaged shim in the archive" "$FW"
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
grep -q "^result=current" "$SR/var/lib/auros/bootchain.state" && \
  grep -q "^last_update=" "$SR/var/lib/auros/bootchain.state" && \
  grep -q "^judged=grub; .*Put Windows back kernel" "$SR/var/lib/auros/bootchain.state" \
  && ok "...and status still says when it last updated, and what it judged" \
  || bad "...and status still says when it last updated, and what it judged" "$(cat "$SR/var/lib/auros/bootchain.state")"

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

# ── the restore kernel, looked for the way an installed AurOS does ──
# Every other case names a directory (--esp-roots). This one gives the
# scan a real disk -- a loop device with an EFI partition -- so the
# read-only mount and the refusal when it fails run for real.
echo
echo "  the Put Windows back kernel, found on a real EFI partition"
WD="$T/windisk.img"
truncate -s 64M "$WD"
sgdisk -n1:2048:+40M -t1:EF00 "$WD" >/dev/null
LD=$(losetup --find --show -P "$WD" 2>/dev/null) && { [ -b "${LD}p1" ] || partx -a "$LD" 2>/dev/null; }
if [ -z "$LD" ] || [ ! -b "${LD}p1" ]; then
    bad "a loop device with partitions for the scan" "losetup -P failed"
else
    mkfs.vfat -F 32 "${LD}p1" >/dev/null
    export MTOOLS_SKIP_CHECK=1
    mmd -i "${LD}p1" ::/EFI ::/EFI/AurOS 2>/dev/null
    scan() { python3 "$BC" sync --root "$SR" --esp "$ESP" --efivars "$FW" --esp-scan "$LD"; }
    mcopy -o -i "${LD}p1" "$KREAL" ::/EFI/AurOS/staging.efi 2>/dev/null
    md5sum "${LD}p1" > "$T/p1.md5"
    if grep -qw vfat /proc/filesystems || modprobe vfat 2>/dev/null; then
        mcopy -o -i "${LD}p1" "$T/k.forged" ::/EFI/AurOS/staging.efi 2>/dev/null
        newesp "$ESP"; out=$(scan 2>&1); rc=$?
        case "$out" in *"refuse the Put Windows back kernel"*)
            [ "$rc" = 3 ] && same "$ESP" && ! grep -q "auros-esp" /proc/mounts \
              && ok "a restore kernel the new shim would refuse, found by mounting: refused" \
              || bad "a restore kernel the new shim would refuse, found by mounting: refused" "rc=$rc" ;;
            *) bad "a restore kernel the new shim would refuse, found by mounting: refused" "$out" ;; esac
        mcopy -o -i "${LD}p1" "$KREAL" ::/EFI/AurOS/staging.efi 2>/dev/null
        md5sum "${LD}p1" > "$T/p1.md5"
        newesp "$ESP"; out=$(scan 2>&1); rc=$?
        [ "$rc" = 0 ] && cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$SHIM" && ! grep -q "auros-esp" /proc/mounts \
          && ok "the real one: installed, and the partition unmounted again" \
          || bad "the real one: installed, and the partition unmounted again" "rc=$rc" "$out"
    else
        # This kernel cannot mount FAT at all (a container). What can be
        # shown here is the half that matters most: a partition that
        # cannot be looked inside refuses the update. The finding and
        # judging of a real one runs inside AurOS, in bootupdatetest.
        echo "    (this kernel has no vfat: mounting is proven in tools/bootupdatetest.sh)"
        newesp "$ESP"; out=$(scan 2>&1); rc=$?
        case "$out" in *"could not look inside ${LD}p1"*)
            [ "$rc" = 3 ] && same "$ESP" \
              && ok "an EFI partition it cannot mount: refused, not taken as empty" \
              || bad "an EFI partition it cannot mount: refused, not taken as empty" "rc=$rc" ;;
            *) bad "an EFI partition it cannot mount: refused, not taken as empty" "$out" ;; esac
    fi
    md5sum -c --quiet "$T/p1.md5" && ok "...and Windows' partition was only read" \
      || bad "...and Windows' partition was only read"
    dd if=/dev/zero of="${LD}p1" bs=512 count=1 conv=notrunc 2>/dev/null
    newesp "$ESP"; out=$(scan 2>&1); rc=$?
    [ "$rc" = 3 ] && same "$ESP" \
      && ok "a damaged EFI partition: refused, not taken as empty" \
      || bad "a damaged EFI partition: refused, not taken as empty" "rc=$rc" "$out"
    losetup -d "$LD"
fi

# ── more fixtures, one per check ────────────────────────────────────
# dbx listing the 2011 CA by the SHA-256 of its TBS (EFI_CERT_X509_SHA256)
FWT="$T/fwt"; mkfw "$FWT" "$VARS"
pyc "
import struct, hashlib
c=m['load_cert'](open('$T/ms2011.der','rb').read())
h=hashlib.sha256(c.tbs_certificate_bytes).digest()
G=bytes.fromhex('92a4d23bc0967940b420fcf98ef103ed')
open('$FWT/dbx-$G_SEC','ab').write(G+struct.pack('<III',28+64,0,64)+bytes(16)+h+bytes(16))"
# NVRAM already above this shim's own generation
FWL="$T/fwl"; mkfw "$FWL" "$VARS"
printf 'sbat,1,2099010100\nshim,99\n' > "$T/lvl2"; efi "$FWL" SbatLevelRT $G_SHIM "$T/lvl2"

# Microsoft's real signature on a changed program, its digests rewritten
# to match: GRAFT_RSA has the messageDigest rewritten too (only the RSA
# signature is wrong); GRAFT_MD leaves it (the signed attributes still
# verify, but no longer describe what is signed).
pyc "
import struct, hashlib
real=bytearray(open('$OLDSHIM','rb').read()); p=m['PE'](bytes(real))
real[8192]^=1
nd=m['PE'](bytes(real)).authenticode_sha256()
ln,=struct.unpack_from('<I',real,p.cert_off)
P=bytes(real[p.cert_off+8:p.cert_off+ln]); p7=bytearray(P)
ci=m['children'](P,0); sd=m['children'](P,ci[1])[0]; parts=m['children'](P,sd)
spc=m['children'](P,m['children'](P,parts[2])[1])[0]
di=m['children'](P,spc)[1]; alg,dig=m['children'](P,di)
t,h,n=m['der'](P,dig); p7[dig+h:dig+h+n]=nd
oldmd=hashlib.sha256(m['inner'](P,spc)).digest()
real[p.cert_off+8:p.cert_off+ln]=p7
open('$T/graft_md.efi','wb').write(bytes(real))
P2=bytes(p7); newmd=hashlib.sha256(m['inner'](P2,spc)).digest()
i=P2.find(oldmd); assert i>0 and P2.count(oldmd)==1
p7[i:i+32]=newmd; real[p.cert_off+8:p.cert_off+ln]=p7
open('$T/graft_rsa.efi','wb').write(bytes(real))"

# A certificate table EDK2 would not walk (eight zero bytes after the
# last entry), and a second, broken SignerInfo beside the real one.
pyc "
import struct
real=open('$OLDSHIM','rb').read(); p=m['PE'](real)
table=real[p.cert_off:p.cert_off+p.cert_size]
def with_table(tb):
    o=bytearray(real[:p.cert_off]+tb); struct.pack_into('<II',o,p.certdir_off,p.cert_off,len(tb)); return bytes(o)
open('$T/tablepad.efi','wb').write(with_table(table+bytes(8)))
def tlv(tag,body):
    n=len(body)
    ln=bytes([n]) if n<0x80 else (lambda b: bytes([0x80|len(b)])+b)(n.to_bytes((n.bit_length()+7)//8,'big'))
    return bytes([tag])+ln+body
ln,rev,typ=struct.unpack_from('<IHH',table,0); p7=table[8:ln]
C=m['children']; W=m['whole']; I=m['inner']
ci=C(p7,0); sd=C(p7,ci[1])[0]; parts=C(p7,sd); sis=parts[-1]
si=bytearray(W(p7,C(p7,sis)[0])); si[-5]^=0xff
body=b''.join(W(p7,x) for x in parts[:-1])+tlv(0x31,I(p7,sis)+bytes(si))
np7=tlv(0x30,W(p7,ci[0])+tlv(0xA0,tlv(0x30,body)))
wc=struct.pack('<IHH',8+len(np7),rev,typ)+np7; wc+=bytes((8-len(wc)%8)%8)
open('$T/twosi.efi','wb').write(with_table(wc))"

# A small PKI: a root; an intermediate that is a CA (the control) and one
# that is not; a signer with an unknown CRITICAL extension.
K="$T/pki"; mkdir -p "$K"
cat > "$K/ext" <<'EOX'
[ca]
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
[notca]
basicConstraints=critical,CA:FALSE
[leaf]
basicConstraints=critical,CA:FALSE
extendedKeyUsage=codeSigning
[weird]
basicConstraints=critical,CA:FALSE
extendedKeyUsage=codeSigning
1.3.6.1.4.1.99999.1=critical,ASN1:NULL
EOX
mkcert() { # name subject issuer-name extsection
    openssl req -new -newkey rsa:2048 -nodes -keyout "$K/$1.key" -out "$K/$1.csr" -subj "/CN=$2" 2>/dev/null
    if [ "$3" = self ]; then
        openssl x509 -req -in "$K/$1.csr" -signkey "$K/$1.key" -out "$K/$1.pem" -days 30 \
            -extfile "$K/ext" -extensions "$4" 2>/dev/null
    else
        openssl x509 -req -in "$K/$1.csr" -CA "$K/$3.pem" -CAkey "$K/$3.key" -CAcreateserial \
            -out "$K/$1.pem" -days 30 -extfile "$K/ext" -extensions "$4" 2>/dev/null
    fi
}
mkcert root "Test Root" self ca
mkcert ica "Test Intermediate CA" root ca
mkcert inot "Test Intermediate Not A CA" root notca
mkcert lp "Test Signer" ica leaf
mkcert ln "Test Signer Under A Leaf" inot leaf
mkcert lw "Test Signer Weird" root weird
openssl x509 -in "$K/root.pem" -outform DER -out "$K/root.der"
pkisign() { osslsigncode sign -certs "$K/$1.pem" -key "$K/$1.key" ${2:+-ac "$K/$2.pem"} -h sha256 \
    -in "$RFS/usr/lib/shim/shimx64.efi" -out "$T/pki_$1.efi" >/dev/null 2>&1; }
pkisign lp ica; pkisign ln inot; pkisign lw

# ── each check: its bad case is refused, and with the check removed it is not ──
echo
echo "  every check refuses its own bad case -- and, switched off, does not"
# Each case prints "refused" or "accepted"; anything else (a traceback)
# is neither and fails both ways.
js() { pyc "
fw=m['firmware']('$1'); w=m['judge_shim'](open('$2','rb').read(), fw)
print('accepted' if w is None else 'refused')" 2>/dev/null; }
jl() { pyc "
$3
fw=m['firmware']('$1'); loads=$4
w=m['judge_loads'](open('$SHIM','rb').read(), open('$2','rb').read(), fw, loads)
print('accepted' if w is None else 'refused')" 2>/dev/null; }
jroot() { pyc "
r=m['load_cert'](open('$K/root.der','rb').read())
w=m['judge_image'](open('$1','rb').read(), [r], [], set(), set())
print('accepted' if w is None else 'refused')" 2>/dev/null; }
KL="{'k': open('$KREAL','rb').read()}"
c_db23()     { js "$FW23" "$SHIM"; }
c_digest()   { js "$FW" "$TAMPERED"; }
c_forged()   { js "$FW" "$FORGED"; }
c_rsa()      { js "$FW" "$T/graft_rsa.efi"; }
c_md()       { js "$FW" "$T/graft_md.efi"; }
c_dbxhash()  { js "$FWH" "$SHIM"; }
c_dbxcert()  { js "$FWC" "$SHIM"; }
c_dbxtbs()   { js "$FWT" "$SHIM"; }
c_shimsbat() { js "$FWL" "$SHIM"; }
c_pad()      { js "$FW" "$T/tablepad.efi"; }
c_twosi()    { js "$FW" "$T/twosi.efi"; }
c_notca()    { jroot "$T/pki_ln.efi"; }
c_crit()     { jroot "$T/pki_lw.efi"; }
c_grubsbat() { jl "$FWS" "$GRUB" "" "{}"; }
c_nosbat()   { jl "$FW" "$KREAL" "" "{}"; }
c_merge()    { jl "$FW" "$GRUB" "m['shim_levels']=lambda pe: ({}, {'grub': 99})" "{}"; }
c_nolevel()  { jl "$FW" "$GRUB" "m['shim_levels']=lambda pe: ({}, {})" "{}"; }
c_vdbxcert() { jl "$FW" "$GRUB" "
pe=m['PE'](open('$KREAL','rb').read()); s=m['Signature'](pe.signatures()[0], pe.authenticode_sha256())
real=m['shim_vendor']; d=s.signer.public_bytes(m['_DER'])
m['shim_vendor']=lambda p: (real(p)[0], ([d], set(), set()))" "$KL"; }
c_vdbxhash() { jl "$FW" "$GRUB" "
h=m['PE'](open('$KREAL','rb').read()).authenticode_sha256(); real=m['shim_vendor']
m['shim_vendor']=lambda p: (real(p)[0], ([], {h}, set()))" "$KL"; }
c_fallback() {
    F="$T/fb"; rm -rf "$F"; mkdir -p "$F/boot/grub" "$F/etc" "$F/usr/lib/auros"
    awk '/^menuentry .AurOS. /{print "if true ; then\nmenuentry '"'"'early'"'"' --id early { true }\nfi"} {print}' \
        rootfs/usr/lib/auros/grub.cfg.in > "$F/usr/lib/auros/grub.cfg.in"
    cp "$M/etc/fstab" "$F/etc/"
    for v in 6.8.0-1-generic 6.8.0-2-generic; do : > "$F/boot/vmlinuz-$v"; : > "$F/boot/initrd.img-$v"; done
    python3 "${B:-$BC}" menu --root "$F" --running none >/dev/null 2>&1 && echo accepted || echo refused; }
c_previous() {
    newesp "$ESP"; candidate "$SHIM"
    ln -sf /etc/alternatives/shimx64.efi.signed "$SR/usr/lib/shim/shimx64.efi.signed"
    mkdir -p "$SR/etc/alternatives"; ln -sf /usr/lib/shim/shimx64.efi.signed.previous "$SR/etc/alternatives/shimx64.efi.signed"
    cp "$RFS/usr/lib/shim/shimx64.efi.signed.previous" "$SR/usr/lib/shim/"
    B="${B:-$BC}" python3 "${B:-$BC}" sync --root "$SR" --esp "$ESP" --efivars "$FW" --esp-roots "$STG" >/dev/null 2>&1
    cmp -s "$ESP/EFI/AurOS/shimx64.efi" "$SHIM" && echo accepted || echo refused
    rm -f "$SR/usr/lib/shim/shimx64.efi.signed" "$SR/etc/alternatives/shimx64.efi.signed" "$SR/usr/lib/shim/shimx64.efi.signed.previous"; }
c_older() {
    newesp "$ESP"; candidate "$SHIM"
    cp "$KREAL" "$SR/boot/vmlinuz-6.8.0-1-generic"
    cp "$T/k.forged" "$SR/boot/vmlinuz-6.8.0-0-generic"; : > "$SR/boot/initrd.img-6.8.0-0-generic"
    python3 "${B:-$BC}" sync --root "$SR" --esp "$ESP" --efivars "$FW" --esp-roots "$STG" >/dev/null 2>&1
    same "$ESP" && echo refused || echo accepted
    rm -f "$SR/boot/vmlinuz-6.8.0-0-generic" "$SR/boot/initrd.img-6.8.0-0-generic"; }

[ "$(jroot "$T/pki_lp.efi")" = accepted ] \
  && ok "control: a proper root -> CA -> signer chain is accepted" \
  || bad "control: a proper root -> CA -> signer chain is accepted" "$(jroot "$T/pki_lp.efi")"
[ "$(js "$FW" "$OLDSHIM")" = accepted ] && [ "$(jl "$FW" "$GRUB" "" "$KL")" = accepted ] \
  && ok "control: the real shim, grub and kernel are accepted" \
  || bad "control: the real shim, grub and kernel are accepted"

check() { # case, name, from, to
    r=$($1)
    if [ "$r" != refused ]; then bad "$2" "the real code said: ${r:-nothing (a traceback?)}"; return; fi
    d=$(printf '\001')
    sed "s$d$3$d$4$d" "$BC" > "$T/mut"
    if cmp -s "$T/mut" "$BC"; then bad "$2" "the mutation matched nothing"; return; fi
    r=$(B="$T/mut" $1)
    if [ "$r" = accepted ]; then ok "$2"
    else bad "$2" "with the check removed it still said: ${r:-nothing}"; fi
}
check c_db23     "firmware without the 2011 key" \
      'if path_to(s.signer, s.bag, anchors):' 'if True:'
check c_digest   "a changed byte: the file's digest is not the signed one" \
      'if inner(p7, dig) != image_digest:' 'if False:'
check c_forged   "a signer nobody trusts, Microsoft's CA in its bag" \
      'if path_to(s.signer, s.bag, anchors):' 'if any(path_to(c, s.bag, anchors) for c in s.bag):'
check c_rsa      "Microsoft's signature grafted onto a changed program (RSA)" \
      'key.verify(sig, signed, padding.PKCS1v15(), hashes.SHA256())' 'pass'
check c_md       "...with its messageDigest left stale" \
      'if md != hashlib.sha256(inner(p7, spc)).digest():' 'if False:'
check c_dbxhash  "the shim's hash in dbx" \
      'if digest in deny_hashes:' 'if False:'
check c_dbxcert  "one of two signers in dbx" \
      'if path_to(s.signer, s.bag, deny_certs, deny_tbs) is not None:' 'if False:'
check c_dbxtbs   "the 2011 CA in dbx by its TBS hash" \
      'if hashlib.sha256(cur.tbs_certificate_bytes).digest() in anchor_hashes:' 'if False:'
check c_shimsbat "NVRAM's SBAT level above the shim's own generation" \
      'r = revoked_by(PE(shim_data).sbat(), fw\["sbat"\])' 'r = []'
check c_pad      "a certificate table EDK2 would not walk" \
      'if o != end:' 'if False:'
check c_twosi    "a second, broken SignerInfo" \
      'if len(sis) != 1:' 'if False:'
check c_notca    "a signer under a certificate that is not a CA" \
      'if not bc.ca:' 'if False:'
check c_crit     "a signer with an unknown critical extension" \
      'if e.critical and type(e.value) not in KNOWN_EXT:' 'if False:'
check c_grubsbat "NVRAM's SBAT level revokes the grub" \
      'r = revoked_by(sb, level)' 'r = []'
check c_nosbat   "a grub with no .sbat section" \
      'if name == "grub" and not sb:' 'if False:'
check c_merge    "the new shim's own latest level revokes the grub" \
      'level\[k\] = max(level.get(k, 0), v)' 'pass'
check c_nolevel  "a shim whose level cannot be read" \
      'return "the shim has no SBAT level this program can read"' 'pass'
check c_vdbxcert "the shim's vendor dbx revokes the kernel's signer" \
      'deny = fw\["dbx"\] + certs_of(vx_c)' 'deny = fw["dbx"]'
check c_vdbxhash "the shim's vendor dbx lists the kernel's hash" \
      'deny_h = fw\["dbx_hashes"\] | vx_h' 'deny_h = fw["dbx_hashes"]'
check c_older    "the fallback kernel, not only the newest, is judged" \
      'for v in ks\[:2\] + \[os.uname().release\]:' 'for v in ks[:1]:'
check c_previous "Ubuntu chose .previous: the newer shim is not installed" \
      'return \[os.path.join(root, SHIM_PREVIOUS)\]' 'pass'
check c_fallback "an entry under an if before the fallback: menu refused" \
      'raise ValueError("a conditional menu entry comes before auros-previous")' 'pass'

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
