/* Exhaustive proof that the reciprocal-multiply replacement for the
 * blur's per-pixel division is EXACT over its entire input domain.
 * Not a spot check: every (window, sum) pair a blur can produce. */
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    long long checked = 0;
    for (int radius = 1; radius <= 128; radius++) {
        int win = radius * 2 + 1;
        uint32_t recip = (0xFFFFFFFFu / (uint32_t)win) + 1u;
        int32_t maxsum = win * 255;
        for (int32_t sum = 0; sum <= maxsum; sum++) {
            int32_t want = (sum + win / 2) / win;
            uint32_t got = (uint32_t)(((uint64_t)(uint32_t)(sum + win / 2) * recip) >> 32);
            if ((int32_t)got != want) {
                printf("MISMATCH radius=%d win=%d sum=%d want=%d got=%u\n",
                       radius, win, sum, want, got);
                return 1;
            }
            checked++;
        }
    }
    printf("exact for all %lld (window, sum) pairs, radius 1..128\n", checked);
    return 0;
}
