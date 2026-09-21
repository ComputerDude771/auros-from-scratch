#!/bin/sh
# filetypes.sh — does every file type open something that exists?
#
# build/forge writes an /etc/xdg/mimeapps.list saying which program
# handles which kind of file. Every line in it names a .desktop file. If
# that .desktop does not exist -- because the package renamed it, or was
# never in packages_files, or the name was simply guessed -- the line
# does nothing at all. Nothing warns. The file just opens with whatever
# the database happens to rank first, or with nothing.
#
# This is not hypothetical. Two of the six entries were wrong the first
# time they were written: ristretto ships org.xfce.ristretto.desktop and
# mousepad ships org.xfce.mousepad.desktop, not the names anyone would
# assume. A .deb that opens an archive manager instead of an installer
# is how a person ends up with a folder full of files and no program.
#
#   sh tools/filetypes.sh
#
# Needs the packages in packages_files installed on THIS machine, since
# the check is "does this .desktop exist". Exits 2 and says so if they
# are not, rather than passing on an empty search.
set -u
cd "$(dirname "$0")/.."

# Overridable so this can also be run against a rootfs a build has just
# produced, rather than only against the machine it is running on.
APPS="${APPS:-/usr/share/applications}"
FORGE=build/forge

# The mimeapps.list block, taken out of forge rather than copied -- a
# second copy is a second thing to keep in step.
LIST=$(awk '/^\[Default Applications\]$/{f=1} f{print} /^EOL$/{if(f)exit}' "$FORGE" \
       | grep -E '^[a-z].*=.*\.desktop$')

if [ -z "$LIST" ]; then
    echo "could not find the file-type table in $FORGE"
    exit 2
fi

# Which .desktop files this machine has at all. If it has almost none,
# the packages are not installed and every check below would "pass" by
# finding nothing to contradict.
have=$(ls "$APPS"/*.desktop 2>/dev/null | wc -l)
if [ "$have" -lt 5 ]; then
    echo "only $have .desktop files on this machine -- the packages in"
    echo "packages_files are not installed, so this check cannot run."
    echo "  sudo apt-get install thunar gdebi ristretto atril mousepad xarchiver"
    exit 2
fi

fail=0
checked=0
echo "does every file type open something that exists?"

# Everything named in the table must be a file that is really there.
for line in $LIST; do
    mime=${line%%=*}
    desk=${line#*=}
    checked=$((checked + 1))
    if [ -f "$APPS/$desk" ]; then
        printf '  %-42s %s\n' "$mime" "$desk"
    else
        printf '  %-42s %s   MISSING\n' "$mime" "$desk"
        fail=$((fail + 1))
    fi
done

# And the program each one names must exist too: a .desktop whose Exec
# points at something not installed opens nothing and says nothing.
echo
echo "and does each of those actually run something?"
for line in $LIST; do
    desk=${line#*=}
    [ -f "$APPS/$desk" ] || continue
    ex=$(sed -n 's/^Exec=\([^ %]*\).*/\1/p' "$APPS/$desk" | head -1)
    [ -n "$ex" ] || continue
    case "$ex" in
        /*) [ -x "$ex" ] && continue ;;
        *)  command -v "$ex" >/dev/null 2>&1 && continue ;;
    esac
    printf '  %-42s %s is not installed\n' "$desk" "$ex"
    fail=$((fail + 1))
done

# The one that matters most, stated on its own because it is the whole
# "download things like any other Linux distro" path.
echo
deb=$(printf '%s\n' "$LIST" | sed -n 's/^application\/vnd\.debian\.binary-package=//p')
if [ -z "$deb" ]; then
    echo "  nothing claims a downloaded .deb -- she cannot install anything"
    echo "  she downloads, which is how Chrome is actually installed."
    fail=$((fail + 1))
elif [ -f "$APPS/$deb" ]; then
    echo "  a downloaded .deb opens $deb"
else
    echo "  a downloaded .deb opens $deb, which does not exist"
    fail=$((fail + 1))
fi

# ── and is each one CALLED something she would recognise ───────────
#
# forge also ships a plain-language rename for the handful of programs
# AurOS installs: an override .desktop in /usr/local/share/applications,
# which the XDG search order puts ahead of the packager's.
#
# Two ways that silently does nothing, and both have happened:
#
#   the name is wrong    the table said "gnome-software" and the archive
#                        ships org.gnome.Software.desktop, so the line
#                        renamed a file that is not there
#
#   the name is right    but the package also ships Name[en_US], and the
#   and it still loses   XDG rule -- which src/aurshell/apps.c follows
#                        correctly -- is that an exact language-and-
#                        country match beats the plain Name=. atril does
#                        exactly this, so a freshly booted image read
#                        "Atril Document Viewer" next to "Your files"
#
# Neither is visible in the build output. Both are visible here.
echo
echo "and is each program called something she would recognise?"
RENAMES=$(awk "/^thunar\\|/,/^EOL$/" "$FORGE" | grep -E '^[a-zA-Z0-9._-]+\|')
rn_fail=0
if [ -z "$RENAMES" ]; then
    echo "  could not find the rename table in $FORGE"
    rn_fail=1
else
    for line in $(printf '%s\n' "$RENAMES" | cut -d'|' -f1); do
        if [ -f "$APPS/$line.desktop" ]; then
            printf '  %-42s %s\n' "$line" "installed"
        else
            printf '  %-42s %s\n' "$line" "NOT INSTALLED -- this line renames nothing"
            rn_fail=$((rn_fail + 1))
        fi
    done
fi

# And the rename has to out-rank what the package itself ships. Checked
# against forge rather than against the packages, because it is a
# property of the transform and not of any one .desktop: whatever a
# package ships, an override that leaves Name[en_US] in place loses to
# it on the machine this product is built for.
echo
echo "does the rename out-rank the name the package ships?"
if awk "/^    install -d .*local\\/share\\/applications/,/^EOL$/" "$FORGE" \
     | grep -q "Name\\\\\\[/d"; then
    echo "  ok    the override drops the packagers' localised names"
else
    echo "  the override keeps Name[..] lines, so on an en_US machine the"
    echo "  packager's English name wins and the rename is invisible"
    rn_fail=$((rn_fail + 1))
fi
fail=$((fail + rn_fail))

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail thing$([ "$fail" -eq 1 ] || echo s) in $FORGE point at nothing,"
    echo "or are overridden by a name the package also ships. The name has to"
    echo "match the .desktop the package really ships -- check with:"
    echo "  dpkg -L PACKAGE | grep applications"
    exit 1
fi
echo "all $checked file types open a program that is installed,"
echo "and every rename reaches the screen"
exit 0
