/* fuzz_font.c -- the TrueType/OpenType rasteriser against hostile files.
 *
 * src/common/font.c parses sfnt containers, cmap formats 0/4/6/12,
 * `glyf` simple and composite outlines, a Type 2 charstring
 * interpreter for `CFF ` (subroutines, hintmask, flex, seac, CID
 * keying), legacy `kern` and GPOS pair kerning. Every offset in every
 * one of those is the file's to choose. This harness hands it the
 * fuzzer's bytes as a font file, then does what the shell does with a
 * font: measures and draws text, including accents (composites/seac),
 * Cyrillic, a private-use icon and an astral codepoint (cmap 12), at a
 * size picked from the input, at pen positions that clip on every edge.
 *
 * The file goes through font_load() itself -- the real entry point,
 * path and all -- via a memfd, so nothing about the loader is
 * simulated.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../../src/common/font.h"

static int  memfd = -1;
static char memfd_path[64];

static const char *const TEXT[] = {
    "Ta Wo P. AV fi",
    "\xC3\x84\xC3\xA9\xC3\xB1 \xC5\x93",                 /* Ä é ñ œ: composites, seac */
    "\xD0\x96\xD1\x8F\xD0\x99",                          /* Ж я Й */
    "\xEF\x80\x81 \xF0\x9F\x98\x80 \xEF\xBF\xBD",        /* U+F001, U+1F600, U+FFFD */
    "\xC3\x28 \xED\xA0\x80 \xF4\x90\x80\x80 \xE2\x82",   /* broken UTF-8 */
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > (1u << 20)) return 0;
    if (memfd < 0) {
        memfd = memfd_create("fuzz-font", MFD_CLOEXEC);
        if (memfd < 0) abort();
        snprintf(memfd_path, sizeof memfd_path, "/proc/self/fd/%d", memfd);
    }
    if (ftruncate(memfd, 0) < 0) abort();
    if (pwrite(memfd, data, size, 0) != (ssize_t)size) abort();

    /* The last byte picks the size: 6..69 px covers the UI range, and
     * one value in eight goes large to exercise the big-bitmap paths. */
    uint8_t k = data[size - 1];
    float px = 6.0f + (float)(k % 64);
    if ((k & 0xE0) == 0xE0) px = 180.0f + (float)(k & 31) * 8.0f;

    font *f = font_load(memfd_path, px);
    if (!f) return 0;

    (void)font_ascent(f); (void)font_descent(f); (void)font_line_height(f);

    enum { W = 96, H = 40, STRIDE = 104 };
    static uint32_t fb[STRIDE * H];
    memset(fb, 0, sizeof fb);
    for (size_t i = 0; i < sizeof TEXT / sizeof *TEXT; i++) {
        float tw = font_text_width(f, TEXT[i]);
        (void)tw;
        /* Inside, then hanging off the left/top, then off the
         * right/bottom: the clipping arithmetic is half the attack
         * surface of a blitter. */
        font_draw(f, fb, W, H, STRIDE, 2.0f, (float)H * 0.7f, TEXT[i], 0xD4DCEA, 1.0f);
        font_draw(f, fb, W, H, STRIDE, -7.25f, 3.0f, TEXT[i], 0x7DD3C0, 0.5f);
        font_draw(f, fb, W, H, STRIDE, (float)W - 5.5f, (float)H + 4.0f, TEXT[i], 0xFFFFFF, 1.0f);
    }
    font_free(f);
    return 0;
}
