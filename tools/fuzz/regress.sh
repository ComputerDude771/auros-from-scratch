#!/bin/sh
# regress.sh -- every fuzz finding, re-checked, with nothing but gcc.
#
#   sh tools/fuzz/regress.sh [OUT]
#
# Builds each harness with ASan + UBSan and standalone.c's main() (no
# libFuzzer needed), then runs it over its seed corpus AND over
# tools/fuzz/regress/<target>/, where every input that ever crashed a
# harness is kept. Any sanitizer report aborts that harness, and the
# script exits non-zero.
#
# A file in regress/ is the proof a fix stays fixed: delete the fix and
# this script fails on that file.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${1:-${OUT:-/tmp/auros-fuzz-regress}}"
mkdir -p "$OUT"
blog=$(MODE=regress sh "$HERE/build.sh" "$OUT" 2>&1) || { printf '%s\n' "$blog"; echo "FAIL: build"; exit 1; }

: "${TMPDIR:=/tmp}"; export TMPDIR
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

fail=0; ran=0
for bin in "$OUT"/fuzz_*-regress; do
    [ -x "$bin" ] || continue
    t=$(basename "$bin" -regress); t=${t#fuzz_}
    set --
    [ -d "$HERE/corpus/$t" ]  && set -- "$@" "$HERE/corpus/$t"
    [ -d "$HERE/regress/$t" ] && set -- "$@" "$HERE/regress/$t"
    [ $# -gt 0 ] || continue
    if out=$("$bin" "$@" 2>&1); then
        printf 'ok    %-8s %s\n' "$t" "$(printf '%s\n' "$out" | tail -1)"
        ran=$((ran + 1))
    else
        printf 'FAIL  %-8s\n' "$t"
        printf '%s\n' "$out" | grep -E 'ERROR|runtime error|SUMMARY|#[0-9] ' | head -12 | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done
[ $ran -gt 0 ] || { echo "FAIL: no harness ran"; exit 1; }
[ $fail -eq 0 ] && echo "PASS: $ran harness(es), no sanitizer reports" || echo "FAIL: $fail harness(es)"
exit $fail
