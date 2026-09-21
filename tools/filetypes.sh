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

APPS=/usr/share/applications
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

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail file type$([ "$fail" -eq 1 ] || echo s) open nothing."
    echo "The name in $FORGE has to match the .desktop the package really"
    echo "ships -- check with: dpkg -L PACKAGE | grep applications"
    exit 1
fi
echo "all $checked file types open a program that is installed"
exit 0
