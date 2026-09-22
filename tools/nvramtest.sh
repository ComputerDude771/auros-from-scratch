#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  nvramtest — the one file that may write an EFI variable
#
#  Two things, and only one of them is the code.
#
#  The code: a boot entry is a wire format that firmware reads and
#  nothing on the machine ever reads back. If it is wrong, the symptom
#  is a computer that starts nothing, with no error message, and the
#  product's own logs saying the entry was written successfully. So
#  the bytes are decoded here by a SECOND implementation, in another
#  language -- tools/efivarstore.py, which the end-to-end test uses to
#  read real OVMF NVRAM -- rather than compared against the constants
#  that produced them.
#
#  The arrangement: build/staging must refuse to build an image in
#  which nvram.c has stopped being what it says it is. It is allowed
#  the writable open every other file is refused, and pays for it with
#  four checks; a gate that has never fired is a gate nobody should
#  trust, so each of them is made to fire here.
#
#    sh tools/nvramtest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/nvramtest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
VD="$TMP/efivars"
G=8be4df61-93ca-11d2-aa0d-00e098032b8c

echo
echo "Does the boot entry say what the firmware needs to hear?"
echo

cat > "$TMP/n.c" <<'EOC'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvram.h"

/* The partition the fixture pretends AurOS was installed into. */
static void fill(nvram_hd *hd)
{
    memset(hd, 0, sizeof *hd);
    hd->number    = 6;
    hd->first_lba = 5400576;
    hd->blocks    = 98304;
    for (int i = 0; i < 16; i++) hd->guid[i] = (uint8_t)(0x10 + i);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    char why[300] = {0};
    nvram_hd hd; fill(&hd);

    if (!strcmp(argv[1], "option")) {
        /* The raw load option, on stdout, for the python decoder. */
        static uint8_t buf[2048];
        size_t k = nvram_load_option(buf, sizeof buf, "AurOS", &hd,
                                     "\\EFI\\AurOS\\shimx64.efi", NULL);
        if (!k) { fprintf(stderr, "no option\n"); return 1; }
        fwrite(buf, 1, k, stdout);
        return 0;
    }
    if (!strcmp(argv[1], "option-cmdline")) {
        static uint8_t buf[2048];
        size_t k = nvram_load_option(buf, sizeof buf, "AurOS", &hd,
                                     "\\EFI\\AurOS\\shimx64.efi",
                                     "aurstage.restore");
        if (!k) return 1;
        fwrite(buf, 1, k, stdout);
        return 0;
    }
    if (!strcmp(argv[1], "no-partition")) {
        static uint8_t buf[2048];
        nvram_hd empty; memset(&empty, 0, sizeof empty);
        size_t k = nvram_load_option(buf, sizeof buf, "AurOS", &empty,
                                     "\\EFI\\AurOS\\shimx64.efi", NULL);
        printf("%zu\n", k);
        return 0;
    }
    if (!strcmp(argv[1], "set")) {
        uint16_t num = 0xFFFF;
        int rc = nvram_boot_set("AurOS", &hd, "\\EFI\\AurOS\\shimx64.efi",
                                NULL, &num, why, sizeof why);
        printf("%d %04X %s\n", rc, num, why);
        return 0;
    }
    if (!strcmp(argv[1], "next")) {
        int rc = nvram_boot_next((uint16_t)strtoul(argv[2], NULL, 16),
                                 why, sizeof why);
        printf("%d %s\n", rc, why);
        return 0;
    }
    if (!strcmp(argv[1], "forget")) {
        int rc = nvram_boot_forget(argv[2], why, sizeof why);
        printf("%d %s\n", rc, why);
        return 0;
    }
    if (!strcmp(argv[1], "present")) { printf("%d\n", nvram_present()); return 0; }
    return 2;
}
EOC
gcc -O1 -std=gnu11 -Wall -Wextra -DNVRAM_DIR="\"$VD\"" -o "$TMP/n" \
    "$TMP/n.c" src/aurstage/nvram.c \
    -Isrc/aurstage 2>"$TMP/cc.log" \
    || { echo "  nvram.c did not build:"; sed -n '1,12p' "$TMP/cc.log"; exit 2; }

