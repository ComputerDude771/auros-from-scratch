#!/bin/sh
# permissions.sh — can this computer do the things it offers to do?
#
# Four times now the same bug: the product puts a control on the
# screen, the control runs something, and the permission service
# refuses it in silence.
#
#   the Internet button   found by reading the code
#   Turn off, and opening a USB stick   found by an adversarial review,
#                                       three lines away in the same file
#   installing a downloaded .deb        found by reading gdebi's own
#                                       policy while checking the second
#   Let it sleep          found by an adversarial review of the commit
#                         that ADDED the button -- it relied on the
#                         distribution's allow_active and nothing
#                         granted it
#
# Every one of them looked right. Every one of them pressed cleanly.
# None of them did anything, and nothing anywhere said so -- not the
# build, not the journal, not the screen. A control that looks like a
# control and is not is the specific failure this product cannot
# afford, because the person it is for does not try twice.
#
# WHAT THIS CAN AND CANNOT SEE
#
# It reads a built rootfs. It can see what is INSTALLED and what is
# WRITTEN DOWN: which polkit actions exist, which our rule grants,
# which groups the account is in, which groups the unit demands.
#
# It CANNOT see a running system. In particular it cannot see whether
# the desktop's logind session is active, which is what every
# `allow_active` in the distribution's own policies depends on -- and
# "there is no session at all" was the original bug here. So section 5
# checks the things that MAKE it active, statically, and the smoke
# test on a booted image checks the rest.
#
#   RFS=<rootfs> sh tools/permissions.sh
#   sh tools/permissions.sh          # against a built desktop rootfs
#
# Exits 2 if there is no built image to check, rather than passing on
# an empty search.
set -u
cd "$(dirname "$0")/.."

RFS="${RFS:-work/forge/desktop/rootfs}"
ACTIONS="$RFS/usr/share/polkit-1/actions"
RULE="$RFS/etc/polkit-1/rules.d/49-auros.rules"
UNIT="$RFS/etc/systemd/system/aurshell.service"
DROPIN="$RFS/etc/systemd/system/aurshell.service.d/user.conf"

if [ ! -d "$ACTIONS" ]; then
    echo "no built image at $RFS -- nothing to check."
    echo "  ./build/forge build desktop     (or set RFS=<rootfs>)"
    exit 2
fi

# The tally, in a file because each section runs in a `while` subshell
# and a variable set there does not survive it.
#
# It used to be /tmp/permissions.hits, a fixed world-writable path. A
# stale one from a run that exited early made the next run fail with a
# bogus count; a /tmp that could not be written made every failure
# invisible and the script exit 0 -- a harness whose failure channel
# can fail open. As a root-run build tool it was also an
# append-to-any-file primitive by symlink.
HITS=$(mktemp) || { echo "cannot make a temporary file"; exit 2; }
trap 'rm -f "$HITS"' EXIT INT TERM
hit() { printf '%s\n' "$1" >> "$HITS" || { echo "CANNOT RECORD FAILURES"; exit 2; }; }
checked=0
count() { checked=$((checked + 1)); }

USER_NAME=$(sed -n 's/^default_user *= *//p' "$RFS/etc/auros/policy.conf" 2>/dev/null)
[ -n "$USER_NAME" ] || USER_NAME=auros

# ── 1. the things the product offers to do ─────────────────────────
#
# The left column is the words she reads on the screen. If a row here
# stops matching something the product actually does, that is the bug
# this file exists to make visible -- which is how Restart and Sleep
# came to be on the band for two commits without being on this list.
OFFERS='Turn it off|org.freedesktop.login1.power-off
Start it again|org.freedesktop.login1.reboot
Let it sleep|org.freedesktop.login1.suspend
Open a USB stick|org.freedesktop.udisks2.filesystem-mount
Eject a USB stick|org.freedesktop.udisks2.eject-media
Safely remove a USB stick|org.freedesktop.udisks2.power-off-drive
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

    # 1a. Does the permission service know this action? polkit says
    #     nothing about an id it has never heard of, so a typo is a
    #     grant that silently is not one.
    if ! grep -rqs "action id=\"$act\"" "$ACTIONS"; then
        printf '  %-32s %s\n' "$what" "no such permission exists in this image"
        printf '      %s\n' "$act"
        hit "$what: no such action"
        continue
    fi

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
        # NOT a clean pass. allow_active means "yes, IF the session is
        # active", and this harness cannot see a session. The Sleep
        # button sat in exactly this square and was called working.
        printf '  %-32s %s\n' "$what" "allowed only while the session is ACTIVE"
        printf '      %s -- grant it outright, or it fails silently the\n' "$act"
        printf '      first time the session is not active\n'
        hit "$what: relies on allow_active"
    else
        printf '  %-32s %s\n' "$what" "REFUSED -- needs ${dist:-a password} and nothing grants it"
        printf '      %s\n' "$act"
        hit "$what: refused"
    fi
    :
