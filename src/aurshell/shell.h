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
/* More than any real Exec= line, and a bound on what a hostile one can
 * make us allocate. */
#define APP_MAX_ARGS   16
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
    /* The argument vector that starts it: NUL-separated tokens, ending
     * in a second NUL, with n_args of them. NOT a shell command line --
     * a .desktop Exec= is parsed per the XDG quoting rules and handed
     * to execv, so a semicolon or a backtick in a file the user can
     * write is an argument rather than an instruction.
     *
     * An empty first token means there is nothing behind this icon,
     * which today is true only of the shell's own Settings entry. */
    char        exec[192];
    int         n_args;
    char        wm_class[64];  /* app_id a window reports; links the two */
} app_entry;

typedef struct {
    char      title[64];
    char      subtitle[96];
    int       app;             /* index into ctx->apps, or -1        */
    surface  *content;         /* live view; NULL draws a placeholder */
    int       minimised;
    rect      geom;            /* used by overlapping and tiled models */

    /* The compositor window this entry stands for, or 0 while the
     * application is still starting. A slot is created the instant the
     * user clicks, because two seconds of nothing happening reads as a
     * broken machine; the real window adopts the slot when it maps. */
    uint32_t  wid;
    int       starting;
    uint32_t  start_ms;        /* when the wait began; 0 = not yet set */
} win_entry;

/* Two faces, not one, at two real weights.
 *
 * The old version of this struct was four SIZES of one file, which is
 * why the desktop had no typographic hierarchy: size alone is a weak
 * signal, and a 26px and a 19px cut of the same regular sans read as
 * the same voice slightly louder. Here `huge`/`big`/`dmid` come from a
 * bold DISPLAY serif and `mid`/`small`/`tiny`/`label` from a
 * caption-optimised sans, `mid` and `label` being its BOLD cut. The
 * contrast between a 42px bold serif and an 11px regular sans is the
 * hierarchy; nothing needs a colour or a glow to be important.
 *
 * `lh` is the theme's line_height, carried here so that block spacing
 * is a theme decision rather than a constant in six layouts. */
typedef struct {
    font *huge, *big, *dmid;      /* display face: headlines only     */
    font *mid, *label;            /* text face, BOLD: names, rubrics  */
    font *small, *tiny;           /* text face, regular: body, hints  */
    float lh;
} shell_fonts;

typedef struct shell_ctx_s {
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
    /* An empty string means every installed application. A non-empty
     * one is an allow-list of desktop-file names, window classes or
     * commands, and is enforced when the app table is built rather than
     * when an icon is pressed -- see shell_scan_apps(). */
    char  allowed_apps[256];
    /* The other direction: everything EXCEPT these. An organisation
     * usually wants a deny-list, not an allow-list, and this was
     * written into /etc/auros/policy.conf by the build and read by
     * nothing at all -- a knob that looked like a control and was not. */
    char  blocked_apps[256];
    /* Set when the policy file exists but could not be trusted. The
     * allow-list is then not "empty" -- which would mean everything --
     * but "nothing", which is the only safe reading of a lockdown file
     * we failed to parse. */
    int   deny_all_apps;

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

    /* ── the band that is always there (src/aurshell/foot.c) ────── */
    /* Her chosen text size, as a multiplier over the theme's sizes.
     * Lives in her own settings, not the system theme, so changing how
     * big the words are needs nothing privileged. */
    float text_scale;
    int   text_changed;        /* she just changed it; reload the fonts */
    int   help_open;
    int   foot_hover;          /* which button, -1 for none             */
    int   no_foot;             /* a profile turned the band off         */
    int   want_power_off;      /* she pressed Turn off                  */

    /* ── the running system ─────────────────────────────────────── */
    /* The Wayland server applications connect to, or NULL. It is NULL
     * in the preview renderer, the contact sheet and the hit-test
     * harness -- which is why every use of it is guarded rather than
     * assumed, and why a layout asks shell_launch() rather than
     * reaching for it. */
    struct aurwl *wl;
    /* How the host actually starts a program. A function pointer rather
     * than a direct call so that shellcommon.c -- which every preview
     * and test harness links -- does not drag in the compositor. NULL
     * means nothing starts, which is exactly right for a still render. */
    int (*spawn)(struct shell_ctx_s *c, const char *argv_blob, int n_args);

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
    /* A window just appeared, or the user asked for this one. Bring it
     * to where the user is looking -- which in a carousel means
     * scrolling to it and in a stack means raising it, and is why this
     * cannot be done by setting c->focus from outside. Optional; the
     * default is to set c->focus and let the archetype read it. */
    void (*present)(shell_ctx *c, int win);
    /* A window is about to be removed from c->wins, and everything
     * above it will shift down by one. An archetype that keeps its own
     * arrays indexed by slot number must shift them here -- which
     * workspace each window is on, which are maximised, the stacking
     * order. Optional, and only for archetypes that keep such arrays.
     *
     * Called for the path a real application exits through, which is
     * the one that actually happens; an archetype's own close button
     * does its bookkeeping inline. */
    void (*removed)(shell_ctx *c, int win);
} shell_layout;

