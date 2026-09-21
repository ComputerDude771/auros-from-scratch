#!/bin/sh
# permissions.sh — can this computer do the things it offers to do?
#
# Three times now, in one day, the same bug: the product puts a control
# on the screen, the control runs something, and the permission service
# refuses it in silence.
#
#   the Internet button   found by reading the code
#   Turn off, and opening a USB stick   found by an adversarial review,
#                                       three lines away in the same file
#   installing a downloaded .deb        found by reading gdebi's own
#                                       policy while checking the second
#
# Every one of them looked right. Every one of them pressed cleanly.
# None of them did anything, and nothing anywhere said so -- not the
# build, not the journal, not the screen. A control that looks like a
# control and is not is the specific failure this product cannot
# afford, because the person it is for does not try twice.
#
# So this is the check. It is not clever. It reads the list of things
# the product OFFERS TO DO, and for each one asks two questions of the
# image that was actually built:
#
#   does the permission service know this action at all?   a typo in an
#      action id is a grant that does nothing, and polkit does not
#      complain about an id it has never heard of
#   does our rule grant it?   or does the distribution already allow it
#      for a person sitting at the machine
#
#   APPS=<rootfs>/usr/share/applications sh tools/permissions.sh
#   sh tools/permissions.sh          # against a built desktop rootfs
#
# Exits 2 if there is no built image to check, rather than passing on
# an empty search.
set -u
cd "$(dirname "$0")/.."

RFS="${RFS:-work/forge/desktop/rootfs}"
ACTIONS="$RFS/usr/share/polkit-1/actions"
RULE="$RFS/etc/polkit-1/rules.d/49-auros.rules"

if [ ! -d "$ACTIONS" ]; then
    echo "no built image at $RFS -- nothing to check."
    echo "  ./build/forge build desktop     (or set RFS=<rootfs>)"
    exit 2
fi

fail=0
checked=0

# What the product offers her, and the action each one needs. The left
# column is the words she reads on the screen; if a row here stops
# matching something the product actually does, that is the bug this
# file exists to make visible.
OFFERS='Turn off|org.freedesktop.login1.power-off
Open a USB stick|org.freedesktop.udisks2.filesystem-mount
Eject a USB stick|org.freedesktop.udisks2.eject-media
Internet: look for wifi|org.freedesktop.NetworkManager.wifi.scan
Internet: join a network|org.freedesktop.NetworkManager.settings.modify.system
Internet: connect|org.freedesktop.NetworkManager.network-control
Install a downloaded program|com.ubuntu.pkexec.gdebi.gtk
Get more programs (the store)|org.freedesktop.packagekit.package-install
Remove a program|org.freedesktop.packagekit.package-remove
Update this computer|org.freedesktop.packagekit.system-update
Settings: where you are|org.freedesktop.timedate1.set-timezone'

echo "can this computer do the things it offers to do?"
echo

printf '%s\n' "$OFFERS" | while IFS='|' read -r what act; do
    [ -n "$act" ] || continue

    # 1. Does the permission service know this action?
    if ! grep -rqs "action id=\"$act\"" "$ACTIONS"; then
        printf '  %-32s %s\n' "$what" "no such permission exists in this image"
        printf '      %s\n' "$act"
        echo HIT >> /tmp/permissions.hits
        continue
    fi

    # 2. Is it allowed for a person sitting at the machine, either
    #    because the distribution says so or because we granted it?
    dist=$(awk -v a="$act" '
        $0 ~ "action id=\"" a "\"" { inside = 1 }
        inside && /<allow_active>/ {
            gsub(/.*<allow_active>|<\/allow_active>.*/, ""); print; exit
        }
        inside && /<\/action>/ { exit }
    ' "$ACTIONS"/*.policy 2>/dev/null)

    ours=no
    [ -f "$RULE" ] && grep -q "\"$act\"" "$RULE" && ours=yes

    if [ "$ours" = "yes" ]; then
        printf '  %-32s %s\n' "$what" "granted by this build"
    elif [ "$dist" = "yes" ]; then
        printf '  %-32s %s\n' "$what" "already allowed (${dist})"
    else
        printf '  %-32s %s\n' "$what" "REFUSED -- needs ${dist:-a password} and nothing grants it"
        printf '      %s\n' "$act"
        echo HIT >> /tmp/permissions.hits
    fi
done

# 3. And nothing in our rule may name an action that does not exist.
#    polkit says nothing about an id it has never heard of, so a typo
#    is a grant that silently is not one.
echo
echo "and does every permission this build grants actually exist?"
if [ -f "$RULE" ]; then
    sed -n 's/^[[:space:]]*case "\([^"]*\)":.*/\1/p' "$RULE" | while read -r act; do
        if grep -rqs "action id=\"$act\"" "$ACTIONS"; then
            printf '  %-58s ok\n' "$act"
        else
            printf '  %-58s NO SUCH ACTION\n' "$act"
            echo HIT >> /tmp/permissions.hits
        fi
    done
else
    echo "  this build grants nothing (no $RULE)"
fi

if [ -f /tmp/permissions.hits ]; then
    fail=$(wc -l < /tmp/permissions.hits)
    rm -f /tmp/permissions.hits
fi
checked=$(printf '%s\n' "$OFFERS" | wc -l)

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail thing$([ "$fail" -eq 1 ] || echo s) this computer offers to do would be"
    echo "refused, silently, with nothing on the screen to say so."
    echo "The grants are written by build/forge from the profile's"
    echo "allow_* flags. A control the profile forbids should not be on"
    echo "the screen either -- those two have to agree."
    exit 1
fi
echo "all $checked things this computer offers to do are permitted"
exit 0
