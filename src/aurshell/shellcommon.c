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

void shell_text(surface *s, font *f, float x, float y, const char *t, uint32_t c, float a)
{
    if (f && t && *t) font_draw(f, s->px, s->w, s->h, x, y, t, c, a);
}
void shell_text_centred(surface *s, font *f, float cx, float y, const char *t, uint32_t c, float a)
{
    if (f && t && *t) font_draw(f, s->px, s->w, s->h, cx - shell_text_w(f, t)/2.f, y, t, c, a);
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

void shell_icon_draw(surface *s, shell_icon ic, float cx, float cy, float sz,
                     uint32_t col, float a)
{
    float u = sz / 24.f;
    switch (ic) {
    case ICON_GLOBE:
        draw_circle(s, cx, cy, 10*u, col, a * 0.20f);
        for (float g = 0; g < 6.2832f; g += 0.06f)
            draw_circle(s, cx + cosf(g)*10*u, cy + sinf(g)*10*u, 1.1f*u, col, a);
        draw_line(s, cx-10*u, cy, cx+10*u, cy, 1.6f*u, col, a * 0.85f);
        for (float t = -1.f; t <= 1.f; t += 0.02f) {
            float y = t * 10*u, x = sqrtf(fmaxf(0.f, 1.f - t*t)) * 4.6f*u;
            draw_circle(s, cx - x, cy + y, 0.85f*u, col, a * 0.8f);
            draw_circle(s, cx + x, cy + y, 0.85f*u, col, a * 0.8f);
        }
        break;
    case ICON_MAIL: {
        rect e = { (int)(cx-11*u), (int)(cy-8*u), (int)(22*u), (int)(16*u) };
        draw_round_rect(s, e, corners_all(3*u), col, a * 0.20f);
        draw_round_rect_border(s, e, corners_all(3*u), 1.6f*u, col, a);
        draw_line(s, cx-9.5f*u, cy-6*u, cx, cy+1.5f*u, 1.6f*u, col, a);
        draw_line(s, cx+9.5f*u, cy-6*u, cx, cy+1.5f*u, 1.6f*u, col, a);
        break; }
    case ICON_PHOTOS: {
        rect f = { (int)(cx-11*u), (int)(cy-9*u), (int)(22*u), (int)(18*u) };
        draw_round_rect(s, f, corners_all(3*u), col, a * 0.20f);
        draw_round_rect_border(s, f, corners_all(3*u), 1.6f*u, col, a);
        draw_circle(s, cx-4.5f*u, cy-3.5f*u, 2.2f*u, col, a);
        for (int i = 0; i < (int)(8*u); i++) {
            float yy = cy + 7*u - (float)i, hw = (float)i * 1.35f;
            draw_line(s, cx + 1*u - hw, yy, cx + 1*u + hw, yy, 1.f, col, a * 0.9f);
        }
        break; }
    case ICON_FILES: {
        rect d = { (int)(cx-8*u), (int)(cy-11*u), (int)(16*u), (int)(22*u) };
        draw_round_rect(s, d, corners_all(2.5f*u), col, a * 0.20f);
        draw_round_rect_border(s, d, corners_all(2.5f*u), 1.6f*u, col, a);
        for (int i = 0; i < 3; i++)
            draw_line(s, cx-4.5f*u, cy-4*u + (float)i*5*u, cx+4.5f*u, cy-4*u + (float)i*5*u,
                      1.4f*u, col, a * 0.8f);
        break; }
    case ICON_SETTINGS:
        for (int i = 0; i < 8; i++) {
            float g = (float)i * 0.7854f;
            draw_circle(s, cx + cosf(g)*9.5f*u, cy + sinf(g)*9.5f*u, 2.1f*u, col, a);
        }
        draw_circle(s, cx, cy, 7.f*u, col, a * 0.30f);
        for (float g = 0; g < 6.2832f; g += 0.08f)
            draw_circle(s, cx + cosf(g)*7*u, cy + sinf(g)*7*u, 1.1f*u, col, a);
        draw_circle(s, cx, cy, 2.6f*u, col, a);
        break;
    case ICON_HELP:
        draw_circle(s, cx, cy, 11*u, col, a * 0.20f);
        for (float g = 0; g < 6.2832f; g += 0.055f)
            draw_circle(s, cx + cosf(g)*11*u, cy + sinf(g)*11*u, 1.1f*u, col, a);
        for (float g = 2.5f; g > -1.5f; g -= 0.06f)
            draw_circle(s, cx + cosf(g)*4.2f*u, cy - 3.2f*u + sinf(g)*4.2f*u, 1.3f*u, col, a);
        draw_line(s, cx + 0.4f*u, cy + 0.2f*u, cx + 0.4f*u, cy + 3.4f*u, 2.4f*u, col, a);
        draw_circle(s, cx + 0.4f*u, cy + 7.2f*u, 1.5f*u, col, a);
        break;
    case ICON_WINDOW: {
        rect w = { (int)(cx-11*u), (int)(cy-8*u), (int)(22*u), (int)(16*u) };
        draw_round_rect(s, w, corners_all(3*u), col, a * 0.20f);
        draw_round_rect_border(s, w, corners_all(3*u), 1.6f*u, col, a);
        draw_line(s, cx-11*u, cy-3.2f*u, cx+11*u, cy-3.2f*u, 1.4f*u, col, a * 0.8f);
        break; }
    case ICON_PLUS:
        draw_round_rect(s, (rect){ (int)(cx-9*u), (int)(cy-1.6f*u), (int)(18*u), (int)(3.2f*u) },
                        corners_all(1.6f*u), col, a);
        draw_round_rect(s, (rect){ (int)(cx-1.6f*u), (int)(cy-9*u), (int)(3.2f*u), (int)(18*u) },
                        corners_all(1.6f*u), col, a);
        break;
    case ICON_TEXT: {
        rect d = { (int)(cx-9*u), (int)(cy-11*u), (int)(18*u), (int)(22*u) };
        draw_round_rect(s, d, corners_all(2.5f*u), col, a * 0.20f);
        draw_round_rect_border(s, d, corners_all(2.5f*u), 1.6f*u, col, a);
        for (int i = 0; i < 4; i++) {
            float w = (i == 3) ? 5.f*u : 10.f*u;
            draw_line(s, cx-5*u, cy-6*u + (float)i*4*u, cx-5*u + w, cy-6*u + (float)i*4*u,
                      1.3f*u, col, a * 0.8f);
        }
        break; }
    case ICON_MUSIC:
        draw_circle(s, cx-5*u, cy+7*u, 3.4f*u, col, a);
        draw_circle(s, cx+6*u, cy+5*u, 3.4f*u, col, a);
        draw_line(s, cx-5*u+3.2f*u, cy+7*u, cx-5*u+3.2f*u, cy-9*u, 1.8f*u, col, a);
        draw_line(s, cx+6*u+3.2f*u, cy+5*u, cx+6*u+3.2f*u, cy-11*u, 1.8f*u, col, a);
        draw_line(s, cx-1.8f*u, cy-9*u, cx+9.2f*u, cy-11*u, 1.8f*u, col, a);
        break;
    case ICON_TERMINAL: {
        rect w = { (int)(cx-11*u), (int)(cy-9*u), (int)(22*u), (int)(18*u) };
        draw_round_rect(s, w, corners_all(3*u), col, a * 0.20f);
        draw_round_rect_border(s, w, corners_all(3*u), 1.6f*u, col, a);
        draw_line(s, cx-6*u, cy-3*u, cx-2*u, cy+0.5f*u, 1.7f*u, col, a);
        draw_line(s, cx-6*u, cy+4*u, cx-2*u, cy+0.5f*u, 1.7f*u, col, a);
        draw_line(s, cx+0.5f*u, cy+4.5f*u, cx+6.5f*u, cy+4.5f*u, 1.7f*u, col, a);
        break; }
    case ICON_CALC: {
        rect w = { (int)(cx-9*u), (int)(cy-11*u), (int)(18*u), (int)(22*u) };
        draw_round_rect(s, w, corners_all(2.5f*u), col, a * 0.20f);
        draw_round_rect_border(s, w, corners_all(2.5f*u), 1.6f*u, col, a);
        draw_round_rect(s, (rect){ (int)(cx-5.5f*u), (int)(cy-8*u), (int)(11*u), (int)(4*u) },
                        corners_all(1*u), col, a * 0.65f);
        for (int r = 0; r < 2; r++) for (int cc = 0; cc < 3; cc++)
            draw_circle(s, cx-4*u + (float)cc*4*u, cy + 0.5f*u + (float)r*5*u, 1.4f*u, col, a);
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
    const uint32_t tint[3] = { c->accent, c->accent_alt, c->accent_warm };

    c->n_apps = (int)(sizeof A / sizeof A[0]);
    if (c->n_apps > SHELL_MAX_APPS) c->n_apps = SHELL_MAX_APPS;
    for (int i = 0; i < c->n_apps; i++) {
        snprintf(c->apps[i].id,   sizeof c->apps[i].id,   "%s", A[i].id);
        snprintf(c->apps[i].name, sizeof c->apps[i].name, "%s", A[i].name);
        snprintf(c->apps[i].hint, sizeof c->apps[i].hint, "%s", A[i].hint);
        c->apps[i].icon   = A[i].ic;
        c->apps[i].pinned = A[i].pin;
        /* Settings is machinery, not one of the user's things, so it
         * takes the quiet colour rather than a place in the cycle. */
        c->apps[i].tint = (A[i].ic == ICON_SETTINGS) ? c->subtle : tint[i % 3];
    }
}

void shell_theme_load(shell_ctx *c, const theme_t *t)
{
    c->theme = *t;
    c->radius    = theme_int(t, "radius", 14);
    c->radius_sm = theme_int(t, "radius_sm", 8);
    c->margin    = theme_int(t, "margin", 14);
    c->padding   = theme_int(t, "padding", 14);
    c->border    = theme_int(t, "border", 2);
    c->bar_h     = theme_int(t, "bar_height", 38);
    c->blur_r    = theme_int(t, "blur_radius", 20);
    c->shadow_r  = theme_int(t, "shadow_radius", 28);
    c->panel_a   = (float)theme_num(t, "opacity_panel", 0.88);
    c->shadow_a  = (float)theme_num(t, "shadow_opacity", 0.50);
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
        if (c->spawn(c, c->apps[app].exec) < 0) {
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
