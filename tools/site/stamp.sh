#!/bin/sh
# sh tools/site/stamp.sh SRC DST
#
# Copies the website from SRC to DST with the repository it is published
# from written in, so the site never names a repository in the tree and
# a move to another repository edits nothing (docs/MOVING.md). The pages
# hold two placeholders, filled from tools/repo.sh (AUROS_REPO, else
# GITHUB_REPOSITORY, else the origin remote):
#
#   @AUROS_REPO@      owner/name, as in https://github.com/@AUROS_REPO@/...
#   @AUROS_ADDRESS@   the site's address as she would write it down,
#                     owner.github.io/name (no https://, no last slash)
#
# pages.yml stamps before it checks and before it publishes; check.mjs
# stamps the copy it serves. The download button also finds its address
# from the page's own address on GitHub Pages (assets/door.js), so a site
# published without this step still downloads the right file. Refuses
# to leave a placeholder behind.
set -eu
[ $# = 2 ] || { echo "usage: sh tools/site/stamp.sh SRC DST" >&2; exit 2; }
src=$1; dst=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(sh "$here/../repo.sh" repo)
address=$(sh "$here/../repo.sh" host | sed 's|/$||')
[ -d "$src" ] || { echo "stamp: $src is not a folder" >&2; exit 1; }
rm -rf "$dst"; mkdir -p "$dst"
cp -R "$src"/. "$dst"/
# tools/repo.sh lets through only [A-Za-z0-9._/-], none of which sed's
# replacement text treats specially with | as the separator.
grep -rlI -e '@AUROS_' "$dst" | while IFS= read -r f; do
    sed -i -e "s|@AUROS_REPO@|$repo|g" -e "s|@AUROS_ADDRESS@|$address|g" "$f"
done
if left=$(grep -rnI -e '@AUROS_[A-Z_]*@' "$dst"); then
    echo "stamp: placeholders nobody fills:" >&2; echo "$left" >&2; exit 1
fi
echo "stamp: $dst is the site of $repo ($address)"
