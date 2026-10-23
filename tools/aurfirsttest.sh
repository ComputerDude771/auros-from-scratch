#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  aurfirsttest — the last two steps, and the one write that is not
#  reversible by restarting
#
#  aurfirst is small and its whole job is two EFI variables, so what is
#  worth testing is not "does it run" but the four sentences the design
#  makes about it:
#
#    - until somebody says AurOS works, switching the machine on
#      reaches Windows. BootOrder is not touched, at all, by anything
#      except `confirm`.
#    - while nobody has answered, the next start reaches AurOS once.
#      BootNext is re-armed every boot, because the firmware eats it.
#    - `confirm` PROMOTES our entry. It never removes anybody else's,
#      and on firmware that ships without a BootOrder it builds one
#      that still contains everything. Everything: the entries the
#      firmware marks inactive, hidden or not-a-boot-option are put
#      BEHIND the ones it vouches for rather than left out, because a
#      BootOrder is a list of numbers and firmware skips a number it
#      will not start -- while leaving one out can cost somebody the
#      Windows they were promised they could go back to.
#    - "it does not work" clears the one-shot BEFORE it records the
#      answer, so there is no instant where the machine has stopped
#      re-arming and is still armed once.
#
#    sh tools/aurfirsttest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aurfirsttest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
VD="$TMP/efivars"; SD="$TMP/state"
G=8be4df61-93ca-11d2-aa0d-00e098032b8c

echo
echo "Does the machine stay Windows' until somebody says otherwise?"
echo

