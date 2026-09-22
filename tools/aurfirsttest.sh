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
#      that still contains everything.
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
    -o "$TMP/af" src/aurfirst/*.c -Isrc/aurfirst 2>"$TMP/cc.log" \
    || { echo "  aurfirst did not build:"; sed -n '1,12p' "$TMP/cc.log"; exit 2; }
AF="$TMP/af"

# ── the fixture ─────────────────────────────────────────────────────
plant() { # slot description
    python3 - "$VD/Boot$1-$G" "$2" <<'EOPY'
import sys, struct
d = sys.argv[2].encode('utf-16-le') + b'\x00\x00'
f = '\\EFI\\x.efi'.encode('utf-16-le') + b'\x00\x00'
dp = struct.pack('<BBH', 4, 4, 4 + len(f)) + f + struct.pack('<BBH', 0x7F, 0xFF, 4)
open(sys.argv[1], 'wb').write(struct.pack('<I', 7) +
                              struct.pack('<IH', 1, len(dp)) + d + dp)
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

# ── the privilege boundary ──────────────────────────────────────────
echo
echo "  and the three words the desktop is allowed to ask for as root"
# answer.sh runs as root and is started by a file appearing in a
# directory the desktop's own user owns. The only thing standing
# between "that user" and "root" is which words it acts on, so every
# shape of thing that is not one of those three is tried here.
AR=$TMP/run
ask() { # word...  -> the result file
    mkdir -p "$AR"
    printf '%s' "$1" > "$AR/answer"
    AUROS_RUN="$AR" AUROS_STATE="$TMP/state2" AUROS_LOG="$TMP/answer.log"         sh rootfs/usr/lib/auros/answer.sh >/dev/null 2>&1
    cat "$AR/answer.result" 2>/dev/null
}
for w in 'reboot' 'CONFIRM' 'confirm; rm -rf /tmp/xx' '../../bin/sh' ''          'importx' 'ferry run'; do
    R=$(ask "$w")
    case "$R" in
      *"result=refused"*|*"result=failed"*) : ;;
      *) bad "'$w' is not acted on" "it answered: $(echo "$R" | tr '\n' ' ')" ;;
    esac
done
ok "anything that is not one of the three words is refused"
[ ! -f "$AR/answer" ] && ok "and the request is consumed either way"                       || bad "and the request is consumed either way"
# WHAT THIS CHECKS AND WHAT IT DOES NOT. The gate itself -- whether
# `aurfirst ferry` says yes -- is tested against a real aurfirst
# further up. Here there is no /usr/sbin/aurfirst to ask, so what is
# being proved is the other half of the same property: when the gate
# does not say yes, for any reason at all, nothing is mounted and
# nobody's files are read. A missing gate is a closed gate.
R=$(ask 'import')
case "$R" in
  *"result=refused"*) : ;;
  *) bad "a gate that cannot be asked is a closed one" \
         "$(echo "$R" | tr '\n' ' ')" ;;
esac
[ ! -f "$AR/import.log" ] \
  && ok "a gate that cannot be asked is a closed one, and nothing is read" \
  || bad "a gate that cannot be asked is a closed one, and nothing is read" \
         "it started importing anyway"

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
