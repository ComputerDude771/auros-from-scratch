#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  answer.sh — the three things the desktop may ask for as root
#
#  Started by auros-answer.path when a word appears in
#  /run/auros/answer. That directory is aurshell.service's own
#  RuntimeDirectory: mode 0700, owned by the person using the machine,
#  so nothing else on it can ask.
#
#  THE WORD IS NOT A COMMAND. It is matched against a list of exactly
#  three, and anything else is written down and ignored. A request file
#  that could name a program would be a way for anything that can write
#  one byte into that directory to run as root.
#
#  The answer comes back in /run/auros/answer.result, one key=value per
#  line, because the desktop has to be able to say what happened rather
#  than spin for ever.
# ═══════════════════════════════════════════════════════════════════
set -u
# THE DIRECTORIES ARE OVERRIDABLE; THE PROGRAMS ARE NOT.
#
# tools/aurfirsttest.sh drives this script against a scratch directory
# to check the one thing that is a privilege boundary: which words are
# allowed through. The unit that really starts it sets none of these,
# so on a machine they are the paths below.
#
# What is deliberately NOT configurable is any program this runs. An
# environment that could name the binary would be the injection this
# whole arrangement exists to avoid; the words are matched against a
# fixed list and each branch names an absolute path.
RUN=${AUROS_RUN:-/run/auros}
STATE=${AUROS_STATE:-/var/lib/auros}
REQ="$RUN/answer"
RES="$RUN/answer.result"
LOG=${AUROS_LOG:-/var/log/auros-answer.log}
exec >>"$LOG" 2>&1
echo "=== $(date -Is) ==="

[ -f "$REQ" ] || exit 0
# One word, letters only, at most sixteen of them. Everything else the
# file might contain is discarded here rather than parsed later.
word=$(head -c 64 "$REQ" 2>/dev/null | tr -dc 'a-z' | cut -c1-16)
rm -f "$REQ"
say() { printf '%s\n' "$*" >> "$RES.new"; }
: > "$RES.new"
say "request=$word"

case "$word" in
  confirm)
    if /usr/sbin/aurfirst confirm 2>&1; then
        say "result=ok"
        say "note=AurOS is now what this computer starts"
    else
        say "result=failed"
        say "note=this computer would not let AurOS change what it starts"
    fi
    ;;
  decline)
    if /usr/sbin/aurfirst decline 2>&1; then
        say "result=ok"
        say "note=the next start will reach Windows"
    else
        say "result=failed"
        say "note=this computer would not let AurOS change its start-up setting"
    fi
    ;;
  import)
    # THE GATE IS ASKED, NOT ASSUMED. Copying somebody's documents is
    # the first thing this product does that restarting cannot undo,
    # so it waits until AurOS has been confirmed -- and that decision
    # lives in aurfirst, in one place, rather than being re-derived
    # here from a stamp file this script would have to know about.
    if ! /usr/sbin/aurfirst ferry >/dev/null 2>&1; then
        say "result=refused"
        say "note=AurOS has not been confirmed yet"
    elif [ ! -x /usr/bin/ferry ]; then
        say "result=failed"
        say "note=this image has no way to import from Windows"
    else
        : > "$RUN/import.log"
        /usr/bin/ferry run --auto >>"$RUN/import.log" 2>&1
        # CAPTURED BEFORE ANYTHING ELSE RUNS. `$?` inside an else
        # branch is the status of the last command executed, which
        # after one line of reporting is the reporting. Ferry's own
        # exit codes are meaningful -- 3 is a volume it refused to
        # read, with a sentence saying why -- so losing them loses the
        # only machine-readable thing it said.
        rc=$?
        if [ "$rc" = 0 ]; then
            say "result=ok"
        else
            say "result=partial"
            say "code=$rc"
        fi
        mkdir -p "$STATE"
        /usr/bin/ferry report > "$STATE/import-report.txt" 2>/dev/null
        cp "$STATE/import-report.txt" "$RUN/import-report.txt" 2>/dev/null
        chmod 0644 "$RUN/import-report.txt" 2>/dev/null
    fi
    ;;
  *)
    say "result=refused"
    say "note=unknown request"
    ;;
esac

# WRITTEN WHOLE, THEN MOVED. The desktop polls this file; one that is
# half written is one it reads as an answer that is missing its result
# line, and then acts on.
chmod 0644 "$RES.new" 2>/dev/null
mv -f "$RES.new" "$RES"
echo "answered: $(cat "$RES")"
exit 0