gcc -O1 -std=gnu11 -Wall -Wextra \
    -DAF_DIR_EFIVARS="\"$VD\"" -DAF_DIR="\"$SD\"" \
    -DAF_STATE_FILE="\"$TMP/first.state\"" -DAF_ALLOW_ENV_DIRS \
    -o "$TMP/af" src/aurfirst/*.c -Isrc/aurfirst 2>"$TMP/cc.log" \
    || { echo "  aurfirst did not build:"; sed -n '1,12p' "$TMP/cc.log"; exit 2; }
AF="$TMP/af"
# WHICH PARTITION IS "OURS" IS READ FROM THE HOST'S /sys AND /dev, and a
# unit test must not depend on the disks of the machine it runs on --
# on an AurOS machine this would find a real AUROS-BOOT and every case
# below would change meaning. So the choosing is pinned: "none" is
# "cannot tell", which is the fallback to the first entry called AurOS.
# The cases that are ABOUT the choosing set it explicitly. The
# discovery itself is tools/firstboottest.sh's, inside a real boot.
AF_OWN_BOOT_PARTUUID=none; export AF_OWN_BOOT_PARTUUID

# ── the fixture ─────────────────────────────────────────────────────
plant() { # slot description [load-option-attributes, default ACTIVE]
    python3 - "$VD/Boot$1-$G" "$2" "${3:-1}" <<'EOPY'
import sys, struct
d = sys.argv[2].encode('utf-16-le') + b'\x00\x00'
f = '\\EFI\\x.efi'.encode('utf-16-le') + b'\x00\x00'
dp = struct.pack('<BBH', 4, 4, 4 + len(f)) + f + struct.pack('<BBH', 0x7F, 0xFF, 4)
attrs = int(sys.argv[3], 0)
open(sys.argv[1], 'wb').write(struct.pack('<I', 7) +
                              struct.pack('<IH', attrs, len(dp)) + d + dp)
EOPY
}
plant_hd() { # slot description partition-guid
    python3 - "$VD/Boot$1-$G" "$2" "$3" <<'EOPY'
import sys, struct, uuid
sys.path.insert(0, 'tools')
from efivarstore import make_hd_load_option
lo = make_hd_load_option(sys.argv[2], '\\EFI\\AurOS\\shimx64.efi',
                         3, 5400576, 98304, uuid.UUID(sys.argv[3]).bytes_le)
open(sys.argv[1], 'wb').write(struct.pack('<I', 7) + lo)
EOPY
}
order() { # 0002 0000 ...
    python3 - "$VD/BootOrder-$G" "$@" <<'EOPY'
import sys, struct
ns = [int(x, 16) for x in sys.argv[2:]]
open(sys.argv[1], 'wb').write(struct.pack('<I', 7) +
                              b''.join(struct.pack('<H', n) for n in ns))
EOPY
}
read_order() {
    [ -f "$VD/BootOrder-$G" ] || { echo "(none)"; return; }
    python3 - "$VD/BootOrder-$G" <<'EOPY'
import sys, struct
b = open(sys.argv[1], 'rb').read()[4:]
print(' '.join('%04X' % v for v in struct.unpack('<%dH' % (len(b)//2), b)))
EOPY
}
read_next() {
    [ -f "$VD/BootNext-$G" ] || { echo "(none)"; return; }
    python3 -c "import sys,struct; b=open(sys.argv[1],'rb').read()[4:]; print('%04X' % struct.unpack('<H', b)[0])" "$VD/BootNext-$G"
}
st() { "$AF" state | sed -n "s/^$1=//p"; }
fresh() { rm -rf "$VD" "$SD"; mkdir -p "$VD" "$SD"; }

# ── a machine AurOS was installed onto directly ─────────────────────
echo "  a machine that was never converted from Windows"
fresh
[ "$(st phase)" = "not-converted" ] && ok "it knows there is nothing to promote" \
                                    || bad "it knows there is nothing to promote" "$("$AF" state | tr '\n' ' ')"
"$AF" hold >/dev/null 2>&1
[ ! -f "$VD/BootNext-$G" ] && ok "and arms nothing" || bad "and arms nothing"
"$AF" confirm >/dev/null 2>&1
[ -f "$SD/converted.confirmed" ] && [ ! -f "$VD/BootOrder-$G" ] \
  && ok "confirming writes no BootOrder it has no entry for" \
  || bad "confirming writes no BootOrder it has no entry for" "$(read_order)"

# ── a converted machine, before anybody answers ─────────────────────
echo
echo "  a converted machine, before anybody has answered"
fresh
plant 0000 "Windows Boot Manager"
plant 0002 "AurOS"
order 0000
[ "$(st phase)" = "asking" ]     && ok "it knows it is waiting for an answer" \
                                 || bad "it knows it is waiting for an answer" "$(st phase)"
[ "$(st entry)" = "0002" ]       && ok "and which entry is AurOS" || bad "and which entry is AurOS" "$(st entry)"
[ "$(st is_default)" = "no" ]    && ok "and that Windows is still the default" \
                                 || bad "and that Windows is still the default"

"$AF" hold >/dev/null 2>&1
python3 - "$VD/BootNext-$G" <<'EOPY'
import sys, struct
b = open(sys.argv[1], 'rb').read()
raise SystemExit(0 if len(b) == 6 and struct.unpack_from('<I', b, 0)[0] == 7
                 and struct.unpack_from('<H', b, 4)[0] == 2 else 1)
EOPY
[ $? -eq 0 ] && ok "hold arms the one-shot at it" \
             || bad "hold arms the one-shot at it" "$(od -An -tx1 "$VD/BootNext-$G" 2>/dev/null)"
[ "$(read_order)" = "0000" ] && ok "and does not touch BootOrder" \
                             || bad "and does not touch BootOrder" "$(read_order)"
BEFORE=$(md5sum "$VD/BootNext-$G" | cut -d' ' -f1)
"$AF" hold >/dev/null 2>&1
[ "$(md5sum "$VD/BootNext-$G" | cut -d' ' -f1)" = "$BEFORE" ] \
  && ok "holding twice writes NVRAM once" || bad "holding twice writes NVRAM once"
"$AF" ferry >/dev/null 2>&1 \
  && bad "and importing from Windows is not allowed yet" "it said yes" \
  || ok "and importing from Windows is not allowed yet"

# ── "this works" ────────────────────────────────────────────────────
echo
echo "  and then somebody says it works"
"$AF" confirm >/dev/null 2>&1
[ "$(read_order)" = "0002 0000" ] \
  && ok "AurOS goes to the front of BootOrder" \
  || bad "AurOS goes to the front of BootOrder" "$(read_order)"
case "$(read_order)" in
  *0000*) ok "and Windows Boot Manager is still in it" ;;
  *) bad "and Windows Boot Manager is still in it" "$(read_order)" ;;
esac
[ ! -f "$VD/BootNext-$G" ] && ok "the one-shot is cleared" || bad "the one-shot is cleared"
[ "$(st phase)" = "done" ] && ok "and the question stops being asked" \
                           || bad "and the question stops being asked" "$(st phase)"
"$AF" ferry >/dev/null 2>&1 && ok "now importing from Windows may run" \
                            || bad "now importing from Windows may run"
"$AF" hold >/dev/null 2>&1
[ ! -f "$VD/BootNext-$G" ] && ok "and hold stops arming anything for ever" \
                           || bad "and hold stops arming anything for ever"

# An entry that is already in BootOrder is MOVED, not added twice.
fresh
plant 0000 "Windows Boot Manager"; plant 0002 "AurOS"; plant 0005 "Fedora"
order 0000 0005 0002
"$AF" confirm >/dev/null 2>&1
[ "$(read_order)" = "0002 0000 0005" ] \
  && ok "an entry already in the order moves rather than repeating" \
  || bad "an entry already in the order moves rather than repeating" "$(read_order)"

# FIRMWARE THAT SHIPS WITHOUT A BootOrder. Writing just our number
# would be a boot menu with one thing in it -- Windows gone, on a
# machine whose whole promise is that it can go back.
fresh
plant 0000 "Windows Boot Manager"; plant 0002 "AurOS"; plant 0005 "Fedora"
"$AF" confirm >/dev/null 2>&1
[ "$(read_order)" = "0002 0000 0005" ] \
  && ok "no BootOrder at all: one is built that still holds everything" \
  || bad "no BootOrder at all: one is built that still holds everything" "$(read_order)"

# AND NOT EVERYTHING. A Boot#### is not automatically something to
# boot: the UEFI load-option attributes mark an entry inactive, hidden,
# or an APPLICATION rather than a boot option, and the manufacturer's
# diagnostics and the firmware setup entry are exactly those. Building
# the order out of every variable in the directory used to promote all
# three above Windows on a machine whose owner never asked for it --
# and left her undoing it in a firmware menu this product exists to
# keep her out of.
fresh
plant 0000 "Windows Boot Manager"
plant 0002 "AurOS"
plant 0003 "Diagnostics"     0x101   # ACTIVE, category APPLICATION
plant 0004 "Recovery"        0x0     # not ACTIVE
plant 0006 "Firmware Setup"  0x9     # ACTIVE | HIDDEN
plant 0005 "Fedora"
"$AF" confirm >/dev/null 2>&1
[ "$(read_order)" = "0002 0000 0005 0003 0004 0006" ] \
  && ok "...with the not-to-be-booted ones behind, not promoted" \
  || bad "...with the not-to-be-booted ones behind, not promoted" \
        "$(read_order)"
case " $(read_order) " in
  *" 0003 "*) case " $(read_order) " in
                *" 0004 "*) case " $(read_order) " in
                              *" 0006 "*) ok "...and none of them dropped" ;;
                              *) bad "...and none of them dropped" "$(read_order)" ;;
                            esac ;;
                *) bad "...and none of them dropped" "$(read_order)" ;;
              esac ;;
  *) bad "...and none of them dropped" "$(read_order)" ;;
esac

# THE DIRECTION THAT WAS NOT TESTED, AND IS THE ONE THAT MATTERS.
#
# The first version of the filter REMOVED what it did not like, and a
# review built the machine that breaks: no BootOrder, and a Windows
# entry whose LOAD_OPTION_ACTIVE is clear -- which is how several
# vendors record "the user switched this off in the boot menu" rather
# than deleting the variable. Windows was filtered out, a BootOrder
# holding only AurOS was written, confirm printed success and stamped
# the answer permanently, and decline then refused to undo it because
# ours was the only entry left.
fresh
plant 0000 "Windows Boot Manager" 0x0     # the vendor switched it off
plant 0002 "AurOS"
"$AF" confirm >/dev/null 2>&1
case " $(read_order) " in
  *" 0000 "*) ok "a Windows entry the firmware marked inactive is still there" ;;
  *) bad "a Windows entry the firmware marked inactive is still there" \
         "$(read_order)" ;;
esac
[ "$(read_order)" != "0002" ] \
  && ok "...so the menu is never left with only AurOS in it" \
  || bad "...so the menu is never left with only AurOS in it" "$(read_order)"
# And the way back still exists, which is the thing that was lost.
"$AF" decline >/dev/null 2>&1
case " $(read_order) " in
  0002*) bad "and declining can still put Windows first" "$(read_order)" ;;
  *) ok "and declining can still put Windows first" ;;
esac

# AN ENTRY THIS PROGRAM CANNOT READ IS NOT AN ENTRY IT MAY JUDGE.
# af_var_get answers -1 for a variable it cannot read and -2 for one
# longer than aurfirst will look at; both used to mean "not bootable",
# which meant "delete from the boot menu".
#
# The numbering below is what makes this a test rather than a
# formality. 0003 is positively hidden, so it belongs at the BACK;
# 0007 and 0008 are merely unreadable, so they belong at the FRONT
# with the ones the firmware vouched for. Since 0003 sorts BEFORE
# them, an implementation that lumps "cannot read" in with "refused"
# produces 0002 0000 0003 0007 0008 and this fails. Give 0003 a higher
# number and both orders agree, and the check tests nothing.
fresh
plant 0000 "Windows Boot Manager"
plant 0002 "AurOS"
plant 0003 "Firmware Setup" 0x9                   # positively hidden
printf 'xyz' > "$VD/Boot0007-$G"                  # too short to be one
plant 0008 "Enormous"
python3 - "$VD/Boot0008-$G" <<'EOPY'
import sys
p = sys.argv[1]
b = open(p, 'rb').read()
open(p, 'wb').write(b + b'\0' * 6000)            # longer than AF_OPT_MAX
EOPY
"$AF" confirm >/dev/null 2>&1
[ "$(read_order)" = "0002 0000 0007 0008 0003" ] \
  && ok "an entry that cannot be read is kept, and not demoted" \
  || bad "an entry that cannot be read is kept, and not demoted" \
        "wanted 0002 0000 0007 0008 0003, got $(read_order)"

# BUT AN ORDER THAT ALREADY NAMES ONE KEEPS IT. The filter above is a
# judgement about what to ADD to a machine that had no order at all.
# Somewhere there is a laptop whose vendor ships a hidden entry in
# BootOrder on purpose, and taking it out is not this program's
# decision to make.
fresh
plant 0000 "Windows Boot Manager"
plant 0002 "AurOS"
plant 0006 "Firmware Setup" 0x9
order 0000 0006
"$AF" confirm >/dev/null 2>&1
# NB: this does not exercise the attribute filter -- there IS a
# BootOrder here, so the branch above runs and af_boot_bootable is
# never called. It is here to keep it that way: a refactor that moved
# the filter into the shared path would reorder somebody's existing
# menu, and this is what would say so.
[ "$(read_order)" = "0002 0000 0006" ] \
  && ok "an existing order is preserved, hidden entry and all" \
  || bad "an existing order is preserved, hidden entry and all" "$(read_order)"

# ── which "AurOS" is ours ───────────────────────────────────────────
#
# Try AurOS, say no, put Windows back, try again: "Put Windows back"
# leaves the firmware's "AurOS" entry behind, pointing at a partition
# that is gone, and the second install adds its own with a HIGHER
# number. Taken by description, the dead one wins -- the hold re-arms
# BootNext to it, the firmware fails it and starts Windows, and
# confirming puts it first so the next start reaches Windows anyway.
echo
echo "  and when there are two entries called AurOS"
DEAD=5b0c6f2e-9d41-4e0a-8c3a-2f6d1e7b9a40
LIVE=0e8a4c71-3b6f-4d29-a1e5-7c9f2b8d6e13
two_aurOS() {
    fresh
    plant 0000 "Windows Boot Manager"
    plant_hd 0001 "AurOS" "$DEAD"       # left behind by "Put Windows back"
    plant_hd 0005 "AurOS" "$LIVE"       # this install's
    order 0000 0001
}
two_aurOS
[ "$(AF_OWN_BOOT_PARTUUID=$LIVE st entry)" = "0005" ] \
  && ok "ours is the one that names this install's partition" \
  || bad "ours is the one that names this install's partition" \
        "$(AF_OWN_BOOT_PARTUUID=$LIVE "$AF" state | tr '\n' ' ')"
[ "$(AF_OWN_BOOT_PARTUUID=$LIVE st entry_by)" = "partition" ] \
  && ok "...and it says that is how it chose" \
  || bad "...and it says that is how it chose"
AF_OWN_BOOT_PARTUUID=$LIVE "$AF" hold >/dev/null 2>&1
[ "$(read_next)" = "0005" ] \
  && ok "the one-shot is armed to the live entry, not the dead one" \
  || bad "the one-shot is armed to the live entry, not the dead one" \
        "BootNext: $(read_next)"
AF_OWN_BOOT_PARTUUID=$LIVE "$AF" confirm >/dev/null 2>&1
[ "$(read_order)" = "0005 0000 0001" ] \
  && ok "and confirming puts the live one first, dropping nobody" \
  || bad "and confirming puts the live one first, dropping nobody" "$(read_order)"

# Written in capitals, which is how some tools print a GUID.
two_aurOS
UPPER=$(printf '%s' "$LIVE" | tr a-f A-F)
[ "$(AF_OWN_BOOT_PARTUUID=$UPPER st entry)" = "0005" ] \
  && ok "the partition is matched however its GUID is written" \
  || bad "the partition is matched however its GUID is written"

# KNOWING OURS AND NOT FINDING IT: there is no entry for this install,
# and every AurOS in the menu is somebody else's or dead. Promoting one
# of them is exactly the bug.
fresh
plant 0000 "Windows Boot Manager"
plant_hd 0001 "AurOS" "$DEAD"
order 0000 0001
[ "$(AF_OWN_BOOT_PARTUUID=$LIVE st converted)" = "no" ] \
  && ok "a menu holding only a dead AurOS has no entry of ours in it" \
  || bad "a menu holding only a dead AurOS has no entry of ours in it" \
        "$(AF_OWN_BOOT_PARTUUID=$LIVE "$AF" state | tr '\n' ' ')"
AF_OWN_BOOT_PARTUUID=$LIVE "$AF" hold >/dev/null 2>&1
[ ! -f "$VD/BootNext-$G" ] \
  && ok "...and nothing arms the dead one" \
  || bad "...and nothing arms the dead one" "BootNext: $(read_next)"

# AND WHEN IT CANNOT TELL, the old rule, said out loud. This is also
# the proof that the cases above test something: the same machine,
# chosen by description, takes the dead entry.
two_aurOS
[ "$(st entry)" = "0001" ] && [ "$(st entry_by)" = "description" ] \
  && ok "unable to tell, it falls back to the first -- and says so" \
  || bad "unable to tell, it falls back to the first -- and says so" \
        "entry=$(st entry) entry_by=$(st entry_by)"

# ── "it does not" ───────────────────────────────────────────────────
echo
echo "  or somebody says it does not"
fresh
plant 0000 "Windows Boot Manager"; plant 0002 "AurOS"
order 0000
"$AF" hold >/dev/null 2>&1
"$AF" decline >/dev/null 2>&1
[ ! -f "$VD/BootNext-$G" ] && ok "the one-shot is cleared" || bad "the one-shot is cleared"
[ "$(read_order)" = "0000" ] && ok "and BootOrder is exactly what it was" \
                             || bad "and BootOrder is exactly what it was" "$(read_order)"
[ "$(st phase)" = "declined" ] && ok "and the machine stops coming back to AurOS" \
                               || bad "and the machine stops coming back to AurOS" "$(st phase)"
"$AF" hold >/dev/null 2>&1
[ ! -f "$VD/BootNext-$G" ] && ok "...even after another boot" \
                           || bad "...even after another boot"

# ── the two halves agreeing ─────────────────────────────────────────
echo
echo "  and the two programs that write EFI variables agree about how"
# nvram.c writes the staging environment's variables and this writes
# AurOS's. They are deliberately separate -- one of them may not write
# BootOrder -- but the bytes that land in efivarfs have to be the same
# shape, and two implementations of one format are the pair that
# drifts.
cat > "$TMP/w.c" <<'EOC'
#include <stdint.h>
#include <stdio.h>
#include "nvram.h"
int main(void)
{
    char why[300];
    return nvram_boot_next(0x0042, why, sizeof why) == 0 ? 0 : 1;
}
EOC
ND="$TMP/nvars"; mkdir -p "$ND"
gcc -O1 -std=gnu11 -Wall -Wextra -DNVRAM_DIR="\"$ND\"" -o "$TMP/w" \
    "$TMP/w.c" src/aurstage/nvram.c -Isrc/aurstage 2>/dev/null \
    && "$TMP/w" \
    || { echo "    (nvram.c would not build here; not compared)"; }
fresh
plant 0042 "AurOS"
"$AF" hold >/dev/null 2>&1
if [ -f "$ND/BootNext-$G" ] && [ -f "$VD/BootNext-$G" ]; then
    cmp -s "$ND/BootNext-$G" "$VD/BootNext-$G" \
      && ok "the same variable, byte for byte, from either side" \
      || bad "the same variable, byte for byte, from either side" \
             "staging: $(od -An -tx1 "$ND/BootNext-$G")" \
             "AurOS:   $(od -An -tx1 "$VD/BootNext-$G")"
else
    bad "the same variable, byte for byte, from either side" "one side wrote nothing"
fi

# And they agree about the NAME the entry is found by, which is the
# whole mechanism: the installer writes it and this program looks it up.
A=$(sed -n 's/.*#define AF_ENTRY_DESC "\(.*\)".*/\1/p' src/aurfirst/aurfirst.h)
B=$(sed -n 's/.*#define LOADER_ENTRY_DESC   "\(.*\)".*/\1/p' src/aurstage/loader.h)
[ -n "$A" ] && [ "$A" = "$B" ] \
  && ok "and about what the installer called the entry" \
  || bad "and about what the installer called the entry" "aurfirst: '$A'  loader: '$B'"

