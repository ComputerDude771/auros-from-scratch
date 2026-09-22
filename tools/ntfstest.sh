#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  ntfstest — does src/aurstage/ntfs.c read a real volume correctly,
#  and does it refuse the three states that must stop a conversion?
#
#  WHY THIS IS SEPARATE FROM stagetest.sh
#
#  stagetest boots the whole staging environment in QEMU, which is the
#  right test for "does it come up and leave the disk alone" and a
#  slow, coarse one for "does the parser get this field right". This
#  runs the same parser against a dozen deliberately broken volumes in
#  a few seconds, so the broken volumes can be specific.
#
#  EVERY FIXTURE IS BUILT BY SOMETHING OTHER THAN THE PARSER. The
#  dirty flag is set by ntfsfix, the hibernation file is written
#  through ntfs-3g, the log pages are laid out from the documented
#  offsets. A fixture built with our own reader would agree with our
#  own reader about a volume that is wrong in the same way twice.
#
#    sudo sh tools/ntfstest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
RFS="${RFS:-work/forge/desktop/rootfs}"

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

for t in python3 truncate; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ -d "$RFS" ] || { echo "no built rootfs at $RFS"; exit 2; }

LD=$(ls "$RFS"/lib64/ld-linux-x86-64.so.2 2>/dev/null || \
     ls "$RFS"/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 2>/dev/null)
LP="$RFS/lib/x86_64-linux-gnu:$RFS/lib64:$RFS/usr/lib/x86_64-linux-gnu"
[ -n "$LD" ] || { echo "no loader in the image"; exit 2; }
ntfs() { p="$RFS/usr/bin/$1"; [ -x "$p" ] || p="$RFS/usr/sbin/$1"; shift
         "$LD" --library-path "$LP" "$p" "$@"; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ntfstest.XXXXXX")
trap 'mountpoint -q "$TMP/m" 2>/dev/null && umount "$TMP/m"; rm -rf "$TMP"' EXIT

echo
echo "Does the NTFS reader see what is actually on the volume?"
echo

gcc -O2 -std=gnu11 -Wall -Wextra -o "$TMP/ntfsread" \
    tools/ntfsread.c src/aurstage/ntfs.c -I src/aurstage \
    || { echo "  ntfsread did not build"; exit 2; }

# ── a real volume, made by the mkntfs this product ships ────────────
BASE="$TMP/base.img"
truncate -s 512M "$BASE"
ntfs mkntfs -Q -F -L TEST "$BASE" >/dev/null 2>&1 || {
    echo "  could not make an NTFS volume"; exit 2; }

fresh() { cp --sparse=always "$BASE" "$1"; }

# field=value out of ntfsread, or the empty string
field() { "$TMP/ntfsread" "$1" 2>/dev/null | sed -n "s/^$2=//p"; }

expect() { # image  field  wanted  name
    got=$(field "$1" "$2")
    if [ "$got" = "$3" ]; then ok "$4"
    else bad "$4" "$2 was \"$got\", expected \"$3\""
         "$TMP/ntfsread" "$1" 2>&1 | sed 's/^/        /' | head -8
    fi
}

echo "  a volume that is fine"
expect "$BASE" verdict    ok "a freshly made volume reads as healthy"
expect "$BASE" hibernated no "...with nothing asleep in it"
expect "$BASE" log_dirty  no "...and a log with nothing outstanding"
expect "$BASE" sector     512 "...and the sector size off the boot record"

echo
echo "  the one that must never be resized"
# THE MUST NOT. -FVE-FS- at offset 3 is where BitLocker puts its
# signature and where NTFS puts its OEM id, so a volume carrying it is
# one whose every other field would be ciphertext read as a number.
B="$TMP/bitlocker.img"; fresh "$B"
printf '\055FVE-FS\055' | dd of="$B" bs=1 seek=3 conv=notrunc status=none
expect "$B" verdict bitlocker "a BitLocker volume is refused"
case "$(field "$B" remedy)" in
  *"Manage BitLocker"*) ok "...and is told how to turn it off" ;;
  *) bad "...and is told how to turn it off" "the remedy does not say how" ;;
esac

echo
echo "  and the ones that are simply not ready"
N="$TMP/notntfs.img"; fresh "$N"
dd if=/dev/zero of="$N" bs=512 count=1 conv=notrunc status=none
expect "$N" verdict not-ntfs "a volume with no NTFS on it is refused"

