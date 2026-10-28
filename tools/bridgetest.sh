#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  bridgetest — do the two halves of the installer actually agree?
#
#  AurBridge writes bytes on Windows; the staging environment reads
#  them after the restart and refuses the install if it does not
#  recognise them. The two are separate programs in separate trees
#  built by separate toolchains, and nothing in either build would
#  notice a field renamed on one side. This is what notices.
#
#  It does not simulate anything. It links the REAL writer
#  (src/aurbridge/format.c) and the REAL reader
#  (src/aurstage/journal.c) into one program, hands what the first
#  produces to the second, and asks it to agree -- and it checks the
#  formats the KERNEL reads with the kernel's own userland tools
#  rather than with a second opinion of our own.
#
#    sh tools/bridgetest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-56s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-56s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

T=$(mktemp -d "${TMPDIR:-/tmp}/bridgetest.XXXXXX")
trap 'rm -rf "$T"' EXIT

echo
echo "Do the Windows half and the Linux half agree about the bytes?"
echo

# ── every pure function, against its own vectors ────────────────────
cat > "$T/v.c" <<'EOC'
#include <stdio.h>
int fmt_selftest(void);
int main(void){ return fmt_selftest() ? 1 : 0; }
EOC
if cc -std=gnu11 -O2 -Isrc/aurbridge -o "$T/v" "$T/v.c" src/aurbridge/format.c \
     2>"$T/cc.log" && "$T/v" >"$T/v.log" 2>&1; then
    ok "every published vector in format.c checks out"
else
    bad "every published vector in format.c checks out" \
        "$(tail -5 "$T/v.log" "$T/cc.log" 2>/dev/null)"
fi

# It has to build for Windows too, which is the only place it runs.
if x86_64-w64-mingw32-gcc -std=gnu11 -Wall -Wextra -Werror \
     -c src/aurbridge/format.c -Isrc/aurbridge -o "$T/fw.o" 2>"$T/mw.log"; then
    ok "and it builds clean for Windows"
else
    bad "and it builds clean for Windows" "$(head -5 "$T/mw.log")"
fi

# ── the journal: written by one half, read by the other ─────────────
cat > "$T/j.c" <<'EOC'
/* Links the real writer and the real reader. Nothing is reimplemented
 * here; if these two disagree, the install refuses every machine. */
#include <stdio.h>
#include <string.h>
#include "format.h"
#include "journal.h"

/* journal_check() calls this, and journal_check() is not what is under
 * test here -- journal_read() is. Linking src/aurstage/disks.c in for
 * it would drag /sys and /dev into a test that is about two structs
 * agreeing, so it is stubbed and never called. */
int stage_gpt_sha256(const stage_disk *d, char *hex, size_t n)
{ (void)d; if (n) hex[0] = 0; return -1; }

