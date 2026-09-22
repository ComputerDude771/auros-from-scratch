#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  answer.sh — the three things the desktop may ask for as root
#
#  Started by auros-answer.path when a file appears in /run/auros,
#  which is aurshell.service's own RuntimeDirectory: mode 0700, owned
#  by the person using the machine.
#
#  ROOT DOES NOT OPEN A PATH SHE OWNS. That is the whole shape of this
#  script and it is the second attempt at it. The first one read the
#  request with `head -c 64 "$REQ"` and wrote its answer, its log and
#  its report through `$RUN/...` -- as root, through names she
#  controls, with nothing to stop any of them being a symbolic link. An
#  adversarial review reproduced truncate-anything, append-anything and
#  chmod-anything from one request, and a read oracle for root-only
#  files from another. That is a local root escalation, on every built
#  image, reachable by anything running as her.
#
#  So:
#
#    reading  the request is taken by `aurfirst request`, in C, with
#             O_NOFOLLOW and an fstat on the descriptor it reads from
#             -- not a stat on the name, which is a race she wins. See
#             src/aurfirst/request.c.
#    writing  everything goes into /run/auros-answer, which is THIS
#             service's own RuntimeDirectory: root-owned, mode 0755,
#             world-readable. The desktop reads from there and cannot
#             put anything in it.
#
#  The word is still not a command. It is matched against a fixed list
#  of three and anything else is written down as unknown.
# ═══════════════════════════════════════════════════════════════════
set -u
# Overridable for tools/aurfirsttest.sh and for nothing else; the unit
# sets neither.
OUT=${AUROS_ANSWER_OUT:-/run/auros-answer}
STATE=${AUROS_STATE:-/var/lib/auros}
BIN=${AUROS_BIN:-/usr/sbin}
LOG=${AUROS_LOG:-/var/log/auros-answer.log}
RES="$OUT/result"
exec >>"$LOG" 2>&1
echo "=== $(date -Is) ==="

mkdir -p "$OUT" 2>/dev/null
chmod 0755 "$OUT" 2>/dev/null

# ONE WORD, TAKEN SAFELY AND CONSUMED. Exit 1 means there was nothing
# there -- a spurious edge, or a request another run has already dealt
# with -- and there is nothing to answer.
word=$("$BIN/aurfirst" request 2>/dev/null)
rc=$?
[ "$rc" = 1 ] && { echo "nothing to answer"; exit 0; }

say() { printf '%s\n' "$*" >> "$RES.new"; }
: > "$RES.new"
say "request=$word"

case "$word" in
  confirm)
    if "$BIN/aurfirst" confirm 2>&1; then
        say "result=ok"
        say "note=AurOS is now what this computer starts"
    else
        say "result=failed"
        say "note=this computer would not let AurOS change what it starts"
    fi
    ;;
  decline)
    if "$BIN/aurfirst" decline 2>&1; then
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
    if ! "$BIN/aurfirst" ferry >/dev/null 2>&1; then
        say "result=refused"
        say "note=AurOS has not been confirmed yet"
    elif [ ! -x /usr/bin/ferry ]; then
        say "result=failed"
        say "note=this image has no way to import from Windows"
    else
        : > "$OUT/import.log"
        chmod 0644 "$OUT/import.log" 2>/dev/null
        /usr/bin/ferry run --auto >>"$OUT/import.log" 2>&1
        # CAPTURED BEFORE ANYTHING ELSE RUNS. `$?` inside an else
        # branch is the status of the last command executed, which
        # after one line of reporting is the reporting.
        frc=$?
        mkdir -p "$STATE"
        /usr/bin/ferry report > "$STATE/import-report.txt" 2>/dev/null
        cp "$STATE/import-report.txt" "$OUT/import-report.txt" 2>/dev/null
        chmod 0644 "$OUT/import-report.txt" 2>/dev/null
        # FERRY'S 3 IS NOT A PARTIAL IMPORT. ferry-common.sh's refuse()
        # exits 3 before any stage starts -- a hibernated or dirty
        # volume, which Fast Startup makes the COMMON case -- and
        # nothing was read at all. Reporting that as "partial" made the
        # desktop say "Your files are here" on the most likely import
        # failure there is.
        if [ "$frc" = 0 ]; then
            say "result=ok"
        elif [ "$frc" = 3 ]; then
            say "result=refused"
            say "code=3"
            say "note=the Windows drive was not shut down properly"
        else
            say "result=partial"
            say "code=$frc"
        fi
    fi
    ;;
  *)
    say "result=refused"
    say "note=unknown request"
    ;;
esac

# WRITTEN WHOLE, THEN MOVED. The desktop polls this file; one that is
# half written is one it reads as an answer that is missing its result
# line, and then acts on. Both paths are inside a directory root owns,
# so neither can be a link to somewhere else.
chmod 0644 "$RES.new" 2>/dev/null
mv -f "$RES.new" "$RES"
echo "answered: $(cat "$RES")"
exit 0
