/* fde.c — see fde.h. Reads. Never writes. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "fde.h"

/* The names, as the products write them into their own boot code.
 *
 * Each is here because it is a string that identifies a product and
 * would be bizarre in the first megabyte of an ordinary Windows disk.
 * "ESET" and "Check Point" are NOT here for that reason: four letters
 * and two common words turn up by accident, and a false refusal on a
 * machine with nothing wrong with it is a user who cannot proceed and
 * cannot find out why. A net with a hole in it beats one that closes
 * on the wrong thing, because net 2 below is behind it either way. */
static const char *const NAMES[] = {
    "VeraCrypt", "TrueCrypt", "DcsBoot",
    "SafeGuard", "Sophos",
    "SecureDoc",
    "Endpoint Encryption", "McAfee",
    "PGPGUARD", "Symantec Drive Encryption",
    "BestCrypt", "DriveCrypt", "ZENworks",
    /* FAT SHORT NAMES. The ESP is scanned as raw sectors, so what is
     * actually there is a FAT directory: uppercased, space-padded,
     * eight characters and a tilde. "VeraCrypt" as a directory name is
     * stored "VERACR~1", which none of the entries above matches. The
     * match may still succeed on ASCII inside DcsBoot.efi's own PE
     * image -- but succeeding by accident is not the same as being
     * right, and the comparison below is case-insensitive now so
     * "DCSBOOT" matches too. */
    "VERACR~1", "TRUECR~1", "SAFEGU~1",
};

static int same_ci(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
    return a == b;
}

/* Search a buffer for any of them, ignoring case. Not strstr: the
 * buffer is raw sectors and contains zero bytes everywhere. */
static const char *scan(const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        size_t len = strlen(NAMES[i]);
        if (len > n) continue;
        for (size_t at = 0; at + len <= n; at++) {
            size_t k = 0;
            while (k < len && same_ci(b[at + k], (unsigned char)NAMES[i][k])) k++;
            if (k == len) return NAMES[i];
        }
    }
    return NULL;
}

int fde_looks_random(const unsigned char *s, size_t n)
{
    if (!s || n < 512) return 0;
    /* An encrypted sector is close to uniform over all 256 values. A
     * boot sector, a partition table, a superblock or a run of zeroes
     * is not: every filesystem on earth puts structure, text and
     * padding in its first sector.
     *
     * 200 distinct values out of 256 in 512 bytes is far above what
     * any real boot sector reaches and far below what random data
     * misses. For uniform random bytes the expected count is
     * 256*(1 - (255/256)^512) = 221.4 with a standard deviation of
     * 4.7, so 200 is four and a half deviations low and a genuinely
     * random sector falls under it about three times in a million.
     * (That is not even the dominant error: ciphertext whose last two
     * bytes happen to be 55 AA is let through by the test below, which
     * is one chance in 65536.) Being wrong here costs a sentence, not
     * a disk -- both branches refuse. */
    int seen[256] = {0}, distinct = 0;
    for (size_t i = 0; i < 512; i++)
        if (!seen[s[i]]++) distinct++;
    /* ...unless it ends in the boot signature, in which case
     * something meant it to be a boot sector and we are looking at
     * damage rather than ciphertext. */
    if (s[510] == 0x55 && s[511] == 0xAA) return 0;
    return distinct >= 200;
}

static fde_verdict sweep(const char *dev, uint64_t limit,
                         char *found, size_t n)
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FDE_UNSURE;

    /* THE LIMIT IS "UP TO", NOT "AT LEAST". An ESP is commonly 100 MB
     * and the sweep is bounded at 512 MB, so the read runs off the end
     * of the partition long before the limit -- and the first version
     * called that a read failure and refused the whole install with
     * "AurOS could not check whether this drive is encrypted by other
     * software". Every machine with a normal-sized ESP. The
     * end-to-end test found it on the first run; nothing smaller
     * would have, because every unit test pointed at a file big
     * enough. */
    uint64_t have = 0;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        have = (uint64_t)st.st_size;
    } else {
        unsigned long long b = 0;
        if (ioctl(fd, _IOR(0x12, 114, size_t), &b) == 0) have = b;  /* BLKGETSIZE64 */
    }
    if (have && limit > have) limit = have;

    /* Overlapping windows, so a name that straddles a read boundary
     * is still found. The overlap is one name longer than the longest
     * name, which is cheaper to state than to get wrong. */
    enum { WIN = 1u << 20, LAP = 64 };
    static unsigned char buf[WIN + LAP];
    uint64_t at = 0;
    fde_verdict v = FDE_NONE;
    while (at < limit) {
        size_t want = WIN + LAP;
        ssize_t k = pread(fd, buf, want, (off_t)at);
        if (k < 0) { v = FDE_UNSURE; break; }
        if (k == 0) break;                      /* the end of the device */
        const char *hit = scan(buf, (size_t)k);
        if (hit) {
            snprintf(found, n, "%s", hit);
            v = FDE_NAMED;
            break;
        }
        if ((size_t)k < want) {
            /* Short of the end of the device is fine -- the sweep
             * simply reached it. Short of the limit is a read that
             * failed, and the bytes past it were never examined. */
            if (at + (uint64_t)k >= limit) break;
            v = FDE_UNSURE;
            break;
        }
        at += WIN;
    }
    close(fd);
    return v;
}

fde_verdict fde_scan_disk(const char *disk_dev, const char *esp_dev,
                          char *found, size_t n)
{
    if (found && n) found[0] = 0;
    int unsure = 0;

    /* The front of the disk: where a BIOS-booting machine keeps its
     * boot code, and where every product that predates UEFI put its
     * name. One megabyte, because that is the gap before the first
     * partition on a modern disk and nothing legitimate lives there. */
    if (disk_dev) {
        fde_verdict v = sweep(disk_dev, 1u << 20, found, n);
        if (v == FDE_NAMED)  return FDE_NAMED;
        if (v == FDE_UNSURE) unsure = 1;
    }

    /* And the EFI partition, which is where a UEFI machine keeps it
     * instead -- VeraCrypt's is EFI/VeraCrypt/DcsBoot.efi. Read as
     * raw sectors rather than mounted: this environment mounts
     * nothing, and a FAT driver is a great deal of code to find one
     * string with. Bounded, because an ESP is a hundred megabytes on
     * a normal machine and we are not going to read a mislabelled
     * terabyte. */
    if (esp_dev) {
        fde_verdict v = sweep(esp_dev, 512u << 20, found, n);
        if (v == FDE_NAMED)  return FDE_NAMED;
        if (v == FDE_UNSURE) unsure = 1;
    }

    return unsure ? FDE_UNSURE : FDE_NONE;
}
