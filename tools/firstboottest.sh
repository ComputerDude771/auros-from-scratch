#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  firstboottest — does the real AurOS ask, and does the answer stick?
#
#  Phases 9 and 10 are the only part of this product that decides what
#  the computer starts FOR GOOD. Until somebody says AurOS works, every
#  power-on must reach Windows unless AurOS has been asked for once
#  more; the moment they say it works, AurOS becomes the default and
#  Windows stays in the menu; the moment they say it does not, the next
#  power-on reaches Windows and nothing ever re-arms.
#
#  Every one of those sentences was, until this test, true only of a
#  unit test: aurfirsttest drives aurfirst against a directory that
#  pretends to be efivarfs, and welcometest drives the panel against a
#  directory that pretends to be /run. Neither is the real thing.
#  installtest and loadertest boot the real INSTALLER, but the system
#  they hand over to is a 334 MiB ext4 whose init prints one line --
#  enough to prove the boot chain, and not a single systemd unit.
#
#  This one boots the REAL image -- out/auros-<profile>.img.zst, the
#  artifact build/all publishes, byte for byte what AurBridge writes --
#  under OVMF with NVRAM that persists between boots, in the state an
#  install leaves the firmware in:
#
#      Boot0000  "Windows Boot Manager"   first in BootOrder
#      Boot0002  "AurOS"                  HD(ESP)/\EFI\AurOS\shimx64.efi
#      BootNext  0002                     the one restart
#
#  and then reads, after every boot, the two things that cannot lie
#  about it: OVMF's own serial console, which names each entry it tries
#  and the one it STARTS, and the variable store the next boot will
#  read.
#
#  WHAT IS NOT REAL, AND WHY. One thing in the booted system is not
#  AurOS: a unit that stands in for a person pressing one button. It
#  writes one word into /run/auros/answer, as the desktop user, with
#  O_EXCL and mode 0600 -- exactly what src/aurshell/welcome.c does --
#  and then powers the machine off. Everything from that file onwards
#  is the product: auros-answer.path, answer.sh running as root,
#  aurfirst reading the request through O_NOFOLLOW, BootOrder, the
#  stamp. The panel's own drawing and hit-testing are welcometest's.
#
#  WHAT IS NOT PROVEN: a real firmware, which will not print what it
#  did and may reorder or prune the boot menu in ways OVMF does not.
#
#    sudo sh tools/firstboottest.sh [profile]     (default: desktop)
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

PROFILE="${1:-desktop}"
E=tools/efivarstore.py
CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
BOOT_TIMEOUT="${FIRSTBOOT_TIMEOUT:-1200}"   # seconds; TCG boots a whole desktop

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-60s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-60s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

for t in qemu-system-x86_64 zstd sgdisk python3 losetup mount runuser; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ -f "$CODE" ] && [ -f "$VARS" ] || { echo "need OVMF"; exit 2; }
SRC="out/auros-$PROFILE.img.zst"
[ -f "$SRC" ] || [ -f "out/auros-$PROFILE.img" ] \
    || { echo "no image for $PROFILE: run ./build/all $PROFILE"; exit 2; }

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another end-to-end test is running"; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/firstboot.XXXXXX")
[ -n "$T" ] && [ -d "$T" ] || { rmdir "$LOCK"; echo "no scratch dir"; exit 2; }
QP=""
cleanup() {
    [ -n "$QP" ] && kill -9 "$QP" 2>/dev/null
    mountpoint -q "$T/m" 2>/dev/null && umount "$T/m"
    [ -n "${LP:-}" ] && losetup -d "$LP" 2>/dev/null
    rm -rf "$T" "$LOCK"
}
trap cleanup EXIT

# ── the disk and its root partition ─────────────────────────────────
#
# The partitions are found by NAME, not number, because the name is
# what build/mkimage promises and a number is what it happens to use.
part_of() { # image name -> "number first last"
    sgdisk -p "$1" 2>/dev/null | awk -v n="$2" '$NF==n {print $1, $2, $3}'
}

