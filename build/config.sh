# AurOS build configuration — sourced by every stage.
set -eu

AUROS_NAME="AurOS"
AUROS_VERSION="0.1.0"
AUROS_CODENAME="Nocturne"
AUROS_ARCH="x86_64"

# Every path is relative to the repo root so a build is relocatable.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCES="$ROOT/sources"        # upstream tarballs
WORK="$ROOT/work"              # extraction + build trees
OUT="$ROOT/out"                # built .aurpkg files, kernel, ISO
SYSROOT="$ROOT/sysroot"        # musl headers/libs that later pkgs link against
ROOTFS="$ROOT/work/rootfs"     # the assembled system
PKGDIR="$ROOT/pkg"             # recipes

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

# Upstream versions. Pinned so a rebuild is reproducible.
MUSL_VER="1.2.4"
BUSYBOX_VER="1.36.1"
KERNEL_VER="6.8.0"

# Toolchain. musl-gcc is a thin spec wrapper around the host gcc that
# redirects the include and library search paths at the musl sysroot;
# it is how everything in userspace stops linking against host glibc.
MUSLGCC="$SYSROOT/bin/musl-gcc"

export ROOT SOURCES WORK OUT SYSROOT ROOTFS PKGDIR JOBS
export AUROS_NAME AUROS_VERSION AUROS_CODENAME AUROS_ARCH
export MUSL_VER BUSYBOX_VER KERNEL_VER MUSLGCC
