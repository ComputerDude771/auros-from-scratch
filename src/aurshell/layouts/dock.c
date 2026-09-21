/* layouts/dock.c — the "dock" archetype. STUB: replace with a real renderer.
 *
 * Exists so that partial builds always link while the six archetypes are
 * developed independently. See shells/dock.shell for the interaction model
 * this must implement, and layouts/rail.c for the house style. */
#include "../shell.h"
#include <string.h>
#include <stdio.h>

static void l_init(shell_ctx *c) { (void)c; }
static void l_paint(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall)
{
    if (wall) {
        for (int y = 0; y < s->h && y < wall->h; y++)
            memcpy(s->px + (size_t)y * s->stride, wall->px + (size_t)y * wall->stride,
                   (size_t)(s->w < wall->w ? s->w : wall->w) * sizeof *s->px);
    } else surface_fill(s, 0xFF000000u | c->bg);
    char msg[96];
    snprintf(msg, sizeof msg, "%s — not implemented yet", c->shell_name);
    shell_text_centred(s, f->mid, (float)s->w/2.f, (float)s->h/2.f, msg, c->subtle, 0.8f);
}
static int  l_click(shell_ctx *c, int x, int y) { (void)c;(void)x;(void)y; return 0; }
static void l_motion(shell_ctx *c, int x, int y) { (void)c;(void)x;(void)y; }
static void l_key(shell_ctx *c, int k) { (void)c;(void)k; }
static int  l_step(shell_ctx *c, float dt) { (void)c;(void)dt; return 0; }
static void l_fini(shell_ctx *c) { (void)c; }

const shell_layout layout_dock = {
    "dock", l_init, l_paint, l_click, l_motion, l_key, l_step, l_fini
};
