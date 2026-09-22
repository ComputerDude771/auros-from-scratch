/* ntfsread — print what src/aurstage/ntfs.c makes of a volume.
 *
 * A window onto the parser, and the thing tools/ntfstest.sh asserts
 * against. It is also the program to reach for when a machine in the
 * field comes back with a refusal nobody expected: point it at the
 * image and it says, in one screen, every field the decision was made
 * from.
 *
 *     cc -O2 -o ntfsread tools/ntfsread.c src/aurstage/ntfs.c \
 *        -I src/aurstage
 *     ./ntfsread /dev/sda2
 *
 * It opens read-only, like everything else in that directory.
 */
#include <stdio.h>
#include "ntfs.h"

static const char *tri(ntfs_tri t)
{ return t == NTFS_NO ? "no" : t == NTFS_YES ? "yes" : "unsure"; }

static const char *verdict(ntfs_verdict v)
{
    switch (v) {
    case NTFS_OK:         return "ok";
    case NTFS_NOT_NTFS:   return "not-ntfs";
    case NTFS_BITLOCKER:  return "bitlocker";
    case NTFS_DIRTY:      return "dirty";
    case NTFS_LOG_UNCLEAN:return "log-unclean";
    case NTFS_HIBERNATED: return "hibernated";
    case NTFS_UNREADABLE: return "unreadable";
    case NTFS_STRANGE:    return "strange";
    }
    return "?";
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: ntfsread <device-or-image>\n"); return 2; }
    ntfs_state st;
    ntfs_read_state(argv[1], &st);
    printf("verdict=%s\n",     verdict(st.verdict));
    printf("dirty=%s\n",       tri(st.dirty));
    printf("hibernated=%s\n",  tri(st.hibernated));
    printf("log_dirty=%s\n",   tri(st.log_dirty));
    printf("cluster=%u\n",     st.bytes_per_cluster);
    printf("sector=%u\n",      st.bytes_per_sector);
    printf("flags=%04X\n",     st.volume_flags);
    printf("serial=%016llX\n", (unsigned long long)st.serial);
    printf("why=%s\n",         st.why);
    if (st.remedy[0]) printf("remedy=%s\n", st.remedy);
    return st.verdict == NTFS_OK ? 0 : 1;
}
