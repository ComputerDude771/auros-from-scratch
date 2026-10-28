#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  hardentest — is every hardening and recovery control in this rootfs?
#
#  Static: reads a built AurOS root filesystem and checks each control
#  in docs/security/HARDENING.md is present AND says what it should --
#  the effective sysctl value after every sysctl.d file is read in
#  systemd's order, every kernel line in the boot menu, the firewall's
#  accept rules against an allow-list, the units actually enabled, the
#  printing listener actually gone. It also runs the two recovery
#  scripts the image ships (shell-guard, dpkg-repair) against scratch
#  state, so what is tested is the shipped file, not a copy.
#
#  build/harden runs this at the seal and stops the build on a FAIL.
#  tools/hardentest-mutants.sh proves every check fails when the thing
#  it checks is removed or weakened. tools/hardenboottest.sh is the
#  same controls on a booted image.
#
#    RFS=work/forge/desktop/rootfs sh tools/hardentest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
RFS="${RFS:-work/forge/desktop/rootfs}"
[ -d "$RFS/etc" ] || { echo "no rootfs at $RFS (set RFS=)"; exit 2; }

fail=0; checked=0
ok()  { checked=$((checked+1)); [ -n "${HARDEN_QUIET:-}" ] || printf '    %-62s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-62s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }
say() { [ -n "${HARDEN_QUIET:-}" ] || printf '%s\n' "$*"; }

T=$(mktemp -d "${TMPDIR:-/tmp}/hardentest.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT

# A path inside the rootfs, following symlinks (absolute ones are
# relative to the rootfs, not to this machine).
inroot() {
    p="$1"; n=0
    while [ -L "$RFS$p" ] && [ $n -lt 20 ]; do
        l=$(readlink "$RFS$p")
        case "$l" in /*) p="$l" ;; *) p="$(dirname "$p")/$l" ;; esac
        n=$((n+1))
    done
    printf '%s%s' "$RFS" "$p"
}
enabled() { # target unit -> 0 if the enable link exists
    [ -L "$RFS/etc/systemd/system/$1.wants/$2" ] || [ -e "$RFS/etc/systemd/system/$1.wants/$2" ] \
    || [ -L "$RFS/etc/systemd/system/$1.requires/$2" ]
}
installed() { # package -> 0 if dpkg says it is installed
    awk -v p="$1" '
        /^Package: / { pk = $2 }
        /^Status: / && pk == p && $0 ~ / installed$/ { found = 1 }
        END { exit !found }' "$RFS/var/lib/dpkg/status" 2>/dev/null
}

# ── 1. the kernel's settings, as systemd-sysctl will leave them ──────
#
# systemd-sysctl reads every *.conf in these directories, a name in an
# earlier directory hiding the same name in a later one, all in order
# of file NAME; the last assignment of a key wins.
say ""
say "Kernel settings (sysctl), as the machine will have them"
for d in /etc/sysctl.d /run/sysctl.d /usr/local/lib/sysctl.d /usr/lib/sysctl.d; do
    [ -d "$RFS$d" ] || continue
    for f in "$RFS$d"/*.conf; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        grep -qxF "$b" "$T/seen" 2>/dev/null && continue
        echo "$b" >> "$T/seen"
        printf '%s\t%s\n' "$b" "$d/$b" >> "$T/files"
    done
done
sort "$T/files" 2>/dev/null | while IFS="$(printf '\t')" read -r b p; do
    sed -e 's/#.*//' -e 's/;.*//' "$(inroot "$p")" 2>/dev/null | awk -F= '
        NF >= 2 { k = $1; sub(/^[ \t-]+/, "", k); sub(/[ \t]+$/, "", k); gsub("/", ".", k)
                  v = substr($0, index($0, "=") + 1); sub(/^[ \t]+/, "", v); sub(/[ \t]+$/, "", v)
                  if (k != "") print k "=" v }'
done > "$T/assign"
eff() { awk -F= -v k="$1" '$1 == k { v = substr($0, length(k) + 2) } END { print v }' "$T/assign"; }

while read -r key want why; do
    [ -n "$key" ] || continue
    got=$(eff "$key")
    if [ "$got" = "$want" ]; then ok "$key = $want"
    else bad "$key = $want" "is '${got:-unset}': $why"; fi
done <<'EOF'
kernel.kptr_restrict 2 kernel addresses would be visible
kernel.dmesg_restrict 1 the kernel log would be readable by programs
kernel.unprivileged_bpf_disabled 1 programs could load BPF into the kernel
net.core.bpf_jit_harden 2 the BPF compiler would be unhardened
kernel.yama.ptrace_scope 1 programs could read other programs' memory
kernel.kexec_load_disabled 1 the running kernel could be replaced
fs.protected_symlinks 1 /tmp symlink tricks
fs.protected_hardlinks 1 hardlink tricks
fs.protected_fifos 2 FIFO tricks in shared directories
fs.protected_regular 2 file tricks in shared directories
fs.suid_dumpable 0 privileged programs could dump memory
kernel.sysrq 176 SysRq keys could read memory or kill programs
dev.tty.ldisc_autoload 0 line-discipline modules load on demand
vm.unprivileged_userfaultfd 0 userfaultfd open to programs
kernel.io_uring_disabled 2 io_uring open to programs
net.ipv4.conf.all.accept_redirects 0 ICMP redirects accepted
net.ipv6.conf.all.accept_redirects 0 ICMPv6 redirects accepted
net.ipv4.conf.all.send_redirects 0 the machine could act as a router
net.ipv4.conf.all.accept_source_route 0 source routing accepted
net.ipv4.tcp_syncookies 1 SYN floods
kernel.panic 10 a kernel panic would freeze the screen for ever
kernel.panic_on_oops 0 one driver's bug would take the whole machine down
vm.swappiness 150 zram swap would be used like a slow disk
vm.page-cluster 0 zram would read ahead for nothing
EOF

# What must NOT be turned off: Flatpak's sandbox (bubblewrap) needs
# user namespaces, which AppArmor already restricts to the programs
# allowed them (/etc/apparmor.d/flatpak has `userns,`).
for kv in kernel.unprivileged_userns_clone=0 user.max_user_namespaces=0; do
    k=${kv%=*}
    if [ "$(eff "$k")" = "${kv#*=}" ]; then
        bad "$k is not ${kv#*=} (Flatpak's sandbox needs it)" "it is ${kv#*=}: every Flathub program would fail to start"
    else ok "$k is not ${kv#*=} (Flatpak's sandbox needs it)"; fi
done
if [ -e "$RFS/usr/bin/flatpak" ]; then
    grep -q '^[[:space:]]*userns,' "$RFS/etc/apparmor.d/flatpak" 2>/dev/null \
        && ok "AppArmor lets Flatpak make its sandbox (userns)" \
        || bad "AppArmor lets Flatpak make its sandbox (userns)" "etc/apparmor.d/flatpak has no userns rule"
fi

# ── 2. the kernel command line, on every way AurOS can start ─────────
say ""
say "Kernel command line"
KOPTS="init_on_alloc=1 slab_nomerge randomize_kstack_offset=on vsyscall=none debugfs=off panic=10 fsck.repair=yes"
check_lines() { # label file pattern
    lines=$(grep -E "$3" "$2" 2>/dev/null)
    if [ -z "$lines" ]; then bad "$1" "no kernel line in ${2#"$RFS"/}"; return; fi
    miss=""
    for o in $KOPTS; do
        printf '%s\n' "$lines" | while read -r l; do
            case " $l " in *" $o "*) ;; *) echo x ;; esac
        done | grep -q x && miss="$miss $o"
    done
    if printf '%s\n' "$lines" | grep -q 'init_on_free=1'; then miss="$miss (init_on_free=1 is there: a measured cost for little)"; fi
    [ -z "$miss" ] && ok "$1" || bad "$1" "missing:$miss"
}
check_lines "every AurOS entry in grub.cfg.in" "$RFS/usr/lib/auros/grub.cfg.in" '^[[:space:]]*linux[[:space:]]+/boot/vmlinuz-'
check_lines "the previous-kernel entry auros-bootchain writes" "$RFS/usr/lib/auros/bootchain" '^[[:space:]]*linux[[:space:]]+/boot/vmlinuz-\{v\}'
if [ -f "$RFS/boot/grub/grub.cfg" ]; then
    check_lines "every AurOS entry in the written grub.cfg" "$RFS/boot/grub/grub.cfg" '^[[:space:]]*linux[[:space:]]+/boot/vmlinuz-'
fi

# ── 3. the firewall ──────────────────────────────────────────────────
say ""
say "Firewall"
NFT="$RFS/etc/nftables.conf"
if [ -x "$RFS/usr/sbin/nft" ]; then ok "nftables is installed"; else bad "nftables is installed" "no usr/sbin/nft"; fi
if enabled sysinit.target nftables.service; then ok "nftables.service starts at boot"
else bad "nftables.service starts at boot" "no sysinit.target.wants/nftables.service link"; fi
sed 's/#.*//' "$NFT" 2>/dev/null > "$T/nft"
chain_policy() { # chain -> its policy
    awk -v c="$1" '$1 == "chain" && $2 == c { inside = 1 }
                   inside && /policy/ { s = $0; sub(/.*policy[ \t]+/, "", s); sub(/[ ;].*/, "", s); print s; exit }' "$T/nft"
}
[ "$(chain_policy input)" = drop ] && ok "inbound: everything not answered is dropped" \
    || bad "inbound: everything not answered is dropped" "input chain policy is '$(chain_policy input)'"
[ "$(chain_policy forward)" = drop ] && ok "forwarding: dropped (this is not a router)" \
    || bad "forwarding: dropped (this is not a router)" "forward chain policy is '$(chain_policy forward)'"
grep -q '^flush ruleset' "$T/nft" && ok "the ruleset replaces whatever was there" \
    || bad "the ruleset replaces whatever was there" "no 'flush ruleset'"
# Every accept rule must be one of these. A new one -- an open port --
# is a decision to write down here and in HARDENING.md, not a line to
# slip into the file.
cat > "$T/allowed" <<'EOF'
iif "lo" accept
ct state established,related accept
nd-neighbor-advert, mld-listener-query } accept
udp sport 67 udp dport 68 accept
ip6 saddr fe80::/10 udp sport 547 udp dport 546 accept
udp dport 5353 @th,80,1 1 accept
EOF
extra=$(grep -E '(^|[[:space:]])accept([[:space:]]|;|$)' "$T/nft" | grep -v 'policy accept' \
        | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' | grep -vxF -f "$T/allowed")
[ -z "$extra" ] && ok "nothing is let in but answers, DHCP, IPv6 basics, printer search" \
    || bad "nothing is let in but answers, DHCP, IPv6 basics, printer search" "unexpected: $extra"
grep -q 'ct state established,related accept' "$T/nft" \
    && ok "answers to her own traffic come back" \
    || bad "answers to her own traffic come back" "no established,related accept: nothing would work"
if [ "$(id -u)" = 0 ] && command -v nft >/dev/null 2>&1; then
    e=$(nft -c -f "$NFT" 2>&1) && ok "the ruleset loads (nft -c)" || bad "the ruleset loads (nft -c)" "$e"
fi

# ── 4. printing without a network listener ───────────────────────────
say ""
say "Printing and discovery"
if installed cups-browsed; then bad "cups-browsed is not installed (CVE-2024-47176)" "dpkg says it is"
else ok "cups-browsed is not installed (CVE-2024-47176)"; fi
if enabled multi-user.target cups-browsed.service; then bad "cups-browsed is not started" "multi-user.target.wants/cups-browsed.service"
else ok "cups-browsed is not started"; fi
if installed cups-daemon; then
    l=$(grep -E '^[[:space:]]*(Listen|Port)[[:space:]]' "$RFS/etc/cups/cupsd.conf" | grep -vE 'Listen[[:space:]]+(localhost:631|/run/cups/cups.sock)$')
    [ -z "$l" ] && ok "cups listens on this computer only" || bad "cups listens on this computer only" "$l"
    [ -x "$RFS/usr/lib/cups/backend/usb" ] && ok "cups can still print to USB" \
        || bad "cups can still print to USB" "no usr/lib/cups/backend/usb"
fi
if [ -f "$RFS/etc/avahi/avahi-daemon.conf" ]; then
    v=$(awk '/^\[/ { s = $0 } s == "[publish]" && /^disable-publishing[[:space:]]*=/ { sub(/.*=[[:space:]]*/, ""); print }' \
        "$RFS/etc/avahi/avahi-daemon.conf")
    [ "$v" = yes ] && ok "avahi announces nothing about this computer" \
        || bad "avahi announces nothing about this computer" "[publish] disable-publishing is '${v:-unset}'"
fi

# ── 5. kernel modules an ordinary program could load ─────────────────
say ""
say "Kernel modules that stay unloaded"
MP="$RFS/etc/modprobe.d/auros-hardening.conf"
miss=""
for m in dccp sctp rds tipc n_hdlc ax25 netrom x25 rose decnet econet af_802154 ipx \
         appletalk psnap p8023 p8022 can can-bcm can-raw can-gw can-isotp can-j1939 atm \
         ksmbd vivid cramfs freevxfs jffs2 hfs firewire-core firewire-ohci firewire-sbp2; do
    grep -qx "install $m /bin/false" "$MP" 2>/dev/null || miss="$miss $m"
done
[ -z "$miss" ] && ok "32 rarely used, often exploited modules cannot load" \
    || bad "32 rarely used, often exploited modules cannot load" "loadable:$miss"

# ── 6. memory, logs, recovery ────────────────────────────────────────
say ""
say "Memory, logs and recovery"
J="$RFS/etc/systemd/journald.conf.d/50-auros.conf"
mx=$(sed -n 's/^SystemMaxUse=\([0-9]*\)M$/\1/p' "$J" 2>/dev/null)
[ -n "$mx" ] && [ "$mx" -le 500 ] && ok "the system log is capped (${mx} MB)" \
    || bad "the system log is capped" "SystemMaxUse in ${J#"$RFS"/} is '${mx:-unset}'"
grep -q '^SystemKeepFree=' "$J" 2>/dev/null && ok "the log stops before the disk is full" \
    || bad "the log stops before the disk is full" "no SystemKeepFree="
if enabled multi-user.target zramswap.service && grep -q '^PERCENT=[1-9]' "$RFS/etc/default/zramswap" 2>/dev/null; then
    ok "compressed swap in RAM (zram, $(sed -n 's/^PERCENT=//p' "$RFS/etc/default/zramswap")%)"
else bad "compressed swap in RAM (zram)" "zramswap.service not enabled or etc/default/zramswap has no PERCENT"; fi
if enabled multi-user.target earlyoom.service &&
   grep -q -- "--avoid '[^']*aurshell" "$RFS/etc/default/earlyoom" 2>/dev/null; then
    ok "earlyoom stops a runaway program, never the desktop"
else bad "earlyoom stops a runaway program, never the desktop" "earlyoom.service not enabled, or no --avoid aurshell"; fi

# The desktop's safe mode.
U="$RFS/etc/systemd/system/aurshell.service"
DI="$RFS/etc/systemd/system/aurshell.service.d/50-auros-safe-mode.conf"
grep -q '^ExecStart=.*/usr/bin/aurshell \$AURSHELL_SAFE$' "$U" 2>/dev/null \
    && ok "the desktop's command line takes the safe-mode words" \
    || bad "the desktop's command line takes the safe-mode words" "aurshell.service ExecStart has no \$AURSHELL_SAFE"
if grep -qx 'EnvironmentFile=-/run/auros-guard/aurshell.env' "$DI" 2>/dev/null &&
   grep -qx 'ExecStartPre=+/usr/lib/auros/shell-guard start' "$DI" &&
   grep -qx 'ExecStopPost=+/usr/lib/auros/shell-guard stopped' "$DI"; then
    ok "the safe-mode guard runs around every desktop start"
else bad "the safe-mode guard runs around every desktop start" "${DI#"$RFS"/} is missing or incomplete"; fi
G="$RFS/usr/lib/auros/shell-guard"
if [ -x "$G" ]; then
    gd="$T/guard"; mkdir -p "$gd"; printf 'kiosk_mode = no\n' > "$T/policy"
    guard() { AUROS_GUARD_DIR="$gd" AUROS_GUARD_POLICY="$T/policy" AUROS_GUARD_NOW="$1" sh "$G" "$2" 2>/dev/null; }
    envw() { sed -n 's/^AURSHELL_SAFE=//p' "$gd/aurshell.env" 2>/dev/null; }
    guard 100 start
    [ -z "$(envw)" ] && ok "a normal start is a normal start" || bad "a normal start is a normal start" "env: $(envw)"
    SERVICE_RESULT=success guard 110 stopped
    SERVICE_RESULT=success guard 120 stopped
    SERVICE_RESULT=success guard 130 stopped
    guard 140 start
    [ -z "$(envw)" ] && ok "stopping it on purpose is not a crash" || bad "stopping it on purpose is not a crash" "env: $(envw)"
    SERVICE_RESULT=signal EXIT_CODE=dumped EXIT_STATUS=SEGV guard 150 stopped
    SERVICE_RESULT=signal guard 200 stopped
    guard 210 start
    [ -z "$(envw)" ] && ok "two crashes: still her own desktop" || bad "two crashes: still her own desktop" "env: $(envw)"
    SERVICE_RESULT=exit-code guard 260 stopped
    guard 270 start
    [ "$(envw)" = "--shell /usr/share/auros/shells/rail.shell --conf /etc/auros/shell.conf" ] \
        && ok "three crashes in ten minutes: the plain home, the machine's settings" \
        || bad "three crashes in ten minutes: the plain home, the machine's settings" "env: '$(envw)'"
    [ -f "$RFS/usr/share/auros/shells/rail.shell" ] && ok "...and that home is in the image" \
        || bad "...and that home is in the image" "no usr/share/auros/shells/rail.shell"
    for t in 300 340 380; do SERVICE_RESULT=signal guard $t stopped; done
    guard 390 start
    case "$(envw)" in "--shell /usr/share/auros/shells/rail.shell --conf "*)
        [ "$(envw)" != "--shell /usr/share/auros/shells/rail.shell --conf /etc/auros/shell.conf" ] \
            && ok "three more: the built-in look as well" || bad "three more: the built-in look as well" "env: $(envw)" ;;
        *) bad "three more: the built-in look as well" "env: '$(envw)'" ;; esac
    rm -rf "$gd"; mkdir -p "$gd"
    for t in 10 20 30; do SERVICE_RESULT=signal guard $t stopped; done
    guard 900 start
    [ -z "$(envw)" ] && ok "crashes long ago do not count" || bad "crashes long ago do not count" "env: $(envw)"
    rm -rf "$gd"; mkdir -p "$gd"; printf 'kiosk_mode = yes\n' > "$T/policy"
    for t in 10 20 30; do SERVICE_RESULT=signal guard $t stopped; done
    guard 40 start
    case "$(envw)" in *locked.shell*) ok "a kiosk's safe mode is still locked" ;;
        *) bad "a kiosk's safe mode is still locked" "env: '$(envw)'" ;; esac