# ── what the desktop is told ────────────────────────────────────────
echo
echo "  and what it leaves for the desktop to read"
# The shell runs as the person using the machine. It does not read
# efivarfs, does not parse a load option and does not start a program
# to ask a question -- it reads one file, and this is the file. A
# desktop that cannot read it asks nothing, on the one boot that
# mattered.
fresh
plant 0000 "Windows Boot Manager"; plant 0002 "AurOS"
order 0000
rm -f "$TMP/first.state"
"$AF" hold >/dev/null 2>&1
grep -q '^phase=asking$' "$TMP/first.state" 2>/dev/null \
  && ok "the hold writes down that the question is open" \
  || bad "the hold writes down that the question is open" \
         "$(cat "$TMP/first.state" 2>/dev/null | tr '\n' ' ')"
[ "$(stat -c %a "$TMP/first.state" 2>/dev/null)" = "644" ] \
  && ok "and leaves it readable by the desktop's own user" \
  || bad "and leaves it readable by the desktop's own user" \
         "mode $(stat -c %a "$TMP/first.state" 2>/dev/null)"
# AFTER the change, not before it. Publishing the state the run started
# with leaves a machine that has just been confirmed still asking.
"$AF" confirm >/dev/null 2>&1
grep -q '^phase=done$' "$TMP/first.state" 2>/dev/null \
  && ok "and confirming rewrites it, so the question stops being asked" \
  || bad "and confirming rewrites it, so the question stops being asked" \
         "$(cat "$TMP/first.state" 2>/dev/null | tr '\n' ' ')"
