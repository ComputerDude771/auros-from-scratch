#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  AurOS: make "Get more programs" ready, the first time it is online
#
#  Run by auros-software.service until it has worked once, then never
#  again (/var/lib/auros/software-ready).
#
#    1. Flathub, if the image does not already have it. The build adds
#       it when the builder can reach dl.flathub.org (GitHub's runners
#       can); otherwise it is added here. From the copy the build saved
#       if there is one, else from Flathub itself over https -- the
#       same command Flathub's own setup page gives everybody.
#    2. Flathub's catalogue, so a search for "Zoom" finds Zoom on the
#       first try instead of an empty page.
#    3. apt's lists and the archive's catalogue (apt runs appstream's
#       hook), which the image deliberately does not carry.
#
#  Every step is allowed to fail; the whole thing is tried again a
#  minute later, for as long as it takes. Nothing here can hold up the
#  desktop: the unit is Type=simple and waits for the network itself.
# ═══════════════════════════════════════════════════════════════════
set -u
STAMP=/var/lib/auros/software-ready
SAVED=/usr/share/auros/flathub.flatpakrepo
URL=https://dl.flathub.org/repo/flathub.flatpakrepo

say() { printf 'software-ready: %s\n' "$*"; }

online() {
    # NetworkManager's own answer first: it knows when the wifi is up.
    # nm-online exits 0 at once when it is, and waits up to the
    # timeout when it is not.
    if command -v nm-online >/dev/null 2>&1; then
        nm-online -q -t 60 >/dev/null 2>&1 || return 1
    fi
    return 0
}

have_flathub() {
    flatpak remotes --system --columns=name 2>/dev/null | grep -qx flathub
}

while :; do
    if online; then
        ok=1
        if command -v flatpak >/dev/null 2>&1; then
            if ! have_flathub; then
                if [ -s "$SAVED" ]; then src=$SAVED; else src=$URL; fi
                if flatpak remote-add --system --if-not-exists flathub "$src"; then
                    say "Flathub added (from $src)"
                else
                    say "could not add Flathub yet"; ok=0
                fi
            fi
            if have_flathub; then
                flatpak update --system --appstream flathub >/dev/null 2>&1 \
                    && say "Flathub's catalogue fetched" \
                    || { say "Flathub's catalogue: not yet"; ok=0; }
            fi
        fi
        if command -v apt-get >/dev/null 2>&1; then
            apt-get -q update >/dev/null 2>&1 \
                && say "apt's lists fetched" \
                || { say "apt's lists: not yet"; ok=0; }
        fi
        if [ "$ok" = 1 ]; then
            mkdir -p /var/lib/auros
            date -Is > "$STAMP"
            say "ready"
            exit 0
        fi
    fi
    sleep 60
done
