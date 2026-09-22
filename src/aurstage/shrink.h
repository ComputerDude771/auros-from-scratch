/* shrink.h — asking how small the Windows volume can get, and whether
 * the disk underneath it can be trusted with the answer.
 *
 * NOTHING IN THIS FILE WRITES. Stage B is read-only by construction;
 * `ntfsresize` is invoked with --no-action and --info only, and the
 * surface test reads. The function that actually resizes belongs to
 * stage C and does not exist yet.
 *
 * --force IS UNREACHABLE BY CONSTRUCTION, not merely unpassed.
 * docs/AURBRIDGE.md requires that, and the reason is that it
 * authorises resizing a filesystem whose own metadata Windows has
 * declared untrustworthy -- which is R9 verbatim -- and it is one word
 * away at all times. The argument vectors here are fixed arrays in
 * this file; there is no parameter through which a caller could add it.
 */
#ifndef AUROS_SHRINK_H
#define AUROS_SHRINK_H

#include <stdint.h>

typedef struct {
    int      ok;                /* ntfsresize answered                */
    int      refused;           /* it refused, and why is in `why`    */
    uint64_t current_bytes;     /* the volume as it stands            */
    uint64_t smallest_bytes;    /* the REAL floor, from the tool      */
    char     why[240];
} shrink_plan;

/* Ask ntfsresize what the true achievable size is.
 *
 * Not an estimate from filesystem free space: R7 requires the real
 * number, and the two differ by a lot on a volume whose data is
 * scattered up at the far end -- which after five years of Windows is
 * every volume. Free space says "40 GB available"; the real floor is
 * where the last immovable extent sits. */
void shrink_ask(const char *dev, shrink_plan *out);

/* Read every sector of a region and report the first one that will
 * not come back.
 *
 * docs/AURBRIDGE.md, R5: this is done on the region being reclaimed
 * AND on the region NTFS will relocate into, before the resize, "and
 * it is cheap". The machines this product exists for are ten years
 * old; a bad sector discovered halfway through a shrink is the one
 * failure with no way back.
 *
 * Returns 0 if every sector read, -1 otherwise, and fills `first_bad`
 * with the byte offset that failed. `progress` may be NULL. */
int  surface_test(const char *dev, uint64_t from, uint64_t to,
                  uint64_t *first_bad,
                  void (*progress)(uint64_t done, uint64_t total));

#endif
