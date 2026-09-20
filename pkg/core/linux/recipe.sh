# linux — the kernel.
#
# Source is Ubuntu's linux-source package rather than kernel.org: the
# session's egress policy blocks cdn.kernel.org, and this is the same
# tree plus Ubuntu's patch set. Documented here because "where did the
# kernel come from" is exactly the question a security review asks.
pkgname=linux
pkgver=6.8.0
pkgdesc="The Linux kernel and its userspace API headers"
license="GPL-2.0-only WITH Linux-syscall-note"
url="https://kernel.org"
source="linux-6.8.0.tar.bz2"
sha256="aba4ca1e601f04dbd3dd0958e920b6a7ade1841c01d66ecdd91bca4bfe43d81b"
depends=""
provides="kernel linux-headers"

build() {
    cd "$srcdir/linux-source-$pkgver"

    make ARCH=x86_64 defconfig
    # Merge our fragment over defconfig. merge_config.sh reports any
    # option the final .config did not honour, which catches typos and
    # silently-dropped symbols instead of letting them vanish.
    ./scripts/kconfig/merge_config.sh -m .config "$rdir/config.fragment"
    make ARCH=x86_64 olddefconfig

    # Fail loudly if something we depend on to boot got dropped.
    for must in CONFIG_NTFS3_FS CONFIG_EXT4_FS CONFIG_DEVTMPFS_MOUNT \
                CONFIG_BLK_DEV_INITRD CONFIG_DRM_SIMPLEDRM \
                CONFIG_SERIAL_8250_CONSOLE CONFIG_INPUT_EVDEV; do
        grep -q "^$must=y" .config || {
            echo "aurb: kernel config lost $must — refusing to ship a kernel that cannot boot" >&2
            exit 1
        }
    done

    make ARCH=x86_64 -j"$JOBS" bzImage
}

package() {
    cd "$srcdir/linux-source-$pkgver"
    install -Dm644 arch/x86/boot/bzImage "$pkgdir/boot/vmlinuz-$pkgver"
    install -Dm644 .config               "$pkgdir/boot/config-$pkgver"
    install -Dm644 System.map            "$pkgdir/boot/System.map-$pkgver"

    # Userspace API headers, so later packages compile against our
    # kernel rather than whatever the build host happens to have.
    make ARCH=x86_64 headers_install INSTALL_HDR_PATH="$pkgdir/usr"
    make ARCH=x86_64 headers_install INSTALL_HDR_PATH="$SYSROOT/usr"
}