"$AF" state >/dev/null 2>&1
grep -q '^entry=0002$' "$TMP/first.state" 2>/dev/null \
  && ok "and it names the entry, so nothing else has to look" \
  || bad "and it names the entry, so nothing else has to look"

# ── the privilege boundary ──────────────────────────────────────────
echo
echo "  and the boundary between the desktop and root"
# answer.sh runs as root and is started by a file appearing in a
# directory the desktop's own user OWNS. The first version of it read
# that file with `head -c` and wrote its answer, its log and its report
# back through paths in the same directory -- as root, through names
# she controls, with nothing to stop any of them being a symbolic link.
# A review reproduced truncate-anything, append-anything and
# chmod-anything from one request, and a read oracle for root-only
# files from another. Everything below is one of those shapes.
AR="$TMP/run"        # hers: mode 0700, she owns it
AO="$TMP/answered"   # root's: nothing she writes goes here
mkdir -p "$AR" "$AO"
mkdir -p "$TMP/bin"
# The test's own aurfirst, where answer.sh will look for it. Built
# against the scratch efivarfs above, so `confirm` here is a real
# confirm of a real (simulated) machine rather than a missing program.
cp "$AF" "$TMP/bin/aurfirst"

ask() { # word-or-nothing  -> the result file's contents
    rm -f "$AO/result"
    [ $# -gt 0 ] && printf '%s' "$1" > "$AR/answer"
    AUROS_ANSWER_OUT="$AO" AUROS_STATE="$TMP/state2" \
    AUROS_BIN="$TMP/bin" AUROS_LOG="$TMP/answer.log" \
    AF_RUN_USER="$AR" \
        sh rootfs/usr/lib/auros/answer.sh >/dev/null 2>&1
    cat "$AO/result" 2>/dev/null
}

# A WORD THAT IS NOT ONE OF THE FOUR MUST REACH THE DEFAULT ARM.
# Accepting any refusal is not enough: `result=failed` means a named
# branch RAN and errored, which is the opposite of what is being
# claimed. The note is what tells them apart.
before=$fail
for w in 'reboot' 'CONFIRM' 'confirm; rm -rf /tmp/xx' '../../bin/sh' '' \
         'importx' 'ferry run' 'confirmx' 'PUTBACK' 'putbackx' 'put back' \
         'con firm' 'decline!'; do
    R=$(ask "$w")
    case "$R" in
      *"note=unknown request"*) : ;;
      "") bad "'$w' is not acted on" "no answer was written at all" ;;
      *)  bad "'$w' is not acted on" "$(echo "$R" | tr '\n' ' ')" ;;
    esac
