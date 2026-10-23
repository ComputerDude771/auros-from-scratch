/* plansay.c — plan_say(), the one part of the planner that talks.
 *
 * Its own file because plan.c is compiled into the Windows half as well
 * (build/aurbridge), and stage_say() is the staging environment's. */
#include <stdio.h>

#include "plan.h"
#include "aurstage.h"

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
    say_span("startup", L->rec_first,  L->rec_last,     L->sector);
}
