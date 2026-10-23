#!/bin/bash
# publish.sh init | variant VARIANT | notes
# Evidence goes to the prerelease othersys-<run id>:
#   https://github.com/<repo>/releases/download/othersys-<run id>/<file>
set -uo pipefail
tag=othersys-${GITHUB_RUN_ID:-local}
url=${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-}
export GH_REPO=${GH_REPO:-$(sh "$(dirname "$0")/../repo.sh")}
[ -n "${GH_TOKEN:-}" ] || { echo "publish: no token; nothing published"; exit 0; }
case $1 in
init)
    gh release delete "$tag" --cleanup-tag -y >/dev/null 2>&1 || true
    gh release create "$tag" --prerelease --target "$GITHUB_SHA" \
        --title "othersys evidence, run $GITHUB_RUN_ID" \
        --notes "Running: $url/actions/runs/$GITHUB_RUN_ID"
    ;;
variant)
    v=$2; out=$OS_DIR/out-$v
    cd "$out" || exit 0
    files=()
    for f in results.md results.json steps.md driver.log seed-grub.cfg seed-user-data layout.json before.json; do
        [ -f "$f" ] && { cp "$f" "$v-$f"; files+=("$v-$f"); }
    done
    for f in serial-*.log; do [ -f "$f" ] && { cp "$f" "$v-$f"; files+=("$v-$f"); }; done
    for f in shots/*-desktop.png shots/*-windows.png shots/*-end.png shots/*-error.png; do
        [ -f "$f" ] && { cp "$f" "$v-$(basename "$f")"; files+=("$v-$(basename "$f")"); }
    done
    ls shots/moknegative-*.png >/dev/null 2>&1 && { cp "$(ls shots/moknegative-*.png | tail -1)" "$v-moknegative-last.png"; files+=("$v-moknegative-last.png"); }
    zip -qr "$v-evidence.zip" . -x '*.zip' -x "$v-*" && files+=("$v-evidence.zip")
    for i in 1 2 3; do
        gh release upload "$tag" "${files[@]}" --clobber && break
        sleep $((i * 10))
    done
    echo "publish: $url/releases/download/$tag/$v-results.md"
    ;;
live)
    v=$2; out=$OS_DIR/out-$v
    while sleep 120; do
        cd "$out" 2>/dev/null || continue
        shot=$(ls -t shots/*.png 2>/dev/null | head -1)
        [ -n "$shot" ] && cp "$shot" "$v-live.png"
        tail -n 40 driver.log > "$v-live.txt" 2>/dev/null
        f=("$v-live.txt"); [ -f "$v-live.png" ] && f+=("$v-live.png")
        gh release upload "$tag" "${f[@]}" --clobber >/dev/null 2>&1 || true
    done
    ;;
esac
exit 0
