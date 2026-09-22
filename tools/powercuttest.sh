#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  powercuttest — the risk register's two worst entries, made to happen
#
#  R4 says: a single-PC household whose install goes wrong has no
#  second computer and no way to make a stick. R6 says: a power cut in
#  the window where the disk is inconsistent. Every other test in this
#  tree asks whether the installer works. This one asks the only
#  question that decides whether it may be given to anybody:
#
#      IF THE POWER GOES AT THIS EXACT INSTANT, CAN SHE GET WINDOWS
#      BACK, WITH EVERY FILE IN IT UNCHANGED?
#
#  It is asked once per named dangerous instant in the installer, and
#  once per named dangerous instant in the restore -- which is the more
#  important half, because a machine running the restore is already a
#  machine something has gone wrong with, and it is where a second
#  failure is likeliest.
#
#  HOW THE CUT IS MADE, and why not with a cord and a stopwatch.
#
#  Killing a virtual machine from outside at a wall-clock moment lands
#  on a different instruction every run: a pass proves nothing about
#  the instant you cared about and a failure cannot be reproduced. So
#  the installer names its own dangerous instants (see fault.h) and a
#  fault-injection build can be told to stop dead at exactly one of
#  them. Every run is the same run.
#
#  WHAT THIS DOES NOT PROVE, said plainly:
#
#    - A disk's own write cache. QEMU's is not a Seagate's. What is
#      tested here is the ORDER writes reach the device and that each
#      commit is one sector, which is the property the design rests on;
#      whether a particular drive lies about having flushed is a
#      property of that drive.
#    - One firmware. OVMF is not an Insyde H2O from 2014.
#    - One geometry. tools/matrixtest.sh varies that.
#
#    sudo AURSTAGE_FAULT=1 ./build/staging      # once
#    sudo sh tools/powercuttest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
. tools/machine.sh

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-58s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-58s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

IMG=out/auros-staging-fault.img
[ -f "$IMG" ] || { echo "run: sudo AURSTAGE_FAULT=1 ./build/staging"; exit 2; }
mach_need

LOCK="${TMPDIR:-/tmp}/installtest.lock"
mkdir "$LOCK" 2>/dev/null || { echo "another end-to-end test is running"; exit 2; }
MTMP=$(mktemp -d "${TMPDIR:-/tmp}/powercut.XXXXXX")
trap 'mountpoint -q "$MTMP/m" 2>/dev/null && umount "$MTMP/m"; rm -rf "$MTMP" "$LOCK"' EXIT

echo
echo "If the power goes at the worst possible instant, does Windows come back?"
echo

mach_disk
mach_image
mach_stick
mach_journal
echo "  a 3 GiB machine with $WINFILES files in Windows, and an AurOS stick"

# ── the two things every check below is made of ─────────────────────

