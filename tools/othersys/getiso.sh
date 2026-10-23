#!/bin/bash
# getiso.sh DIR: the official ISO, checked the way AurOS's Windows side
# must check it: Canonical's signature on SHA256SUMS (pinned key), the
# pinned hash in that file, then the ISO's own hash.
set -euo pipefail
dir=$1
here=$(cd "$(dirname "$0")" && pwd)
sys=$here/../../src/othersys/systems/${2:-ubuntu-26.04.1-desktop}.json
get() { python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" "$sys" "$1"; }
mkdir -p "$dir"
curl -fsSL --retry 5 -o "$dir/SHA256SUMS" "$(get sums_url)"
curl -fsSL --retry 5 -o "$dir/SHA256SUMS.gpg" "$(get sums_sig_url)"
python3 -I "$here/../../src/othersys/verify.py" "$sys" "$dir/SHA256SUMS" "$dir/SHA256SUMS.gpg"
iso=$dir/$(get iso_name)
t0=$(date +%s)
curl -fsSL --retry 5 -C - -o "$iso" "$(get iso_url)" || curl -fsSL --retry 5 -o "$iso" "$(get iso_url)"
t1=$(date +%s)
python3 -I "$here/../../src/othersys/verify.py" "$sys" "$dir/SHA256SUMS" "$dir/SHA256SUMS.gpg" "$iso"
echo "getiso: $(stat -c %s "$iso") bytes in $((t1 - t0)) s, verified in $(( $(date +%s) - t1 )) s"
echo "ISO=$iso"
