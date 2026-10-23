#!/bin/bash
# publish.sh init | live OUT | final OUT
#
# The run's evidence goes to a GitHub prerelease named e2e-<run id>, so it
# can be fetched over plain HTTPS from
#   https://github.com/<repo>/releases/download/e2e-<run id>/<file>
# (artifact storage is a host some sandboxes cannot reach).
#
#   init   creates the prerelease at the start of the job (once per run:
#          every variant's job shares it)
#   live   every few minutes, until killed: the newest screenshot
#          (live-VARIANT.png) and the tail of the driver's log
#          (live-VARIANT.txt), so a run that takes hours can be watched
#   final  every screenshot, the serial logs, the driver log and the
#          timings in e2e-evidence-VARIANT.zip; the timings in the notes.
#          The base variant keeps the newest $E2E_KEEP_RELEASES (default
#          8) e2e-* prereleases and deletes older ones, tags included.
#
# VARIANT is $E2E_VARIANT (default base).
#
# Needs GH_TOKEN with contents: write; without it, does nothing but zip.
set -uo pipefail
what=$1
tag=e2e-${GITHUB_RUN_ID:-local}
url=${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-}
variant=${E2E_VARIANT:-base}
have_gh() { [ -n "${GH_TOKEN:-}" ]; }
# gh works out the repository from the git checkout it runs in; the
# evidence directory is not one
export GH_REPO=${GH_REPO:-${GITHUB_REPOSITORY:-ComputerDude771/auros-from-scratch}}

case $what in
init)
    have_gh || exit 0
    gh release view "$tag" >/dev/null 2>&1 ||
        gh release create "$tag" --prerelease --target "${GITHUB_SHA}" \
            --title "e2e evidence, run ${GITHUB_RUN_ID}" \
            --notes "Running: $url/actions/runs/${GITHUB_RUN_ID}. live-VARIANT.png and live-VARIANT.txt are refreshed every few minutes." ||
        true    # another variant's job made it a moment ago
    echo "publish: $url/releases/tag/$tag"
    ;;
live)
    out=$2
    have_gh || exit 0
    while sleep "${E2E_LIVE_EVERY:-180}"; do
        shot=$(ls -t "$out"/*/shots/*.png 2>/dev/null | head -1)
        [ -n "$shot" ] && cp "$shot" "$out/live-$variant.png"
        { date -u; echo "newest screenshot: ${shot##*/}"; echo
          for f in "$out"/*/timings.md; do [ -f "$f" ] && { echo "## $f"; cat "$f"; echo; }; done
          tail -n 60 "$(ls -t "$out"/*/driver.log 2>/dev/null | head -1)" 2>/dev/null
        } > "$out/live-$variant.txt"
        files=("$out/live-$variant.txt")
        [ -f "$out/live-$variant.png" ] && files+=("$out/live-$variant.png")
        gh release upload "$tag" "${files[@]}" --clobber >/dev/null 2>&1 || true
    done
    ;;
final)
    out=$2
    cd "$out" || exit 1
    {
        echo "## variant: $variant"
        echo
        for d in */; do
            [ -f "$d/timings.md" ] || continue
            echo "### $variant: ${d%/}"
            if [ -f "$d/FAILED.txt" ]; then echo; echo "**FAILED:** $(head -c 1500 "$d/FAILED.txt")"; fi
            echo
            tail -n +3 "$d/timings.md"
            echo
        done
    } > notes.md
    cat notes.md
    zipf=e2e-evidence-$variant.zip
    rm -f "$zipf"
    zip -qr "$zipf" . -x 'e2e-evidence*.zip' -x '*/www/*' -x '*/www-empty/*' -x 'live*'
    ls -l "$zipf"
    have_gh || exit 0
    gh release view "$tag" >/dev/null 2>&1 ||
        gh release create "$tag" --prerelease --target "${GITHUB_SHA}" \
            --title "e2e evidence, run ${GITHUB_RUN_ID}" --notes "-"
    for i in 1 2 3; do
        if gh release upload "$tag" "$zipf" --clobber; then
            # every variant adds its section to the notes
            { gh release view "$tag" --json body -q .body | grep -v '^Running: '; echo; cat notes.md
              echo; echo "Run: $url/actions/runs/${GITHUB_RUN_ID}"; } > notes-all.md
            gh release edit "$tag" --notes-file notes-all.md >/dev/null
            echo "publish: $url/releases/download/$tag/$zipf"
            break
        fi
        sleep $((i * 10))
    done
    [ "$variant" = base ] || exit 0
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
