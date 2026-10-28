#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  gapfencetest — the fast path's one safety property, on its own
#
#  The fast path (docs/AURBRIDGE.md D1) writes AurOS raw into the
#  unallocated gap a shrink frees, on the SYSTEM disk. The single thing
#  that makes that defensible is the fence: a write can land only inside
#  the gap it was given and never on a partition, checked against the
#  live table. This exercises that fence against the simulated platform
#  -- the same plat_sim.c the engine is tested through -- on a disk with
#  a real GPT made by sgdisk.
#
#  It is deliberately a unit test of plat_gap_* and fmt_gpt_parts: no
#  QEMU, no root, no shrink. The shrink is Windows' own and is proven
#  against real Windows in windows.yml's gapselftest; here we prove the
#  thing that stands between a raw write and somebody's Windows.
#
#    sh tools/gapfencetest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-58s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-58s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

command -v sgdisk >/dev/null 2>&1 || { echo "  no sgdisk; skipping"; exit 0; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/gapfence.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# A harness that drives the real fence (plat_sim.c) and the real parser
# (format.c). Nothing here is reimplemented.
cat > "$TMP/h.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
int main(int argc, char **argv)
{
    char why[PLAT_WHY] = "";
    if (argc >= 3 && !strcmp(argv[1], "parts")) {
        fmt_extent e[64]; int n = 0;
        if (plat_disk_parts(atoi(argv[2]), e, 64, &n, why, sizeof why) != 0) {
            printf("PARTS err %s\n", why); return 1;
        }
        printf("PARTS %d\n", n);
        for (int i = 0; i < n; i++)
            printf("P %llu %llu\n", (unsigned long long)e[i].first_byte,
                   (unsigned long long)e[i].last_byte);
        return 0;
    }
    /* allow <idx> <lo> <hi> */
    if (argc >= 5 && !strcmp(argv[1], "allow")) {
        int r = plat_gap_allow(atoi(argv[2]), strtoull(argv[3],0,10),
                               strtoull(argv[4],0,10), why, sizeof why);
        printf("ALLOW %s %s\n", r == 0 ? "ok" : "refused", why);
        return r == 0 ? 0 : 1;
    }
    /* write <idx> <lo> <hi> <off> <len>  -- arm then write <len> of 0xAB */
    if (argc >= 7 && !strcmp(argv[1], "write")) {
        int idx = atoi(argv[2]);
        if (plat_gap_allow(idx, strtoull(argv[3],0,10), strtoull(argv[4],0,10),
                           why, sizeof why) != 0) {
            printf("ARM refused %s\n", why); return 2;
        }
        unsigned long long off = strtoull(argv[5],0,10), len = strtoull(argv[6],0,10);
        char *buf = malloc(len); memset(buf, 0xAB, len);
        int r = plat_gap_write(idx, off, buf, len, why, sizeof why);
        if (r != 0) { printf("WRITE refused %s\n", why); free(buf); return 1; }
        /* read it back through plat_read and check it is what we wrote */
        char *back = malloc(len);
        int rr = plat_read(idx, off, back, len, why, sizeof why);
        int same = rr == 0 && memcmp(buf, back, len) == 0;
        printf("WRITE ok VERIFY %s\n", same ? "ok" : "bad");
        free(buf); free(back);
        return same ? 0 : 1;
    }
    /* nowrite <idx> <off> <len> -- write with nothing armed */
    if (argc >= 5 && !strcmp(argv[1], "nowrite")) {
        plat_gap_forbid();
        char buf[4096]; memset(buf, 0xAB, sizeof buf);
        int r = plat_gap_write(atoi(argv[2]), strtoull(argv[3],0,10),
                               strtoull(argv[4],0,10) < sizeof buf ?
                               strtoull(argv[4],0,10) : sizeof buf, buf, why, sizeof why);
        printf("NOWRITE %s %s\n", r == 0 ? "ok" : "refused", why);
        return r == 0 ? 0 : 1;
    }
    return 2;
}
EOC
cc -O2 -std=gnu11 -Wall -Wextra -I src/aurbridge -o "$TMP/h" "$TMP/h.c" \
   src/aurbridge/plat_sim.c src/aurbridge/format.c 2>"$TMP/cc.log" \
   || { echo "  harness did not build:"; sed -n 1,20p "$TMP/cc.log"; exit 2; }

# A 64 MiB disk: ESP, a "Windows" partition, and a gap at the end.
DISK="$TMP/disk0.img"
truncate -s 64M "$DISK"
sgdisk --zap-all "$DISK" >/dev/null 2>&1
sgdisk -n 1:2048:+16M   -t 1:C12A7328-F81F-11D2-BA4B-00A0C93EC93B -c 1:"ESP" "$DISK" >/dev/null 2>&1
sgdisk -n 2:0:+24M      -t 2:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 -c 2:"Windows" "$DISK" >/dev/null 2>&1
# Partition 2 ends; the rest of the disk (up to the backup GPT) is the gap.
mkdir -p "$TMP/sim"
printf '0\t%s\t67108864\t512\tSIMSER0\tSIM DISK\t0\n' "$DISK" > "$TMP/sim/disks.txt"
export AURBRIDGE_SIM="$TMP/sim"
H() { "$TMP/h" "$@"; }

echo
echo "The fence: a write lands only in the gap, never on a partition."
echo

# Where the partitions are, and where the gap is.
P=$(H parts 0)
NPARTS=$(printf '%s\n' "$P" | sed -n 's/^PARTS //p')
[ "$NPARTS" = "2" ] && ok "the two partitions are read from the live table" \
    || bad "the two partitions are read from the live table" "$P"
WIN_LAST=$(printf '%s\n' "$P" | sed -n '3p' | awk '{print $3}')   # P2 last byte
GAP_LO=$(( (WIN_LAST/1048576 + 1) * 1048576 ))                    # next MiB
GAP_HI=$(( 63 * 1048576 ))                                        # short of backup GPT

# The happy path: a write inside the gap, read back unchanged.
R=$(H write 0 "$GAP_LO" "$GAP_HI" "$((GAP_LO + 1048576))" 1048576)
case "$R" in
  "WRITE ok VERIFY ok") ok "a write inside the gap is written and reads back" ;;
  *) bad "a write inside the gap is written and reads back" "$R" ;;
