/* wr.h — the only place in this directory that writes to a disk.
 *
 * Stages A and B write nothing, and build/staging enforces that by
 * refusing to build an image if anything under src/aurstage/ opens a
 * device writably. Stage C has to write. This file is the exception,
 * and it is ONE file so that the gate can stay a gate: everything else
 * remains provably read-only, and the whole of the destructive
 * surface is in one place a person can read in an afternoon.
 *
 * NAMED WINDOWS, NOT ONE RANGE
 *
 * The first design had a single byte range and a single armed flag.
 * An adversarial review pointed out that the GPT commit cannot be
 * expressed in it -- the backup header is at the end of the disk, the
 * primary is at LBA 1, and neither is inside the root extent -- so the
 * one function the header said existed "for the GPT commit" could
 * never be called successfully. The natural repair under deadline is
 * to widen the range or add a bypass flag, and that is exactly the
 * property the file exists to hold.
 *
 * So the windows are disjoint, each with a purpose in its name,
 * all decided in one place. A write names the window it belongs
 * to. Widening one to make a write fit is then a change somebody has
 * to defend, not a flag somebody sets.
 *
 * WHAT THIS DOES NOT COVER, said plainly because a reader will assume
 * otherwise: writes the kernel makes on our behalf. A mount(2) is
 * invisible to it, and so is anything a program we exec does.
 *
 * That paragraph used to end by promising an `espfix` that would mount
 * the machine's EFI partition read-write and read its own writes back
 * by hand. There is no such file and there will not be one. R12 says
 * that partition is never mounted, and loader.c is what replaced the
 * idea: AurOS is started from an EFI System partition of its own,
 * written through THIS file like everything else, so the boot chain is
 * inside the gate rather than beside it.
 *
 * EFI VARIABLES ARE THE ONE THING GENUINELY OUTSIDE IT, and they have
 * a gate of their own: nvram.c, named in build/staging the way this
 * file is, with four checks of its own. See nvram.h.
 */
#ifndef AUROS_WR_H
#define AUROS_WR_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    WR_ROOT = 0,        /* the AurOS root extent                      */
    WR_RECOVERY,        /* the AurOS boot partition: the copy of the
                         * image's own ESP that makes the machine able
                         * to start AurOS on its own. See loader.h.   */
    WR_GPT_PRIMARY,     /* protective MBR, header, primary entry array*/
    WR_GPT_BACKUP,      /* backup entry array and header              */
    WR_LOG,             /* the preallocated journal/log extents       */
    WR_RESCUE,          /* the rescue capture area, ON THE STICK      */
    WR_MIRROR,          /* the second copy of the way back, on the
                         * machine's own disk. Its own kind and not
                         * WR_RECOVERY's, because two extents behind
                         * one name is "one window with two names"
                         * inverted -- and the sentence a failure
                         * prints comes from the name.               */
    WR_RESTORE,         /* captured bytes going back inside a
                         * partition: the ESP, or one volume's $Boot.
                         * Armed over exactly the extent being put
                         * back and disarmed straight after, so the
                         * restore never holds a window wider than the
                         * one thing it is writing. */
    WR_N
} wr_kind;

typedef struct {
    uint64_t lo, hi;    /* [lo, hi) in bytes, absolute on the device  */
    int      armed;
} wr_window;

typedef struct {
    int       fd;
    char      dev[72];
    uint64_t  dev_bytes;
    wr_window win[WR_N];
    uint64_t  written;      /* bytes actually written                 */
    uint64_t  verified;     /* bytes read back and compared           */
    int       touched;      /* has anything at all been written yet   */
} wr_target;

/* Open the device for writing. This is the ONLY O_RDWR open in
 * src/aurstage. Returns 0, or -1 with a sentence in `why`. */
int  wr_open(wr_target *t, const char *dev, char *why, size_t n);

/* Arm one window. Refuses to overlap an already-armed window: two
 * windows that overlap are one window with two names, and the whole
 * point is that a write cannot land somewhere its purpose does not
 * describe. */
int  wr_arm(wr_target *t, wr_kind k, uint64_t lo, uint64_t hi,
            char *why, size_t n);

/* Disarm one window, so that a phase which is finished cannot write
 * again. Called as each phase completes. */
void wr_disarm(wr_target *t, wr_kind k);

/* Write, inside the named window. Any byte outside it is a refusal,
 * not a clamp. */
int  wr_bytes(wr_target *t, wr_kind k, uint64_t off,
              const void *buf, size_t len, char *why, size_t n);

/* Read back and compare. R5 requires every written block to be
 * read-back-verified; making that a function here means it is counted,
 * and wr_verified() is what a test asserts on. */
int  wr_check(wr_target *t, uint64_t off, const void *expect, size_t len,
              uint64_t *first_bad);

/* Push everything to the platter. Before the commit sector, and after
 * it. A commit that is still in a write cache is not a commit. */
int  wr_flush(wr_target *t);

void wr_close(wr_target *t);

uint64_t wr_written(const wr_target *t);
uint64_t wr_verified(const wr_target *t);
/* 1 if not one byte has been written yet. Every refusal path before
 * the shrink asserts this, and so does every test. */
int      wr_untouched(const wr_target *t);

#endif
