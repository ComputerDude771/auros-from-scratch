#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  committest — phase 8, and the claim the whole ordering makes
#
#  Rule 4: "one atomic commit point per destructive phase. All data
#  movement happens with the old layout still in force; the new
#  partition table is a single sector write."
#
#  That claim is testable and this file tests it. The commit is
#  stopped after each flush and the disk is read back the way a
#  firmware or a kernel would read it. Until LBA 1 lands the answer
#  must be THE OLD LAYOUT, every time. After it lands, the new one.
#
#  A snapshot is taken at each step rather than reasoning about the
#  order, because reasoning about write ordering is how everybody who
#  has ever got this wrong got it wrong.
#
#    sh tools/committest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

command -v sgdisk >/dev/null 2>&1 || { echo "need sgdisk"; exit 2; }
TMP=$(mktemp -d "${TMPDIR:-/tmp}/committest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

echo
echo "Is the commit really one sector -- and does the disk read as the"
echo "old layout until that sector lands?"
echo

cat > "$TMP/c.c" <<'EOC'
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "commit.h"

static const char *g_img;
static int g_stop;          /* snapshot after this step and stop     */

/* Copy the whole disk the moment a step finishes, so it can be read
 * back exactly as a reader would have found it at that instant. */
static void snap(int step, void *ud)
{
    (void)ud;
    char dst[512];
    snprintf(dst, sizeof dst, "%s.step%d", g_img, step);
    int a = open(g_img, O_RDONLY), b = open(dst, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (a >= 0 && b >= 0) {
        char buf[1 << 16]; ssize_t k;
        while ((k = read(a, buf, sizeof buf)) > 0) { ssize_t w = write(b, buf, k); (void)w; }
    }
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    if (g_stop && step >= g_stop) _exit(7);   /* the power goes here */
}

static uint64_t fsize(const char *p)
{ struct stat st; return stat(p, &st) == 0 ? (uint64_t)st.st_size : 0; }

int main(int argc, char **argv)
{
    /* commit <img> <win_first_lba> <win_new_bytes> <root_src> <rec> <min> [stop] */
    if (argc < 7) return 2;
    g_img = argv[1];
    g_stop = argc > 7 ? atoi(argv[7]) : 0;

    int fd = open(g_img, O_RDONLY);
    gpt_table old;
    if (fd < 0 || gpt_read(fd, 512, fsize(g_img), &old) != 0) {
        puts("read=failed"); return 2;
    }
    close(fd);

    int wi = gpt_find_start(&old, strtoull(argv[2], NULL, 10));
    stage_layout L; char why[240] = {0};
    if (plan_compute(&old, wi, strtoull(argv[3], NULL, 10),
                     strtoull(argv[4], NULL, 10), strtoull(argv[5], NULL, 10),
                     128ull * 1024 * 1024,
                     strtoull(argv[6], NULL, 10), &L, why, sizeof why) != 0) {
        printf("plan=failed\n"); printf("why=%s\n", why); return 2;
    }
    gpt_table nw;
    if (commit_build(&old, &L, &nw, why, sizeof why) != 0) {
        printf("build=failed\n"); printf("why=%s\n", why); return 2;
    }
    printf("build=ok\n");

    wr_target t;
    if (wr_open(&t, g_img, why, sizeof why) != 0) {
        printf("open=failed\n"); return 2;
    }
    uint32_t ss = nw.sector;
    size_t ab = gpt_array_bytes(&nw);
    uint64_t alt = nw.disk_sectors - 1;
    uint64_t barr = alt - (ab + ss - 1) / ss;
    if (wr_arm(&t, WR_GPT_BACKUP, barr * ss, (alt + 1) * ss, why, sizeof why) != 0 ||
        wr_arm(&t, WR_GPT_PRIMARY, 1ull * ss,
               nw.entry_lba * ss + ab, why, sizeof why) != 0) {
        printf("arm=failed\n"); printf("why=%s\n", why); return 2;
    }
    int r = commit_table(&t, &nw, snap, NULL, why, sizeof why);
    printf("commit=%d\n", r);
    printf("why=%s\n", why);
    wr_close(&t);
    return r ? 1 : 0;
}
EOC
gcc -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I src/aurstage -o "$TMP/c" "$TMP/c.c" \
    src/aurstage/commit.c src/aurstage/gpt.c src/aurstage/plan.c \
    src/aurstage/wr.c src/aurstage/boot.c src/aurstage/disks.c \
    src/aurstage/sha256.c src/aurstage/ntfs.c src/aurstage/fde.c \
    src/aurstage/rescue.c src/aurstage/shrink.c src/aurstage/fault.c \
    || { echo "  did not build"; exit 2; }

# The OEM shape again: ESP, MSR, Windows, and a recovery partition at
# the very end, with a gap opened in the middle by the shrink.
mkdisk() {
    D="$1"; rm -f "$D"; truncate -s 3G "$D"
    sgdisk --zap-all "$D" >/dev/null 2>&1
    sgdisk -n 1:2048:+260M -t 1:ef00 -c 1:"EFI"     "$D" >/dev/null 2>&1
    sgdisk -n 2:0:+128M    -t 2:0c01 -c 2:"MSR"     "$D" >/dev/null 2>&1
    sgdisk -n 3:0:+1G      -t 3:0700 -c 3:"Windows" "$D" >/dev/null 2>&1
    sgdisk -n 4:5500000:0  -t 4:2700 -c 4:"WinRE"   "$D" >/dev/null 2>&1
}
parts() { sgdisk -p "$1" 2>/dev/null | sed -n '/^Number/,$p' | tail -n +2 |
          awk 'NF{print $1":"$2":"$3}'; }
G() { sed -n "s/^$1=//p"; }

D="$TMP/d.img"; mkdisk "$D"
WS=$(sgdisk -i 3 "$D" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
BEFORE=$(parts "$D")
echo "  a disk with 4 partitions and a gap after Windows"

echo
echo "  building the new table"
OUT=$("$TMP/c" "$D" "$WS" 536870912 268435456 209715200 200000000 2>/dev/null)
[ "$(printf '%s\n' "$OUT" | G build)" = "ok" ] && ok "the new table is built" \
    || bad "the new table is built" "$OUT"
[ "$(printf '%s\n' "$OUT" | G commit)" = "0" ] && ok "and written" \
    || bad "and written" "$(printf '%s\n' "$OUT" | G why)"

AFTER=$(parts "$D")
N=$(printf '%s\n' "$AFTER" | wc -l)
# Four before (ESP, MSR, Windows, OEM recovery) plus the three AurOS
# adds: the root, the saved copy of this machine's Windows startup, and
# the recovery partition.
[ "$N" = "7" ] && ok "the disk now has 7 partitions" || bad "the disk now has 7 partitions" "got $N"
# sgdisk is an independent reader: if it agrees, our CRCs are right.
if sgdisk -v "$D" 2>&1 | grep -q "No problems found"; then
    ok "and another tool agrees the table is valid"
else
    bad "and another tool agrees the table is valid" "$(sgdisk -v "$D" 2>&1 | head -3)"
fi
printf '%s\n' "$AFTER" | sed 's/^/      p/'

echo
echo "  and what a reader would have seen at each instant"
# Snapshots were taken after each flush. Until LBA 1 lands the disk
# must still read as the OLD layout -- that is the whole claim.
for s in 1; do
    if [ -f "$D.step$s" ]; then
        GOT=$(parts "$D.step$s")
        if [ "$GOT" = "$BEFORE" ]; then
            ok "after step $s the disk still reads as the old layout"
        else
            bad "after step $s the disk still reads as the old layout" \
                "it already shows the new one -- the commit is not one sector"
        fi
    else
        bad "after step $s the disk still reads as the old layout" "no snapshot"
    fi
done
if [ -f "$D.step2" ]; then
    GOT=$(parts "$D.step2")
    [ "$GOT" != "$BEFORE" ] && ok "after step 2 -- the one sector -- it reads as the new one" \
                            || bad "after step 2 it reads as the new one" "still the old layout"
else
    bad "after step 2 it reads as the new one" "no snapshot"
fi
# And the backup, written afterwards, must agree with the primary.
if [ -f "$D.step3" ]; then
    if sgdisk -v "$D.step3" 2>&1 | grep -q "No problems found"; then
        ok "and after step 3 both copies of the table agree"
    else
        bad "and after step 3 both copies of the table agree" \
            "$(sgdisk -v "$D.step3" 2>&1 | head -3)"
    fi
fi

echo
echo "  and a power cut before that sector"
# Stop the program dead after step 1 -- the array is down, the commit
# sector is not -- the way losing power would, and then ask what is on
# the disk.
D2="$TMP/d2.img"; mkdisk "$D2"
B2=$(parts "$D2")
"$TMP/c" "$D2" "$WS" 536870912 268435456 209715200 200000000 1 >/dev/null 2>&1
A2=$(parts "$D2" 2>/dev/null)
[ "$A2" = "$B2" ] && ok "the machine still has exactly its old partitions" \
                  || bad "the machine still has exactly its old partitions" \
                         "before: $(printf '%s' "$B2" | tr '\n' ' ')" \
                         "after:  $(printf '%s' "$A2" | tr '\n' ' ')"

# WHAT A CORRECT INTERRUPTED DISK LOOKS LIKE, which is not "no
# problems found". The primary array has been rewritten and the
# primary header has not, so the primary CRC fails BY DESIGN and every
# reader falls back to the backup -- which is still the old table and
# is intact. That is the property: the machine has its old partitions
# and a valid table to read them from.
V=$(sgdisk -v "$D2" 2>&1)
if printf '%s' "$V" | grep -q "Backup partition table: OK"; then
    ok "...with an intact backup table for a reader to fall back to"
else
    bad "...with an intact backup table for a reader to fall back to" \
        "$(printf '%s' "$V" | head -4)"
fi
if printf '%s' "$V" | grep -q "Main partition table: ERROR"; then
    ok "...and the half-written primary is REJECTED, not believed"
else
    bad "...and the half-written primary is REJECTED, not believed" \
        "a reader would have used a table we never committed"
fi

# And on the disk that DID commit, Windows is smaller than it was.
WOLD=$(printf '%s\n' "$BEFORE" | sed -n '3p' | cut -d: -f3)
WNEW=$(printf '%s\n' "$AFTER"  | sed -n '3p' | cut -d: -f3)
if [ -n "$WOLD" ] && [ -n "$WNEW" ] && [ "$WNEW" -lt "$WOLD" ]; then
    ok "and on the committed disk Windows really is smaller"
else
    bad "and on the committed disk Windows really is smaller" \
        "was $WOLD, now $WNEW"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "This is the sector that decides where a stranger's partitions are."
    exit 1
fi
echo "$checked checks: the commit is one sector, and until it lands the"
echo "disk reads exactly as it did before anything started."
exit 0