mount_root() { # image
    set -- $(part_of "$1" AUROS-ROOT)
    [ -n "${2:-}" ] || { echo "  $1 has no AUROS-ROOT"; return 1; }
    LP=$(losetup --find --show -o $(( $2 * 512 )) \
                 --sizelimit $(( ($3 - $2 + 1) * 512 )) "$DISK") || return 1
    mkdir -p "$T/m"
    mount "$LP" "$T/m" || { losetup -d "$LP"; LP=""; return 1; }
}
umount_root() {
    sync; umount "$T/m" 2>/dev/null; losetup -d "$LP" 2>/dev/null; LP=""
}

# ── the stand-in for one press of one button ────────────────────────
inject() {
    mkdir -p "$T/m/usr/lib/auros-e2e" "$T/m/etc/auros-e2e"
    cat > "$T/m/usr/lib/auros-e2e/run.sh" <<'EOS'
#!/bin/sh
# NOT PART OF AUROS. tools/firstboottest.sh puts this into a scratch
# copy of the image and nothing else ever does. It stands in for one
# person pressing one button on the welcome panel, and then turns the
# machine off so the test can read what the answer did.
OUT=/var/lib/auros-e2e
mkdir -p "$OUT"
mode=$(cat /etc/auros-e2e/mode 2>/dev/null || echo none)
n=$(cat /etc/auros-e2e/boot 2>/dev/null || echo 0)
log() { printf '%s %s\n' "$n" "$*" >> "$OUT/log"; }
log "up mode=$mode"
log "hold $(systemctl show -p Result --value auros-hold.service) \
exit=$(systemctl show -p ExecMainStatus --value auros-hold.service)"
cp /run/auros-first.state "$OUT/first.state.$n" 2>/dev/null
# The desktop's own directory. It is aurshell.service's
# RuntimeDirectory, so its existence says the desktop came up.
i=0
while [ ! -d /run/auros ] && [ "$i" -lt 300 ]; do sleep 1; i=$((i+1)); done
if [ -d /run/auros ]; then log "desktop $(stat -c %U:%a /run/auros) after ${i}s"
else log "desktop NEVER after ${i}s: $(systemctl is-active aurshell.service)"; fi
if [ "$mode" = confirm ] || [ "$mode" = decline ]; then
    # EXACTLY what welcome.c does: the bare word, O_CREAT|O_EXCL, 0600,
    # as the desktop user. `set -C` is the shell's O_EXCL.
    u=$(stat -c %U /run/auros 2>/dev/null)
    if runuser -u "$u" -- sh -c "umask 077; set -C; printf %s $mode > /run/auros/answer"
    then log "pressed $mode as $u"; else log "could not press $mode as $u"; fi
    i=0
    while [ "$i" -lt 180 ]; do
        grep -qx "request=$mode" /run/auros-answer/result 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    cp /run/auros-answer/result "$OUT/result.$n" 2>/dev/null \
        && log "answered after ${i}s" || log "NO ANSWER after ${i}s"
    # And it must have been consumed: the root side takes the request.
    [ -e /run/auros/answer ] && log "request LEFT BEHIND" || log "request consumed"
fi
journalctl -b --no-pager -o short-monotonic \
    -u auros-hold.service -u auros-answer.path -u auros-answer.service \
    -u aurshell.service > "$OUT/journal.$n" 2>&1
cp /var/log/auros-answer.log "$OUT/answer.log.$n" 2>/dev/null
sync
systemctl poweroff
EOS
    chmod 755 "$T/m/usr/lib/auros-e2e/run.sh"
    cat > "$T/m/etc/systemd/system/auros-e2e.service" <<'EOU'
[Unit]
Description=NOT PART OF AUROS: firstboottest's stand-in for one button press
After=auros-hold.service aurshell.service
[Service]
Type=oneshot
ExecStart=/usr/lib/auros-e2e/run.sh
TimeoutStartSec=900
[Install]
WantedBy=multi-user.target
EOU
    ln -sf /etc/systemd/system/auros-e2e.service \
        "$T/m/etc/systemd/system/multi-user.target.wants/auros-e2e.service"
}

