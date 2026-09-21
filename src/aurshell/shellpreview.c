/* shellpreview — render any archetype to a PNG through its real paint path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"
#include "../common/wall.h"
#include "../common/png.h"

static font *openf(const char *p, float px)
{
    font *f = (p && *p) ? font_load(p, px) : NULL;
    if (f) return f;
    const char *fb[] = {
      "/home/user/auros-from-scratch/work/forge/desktop/rootfs/usr/share/fonts/opentype/inter/Inter-Regular.otf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL };
    for (int i = 0; fb[i]; i++) { f = font_load(fb[i], px); if (f) return f; }
    return NULL;
}

/* A representative machine: some things open, some not. Every layout
 * renders the SAME state, so the previews are genuinely comparable. */
static void seed(shell_ctx *c, int n_open)
{
    const struct { const char *id, *name, *hint; shell_icon ic; uint32_t t; int pin; } A[] = {
      { "web",   "Internet", "Browse the web",        ICON_GLOBE,    0x7DD3C0, 1 },
      { "mail",  "Email",    "Read your messages",    ICON_MAIL,     0x82AAFF, 1 },
      { "photo", "Photos",   "Pictures and videos",   ICON_PHOTOS,   0xA78BFA, 1 },
      { "files", "My Files", "Documents you saved",   ICON_FILES,    0xF2B880, 1 },
      { "write", "Writing",  "Letters and notes",     ICON_TEXT,     0x6FD8DC, 0 },
      { "music", "Music",    "Songs and radio",       ICON_MUSIC,    0xF2788D, 0 },
      { "calc",  "Calculator","Do sums",              ICON_CALC,     0x9BE8D8, 0 },
      { "set",   "Settings", "Change how this works", ICON_SETTINGS, 0x8793A4, 1 },
      { "help",  "Help",     "Show me how",           ICON_HELP,     0x6FD8DC, 0 },
      { "term",  "Terminal", "For advanced use",      ICON_TERMINAL, 0x55606E, 0 },
    };
    c->n_apps = (int)(sizeof A / sizeof A[0]);
    for (int i = 0; i < c->n_apps; i++) {
        snprintf(c->apps[i].id,   sizeof c->apps[i].id,   "%s", A[i].id);
        snprintf(c->apps[i].name, sizeof c->apps[i].name, "%s", A[i].name);
        snprintf(c->apps[i].hint, sizeof c->apps[i].hint, "%s", A[i].hint);
        c->apps[i].icon = A[i].ic; c->apps[i].tint = A[i].t; c->apps[i].pinned = A[i].pin;
    }
    const char *subs[] = { "auros.example", "3 new messages", "Last summer", "Documents" };
    c->n_wins = n_open;
    for (int i = 0; i < n_open && i < SHELL_MAX_WINS; i++) {
        c->wins[i].app = i % c->n_apps;
        snprintf(c->wins[i].title, sizeof c->wins[i].title, "%s", c->apps[c->wins[i].app].name);
        snprintf(c->wins[i].subtitle, sizeof c->wins[i].subtitle, "%s", subs[i % 4]);
        c->wins[i].content = NULL;
    }
    c->focus = n_open ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *shellf = argc > 1 ? argv[1] : "shells/rail.shell";
    const char *themef = argc > 2 ? argv[2] : "themes/nocturne.theme";
    const char *out    = argc > 3 ? argv[3] : "/tmp/shell.png";
    int W = argc > 4 ? atoi(argv[4]) : 1600;
    int H = argc > 5 ? atoi(argv[5]) : 900;
    int nopen = argc > 6 ? atoi(argv[6]) : 3;

    theme_t t = {0};
    if (theme_load(&t, themef) < 0) { fprintf(stderr, "no theme %s\n", themef); return 1; }

    shell_ctx c; memset(&c, 0, sizeof c);
    shell_theme_load(&c, &t);
    if (shell_archetype_load(&c, shellf) < 0) { fprintf(stderr, "no shell %s\n", shellf); return 1; }
    c.allow_install = c.allow_settings = c.allow_theme_change = 1;
    c.mouse_x = c.mouse_y = -1; c.hover = -1;
    seed(&c, nopen);

    const shell_layout *L = shell_layout_by_id(c.layout_id);
    c.screen_w = W; c.screen_h = H;   /* before init: hit-testing needs it */
    if (L->init) L->init(&c);

    const char *fs = theme_str(&t, "font_sans", "");
    shell_fonts f = { openf(fs, 26.f), openf(fs, 17.f), openf(fs, 13.f), openf(fs, 40.f) };

    surface *s = surface_new(W, H), *wall = surface_new(W, H);
    uint32_t *tmp = malloc((size_t)W*H*sizeof *tmp);
    wall_render(tmp, W, H, &t);
    for (int i = 0; i < W*H; i++) wall->px[i] = 0xFF000000u | tmp[i];
    free(tmp);

    L->paint(&c, s, &f, wall);

    uint32_t *o = malloc((size_t)W*H*sizeof *o);
    for (int i = 0; i < W*H; i++) o[i] = s->px[i] & 0xFFFFFFu;
    int rc = png_write_rgb(out, o, W, H);
    free(o); surface_free(s); surface_free(wall);
    if (L->fini) L->fini(&c);
    fprintf(stderr, "%s  %dx%d  layout=%s  theme=%s  open=%d\n",
            out, W, H, c.layout_id, theme_str(&t, "theme_name", "?"), nopen);
    return rc;
}