/* Shared helpers every layout may use, so six renderers do not each
 * grow their own slightly different icon set or clock. */
/* A mark, not an icon in the app-store sense: a solid silhouette in
 * one ink with its interior detail KNOCKED OUT in the paper colour
 * underneath, the way a woodcut or a signage pictogram works.
 *
 * `paper` is therefore not decoration — it is the colour the caller
 * has just filled behind the mark, and the mark cuts holes in itself
 * with it. Passing the wrong one shows, which is the point: a mark
 * that does not know what it is sitting on cannot be solid.
 *
 * The version this replaces drew every one of the twelve as the same
 * rounded box at the same 1.6px stroke with the same 20% tint fill,
 * so ICON_MAIL, ICON_PHOTOS, ICON_WINDOW and ICON_TERMINAL were
 * literally the same box with different lines in it. These have
 * deliberately different visual weights — the globe is the heaviest
 * thing in the set, settings the lightest — because a row of marks
 * that all weigh the same is a row with no rhythm. */
void shell_icon_draw(surface *s, shell_icon ic, float cx, float cy,
                     float size, uint32_t ink, uint32_t paper, float a);
void shell_text(surface *s, font *f, float x, float y_baseline,
                const char *t, uint32_t col, float a);
void shell_text_centred(surface *s, font *f, float cx, float y_baseline,
                        const char *t, uint32_t col, float a);
float shell_text_w(font *f, const char *t);
/* Text that stops at a width, ending in an ellipsis if it had to.
 * Application names come from packages and are as long as they like. */
void shell_text_elided(surface *s, font *f, float x, float y, float max_w,
                       const char *t, uint32_t col, float a);
/* Letter-spaced text, for the small rubrics that label a region the
 * way a folio labels a page. Drawn a codepoint at a time, so kerning
 * is deliberately dropped: tracked capitals do not want it. */
void shell_text_tracked(surface *s, font *f, float x, float y_baseline,
                        const char *t, uint32_t col, float a, float track);
float shell_text_tracked_w(font *f, const char *t, float track);
/* Baseline that vertically centres text in a band of height h at y. */
float shell_baseline(font *f, float y, float h);
void shell_clock(char *hm, size_t hm_n, char *date, size_t date_n);

void shell_theme_load(shell_ctx *c, const theme_t *t);

/* Open every face the archetypes expect, at the sizes the theme asks
 * for, with fallbacks. ONE implementation: a harness that fills this
 * struct by hand fills it wrongly the moment a face is added, and then
 * renders a desktop that does not exist while reporting success. */
void shell_fonts_load(shell_fonts *f, const shell_ctx *c);
void shell_fonts_free(shell_fonts *f);
/* The starter app set. Call AFTER shell_theme_load(): tints come from
 * the theme's accents, because nothing may hardcode a colour. */
void shell_seed_apps(shell_ctx *c);
/* Loads a .shell archetype file into the ctx. Returns 0 on success. */
int  shell_archetype_load(shell_ctx *c, const char *path);

/* Replaces the seeded app table with what is actually installed, read
 * from XDG desktop entries. This is what makes "install an application
 * and it appears" true without the shell knowing anything about the
 * application: a .deb drops a .desktop file, and the next scan finds
 * it. Returns the number of apps found, or -1 if nothing was readable
 * (in which case the caller keeps the seeded table). */
int  shell_scan_apps(shell_ctx *c);

/* Start an application and give the user something to look at while it
 * loads. Safe to call with no compositor; it then only marks the slot,
 * which is what the preview renderers want. Returns the window index,
 * or -1. */
int  shell_launch(shell_ctx *c, int app);

/* Ask a window to close, the way its own title bar button would. */
void shell_close_win(shell_ctx *c, int win);

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