int main(int argc, char **argv)
{
    fmt_journal j;
    memset(&j, 0, sizeof j);
    snprintf(j.disk_serial, sizeof j.disk_serial, "S/N 123-456 XYZ");
    snprintf(j.disk_model,  sizeof j.disk_model,  "Samsung SSD 860 EVO");
    j.disk_bytes = 500107862016ull;
    j.logical_sector = 4096;
    snprintf(j.win_part, sizeof j.win_part, "3");
    j.win_start_lba = 1050624;
    j.win_sectors   = 931842048;
    j.win_ntfs_serial = 0x9C3F1A2B4D5E6F70ull;
    snprintf(j.gpt_sha256, sizeof j.gpt_sha256,
             "%s", argc > 2 ? argv[2]
             : "0123456789abcdef0123456789abcdef"
               "0123456789abcdef0123456789abcdef");
    snprintf(j.stage, sizeof j.stage, "armed");
    snprintf(j.boot_from, sizeof j.boot_from, "esp");
    snprintf(j.profile, sizeof j.profile, "school-kiosk");
    j.run_id = 7788990011ull;
    j.written_unix = 1758000000ull;

    char buf[2048];
    size_t n = fmt_journal_json(&j, buf, sizeof buf);
    if (!n) { puts("WRITE-FAILED"); return 1; }
    FILE *f = fopen(argv[1], "wb");
    if (!f) { puts("OPEN-FAILED"); return 1; }
    fwrite(buf, 1, n, f);
    fclose(f);

    journal r;
    if (!journal_read(argv[1], &r)) { puts("PARSE-FAILED"); return 1; }
    int bad = 0;
#define EQ(a, b, what) do { if ((a) != (b)) { printf("DIFF %s\n", what); bad++; } } while (0)
#define SEQ(a, b, what) do { if (strcmp((a), (b))) { printf("DIFF %s (%s vs %s)\n", what, a, b); bad++; } } while (0)
    SEQ(r.disk_serial, j.disk_serial, "disk_serial");
    SEQ(r.disk_model,  j.disk_model,  "disk_model");
    EQ(r.disk_bytes,   j.disk_bytes,  "disk_bytes");
    EQ(r.logical_sector, j.logical_sector, "logical_sector");
    SEQ(r.win_part,    j.win_part,    "win_part");
    EQ(r.win_start_lba, j.win_start_lba, "win_start_lba");
    EQ(r.win_sectors,  j.win_sectors,  "win_sectors");
    EQ(r.win_ntfs_serial, j.win_ntfs_serial, "win_ntfs_serial");
    SEQ(r.gpt_sha256,  j.gpt_sha256,  "gpt_sha256");
    SEQ(r.stage,       j.stage,       "stage");
    SEQ(r.boot_from,   j.boot_from,   "boot_from");
    SEQ(r.profile,     j.profile,     "profile");
    EQ(r.run_id,       j.run_id,      "run_id");
    EQ(r.written_unix, j.written_unix, "written_unix");
    if (r.corrupt) { puts("DIFF corrupt-flag-set"); bad++; }
    puts(bad ? "MISMATCH" : "AGREE");
    return bad ? 1 : 0;
}
EOC
if cc -std=gnu11 -O2 -Isrc/aurbridge -Isrc/aurstage -o "$T/j" "$T/j.c" \
     src/aurbridge/format.c src/aurstage/journal.c src/aurstage/sha256.c \
     2>"$T/jc.log" && "$T/j" "$T/journal.json" > "$T/j.out" 2>&1 \
     && grep -q AGREE "$T/j.out"; then
    ok "the journal one half writes is the journal the other half reads"
else
    bad "the journal one half writes is the journal the other half reads" \
        "$(cat "$T/j.out" 2>/dev/null | head -6)" "$(head -4 "$T/jc.log")"
fi

# EVERY FIELD, NOT JUST THE ONES THAT PARSE. A writer that emits a
# field the reader ignores passes the test above while the install
# refuses on the machine.
#
# AND THE CHECK RUNS EITHER WAY. It used to sit inside
# `if [ -f "$T/journal.json" ]`, so when the step above failed to build
# or run, this printed neither ok nor bad and was not even counted --
# the suite reported one fewer check than the last run and exited 0.
if [ -f "$T/journal.json" ]; then
    miss=""
    for k in disk_serial disk_model disk_bytes logical_sector win_part \
             win_start_lba win_sectors win_ntfs_serial gpt_sha256 stage \
             boot_from profile run_id written_unix; do
        grep -q "\"$k\"" "$T/journal.json" || miss="$miss $k"
    done
    [ -z "$miss" ] && ok "and it carries every field the reader looks for" \
                   || bad "and it carries every field the reader looks for" \
                          "missing:$miss"
else
    bad "and it carries every field the reader looks for" \
        "no journal was written at all"
fi

# ── the fast path's journal, and the plan both halves must agree on ──
#
# image_on=gap: Windows has shrunk itself and AurBridge has written the
# image into the gap. The journal then carries the PLAN -- three extents
# -- and the hashes of what was written, and the staging environment
# refuses unless the plan it computes from the table it finds is the
# same one. So this lays out a post-shrink disk with sgdisk, plans it the
# way the Windows half does (gpt_read_with + plan_compute_prepared, the
# shared files), writes the journal with the real writer, reads it with
# the real reader, and plans it again the way the staging half does
# (gpt_read on a file descriptor). Every field, and the plan, must match.
python3 - "$T/fast.img" <<'EOPY'
import subprocess, sys
open(sys.argv[1],'wb').truncate(64*1024**3)
for a in (["--zap-all"],
          ["-n","1:2048:+100M","-t","1:ef00","-c","1:EFI"],
          ["-n","2:0:+16M","-t","2:0c01","-c","2:MSR"],
          ["-n","3:0:+20G","-t","3:0700","-c","3:Windows"],
          ["-n","4:-1G:0","-t","4:2700","-c","4:WinRE"]):
    subprocess.run(["sgdisk", *a, sys.argv[1]], capture_output=True, check=True)