done
[ "$fail" = "$before" ] \
  && ok "anything that is not one of the four words reaches no branch" \
  || true
[ ! -f "$AR/answer" ] && ok "and the request is consumed either way" \
                      || bad "and the request is consumed either way"

# ── the shapes that were the escalation ─────────────────────────────
echo
echo "  and the shapes a directory she owns can hold"
printf 'PRECIOUS\n' > "$TMP/victim"; chmod 600 "$TMP/victim"
VB=$(md5sum "$TMP/victim" | cut -d' ' -f1)

# 1. The ANSWER written through a symlink she planted. This is the one
#    that was chmod-anything and truncate-anything as root.
rm -f "$AO/result"
ln -sf "$TMP/victim" "$AO/result" 2>/dev/null
ask confirm >/dev/null
# The root side owns $AO, so this cannot happen there any more -- but
# the check stays, because the property is "nothing root writes lands
# outside the directory root owns", and a future path that moved back
# into hers would break it silently.
[ "$(md5sum "$TMP/victim" | cut -d' ' -f1)" = "$VB" ] \
  && [ "$(stat -c %a "$TMP/victim")" = "600" ] \
  && ok "an answer written through a planted link changes nothing" \
  || bad "an answer written through a planted link changes nothing" \
         "$(ls -l "$TMP/victim")"