mkdir -p "$VD"

# ── the wire format ─────────────────────────────────────────────────
echo "  the load option, decoded by something that did not write it"
"$TMP/n" option > "$TMP/opt.bin" || { echo "  no option produced"; exit 2; }
D=$(python3 - "$TMP/opt.bin" <<'EOPY'
import sys, importlib.util
spec = importlib.util.spec_from_file_location('ev', 'tools/efivarstore.py')
ev = importlib.util.module_from_spec(spec); spec.loader.exec_module(ev)
lo = ev.load_option(open(sys.argv[1], 'rb').read())
if not lo: raise SystemExit('unparseable')
print('desc=%s' % lo['description'])
print('path=%s' % lo['path'])
print('num=%s' % lo['part_number'])
print('first=%s' % lo['part_first'])
print('blocks=%s' % lo['part_blocks'])
print('guid=%s' % lo['part_guid'])
print('attrs=%d' % lo['attributes'])
print('nodes=%s' % ','.join('%02x/%02x' % (t, s) for t, s, _l in lo['nodes']))
EOPY
) || { echo "  the option could not be decoded at all"; exit 2; }
f() { echo "$D" | sed -n "s/^$1=//p"; }

[ "$(f desc)" = "AurOS" ] && ok "the description comes back" \
                          || bad "the description comes back" "got $(f desc)"
[ "$(f path)" = '\EFI\AurOS\shimx64.efi' ] \
  && ok "and the loader path" || bad "and the loader path" "got $(f path)"
[ "$(f attrs)" = "1" ] && ok "and it is marked ACTIVE" \
                       || bad "and it is marked ACTIVE" "got $(f attrs)"
# THE SHORT FORM. A bare File() node is not one of the two forms UEFI
# 2.10 s10.3.5 makes the boot manager expand; LoadImage then has no
# handle to anchor LocateDevicePath to and returns EFI_NOT_FOUND.
[ "$(f nodes)" = "04/01,04/04,7f/ff" ] \
  && ok "Hard Drive node, File node, End -- the short form UEFI wants" \
  || bad "Hard Drive node, File node, End -- the short form UEFI wants" \
         "got $(f nodes)"
[ "$(f num)" = "6" ] && [ "$(f first)" = "5400576" ] && [ "$(f blocks)" = "98304" ] \
  && ok "the partition number, start and length survive the trip" \
  || bad "the partition number, start and length survive the trip" \
         "got $(f num) $(f first) $(f blocks)"
[ "$(f guid)" = "13121110-1514-1716-1819-1a1b1c1d1e1f" ] \
  && ok "and the partition GUID keeps its on-disk byte order" \
  || bad "and the partition GUID keeps its on-disk byte order" "got $(f guid)"

# The optional data follows the device path, and the device path
# length field is what separates them. Get it wrong and the firmware
# reads the command line as part of the path.
"$TMP/n" option-cmdline > "$TMP/opt2.bin"
D2=$(python3 - "$TMP/opt2.bin" <<'EOPY'
import sys, struct, importlib.util
spec = importlib.util.spec_from_file_location('ev', 'tools/efivarstore.py')
ev = importlib.util.module_from_spec(spec); spec.loader.exec_module(ev)
raw = open(sys.argv[1], 'rb').read()
lo = ev.load_option(raw)
dp_len = struct.unpack_from('<H', raw, 4)[0]
i = 6
while struct.unpack_from('<H', raw, i)[0] != 0: i += 2
i += 2
tail = raw[i + dp_len:]
print('path=%s' % lo['path'])
print('tail=%s' % tail.decode('utf-16-le').rstrip('\x00'))
EOPY
)
[ "$(echo "$D2" | sed -n 's/^tail=//p')" = "aurstage.restore" ] \
  && ok "a command line lands after the device path, not inside it" \
  || bad "a command line lands after the device path, not inside it" "$D2"