EOPY
cat > "$T/f.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "format.h"
#include "journal.h"
#include "gpt.h"
#include "plan.h"
int stage_gpt_sha256(const stage_disk *d, char *hex, size_t n)
{ (void)d; if (n) hex[0] = 0; return -1; }
static FILE *g;
static int rd(void *ud, uint64_t off, void *buf, size_t n)
{ (void)ud; if (fseeko(g, (off_t)off, SEEK_SET)) return -1;
  return fread(buf, 1, n, g) == n ? 0 : -1; }
int main(int argc, char **argv)
{
    (void)argc;
    const uint64_t disk = 64ull << 30, root = 5333057536ull, boot = 512ull << 20,
                   rsc = 300ull << 20, minr = 24000000000ull;
    char why[300];
    static gpt_table tw, ts;
    g = fopen(argv[1], "rb");
    if (!g || gpt_read_with(rd, NULL, 512, disk, &tw) != 0) { puts("NO-TABLE"); return 1; }
    int wi = 2;                                   /* partition 3 */
    stage_layout W, S;
    if (plan_compute_prepared(&tw, wi, root, boot, rsc, minr, &W, why, sizeof why) != 0)
        { printf("WINDOWS-PLAN-REFUSED %s\n", why); return 1; }
    fmt_journal j;
    memset(&j, 0, sizeof j);
    snprintf(j.disk_serial, sizeof j.disk_serial, "S4EWNX0N123456");
    j.disk_bytes = disk; j.logical_sector = 512;
    snprintf(j.win_part, sizeof j.win_part, "3");
    j.win_start_lba = tw.ent[wi].first;
    j.win_sectors = tw.ent[wi].last - tw.ent[wi].first + 1;
    j.win_orig_sectors = j.win_sectors + (40ull << 21);
    snprintf(j.gpt_sha256, sizeof j.gpt_sha256, "%064d", 0);
    snprintf(j.stage, sizeof j.stage, "armed");
    snprintf(j.boot_from, sizeof j.boot_from, "esp");
    snprintf(j.profile, sizeof j.profile, "desktop");
    snprintf(j.image_on, sizeof j.image_on, "gap");
    j.run_id = 42; j.written_unix = 1760000000ull;
    j.gap_root_first = W.root_first; j.gap_root_last = W.root_last;
    j.gap_rsc_first = W.rsc_first;   j.gap_rsc_last = W.rsc_last;
    j.gap_rec_first = W.rec_first;   j.gap_rec_last = W.rec_last;
    j.root_len = root; j.boot_len = boot; j.rsc_bytes = rsc; j.min_root_bytes = minr;
    snprintf(j.root_sha256, sizeof j.root_sha256, "%s",
             "b36100653b1b3dd339c6209cdd60ab169706717f512f2bb5dee0273dd404da41");
    snprintf(j.boot_sha256, sizeof j.boot_sha256, "%s",
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    j.bitlocker = 1; j.wifi = 1;
    char buf[4096];
    size_t n = fmt_journal_json(&j, buf, sizeof buf);
    if (!n) { puts("WRITE-FAILED"); return 1; }
    FILE *f = fopen(argv[2], "wb"); fwrite(buf, 1, n, f); fclose(f);
    journal r;
    if (!journal_read(argv[2], &r)) { puts("PARSE-FAILED"); return 1; }
    int bad = 0;
#define EQ(a, b, what) do { if ((uint64_t)(a) != (uint64_t)(b)) { printf("DIFF %s\n", what); bad++; } } while (0)
    EQ(r.gap_root_first, j.gap_root_first, "gap_root_first");
    EQ(r.gap_root_last,  j.gap_root_last,  "gap_root_last");
    EQ(r.gap_rsc_first,  j.gap_rsc_first,  "gap_rsc_first");
    EQ(r.gap_rsc_last,   j.gap_rsc_last,   "gap_rsc_last");
    EQ(r.gap_rec_first,  j.gap_rec_first,  "gap_rec_first");
    EQ(r.gap_rec_last,   j.gap_rec_last,   "gap_rec_last");
    EQ(r.root_len, j.root_len, "root_len");
    EQ(r.boot_len, j.boot_len, "boot_len");
    EQ(r.rsc_bytes, j.rsc_bytes, "rsc_bytes");
    EQ(r.min_root_bytes, j.min_root_bytes, "min_root_bytes");
    EQ(r.win_orig_sectors, j.win_orig_sectors, "win_orig_sectors");
    EQ(r.bitlocker, 1, "bitlocker");
    EQ(r.wifi, 1, "wifi");
    if (strcmp(r.root_sha256, j.root_sha256)) { puts("DIFF root_sha256"); bad++; }
    if (strcmp(r.boot_sha256, j.boot_sha256)) { puts("DIFF boot_sha256"); bad++; }
    if (strcmp(r.image_on, "gap")) { puts("DIFF image_on"); bad++; }
    /* The staging half: the same disk through a file descriptor, planned
     * from the journal's own numbers. */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0 || gpt_read(fd, 512, disk, &ts) != 0) { puts("STAGE-NO-TABLE"); return 1; }
    int si = gpt_find_start(&ts, r.win_start_lba);
    if (si != wi) { puts("DIFF windows-entry"); bad++; }
    if (plan_compute_prepared(&ts, si, r.root_len, r.boot_len, r.rsc_bytes,
                              r.min_root_bytes, &S, why, sizeof why) != 0)
        { printf("STAGE-PLAN-REFUSED %s\n", why); return 1; }
    EQ(S.root_first, r.gap_root_first, "plan root_first");
    EQ(S.root_last,  r.gap_root_last,  "plan root_last");
    EQ(S.rsc_first,  r.gap_rsc_first,  "plan rsc_first");
    EQ(S.rsc_last,   r.gap_rsc_last,   "plan rsc_last");
    EQ(S.rec_first,  r.gap_rec_first,  "plan rec_first");
    EQ(S.rec_last,   r.gap_rec_last,   "plan rec_last");
    EQ(S.win_last_new, ts.ent[si].last, "the Windows entry is left as it is");
    printf("root %llu..%llu saved %llu..%llu boot %llu..%llu\n",
           (unsigned long long)S.root_first, (unsigned long long)S.root_last,
           (unsigned long long)S.rsc_first, (unsigned long long)S.rsc_last,
           (unsigned long long)S.rec_first, (unsigned long long)S.rec_last);
    puts(bad ? "MISMATCH" : "AGREE");
    return bad ? 1 : 0;
}
EOC
if cc -std=gnu11 -O2 -Wall -Isrc/aurbridge -Isrc/aurstage -o "$T/f" "$T/f.c" \
     src/aurbridge/format.c src/aurstage/journal.c src/aurstage/sha256.c \
     src/aurstage/gpt.c src/aurstage/plan.c 2>"$T/fc.log" \
   && "$T/f" "$T/fast.img" "$T/fast.json" > "$T/f.out" 2>&1 && grep -q AGREE "$T/f.out"; then
    ok "fast path: both halves plan the gap the same, field for field"
