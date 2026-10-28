/* shellcommon.c — helpers shared by every archetype.
 *
 * These live here rather than in each layout so that six independently
 * written renderers cannot drift into six slightly different icon sets,
 * six clock formats and six ideas of where a text baseline sits. */
#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

float shell_text_w(font *f, const char *t) { return (f && t) ? font_text_width(f, t) : 0.f; }

/* ── what a key types when there is no keymap to ask ───────────────
 *
 * A real session fills c->key_text from the keymap the machine is
 * actually configured with, and everything that reads typing prefers
 * it. This is the answer when there is nothing to ask: the preview
 * renderer, the contact sheet, the harnesses -- and, on a real machine,
 * the fallback mode where the compositor could not start at all.
 *
 * That last one is why this is here rather than private to one
 * archetype. A machine whose compositor failed can run no applications,
 * and the one thing still worth doing on it is getting it onto the
 * network so it can be repaired -- which needs a password field that
 * accepts characters. The wifi panel could not type into one, because
 * the only table in the product was a static in dock.c.
 *
 * It is unshifted US QWERTY and it is not trying to be more. A machine
 * with a keymap does not consult it.
 */
char shell_key_char(int evdev_code)
{
    static const struct { int base; const char *row; } R[] = {
        {  2, "1234567890-=" }, { 16, "qwertyuiop[]" },
        { 30, "asdfghjkl;'"  }, { 44, "zxcvbnm,./"   },
    };
    if (evdev_code == 57) return ' ';
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        int len = (int)strlen(R[i].row);
        if (evdev_code >= R[i].base && evdev_code < R[i].base + len)
            return R[i].row[evdev_code - R[i].base];
    }
    return 0;
}

/* See shell.h: off on every real machine, on in tools/storetest.c. */
int            shell_text_log_on;
int            shell_text_log_n;
shell_text_rec shell_text_log[SHELL_TEXT_LOG_MAX];

static void text_log(font *f, float x, float y, const char *t, float w)
{
    if (!shell_text_log_on || !f || !t || !*t) return;
    if (shell_text_log_n >= SHELL_TEXT_LOG_MAX) return;
    shell_text_rec *r = &shell_text_log[shell_text_log_n++];
    snprintf(r->s, sizeof r->s, "%s", t);
    float asc = font_ascent(f), des = font_descent(f);
    r->x = (int)x;
    r->y = (int)(y - asc);
    r->w = (int)(w + 0.5f);
    r->h = (int)(asc + des + 0.5f);
}

