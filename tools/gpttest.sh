#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  gpttest — the partition table, read and written back
#
#  src/aurstage/gpt.c produces the single most dangerous write in this
#  product: one sector, LBA 1, after which the machine's idea of where
#  its partitions are has changed. A wrong CRC there does not produce
#  an error message. It produces firmware that quietly falls back to
#  the backup table, or refuses the disk, on a machine that is now
#  three thousand miles away.
#
#  THE TEST THAT MATTERS is the round trip: read a real GPT made by
#  somebody else's tool, serialise it back, and require the bytes to be
#  identical. Nothing else proves the writer and the reader agree with
#  the world rather than merely with each other.
#
#    sh tools/gpttest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

command -v sgdisk >/dev/null 2>&1 || { echo "need sgdisk"; exit 2; }
TMP=$(mktemp -d "${TMPDIR:-/tmp}/gpttest.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

echo
echo "Does the partition table survive being read and written back?"
echo

cat > "$TMP/g.c" <<'EOC'
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "gpt.h"
#include "plan.h"

static uint64_t disk_bytes(int fd)
{ struct stat st; return fstat(fd, &st) == 0 ? (uint64_t)st.st_size : 0; }

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "crc")) {
        printf("crc=%08X\n", gpt_crc32(argv[2], strlen(argv[2])));
        return 0;
    }
    if (argc < 3) { fputs("usage: g <cmd> <image> [...]\n", stderr); return 2; }
    const char *cmd = argv[1], *img = argv[2];
    uint32_t ss = argc > 3 ? (uint32_t)atoi(argv[3]) : 512;

    int fd = open(img, O_RDONLY);
    if (fd < 0) { perror(img); return 2; }
    gpt_table t;
    if (gpt_read(fd, ss, disk_bytes(fd), &t) != 0) {
        puts("read=failed"); return 1;
    }
    printf("read=ok\n");
    printf("from_backup=%d\n", t.from_backup);
    printf("header_size=%u\n", t.header_size);
    printf("n_entries=%u\n", t.n_entries);
    printf("entry_size=%u\n", t.entry_size);
    printf("first_usable=%llu\n", (unsigned long long)t.first_usable);
    printf("last_usable=%llu\n", (unsigned long long)t.last_usable);

    if (!strcmp(cmd, "list")) {
        for (uint32_t i = 0; i < t.n_entries; i++) {
            if (!gpt_used(&t.ent[i])) continue;
            char nm[40]; int k = 0;
            for (int c = 0; c < GPT_NAME_CH && t.ent[i].name[c]; c++)
                nm[k++] = (char)(t.ent[i].name[c] & 0x7F);
            nm[k] = 0;
            printf("p%u=%llu:%llu:%s\n", i + 1,
                   (unsigned long long)t.ent[i].first,
                   (unsigned long long)t.ent[i].last, nm);
        }
        close(fd); return 0;
    }

    if (!strcmp(cmd, "roundtrip")) {
        /* THE ONE THAT MATTERS. Serialise what we read and compare
         * against the bytes somebody else's tool actually wrote. */
        size_t ab = gpt_array_bytes(&t);
        uint8_t *hdr = malloc(t.sector), *arr = malloc(ab);
        uint8_t *dh  = malloc(t.sector), *da  = malloc(ab);
        gpt_serialize(&t, 1, hdr, arr);
        if (pread(fd, dh, t.sector, t.sector) != (ssize_t)t.sector ||
            pread(fd, da, ab, (off_t)(t.entry_lba * t.sector)) != (ssize_t)ab) {
            puts("roundtrip=unreadable");
            free(hdr); free(arr); free(dh); free(da);
            return 1;
        }
        /* The header block is one sector; only header_size bytes are
         * defined, the rest is padding the firmware wrote. */
        int hdr_same = memcmp(hdr, dh, t.header_size) == 0;
        int arr_same = memcmp(arr, da, ab) == 0;
        printf("roundtrip_header=%d\n", hdr_same);
        printf("roundtrip_array=%d\n", arr_same);
        if (!hdr_same)
            for (uint32_t i = 0; i < t.header_size; i++)
                if (hdr[i] != dh[i]) { printf("first_diff=%u\n", i); break; }
        free(hdr); free(arr); free(dh); free(da);
        close(fd); return (hdr_same && arr_same) ? 0 : 1;
    }

    if (!strcmp(cmd, "gap")) {
        uint64_t after = strtoull(argv[4], NULL, 10);
        printf("gap_end=%llu\n",
               (unsigned long long)gpt_gap_end(&t, after));
        close(fd); return 0;
    }

    if (!strcmp(cmd, "add")) {
        uint64_t f = strtoull(argv[4], NULL, 10), l = strtoull(argv[5], NULL, 10);
        char why[200] = {0}; uint8_t u[16];
        int r = gpt_add(&t, GPT_TYPE_LINUX_ROOT, "AUROS-ROOT", f, l, u,
                        why, sizeof why);
        printf("add=%d\n", r);
        printf("why=%s\n", why);
        close(fd); return r < 0 ? 1 : 0;
    }

    if (!strcmp(cmd, "grow")) {
        int idx = atoi(argv[4]);
        uint64_t nl = strtoull(argv[5], NULL, 10);
        printf("resize=%d\n", gpt_resize_entry(&t, idx, nl));
        close(fd); return 0;
    }

    if (!strcmp(cmd, "plan")) {
        /* win_new_bytes root_src rec_bytes min_root */
        int wi = gpt_find_start(&t, strtoull(argv[4], NULL, 10));
        stage_layout L; char why[240] = {0};
        int r = plan_compute(&t, wi,
                             strtoull(argv[5], NULL, 10),
                             strtoull(argv[6], NULL, 10),
                             strtoull(argv[7], NULL, 10),
                             strtoull(argv[8], NULL, 10),
                             &L, why, sizeof why);
        printf("plan=%d\n", r);
        printf("why=%s\n", why);
        if (r == 0) {
            printf("win_last_new=%llu\n", (unsigned long long)L.win_last_new);
            printf("root_first=%llu\n", (unsigned long long)L.root_first);
            printf("root_last=%llu\n",  (unsigned long long)L.root_last);
            printf("rec_first=%llu\n",  (unsigned long long)L.rec_first);
            printf("rec_last=%llu\n",   (unsigned long long)L.rec_last);
            printf("root_gib=%.2f\n",
                   (double)((L.root_last - L.root_first + 1) * (uint64_t)L.sector)
                   / (1024.0*1024.0*1024.0));
        }
        close(fd); return r ? 1 : 0;
    }

    if (!strcmp(cmd, "fill")) {
        /* Use every free slot, then ask for one more. */
        char why[200] = {0}; uint8_t u[16];
        uint64_t at = t.first_usable;
        int made = 0;
        while (gpt_add(&t, GPT_TYPE_LINUX, "X", at, at, u, why, sizeof why) >= 0) {
            made++; at++;
            if (at > t.last_usable) break;
        }
        printf("made=%d\n", made);
        printf("why=%s\n", why);
        close(fd); return 0;
    }
    close(fd);
    fputs("unknown command\n", stderr);
    return 2;
}
EOC
gcc -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I src/aurstage -o "$TMP/g" \
    "$TMP/g.c" src/aurstage/gpt.c src/aurstage/plan.c src/aurstage/boot.c \
    src/aurstage/disks.c src/aurstage/sha256.c src/aurstage/ntfs.c \
    src/aurstage/fde.c || { echo "  did not build"; exit 2; }

