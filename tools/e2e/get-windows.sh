#!/bin/sh
# get-windows.sh ISO
#
# Downloads the Windows 11 Enterprise evaluation ISO (en-US, x64) from
# Microsoft and prints its SHA-256. The evaluation is free, needs no key,
# and runs 90 days, which is longer than any VM here lives.
#
#   WINDOWS_ISO_URL     Microsoft's link (default: the evaluation center's
#                       fwlink for win11ent64us-iso-dl). When it stops
#                       working, the evaluation center page is searched
#                       for the same link.
#   WINDOWS_ISO_SHA256  if set, the download must hash to it.
set -eu
out=$1
page=https://www.microsoft.com/en-us/evalcenter/download-windows-11-enterprise
url=${WINDOWS_ISO_URL:-"https://go.microsoft.com/fwlink/?LinkId=2382600&clcid=0x409&culture=en-us&country=us"}

resolve() {  # the URL the link finally lands on, by asking for one byte
    curl -sS -L -r 0-0 -o /dev/null -w '%{url_effective} %{http_code}' "$1" || true
}

r=$(resolve "$url")
echo "get-windows: $url"
echo "get-windows:   -> $r"
case "$r" in
    *.iso*" 20"[06]) ;;
    *)
        echo "get-windows: that did not reach an ISO; reading $page"
        link=$(curl -sS -L "$page" | tr '<' '\n' |
               grep 'win11ent64us-iso-dl' | grep -o 'https://go.microsoft.com/fwlink/[^"]*' |
               head -1 | sed 's/&amp;/\&/g')
        [ -n "$link" ] || { echo "get-windows: no en-US ISO link on the page"; exit 1; }
        url=$link
        r=$(resolve "$url")
        echo "get-windows: $url"
        echo "get-windows:   -> $r"
        ;;
esac
final=${r% *}
curl -fsS -L --retry 5 --retry-delay 10 -o "$out" "$final"
ls -l "$out"
sum=$(sha256sum "$out" | cut -d' ' -f1)
echo "get-windows: SHA-256 $sum  $(basename "${final%%\?*}")"
if [ -n "${WINDOWS_ISO_SHA256:-}" ] && [ "$sum" != "$WINDOWS_ISO_SHA256" ]; then
    echo "get-windows: expected $WINDOWS_ISO_SHA256"
    exit 1
fi
