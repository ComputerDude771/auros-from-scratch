#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  nmtest — is she online on the first start, without doing anything?
#
#  Asked of the image's OWN NetworkManager (1.46 on noble), run from the
#  built rootfs through the rootfs's own loader: no root, no chroot, no
#  daemon, nothing written into the rootfs.
#
#  1. The configuration the next image will have -- the rootfs's
#     /etc/NetworkManager/conf.d with this repository's rootfs/ laid
#     over it, the way build/forge lays it -- merged by NetworkManager
#     itself (--print-config):
#       - the cable is managed. Ubuntu's 10-globally-managed-devices.conf
#         leaves everything but wifi and mobile unmanaged unless netplan
#         says otherwise, and this image has no netplan: a PC on a cable
#         was offline.
#       - names can be looked up. Ubuntu's 10-dns-resolved.conf hands DNS
#         to systemd-resolved, which this image does not install.
#  2. Her wifi, as the installer carries it across (D2): a keyfile with
#     no permissions= (so it belongs to the machine, not to a login
#     session that does not exist at boot) and autoconnect on. Written
#     by Ferry's own emitter, and read back by `nmcli --offline`, which
#     is NetworkManager's keyfile parser and normaliser. Two keyfiles
#     that are wrong on purpose prove the checks can see the difference.
#
#  NetworkManager also ignores a keyfile that is not root's or that
#  anybody else can read (nm_utils_file_is_private); 0600 root:root is
#  the writer's job and is checked where the writer is tested.
#
#    sh tools/nmtest.sh            (RFS=<rootfs>, default work/forge/desktop/rootfs)
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
RFS="${RFS:-work/forge/desktop/rootfs}"
NM="$RFS/usr/sbin/NetworkManager"
LDSO="$RFS/lib64/ld-linux-x86-64.so.2"
if [ ! -x "$NM" ] || [ ! -x "$LDSO" ]; then
    echo "no built rootfs with NetworkManager at $RFS -- nothing to check."
    exit 2
fi
LIBS="$RFS/usr/lib/x86_64-linux-gnu:$RFS/lib/x86_64-linux-gnu"
run_nm()    { "$LDSO" --library-path "$LIBS" "$NM" "$@"; }
run_nmcli() { "$LDSO" --library-path "$LIBS" "$RFS/usr/bin/nmcli" "$@"; }

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '  %-64s ok\n' "$1"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '  %-64s FAIL\n' "$1"; }
check() { l=$1; shift; if "$@" >/dev/null 2>&1; then ok "$l"; else bad "$l"; fi; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/nmtest.XXXXXX") || exit 2
trap 'rm -rf "$TMP"' EXIT

echo "NetworkManager $(run_nm --version 2>/dev/null), from $RFS"
echo
echo "  the configuration (the image's, with rootfs/ laid over it)"
mkdir -p "$TMP/conf.d"
cp "$RFS"/etc/NetworkManager/conf.d/*.conf "$TMP/conf.d/" 2>/dev/null
cp rootfs/etc/NetworkManager/conf.d/*.conf "$TMP/conf.d/" 2>/dev/null
run_nm --print-config --config="$RFS/etc/NetworkManager/NetworkManager.conf" \
       --config-dir="$TMP/conf.d" \
       --system-config-dir="$RFS/usr/lib/NetworkManager/conf.d" > "$TMP/conf" 2>&1
check "NetworkManager reads it"         grep -q '^\[main\]' "$TMP/conf"
check "no device type is left unmanaged (the cable works)" \
    sh -c "! grep -q '^unmanaged-devices=' '$TMP/conf'"
if [ -x "$RFS/usr/lib/systemd/systemd-resolved" ]; then
    ok "systemd-resolved is in the image, so dns=systemd-resolved is fine"
else
    check "DNS is not handed to a systemd-resolved that is not there" \
        sh -c "! grep -q '^dns=systemd-resolved' '$TMP/conf'"
    check "NetworkManager writes /etc/resolv.conf itself" grep -q '^rc-manager=file' "$TMP/conf"
fi

echo
echo "  her wifi, carried across from Windows"
# Ferry's emitter and its uuid, taken out of the scripts rather than copied.
eval "$(sed -n '/^stable_uuid() {/,/^}/p' src/ferry/ferry-common.sh)"
eval "$(sed -n '/^emit_nmconnection() {/,/^}/p' src/ferry/ferry-wifi)"
emit_nmconnection "Grandma's Wi-Fi" wpa-psk 'correct horse battery staple' 0 true > "$TMP/home.nmconnection"
norm() { run_nmcli --offline connection modify connection.zone '' < "$1" > "$2" 2>&1; }
norm "$TMP/home.nmconnection" "$TMP/home.out"
check "NetworkManager accepts the keyfile"        grep -q '^type=wifi' "$TMP/home.out"
check "it belongs to the machine (no permissions=)" sh -c "! grep -q '^permissions=' '$TMP/home.out'"
check "it connects by itself (autoconnect is not off)" sh -c "! grep -q '^autoconnect=false' '$TMP/home.out'"
check "the password is in it, for a connection at boot with nobody there" \
    grep -q '^psk=correct horse battery staple$' "$TMP/home.out"

# The checks above have to be able to fail. Two keyfiles wrong on purpose:
sed 's/^type=wifi$/type=wifi\npermissions=user:auros;/' "$TMP/home.nmconnection" > "$TMP/mine.nmconnection"
norm "$TMP/mine.nmconnection" "$TMP/mine.out"
check "(a keyfile tied to one login IS seen as such)" grep -q '^permissions=user:auros' "$TMP/mine.out"
sed 's/^autoconnect=true$/autoconnect=false/' "$TMP/home.nmconnection" > "$TMP/off.nmconnection"
norm "$TMP/off.nmconnection" "$TMP/off.out"
check "(a keyfile with autoconnect off IS seen as such)" grep -q '^autoconnect=false' "$TMP/off.out"
printf '[connection]\nid=broken\ntype=wifi\n' > "$TMP/bad.nmconnection"
norm "$TMP/bad.nmconnection" "$TMP/bad.out"
check "(and a broken one is refused)" grep -qi 'error' "$TMP/bad.out"

echo
if [ "$fail" -gt 0 ]; then echo "$fail of $checked wrong."; exit 1; fi
echo "$checked checks: the cable and her wifi both connect at boot, and names resolve."