[ "$("$TMP/n" no-partition)" = "0" ] \
  && ok "an entry naming no partition is refused, not written" \
  || bad "an entry naming no partition is refused, not written"

# ── choosing a slot ─────────────────────────────────────────────────
echo
echo "  choosing which Boot#### to be"
rm -f "$VD"/*
R=$("$TMP/n" set)
[ "${R%% *}" = "0" ] && ok "an empty menu gets Boot0000" || bad "an empty menu gets Boot0000" "$R"
[ "$(echo "$R" | cut -d' ' -f2)" = "0000" ] \
  || bad "an empty menu gets Boot0000" "it took $(echo "$R" | cut -d' ' -f2)"
[ -f "$VD/Boot0000-$G" ] && ok "and the variable is really there" \
                        || bad "and the variable is really there" "$(ls "$VD")"
# Four bytes of attributes in front, in one write.
python3 - "$VD/Boot0000-$G" "$TMP/opt.bin" <<'EOPY'
import sys, struct
got = open(sys.argv[1], 'rb').read()
want = open(sys.argv[2], 'rb').read()
attrs = struct.unpack_from('<I', got, 0)[0]
raise SystemExit(0 if attrs == 7 and got[4:] == want else 1)
EOPY
[ $? -eq 0 ] && ok "non-volatile, boot and runtime, then the option itself" \
             || bad "non-volatile, boot and runtime, then the option itself"

# The lowest FREE number, and a number is free only when there is
# nothing in it. plat_win.c's scar: a probe that could not tell an
# unreadable entry from an absent one declared Boot0000 free on every
# UEFI machine and overwrote the Windows Boot Manager.
rm -f "$VD"/*
for i in 0000 0001 0002 0004; do printf 'xxxxyyyy' > "$VD/Boot$i-$G"; done
R=$("$TMP/n" set)
[ "$(echo "$R" | cut -d' ' -f2)" = "0003" ] \
  && ok "it takes the lowest gap and steps over what is in use" \
  || bad "it takes the lowest gap and steps over what is in use" "$R"
for i in 0000 0001 0002 0004; do
    [ "$(cat "$VD/Boot$i-$G")" = "xxxxyyyy" ] || \
        bad "and does not touch anything already there" "Boot$i changed"
done
ok "and does not touch anything already there"

# An entry we wrote before is ours to reuse -- and it is found by its
# description, not by "the ones we do not recognise", which also
# selects the Fedora somebody installed last month.
rm -f "$VD"/*
cp "$TMP/opt.bin" "$TMP/reuse.bin"
python3 - "$VD/Boot0007-$G" "$TMP/opt.bin" <<'EOPY'
import sys, struct
open(sys.argv[1], 'wb').write(struct.pack('<I', 7) + open(sys.argv[2], 'rb').read())
EOPY
printf 'xxxxyyyy' > "$VD/Boot0000-$G"
R=$("$TMP/n" set)
[ "$(echo "$R" | cut -d' ' -f2)" = "0007" ] \
  && ok "a previous attempt's own entry is replaced, not duplicated" \
  || bad "a previous attempt's own entry is replaced, not duplicated" "$R"

# ── BootNext ────────────────────────────────────────────────────────
echo
echo "  the one-shot"
rm -f "$VD"/*
R=$("$TMP/n" next 0042)
[ "${R%% *}" = "0" ] && ok "BootNext is written" || bad "BootNext is written" "$R"
python3 - "$VD/BootNext-$G" <<'EOPY'
import sys, struct
b = open(sys.argv[1], 'rb').read()
raise SystemExit(0 if len(b) == 6 and struct.unpack_from('<I', b, 0)[0] == 7
                 and struct.unpack_from('<H', b, 4)[0] == 0x42 else 1)
EOPY
[ $? -eq 0 ] && ok "as two bytes, little-endian, behind its attributes" \
             || bad "as two bytes, little-endian, behind its attributes" \
                    "$(od -An -tx1 "$VD/BootNext-$G")"
# NOTHING HERE MAY WRITE BootOrder. Not a missing feature: it is the
# whole of rule 3, and the way it gets broken is somebody finding a
# function that already does it.
# COMMENTS ARE NOT CODE, which is the same trap build/staging's own
# write gate fell into: nvram.c explains in a comment that BootOrder
# and BootNext start with the same four letters, and a plain grep read
# that as the file writing BootOrder.
gcc -fpreprocessed -dD -E src/aurstage/nvram.c 2>/dev/null \
    | grep -q 'BootOrder' \
  && bad "nothing in this file can write BootOrder" \
         "nvram.c names BootOrder in its code" \
  || ok "nothing in this file can write BootOrder"

# ── forgetting ──────────────────────────────────────────────────────
echo
echo "  tidying away the entry the Windows half made"
rm -f "$VD"/*
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
plant 0000 "Windows Boot Manager"
plant 0001 "AurOS Installer"
plant 0002 "AurOS"
plant 0003 "AurOS Installer"
R=$("$TMP/n" forget "AurOS Installer")
[ "${R%% *}" = "2" ] && ok "both copies of it go" || bad "both copies of it go" "$R"
[ -f "$VD/Boot0000-$G" ] && [ -f "$VD/Boot0002-$G" ] \
  && ok "and Windows Boot Manager and AurOS stay" \
  || bad "and Windows Boot Manager and AurOS stay" "$(ls "$VD")"
# EXACTLY, never a prefix. "AurOS" and "AurOS Installer" are different
# entries and one of them is the one that boots.
R=$("$TMP/n" forget "AurOS")
[ "${R%% *}" = "1" ] && [ ! -f "$VD/Boot0002-$G" ] && [ -f "$VD/Boot0000-$G" ] \
  && ok "a description is matched whole, never as a prefix" \
  || bad "a description is matched whole, never as a prefix" "$R $(ls "$VD")"

# ── the gate ────────────────────────────────────────────────────────
echo
echo "  and the build refuses an nvram.c that has stopped being one"
cp src/aurstage/nvram.c "$TMP/nvram.bak"
gate() { # description  sed-script  expected-message
    cp "$TMP/nvram.bak" src/aurstage/nvram.c
    sed -i "$2" src/aurstage/nvram.c
    if ./build/staging desktop 2>&1 | grep -q "$3"; then ok "$1"
    else bad "$1" "the build allowed it"; fi
    cp "$TMP/nvram.bak" src/aurstage/nvram.c
}
if [ -d work/forge/desktop/rootfs ]; then
    # `@` as sed's delimiter throughout: two of these patterns contain
    # a `|`, and with `|` as the delimiter they matched nothing at all
    # -- so the gate test reported that the gate had allowed something
    # it was never shown. A test that is not testing is the only kind
    # worse than no test, and this file exists to say that about
    # somebody else's code.
    gate "one that names a device"        's@"efivarfs"@"/dev/sda"@' \
         'names a device'
    gate "one that opens something read-write" \
         's@O_WRONLY | O_CREAT@O_RDWR | O_CREAT@' 'read-write'
    gate "one that no longer writes anything" \
         's@O_WRONLY | O_CREAT, 0644@O_RDONLY, 0644@' 'no longer writes'
    gate "one that writes somewhere else" \
         's@/sys/firmware/efi/efivars@/run/auros/vars@' \
         'does not name the efivarfs mount point'
    ./build/staging desktop >/dev/null 2>&1 \
      && ok "...and the real tree still builds" \
      || bad "...and the real tree still builds"
else
    echo "    (no forged rootfs here; the build gate was not exercised)"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "A wrong boot entry is a computer that starts nothing and says why to"
    echo "nobody."
    exit 1
fi
echo "$checked checks: the entry decodes to what the firmware needs, it takes"
echo "a slot nothing else is using, BootOrder is out of reach, and the build"
echo "refuses an nvram.c that is not one."
exit 0
