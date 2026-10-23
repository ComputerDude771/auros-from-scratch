#!/bin/sh
# mkanswer.sh OUT.iso
#
# The second CD Windows Setup finds its answers on: autounattend.xml at
# the root (Setup searches every drive for it), and the scripts it calls
# in e2e\. Windows reads the Joliet names.
#
#   E2E_DISK_GB   the disk's size (default 128); C: is sized to fill it
#   E2E_GOLDEN    which installed Windows this is (default base); the
#                 first-logon script reads it from e2e\variant.txt
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$1
disk_gb=${E2E_DISK_GB:-128}
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
mkdir "$tree/e2e"
# EFI 100 MB, MSR 16 MB and about 1 GB of recovery after C:
sed "s/@WINDOWS_MB@/$((disk_gb * 1024 - 1200))/" "$here/guest/autounattend.xml" > "$tree/autounattend.xml"
printf '%s\r\n' "${E2E_GOLDEN:-base}" > "$tree/e2e/variant.txt"
for f in specialize.cmd firstlogon.ps1 boot.ps1 boot.vbs; do
    # CRLF: cmd.exe and wscript read LF-only files, but not always the
    # way they read CRLF ones, and Windows never writes them like that.
    sed 's/\r*$/\r/' "$here/guest/$f" > "$tree/e2e/$f"
done
xorriso -as mkisofs -quiet -o "$out" -J -joliet-long -r -V E2E_ANSWERS "$tree"
ls -l "$out"