esac

# A write that reaches back onto the Windows partition: refused. The
# window is armed from the start of Windows so the overlap, not the
# window edge, is what must stop it.
WIN_FIRST=$(printf '%s\n' "$P" | sed -n '3p' | awk '{print $2}')
R=$(H write 0 "$WIN_FIRST" "$GAP_HI" "$WIN_FIRST" 1048576)
case "$R" in
  "ARM refused "*) ok "a gap window that overlaps Windows is refused at arm" ;;
  *) bad "a gap window that overlaps Windows is refused at arm" "$R" ;;
esac

# A write outside the armed gap (before it): refused, not clamped.
R=$(H write 0 "$GAP_LO" "$GAP_HI" "$((GAP_LO - 1048576))" 1048576)
case "$R" in
  "WRITE refused "*) ok "a write before the gap is refused, not clamped" ;;
  *) bad "a write before the gap is refused, not clamped" "$R" ;;
esac

# A write past the armed gap: refused.
R=$(H write 0 "$GAP_LO" "$GAP_HI" "$((GAP_HI - 524288))" 1048576)
case "$R" in
  "WRITE refused "*) ok "a write running past the gap is refused" ;;
  *) bad "a write running past the gap is refused" "$R" ;;
esac

# A write with nothing armed at all: refused.
R=$(H nowrite 0 "$GAP_LO" 4096)
case "$R" in
  "NOWRITE refused "*) ok "a write with no gap armed is refused" ;;
  *) bad "a write with no gap armed is refused" "$R" ;;
esac

echo
if [ "$fail" -gt 0 ]; then echo "$fail of $checked wrong."; exit 1; fi
echo "$checked checks: the fence lets a write into the gap and refuses"
echo "everything that would reach a partition or leave the gap."
exit 0
