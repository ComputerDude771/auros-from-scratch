/* plan.c — see plan.h. Computes; writes nothing. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "plan.h"
#include "aurstage.h"

static uint64_t align_up(uint64_t v, uint64_t a)
{ return a ? ((v + a - 1) / a) * a : v; }
static uint64_t align_dn(uint64_t v, uint64_t a)
{ return a ? (v / a) * a : v; }

int plan_compute(const gpt_table *t, int win_idx,
                 uint64_t win_new_bytes, uint64_t root_src_bytes,
                 uint64_t rec_bytes, uint64_t rsc_bytes,
                 uint64_t min_root_bytes,
                 stage_layout *L, char *why, size_t n)
{
    memset(L, 0, sizeof *L);
    if (!t->valid || win_idx < 0 || (uint32_t)win_idx >= t->n_entries ||
        !gpt_used(&t->ent[win_idx])) {
        snprintf(why, n, "there is no Windows partition to work from");
        return -1;
    }
    uint32_t ss = t->sector;
    uint64_t align = PLAN_ALIGN_BYTES / ss;
    if (!align) align = 1;

    L->sector       = ss;
    L->win_idx      = win_idx;
    L->win_first    = t->ent[win_idx].first;
    L->win_last_old = t->ent[win_idx].last;

    /* The volume's new size, rounded UP to a whole block. The
     * partition entry must never end below the filesystem inside it:
     * a partition shorter than its filesystem is a filesystem whose
     * last blocks are outside the partition, which is unmountable and
     * is not something a later step can notice. */
    uint64_t win_blocks = (win_new_bytes + ss - 1) / ss;
    if (win_blocks == 0) {
        snprintf(why, n, "the Windows drive would be left with no size at all");
        return -1;
    }
    L->win_last_new = L->win_first + win_blocks - 1;
    if (L->win_last_new >= L->win_last_old) {
        snprintf(why, n,
                 "the Windows drive cannot be made smaller than it already is");
        return -1;
    }

    /* THE GAP. Its far edge is the next partition in the TABLE, not
     * the end of the disk. See the header. */
    L->gap_first = L->win_last_new + 1;
    uint64_t gap_end = gpt_gap_end(t, L->win_last_old + 1);   /* exclusive */
    if (gap_end > t->last_usable + 1) gap_end = t->last_usable + 1;
    if (gap_end <= L->gap_first) {
        snprintf(why, n,
                 "there is nothing between the Windows drive and the next "
                 "part of this disk");
        return -1;
    }
    L->gap_last = gap_end - 1;

    /* The recovery partition goes at the FAR end of the gap and the
     * AurOS root takes what is left. That way the root is the thing
     * that grows when a person gives AurOS more space, and the
     * recovery partition stays where the boot entry says it is. */
    uint64_t rec_blocks = align_up((rec_bytes + ss - 1) / ss, align);
    if (rec_blocks == 0) {
        snprintf(why, n, "the recovery area was given no size");
        return -1;
    }
    uint64_t rsc_blocks = align_up((rsc_bytes + ss - 1) / ss, align);
    if (rsc_blocks == 0) {
        snprintf(why, n, "the copy of the way back was given no size");
        return -1;
    }
    /* Carved from the far end inwards, so that the root -- the one
     * thing a person notices the size of -- is what absorbs whatever
     * is left over, and the two small partitions stay where the boot
     * entry and the type GUID say they are. gap_end is exclusive, and
     * both align_dn calls are the reason this cannot be written as one
     * subtraction: rounding each start down to a megabyte moves it,
     * and the next one down has to start from where it landed. */
    uint64_t rec_first = align_dn(gap_end - rec_blocks, align);
    if (rec_first < L->gap_first || rec_first < rsc_blocks) {
        snprintf(why, n,
                 "there is not enough room on this computer for AurOS and a "
                 "way back to Windows");
        return -1;
    }
    uint64_t rsc_first = align_dn(rec_first - rsc_blocks, align);
    uint64_t root_first = align_up(L->gap_first, align);

    if (rsc_first <= root_first) {
        snprintf(why, n,
                 "there is not enough room on this computer for AurOS and a "
                 "way back to Windows");
        return -1;
    }
    L->rec_first = rec_first;
    L->rec_last  = gap_end - 1;
    L->rsc_first = rsc_first;
    L->rsc_last  = rec_first - 1;
    L->root_first = root_first;
    L->root_last  = rsc_first - 1;

    return plan_check(t, L, root_src_bytes, rec_bytes, rsc_bytes,
                      min_root_bytes, why, n);
}

