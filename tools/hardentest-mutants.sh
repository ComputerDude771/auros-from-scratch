#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  hardentest-mutants — does every hardentest check fail when it should?
#
#  A check that has never failed proves nothing. This copies the files
#  hardentest reads out of a good rootfs into a scratch tree, then, one
#  at a time, removes or weakens ONE control the way a careless edit
#  would, and requires hardentest to fail -- on the check that names
#  that control, not somewhere else. The unchanged copy must pass.
#
#    RFS=work/forge/desktop/rootfs sh tools/hardentest-mutants.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
RFS="${RFS:-work/forge/desktop/rootfs}"
[ -d "$RFS/etc" ] || { echo "no rootfs at $RFS (set RFS=)"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/hardenmut.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT

PATHS="etc/sysctl.d usr/lib/sysctl.d etc/sysctl.conf
       usr/lib/auros/grub.cfg.in usr/lib/auros/bootchain usr/lib/auros/shell-guard
       usr/lib/auros/dpkg-repair boot/grub/grub.cfg etc/nftables.conf usr/sbin/nft
       etc/systemd/system var/lib/dpkg/status etc/cups/cupsd.conf
       usr/lib/cups/backend/usb etc/avahi/avahi-daemon.conf etc/modprobe.d
       etc/systemd/journald.conf.d etc/default/zramswap etc/default/earlyoom
       usr/share/auros/shells/rail.shell etc/apparmor.d/flatpak usr/bin/flatpak"
mkdir -p "$T/good"
for p in $PATHS; do
    [ -e "$RFS/$p" ] || [ -L "$RFS/$p" ] || continue
    mkdir -p "$T/good/$(dirname "$p")"
    cp -a "$RFS/$p" "$T/good/$p"
done

fail=0; n=0
run() { RFS="$1" sh tools/hardentest.sh 2>&1; }
out=$(run "$T/good"); rc=$?
if [ $rc -ne 0 ]; then
    printf '%s\n' "$out" | grep -A2 FAIL
    echo "hardentest-mutants: the unchanged copy does not pass; nothing to prove"
    exit 1
fi
echo
echo "Every check fails when its control is taken away ($(printf '%s\n' "$out" | tail -1))"
echo

# mutate "check that must fail" 'shell code run inside the copy'
mutate() {
    n=$((n+1))
    rm -rf "$T/m"; cp -a "$T/good" "$T/m"
    ( cd "$T/m" && eval "$2" ) >/dev/null 2>&1
    o=$(run "$T/m"); rc=$?
    if [ $rc -ne 0 ] && printf '%s\n' "$o" | grep -F -- "$1" | grep -q 'FAIL$'; then
        printf '    %-66s %s\n' "$1" "caught"
    else
        fail=$((fail+1))
        printf '    %-66s %s\n' "$1" "MISSED"
        printf '      mutation: %s\n' "$2"
    fi
}
H=etc/sysctl.d/60-auros-hardening.conf
mutate "kernel.kptr_restrict = 2"            "sed -i '/^kernel.kptr_restrict/d' $H"
mutate "kernel.unprivileged_bpf_disabled = 1" "sed -i '/^kernel.unprivileged_bpf_disabled/d' $H"
mutate "kernel.io_uring_disabled = 2"        "sed -i 's/^kernel.io_uring_disabled = 2/kernel.io_uring_disabled = 0/' $H"
mutate "kernel.kexec_load_disabled = 1"      "sed -i '/^kernel.kexec_load_disabled/d' $H"
mutate "fs.protected_fifos = 2"              "rm etc/sysctl.d/99-protect-links.conf"
mutate "kernel.panic = 10"                   "rm etc/sysctl.d/61-auros-reliability.conf"
mutate "vm.swappiness = 150"                 "echo 'vm.swappiness = 60' > etc/sysctl.d/90-later.conf"
mutate "user.max_user_namespaces is not 0"   "echo 'user.max_user_namespaces = 0' > etc/sysctl.d/70-x.conf"
if [ -e "$T/good/usr/bin/flatpak" ]; then
mutate "AppArmor lets Flatpak make its sandbox" "sed -i '/userns,/d' etc/apparmor.d/flatpak"
fi
mutate "every AurOS entry in grub.cfg.in"    "sed -i '0,/^ *linux .* slab_nomerge/s/ slab_nomerge//' usr/lib/auros/grub.cfg.in"
mutate "every AurOS entry in grub.cfg.in"    "sed -i 's/ panic=10 / panic=10 init_on_free=1 /' usr/lib/auros/grub.cfg.in"
mutate "the previous-kernel entry auros-bootchain writes" "sed -i 's/ debugfs=off//' usr/lib/auros/bootchain"
mutate "nftables.service starts at boot"     "rm etc/systemd/system/sysinit.target.wants/nftables.service"
mutate "inbound: everything not answered is dropped" "sed -i '/hook input/s/policy drop/policy accept/' etc/nftables.conf"
mutate "forwarding: dropped"                 "sed -i '/hook forward/s/policy drop/policy accept/' etc/nftables.conf"
mutate "nothing is let in but answers"       "sed -i 's/^\\( *\\)ct state invalid drop/&\\n\\1tcp dport 22 accept/' etc/nftables.conf"
mutate "answers to her own traffic come back" "sed -i '/established,related accept/d' etc/nftables.conf"
mutate "the ruleset replaces whatever was there" "sed -i '/^flush ruleset/d' etc/nftables.conf"
mutate "cups-browsed is not installed"       "printf '\\nPackage: cups-browsed\\nStatus: install ok installed\\n' >> var/lib/dpkg/status"
mutate "cups-browsed is not started"         "mkdir -p etc/systemd/system/multi-user.target.wants && ln -s /usr/lib/systemd/system/cups-browsed.service etc/systemd/system/multi-user.target.wants/cups-browsed.service"
mutate "cups listens on this computer only"  "sed -i 's/^Listen localhost:631/Listen 0.0.0.0:631/' etc/cups/cupsd.conf"
mutate "avahi announces nothing"             "sed -i 's/^disable-publishing=yes/disable-publishing=no/' etc/avahi/avahi-daemon.conf"
mutate "32 rarely used, often exploited modules" "sed -i '/^install dccp /d' etc/modprobe.d/auros-hardening.conf"
mutate "the system log is capped"            "rm etc/systemd/journald.conf.d/50-auros.conf"
mutate "compressed swap in RAM"              "rm etc/systemd/system/multi-user.target.wants/zramswap.service"
mutate "earlyoom stops a runaway program"    "sed -i 's/--avoid [^ ]* //' etc/default/earlyoom"
mutate "the desktop's command line takes the safe-mode words" "sed -i 's/ \\\$AURSHELL_SAFE\$//' etc/systemd/system/aurshell.service"
mutate "the safe-mode guard runs around every desktop start" "rm etc/systemd/system/aurshell.service.d/50-auros-safe-mode.conf"
mutate "three crashes in ten minutes"        "sed -i 's/^LIMIT=3/LIMIT=30/' usr/lib/auros/shell-guard"
mutate "stopping it on purpose is not a crash" "sed -i 's/= success \\] && exit 0/= never ] \\&\\& exit 0/' usr/lib/auros/shell-guard"
mutate "crashes long ago do not count"       "sed -i 's/^WINDOW=600/WINDOW=99999/' usr/lib/auros/shell-guard"
mutate "a kiosk's safe mode is still locked" "sed -i 's/kiosk_mode\\[/kiosk_mod[/' usr/lib/auros/shell-guard"
mutate "an interrupted update is finished at the next start" "rm etc/systemd/system/multi-user.target.wants/auros-dpkg-repair.service"
mutate "a half-configured package is configured" "sed -i 's/half-configured|//' usr/lib/auros/dpkg-repair"
mutate "a clean machine costs nothing"       "sed -i 's/^interrupted || exit 0/true/' usr/lib/auros/dpkg-repair"

echo
if [ $fail -eq 0 ]; then echo "hardentest-mutants: $n mutations, all caught"; exit 0; fi
echo "hardentest-mutants: $fail of $n mutations MISSED"
exit 1
