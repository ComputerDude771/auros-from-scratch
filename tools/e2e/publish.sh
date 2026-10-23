#!/bin/bash
# publish.sh OUT
#
# Puts the run's evidence (every screenshot, the serial logs, the driver
# log, timings) in one zip and attaches it to a GitHub prerelease named
# e2e-<run id>, so it can be fetched with plain HTTPS from
# https://github.com/<repo>/releases/download/e2e-<run id>/e2e-evidence.zip
# (artifact storage is a host some sandboxes cannot reach). Keeps the
# newest $E2E_KEEP_RELEASES (default 8) such prereleases and deletes the
# rest, tags included. Needs GH_TOKEN with contents: write.
set -uo pipefail
out=$1
tag=e2e-${GITHUB_RUN_ID:-local}
cd "$out" || exit 1

{
    echo "End-to-end run ${GITHUB_RUN_ID:-} on \`${GITHUB_SHA:-?}\`"
    echo "(${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-})."
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
zip -qr e2e-evidence.zip . -x 'e2e-evidence.zip' -x '*/www/*' -x '*/www-empty/*'
ls -l e2e-evidence.zip

if [ -z "${GH_TOKEN:-}" ]; then
    echo "publish: no GH_TOKEN, leaving $out/e2e-evidence.zip"
    exit 0
fi
gh release delete "$tag" --cleanup-tag -y >/dev/null 2>&1 || true
for i in 1 2 3; do
    if gh release create "$tag" --prerelease --target "${GITHUB_SHA}" \
        --title "e2e evidence, run ${GITHUB_RUN_ID}" --notes-file notes.md \
        e2e-evidence.zip 2>&1; then
        echo "publish: ${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}/releases/download/$tag/e2e-evidence.zip"
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
exit 0