get() { "$TMP/g" "$@" 2>/dev/null | sed -n "s/^$F=//p"; }

echo "  the checksum"
# The IEEE CRC-32 of "123456789" is the standard check value for this
# polynomial. If this is wrong every table we write is refused by
# firmware, silently.
F=crc; got=$(get crc 123456789)
[ "$got" = "CBF43926" ] && ok "CRC-32 of \"123456789\" is CBF43926" \
                        || bad "CRC-32 of \"123456789\" is CBF43926" "got $got"

echo
echo "  a table somebody else's tool made"
D="$TMP/oem.img"
# The layout docs/AURBRIDGE.md calls typical, and the one the first
# stage C design got wrong: the recovery partition is at the END, so
# the space a shrink makes is a GAP in the middle.
truncate -s 8G "$D"
sgdisk --zap-all "$D" >/dev/null 2>&1
sgdisk -n 1:2048:+260M   -t 1:ef00 -c 1:"EFI"      "$D" >/dev/null 2>&1
sgdisk -n 2:0:+128M      -t 2:0c01 -c 2:"MSR"      "$D" >/dev/null 2>&1
sgdisk -n 3:0:+4G        -t 3:0700 -c 3:"Windows"  "$D" >/dev/null 2>&1
# WinRE at the very end, with two gibibytes of unallocated space between
# it and Windows -- which is what the disk looks like AFTER the shrink,
# and the shape the first stage C design could not express at all.
sgdisk -n 4:15000000:0   -t 4:2700 -c 4:"WinRE"    "$D" >/dev/null 2>&1