else
    bad "the safe-mode guard is in the image" "no executable usr/lib/auros/shell-guard"
fi

# dpkg repaired at boot.
if enabled multi-user.target auros-dpkg-repair.service; then ok "an interrupted update is finished at the next start"
else bad "an interrupted update is finished at the next start" "auros-dpkg-repair.service not enabled"; fi
DR="$RFS/usr/lib/auros/dpkg-repair"
if [ -x "$DR" ]; then
    mkdir -p "$T/bin" "$T/adm/updates"
    printf '#!/bin/sh\necho "$0 $*" >> "%s/calls"\n' "$T" > "$T/bin/dpkg"
    cp "$T/bin/dpkg" "$T/bin/apt-get"; chmod 755 "$T/bin/dpkg" "$T/bin/apt-get"
    printf 'Package: less\nStatus: install ok installed\n\nPackage: nano\nStatus: install ok installed\n' > "$T/adm/status"
    rep() { : > "$T/calls"; PATH="$T/bin:$PATH" DPKG_ADMINDIR="$T/adm" sh "$DR" >/dev/null 2>&1; }
    rep
    [ ! -s "$T/calls" ] && ok "...and a clean machine costs nothing (dpkg not run)" \
        || bad "...and a clean machine costs nothing (dpkg not run)" "$(cat "$T/calls")"
    printf 'Package: less\nStatus: install ok half-configured\n' > "$T/adm/status"
    rep
    grep -q 'dpkg --force-confdef --force-confold --configure -a' "$T/calls" \
        && ok "...a half-configured package is configured" \
        || bad "...a half-configured package is configured" "calls: $(cat "$T/calls")"
    printf 'Package: less\nStatus: install ok installed\n' > "$T/adm/status"; : > "$T/adm/updates/0001"
    rep
    grep -q -- '--configure -a' "$T/calls" && ok "...and so is an update dpkg had only begun" \
        || bad "...and so is an update dpkg had only begun" "calls: $(cat "$T/calls")"
else
    bad "the dpkg repair is in the image" "no executable usr/lib/auros/dpkg-repair"
fi

say ""
if [ "$fail" -eq 0 ]; then
    echo "hardentest: $checked checks, all ok"
    exit 0
fi
echo "hardentest: $fail of $checked checks FAILED"
exit 1
