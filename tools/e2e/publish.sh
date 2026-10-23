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
# WHERE IT GOES. Making the run's prerelease has failed with "HTTP 403:
# Resource not accessible by integration" for runs whose token could make
# one minutes earlier, and a green step that published nothing is how a
# run's evidence was lost. So init tries, in order: e2e-<run> at the
# run's commit; e2e-<run> at the default branch; and last, the newest
# existing e2e-* prerelease, with this run's files named r<run>-<file>.
# It writes the choice to $STATE for live and final, puts the links in
# the job summary, and says ::warning:: when it had to fall back or
# could not publish at all.
#
# Needs GH_TOKEN with contents: write; without it, does nothing but zip.
set -uo pipefail
what=$1
run=${GITHUB_RUN_ID:-local}
url=${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-}
variant=${E2E_VARIANT:-base}
STATE=${E2E_PUBLISH_STATE:-${RUNNER_TEMP:-/tmp}/e2e-publish-$run.state}
have_gh() { [ -n "${GH_TOKEN:-}" ]; }
# gh works out the repository from the git checkout it runs in; the
# evidence directory is not one
export GH_REPO=${GH_REPO:-$(sh "$(dirname "$0")/../repo.sh")}

notes0="Running: $url/actions/runs/$run. live-VARIANT.png and live-VARIANT.txt are refreshed every few minutes."
try_create() {  # TARGET -- e2e-<run> at TARGET; prints gh's error
    gh release create "e2e-$run" --prerelease --target "$1" \
        --title "e2e evidence, run $run" --notes "$notes0" >/dev/null
}
pick_release() {  # sets tag, prefix, own (1: the release is this run's)
    tag="" prefix="" own=0
    if [ -f "$STATE" ]; then . "$STATE"; [ -n "$tag" ] && return 0; fi
    if gh release view "e2e-$run" >/dev/null 2>&1 || try_create "${GITHUB_SHA:-HEAD}"; then
        tag=e2e-$run own=1
    else
        sleep 5   # another variant's job may have made it meanwhile
        if gh release view "e2e-$run" >/dev/null 2>&1; then
            tag=e2e-$run own=1
        else
            def=$(gh api "repos/$GH_REPO" --jq .default_branch 2>/dev/null)
            if [ -n "$def" ] && try_create "$def"; then
                tag=e2e-$run own=1
                echo "::warning::e2e evidence: e2e-$run could not be made at ${GITHUB_SHA:-?}; it was made at $def instead (the files are this run's)"
            else
                tag=$(gh release list --limit 50 --json tagName,createdAt \
                      --jq '[.[] | select(.tagName | startswith("e2e-"))] | sort_by(.createdAt) | reverse | .[0].tagName' 2>/dev/null)
                prefix=r$run-
                if [ -n "$tag" ]; then
                    echo "::warning::e2e evidence: no release could be made for this run; its files go into $tag as ${prefix}*"
                else
                    echo "::warning::e2e evidence: no release could be made and none exists; nothing will be published (the Actions artifact still has it)"
                    tag=none
                fi
            fi
        fi
    fi
    printf 'tag=%s\nprefix=%s\nown=%s\n' "$tag" "$prefix" "$own" > "$STATE"
}

case $what in
init)
    have_gh || exit 0
    pick_release
    [ "$tag" = none ] && exit 0
    base_url=$url/releases/download/$tag/$prefix
    echo "publish: ${base_url}live-$variant.png"
    { echo "### e2e evidence ($variant)"
      echo "- live: ${base_url}live-$variant.png and ${base_url}live-$variant.txt"
      echo "- after the job: ${base_url}e2e-evidence-$variant.zip"
    } >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    ;;
live)
    out=$2
    have_gh || exit 0
    pick_release
    [ "$tag" = none ] && exit 0
    while sleep "${E2E_LIVE_EVERY:-180}"; do
        shot=$(ls -t "$out"/*/shots/*.png 2>/dev/null | head -1)
        [ -n "$shot" ] && cp "$shot" "$out/live-$variant.png"
        { date -u; echo "newest screenshot: ${shot##*/}"; echo
          for f in "$out"/*/timings.md; do [ -f "$f" ] && { echo "## $f"; cat "$f"; echo; }; done
          tail -n 60 "$(ls -t "$out"/*/driver.log 2>/dev/null | head -1)" 2>/dev/null
        } > "$out/live-$variant.txt"
        # (gh names an asset after its file; FILE#LABEL only sets a label)
        if [ -n "$prefix" ]; then
            cp "$out/live-$variant.txt" "$out/${prefix}live-$variant.txt"
            files=("$out/${prefix}live-$variant.txt")
            [ -f "$out/live-$variant.png" ] && cp "$out/live-$variant.png" "$out/${prefix}live-$variant.png" &&
                files+=("$out/${prefix}live-$variant.png")
        else
            files=("$out/live-$variant.txt"); [ -f "$out/live-$variant.png" ] && files+=("$out/live-$variant.png")
        fi
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
    zip -qr "$zipf" . -x 'e2e-evidence*.zip' -x '*/www/*' -x '*/www-empty/*' -x 'live*' -x 'r[0-9]*-*'
    ls -l "$zipf"
    have_gh || exit 0
    pick_release
    [ "$tag" = none ] && exit 0
    up=$zipf notesf=notes-$variant.md
    cp notes.md "$notesf"
    if [ -n "$prefix" ]; then
        cp "$zipf" "$prefix$zipf"; cp notes.md "$prefix$notesf"; up=$prefix$zipf notesf=$prefix$notesf
    fi
    ok=0
    for i in 1 2 3; do
        if gh release upload "$tag" "$up" "$notesf" --clobber; then
            ok=1
            echo "publish: $url/releases/download/$tag/$up"
            break
        fi
        sleep $((i * 10))
    done
    [ $ok = 1 ] || { echo "::warning::e2e evidence: the upload to $tag failed (the Actions artifact still has it)"; exit 0; }
    # the notes are this run's only when the release is (every variant
    # adds its section); another run's release keeps its own
    if [ "$own" = 1 ]; then
        { gh release view "$tag" --json body -q .body | grep -v '^Running: '; echo; cat notes.md
          echo; echo "Run: $url/actions/runs/$run"; } > notes-all.md
        gh release edit "$tag" --notes-file notes-all.md >/dev/null
    fi
    [ "$variant" = base ] && [ "$own" = 1 ] || exit 0
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