else
    bad "fast path: both halves plan the gap the same, field for field" \
        "$(head -8 "$T/f.out" 2>/dev/null)" "$(head -4 "$T/fc.log")"
fi
# A fast-path journal with any one of its fields missing is refused, not
# read as a journal with a zero in it.
cat > "$T/r.c" <<'EOC'
#include <stdio.h>
#include "journal.h"
int stage_gpt_sha256(const stage_disk *d, char *hex, size_t n)
{ (void)d; if (n) hex[0] = 0; return -1; }
int main(int c, char **v) { journal j; (void)c;
  int ok = journal_read(v[1], &j); printf("%s\n", ok ? "READ" : j.corrupt ? "REFUSED" : "NONE");
  return 0; }
EOC
cc -std=gnu11 -O2 -Isrc/aurstage -o "$T/r" "$T/r.c" \
    src/aurstage/journal.c src/aurstage/sha256.c 2>>"$T/fc.log"
if [ -f "$T/fast.json" ] && [ -x "$T/r" ]; then
    miss=0
    for k in gap_root_first gap_root_last gap_rsc_first gap_rsc_last \
             gap_rec_first gap_rec_last root_len root_sha256 boot_len \
             boot_sha256 rsc_bytes min_root_bytes win_orig_sectors; do
        sed "s/\"$k\":\"[0-9a-f]*\",//; s/\"$k\":[0-9]*,//" "$T/fast.json" > "$T/cut.json"
        if grep -q "\"$k\"" "$T/cut.json"; then miss=$((miss+1)); echo "    (could not cut $k)"; continue; fi
        [ "$("$T/r" "$T/cut.json")" = REFUSED ] || { miss=$((miss+1)); echo "    (without $k it was not refused)"; }
    done
    [ "$("$T/r" "$T/fast.json")" = READ ] || { miss=$((miss+1)); echo "    (the whole one was not read)"; }
    [ "$miss" = 0 ] && ok "fast path: a journal missing any one of its fields is refused" \
                    || bad "fast path: a journal missing any one of its fields is refused"