done
checked=$(printf '%s\n' "$OFFERS" | grep -c '|')

# ── 2. not everything is polkit ────────────────────────────────────
#
#    CUPS is gated by cups-files.conf, which names a GROUP. Neither it
#    nor bluez has a polkit action at all -- five were added for them
#    from memory, and section 1 answered "NO SUCH ACTION" to every one.
#    A harness that only knew about polkit would have said all was well
#    while she could not add her printer.
#
#    BlueZ IS THE CAUTIONARY TALE IN THE OTHER DIRECTION. The commit
#    that added the group said its D-Bus policy names it -- true -- and
#    did not read the next stanza, which is
#      <policy context="default"><allow send_destination="org.bluez"/></policy>
#    On this image EVERY user may talk to org.bluez, so the group is
#    not what grants it. Being in it is harmless and portable; ASSERTING
#    it here would have failed the build over a condition nothing needs,
#    which is its own kind of false alarm. So it is reported and not
#    counted.
user_gid=$(awk -F: -v u="$USER_NAME" '$1==u{print $4}' "$RFS/etc/passwd" 2>/dev/null)

# Returns 0 in, 1 not in, 2 no such group. The old version grepped the
# members field only, so a user whose PRIMARY group is the one being
# asked about read as not in it -- a false failure waiting for the
# first profile that reorders the account's groups.
in_group() {
    _line=$(awk -F: -v g="$1" '$1==g{print; exit}' "$RFS/etc/group" 2>/dev/null)
    [ -n "$_line" ] || return 2
    _gid=$(printf '%s' "$_line" | cut -d: -f3)
    [ -n "$user_gid" ] && [ "$_gid" = "$user_gid" ] && return 0
    _mem=$(printf '%s' "$_line" | cut -d: -f4)
    case ",$_mem," in *",$USER_NAME,"*) return 0 ;; esac
    return 1
}

echo
echo "and is she in the groups the rest of it uses?"
printf '%s\n' \
  'lpadmin|setting up a printer|required' \
  'video|the screen brightness|required' \
  'input|the keyboard and touchpad|required' \
  'audio|the sound card|required' \
  'netdev|changing the network|required' \
  'bluetooth|pairing headphones|not required on this image' \
  | while IFS='|' read -r grp what need; do
    in_group "$grp"; r=$?
    if [ "$r" = 0 ]; then
        printf '  %-32s %s\n' "$what" "$USER_NAME is in $grp"
    elif [ "$r" = 2 ]; then
        printf '  %-32s %s\n' "$what" "there is no $grp group in this image"
        [ "$need" = required ] && hit "$what: no $grp group"
    else
        printf '  %-32s %s\n' "$what" "$USER_NAME is NOT in $grp ($need)"
        [ "$need" = required ] && hit "$what: not in $grp"
    fi
    :
done

# ── 3. the unit must be able to START ──────────────────────────────
#
#    systemd resolves SupplementaryGroups= BEFORE ExecStart, and one it
#    cannot resolve aborts the unit with status=216/GROUP. No desktop,
#    and no aursorry either, because the unit never ran: a black screen
#    with nothing on the machine able to explain it. `bluetooth` and
#    `lpadmin` exist only because bluez's and cups-daemon's postinsts
#    create them, so naming them in a unit that is not templated made
#    the desktop unbootable on any profile that trims those packages.
echo
echo "and can the desktop's own unit resolve the groups it demands?"
UNIT_GROUPS=$( { [ -f "$UNIT" ]   && sed -n 's/^SupplementaryGroups=//p' "$UNIT";
                 [ -f "$DROPIN" ] && sed -n 's/^SupplementaryGroups=//p' "$DROPIN"; } 2>/dev/null )
if [ -z "$UNIT_GROUPS" ]; then
    echo "  the unit demands no groups"
else
    for g in $UNIT_GROUPS; do
        if awk -F: -v g="$g" '$1==g{found=1} END{exit !found}' "$RFS/etc/group"; then
            printf '  %-58s ok\n' "SupplementaryGroups=$g resolves"
        else
            printf '  %-58s 216/GROUP\n' "SupplementaryGroups=$g"
            printf '      the desktop cannot start at all. Nothing draws,\n'
            printf '      not even the recovery screen.\n'
            hit "unit demands a group that does not exist: $g"
        fi
    done
fi

# ── 4. nothing granted may be a name for nothing ───────────────────
echo
echo "and does every permission this build grants actually exist?"
if [ -f "$RULE" ]; then
    # The rule is written for one account. A profile that renames it
    # and a rule that does not are a grant for a user who is not there.
    if ! grep -q "subject.user !== \"$USER_NAME\"" "$RULE"; then
        printf '  %-58s WRONG USER\n' "the rule's subject"
        printf '      policy.conf says %s\n' "$USER_NAME"
        hit "the rule does not name $USER_NAME"
    fi
    sed -n 's/^[[:space:]]*case "\([^"]*\)":.*/\1/p' "$RULE" | while read -r act; do
        if grep -rqs "action id=\"$act\"" "$ACTIONS"; then
            printf '  %-58s ok\n' "$act"
        else
            printf '  %-58s NO SUCH ACTION\n' "$act"
            hit "granted but does not exist: $act"
        fi
        :
    done