# ── a machine, fresh: the image, and firmware as an install leaves it ─
fresh_machine() {
    DISK="$T/disk.img"; NV="$T/vars.fd"; BOOTN=0
    rm -f "$DISK" "$NV"
    if [ -f "out/auros-$PROFILE.img" ]; then
        cp --sparse=always "out/auros-$PROFILE.img" "$DISK"
    else
        zstd -q -d --sparse "$SRC" -o "$DISK" || return 1
    fi
    mount_root "$DISK" || return 1
    inject
    umount_root
    set -- $(part_of "$DISK" AUROS-ESP)
    [ -n "${1:-}" ] || { echo "  the image has no AUROS-ESP"; return 1; }
    cp "$VARS" "$NV"
    python3 "$E" "$NV" plant Boot0000 "Windows Boot Manager" \
        '\EFI\Microsoft\Boot\bootmgfw.efi' &&
    python3 "$E" "$NV" plant-hd Boot0002 "AurOS" \
        '\EFI\AurOS\shimx64.efi' "$DISK" "$1" &&
    python3 "$E" "$NV" set-order 0000 &&
    python3 "$E" "$NV" set-next 0002
}

# ── one power-on ────────────────────────────────────────────────────
power_on() { # mode
    BOOTN=$((BOOTN + 1))
    mount_root "$DISK" || return 1
    printf '%s\n' "$1" > "$T/m/etc/auros-e2e/mode"
    printf '%s\n' "$BOOTN" > "$T/m/etc/auros-e2e/boot"
    umount_root
    SER="$T/serial.$BOOTN"; : > "$SER"
    qemu-system-x86_64 -machine q35,accel=tcg -m 2048 -smp 2 -no-reboot \
        -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
        -drive if=pflash,format=raw,unit=1,file="$NV" \
        -drive file="$DISK",format=raw,if=none,id=d0 \
        -device virtio-blk-pci,drive=d0 \
        -device virtio-vga -display none -serial file:"$SER" \
        -net none >/dev/null 2>&1 &
    QP=$!
    i=0
    while kill -0 "$QP" 2>/dev/null && [ "$i" -lt "$BOOT_TIMEOUT" ]; do
        # A firmware that has fallen into its own shell is not coming
        # back; there is no point waiting twenty minutes to be told so.
        grep -aq 'starting Boot0004 "EFI Internal Shell"' "$SER" && break
        sleep 2; i=$((i + 2))
    done
    if kill -0 "$QP" 2>/dev/null; then
        kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null
        POWERED_OFF=0
    else
        wait "$QP" 2>/dev/null; POWERED_OFF=1
    fi
    QP=""
    ELAPSED=$i
}

# What OVMF said about the entries, in the order it said it.
bds() { tr -d '\r' < "$SER" | grep -ao 'BdsDxe: [a-z ]*Boot[0-9A-F]\{4\} "[^"]*"'; }
started() { bds | grep -q "starting Boot$1 "; }
first_tried() { bds | head -1 | grep -o 'Boot[0-9A-F]\{4\}'; }
order() { python3 "$E" "$NV" bootorder; }
next()  { python3 "$E" "$NV" bootnext; }
first_in_order() { order | awk '{print $1}'; }

# After a boot: pull the harness's record off the disk.
read_back() {
    mount_root "$DISK" || return 1
    R="$T/rec.$BOOTN"; mkdir -p "$R"
    cp -a "$T/m/var/lib/auros-e2e/." "$R/" 2>/dev/null
    cp -a "$T/m/var/lib/auros/." "$R/stamps/" 2>/dev/null || mkdir -p "$R/stamps"
    umount_root
}
said() { grep -q "^$BOOTN $1" "$R/log" 2>/dev/null; }
why_not() { # a few lines that say what happened, for a FAIL
    [ -f "$R/log" ] && sed -n "s/^$BOOTN /  harness: /p" "$R/log" | tail -4
    bds | sed 's/^/  firmware: /' | tail -4
    [ "$POWERED_OFF" = 1 ] || echo "  (it never powered itself off; killed after ${ELAPSED}s)"
}

echo
echo "Does the real AurOS ask, and does the answer stick?"
echo
echo "  the $PROFILE image, under firmware that persists between boots,"
echo "  in the state an install leaves it: Windows first, AurOS in the"
echo "  menu, BootNext pointing at AurOS"

# ═══ A. "it works" ══════════════════════════════════════════════════
echo
echo "  somebody switches it on, and says nothing yet"
fresh_machine || { echo "  could not prepare the machine"; exit 2; }
power_on none; read_back

