/* shell.h — the contract every AurOS shell archetype implements.
 *
 * An archetype answers exactly one question: when you want to reach a
 * different thing, what do you do? Colours are the theme; what the user
 * is allowed to do is the policy; this is only the interaction model.
 *
 * Because all archetypes share this context, building a distribution
 * with a different feel is choosing a .shell file — not writing code.
 * That is the whole point of the split.
 */
#ifndef AUROS_SHELL_H
#define AUROS_SHELL_H

#include <stdint.h>
#include <stddef.h>
#include "draw.h"
#include "anim.h"
#include "../common/theme.h"
#include "../common/font.h"

#define SHELL_MAX_APPS 32
#define SHELL_MAX_WINS 32

/* Icons are drawn from primitives, so they inherit theme colours, scale
 * to any size without assets, and add nothing to the image. */
typedef enum {
    ICON_GLOBE, ICON_MAIL, ICON_PHOTOS, ICON_FILES,
    ICON_SETTINGS, ICON_HELP, ICON_WINDOW, ICON_PLUS,
    ICON_TEXT, ICON_MUSIC, ICON_TERMINAL, ICON_CALC
} shell_icon;

typedef struct {
    char        id[32];
    char        name[40];      /* the word the USER would use */
    char        hint[56];      /* one line of plain language   */
    shell_icon  icon;
    uint32_t    tint;
    int         pinned;        /* appears in a dock / favourites strip */
} app_entry;

typedef struct {
    char      title[64];
    char      subtitle[96];
    int       app;             /* index into ctx->apps, or -1        */
    surface  *content;         /* live view; NULL draws a placeholder */
    int       minimised;
    rect      geom;            /* used by overlapping and tiled models */
} win_entry;

typedef struct { font *big, *mid, *small, *huge; } shell_fonts;

typedef struct {
    /* ── theme, resolved once ───────────────────────────────────── */
    int      radius, radius_sm, margin, padding, border;
    int      bar_h, blur_r, shadow_r;
    float    panel_a, shadow_a;
    uint32_t bg, bg_alt, surface_c, surface_hi, overlay;
    uint32_t muted, subtle, fg, fg_hi;
    uint32_t accent, accent_alt, accent_warm, err, ok, info;
    char     brand[48];
    theme_t  theme;

    /* ── archetype parameters, from the .shell file ─────────────── */
    char  layout_id[24];
    char  shell_name[48];
    int   target_large;        /* bigger targets and type            */
    int   status_full;         /* the strip launches and switches    */
    int   show_clock, show_positions;
    int   workspaces;

    /* Locked-archetype parameters. They live in the shared context
     * rather than as constants in the layout so that an administrator
     * changes behaviour by editing a .shell file, which is the entire
     * point of splitting archetype from code. */
    int   locked_autostart;       /* 1 = the first allowed app is showing  */
    int   locked_show_switcher;   /* 0 auto (>1 app), 1 always, 2 never    */
    int   locked_exit_combo;      /* 0 none, 1 an admin escape exists      */

    /* ── policy, from the profile ───────────────────────────────── */
    int   allow_install, allow_settings, allow_theme_change, kiosk;
    /* allow_tty is not cosmetic. Masking getty units does not stop a
     * VT switch -- the kernel's VT layer handles the chord and needs no
     * cooperation from userspace -- so the shell has to refuse the
     * switch itself. See console_release() in main.c. */
    int   allow_tty;

    /* ── content ────────────────────────────────────────────────── */
    app_entry apps[SHELL_MAX_APPS]; int n_apps;
    win_entry wins[SHELL_MAX_WINS]; int n_wins;
    int   focus;               /* index into wins, -1 for none/home  */
    int   workspace;

    /* ── the display ────────────────────────────────────────────── */
    /* Set once by the host before init(), and it is what hit-testing
     * must use. A layout that paints with the surface's size but
     * hit-tests against a constant is off by however much the two
     * differ -- which on the 1366x768 panels this product exists to
     * rescue is most of the screen. docs/SHELLS.md states the rule:
     * one function owns geometry, called by painting AND hit-testing. */
    int   screen_w, screen_h;

    /* ── input, updated before paint ────────────────────────────── */
    /* mouse_down is set BEFORE click() is dispatched, and click() is
     * dispatched on PRESS. Layouts start drags there and end them in
     * motion() when mouse_down goes false, so the order matters. */
    int   mouse_x, mouse_y, mouse_down;
    int   hover;               /* layout-defined hot item, -1 none   */

    /* ── per-layout scratch. Layouts own this; nothing else reads. */
    void *priv;
} shell_ctx;

typedef struct {
    const char *id;
    void (*init) (shell_ctx *c);
    void (*paint)(shell_ctx *c, surface *s, shell_fonts *f, const surface *wall);
    /* Return 1 if the click was consumed. Coordinates are screen px. */
    int  (*click)(shell_ctx *c, int x, int y);
    void (*motion)(shell_ctx *c, int x, int y);
    void (*key)  (shell_ctx *c, int keycode);
    int  (*step) (shell_ctx *c, float dt);   /* 1 while animating */
    void (*fini) (shell_ctx *c);
} shell_layout;

/* Shared helpers every layout may use, so six renderers do not each
 * grow their own slightly different icon set or clock. */
void shell_icon_draw(surface *s, shell_icon ic, float cx, float cy,
                     float size, uint32_t col, float a);
void shell_text(surface *s, font *f, float x, float y_baseline,
                const char *t, uint32_t col, float a);
void shell_text_centred(surface *s, font *f, float cx, float y_baseline,
                        const char *t, uint32_t col, float a);
float shell_text_w(font *f, const char *t);
/* Baseline that vertically centres text in a band of height h at y. */
float shell_baseline(font *f, float y, float h);
void shell_clock(char *hm, size_t hm_n, char *date, size_t date_n);

void shell_theme_load(shell_ctx *c, const theme_t *t);
/* The starter app set. Call AFTER shell_theme_load(): tints come from
 * the theme's accents, because nothing may hardcode a colour. */
void shell_seed_apps(shell_ctx *c);
/* Loads a .shell archetype file into the ctx. Returns 0 on success. */
int  shell_archetype_load(shell_ctx *c, const char *path);

/* Registry. Each layout lives in its own translation unit and exposes
 * exactly one of these, so six of them can be written independently
 * without touching a shared file. */
extern const shell_layout layout_rail;
extern const shell_layout layout_tiles;
extern const shell_layout layout_locked;
extern const shell_layout layout_taskbar;
extern const shell_layout layout_dock;
extern const shell_layout layout_workbench;

const shell_layout *shell_layout_by_id(const char *id);

#endif
