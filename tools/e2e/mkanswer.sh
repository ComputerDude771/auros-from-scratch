#!/bin/sh
# mkanswer.sh OUT.iso
#
# The second CD Windows Setup finds its answers on: autounattend.xml at
# the root (Setup searches every drive for it), and the scripts it calls
# in e2e\. Windows reads the Joliet names.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$1
tree=$(mktemp -d)
trap 'rm -rf "$tree"' EXIT
mkdir "$tree/e2e"
cp "$here/guest/autounattend.xml" "$tree/"
for f in specialize.cmd firstlogon.ps1 boot.ps1 boot.vbs; do
    # CRLF: cmd.exe and wscript read LF-only files, but not always the
    # way they read CRLF ones, and Windows never writes them like that.
    sed 's/\r*$/\r/' "$here/guest/$f" > "$tree/e2e/$f"
done
xorriso -as mkisofs -quiet -o "$out" -J -joliet-long -r -V E2E_ANSWERS "$tree"
ls -l "$out"