rm -f "$AO/result"

# 2. The REQUEST as a symlink to a file root can read and she cannot.
#    It must not be read, and not one byte of it may come back.
printf 'rootsecrethashvalue\n' > "$TMP/secret"; chmod 600 "$TMP/secret"
rm -f "$AR/answer"; ln -s "$TMP/secret" "$AR/answer"
R=$(ask)
case "$R" in
  *rootsecret*) bad "a request that is a link is not read" "it echoed the target" ;;
  *"note=unknown request"*) ok "a request that is a link is not read" ;;
  *) bad "a request that is a link is not read" "$(echo "$R" | tr '\n' ' ')" ;;
esac
[ -e "$AR/answer" ] && bad "...and the link is removed" "it is still there" \
                    || ok "...and the link is removed"
[ -f "$TMP/secret" ] && ok "...and the file it pointed at is untouched" \
                     || bad "...and the file it pointed at is untouched"

# 3. A DIRECTORY named `answer`. It used to wedge the machine for ever:
#    the guard returned before the removal, the path unit never edged
#    again, and her O_EXCL create failed for ever -- so one mkdir took
#    away her ability to say "No, go back to Windows".
rm -f "$AR/answer"; mkdir -p "$AR/answer"
ask >/dev/null
[ ! -e "$AR/answer" ] && ok "a directory in its place is cleared, not fatal" \
                      || bad "a directory in its place is cleared, not fatal"