void shell_text(surface *s, font *f, float x, float y, const char *t, uint32_t c, float a)
{
    if (!f || !t || !*t) return;
    font_draw(f, s->px, s->w, s->h, s->stride, x, y, t, c, a);
    if (shell_text_log_on) text_log(f, x, y, t, shell_text_w(f, t));
}
void shell_text_centred(surface *s, font *f, float cx, float y, const char *t, uint32_t c, float a)
{
    if (!f || !t || !*t) return;
    float w = shell_text_w(f, t);
    font_draw(f, s->px, s->w, s->h, s->stride, cx - w/2.f, y, t, c, a);
    if (shell_text_log_on) text_log(f, cx - w/2.f, y, t, w);
}
float shell_baseline(font *f, float y, float h)
{
    if (!f) return y + h * 0.5f + 5.f;
    return y + h * 0.5f + font_ascent(f) * 0.5f - font_descent(f) * 0.5f;
}
void shell_clock(char *hm, size_t hm_n, char *date, size_t date_n)
{
    /* AURSHELL_CLOCK pins the clock so two renders are comparable.
     * Without it, any pixel comparison between two builds picks up
     * whatever minute each one happened to run in -- which reads as a
     * rendering regression of a hundred levels in the top bar, and
     * costs an hour before anyone notices it is the time. Screenshots
     * for documentation want this too. */
    const char *fake = getenv("AURSHELL_CLOCK");
    if (fake && *fake) {
        time_t t = (time_t)strtol(fake, NULL, 10);
        struct tm tmv;
        gmtime_r(&t, &tmv);
        if (hm)   strftime(hm,   hm_n,   "%H:%M", &tmv);
        if (date) strftime(date, date_n, "%a %d %b", &tmv);
        return;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (hm)   strftime(hm,   hm_n,   "%H:%M", &tmv);
    if (date) strftime(date, date_n, "%a %d %b", &tmv);
}

/* -- the marks ------------------------------------------------------
 *
 * Twelve silhouettes, cut rather than outlined.
 *
 * What was here before was one recipe applied twelve times: a rounded
 * box at 20% fill with a 1.6px border and some lines inside it. At
 * 40px on a tile that reads as twelve of the same object, which is
 * exactly the generic-app-icon look the brief forbids, and it is also
 * why every archetype had to put a tinted halo disc behind one to make
 * it register at all.
 *
 * These are solid instead. Ink goes down first as a shape you could
 * recognise from across a room; the detail is then KNOCKED OUT in the
 * paper colour the caller just filled, which is how a pictogram in a
 * printed timetable works and why those survive being photocopied.
 * Nothing here needs a halo, because a solid shape on paper already
 * has contrast.
 *
 * The weights are deliberately uneven. GLOBE, MAIL and PLUS are heavy
 * masses; SETTINGS and TEXT are almost entirely rule-work; WINDOW is a
 * hollow frame where TERMINAL is a filled block, so the two that used
 * to be the same rectangle are now opposites. A set where every mark
 * weighs the same has no rhythm, and a row of them reads as wallpaper.
 *
 * Cost, since this is an Atom: the old ICON_GLOBE stamped roughly 105
 * overlapping anti-aliased discs for its ring and another 100 for its
 * meridians, per draw, because draw.h had no stroked-arc primitive.
 * This one is a disc and four spans.
 * ------------------------------------------------------------------ */

/* An integer-snapped box in the middle of a mark. Marks are specified
 * on a 24-unit grid and scaled, so every edge would otherwise land on
 * a fraction and come back soft; snapping keeps a 14px mark crisp. */
static rect mk_box(float cx, float cy, float x, float y, float w, float h, float u)
{
    rect r;
    r.x = (int)(cx + x * u + 0.5f);
    r.y = (int)(cy + y * u + 0.5f);
    r.w = (int)(w * u + 0.5f);
    r.h = (int)(h * u + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

void shell_icon_draw(surface *s, shell_icon ic, float cx, float cy, float sz,
                     uint32_t ink, uint32_t paper, float a)
{
    float u = sz / 24.f;

    switch (ic) {

    /* Internet. The heaviest mark in the set: a filled world with the
     * grid cut out of it, so at 16px it is still a dark disc and at
     * 64px it is still a globe. */
    case ICON_GLOBE:
        draw_circle(s, cx, cy, 10.5f*u, ink, a);
        draw_rect(s, mk_box(cx, cy, -10.5f, -0.9f, 21.0f, 1.8f, u), paper, a);
        draw_rect(s, mk_box(cx, cy,  -9.2f, -5.6f, 18.4f, 1.5f, u), paper, a);
        draw_rect(s, mk_box(cx, cy,  -9.2f,  4.1f, 18.4f, 1.5f, u), paper, a);
        draw_rect(s, mk_box(cx, cy,  -0.8f,-10.5f,  1.7f,21.0f, u), paper, a);
        break;

    /* Email. A sealed slab with the flap cut into it. */
    case ICON_MAIL:
        draw_rect(s, mk_box(cx, cy, -11.f, -8.f, 22.f, 16.f, u), ink, a);
        draw_line(s, cx - 10.2f*u, cy - 7.2f*u, cx, cy + 0.9f*u, 2.3f*u, paper, a);
        draw_line(s, cx + 10.2f*u, cy - 7.2f*u, cx, cy + 0.9f*u, 2.3f*u, paper, a);
        break;

    /* Photos. A slab with a sun and a ridge cut out -- the oldest
     * pictogram in the world and still the fastest to read. */
    case ICON_PHOTOS: {
        rect slab = mk_box(cx, cy, -11.f, -9.f, 22.f, 18.f, u);
        draw_rect(s, slab, ink, a);
        draw_circle(s, cx - 5.8f*u, cy - 4.6f*u, 2.4f*u, paper, a);
        /* The ridge, rasterised row by row and CLIPPED TO THE SLAB. A
         * wedge whose half-width is allowed to outgrow the frame stops
         * being a mountain in a picture and becomes a white corner. */
        float apx = cx + 1.6f*u, apy = cy + 0.4f*u;
        float basey = (float)(slab.y + slab.h) - 1.f;
        float half  = 7.6f*u;
        int y0 = (int)apy, y1 = (int)basey;
        for (int y = y0; y <= y1; y++) {
            float k  = (basey > apy) ? ((float)y - apy) / (basey - apy) : 1.f;
            float hw = half * k + 0.6f*u;
            int xa = (int)(apx - hw), xb = (int)(apx + hw);
            if (xa < slab.x + 1) xa = slab.x + 1;
            if (xb > slab.x + slab.w - 1) xb = slab.x + slab.w - 1;
            if (xb > xa) draw_rect(s, (rect){ xa, y, xb - xa, 1 }, paper, a);
        }
        break; }

    /* My Files. A folder: a tab and a body, one hairline of paper
     * between the back and the front so it has a mouth. */
    case ICON_FILES:
        draw_rect(s, mk_box(cx, cy, -11.f, -10.5f, 11.5f, 5.2f, u), ink, a);
        draw_rect(s, mk_box(cx, cy, -11.f,  -5.6f, 22.f, 16.6f, u), ink, a);
        /* The lip of the front flap, set high and short so the mark
         * does not read as an equals sign at 17px — which is the size
         * it lands at in the rail's index. */
        draw_rect(s, mk_box(cx, cy,  -7.f,  -0.6f, 12.f,  1.4f, u), paper, a);
        break;

    /* Settings. Not a gear -- a bank of faders. Almost pure rule-work,
     * the lightest mark in the set, and it says "adjust" rather than
     * "machinery", which is the right promise for this button. */
    case ICON_SETTINGS: {
        const float ys[3] = { -7.2f, 0.f, 7.2f };
        const float ks[3] = { -2.5f, 4.0f, -5.5f };
        for (int i = 0; i < 3; i++) {
            draw_rect(s, mk_box(cx, cy, -10.5f, ys[i] - 0.7f, 21.f, 1.4f, u), ink, a);
            draw_rect(s, mk_box(cx, cy, ks[i] - 1.9f, ys[i] - 3.4f, 3.8f, 6.8f, u), ink, a);
        }
        break; }

    /* Help. A solid disc with the question cut out of it: the mark is
     * the ink, the meaning is the hole. */
    case ICON_HELP:
        draw_circle(s, cx, cy, 11.f*u, ink, a);
        /* The hook wants a big radius and a THIN stroke, or the discs
         * that draw it close the counter and the whole mark comes back
         * as a blob with a notch. Measured at 33px, where it lands on
         * the locked archetype's switcher. */
        for (float g = 2.85f; g > -1.15f; g -= 0.07f)
            draw_circle(s, cx + cosf(g)*5.2f*u, cy - 3.9f*u + sinf(g)*5.2f*u,
                        1.35f*u, paper, a);
        draw_rect(s, mk_box(cx, cy, -1.3f, 0.6f, 2.7f, 3.9f, u), paper, a);
        draw_rect(s, mk_box(cx, cy, -1.5f, 6.2f, 3.1f, 3.1f, u), paper, a);
        break;

    /* A window: hollow, with a title band. Its opposite number is
     * ICON_TERMINAL, which is the same proportion filled solid. */
    case ICON_WINDOW: {
        rect o = mk_box(cx, cy, -11.f, -8.5f, 22.f, 17.f, u);
        draw_rect(s, o, ink, a);
        rect in = { o.x + (int)(2.f*u + 0.5f), o.y + (int)(5.2f*u + 0.5f),
                    o.w - (int)(4.f*u + 1.f), o.h - (int)(7.2f*u + 1.f) };
        if (in.w > 0 && in.h > 0) draw_rect(s, in, paper, a);
        break; }

    /* Plus. Square ends, no radius, no apology. */
    case ICON_PLUS:
        draw_rect(s, mk_box(cx, cy, -10.f, -2.2f, 20.f, 4.4f, u), ink, a);
        draw_rect(s, mk_box(cx, cy, -2.2f, -10.f, 4.4f, 20.f, u), ink, a);
        break;

    /* Writing. A sheet with ragged lines knocked out: the only mark
     * whose interior is deliberately uneven, because writing is. */
    case ICON_TEXT: {
        draw_rect(s, mk_box(cx, cy, -8.5f, -11.f, 17.f, 22.f, u), ink, a);
        const float ws[5] = { 10.5f, 11.5f, 9.f, 11.5f, 5.5f };
        for (int i = 0; i < 5; i++)
            draw_rect(s, mk_box(cx, cy, -5.8f, -7.6f + (float)i*3.6f, ws[i], 1.5f, u),
                      paper, a);
        break; }

    /* Music. All positive shape, no knockout at all -- the one mark in
     * the set that is pure silhouette. */
    case ICON_MUSIC:
        draw_circle(s, cx - 5.f*u, cy + 6.8f*u, 3.8f*u, ink, a);
        draw_circle(s, cx + 5.6f*u, cy + 4.6f*u, 3.8f*u, ink, a);
        draw_rect(s, mk_box(cx, cy, -2.4f, -9.6f, 2.6f, 16.6f, u), ink, a);
        draw_rect(s, mk_box(cx, cy,  6.8f,-11.6f, 2.6f, 16.4f, u), ink, a);
        draw_line(s, cx - 2.4f*u, cy - 8.8f*u, cx + 9.4f*u, cy - 10.8f*u,
                  3.f*u, ink, a);
        break;

    /* Terminal. Solid block, prompt cut out. */
    case ICON_TERMINAL:
        draw_rect(s, mk_box(cx, cy, -11.f, -8.5f, 22.f, 17.f, u), ink, a);
        draw_line(s, cx - 6.2f*u, cy - 3.6f*u, cx - 1.6f*u, cy + 0.2f*u, 2.2f*u, paper, a);
        draw_line(s, cx - 6.2f*u, cy + 4.0f*u, cx - 1.6f*u, cy + 0.2f*u, 2.2f*u, paper, a);
        draw_rect(s, mk_box(cx, cy, 0.8f, 3.2f, 7.f, 1.8f, u), paper, a);
        break;

    /* Calculator. Portrait block, a display and six keys knocked out --
     * the same portrait footprint as ICON_TEXT, told apart by what is
     * cut into it rather than by its outline, which is what a real
     * pictogram family does. */
    /* Get more programs. A carrier bag -- the shop, not the shelf --
     * with a plus cut through it: something is added. A solid body and
     * an open handle, so at 17px it is still a bag and not a box. */
    case ICON_STORE:
        /* Wider than it is tall, with a handle nearly as wide as the
         * bag and a thin one: a narrow, thick handle on a tall body is
         * a padlock, which is the opposite of the promise. */
        draw_rect(s, mk_box(cx, cy, -11.f, -3.0f, 22.f, 13.5f, u), ink, a);
        draw_rect(s, mk_box(cx, cy, -7.5f, -10.5f, 15.f, 8.0f, u), ink, a);
        draw_rect(s, mk_box(cx, cy, -5.9f, -8.9f, 11.8f, 5.9f, u), paper, a);
        draw_rect(s, mk_box(cx, cy, -4.6f,  2.6f, 9.2f, 2.3f, u), paper, a);
        draw_rect(s, mk_box(cx, cy, -1.15f, -0.9f, 2.3f, 9.3f, u), paper, a);
        break;

    /* All my programs. Three rows, each a mark and a line: the shape of
     * the list it opens, which is the most literal promise a button can
     * make. */
    case ICON_LIST:
        for (int r = 0; r < 3; r++) {
            float y = -8.5f + (float)r * 7.0f;
            draw_rect(s, mk_box(cx, cy, -11.f, y, 4.4f, 4.4f, u), ink, a);
            draw_rect(s, mk_box(cx, cy, -4.5f, y + 0.6f, 15.5f, 3.2f, u), ink, a);
        }
        break;

    case ICON_CALC: {
        draw_rect(s, mk_box(cx, cy, -8.5f, -11.f, 17.f, 22.f, u), ink, a);
        draw_rect(s, mk_box(cx, cy, -5.8f, -8.2f, 11.6f, 4.4f, u), paper, a);
        for (int r = 0; r < 2; r++)
            for (int k = 0; k < 3; k++)
                draw_rect(s, mk_box(cx, cy, -5.8f + (float)k*4.3f,
                                    -0.6f + (float)r*4.6f, 2.9f, 2.9f, u), paper, a);
        break; }
    }
}

/* The starter set of things the machine can do.
 *
 * Shared, because it was three separate copies — main.c, shellpreview.c
 * and railpreview.c — and each one carried the SAME nine Nocturne hexes
 * baked in as app tints. docs/SHELLS.md states the rule in writing: no
 * archetype may hardcode a colour. The file that seeds every icon in
 * the system broke it, which is why Sandstone's warm terracotta-and-oat
 * desktop still glowed mint, cornflower and lavender — another theme's
 * palette pasted onto its design.
 *
 * Tints now cycle the theme's own decorative accents, so a theme that
 * defines three gets three and a theme that sets all three alike gets a
 * monochrome icon set, which is a legitimate thing for a theme to want.
 * Call this AFTER shell_theme_load(); it reads the resolved accents.
 */
void shell_seed_apps(shell_ctx *c)
{
    static const struct { const char *id, *name, *hint; shell_icon ic; int pin; } A[] = {
      { "web",   "Internet",   "Browse the web",        ICON_GLOBE,    1 },
      { "mail",  "Email",      "Read your messages",    ICON_MAIL,     1 },
      { "photo", "Photos",     "Pictures and videos",   ICON_PHOTOS,   1 },
      { "files", "My Files",   "Documents you saved",   ICON_FILES,    1 },
      { "write", "Writing",    "Letters and notes",     ICON_TEXT,     0 },
      { "music", "Music",      "Songs and radio",       ICON_MUSIC,    0 },
      { "calc",  "Calculator", "Do sums",               ICON_CALC,     0 },
      { "set",   "Settings",   "Change how this works", ICON_SETTINGS, 1 },
      { "help",  "Help",       "Show me how",           ICON_HELP,     0 },
    };
    c->n_apps = (int)(sizeof A / sizeof A[0]);
    if (c->n_apps > SHELL_MAX_APPS) c->n_apps = SHELL_MAX_APPS;
    for (int i = 0; i < c->n_apps; i++) {
        snprintf(c->apps[i].id,   sizeof c->apps[i].id,   "%s", A[i].id);
        snprintf(c->apps[i].name, sizeof c->apps[i].name, "%s", A[i].name);
        snprintf(c->apps[i].hint, sizeof c->apps[i].hint, "%s", A[i].hint);
        c->apps[i].icon   = A[i].ic;
        c->apps[i].pinned = A[i].pin;
        /* One ink for every mark.
         *
         * There used to be a colour column here, and three accents
         * cycling down it. Cycling three accents for variety is the
         * thing docs/THEMING.md forbids in writing -- accent_alt is for
         * a different KIND of thing, not for making a list look less
         * plain -- and a row of differently tinted discs is the single
         * most recognisable AI-generated app grid there is.
         *
         * What tells two applications apart here is the shape of the
         * mark. What colour means in this system is state: one accent,
         * used where something is happening. */
        c->apps[i].tint = c->fg;
    }
}

static int utf8_len(unsigned char b)
{
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Draw text, and stop at a width.
 *
 * Application names come from packages, not from us: "ImageMagick
 * (color depth=q16)" is a real entry on a real machine, and it ran
 * straight through the column beside it and printed on top of that
 * row's description. A list whose longest row is unreadable is not a
 * list. Cut at the last whole character that fits and close with an
 * ellipsis, which is the printer's answer and still tells the user
 * there is more name than this. */
void shell_text_elided(surface *s, font *f, float x, float y, float max_w,
                       const char *t, uint32_t col, float a)
{
    if (!f || !t || !*t || max_w <= 0.f) return;
    if (font_text_width(f, t) <= max_w) { shell_text(s, f, x, y, t, col, a); return; }

    const char *ell = "\xE2\x80\xA6";                 /* U+2026 */
    float ew = font_text_width(f, ell);
    char buf[160];
    size_t n = 0;
    for (const char *p = t; *p && n + 8 < sizeof buf; ) {
        int l = utf8_len((unsigned char)*p);
        for (int i = 0; i < l && p[i]; i++) buf[n + (size_t)i] = p[i];
        size_t nn = n + (size_t)l;
        buf[nn] = 0;
        if (font_text_width(f, buf) + ew > max_w) { buf[n] = 0; break; }
        n = nn;
        p += l;
    }
    /* Not even one character fits: draw nothing rather than an ellipsis
     * floating where a name should be. */
    if (!n) return;
    snprintf(buf + n, sizeof buf - n, "%s", ell);
    shell_text(s, f, x, y, buf, col, a);
}

/* Tracked capitals, drawn a codepoint at a time. The tracking is what
 * makes a 10px label read as a rubric rather than as shouted body
 * text -- a printed form does the same thing above a field. Kerning is
 * deliberately dropped: tracked capitals do not want it. */
float shell_text_tracked_w(font *f, const char *t, float track)
{
    if (!f || !t || !*t) return 0.f;
    float w = 0.f;
    int n = 0;
    for (const char *p = t; *p; ) {
        int l = utf8_len((unsigned char)*p);
        char g[5] = {0};
        for (int i = 0; i < l && p[i]; i++) g[i] = p[i];
        w += font_text_width(f, g);
        p += l; n++;
    }
    if (n > 1) w += track * (float)(n - 1);
    return w;
}

void shell_text_tracked(surface *s, font *f, float x, float y,
                        const char *t, uint32_t col, float a, float track)
{
    if (!f || !t || !*t) return;
    if (shell_text_log_on) text_log(f, x, y, t, shell_text_tracked_w(f, t, track));
    for (const char *p = t; *p; ) {
        int l = utf8_len((unsigned char)*p);
        char g[5] = {0};
        for (int i = 0; i < l && p[i]; i++) g[i] = p[i];
        font_draw(f, s->px, s->w, s->h, s->stride, x, y, g, col, a);
        x += font_text_width(f, g) + track;
        p += l;
    }
}

/* Theme numbers are clamped HERE, once, because every layout does its
 * own arithmetic with them -- s->h - bar_h - 2 * margin and the like --
 * and the theme the shell reads may be her own copy of shell.conf,
 * which any program she runs can rewrite. radius=2147483647 used to
 * reach draw.c as signed overflow (tools/fuzz/fuzz_theme). The ranges
 * are several times what any shipped theme uses (radius <= 18,
 * border 2, bar 38, blur and shadow 0), so no real theme changes. */
static int theme_int_in(const theme_t *t, const char *k, int fb, int lo, int hi)
{
    int v = theme_int(t, k, fb);
    return v < lo ? lo : v > hi ? hi : v;
}
static float theme_frac(const theme_t *t, const char *k, double fb)
{
    double v = theme_num(t, k, fb);         /* finite: theme_num refuses nan/inf */
    return (float)(v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v);
}

void shell_theme_load(shell_ctx *c, const theme_t *t)
{
    c->theme = *t;
    c->radius    = theme_int_in(t, "radius", 14, 0, 64);
    c->radius_sm = theme_int_in(t, "radius_sm", 8, 0, 64);
    c->margin    = theme_int_in(t, "margin", 14, 0, 128);
    c->padding   = theme_int_in(t, "padding", 14, 0, 128);
    c->border    = theme_int_in(t, "border", 2, 0, 16);
    c->bar_h     = theme_int_in(t, "bar_height", 38, 0, 256);
    c->blur_r    = theme_int_in(t, "blur_radius", 20, 0, 128);
    c->shadow_r  = theme_int_in(t, "shadow_radius", 28, 0, 256);
    c->panel_a   = theme_frac(t, "opacity_panel", 0.88);
    c->shadow_a  = theme_frac(t, "shadow_opacity", 0.50);
    c->bg         = theme_color(t, "col_bg",          theme_color(t, "bg", 0x0B0E14));
    c->bg_alt     = theme_color(t, "col_bar_bg",      theme_color(t, "bg_alt", 0x10151F));
    c->surface_c  = theme_color(t, "col_surface",     theme_color(t, "surface", 0x161C28));
    c->surface_hi = theme_color(t, "col_surface_hi",  theme_color(t, "surface_hi", 0x1F2735));
    c->overlay    = theme_color(t, "col_overlay",     theme_color(t, "overlay", 0x2B3542));
    c->muted      = theme_color(t, "col_muted",       theme_color(t, "muted", 0x55606E));
    c->subtle     = theme_color(t, "col_subtle",      theme_color(t, "subtle", 0x8793A4));
    c->fg         = theme_color(t, "col_fg",          theme_color(t, "fg", 0xD4DCEA));
    c->fg_hi      = theme_color(t, "col_fg_hi",       theme_color(t, "fg_hi", 0xF3F7FD));
    c->accent     = theme_color(t, "col_accent",      theme_color(t, "accent", 0x7DD3C0));
    c->accent_alt = theme_color(t, "col_accent_alt",  theme_color(t, "accent_alt", 0xA78BFA));
    c->accent_warm= theme_color(t, "col_accent_warm", theme_color(t, "accent_warm", 0xF2B880));
    c->err        = theme_color(t, "col_err",         theme_color(t, "err", 0xF2788D));
    c->ok         = theme_color(t, "col_ok",          theme_color(t, "ok", 0x7DD3C0));
    c->info       = theme_color(t, "col_info",        theme_color(t, "info", 0x82AAFF));
    snprintf(c->brand, sizeof c->brand, "%s", theme_str(t, "brand_text", "AurOS"));

    /* docs/SHELLS.md:180 -- "No archetype may hardcode a colour." The
     * app table used to carry nine literal hexes from one theme, so
     * every tile glowed mint and lavender on a warm-paper desktop. A
     * mark is drawn in the theme's ink; identity comes from its SHAPE.
     * Colour in this system means state, and an app that merely exists
     * is not a state. Kept as a field rather than folded away so a
     * profile can still assign one deliberately. */
    for (int i = 0; i < c->n_apps; i++) c->apps[i].tint = c->fg;
}

int shell_archetype_load(shell_ctx *c, const char *path)
{
    /* The .shell format is the same key="value" shape as a theme, so it
     * reuses the theme parser rather than inventing a second one. */
    theme_t a = {0};
    if (theme_load(&a, path) < 0) return -1;
    snprintf(c->layout_id,  sizeof c->layout_id,  "%s", theme_str(&a, "layout", "rail"));
    snprintf(c->shell_name, sizeof c->shell_name, "%s", theme_str(&a, "shell_name", "AurOS"));
    c->target_large    = strcmp(theme_str(&a, "target_size", "large"), "dense") != 0;
    c->status_full     = strcmp(theme_str(&a, "status_strip", "minimal"), "full") == 0;
    c->show_clock      = strcmp(theme_str(&a, "show_clock", "yes"), "no") != 0;
    c->show_positions  = strcmp(theme_str(&a, "show_positions", "no"), "yes") == 0;
    c->workspaces      = theme_int(&a, "workspaces", 0);

    c->locked_autostart = strcmp(theme_str(&a, "locked_autostart", "first"), "none") != 0;
    {
        const char *sw = theme_str(&a, "locked_show_switcher", "auto");
        c->locked_show_switcher = !strcmp(sw, "always") ? 1 : !strcmp(sw, "never") ? 2 : 0;
    }
    /* Defaults to none on purpose. An escape hatch that appears because
     * a key was missing from a profile is the opposite of a locked
     * machine, so absence of the key must mean absence of the hatch. */
    c->locked_exit_combo = strcmp(theme_str(&a, "locked_exit_combo", "none"), "admin") == 0;
    return 0;
}

const shell_layout *shell_layout_by_id(const char *id)
{
    const shell_layout *all[] = {
        &layout_rail, &layout_tiles, &layout_locked,
        &layout_taskbar, &layout_dock, &layout_workbench, NULL
    };
    for (int i = 0; all[i]; i++)
        if (all[i]->id && strcmp(all[i]->id, id) == 0) return all[i];
    return &layout_rail;   /* the safe default: nothing can hide in it */
}

/* ── fonts ───────────────────────────────────────────────────────── */
static font *open_font(const char *named, float px)
{
    if (named && named[0] == '/') {
        font *f = font_load(named, px);
        if (f) return f;
    }
    /* A desktop with the wrong font is recoverable; one with no text is
     * not, so fall through every plausible location before giving up. */
    static const char *fb[] = {
        "/usr/share/fonts/truetype/paratype/PTC55F.ttf",   /* PT Sans Caption */
        "/usr/share/fonts/truetype/paratype/PTS55F.ttf",
        "/usr/share/fonts/truetype/clear-sans/ClearSans-Regular.ttf",
        "/usr/share/auros/fonts/Inter.ttf",
        "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL
    };
    for (int i = 0; fb[i]; i++) {
        font *f = font_load(fb[i], px);
        if (f) return f;
    }
    return NULL;
}

/* The type system, and it IS a system now rather than one file opened
 * at four sizes.
 *
 * Two things were wrong with the old four lines. The first is that
 * four sizes of one regular face is not a hierarchy: 36px and 24px of
 * the same sans read as the same voice at two volumes, which is why
 * nothing on screen could be important without also being coloured.
 * The second is that font_size_sm, font_size_lg and line_height were
 * in the theme schema, in the template, and in every .theme file, and
 * NOTHING READ THEM -- the scale was three multipliers hardcoded here.
 * A theme author who edited font_size_lg got nothing at all.
 *
 * So: the display face supplies huge/big/dmid, the text face supplies
 * small/tiny, and the text face's BOLD cut supplies mid/label. The
 * display steps are multiples of font_size_lg and the text steps are
 * font_size and font_size_sm, so all three keys are now load-bearing
 * and a theme can retune the whole scale without a recompile.
 *
 * Every slot falls back to font_sans, so a theme that names only the
 * old key still gets a working desktop -- one face, as before, but
 * nothing crashes and no surface loses its text. */
void shell_fonts_load(shell_fonts *f, const shell_ctx *c)
{
    const char *sans = theme_str(&c->theme, "font_sans", "");
    const char *disp = theme_str(&c->theme, "font_display", sans);
    const char *text = theme_str(&c->theme, "font_text", sans);
    const char *bold = theme_str(&c->theme, "font_text_bold", text);
    if (!disp || !*disp) disp = sans;
    if (!text || !*text) text = sans;
    if (!bold || !*bold) bold = text;

    /* Clamped before the derived defaults are computed from them:
     * (int)(base * 1.45f) is undefined once base is near INT_MAX, and a
     * 2000px UI font is a few hundred megabytes of glyph bitmaps. */
    float base = (float)theme_int_in(&c->theme, "font_size", 14, 6, 96);
    float sm   = (float)theme_int_in(&c->theme, "font_size_sm",
                                     (int)(base * 0.85f + 0.5f), 6, 96);
    float lg   = (float)theme_int_in(&c->theme, "font_size_lg",
                                     (int)(base * 1.45f + 0.5f), 6, 144);

    /* Her choice multiplies the theme's scale rather than replacing it,
     * so the RELATIONSHIPS the theme set up -- how much bigger a heading
     * is than a caption -- survive her making everything bigger. A
     * design whose hierarchy collapses at 150% was not a hierarchy. */
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;
    base *= k; sm *= k; lg *= k;

    f->huge  = open_font(disp, lg * 2.25f);
    f->big   = open_font(disp, lg * 1.45f);
    f->dmid  = open_font(disp, lg * 0.95f);
    f->mid   = open_font(bold, base * 1.10f);
    f->label = open_font(bold, sm);
    f->small = open_font(text, base);
    f->tiny  = open_font(text, sm);
    {
        double lh = theme_num(&c->theme, "line_height", 1.45);
        f->lh = (float)(lh < 0.8 ? 0.8 : lh > 3.0 ? 3.0 : lh);
    }
}
void shell_fonts_free(shell_fonts *f)
{
    if (f->huge) font_free(f->huge);
    if (f->big) font_free(f->big);
    if (f->dmid) font_free(f->dmid);
    if (f->mid) font_free(f->mid);
    if (f->label) font_free(f->label);
    if (f->small) font_free(f->small);
    if (f->tiny) font_free(f->tiny);
    memset(f, 0, sizeof *f);
}

/* ── starting things ────────────────────────────────────────────── */

/* snprintf with both arguments inside one shell_ctx warns about
 * overlap on every compiler that can see it, and the warning is fair.
 * A bounded copy says what is meant. */
static void copy_str(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* A window slot appears the instant the user clicks, before the
 * application has done anything at all. That is not a cosmetic choice:
 * a browser takes seconds to show its first frame on the hardware this
 * product exists to rescue, and a desktop that does nothing visible for
 * two seconds after a click has, to the person sitting there, ignored
 * them. The real window adopts this slot when it maps. */
int shell_launch(shell_ctx *c, int app)
{
    if (!c || app < 0 || app >= c->n_apps) return -1;

    /* Settings is a panel, not a program. Pressing it on a home screen
     * opened a card that said "Change how this computer looks and
     * works" and nothing else, for ever: a slot with nothing behind
     * it. It opens the panel the band's Settings button opens. */
    if (c->apps[app].role == APP_ROLE_SETTINGS) {
        /* Exactly one panel at a time, as foot_open_only() keeps it --
         * which this file cannot call, since every harness links it
         * without the band. */
        c->help_open = c->net_open = c->bt_open = c->power_open = 0;
        c->welcome_open = 0;
        c->settings_open = 1;
        return -1;
    }

    /* Already running and not adopted-away: raise rather than start a
     * second copy. Most applications are single-instance anyway and
     * would silently do nothing, which looks like a broken icon. */
    for (int i = 0; i < c->n_wins; i++)
        if (c->wins[i].app == app) {
            c->wins[i].minimised = 0;
            c->focus = i;
            return i;
        }

    if (c->n_wins >= SHELL_MAX_WINS) return -1;
    int i = c->n_wins++;
    win_entry *w = &c->wins[i];
    memset(w, 0, sizeof *w);
    w->app = app;
    w->starting = 1;
    copy_str(w->title,    sizeof w->title,    c->apps[app].name);
    copy_str(w->subtitle, sizeof w->subtitle, "Starting…");
    c->focus = i;

    if (c->spawn && c->apps[app].exec[0]) {
        if (c->spawn(c, c->apps[app].exec, c->apps[app].n_args) < 0) {
            copy_str(w->subtitle, sizeof w->subtitle, "Could not start");
            w->starting = 0;
        }
    } else if (!c->apps[app].exec[0]) {
        /* An entry with nothing behind it is the shell's own -- Settings
         * today. Saying so beats a slot that waits forever. */
        copy_str(w->subtitle, sizeof w->subtitle, c->apps[app].hint);
        w->starting = 0;
    }
    return i;
}

void shell_close_win(shell_ctx *c, int win)
{
    if (!c || win < 0 || win >= c->n_wins) return;
    /* Only the slot is removed here. A slot backed by a real window is
     * asked to close by the host, which owns the compositor; it
     * disappears from this list when the client actually goes, so a
     * document with unsaved changes still gets to object. */
    if (c->wins[win].wid) return;
    for (int i = win; i + 1 < c->n_wins; i++) c->wins[i] = c->wins[i + 1];
    c->n_wins--;
    if (c->focus >= c->n_wins) c->focus = c->n_wins - 1;
}

/* ── the store, on every home screen ─────────────────────────────── */

int shell_store_app(const shell_ctx *c)
{
    if (!c) return -1;
    for (int i = 0; i < c->n_apps; i++)
        if (c->apps[i].role == APP_ROLE_STORE) return i;
    return -1;
}

int shell_home_pick(const shell_ctx *c, int cap, int *out)
{
    if (!c || cap <= 0 || !out) return 0;
    int store = shell_store_app(c);
    int slots = cap - (store >= 0 ? 1 : 0);
    int n = 0;
    for (int i = 0; i < c->n_apps && n < slots; i++)
        if (i != store) out[n++] = i;

    /* The newest program she installed while the desktop was running,
     * if the table order did not already put it on this screen. It
     * takes the LAST of the ordinary places, so the first ones -- the
     * browser, her files -- never move under her hand. */
    int newest = -1;
    for (int i = 0; i < c->n_apps; i++)
        if (i != store && c->apps[i].fresh &&
            (newest < 0 || c->apps[i].fresh > c->apps[newest].fresh)) newest = i;
    if (newest >= 0 && slots > 0) {
        int there = 0;
        for (int k = 0; k < n; k++) if (out[k] == newest) there = 1;
        if (!there) {
            if (n < slots) out[n++] = newest;
            else out[n - 1] = newest;
        }
    }
    if (store >= 0) out[n++] = store;
    return n;
}

int shell_display_order(const shell_ctx *c, int first_cap, int *out)
{
    if (!c || !out) return 0;
    int n = shell_home_pick(c, first_cap, out);
    for (int i = 0; i < c->n_apps; i++) {
        int seen = 0;
        for (int k = 0; k < n; k++) if (out[k] == i) { seen = 1; break; }
        if (!seen) out[n++] = i;
    }
    return n;
}