else
    bad "fast path: a journal missing any one of its fields is refused" "no journal or no reader"
fi
# And every other journal is byte for byte what it was: no gap_ fields.
if [ -f "$T/journal.json" ] && ! grep -q '"gap_' "$T/journal.json"; then
    ok "a stick or no-stick journal carries none of the fast path's fields"
else
    bad "a stick or no-stick journal carries none of the fast path's fields"
fi
rm -f "$T/fast.img"

# ── the hash of a partition table, against a third opinion ──────────
#
# aurstage.h defines this precisely because two implementations have to
# agree on it. The third one here is eleven lines of python that follow
# the words in that header and share no code with either.
python3 - "$T/disk.img" <<'EOPY'
import struct, subprocess, sys, os
open(sys.argv[1],'wb').truncate(3*1024*1024*1024)
for a in (["--zap-all"],
          ["-n","1:2048:206847","-t","1:ef00","-c","1:EFI"],
          ["-n","2:206848:2303999","-t","2:0700","-c","2:Windows"],
          ["-n","3:5500000:6291422","-t","3:2700","-c","3:WinRE"]):
    subprocess.run(["sgdisk", *a, sys.argv[1]], capture_output=True)
EOPY
cat > "$T/h.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include "format.h"
static FILE *g;
static int rd(void *ud, uint64_t off, void *buf, size_t n)
{ (void)ud; if (fseek(g, (long)off, SEEK_SET)) return -1;
  return fread(buf, 1, n, g) == n ? 0 : -1; }
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    g = fopen(argv[1], "rb");
    if (!g) return 2;
    char hex[65];
    if (fmt_gpt_sha256(rd, NULL, 512, hex, sizeof hex) != 0)
        { puts("CANNOT-READ"); return 1; }
    puts(hex);
    return 0;
}
EOC
cc -std=gnu11 -O2 -Isrc/aurbridge -o "$T/h" "$T/h.c" src/aurbridge/format.c \
    2>>"$T/cc.log"
A=$("$T/h" "$T/disk.img" 2>/dev/null)
B=$(python3 - "$T/disk.img" <<'EOPY'
import sys, struct, hashlib
ss=512; f=open(sys.argv[1],'rb'); f.seek(ss); h=f.read(ss)
hs=struct.unpack_from('<I',h,12)[0]; pl=struct.unpack_from('<Q',h,72)[0]
n=struct.unpack_from('<I',h,80)[0]; e=struct.unpack_from('<I',h,84)[0]
f.seek(pl*ss); print(hashlib.sha256(h[:hs]+f.read(n*e)).hexdigest())
EOPY
)
if [ -n "$A" ] && [ "$A" = "$B" ]; then
    ok "the partition-table hash matches a third implementation"
else
    bad "the partition-table hash matches a third implementation" \
        "aurbridge: ${A:-nothing}" "python:    ${B:-nothing}"
fi

# ── the formats the KERNEL reads, checked with the kernel's tools ───
cat > "$T/c.c" <<'EOC'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "format.h"
int main(int argc, char **argv)
{
    static unsigned char cp[8192], gz[16384];
    const char *body = "{\"stage\":\"armed\",\"run_id\":1}\n";
    size_t k = fmt_cpio_one("aurbridge/journal.json", body, strlen(body),
                            cp, sizeof cp);
    if (!k) return 1;
    size_t g = fmt_gzip_store(cp, k, gz, sizeof gz);
    if (!g) return 1;
    FILE *f = fopen(argv[1], "wb"); if (!f) return 1;
    fwrite(gz, 1, g, f); fclose(f);
    f = fopen(argv[2], "wb"); if (!f) return 1;
    fwrite(cp, 1, k, f); fclose(f);
    return 0;
}
EOC
if cc -std=gnu11 -O2 -Isrc/aurbridge -o "$T/c" "$T/c.c" src/aurbridge/format.c \
     2>>"$T/cc.log" && "$T/c" "$T/j.cpio.gz" "$T/j.cpio"; then
    if gzip -t "$T/j.cpio.gz" 2>/dev/null; then
        ok "gzip itself accepts the container AurBridge writes"
    else
        bad "gzip itself accepts the container AurBridge writes"
    fi
    if gzip -dc "$T/j.cpio.gz" 2>/dev/null | cmp -s - "$T/j.cpio"; then
        ok "and what comes out of it is byte-for-byte what went in"
    else
        bad "and what comes out of it is byte-for-byte what went in"
    fi
    if cpio -t < "$T/j.cpio" 2>/dev/null | grep -q 'aurbridge/journal.json'; then
        ok "cpio itself finds the journal in the archive"
    else
        bad "cpio itself finds the journal in the archive" \
            "$(cpio -t < "$T/j.cpio" 2>&1 | head -3)"
    fi
    ( cd "$T" && mkdir -p x && cd x && cpio -id --quiet < ../j.cpio 2>/dev/null )
    if [ -f "$T/x/aurbridge/journal.json" ] &&
       grep -q '"stage":"armed"' "$T/x/aurbridge/journal.json"; then
        ok "and unpacking it gives back the file that went in"
    else
        bad "and unpacking it gives back the file that went in"
    fi