# Are all the user's files still there, byte for byte? Read through a
# loop over whatever the partition table says Windows is NOW, so that
# a table which has been changed is followed rather than assumed away.
win_files_ok() { # image  label
    _i=$1; _lab=$2
    _s=$(sgdisk -i 2 "$_i" 2>/dev/null | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    _e=$(sgdisk -i 2 "$_i" 2>/dev/null | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
    [ -n "$_s" ] && [ -n "$_e" ] || { bad "$_lab: Windows is still in the table"; return; }
    _l=$(losetup --find --show -o $((_s*512)) \
         --sizelimit $(((_e-_s+1)*512)) "$_i" 2>/dev/null)
    if [ -n "$_l" ] && nt ntfs-3g "$_l" "$MTMP/m" >/dev/null 2>&1; then
        ( cd "$MTMP/m" && find . -type f -exec md5sum {} \; | sort ) > "$MTMP/win.now"
        umount "$MTMP/m"
        if cmp -s "$MTMP/win.before" "$MTMP/win.now"; then
            ok "$_lab: every file in Windows is still exactly what it was"
        else
            bad "$_lab: every file in Windows is still exactly what it was" \
                "$(diff "$MTMP/win.before" "$MTMP/win.now" | head -3)"
        fi
    else
        bad "$_lab: the Windows volume still mounts" "it does not"
    fi
    [ -n "$_l" ] && losetup -d "$_l"
}

# And is it the machine it started as?
back_to_normal() { # image  label
    _i=$1; _lab=$2
    _n=$(sgdisk -p "$_i" 2>/dev/null | sed -n '/^Number/,$p' | tail -n +2 | grep -c .)
    [ "$_n" = "3" ] && ok "$_lab: the three original partitions are back" \
                    || bad "$_lab: the three original partitions are back" "it has $_n"
    _s=$(sgdisk -i 2 "$_i" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    _e=$(sgdisk -i 2 "$_i" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
    [ "$_s" = "$P2S" ] && [ "$_e" = "$P2E" ] \
        && ok "$_lab: Windows is its full size again" \
        || bad "$_lab: Windows is its full size again" "$_s..$_e, was $P2S..$P2E"
    _m=$(dd if="$_i" bs=512 skip=$P1S count=$((P1E-P1S+1)) status=none |
         md5sum | cut -d' ' -f1)
    [ "$_m" = "$ESPMD5" ] && ok "$_lab: the EFI partition is byte-for-byte what it was" \
                          || bad "$_lab: the EFI partition is byte-for-byte what it was"
    win_files_ok "$_i" "$_lab"
}

# ── one full install, to make the machine the restore cuts happen on ─
echo
echo "  first, one ordinary install, so there is something to undo"
if mach_boot "$IMG" "$DISK" "$STICK" "aurstage.install aurstage.min_gb=1" \
             "INSTALLTEST-AUROS-STARTED"; then
    ok "it installs"
else
    bad "it installs" "$(grep -a 'aurstage:' "$MTMP/out.txt" | tail -4)"
    echo; echo "nothing else here means anything without this."; exit 1
fi
cp --sparse=always "$MTMP/run.img" "$MTMP/installed.img"
cp --sparse=always "$MTMP/stk.img" "$MTMP/stick-after.img"

# ── the cuts during the install ─────────────────────────────────────
echo
echo "  the power goes during the install"
PRISTINE=$(md5sum "$DISK" | cut -d' ' -f1)

for pt in gate capture-mid shrink-begin shrink-end write-mid write-end \
          commit-array commit-sector commit-backup settle-end; do
    printf '\n  ── %s\n' "$pt"
    if mach_boot "$IMG" "$DISK" "$STICK" \
                 "aurstage.install aurstage.min_gb=1 aurstage.die_at=$pt" \
                 "AURSTAGE-FAULT $pt" 600; then
        ok "the machine stops dead at $pt"
    else
        bad "the machine stops dead at $pt" \
            "$(grep -a 'aurstage' "$MTMP/out.txt" | tail -3)"
        continue
    fi
    cp --sparse=always "$MTMP/run.img" "$MTMP/cut.img"
    cp --sparse=always "$MTMP/stk.img" "$MTMP/cut-stick.img"

    case "$pt" in
      gate|capture-mid|shrink-begin)
        # Above the line. Nothing has been written to the machine's
        # disk at all, and that is checkable exactly.
        [ "$(md5sum "$MTMP/cut.img" | cut -d' ' -f1)" = "$PRISTINE" ] \
            && ok "the disk is byte-for-byte untouched" \
            || bad "the disk is byte-for-byte untouched"
        ;;
      *)
        # Below it. The user's files must survive the cut itself,
        # before any restore is attempted -- a shrink that eats a
        # photograph is not something a restore can put back.
        win_files_ok "$MTMP/cut.img" "after the cut"
        ;;
    esac

    # And the machine must be recoverable. Above the line there is
    # nothing to recover, so the restore is asked only below it.
    case "$pt" in
      gate|shrink-begin) ;;
      capture-mid)
        # The saved copy on the stick was half written. It must be
        # REFUSED rather than half believed -- and the disk is
        # untouched anyway, so there is nothing to put back.
        if mach_boot "$IMG" "$MTMP/cut.img" "$MTMP/cut-stick.img" \
                     "aurstage.restore" "aurstage-report v1 verdict=restore" 300; then
            grep -aq "verdict=restored" "$MTMP/out.txt" \
                && bad "a half-written saved copy is refused" "it was used" \
                || ok "a half-written saved copy is refused"
        else
            bad "a half-written saved copy is refused" "no verdict line at all"
        fi
        ;;
      *)
        if mach_boot "$IMG" "$MTMP/cut.img" "$MTMP/cut-stick.img" \
                     "aurstage.restore" "verdict=restored" 900; then
            ok "and Windows can be put back"
            back_to_normal "$MTMP/run.img" "after $pt"
        else
            bad "and Windows can be put back" \
                "$(grep -a 'aurstage' "$MTMP/out.txt" | tail -4)"
        fi
        ;;
    esac
done

# ── the cuts during the restore ─────────────────────────────────────
echo
echo "  the power goes during the restore, which is the worse half"

for pt in restore-array restore-sector restore-backup restore-esp-mid \
          restore-grow; do
    printf '\n  ── %s\n' "$pt"
    if mach_boot "$IMG" "$MTMP/installed.img" "$MTMP/stick-after.img" \
                 "aurstage.restore aurstage.die_at=$pt" \
                 "AURSTAGE-FAULT $pt" 900; then
        ok "the machine stops dead at $pt"
    else
        bad "the machine stops dead at $pt" \
            "$(grep -a 'aurstage' "$MTMP/out.txt" | tail -3)"
        continue
    fi
    cp --sparse=always "$MTMP/run.img" "$MTMP/cut.img"
    cp --sparse=always "$MTMP/stk.img" "$MTMP/cut-stick.img"

    # THE PROPERTY THE WHOLE DESIGN RESTS ON: running it again
    # finishes the job. Every step of the restore is idempotent, so a
    # machine interrupted anywhere is a machine somebody can simply
    # start again with the stick in it.
    if mach_boot "$IMG" "$MTMP/cut.img" "$MTMP/cut-stick.img" \
                 "aurstage.restore" "verdict=restored" 900; then
        ok "running it again finishes the job"
        back_to_normal "$MTMP/run.img" "after $pt"
    else
        bad "running it again finishes the job" \
            "$(grep -a 'aurstage' "$MTMP/out.txt" | tail -4)"
    fi
done

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "Each of these is a person who cannot get her computer back."
    exit 1
fi
echo "$checked checks: the power can go at any named instant of the"
echo "install or the restore, and Windows comes back with every file"
echo "in it exactly as it was."
exit 0