rm -rf "$AR/answer"

# 4. A FIFO, which a read as root would block on for ever.
rm -f "$AR/answer"
if mkfifo "$AR/answer" 2>/dev/null; then
    ( ask >/dev/null ) &
    apid=$!
    i=0
    while [ $i -lt 50 ]; do kill -0 "$apid" 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
    if kill -0 "$apid" 2>/dev/null; then
        kill -9 "$apid" 2>/dev/null
        bad "a pipe in its place does not block root for ever" "it hung"
    else
        ok "a pipe in its place does not block root for ever"
    fi
    wait "$apid" 2>/dev/null
    rm -f "$AR/answer"
else
    echo "    (no mkfifo here; the pipe case was not tried)"
fi

# 5. A HARD LINK to a root-only file -- the one shape O_NOFOLLOW does
#    not catch, which is why the request is also fstat'ed for a link
#    count of one on the descriptor it is read from.
rm -f "$AR/answer"
if ln "$TMP/secret" "$AR/answer" 2>/dev/null; then
    R=$(ask)
    case "$R" in
      *rootsecret*) bad "a hard link to a root-only file is not read" \
                        "it echoed the target" ;;
      *)            ok "a hard link to a root-only file is not read" ;;
    esac
    rm -f "$AR/answer"
else
    echo "    (no hard link here; that case was not tried)"
fi

# ── the import gate ─────────────────────────────────────────────────
echo
echo "  and importing, which waits for the answer"
rm -rf "$SD"; mkdir -p "$SD"        # nobody has confirmed anything
rm -f "$AO/import.log"
R=$(ask import)
case "$R" in
  *"note=AurOS has not been confirmed yet"*)
    ok "importing is refused until AurOS has been confirmed" ;;
  *) bad "importing is refused until AurOS has been confirmed" \
         "$(echo "$R" | tr '\n' ' ')" ;;
esac
[ ! -f "$AO/import.log" ] && ok "...and nothing was mounted or read" \
                          || bad "...and nothing was mounted or read"

# ── putting Windows back ────────────────────────────────────────────
echo
echo "  and putting Windows back, which is asked for from Settings"
# answer.sh does not restore anything: it chooses the restore entry in
# AurOS's own menu for the next start (grubenv next_entry), makes sure
# the next start REACHES that menu (BootNext at our entry, even on a
# machine that was told to start Windows), and restarts. Each part that
# fails takes back the parts before it: a next_entry left behind would
# remove AurOS at some later start nobody asked for.
GE="$TMP/grubenv"; ESP="$TMP/esp"
pb() { # -> the result file's contents
    rm -f "$AO/result" "$TMP/rebooted"
    printf 'putback' > "$AR/answer"
    AUROS_ANSWER_OUT="$AO" AUROS_STATE="$TMP/state2" \
    AUROS_BIN="$TMP/bin" AUROS_LOG="$TMP/answer.log" \
    AF_RUN_USER="$AR" AUROS_GRUBENV="$GE" AUROS_ESP_ROOTS="$ESP" \
    AUROS_REBOOT="touch $TMP/rebooted" AUROS_REBOOT_DELAY=0 \
        sh rootfs/usr/lib/auros/answer.sh >/dev/null 2>&1
    cat "$AO/result" 2>/dev/null
}
next_entry() { sed -n 's/^next_entry=//p' "$GE" 2>/dev/null; }
empty_env() { { printf '# GRUB Environment Block\n'; head -c 999 /dev/zero | tr '\0' '#'; } > "$GE"; }

