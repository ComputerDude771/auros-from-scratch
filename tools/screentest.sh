#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  screentest — does the staging screen say where the install is?
#
#  src/aurscreen paints the one restart from the lines aurstage says.
#  It finds the seven steps by the sentence that starts each, so the
#  check that matters most is that those sentences are still in
#  src/aurstage/install.c, word for word: reword one there and the
#  screen would sit on the step before it for the rest of the install,
#  with nothing failing anywhere.
#
#  Then, off-screen (--state, --png), on transcripts:
#    an install part way           the right step, the bar, the warning
#    an install that finished      every step done, no warning
#    a refusal                     "was not installed", in red, verdict kept
#    a restore                     its own title, no install steps
#    garbage on the pipe           no crash
#  and, if a serial log from a real run is given, that it ends handed
#  over with every step reached.
#
#    sh tools/screentest.sh [SERIAL-LOG-OF-A-REAL-INSTALL]
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-62s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-62s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

T=$(mktemp -d "${TMPDIR:-/tmp}/screen.XXXXXX")
trap 'rm -rf "$T"' EXIT
B="$T/aurscreen"
gcc -O2 -std=gnu11 -Wall -Wextra -Werror -o "$B" src/aurscreen/screen.c \
    src/aurshell/kms.c src/aurshell/draw.c src/common/font.c src/common/png.c -lm \
    || { echo "aurscreen did not build"; exit 1; }

echo
echo "The staging screen"
echo
echo "  the sentences it follows are the ones aurstage says"
sed -n '/^static const struct { const char \*starts, \*label; } STEP/,/^};/p' src/aurscreen/screen.c \
    | sed -n 's/^ *{ "\([^"]\+\)", *".*/\1/p' > "$T/starts"
n=$(wc -l < "$T/starts")
[ "$n" -ge 5 ] && ok "read $n step sentences from screen.c" || bad "read the step sentences from screen.c" "got $n"
while IFS= read -r s; do
    grep -qF "stage_say(\"$s" src/aurstage/install.c \
      && ok "install.c says: $s" || bad "install.c says: $s" "reworded or gone"
done < "$T/starts"

say() { printf 'aurstage: %s\n' "$@"; }
part() {
    say "AurOS staging environment" "" "── installing AurOS ──" "drive    healthy"
    say "" "Saving this computer's Windows startup, so it can be put back." "saved    12 MB"
    say "" "Everything AurOS can check has been checked."
    say "" "Making room on the Windows drive. This is the only part" \
        "that cannot be undone. Do not turn the computer off." "windows  61.2 GiB"
    say "" "Copying AurOS onto this computer."
    printf 'aurstage-progress 30\naurstage-progress 47\n'
}
st() { "$B" --mode "$1" --state; }

echo
echo "  states"
S=$(part | st install)
case "$S" in *"step=3 "*"pct=47 "*"stopped=0 "*) ok "part way: copying, 47%, not stopped" ;;
    *) bad "part way: copying, 47%, not stopped" "$S" ;; esac
case "$S" in *"para=Copying AurOS onto this computer."*) ok "...in aurstage's own words" ;;
    *) bad "...in aurstage's own words" "$S" ;; esac

S=$( { part; say "" "Making this computer able to start AurOS." "" \
        "Checking that this computer works under AurOS." "" "Writing the new layout." \
        "handing over to the system on /dev/vda3, in this same boot"; } | st install)
case "$S" in *"step=6 "*"handover=1 "*) ok "finished: last step reached, handed over" ;;
    *) bad "finished: last step reached, handed over" "$S" ;; esac

S=$( { part; say "aurstage-report v1 verdict=copy-failed record=failed -" "" \
        "Windows itself has not been damaged and will still start."; } | st install)
case "$S" in *"stopped=1 "*"verdict=copy-failed"*) ok "a failure: stopped, verdict kept" ;;
    *) bad "a failure: stopped, verdict kept" "$S" ;; esac
case "$S" in *"para=Windows itself has not been damaged"*) ok "...and what to do is what it shows" ;;
    *) bad "...and what to do is what it shows" "$S" ;; esac

S=$( { say "" "Everything AurOS can check has been checked." \
        "aurstage-report v1 verdict=refused-bitlocker record=refused -"; } | st install)
case "$S" in *"stopped=1 "*) ok "a refusal: stopped" ;; *) bad "a refusal: stopped" "$S" ;; esac

S=$( { say "" "Putting Windows back." ; printf 'aurstage-progress 80\n'; } | st restore)
case "$S" in "mode=1 "*"pct=80 "*"stopped=0"*) ok "a restore: its own mode, its own bar" ;;
    *) bad "a restore: its own mode, its own bar" "$S" ;; esac

S=$( { head -c 20000 /dev/urandom; printf '\naurstage-progress 999\naurstage-progress -5\n'; } | st install)
case "$S" in *"pct=0 "*) ok "garbage and out-of-range numbers: no crash, clamped" ;;
    *) bad "garbage and out-of-range numbers: no crash, clamped" "$S" ;; esac

echo
echo "  pictures"
for sz in "1024 768" "1366 768" "1920 1080" "800 600"; do
    set -- $sz
    if part | "$B" --mode install --png "$T/s.png" --size "$1" "$2" 2>/dev/null && \
       [ "$(head -c 8 "$T/s.png" | od -An -c | tr -d ' ')" = '211PNG\r\n032\n' ]; then
        ok "paints at $1x$2"
    else bad "paints at $1x$2"; fi
done

if [ -n "${1:-}" ] && [ -f "$1" ]; then
    echo
    echo "  a real install's serial log: $1"
    S=$(tr -d '\r' < "$1" | grep -a '^aurstage' | st install)
    case "$S" in *"step=6 "*"handover=1 "*) ok "followed it to the end: every step, handed over" ;;
        *) bad "followed it to the end: every step, handed over" "$S" ;; esac
fi

echo
echo "screentest: $((checked - fail))/$checked"
[ "$fail" = 0 ]
