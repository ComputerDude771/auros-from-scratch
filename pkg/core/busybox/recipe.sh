# busybox — the userland utilities for the base system.
#
# One binary, ~400 applets, linked dynamically against musl so the
# system genuinely exercises its own libc rather than hiding it inside
# a static blob.
pkgname=busybox
pkgver=1.36.1
pkgdesc="The Swiss Army knife of embedded Linux"
license="GPL-2.0-only"
url="https://busybox.net"
source="busybox-1.36.1.tar.bz2"
sha256="b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314"
depends="musl"
provides="sh coreutils"

build() {
    cd "$srcdir/busybox-$pkgver"
    make defconfig

    # Applets that do not build against musl, or that we deliberately
    # replace with our own tools.
    #   TC              needs kernel tc_act headers glibc happens to ship
    #   *_NFS / RPC     wants Sun RPC, absent from musl
    #   INIT / halt     aurinit is PID 1; a second init would be a trap
    #   SHA1_HWACCEL    miscompiles on some hosts with -Os + musl
    for opt in CONFIG_TC CONFIG_FEATURE_MOUNT_NFS CONFIG_FEATURE_INETD_RPC \
               CONFIG_INIT CONFIG_HALT CONFIG_POWEROFF CONFIG_REBOOT \
               CONFIG_SHA1_HWACCEL CONFIG_USE_BB_CRYPT_SHA; do
        ./scripts/config --disable "$opt" 2>/dev/null || \
            sed -i "s/^$opt=y/# $opt is not set/" .config
    done
    # Dynamic against musl; the sysroot wrapper supplies the paths.
    ./scripts/config --disable CONFIG_STATIC 2>/dev/null || true
    yes "" | make oldconfig >/dev/null 2>&1 || true

    make -j"$JOBS" CC="$MUSLGCC" HOSTCC=gcc \
        CFLAGS_EXTRA="-Wno-error" busybox
}

package() {
    cd "$srcdir/busybox-$pkgver"
    install -Dm755 busybox "$pkgdir/bin/busybox"

    # Materialise the applet symlinks ourselves. busybox's own
    # install target wants to write to the live filesystem.
    mkdir -p "$pkgdir/bin" "$pkgdir/sbin" "$pkgdir/usr/bin" "$pkgdir/usr/sbin"
    ./busybox --list-full | while read -r applet; do
        case "$applet" in
            bin/busybox) continue ;;
            # aurinit owns PID 1 and the shutdown verbs.
            sbin/init|sbin/halt|sbin/poweroff|sbin/reboot) continue ;;
        esac
        mkdir -p "$pkgdir/$(dirname "$applet")"
        ln -sf /bin/busybox "$pkgdir/$applet"
    done
}