# A converted machine whose person said "it does not work": Windows is
# the default and nothing is armed.
fresh; plant 0000 "Windows Boot Manager"; plant 0003 "AurOS"; order 0000
"$AF" decline >/dev/null 2>&1
empty_env; rm -rf "$ESP"; mkdir -p "$ESP/EFI/AurOS"

# 1. The restore's files are gone: nothing may be arranged.
R=$(pb)
case "$R" in
  *"result=failed"*"not on this computer"*) ok "with the restore's files gone, it refuses and says why" ;;
  *) bad "with the restore's files gone, it refuses and says why" "$(echo "$R" | tr '\n' ' ')" ;;
esac
[ -z "$(next_entry)" ] && [ "$(read_next)" = "(none)" ] && [ ! -f "$TMP/rebooted" ] \
  && ok "...and arranges nothing, and does not restart" \
  || bad "...and arranges nothing, and does not restart" \
         "next_entry='$(next_entry)' BootNext=$(read_next) rebooted=$([ -f "$TMP/rebooted" ] && echo yes || echo no)"

# 2. They are there: the next start is AurOS's menu, on the restore.
: > "$ESP/EFI/AurOS/staging.efi"; : > "$ESP/EFI/AurOS/staging.img"
R=$(pb)
case "$R" in
  *"request=putback"*"result=ok"*) ok "with the files there, it is arranged" ;;
  *) bad "with the files there, it is arranged" "$(echo "$R" | tr '\n' ' ')" ;;
esac
[ "$(next_entry)" = "put-windows-back>put-windows-back-yes" ] \
  && ok "...the menu will take the restore entry, by its id" \
  || bad "...the menu will take the restore entry, by its id" "next_entry='$(next_entry)'"
[ "$(wc -c < "$GE")" = 1024 ] && ok "...in a block of exactly 1024 bytes" \
  || bad "...in a block of exactly 1024 bytes" "$(wc -c < "$GE") bytes"
if command -v grub-editenv >/dev/null 2>&1; then
    [ "$(grub-editenv "$GE" list 2>/dev/null)" = "next_entry=put-windows-back>put-windows-back-yes" ] \
      && ok "...which grub's own tool reads back the same" \
      || bad "...which grub's own tool reads back the same" "$(grub-editenv "$GE" list 2>&1)"
else
    echo "    (no grub-editenv here; the block was not read with grub's tool)"
fi
[ "$(read_next)" = "0003" ] \
  && ok "...and the firmware starts AurOS's menu next, though she declined" \
  || bad "...and the firmware starts AurOS's menu next, though she declined" "BootNext=$(read_next)"
[ "$(read_order)" = "0000" ] && ok "...without touching BootOrder" \
  || bad "...without touching BootOrder" "BootOrder=$(read_order)"
[ -f "$TMP/rebooted" ] && ok "...and then it restarts" || bad "...and then it restarts"

# 3. The firmware will not take BootNext (no entry of ours): what was
#    already written to the menu is taken back.
fresh; plant 0000 "Windows Boot Manager"; order 0000
empty_env
R=$(pb)
case "$R" in
  *"result=failed"*) ok "when the restart cannot be arranged, it says so" ;;
  *) bad "when the restart cannot be arranged, it says so" "$(echo "$R" | tr '\n' ' ')" ;;
esac
[ -z "$(next_entry)" ] && [ ! -f "$TMP/rebooted" ] \
  && ok "...and takes the menu's choice back, and does not restart" \
  || bad "...and takes the menu's choice back, and does not restart" \
         "next_entry='$(next_entry)' rebooted=$([ -f "$TMP/rebooted" ] && echo yes || echo no)"

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "Until confirm, this machine is supposed to still be Windows'."
    exit 1
fi
echo "$checked checks: BootOrder is untouched until somebody says AurOS works,"
echo "the one-shot keeps the machine coming back until then, and confirming"
echo "promotes AurOS without removing anybody else."
exit 0
