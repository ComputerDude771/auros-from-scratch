#!/bin/bash
# run.sh VARIANT: one variant of the othersys proof on an Actions runner.
#   ntfs       the whole P1 flow on a Windows with NTFS
#   bitlocker  the same with a BitLocker-shaped Windows partition
#   xubuntu, ubuntucinnamon, ubuntu-budgie   the same flow, that flavour
#   powercut   the plug pulled the moment the installer puts Ubuntu
#              first: the next start must be Windows
#   nofixup    install only, AurOS's firmware fix-up switched off: proves
#              the Windows-first check fails on what Ubuntu's installer
#              really leaves behind
set -uo pipefail
variant=$1
here=$(cd "$(dirname "$0")" && pwd)
out=$OS_DIR/out-$variant
mkdir -p "$out"
T0=$(date +%s)
{
    echo "| step | seconds |"; echo "|---|---|"
} > "$out/steps.md"
step() { echo "| $1 | $2 |" >> "$out/steps.md"; }
t=$(date +%s)
system=ubuntu-26.04.1-desktop
case $variant in xubuntu|ubuntucinnamon|ubuntu-budgie) system=$variant-26.04.1-desktop ;; esac
bash "$here/getiso.sh" "$OS_DIR/iso" "$system" | tee "$out/getiso.log"
ISO=$(sed -n 's/^ISO=//p' "$out/getiso.log")
step "download + verify the ISO (Actions network)" $(( $(date +%s) - t ))
[ -f "$ISO" ] || { echo "no ISO"; exit 1; }
t=$(date +%s)
extra=()
v=$variant
if [ "$variant" = nofixup ]; then extra=(--skip-fixup); v=ntfs; fi
case $variant in xubuntu|ubuntucinnamon|ubuntu-budgie|powercut) v=ntfs ;; esac
extra+=(--system "$here/../../src/othersys/systems/$system.json")
python3 -I "$here/mkdisk.py" --iso "$ISO" --variant "$v" --out "$OS_DIR/work-$variant" \
    --test-hooks "$here/hooks" "${extra[@]}" > "$out/mkdisk.log" 2>&1 || { tail -40 "$out/mkdisk.log"; exit 1; }
tail -3 "$out/mkdisk.log"
step "lay the disk out as Windows leaves it (test fixture)" $(( $(date +%s) - t ))
rm -f "$ISO"
cp "$OS_DIR/work-$variant/seed/boot/grub/grub.cfg" "$out/seed-grub.cfg"
sed 's/"password": "[^"]*"/"password": "(hash)"/' "$OS_DIR/work-$variant/seed/user-data" > "$out/seed-user-data"
cp "$OS_DIR/work-$variant/layout.json" "$OS_DIR/work-$variant/before.json" "$out/"
phases=install,firstboot,windowsfirst,confirm,afteryes,moknegative
[ "$variant" = nofixup ] && phases=install
[ "$variant" = powercut ] && phases=powercut
python3 -I "$here/vmtest.py" "$OS_DIR/work-$variant" "$out" --variant "$variant" --phases "$phases"
rc=$?
step "whole run" $(( $(date +%s) - T0 ))
cat "$out/results.md" "$out/steps.md"
exit $rc