F=read; got=$(get list "$D")
[ "$got" = "ok" ] && ok "an OEM-shaped GPT reads" || bad "an OEM-shaped GPT reads" "$got"
F=n_entries; ok "...with $(get list "$D") entry slots"

F=roundtrip_header; got=$(get roundtrip "$D")
[ "$got" = "1" ] && ok "the header we write back is byte-identical" \
                 || bad "the header we write back is byte-identical" \
                        "first differing byte: $(F=first_diff; get roundtrip "$D")"
F=roundtrip_array; got=$(get roundtrip "$D")
[ "$got" = "1" ] && ok "the entry array we write back is byte-identical" \
                 || bad "the entry array we write back is byte-identical" "got $got"

echo
echo "  the gap, which is the whole point"
# Windows ends at the start of WinRE. gpt_gap_end asked for the next
# partition at or after that must answer WinRE's start -- NOT
# last_usable, which is what computing from the end of the disk does.
WINRE=$(sgdisk -i 4 "$D" 2>/dev/null | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
WINEND=$(sgdisk -i 3 "$D" 2>/dev/null | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
F=gap_end; got=$(get gap "$D" 512 $((WINEND + 1)))
if [ "$got" = "$WINRE" ]; then
    ok "the gap after Windows ends where WinRE begins"
else
    bad "the gap after Windows ends where WinRE begins" \
        "got $got, WinRE starts at $WINRE" \
        "computing from the end of the disk is the bug this catches"
fi
F=last_usable; LU=$(get gap "$D" 512 $((WINEND + 1)))
[ "$got" != "$LU" ] && ok "...and that is NOT the end of the disk" \
                    || bad "...and that is NOT the end of the disk" "both are $got"

echo
echo "  and what it refuses"
F=add; got=$(get add "$D" 512 $((WINEND + 1)) $((WINRE + 1000)))
[ "$got" = "-1" ] && ok "a partition overlapping WinRE is refused" \
                  || bad "a partition overlapping WinRE is refused" "add=$got"
F=why; case "$(get add "$D" 512 $((WINEND + 1)) $((WINRE + 1000)))" in
  *"on top of partition 4"*) ok "...and it names which one" ;;
  *) bad "...and it names which one" "it said: $(get add "$D" 512 $((WINEND+1)) $((WINRE+1000)))" ;;
esac

F=add; got=$(get add "$D" 512 $((WINEND + 1)) $((WINRE - 1)))
[ "$got" = "4" ] && ok "a partition inside the gap is accepted" \
                 || bad "a partition inside the gap is accepted" "add=$got"

F=resize; got=$(get grow "$D" 512 2 99999999)
[ "$got" = "-1" ] && ok "growing an existing entry is refused" \
                  || bad "growing an existing entry is refused" "resize=$got"

F=made; got=$(get fill "$D" 512)
[ -n "$got" ] && [ "$got" -gt 100 ] && ok "a full table refuses rather than growing" \
                                    || bad "a full table refuses rather than growing" "made=$got"
F=why; case "$(get fill "$D" 512)" in
  *"full"*"will not enlarge"*) ok "...and says so in words" ;;
  *) bad "...and says so in words" "it said: $(get fill "$D" 512)" ;;
esac

