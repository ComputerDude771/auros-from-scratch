/* Old vs new blur: identical inputs, compare every channel of every
 * pixel, and time both. The old implementation is pasted in verbatim
 * from git history so the comparison is against what actually shipped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../src/aurshell/draw.h"

/* ── the old implementation, verbatim ─────────────────────────────── */
static void old_pass(uint32_t *src, uint32_t *dst, int w, int h, int stride,
                     int radius, int horizontal)
{
    int outer = horizontal ? h : w;
    int inner = horizontal ? w : h;
    int step  = horizontal ? 1 : stride;
    int jump  = horizontal ? stride : 1;
    int win   = radius * 2 + 1;
    for (int o = 0; o < outer; o++) {
        uint32_t *line_s = src + (size_t)o * jump;
        uint32_t *line_d = dst + (size_t)o * jump;
        int32_t ra = 0, rr = 0, rg = 0, rb = 0;
        for (int i = -radius; i <= radius; i++) {
            int k = i < 0 ? 0 : (i >= inner ? inner - 1 : i);
            uint32_t p = line_s[(size_t)k * step];
            ra += (int32_t)((p >> 24) & 0xFF); rr += (int32_t)((p >> 16) & 0xFF);
            rg += (int32_t)((p >> 8)  & 0xFF); rb += (int32_t)( p        & 0xFF);
        }
        for (int i = 0; i < inner; i++) {
            int32_t oa = (ra + win/2) / win, orr = (rr + win/2) / win;
            int32_t og = (rg + win/2) / win, ob  = (rb + win/2) / win;
            if (oa<0) oa=0; if (oa>255) oa=255;
            if (orr<0) orr=0; if (orr>255) orr=255;
            if (og<0) og=0; if (og>255) og=255;
            if (ob<0) ob=0; if (ob>255) ob=255;
            line_d[(size_t)i * step] = ((uint32_t)oa<<24)|((uint32_t)orr<<16)|
                                       ((uint32_t)og<<8)|(uint32_t)ob;
            int add = i + radius + 1; if (add >= inner) add = inner - 1;
            int sub = i - radius;     if (sub < 0)      sub = 0;
            uint32_t pa = line_s[(size_t)add*step], ps = line_s[(size_t)sub*step];
            ra += (int32_t)((pa>>24)&0xFF) - (int32_t)((ps>>24)&0xFF);
            rr += (int32_t)((pa>>16)&0xFF) - (int32_t)((ps>>16)&0xFF);
            rg += (int32_t)((pa>>8) &0xFF) - (int32_t)((ps>>8) &0xFF);
            rb += (int32_t)( pa     &0xFF) - (int32_t)( ps     &0xFF);
        }
    }
}
static void old_blur(surface *s, rect r, int radius)
{
    if (radius < 1) return;
    int x0 = r.x<0?0:r.x, y0 = r.y<0?0:r.y;
    int x1 = r.x+r.w > s->w ? s->w : r.x+r.w;
    int y1 = r.y+r.h > s->h ? s->h : r.y+r.h;
    int w = x1-x0, h = y1-y0;
    if (w<=0||h<=0) return;
    uint32_t *a = malloc((size_t)w*h*4), *b = malloc((size_t)w*h*4);
    for (int y=0;y<h;y++) memcpy(a+(size_t)y*w, s->px+(size_t)(y0+y)*s->stride+x0, (size_t)w*4);
    for (int p=0;p<3;p++){ old_pass(a,b,w,h,w,radius,1); old_pass(b,a,w,h,w,radius,0); }
    for (int y=0;y<h;y++) memcpy(s->px+(size_t)(y0+y)*s->stride+x0, a+(size_t)y*w, (size_t)w*4);
    free(a); free(b);
}

static double ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
                        return t.tv_sec*1e3 + t.tv_nsec/1e6; }

static uint32_t rng = 12345;
static uint32_t nextr(void){ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }

int main(void)
{
    const int W = 1366, H = 768;
    int radii[] = {1, 2, 5, 12, 20, 28, 40, 64, 128};
    int nr = sizeof radii / sizeof *radii;
    /* Region shapes that actually occur: full screen, a wide bar, a
     * narrow side strip, a tall thin sliver, a 1-pixel edge case. */
    rect regions[] = {
        {0,0,W,H}, {0,H-72,W,72}, {W-360,0,360,H}, {40,40,3,600},
        {10,10,1,1}, {100,100,900,500}, {-50,-50,400,400}, {W-20,H-20,200,200},
    };
    int nq = sizeof regions / sizeof *regions;

    long worst = 0; double t_old = 0, t_new = 0;
    for (int ri = 0; ri < nr; ri++) {
        for (int qi = 0; qi < nq; qi++) {
            surface *s1 = surface_new(W,H), *s2 = surface_new(W,H);
            /* Worst case for a running sum: full-contrast noise, plus a
             * flat region and a gradient so edge clamping is exercised. */
            for (int i = 0; i < W*H; i++) {
                uint32_t v;
                if (i % 3 == 0)      v = nextr();
                else if (i % 3 == 1) v = 0xFF000000u | (uint32_t)((i % 256) * 0x010101u);
                else                 v = 0xFFFFFFFFu;
                s1->px[i] = v; s2->px[i] = v;
            }
            double a0 = ms(); old_blur(s1, regions[qi], radii[ri]); t_old += ms()-a0;
            double b0 = ms(); draw_blur_region(s2, regions[qi], radii[ri]); t_new += ms()-b0;

            for (int i = 0; i < W*H; i++) {
                uint32_t p = s1->px[i], q = s2->px[i];
                for (int c = 0; c < 4; c++) {
                    long d = (long)((p >> (c*8)) & 0xFF) - (long)((q >> (c*8)) & 0xFF);
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                }
            }
            surface_free(s1); surface_free(s2);
        }
    }
    printf("max channel difference over %d radii x %d regions: %ld\n", nr, nq, worst);
    printf("old total %.1f ms   new total %.1f ms   speedup %.2fx\n",
           t_old, t_new, t_old / t_new);
    return worst <= 2 ? 0 : 1;
}