started 0002 && ok "AurOS starts from its own entry, not the removable path" \
             || bad "AurOS starts from its own entry, not the removable path" "$(why_not)"
[ "$(first_tried)" = "Boot0002" ] \
    && ok "...because BootNext was asked for first" \
    || bad "...because BootNext was asked for first" "first tried: $(first_tried)"
said "desktop" && ! said "desktop NEVER" \
    && ok "the desktop comes up" \
    || bad "the desktop comes up" "$(why_not)"
[ "$(next)" = "0002" ] \
    && ok "the one-shot is armed again for the next start" \
    || bad "the one-shot is armed again for the next start" \
           "BootNext is '$(next)' -- the firmware ate it and auros-hold did not re-arm it"
[ "$(first_in_order)" = "0000" ] \
    && ok "and Windows is still what the computer starts by default" \
    || bad "and Windows is still what the computer starts by default" "BootOrder: $(order)"
[ ! -e "$R/stamps/converted.confirmed" ] && [ ! -e "$R/stamps/converted.declined" ] \
    && ok "nothing has been decided" \
    || bad "nothing has been decided" "$(ls "$R/stamps")"

echo
echo "  then she presses \"It works\""
power_on confirm; read_back
started 0002 && ok "the second start reaches AurOS too -- the hold held" \
             || bad "the second start reaches AurOS too -- the hold held" "$(why_not)"
said "pressed confirm" && ok "the press is a file in the desktop's own directory" \
                       || bad "the press is a file in the desktop's own directory" "$(why_not)"
grep -qx "result=ok" "$R/result.$BOOTN" 2>/dev/null \
    && ok "root carried it out and said so" \
    || bad "root carried it out and said so" \
           "$(cat "$R/result.$BOOTN" 2>/dev/null || echo 'no result file')" "$(why_not)"
said "request consumed" && ok "...and took the request away" \
                        || bad "...and took the request away" "$(why_not)"
[ "$(first_in_order)" = "0002" ] \
    && ok "AurOS is now first in BootOrder" \
    || bad "AurOS is now first in BootOrder" "BootOrder: $(order)"
case " $(order) " in
  *" 0000 "*) ok "and Windows Boot Manager is still in it" ;;
  *) bad "and Windows Boot Manager is still in it" "BootOrder: $(order)" ;;
esac
[ -z "$(next)" ] && ok "the one-shot is gone" \
                 || bad "the one-shot is gone" "BootNext is still $(next)"
[ -e "$R/stamps/converted.confirmed" ] \
    && ok "and the answer is recorded" \
    || bad "and the answer is recorded" "$(ls "$R/stamps")"

echo
echo "  and switches it on again the next day"
power_on none; read_back
started 0002 && ok "AurOS starts, now from BootOrder" \
             || bad "AurOS starts, now from BootOrder" "$(why_not)"
[ -z "$(next)" ] && ok "and nothing re-arms the one-shot any more" \
                 || bad "and nothing re-arms the one-shot any more" "BootNext: $(next)"
[ "$(first_in_order)" = "0002" ] \
    && ok "and the order she chose is the order it keeps" \
    || bad "and the order she chose is the order it keeps" "BootOrder: $(order)"

# ═══ B. "it does not" ═══════════════════════════════════════════════
echo
echo "  on a fresh machine, she presses \"Go back to Windows\""
fresh_machine || { echo "  could not prepare the machine"; exit 2; }
power_on decline; read_back
started 0002 && ok "AurOS starts, and asks" \
             || bad "AurOS starts, and asks" "$(why_not)"
grep -qx "result=ok" "$R/result.$BOOTN" 2>/dev/null \
    && ok "root carried it out" \
    || bad "root carried it out" \
           "$(cat "$R/result.$BOOTN" 2>/dev/null || echo 'no result file')" "$(why_not)"
[ -z "$(next)" ] && ok "the one-shot is cleared" \
                 || bad "the one-shot is cleared" "BootNext is $(next)"
[ "$(first_in_order)" = "0000" ] \
    && ok "and Windows is first, as it always was" \
    || bad "and Windows is first, as it always was" "BootOrder: $(order)"
