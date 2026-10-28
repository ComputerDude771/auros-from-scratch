#!/bin/sh
# The wizard, checked on Linux under Wine (no Windows needed):
#   build, --navtest (every page's rules), --selftest (the real window,
#   as a screen reader asks it), the console selftest, and every page
#   rendered to PNG in out/wizard-shots/ for you to look at.
# Needs: mingw-w64, wine, xvfb (xvfb-run), python3.
#   sh tools/wizard/check.sh            # from the repository root
# To refresh the committed pictures afterwards:
#   cp out/wizard-shots/*.png docs/shots/wizard/
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
sh build/aurbridge >/dev/null 2>&1 || { sh build/aurbridge; echo "BUILD FAILED"; exit 1; }
# Scratch in RAM when there is some: the BMPs are large and disks fill.
T=${TMPDIR:-/tmp}; [ -d /dev/shm ] && T=/dev/shm
R="$T/wizard-check.$$"; rm -rf "$R"; mkdir -p "$R/shots" "$R/nav" "$R/self"
W="WINEDEBUG=-all"
fail=0

env $W timeout 300 xvfb-run -a wine out/aurbridge-wizard.exe --navtest "Z:$R/nav" >/dev/null 2>&1
cat "$R/nav/navtest.log" 2>/dev/null || { echo "navtest wrote no log"; fail=1; }
grep -q "^ALL PASS" "$R/nav/navtest.log" 2>/dev/null || fail=1

env $W timeout 200 xvfb-run -a wine out/aurbridge-wizard.exe --selftest "Z:$R/self" >/dev/null 2>&1
echo; cat "$R/self/selftest.log" 2>/dev/null || { echo "selftest wrote no log"; fail=1; }
grep -Eq 'screen reader    : "[^"]{10,}", [1-9][0-9]* parts, button "[^"]+"' "$R/self/selftest.log" 2>/dev/null \
    || { echo "a screen reader gets nothing from the window"; fail=1; }

echo; env $W timeout 120 wine out/aurbridge.exe selftest 2>&1 | grep -E "FAIL|all checks" || fail=1

env $W timeout 300 xvfb-run -a wine out/aurbridge-wizard.exe --shot "Z:$R/shots" >/dev/null 2>&1
mkdir -p out/wizard-shots; rm -f out/wizard-shots/*.png
python3 tools/wizard/bmp2png.py --out out/wizard-shots "$R"/shots/*.bmp >/dev/null
echo; echo "$(ls out/wizard-shots | wc -l) pages rendered in out/wizard-shots/"
rm -rf "$R"
[ $fail = 0 ] && echo "WIZARD CHECK: PASS" || echo "WIZARD CHECK: FAIL"
exit $fail