int plan_check(const gpt_table *t, const stage_layout *L,
               uint64_t root_src_bytes, uint64_t rec_bytes,
               uint64_t rsc_bytes, uint64_t min_root_bytes,
               char *why, size_t n)
{
    uint32_t ss = L->sector;
    if (!t->valid || ss == 0) {
        snprintf(why, n, "this disk's partition table could not be read");
        return -1;
    }
    if (t->sector != ss) {
        snprintf(why, n, "this disk changed its sector size mid-install");
        return -1;
    }

    /* In order, and inside the disk. */
    if (!(L->win_first <= L->win_last_new &&
          L->win_last_new < L->root_first &&
          L->root_first <= L->root_last &&
          L->root_last < L->rsc_first &&
          L->rsc_first <= L->rsc_last &&
          L->rsc_last < L->rec_first &&
          L->rec_first <= L->rec_last)) {
        snprintf(why, n, "the pieces of the new layout are out of order");
        return -1;
    }
    if (L->win_first < t->first_usable || L->rec_last > t->last_usable) {
        snprintf(why, n,
                 "the new layout does not fit between the parts of the disk "
                 "the firmware allows");
        return -1;
    }

    /* AGAINST THE TABLE. The one bound that matters: nothing we are
     * about to write may sit on a partition that already exists,
     * except the Windows entry we are shrinking. */
    int hit = gpt_overlaps(t, L->root_first, L->root_last, L->win_idx);
    if (hit >= 0) {
        snprintf(why, n,
                 "the space AurOS would use is already taken by partition %d",
                 hit + 1);
        return -1;
    }
    hit = gpt_overlaps(t, L->rec_first, L->rec_last, L->win_idx);
    if (hit >= 0) {
        snprintf(why, n,
                 "the space the way back would use is already taken by "
                 "partition %d", hit + 1);
        return -1;
    }
    hit = gpt_overlaps(t, L->rsc_first, L->rsc_last, L->win_idx);
    if (hit >= 0) {
        snprintf(why, n,
                 "the space the saved copy of Windows would use is already "
                 "taken by partition %d", hit + 1);
        return -1;
    }
    /* And the shrunk Windows entry must not reach into either. */
    if (L->win_last_new >= L->root_first) {
        snprintf(why, n, "the Windows drive would overlap AurOS");
        return -1;
    }

    uint64_t root_bytes = (L->root_last - L->root_first + 1) * (uint64_t)ss;
    uint64_t rec_have   = (L->rec_last  - L->rec_first  + 1) * (uint64_t)ss;
    uint64_t rsc_have   = (L->rsc_last  - L->rsc_first  + 1) * (uint64_t)ss;
    if (rsc_have < rsc_bytes) {
        snprintf(why, n,
                 "there is not enough room to keep a copy of this computer's "
                 "Windows startup on it");
        return -1;
    }

    if (root_bytes < root_src_bytes) {
        snprintf(why, n,
                 "there is not enough room for AurOS itself on this computer");
        return -1;
    }
    /* THE PRODUCT FLOOR, not the image size. */
    if (root_bytes < min_root_bytes) {
        snprintf(why, n,
                 "AurOS needs %llu GB and this computer can spare only %llu GB",
                 (unsigned long long)(min_root_bytes / 1000000000ull),
                 (unsigned long long)(root_bytes / 1000000000ull));
        return -1;
    }
    if (rec_have < rec_bytes) {
        snprintf(why, n, "there is not enough room for a way back to Windows");
        return -1;
    }

    /* Alignment, which costs nothing to check and halves the write
     * rate on a 4 KiB-physical drive when it is wrong. */
    uint64_t align = PLAN_ALIGN_BYTES / ss;
    if (align && (L->root_first % align || L->rec_first % align)) {
        snprintf(why, n, "the new partitions are not aligned on this disk");
        return -1;
    }
    return 0;
}

static void say_span(const char *what, uint64_t first, uint64_t last,
                     uint32_t ss)
{
    double g = (double)((last - first + 1) * (uint64_t)ss) / (1024.0*1024.0*1024.0);
    stage_say("  %-10s %12llu .. %-12llu  %.1f GiB", what,
              (unsigned long long)first, (unsigned long long)last, g);
}

void plan_say(const stage_layout *L)
{
    stage_say("the new layout, in %u-byte blocks:", L->sector);
    say_span("windows",  L->win_first,  L->win_last_new, L->sector);
    say_span("auros",    L->root_first, L->root_last,    L->sector);
    say_span("saved",    L->rsc_first,  L->rsc_last,     L->sector);
    say_span("way back", L->rec_first,  L->rec_last,     L->sector);
}