[ -e "$R/stamps/converted.declined" ] \
    && ok "and the answer is recorded" \
    || bad "and the answer is recorded" "$(ls "$R/stamps")"

echo
echo "  and the next power-on"
power_on none; read_back
# OVMF has no Windows to start, so after Boot0000 fails it goes on to
# entries of its own -- one of which is this disk's removable path,
# which boots AurOS. On a real machine Boot0000 succeeds and that is
# the end of it. What is asserted is therefore the ORDER: Windows was
# tried first, and AurOS's own entry was not used at all.
[ "$(first_tried)" = "Boot0000" ] \
    && ok "the firmware goes to Windows first" \
    || bad "the firmware goes to Windows first" "first tried: $(first_tried)" "$(why_not)"
started 0002 && bad "and never to AurOS's own entry" "$(bds | tail -4)" \
             || ok "and never to AurOS's own entry"
[ -z "$(next)" ] && ok "and nothing ever re-arms the one-shot" \
                 || bad "and nothing ever re-arms the one-shot" "BootNext: $(next)"

# ═══ C. the second try ══════════════════════════════════════════════
#
# Somebody tried AurOS, said it did not work, put Windows back -- and
# tries again a month later. "Put Windows back" deletes the AurOS
# partitions and leaves the firmware's "AurOS" entry behind, pointing
# at a partition that no longer exists. The new install adds its own
# entry with a higher number, because the installer only reuses an
# entry that names the partition it just made.
#
# So there are two entries called AurOS, and the dead one comes first.
# Taken by description, the hold re-arms BootNext to the dead one --
# the firmware fails it and starts Windows -- and confirming puts the
# dead one first in BootOrder, so the next start skips it and reaches
# Windows. Right after she says AurOS works.
echo
echo "  a second try, after Windows was put back once"
fresh_machine || { echo "  could not prepare the machine"; exit 2; }
python3 "$E" "$NV" plant-hd-raw Boot0001 "AurOS" '\EFI\AurOS\shimx64.efi' \
    3 5400576 98304 5b0c6f2e-9d41-4e0a-8c3a-2f6d1e7b9a40 \
    || { echo "  could not plant the stale entry"; exit 2; }
power_on none; read_back
started 0002 && ok "the new install starts from its own entry" \
             || bad "the new install starts from its own entry" "$(why_not)"
st_by=$(sed -n 's/^entry_by=//p' "$R/first.state.$BOOTN" 2>/dev/null)
st_en=$(sed -n 's/^entry=//p'    "$R/first.state.$BOOTN" 2>/dev/null)
[ "$st_en" = "0002" ] \
    && ok "aurfirst takes the live entry for its own, not the dead one" \
    || bad "aurfirst takes the live entry for its own, not the dead one" \
           "state says entry=${st_en:-none} entry_by=${st_by:-unset}"
[ "$st_by" = "partition" ] \
    && ok "...by the partition it names, found from inside the running system" \
    || bad "...by the partition it names, found from inside the running system" \
           "entry_by=${st_by:-unset}: it could not tell which partition it booted from"
[ "$(next)" = "0002" ] \
    && ok "and the one-shot is re-armed to the live one" \
    || bad "and the one-shot is re-armed to the live one" \
           "BootNext is '$(next)'$( [ "$(next)" = 0001 ] && echo ' -- the dead one')"
power_on confirm; read_back
[ "$(first_in_order)" = "0002" ] \
    && ok "saying it works puts the LIVE entry first" \
    || bad "saying it works puts the LIVE entry first" \
           "BootOrder: $(order)$( [ "$(first_in_order)" = 0001 ] && echo ' -- the dead one is first; the next start reaches Windows')"

echo
if [ "$fail" -eq 0 ]; then
    echo "$checked checks: the real image asks, the answer is carried out by"
    echo "root and not by the desktop, and it decides what the computer starts"
    echo "from then on -- in both directions."
    exit 0
fi
echo "$fail of $checked wrong."
echo "Transcripts: serial.N and rec.N under $T (kept: FIRSTBOOT_KEEP=1)."
[ "${FIRSTBOOT_KEEP:-0}" = 1 ] && { trap - EXIT; rmdir "$LOCK"; echo "kept $T"; }
exit 1
