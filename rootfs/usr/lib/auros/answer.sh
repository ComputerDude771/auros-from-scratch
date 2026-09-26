#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  answer.sh — the four things the desktop may ask for as root
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
#  of four and anything else is written down as unknown.
# ═══════════════════════════════════════════════════════════════════
set -u
# Overridable for tools/aurfirsttest.sh and for nothing else; the unit
# sets neither.
OUT=${AUROS_ANSWER_OUT:-/run/auros-answer}
STATE=${AUROS_STATE:-/var/lib/auros}
BIN=${AUROS_BIN:-/usr/sbin}
LOG=${AUROS_LOG:-/var/log/auros-answer.log}
RES="$OUT/result"
GRUBENV=${AUROS_GRUBENV:-/boot/grub/grubenv}
REBOOT=${AUROS_REBOOT:-systemctl --no-block reboot}
# The one entry "Put Windows back" restarts into: the "Yes" inside the
# submenu build/mkimage writes, by id.
PUTBACK_ENTRY='put-windows-back>put-windows-back-yes'
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

# GRUB'S ENVIRONMENT BLOCK, WRITTEN WHOLE. Exactly 1024 bytes, which
# is the format grub's load_env and save_env read and rewrite in place;
# written beside and moved over, so a power cut leaves the old block or
# the new one and never half of each. Written here rather than with
# grub-editenv so that the one thing this depends on is sh.
grubenv_put() { # [key=value]
    t="$GRUBENV.new"
    { printf '# GRUB Environment Block\n'
      if [ -n "${1:-}" ]; then printf '%s\n' "$1"; fi
    } > "$t" || return 1
    have=$(wc -c < "$t")
    [ "$have" -le 1024 ] || { rm -f "$t"; return 1; }
    head -c $((1024 - have)) /dev/zero | tr '\0' '#' >> "$t" || return 1
    sync "$t" 2>/dev/null
    mv -f "$t" "$GRUBENV"
}

# IS THE WAY BACK STILL ON THIS COMPUTER. The restore starts from the
# staging kernel the installer left in \EFI\AurOS on the machine's own
# EFI partition. Looked for read-only, on every EFI partition, and
# unmounted again; AurOS never writes there. AUROS_ESP_ROOTS (tests
# only) names directories to look in instead of mounting anything.
staging_present() {
    if [ -n "${AUROS_ESP_ROOTS:-}" ]; then
        for d in $AUROS_ESP_ROOTS; do
            [ -f "$d/EFI/AurOS/staging.efi" ] && [ -f "$d/EFI/AurOS/staging.img" ] && return 0
        done
        return 1
    fi
    found=1
    for dev in $(lsblk -rno PATH,PARTTYPE 2>/dev/null |
                 awk 'tolower($2)=="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"{print $1}'); do
        # /tmp, which this unit has privately (PrivateTmp=yes): under
        # ProtectSystem=strict, /run outside our own directories is
        # read-only, and a mount point there could not be made -- so
        # every look found nothing and "Put Windows back" said the files
        # were gone on a machine that had them.
        m=$(mktemp -d "${TMPDIR:-/tmp}/auros-esp.XXXXXX") || continue
        if mount -o ro "$dev" "$m" 2>/dev/null; then
            [ -f "$m/EFI/AurOS/staging.efi" ] && [ -f "$m/EFI/AurOS/staging.img" ] && found=0
            umount "$m" 2>/dev/null
        fi
        rmdir "$m" 2>/dev/null
        [ "$found" = 0 ] && return 0
    done
    return 1
}
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
  putback)
    # PUT WINDOWS BACK. Not done here: a restore rewrites the partition
    # table of the disk it runs from, which cannot be done safely from
    # a running AurOS (docs/AURBRIDGE.md, "Putting it back"). This
    # arranges the next start -- AurOS's menu, told to take the restore
    # entry once, and the firmware told to start that menu once -- and
    # restarts. Each step that fails takes the ones before it back: a
    # next_entry left behind would remove AurOS on some later start
    # nobody asked for.
    if ! staging_present; then
        say "result=failed"
        say "note=the files that put Windows back are not on this computer any more"
    elif ! grubenv_put "next_entry=$PUTBACK_ENTRY"; then
        say "result=failed"
        say "note=AurOS could not set its start-up menu"
    elif ! "$BIN/aurfirst" putback 2>&1; then
        grubenv_put
        say "result=failed"
        say "note=this computer would not let AurOS arrange the restart"
    else
        say "result=ok"
        say "note=restarting to put Windows back"
        putback_restart=1
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
# After the answer is written, so the desktop can say what is about to
# happen before it does.
if [ "${putback_restart:-0}" = 1 ]; then
    sleep "${AUROS_REBOOT_DELAY:-3}"
    $REBOOT
fi
exit 0
