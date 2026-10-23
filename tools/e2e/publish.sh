#!/bin/bash
# publish.sh init | live OUT | final OUT
#
# The run's evidence goes to a GitHub prerelease named e2e-<run id>, so it
# can be fetched over plain HTTPS from
#   https://github.com/<repo>/releases/download/e2e-<run id>/<file>
# (artifact storage is a host some sandboxes cannot reach).
#
#   init   creates the prerelease at the start of the job
#   live   every few minutes, until killed: the newest screenshot
#          (live.png) and the tail of the driver's log (live.txt), so a
#          run that takes hours can be watched while it runs
#   final  every screenshot, the serial logs, the driver log and the
#          timings in e2e-evidence.zip; the timings in the notes. Keeps
#          the newest $E2E_KEEP_RELEASES (default 8) e2e-* prereleases
#          and deletes older ones, tags included.
#
# Needs GH_TOKEN with contents: write; without it, does nothing but zip.
set -uo pipefail
what=$1
tag=e2e-${GITHUB_RUN_ID:-local}
url=${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-}
have_gh() { [ -n "${GH_TOKEN:-}" ]; }
# gh works out the repository from the git checkout it runs in; the
# evidence directory is not one
export GH_REPO=${GH_REPO:-${GITHUB_REPOSITORY:-ComputerDude771/auros-from-scratch}}

case $what in
init)
    have_gh || exit 0
    gh release delete "$tag" --cleanup-tag -y >/dev/null 2>&1 || true
    gh release create "$tag" --prerelease --target "${GITHUB_SHA}" \
        --title "e2e evidence, run ${GITHUB_RUN_ID}" \
        --notes "Running: $url/actions/runs/${GITHUB_RUN_ID}. live.png and live.txt are refreshed every few minutes."
    echo "publish: $url/releases/tag/$tag"
    ;;
live)
    out=$2
    have_gh || exit 0
    while sleep "${E2E_LIVE_EVERY:-180}"; do
        shot=$(ls -t "$out"/*/shots/*.png 2>/dev/null | head -1)
        [ -n "$shot" ] && cp "$shot" "$out/live.png"
        { date -u; echo "newest screenshot: ${shot##*/}"; echo
          for f in "$out"/*/timings.md; do [ -f "$f" ] && { echo "## $f"; cat "$f"; echo; }; done
          tail -n 60 "$(ls -t "$out"/*/driver.log 2>/dev/null | head -1)" 2>/dev/null
        } > "$out/live.txt"
        files=("$out/live.txt")
        [ -f "$out/live.png" ] && files+=("$out/live.png")
        gh release upload "$tag" "${files[@]}" --clobber >/dev/null 2>&1 || true
    done
    ;;
final)
    out=$2
    cd "$out" || exit 1
    {
        echo "End-to-end run ${GITHUB_RUN_ID:-} on \`${GITHUB_SHA:-?}\`"
        echo "($url/actions/runs/${GITHUB_RUN_ID:-})."
        echo
        for d in */; do
            [ -f "$d/timings.md" ] || continue
            echo "### ${d%/}"
            if [ -f "$d/FAILED.txt" ]; then echo; echo "**FAILED:** $(head -c 1500 "$d/FAILED.txt")"; fi
            echo
            tail -n +3 "$d/timings.md"
            echo
        done
    } > notes.md
    cat notes.md
    rm -f e2e-evidence.zip
    zip -qr e2e-evidence.zip . -x 'e2e-evidence.zip' -x '*/www/*' -x '*/www-empty/*' -x 'live.*'
    ls -l e2e-evidence.zip
    have_gh || exit 0
    gh release view "$tag" >/dev/null 2>&1 ||
        gh release create "$tag" --prerelease --target "${GITHUB_SHA}" \
            --title "e2e evidence, run ${GITHUB_RUN_ID}" --notes "-"
    for i in 1 2 3; do
        if gh release upload "$tag" e2e-evidence.zip --clobber; then
            gh release edit "$tag" --notes-file notes.md >/dev/null
            echo "publish: $url/releases/download/$tag/e2e-evidence.zip"
            break
        fi
        sleep $((i * 10))
    done
    keep=${E2E_KEEP_RELEASES:-8}
    gh release list --limit 200 --json tagName,createdAt \
        --jq '[.[] | select(.tagName | startswith("e2e-"))] | sort_by(.createdAt) | reverse | .[].tagName' |
        tail -n +$((keep + 1)) | while read -r old; do
            gh release delete "$old" --cleanup-tag -y && echo "publish: removed old $old"
        done
    ;;
*)
    echo "usage: publish.sh init | live OUT | final OUT" >&2
    exit 2
    ;;
esac
exit 0