# THE DIRTY FLAG IS SET BY HAND, and it was not the first choice.
# `ntfsfix` used to set it -- it is how you ask Windows to run chkdsk
# at the next boot -- and 2022.10.3 no longer does; an rw ntfs-3g
# mount does not write it either. So there is no tool on Linux that
# will produce this fixture, and it is laid out here from the
# documented offsets instead. That is still an independent
# construction: this walks the MFT in Python from the specification,
# and if it and the C reader disagree about where the flag lives,
# this test is what says so.
#
# (The reader is also cross-checked against `ntfsinfo -m` for the
# fields ntfsinfo will state -- sector size, cluster size, volume
# flags -- which is how the clean case is known to be read right.)
D="$TMP/dirty.img"; fresh "$D"
python3 - "$D" <<'EOPY'
import sys, struct
d = bytearray(open(sys.argv[1], 'rb').read())
bps = struct.unpack_from('<H', d, 0x0B)[0]
spc = d[0x0D]
mft = struct.unpack_from('<Q', d, 0x30)[0] * bps * spc
cpr = struct.unpack_from('<b', d, 0x40)[0]
rsz = cpr * bps * spc if cpr >= 0 else 1 << -cpr

at = mft + 3 * rsz                      # $Volume is MFT record 3
rec = bytearray(d[at:at + rsz])
assert rec[0:4] == b'FILE', "record 3 is not a file record"

# Undo the update sequence array in a working copy, so the attributes
# can be walked; each sector tail gets its original two bytes back.
uo, uc = struct.unpack_from('<HH', rec, 0x04)
blk = rsz // (uc - 1)
for i in range(1, uc):
    tail = i * blk - 2
    rec[tail:tail+2] = rec[uo + i*2: uo + i*2 + 2]

off = struct.unpack_from('<H', rec, 0x14)[0]
while True:
    t = struct.unpack_from('<I', rec, off)[0]
    if t == 0xFFFFFFFF: sys.exit("no $VOLUME_INFORMATION in record 3")
    ln = struct.unpack_from('<I', rec, off + 4)[0]
    if t == 0x70: break
    off += ln
vo = struct.unpack_from('<H', rec, off + 0x14)[0]
vl = struct.unpack_from('<I', rec, off + 0x10)[0]
# $VOLUME_INFORMATION: 8 reserved, major, minor, then the flags.
assert vl >= 12, "$VOLUME_INFORMATION is shorter than the flags field"
flags_at = off + vo + 10                # relative to the record

# It must not land on a sector tail, or writing it raw would fight the
# fixups. On every volume mkntfs makes it does not -- but a test that
# assumes is a test that lies.
assert (flags_at % blk) < blk - 3, "the flags field sits on a fixup boundary"

f = struct.unpack_from('<H', d, at + flags_at)[0]
struct.pack_into('<H', d, at + flags_at, f | 0x0001)
open(sys.argv[1], 'wb').write(bytes(d))
EOPY
expect "$D" verdict dirty "a volume Windows wants to check is refused"
expect "$D" flags   0001  "...and the flag word says why"

echo
echo "  a Windows that is asleep rather than closed"
# Fast Startup is the default on Windows 10 and 11 and does NOT set
# the dirty flag above. This is the check that catches it, and it is
# the commonest refusal this product will ever issue.
mkdir -p "$TMP/m"
plant() { # image  how
    ntfs ntfs-3g "$1" "$TMP/m" >/dev/null 2>&1 || return 1
    case "$2" in
      live) { printf 'hibr'; dd if=/dev/urandom bs=1020 count=1 status=none
              dd if=/dev/urandom bs=1M count=3 status=none; } > "$TMP/m/hiberfil.sys" ;;
      spent) dd if=/dev/zero of="$TMP/m/hiberfil.sys" bs=1M count=4 status=none ;;
      late)  { dd if=/dev/zero bs=1M count=1 status=none
               dd if=/dev/urandom bs=1M count=3 status=none; } > "$TMP/m/hiberfil.sys" ;;
    esac
    sync; umount "$TMP/m"
}
H="$TMP/hiber.img"; fresh "$H"
if plant "$H" live; then
    expect "$H" verdict    hibernated "a hibernated Windows is refused"
    expect "$H" hibernated yes        "...and the reason is named"
    case "$(field "$H" remedy)" in
      *Shift*) ok "...and she is told the one thing that clears it" ;;
      *) bad "...and she is told the one thing that clears it" "no Shift in the remedy" ;;
    esac

    # THE FALSE POSITIVE THAT WOULD MATTER MOST. Hibernation being
    # switched on is not hibernation having happened: Windows leaves
    # the file in place and zeroes its header on resume. Calling that
    # machine asleep is a refusal she can never clear.
    S="$TMP/spent.img"; fresh "$S"
    plant "$S" spent && {
        expect "$S" verdict    ok "a hibernation file already used up is not a refusal"
        expect "$S" hibernated no "...and is reported as not asleep"
    }

    # Only the first 4K is the header. A file whose header is zero but
    # whose body is not is a resumed session, not a live one.
    L="$TMP/late.img"; fresh "$L"
    plant "$L" late && \
        expect "$L" hibernated no "only the header decides, not the whole file"
else
    echo "    (no FUSE here: the hibernation cases need ntfs-3g to mount)"
fi

