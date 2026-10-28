#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  install.sh — put Ferry into an image. The one list of what Ferry is.
#
#  build/forge calls this twice and knows nothing else about Ferry:
#
#    sh src/ferry/install.sh scripts ROOTFS
#        (outside the chroot) the shell tools, the data tables and the
#        /usr/bin/ferry link, copied into ROOTFS;
#    sh /usr/src/auros/ferry/install.sh helpers /
#        (inside the chroot, from the copied source) the C helpers,
#        compiled against the image's own libc.
#
#  The list used to live in build/forge, so adding or removing a Ferry
#  tool meant editing another stream's build script, and forgetting to
#  broke the image build. Now it lives here, beside the files it names.
#
#  The helpers read hostile on-disk data as root, so they are built
#  with the warning set and the hardening flags the rest of AurOS uses.
# ═══════════════════════════════════════════════════════════════════
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)

SCRIPTS="ferry ferry-detect ferry-wifi ferry-files ferry-firefox \
ferry-settings ferry-report ferry-hive ferry-common.sh"
# name:source
HELPERS="ferry-hivepeek:hivepeek.c ferry-winattr:winattr.c"

HARDEN="-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong \
-fstack-clash-protection -fcf-protection=full -fPIE -pie \
-Wl,-z,relro,-z,now"

case "${1:-}" in
    scripts)
        R=${2:?usage: install.sh scripts ROOTFS}
        install -d "$R/usr/lib/ferry" "$R/usr/share/ferry" "$R/usr/bin"
        for t in $SCRIPTS; do
            install -m755 "$HERE/$t" "$R/usr/lib/ferry/$t"
        done
        install -m644 "$HERE/data"/*.tab "$R/usr/share/ferry/"
        ln -sf /usr/lib/ferry/ferry "$R/usr/bin/ferry" ;;
    helpers)
        R=${2:-/}
        install -d "$R/usr/lib/ferry"
        CC=${CC:-gcc}
        for h in $HELPERS; do
            # shellcheck disable=SC2086
            "$CC" -std=gnu11 -Wall -Wextra $HARDEN \
                -o "$R/usr/lib/ferry/${h%%:*}" "$HERE/${h#*:}"
        done ;;
    list)
        # For tests: every file this installs, so a test can prove the
        # list and the tree agree.
        for t in $SCRIPTS; do printf '%s\n' "$t"; done
        for h in $HELPERS; do printf '%s\n' "${h#*:}"; done ;;
    *)
        echo "usage: install.sh scripts ROOTFS | helpers [ROOT] | list" >&2
        exit 2 ;;
esac