echo
echo "  when the primary is torn"
B="$TMP/torn.img"; cp --sparse=always "$D" "$B"
# A power cut during the commit leaves the primary half-written and the
# backup correct. A rescue tool that reads only the primary is blind in
# exactly the case it exists for.
dd if=/dev/urandom of="$B" bs=512 seek=1 count=1 conv=notrunc status=none
F=read; got=$(get list "$B")
[ "$got" = "ok" ] && ok "a torn primary falls back to the backup" \
                  || bad "a torn primary falls back to the backup" "$got"
F=from_backup; got=$(get list "$B")
[ "$got" = "1" ] && ok "...and says that is what it did" \
                 || bad "...and says that is what it did" "from_backup=$got"

echo
echo "  and the AurOS image itself"
if [ -f out/auros-desktop.img ]; then
    F=roundtrip_header; got=$(get roundtrip out/auros-desktop.img)
    [ "$got" = "1" ] && ok "the built image's GPT round-trips too" \
                     || bad "the built image's GPT round-trips too" "got $got"
    "$TMP/g" list out/auros-desktop.img 2>/dev/null | sed -n 's/^p/      p/p'
else
    echo "    (no built image here)"
fi

echo
echo "  and the layout that goes in the gap"
WINSTART=$(sgdisk -i 3 "$D" 2>/dev/null | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
# Shrink Windows to 1 GiB. The gap then runs from there to WinRE.
# 600 MiB recovery, a 3 GiB image, and a 1 GB floor so the fixture is
# not refused for being small -- the floor itself is tested separately.
F=plan; got=$(get plan "$D" 512 "$WINSTART" 1073741824 3221225472 629145600 1000000000)
[ "$got" = "0" ] && ok "a layout is found in the gap" \
                 || bad "a layout is found in the gap" "$(F=why; get plan "$D" 512 "$WINSTART" 1073741824 3221225472 629145600 1000000000)"

F=rec_last; got=$(get plan "$D" 512 "$WINSTART" 1073741824 3221225472 629145600 1000000000)
if [ "$got" = "$((WINRE - 1))" ]; then
    ok "the way back ends exactly where WinRE begins"
else
    bad "the way back ends exactly where WinRE begins" "rec_last=$got WinRE=$WINRE"
fi

F=root_first; RF=$(get plan "$D" 512 "$WINSTART" 1073741824 3221225472 629145600 1000000000)
[ -n "$RF" ] && [ "$((RF % 2048))" = "0" ] && ok "AurOS is aligned on a mebibyte" \
                                           || bad "AurOS is aligned on a mebibyte" "root_first=$RF"

# THE FLOOR. The same machine, asked for 24 GB, must be refused -- and
# the refusal must say how much it actually has, not just "no".
F=plan; got=$(get plan "$D" 512 "$WINSTART" 1073741824 3221225472 629145600 24000000000)
[ "$got" = "-1" ] && ok "the product floor refuses a machine that is too small" \
                  || bad "the product floor refuses a machine that is too small" "plan=$got"
F=why; case "$(get plan "$D" 512 "$WINSTART" 1073741824 3221225472 629145600 24000000000)" in
  *"needs 24 GB"*"can spare"*) ok "...and says how much it does have" ;;
  *) bad "...and says how much it does have" \
         "it said: $(get plan "$D" 512 "$WINSTART" 1073741824 3221225472 629145600 24000000000)" ;;
esac

# A shrink that does not shrink.
F=plan; got=$(get plan "$D" 512 "$WINSTART" 99999999999999 3221225472 629145600 1000000000)
[ "$got" = "-1" ] && ok "a Windows that would not get smaller is refused" \
                  || bad "a Windows that would not get smaller is refused" "plan=$got"

# An image bigger than the gap.
F=plan; got=$(get plan "$D" 512 "$WINSTART" 1073741824 999999999999 629145600 1000000000)
[ "$got" = "-1" ] && ok "an image too big for the gap is refused" \
                  || bad "an image too big for the gap is refused" "plan=$got"

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "This file writes the sector that decides where a stranger's"
    echo "partitions are."
    exit 1
fi
echo "$checked checks: it reads a real table, writes back the same bytes,"
echo "finds the gap in the middle, and refuses to touch what is not ours."
exit 0