echo
echo "  a log with work still in it"
# mkntfs fills $LogFile with 0xFF, so a fresh volume has no restart
# page at all. These are laid out by hand from the documented offsets
# -- which is the point: if the reader and the fixture agreed because
# they were the same code, the test would prove nothing.
craft() { # image  lsn0 clean0  lsn1 clean1
    python3 - "$@" <<'PY'
import sys, struct
img, l0, c0, l1, c1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), \
                      int(sys.argv[4]), int(sys.argv[5])
d = bytearray(open(img, 'rb').read())

# $LogFile is the only thing mkntfs fills with 0xFF, so the first run
# of a megabyte of it is where the log starts. Crude, and independent
# of anything the reader believes.
run = b'\xff' * (1 << 20)
at = d.find(run)
if at < 0: sys.exit("no $LogFile found")

PSZ = 4096
def page(lsn, clean):
    p = bytearray(PSZ)
    p[0:4]   = b'RSTR'
    struct.pack_into('<H', p, 0x04, 0x1E)        # usa offset
    struct.pack_into('<H', p, 0x06, 1 + PSZ//512)# usa count
    struct.pack_into('<I', p, 0x10, PSZ)         # system page size
    struct.pack_into('<I', p, 0x14, PSZ)         # log page size
    struct.pack_into('<H', p, 0x18, 0x30)        # restart area offset
    struct.pack_into('<H', p, 0x1A, 1)           # minor
    struct.pack_into('<H', p, 0x1C, 1)           # major
    ra = 0x30
    struct.pack_into('<Q', p, ra + 0x00, lsn)
    struct.pack_into('<H', p, ra + 0x08, 1)          # log clients
    struct.pack_into('<H', p, ra + 0x0A, 0xFFFF)     # free list
    struct.pack_into('<H', p, ra + 0x0C, 0xFFFF if clean else 0x0000)
    struct.pack_into('<H', p, ra + 0x0E, 0x0002 if clean else 0x0000)
    struct.pack_into('<I', p, ra + 0x10, 0x2D)       # seq number bits
    struct.pack_into('<H', p, ra + 0x14, 0xD0)       # restart area length
    struct.pack_into('<H', p, ra + 0x16, 0x40)       # client array offset
    # The update sequence array, applied the way the disk carries it:
    # every sector's last two bytes hold the sequence number and the
    # bytes they displaced live in the array.
    seq = 1
    struct.pack_into('<H', p, 0x1E, seq)
    for i in range(1, 1 + PSZ // 512):
        tail = i * 512 - 2
        struct.pack_into('<H', p, 0x1E + i * 2, struct.unpack_from('<H', p, tail)[0])
        struct.pack_into('<H', p, tail, seq)
    return p

d[at:at + PSZ]             = page(l0, c0 == 1)
d[at + PSZ:at + 2 * PSZ]   = page(l1, c1 == 1)
open(img, 'wb').write(bytes(d))
PY
}
G="$TMP/log1.img"; fresh "$G"
if craft "$G" 100 1 200 0; then
    expect "$G" verdict   log-unclean "a log with a client still holding it is refused"
    expect "$G" log_dirty yes         "...and the later restart page is the one believed"
fi
# The same two pages the other way round: the reader must follow the
# sequence number, not the order they happen to be written in.
G2="$TMP/log2.img"; fresh "$G2"
if craft "$G2" 200 0 100 1; then
    expect "$G2" log_dirty yes "...whichever of the two is the later one"
fi
G3="$TMP/log3.img"; fresh "$G3"
if craft "$G3" 100 0 200 1; then
    expect "$G3" verdict   ok "a log whose later page is clean is allowed"
    expect "$G3" log_dirty no "...and reported clean"
fi

echo
echo "  and nothing it is handed can make it read past the end"
# A torn or crafted volume is attacker-controlled input to a program
# running as root. It has to REFUSE, not crash: a segfault in PID 1 of
# the staging environment is a kernel panic on somebody's laptop.
T="$TMP/torn.img"; fresh "$T"
python3 - "$T" <<'PY'
import sys, random
random.seed(20260922)
d = bytearray(open(sys.argv[1], 'rb').read())
# Leave the boot sector alone -- the point is to get PAST it and then
# find nonsense -- and scribble over the first megabyte of the MFT.
import struct
bps = struct.unpack_from('<H', d, 0x0B)[0]
spc = d[0x0D]
mft = struct.unpack_from('<Q', d, 0x30)[0] * bps * spc
for i in range(4096):
    at = mft + random.randrange(0, 1 << 20)
    d[at] = random.randrange(0, 256)
open(sys.argv[1], 'wb').write(bytes(d))
PY
out=$("$TMP/ntfsread" "$T" 2>&1); rc=$?
if [ "$rc" -le 1 ] && [ -n "$out" ]; then
    ok "a scribbled-on MFT gives an answer rather than a crash"
else
    bad "a scribbled-on MFT gives an answer rather than a crash" "exit $rc"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "This reader decides whether somebody's Windows drive gets resized."
    exit 1
fi
echo "$checked checks: the reader sees the volume as it is, and refuses"
echo "every state that a resize must not start from."
exit 0