fi
[ -f "$RULE" ] || echo "  this build grants nothing (no $RULE)"

# ── 5. what makes the session ACTIVE ───────────────────────────────
#
#    Everything in the distribution's own policies that says
#    allow_active depends on this, and "there is no session at all" is
#    the bug that started this file. A harness reading a rootfs cannot
#    ask logind -- so it checks the four things that produce an active
#    session on seat0, every one of which has already been wrong once.
echo
echo "and will the desktop have an active session to be allowed by?"
sess=0
if grep -q '^PAMName=' "$UNIT" 2>/dev/null; then
    pam=$(sed -n 's/^PAMName=//p' "$UNIT")
    if [ -f "$RFS/etc/pam.d/$pam" ]; then
        if grep -q 'pam_systemd' "$RFS/etc/pam.d/$pam"; then
            printf '  %-58s ok\n' "PAMName=$pam, and its stack opens a logind session"
        else
            printf '  %-58s NO\n' "/etc/pam.d/$pam does not call pam_systemd"
            printf '      without it there is no session, and every allow_active\n'
            printf '      in the image refuses in silence.\n'
            hit "the PAM stack opens no session"
        fi
    else
        printf '  %-58s MISSING\n' "PAMName=$pam but /etc/pam.d/$pam"
        hit "PAMName names a stack that is not in the image"
    fi
    sess=1
else
    printf '  %-58s NO\n' "the unit has no PAMName="
    printf '      a service with no PAM stack gets no logind session, so\n'
    printf '      Turn off, mounting a USB stick and the wifi all refuse.\n'
    hit "no PAMName= on the unit"
fi

tty=$(sed -n 's/^TTYPath=//p' "$UNIT" 2>/dev/null)
if [ "${tty:-}" = "/dev/tty1" ]; then
    printf '  %-58s ok\n' "TTYPath=/dev/tty1, so the session is on seat0 vt1"
else
    printf '  %-58s %s\n' "TTYPath" "${tty:-unset} -- the session's seat follows this"
    hit "TTYPath is not /dev/tty1"
fi

if [ -e "$RFS/etc/systemd/system/getty@tty1.service" ] ||
   grep -qs 'getty@tty1' "$UNIT"; then
    printf '  %-58s ok\n' "nothing else is trying to own vt1"
else
    printf '  %-58s ?\n' "getty@tty1 is neither masked nor conflicted"
    hit "something else may own vt1"
fi

# The dated trap: `splash` on the kernel command line makes
# /etc/grub.d/10_linux add vt.handoff=7. With no plymouth to hand back,
# the active VT is then 7, the session on vt1 is INACTIVE, and every
# allow_active grant in the image dies quietly. build/mkimage writes
# grub.cfg itself and deliberately leaves `splash` out; a kernel
# upgrade running zz-update-grub would regenerate it from
# /etc/default/grub, so that file is what is checked.
if grep -qs 'splash' "$RFS/etc/default/grub" && \
   [ ! -d "$RFS/usr/share/plymouth" ]; then
    printf '  %-58s TRAP\n' "/etc/default/grub says splash, with no plymouth"
    printf '      a kernel upgrade regenerates grub.cfg with vt.handoff=7,\n'
    printf '      after which the active VT is 7, the session on vt1 is\n'
    printf '      inactive, and every allow_active grant dies silently.\n'
    hit "splash without plymouth: vt.handoff will deactivate the session"
else
    printf '  %-58s ok\n' "no splash-without-plymouth vt.handoff trap"
fi
[ "$sess" = 1 ] || true

echo
echo "  This section reads files. It cannot see a running system --"
echo "  on a booted image, confirm with:"
echo "    loginctl show-session \$(loginctl list-sessions --no-legend | awk '{print \$1}') \\"
echo "      -p Active -p State -p Class -p Type -p Seat -p VTNr"
echo "    systemctl is-active user@\$(id -u $USER_NAME) ; cat /sys/class/tty/tty0/active"

fail=$(wc -l < "$HITS" 2>/dev/null || echo 0)
fail=$((fail + 0))

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail thing$([ "$fail" -eq 1 ] || echo s) would be refused, or would stop the desktop"
    echo "starting, with nothing on the screen to say so:"
    sed 's/^/    /' "$HITS"
    echo
    echo "The grants are written by build/forge from the profile's"
    echo "allow_* flags. A control the profile forbids should not be on"
    echo "the screen either -- those two have to agree."
    exit 1
fi
echo "all $checked things this computer offers to do are permitted,"
echo "the groups the rest of it uses are in place, the unit can start,"
echo "and nothing granted is a name for nothing."
exit 0