else
    bad "the cpio and gzip could be built at all" "$(head -5 "$T/cc.log")"
fi

# ── the stick's table, read back by sgdisk ──────────────────────────
cat > "$T/g.c" <<'EOC'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "format.h"
int main(int argc, char **argv)
{
    uint64_t bytes = strtoull(argv[2], NULL, 10);
    uint32_t sector = 512;
    static unsigned char head[2*4096 + 16384], tail[16384 + 4096];
    unsigned char guid[16];
    for (int i = 0; i < 16; i++) guid[i] = (unsigned char)(i * 11 + 3);
    uint64_t lu = fmt_gpt_last_usable(bytes, sector);
    fmt_part p[3] = {
        { FMT_GUID_IMAGE,  "AUROS-IMAGE",  2048,   821247 },
        { FMT_GUID_RECORD, "AUROS-RECORD", 821248, 829439 },
        { FMT_GUID_SAVED,  "AUROS-SAVED",  829440, lu     },
    };
    char why[200];
    if (fmt_gpt_build(bytes, sector, guid, p, 3, head, tail, why, sizeof why))
        { puts(why); return 1; }
    FILE *f = fopen(argv[1], "r+b"); if (!f) return 1;
    fseek(f, 0, SEEK_SET);
    fwrite(head, 1, fmt_gpt_head_bytes(sector), f);
    fseek(f, (long)(fmt_gpt_backup_lba(bytes, sector) * sector), SEEK_SET);
    fwrite(tail, 1, fmt_gpt_tail_bytes(sector), f);
    fclose(f);
    return 0;
}
EOC
truncate -s 768M "$T/stick.img"
if cc -std=gnu11 -O2 -Isrc/aurbridge -o "$T/g" "$T/g.c" src/aurbridge/format.c \
     2>>"$T/cc.log" && "$T/g" "$T/stick.img" $((768*1024*1024)); then
    if sgdisk -v "$T/stick.img" 2>&1 | grep -q "No problems found"; then
        ok "sgdisk finds no fault with the table AurBridge writes"
    else
        bad "sgdisk finds no fault with the table AurBridge writes" \
            "$(sgdisk -v "$T/stick.img" 2>&1 | head -4)"
    fi
    N=$(sgdisk -p "$T/stick.img" 2>/dev/null | sed -n '/^Number/,$p' |
        tail -n +2 | grep -c .)
    [ "$N" = "3" ] && ok "and it has the three partitions it should" \
                   || bad "and it has the three partitions it should" "it has $N"
    G=$(sgdisk -i 1 "$T/stick.img" 2>/dev/null |
        sed -n 's/^Partition GUID code: \([0-9A-F-]*\).*/\1/p')
    [ "$G" = "A12A5E9C-AB6E-4E4D-9F35-5B1C0A2E7D41" ] \
        && ok "and the image partition carries the type the installer looks for" \
        || bad "and the image partition carries the type the installer looks for" \
               "got ${G:-nothing}"
    NM=$(sgdisk -i 3 "$T/stick.img" 2>/dev/null |
         sed -n 's/^Partition name: .\(.*\).$/\1/p')
    [ "$NM" = "AUROS-SAVED" ] && ok "and the names survive the UTF-16" \
                             || bad "and the names survive the UTF-16" "got ${NM:-nothing}"
else
    bad "the stick's table could be built at all" "$(head -5 "$T/cc.log")"
fi

echo
if [ "$fail" -gt 0 ]; then
    echo "$fail of $checked wrong."
    echo "Each one is the two halves of the installer disagreeing."
    exit 1
fi
echo "$checked checks: what AurBridge writes is what the staging"
echo "environment, gzip, cpio and sgdisk all read back."
exit 0
