/* ═══════════════════════════════════════════════════════════════════
 *  AurBridge wizard — the face of the only component of AurOS that can
 *  destroy a stranger's data.
 *
 *  Everything here is owner-drawn: there is not a single Win32 control
 *  in this program. A grey system button in the middle of the Nocturne
 *  palette reads as "unfinished software", and a user who does not trust
 *  the installer is a user who clicks through the disclosure without
 *  reading it. The look is load-bearing.
 *
 *  THIS BUILD PERFORMS NO DESTRUCTIVE ACTION.
 *  Phase execution is stubbed. Every stub is named stub_* and marked
 *  only logs what the real phase would do. There is no code in this
 *  file that opens a disk for writing, and none should be added here —
 *  the phase engine lands in its own file, with its own tests.
 *
 *  Safety invariants enforced in code, not just in pixels:
 *    - nav_allowed() refuses every page from the backup gate onward
 *      unless preflight has completed with zero PF_BLOCK results. The
 *      BLOCKED page has no continue affordance, and even if one were
 *      added by accident, nav_allowed() would refuse it.
 *    - "Start installing" re-runs preflight before the phase list is
 *      touched (AURBRIDGE.md: destructive subcommands re-run preflight
 *      and abort on any block).
 *
 *  Build:
 *    x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -std=gnu11 -mwindows \
 *      -o aurbridge-wizard.exe wizard.c preflight.c -lgdi32 -lcomctl32
 * ═══════════════════════════════════════════════════════════════════ */

/* Vista+ locale APIs (GetLocaleInfoEx); everything newer than that is
 * reached through GetProcAddress so the binary still loads on Windows 7. */
#ifndef WINVER
#define WINVER       0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

#include "preflight.h"

/* ── Palette: themes/nocturne.theme, copied exactly ───────────────── */
#define C_BG          0x0B0E14u   /* bg          */
#define C_BG_ALT      0x10151Fu   /* bg_alt      */
#define C_SURFACE     0x161C28u   /* surface     */
#define C_SURFACE_HI  0x1F2735u   /* surface_hi  */
#define C_OVERLAY     0x2B3542u   /* overlay     */
#define C_MUTED       0x55606Eu   /* muted       */
#define C_SUBTLE      0x8793A4u   /* subtle      */
#define C_FG          0xD4DCEAu   /* fg          */
#define C_FG_HI       0xF3F7FDu   /* fg_hi       */
#define C_ACCENT      0x7DD3C0u   /* accent      */
#define C_ACCENT_ALT  0xA78BFAu   /* accent_alt  */
#define C_WARM        0xF2B880u   /* accent_warm */
#define C_OK          0x7DD3C0u   /* ok          */
#define C_WARN        0xF2B880u   /* warn        */
#define C_ERR         0xF2788Du   /* err         */
#define C_INFO        0x82AAFFu   /* info        */

/* Channel extraction, so the backdrop can be DERIVED from the palette
 * above instead of restating it. It used to hardcode the accent bytes
 * inline -- 0x7D/0xD3/0xC0 and 0xA7/0x8B/0xFA -- which meant the single
 * largest area of colour in the installer ignored the palette block
 * entirely, and editing that block to reskin the product changed
 * everything except the thing you were looking at. */
#define CH_R(c) ((float)(((c) >> 16) & 0xFFu))
#define CH_G(c) ((float)(((c) >>  8) & 0xFFu))
#define CH_B(c) ((float)( (c)        & 0xFFu))

#define CR(c) RGB(((c)>>16)&0xFF, ((c)>>8)&0xFF, (c)&0xFF)

/* Theme picker data, transcribed from the theme files in themes/. The installed
 * system reads the .theme files themselves; the installer cannot, since
 * it runs on Windows before AurOS exists. Keep these in step with
 * themes/<name>.theme — identity block + core palette + accents. */
typedef struct {
    const wchar_t *name;
    const wchar_t *desc;
    uint32_t bg, surface, accent, accent_alt;
    int dark;
} theme_info;

static const theme_info THEMES[] = {
  { L"Nocturne",  L"Deep atmospheric blue-black with an aurora-teal glow.",
    0x0B0E14, 0x161C28, 0x7DD3C0, 0xA78BFA, 1 },
  { L"Moss",      L"A quiet forest floor — desaturated greens and warm stone.",
    0x0E1310, 0x1A221C, 0x8FBF7F, 0xC9A86C, 1 },
  { L"Sandstone", L"Warm paper and ink with a terracotta accent.",
    0xF4F0E8, 0xFFFFFF, 0xC2683D, 0x4E7A6B, 0 },
  { L"Synthwave", L"Neon grid, hot magenta and cyan over a violet horizon.",
    0x160A26, 0x26123F, 0xFF4FD8, 0x4FE9FF, 1 },
};
#define N_THEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

/* ── The six desktop archetypes ───────────────────────────────────────
 * Transcribed from shells/<id>.shell, the same way THEMES above is
 * transcribed from themes/<id>.theme: the installer runs on Windows,
 * before AurOS exists, so it cannot read the real files. Keep in step
 * with shell_name / shell_best_for / shell_tradeoff.
 *
 * `id` is what a later phase writes as shell_archetype into the image's
 * profile; nothing else in this file interprets it.
 *
 * Rules this table obeys, from docs/SHELLS.md:
 *   - Every archetype states its tradeoff. "A menu that only lists
 *     upsides is useless for choosing", so `tradeoff` is never empty and
 *     is drawn at the same size and weight as `best_for`.
 *   - No other operating system, desktop or device brand is named
 *     anywhere. Describe the behaviour instead: "a bar along the bottom
 *     listing everything you have open".
 *   - Workbench is the only one allowed to require learning, and says so
 *     in those words so nobody picks it by accident.
 * Order is by how likely each is to be the right answer for the person
 * this installer exists for; the one that demands study comes last. */
enum { SD_RAIL = 0, SD_TASKBAR, SD_TILES, SD_DOCK, SD_LOCKED, SD_WORKBENCH };

typedef struct {
    const char    *id;          /* shell_archetype, for the profile      */
    const wchar_t *name;        /* shell_name — the plain-language name  */
    const wchar_t *what;        /* what it does, in one or two lines     */
    const wchar_t *best_for;    /* shell_best_for                        */
    const wchar_t *tradeoff;    /* shell_tradeoff — never omitted        */
    const wchar_t *badge;       /* NULL, or a word at the top right      */
    int            diagram;     /* SD_*: which shape to draw             */
    int            warn_tint;   /* draw in C_WARM instead of C_ACCENT    */
} shell_info;

static const shell_info SHELLS[] = {
  { "rail", L"Everything in a row",
    L"Everything you have open sits side by side in a row. Nothing is ever "
    L"hidden: what is beside you shows at the edge of the screen, and you "
    L"click it to go there.",
    L"Someone who loses windows, or has never been comfortable with a computer.",
    L"You see fewer things at once than a normal desktop, and you cannot put "
    L"two windows side by side.",
    L"RECOMMENDED", SD_RAIL, 0 },

  { "taskbar", L"The familiar one",
    L"A bar along the bottom lists everything you have open, so you click a "
    L"button to come back to something. Your windows can overlap, and can be "
    L"moved and resized.",
    L"Someone who has used a computer for years and would rather not learn "
    L"anything new.",
    L"Your windows end up on top of each other and get lost behind one "
    L"another — the oldest complaint in computing.",
    NULL, SD_TASKBAR, 0 },

  { "tiles", L"One thing at a time",
    L"You start on a page of large labelled buttons. Press one and it takes "
    L"over the whole screen. A single Home button brings the page back.",
    L"Anyone who already uses a phone or a tablet and wants this to behave the "
    L"same way.",
    L"You cannot see two things at once, and switching means going back via "
    L"Home every time.",
    NULL, SD_TILES, 0 },

  { "dock", L"Favourites along the edge",
    L"A strip of your most-used programs sits at the bottom, always in the "
    L"same order, whether they are running or not. Anything else you find by "
    L"typing its name.",
    L"Someone who uses the same handful of programs constantly and wants them "
    L"in one fixed place.",
    L"Programs that are open but not in your favourites are harder to find "
    L"again, and typing to search is a habit some people never form.",
    NULL, SD_DOCK, 0 },

  { "locked", L"Just these apps",
    L"The computer does the handful of jobs it was set up for and nothing "
    L"else. No desktop, no settings, and no way to install anything.",
    L"Schools, libraries and reception desks — or a relative who must not be "
    L"able to break it.",
    L"The person using it cannot change anything at all. That is the entire "
    L"point, and it will frustrate anyone who wants more.",
    NULL, SD_LOCKED, 0 },

  { "workbench", L"Panes and keyboard",
    L"Your windows never overlap: they divide the screen between them "
    L"automatically, so everything open is visible at once. You move around "
    L"with the keyboard.",
    L"Someone who will spend an afternoon learning it, and then all day in it.",
    L"It expects you to learn keyboard shortcuts. It will feel hostile on day "
    L"one, and it is a poor choice for anyone who wanted the computer to be "
    L"simpler.",
    L"REQUIRES LEARNING", SD_WORKBENCH, 1 },
};
#define N_SHELLS ((int)(sizeof SHELLS / sizeof SHELLS[0]))

/* Rail is index 0 and the default, because it is the only archetype in
 * which a thing cannot be hidden. Changing this changes the default the
 * chooser opens on; it is not merely the first row. */
#define SHELL_DEFAULT 0

/* ── Pages ────────────────────────────────────────────────────────── */
typedef enum {
    PAGE_WELCOME = 0,
    PAGE_CHECKING,
    PAGE_BLOCKED,
    PAGE_BACKUP,
    PAGE_CONSENT,
    PAGE_CHOOSE,
    PAGE_DESKTOP,
    PAGE_PERSONALIZE,
    PAGE_READY,
    PAGE_PROGRESS,
    PAGE_COUNT
} page_id;

static const struct { const wchar_t *label; page_id first; } RAIL[] = {
    { L"Welcome",           PAGE_WELCOME     },
    { L"Check this PC",     PAGE_CHECKING    },
    { L"Before we start",   PAGE_BACKUP      },
    { L"What will happen",  PAGE_CONSENT     },
    { L"Your choice",       PAGE_CHOOSE      },
    { L"How it works",      PAGE_DESKTOP     },
    { L"Make it yours",     PAGE_PERSONALIZE },
    { L"Ready",             PAGE_READY       },
    { L"Installing",        PAGE_PROGRESS    },
};
#define N_RAIL ((int)(sizeof RAIL / sizeof RAIL[0]))

static int rail_index_for(page_id p)
{
    if (p == PAGE_BLOCKED) return 1;            /* blocked belongs to "check" */
    for (int i = N_RAIL - 1; i >= 0; i--)
        if (p >= RAIL[i].first) return i;
    return 0;
}

/* ── Global UI state ──────────────────────────────────────────────── */
static HWND     g_hwnd;
static int      g_dpi = 96;
static int      g_cw, g_ch;            /* client size, device px         */
static page_id  g_page = PAGE_WELCOME;

/* back-buffer: a 32bpp top-down DIB we both rasterise into by hand and
 * let GDI draw ClearType text into. */
static HDC      g_mdc;
static HBITMAP  g_mbmp, g_moldbmp;
static uint32_t *g_px;
static int      g_mw, g_mh;

static HFONT g_f_title, g_f_h2, g_f_h3, g_f_body, g_f_bodyb, g_f_small,
             g_f_smallb, g_f_tiny, g_f_input;

static RECT  g_clip;                   /* clip for hand-rasterised pixels */

/* input */
static POINT g_mouse = { -1, -1 };
static int   g_mouse_down = 0;
static int   g_press_idx = -1;
static int   g_focus = -1;             /* index into g_w[]                */
static int   g_focus_ring = 0;         /* draw rings only after Tab/arrow */
static int   g_caret_on = 1;

/* scrolling */
static int   g_scroll[PAGE_COUNT];
static int   g_content_h, g_view_h;    /* measured during paint           */

/* preflight */
static pf_report    g_report;
static volatile LONG g_pf_state;       /* 0 idle · 1 running · 2 finished  */
static int          g_pf_valid;        /* g_report holds a real result     */
static int          g_reveal;          /* checklist rows revealed          */
static int          g_ticks;           /* animation clock (60 ms)          */
static page_id      g_check_next = PAGE_BACKUP;   /* where CHECKING goes   */

/* user answers */
static int   g_ack_backup, g_ack_usb;
static wchar_t g_agree[24];
static int   g_choice = 0;             /* 0 = dual boot · 1 = replace      */
static int   g_ack_replace;
static int   g_ready_confirm;
static int   g_sel_lang = 0, g_sel_kbd = 0, g_sel_tz = 0, g_sel_theme = 0;
/* The desktop archetype, alongside the other personalize answers. Its
 * SHELLS[].id is what a later phase writes as shell_archetype into the
 * image's profile — see stub_phase_write(). */
static int   g_sel_shell = SHELL_DEFAULT;

/* detected-from-Windows defaults, filled in at startup */
static wchar_t g_det_lang[96], g_det_kbd[96], g_det_tz[128];

/* dev harness only; never set by anything the user can click */
static int   g_shot_mode = 0;

#define AGREE_WORD L"AGREE"

/* ═══════════════════════════════════════════════════════════════════
 *  Small helpers
 * ═══════════════════════════════════════════════════════════════════ */
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void a2w(const char *a, wchar_t *w, int cch)
{
    if (!MultiByteToWideChar(CP_ACP, 0, a, -1, w, cch)) { w[0] = 0; }
}

static void human_size(uint64_t b, wchar_t *out, int cch)
{
    static const wchar_t *u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    _snwprintf(out, (size_t)cch, L"%.0f %s", v, u[i]);
    out[cch - 1] = 0;
}

static float clampf(float v, float lo, float hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

/* ═══════════════════════════════════════════════════════════════════
 *  Hand-rasterised, anti-aliased primitives.
 *
 *  GDI cannot anti-alias a rounded rectangle, and RoundRect's stair-step
 *  corners are the single most obvious "this is a 1998 dialog" tell. We
 *  own the DIB bits, so shapes are drawn with a signed-distance field and
 *  per-pixel coverage; text is still GDI/ClearType on the same surface.
 *
 *  GdiFlush() before touching bits: GDI batches, and a batched TextOut
 *  landing after our writes would otherwise appear on top of them.
 * ═══════════════════════════════════════════════════════════════════ */
static inline void blend_px(int x, int y, uint32_t col, float a)
{
    if (a <= 0.003f) return;
    if (x < g_clip.left || x >= g_clip.right ||
        y < g_clip.top  || y >= g_clip.bottom) return;
    uint32_t *p = &g_px[(size_t)y * (size_t)g_mw + (size_t)x];
    if (a >= 0.997f) { *p = col; return; }
    uint32_t d = *p;
    int ia = (int)(a * 256.0f);
    int r = (int)((d >> 16) & 0xFF), g = (int)((d >> 8) & 0xFF), b = (int)(d & 0xFF);
    r += ((int)((col >> 16) & 0xFF) - r) * ia >> 8;
    g += ((int)((col >>  8) & 0xFF) - g) * ia >> 8;
    b += ((int)( col        & 0xFF) - b) * ia >> 8;
    *p = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* signed distance to a rounded rectangle; negative inside */
static inline float sd_rr(float px, float py, float cx, float cy,
                          float hw, float hh, float r)
{
    float dx = fabsf(px - cx) - (hw - r);
    float dy = fabsf(py - cy) - (hh - r);
    float ax = dx > 0.f ? dx : 0.f;
    float ay = dy > 0.f ? dy : 0.f;
    float m  = dx > dy ? dx : dy;
    if (m > 0.f) m = 0.f;
    return sqrtf(ax * ax + ay * ay) + m - r;
}

/* One horizontal run of constant coverage. The rounded-rect fill below
 * routes everything that is not within a corner through here, which is
 * what keeps a full-window repaint cheap on the ten-year-old PCs this
 * installer exists for. */
static inline void span(int py, int xa, int xb, uint32_t col, float a)
{
    if (py < g_clip.top || py >= g_clip.bottom) return;
    if (xa < g_clip.left)  xa = g_clip.left;
    if (xb > g_clip.right) xb = g_clip.right;
    if (xa >= xb || a <= 0.003f) return;
    if (a >= 0.997f) {
        uint32_t *p = &g_px[(size_t)py * (size_t)g_mw + (size_t)xa];
        for (int i = xb - xa; i > 0; i--) *p++ = col;
        return;
    }
    for (int x = xa; x < xb; x++) blend_px(x, py, col, a);
}

static void fill_rr(float x, float y, float w, float h, float r,
                    uint32_t col, float alpha)
{
    if (w <= 0.f || h <= 0.f) return;
    GdiFlush();
    float hw = w * 0.5f, hh = h * 0.5f;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float cx = x + hw, cy = y + hh;

    int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + w) + 1;
    int y0 = (int)floorf(y) - 1, y1 = (int)ceilf(y + h) + 1;
    if (x0 < g_clip.left) x0 = g_clip.left;
    if (y0 < g_clip.top)  y0 = g_clip.top;
    if (x1 > g_clip.right)  x1 = g_clip.right;
    if (y1 > g_clip.bottom) y1 = g_clip.bottom;

    int m  = (int)ceilf(r) + 1;
    int mx0 = (int)ceilf(x)  + m, mx1 = (int)floorf(x + w) - m;
    int my0 = (int)ceilf(y)  + m, my1 = (int)floorf(y + h) - m;

    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f;
        if (py >= my0 && py < my1) {
            /* straight part: coverage depends on x alone */
            for (int px = x0; px < x1 && px < mx0; px++)
                blend_px(px, py, col,
                         clampf(0.5f - (fabsf((float)px + 0.5f - cx) - hw), 0.f, 1.f) * alpha);
            span(py, mx0 > x0 ? mx0 : x0, mx1 < x1 ? mx1 : x1, col, alpha);
            for (int px = mx1 > x0 ? mx1 : x0; px < x1; px++)
                blend_px(px, py, col,
                         clampf(0.5f - (fabsf((float)px + 0.5f - cx) - hw), 0.f, 1.f) * alpha);
        } else {
            /* corner band: SDF at the ends, flat coverage in between */
            for (int px = x0; px < x1 && px < mx0; px++)
                blend_px(px, py, col,
                         clampf(0.5f - sd_rr((float)px + 0.5f, fy, cx, cy, hw, hh, r),
                                0.f, 1.f) * alpha);
            float covy = clampf(0.5f - (fabsf(fy - cy) - hh), 0.f, 1.f);
            span(py, mx0 > x0 ? mx0 : x0, mx1 < x1 ? mx1 : x1, col, covy * alpha);
            for (int px = mx1 > x0 ? mx1 : x0; px < x1; px++)
                blend_px(px, py, col,
                         clampf(0.5f - sd_rr((float)px + 0.5f, fy, cx, cy, hw, hh, r),
                                0.f, 1.f) * alpha);
        }
    }
}

/* Only the four border strips are visited, never the interior. */
static void stroke_rr(float x, float y, float w, float h, float r,
                      float t, uint32_t col, float alpha)
{
    if (w <= 0.f || h <= 0.f) return;
    GdiFlush();
    float hw = w * 0.5f, hh = h * 0.5f;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float cx = x + hw, cy = y + hh, ht = t * 0.5f;

    int x0 = (int)floorf(x - t) - 1, x1 = (int)ceilf(x + w + t) + 1;
    int y0 = (int)floorf(y - t) - 1, y1 = (int)ceilf(y + h + t) + 1;
    if (x0 < g_clip.left) x0 = g_clip.left;
    if (y0 < g_clip.top)  y0 = g_clip.top;
    if (x1 > g_clip.right)  x1 = g_clip.right;
    if (y1 > g_clip.bottom) y1 = g_clip.bottom;

    int band = (int)ceilf(r + t) + 2;
    int ytop = (int)ceilf(y) + band, ybot = (int)floorf(y + h) - band;
    int xl   = (int)ceilf(x) + band, xr   = (int)floorf(x + w) - band;

    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f;
        int edge_row = (py < ytop || py >= ybot);
        if (edge_row) {
            for (int px = x0; px < x1; px++) {
                float d = fabsf(sd_rr((float)px + 0.5f, fy, cx, cy, hw, hh, r)) - ht;
                blend_px(px, py, col, clampf(0.5f - d, 0.f, 1.f) * alpha);
            }
        } else {
            for (int px = x0; px < x1; px++) {
                if (px >= xl && px < xr) { px = xr - 1; continue; }
                float d = fabsf(fabsf((float)px + 0.5f - cx) - hw) - ht;
                blend_px(px, py, col, clampf(0.5f - d, 0.f, 1.f) * alpha);
            }
        }
    }
}

/* The AurOS mark. Drawn, not typed: a missing glyph in a substituted
 * font would put a hollow box where the brand should be. */
static void fill_diamond(float cx, float cy, float r, uint32_t col, float a)
{
    GdiFlush();
    int x0 = (int)floorf(cx - r) - 1, x1 = (int)ceilf(cx + r) + 1;
    int y0 = (int)floorf(cy - r) - 1, y1 = (int)ceilf(cy + r) + 1;
    if (x0 < g_clip.left) x0 = g_clip.left;
    if (y0 < g_clip.top)  y0 = g_clip.top;
    if (x1 > g_clip.right)  x1 = g_clip.right;
    if (y1 > g_clip.bottom) y1 = g_clip.bottom;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float d = (fabsf((float)px + 0.5f - cx) + fabsf((float)py + 0.5f - cy) - r)
                      * 0.70711f;
            blend_px(px, py, col, clampf(0.5f - d, 0.f, 1.f) * a);
        }
}

static void fill_circle(float cx, float cy, float r, uint32_t col, float a)
{ fill_rr(cx - r, cy - r, r * 2.f, r * 2.f, r, col, a); }

static void stroke_circle(float cx, float cy, float r, float t, uint32_t col, float a)
{ stroke_rr(cx - r, cy - r, r * 2.f, r * 2.f, r, t, col, a); }

static void aa_line(float ax, float ay, float bx, float by,
                    float t, uint32_t col, float alpha)
{
    GdiFlush();
    float ht = t * 0.5f;
    int x0 = (int)floorf((ax < bx ? ax : bx) - t) - 1;
    int x1 = (int)ceilf ((ax > bx ? ax : bx) + t) + 1;
    int y0 = (int)floorf((ay < by ? ay : by) - t) - 1;
    int y1 = (int)ceilf ((ay > by ? ay : by) + t) + 1;
    if (x0 < g_clip.left) x0 = g_clip.left;
    if (y0 < g_clip.top)  y0 = g_clip.top;
    if (x1 > g_clip.right)  x1 = g_clip.right;
    if (y1 > g_clip.bottom) y1 = g_clip.bottom;
    float vx = bx - ax, vy = by - ay;
    float len2 = vx * vx + vy * vy;
    if (len2 < 1e-6f) len2 = 1e-6f;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float wx = (float)px + 0.5f - ax, wy = (float)py + 0.5f - ay;
            float s  = clampf((wx * vx + wy * vy) / len2, 0.f, 1.f);
            float dx = wx - vx * s, dy = wy - vy * s;
            float d  = sqrtf(dx * dx + dy * dy) - ht;
            blend_px(px, py, col, clampf(0.5f - d, 0.f, 1.f) * alpha);
        }
}

/* a checkmark and a cross, drawn rather than typed: no font dependency */
static void glyph_check(float cx, float cy, float s, uint32_t col, float a)
{
    aa_line(cx - s * 0.46f, cy + s * 0.02f, cx - s * 0.10f, cy + s * 0.38f,
            s * 0.20f, col, a);
    aa_line(cx - s * 0.12f, cy + s * 0.38f, cx + s * 0.48f, cy - s * 0.36f,
            s * 0.20f, col, a);
}
static void glyph_cross(float cx, float cy, float s, uint32_t col, float a)
{
    aa_line(cx - s * 0.32f, cy - s * 0.32f, cx + s * 0.32f, cy + s * 0.32f,
            s * 0.19f, col, a);
    aa_line(cx + s * 0.32f, cy - s * 0.32f, cx - s * 0.32f, cy + s * 0.32f,
            s * 0.19f, col, a);
}
static void glyph_bang(float cx, float cy, float s, uint32_t col, float a)
{
    aa_line(cx, cy - s * 0.40f, cx, cy + s * 0.10f, s * 0.19f, col, a);
    fill_circle(cx, cy + s * 0.38f, s * 0.11f, col, a);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Backdrop. Cached, because it is the only per-pixel cost that scales
 *  with window area.
 *
 *  Every value it uses is named here rather than buried in the loop, so
 *  a reskin is this block plus the palette above. Setting both
 *  intensities to 0 gives a flat ground, which is what a design without
 *  a gradient in it wants.
 * ═══════════════════════════════════════════════════════════════════ */
#define C_BG_FOOT      0x081118u   /* the wash at the foot of the window */
#define C_BACKDROP_1   C_ACCENT    /* the lower ribbon                   */
#define C_BACKDROP_2   C_ACCENT_ALT/* the upper ribbon                   */
#define BACKDROP_A1    0.16f
#define BACKDROP_A2    0.10f
static uint32_t *g_bgcache;
static int       g_bgw, g_bgh;

static void backdrop_build(int w, int h)
{
    free(g_bgcache);
    g_bgcache = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    g_bgw = w; g_bgh = h;
    if (!g_bgcache) return;

    /* two soft ribbons: teal (wall_c3) low, violet (wall_c4) higher */
    float *c1 = (float *)malloc((size_t)w * sizeof(float));
    float *c2 = (float *)malloc((size_t)w * sizeof(float));
    if (!c1 || !c2) { free(c1); free(c2); return; }
    for (int x = 0; x < w; x++) {
        float t = (float)x / (float)(w > 1 ? w - 1 : 1);
        c1[x] = 0.30f * (float)h + 0.10f * (float)h * sinf(t * 3.1f + 0.6f);
        c2[x] = 0.16f * (float)h + 0.07f * (float)h * sinf(t * 2.2f + 2.4f);
    }
    for (int y = 0; y < h; y++) {
        float vy = (float)y / (float)(h > 1 ? h - 1 : 1);
        for (int x = 0; x < w; x++) {
            /* base vertical wash: bg at the top, a touch deeper at the foot */
            float r = CH_R(C_BG) + (CH_R(C_BG_FOOT) - CH_R(C_BG)) * vy;
            float g = CH_G(C_BG) + (CH_G(C_BG_FOOT) - CH_G(C_BG)) * vy;
            float b = CH_B(C_BG) + (CH_B(C_BG_FOOT) - CH_B(C_BG)) * vy;

            float s1 = ((float)y - c1[x]) / (0.13f * (float)h);
            float a1 = expf(-s1 * s1) * BACKDROP_A1;
            float s2 = ((float)y - c2[x]) / (0.10f * (float)h);
            float a2 = expf(-s2 * s2) * BACKDROP_A2;

            r += a1 * CH_R(C_BACKDROP_1) * 0.35f + a2 * CH_R(C_BACKDROP_2) * 0.55f;
            g += a1 * CH_G(C_BACKDROP_1) * 0.55f + a2 * CH_G(C_BACKDROP_2) * 0.35f;
            b += a1 * CH_B(C_BACKDROP_1) * 0.50f + a2 * CH_B(C_BACKDROP_2) * 0.55f;

            /* ordered dither: kills the banding a 20-step gradient shows
             * on a cheap panel, which is most of our market */
            int d = (int)(((x * 7 + y * 13) & 3) - 1);
            int ri = (int)r + d, gi = (int)g + d, bi = (int)b + d;
            if (ri < 0) ri = 0;
            if (gi < 0) gi = 0;
            if (bi < 0) bi = 0;
            if (ri > 255) ri = 255;
            if (gi > 255) gi = 255;
            if (bi > 255) bi = 255;
            g_bgcache[(size_t)y * (size_t)w + (size_t)x] =
                ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
        }
    }
    free(c1); free(c2);
}

static void backdrop_blit(void)
{
    GdiFlush();
    if (!g_bgcache || g_bgw != g_mw || g_bgh != g_mh) backdrop_build(g_mw, g_mh);
    if (!g_bgcache) { for (int i = 0; i < g_mw * g_mh; i++) g_px[i] = C_BG; return; }
    memcpy(g_px, g_bgcache, (size_t)g_mw * (size_t)g_mh * sizeof(uint32_t));
}

/* ═══════════════════════════════════════════════════════════════════
 *  Text
 * ═══════════════════════════════════════════════════════════════════ */
static int text_h(const wchar_t *s, HFONT f, int w, UINT flags)
{
    RECT r = { 0, 0, w, 1 << 20 };
    SelectObject(g_mdc, f);
    DrawTextW(g_mdc, s, -1, &r, flags | DT_CALCRECT | DT_NOPREFIX);
    return r.bottom - r.top;
}

static int text_w(const wchar_t *s, HFONT f)
{
    RECT r = { 0, 0, 1 << 20, 1 << 20 };
    SelectObject(g_mdc, f);
    DrawTextW(g_mdc, s, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    return r.right - r.left;
}

/* returns the height consumed */
static int text_draw(const wchar_t *s, HFONT f, uint32_t col,
                     int x, int y, int w, UINT flags)
{
    RECT r = { x, y, x + w, y + (1 << 20) };
    SelectObject(g_mdc, f);
    SetTextColor(g_mdc, CR(col));
    SetBkMode(g_mdc, TRANSPARENT);
    DrawTextW(g_mdc, s, -1, &r, flags | DT_NOPREFIX);
    return text_h(s, f, w, flags);
}

static void text_in(const wchar_t *s, HFONT f, uint32_t col, RECT box, UINT flags)
{
    SelectObject(g_mdc, f);
    SetTextColor(g_mdc, CR(col));
    SetBkMode(g_mdc, TRANSPARENT);
    DrawTextW(g_mdc, s, -1, &box, flags | DT_NOPREFIX);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Immediate-mode widgets.
 *
 *  Every interactive thing registers its rectangle during paint. Hit
 *  testing and Tab order then read that list, so there is exactly one
 *  description of where things are and no chance of the clickable area
 *  drifting from the drawn one.
 * ═══════════════════════════════════════════════════════════════════ */
typedef enum { W_BUTTON, W_CHECK, W_CARD, W_CHIP, W_INPUT } wkind;

typedef struct { int id; wkind kind; RECT r; int enabled; } widget;

static widget g_w[96];
static int    g_nw;

enum {
    ID_NONE = 0,
    ID_PRIMARY, ID_BACK, ID_QUIT,
    ID_CHK_BACKUP, ID_CHK_USB, ID_INPUT_AGREE,
    ID_CARD_DUAL, ID_CARD_REPLACE, ID_CHK_REPLACE,
    ID_CHK_READY,
    ID_LANG = 100, ID_KBD = 200, ID_TZ = 300, ID_THEME = 400, ID_SHELL = 500
};

static int w_add(int id, wkind k, int x, int y, int w, int h, int enabled)
{
    if (g_nw >= (int)(sizeof g_w / sizeof g_w[0])) return -1;
    widget *p = &g_w[g_nw];
    p->id = id; p->kind = k; p->enabled = enabled;
    p->r.left = x; p->r.top = y; p->r.right = x + w; p->r.bottom = y + h;
    return g_nw++;
}

static int w_hot(int idx)
{
    if (idx < 0) return 0;
    const widget *p = &g_w[idx];
    return p->enabled && g_mouse.x >= p->r.left && g_mouse.x < p->r.right &&
           g_mouse.y >= p->r.top && g_mouse.y < p->r.bottom;
}
static int w_focused(int idx) { return idx >= 0 && idx == g_focus; }

static int w_find_at(int x, int y)
{
    for (int i = g_nw - 1; i >= 0; i--) {
        const widget *p = &g_w[i];
        if (p->enabled && x >= p->r.left && x < p->r.right &&
            y >= p->r.top && y < p->r.bottom) return i;
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Components
 * ═══════════════════════════════════════════════════════════════════ */
static void focus_ring(RECT r, int pad, int radius)
{
    if (!g_focus_ring) return;
    stroke_rr((float)(r.left - pad), (float)(r.top - pad),
              (float)(r.right - r.left + pad * 2), (float)(r.bottom - r.top + pad * 2),
              (float)radius, (float)S(2), C_ACCENT, 0.95f);
}

/* returns the widget index */
static int draw_button(int id, const wchar_t *label, int x, int y, int w, int h,
                       int primary, int enabled, uint32_t tone)
{
    int idx = w_add(id, W_BUTTON, x, y, w, h, enabled);
    int hot = w_hot(idx);
    int down = hot && g_mouse_down && g_press_idx == idx;
    if (down) { y += S(1); h -= S(1); }
    float r = (float)S(10);
    RECT box = { x, y, x + w, y + h };

    if (!enabled) {
        fill_rr((float)x, (float)y, (float)w, (float)h, r, C_SURFACE_HI, 0.55f);
        text_in(label, g_f_bodyb, C_MUTED, box,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    } else if (primary) {
        /* No glow. The primary action is already the only filled shape
         * on the page -- a solid block of the accent against a flat
         * ground -- and that is what makes the eye land on it. A
         * coloured halo bled under the edge was the button asking to be
         * noticed twice, and it is the exact "arbitrary drop-shadow"
         * this design is meant to be free of. The press state is the
         * fill going down a step, which is how a real button behaves. */
        fill_rr((float)x, (float)y, (float)w, (float)h, r, tone,
                down ? 0.80f : (hot ? 1.0f : 0.92f));
        text_in(label, g_f_bodyb, C_BG, box, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    } else {
        fill_rr((float)x, (float)y, (float)w, (float)h, r, C_SURFACE_HI,
                down ? 1.0f : (hot ? 0.95f : 0.55f));
        stroke_rr((float)x, (float)y, (float)w, (float)h, r, 1.2f, C_OVERLAY, 1.f);
        text_in(label, g_f_bodyb, hot ? C_FG_HI : C_FG, box,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    if (w_focused(idx)) focus_ring(box, S(4), S(14));
    return idx;
}

/* checkbox with a title and an optional second line */
static int draw_check(int id, int *state, const wchar_t *title,
                      const wchar_t *sub, int x, int y, int w, uint32_t tint)
{
    int pad  = S(16);
    int boxs = S(22);
    int tx   = x + pad + boxs + S(14);
    int tw   = w - (tx - x) - pad;
    int th   = text_h(title, g_f_bodyb, tw, DT_WORDBREAK);
    int sh   = sub ? text_h(sub, g_f_small, tw, DT_WORDBREAK) + S(4) : 0;
    int h    = th + sh + pad * 2;

    int idx = w_add(id, W_CHECK, x, y, w, h, 1);
    int hot = w_hot(idx);

    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(12), C_SURFACE_HI,
            *state ? 0.85f : (hot ? 0.55f : 0.30f));
    if (*state)
        stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(12), 1.4f,
                  tint, 0.55f);

    float bx = (float)(x + pad), by = (float)(y + pad + S(1));
    if (*state) {
        fill_rr(bx, by, (float)boxs, (float)boxs, (float)S(7), tint, 1.f);
        glyph_check(bx + boxs * 0.5f, by + boxs * 0.5f, (float)boxs * 0.62f,
                    C_BG, 1.f);
    } else {
        stroke_rr(bx, by, (float)boxs, (float)boxs, (float)S(7), 1.6f,
                  hot ? C_SUBTLE : C_OVERLAY, 1.f);
    }

    text_draw(title, g_f_bodyb, *state ? C_FG_HI : C_FG, tx, y + pad, tw, DT_WORDBREAK);
    if (sub)
        text_draw(sub, g_f_small, C_SUBTLE, tx, y + pad + th + S(4), tw, DT_WORDBREAK);

    if (w_focused(idx)) { RECT b = { x, y, x + w, y + h }; focus_ring(b, S(3), S(15)); }
    return h;
}

static int draw_chip(int id, const wchar_t *label, int x, int y, int selected)
{
    int h = S(34);
    int w = text_w(label, g_f_small) + S(28);
    int idx = w_add(id, W_CHIP, x, y, w, h, 1);
    int hot = w_hot(idx);
    RECT box = { x, y, x + w, y + h };
    if (selected) {
        fill_rr((float)x, (float)y, (float)w, (float)h, (float)h * 0.5f,
                C_ACCENT, 0.16f);
        stroke_rr((float)x, (float)y, (float)w, (float)h, (float)h * 0.5f, 1.4f,
                  C_ACCENT, 0.85f);
    } else {
        fill_rr((float)x, (float)y, (float)w, (float)h, (float)h * 0.5f,
                C_SURFACE_HI, hot ? 0.9f : 0.5f);
        stroke_rr((float)x, (float)y, (float)w, (float)h, (float)h * 0.5f, 1.f,
                  C_OVERLAY, 0.9f);
    }
    text_in(label, g_f_small, selected ? C_FG_HI : (hot ? C_FG : C_SUBTLE), box,
            DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    if (w_focused(idx)) focus_ring(box, S(3), (int)(h * 0.5f) + S(3));
    return w;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Preflight plumbing
 * ═══════════════════════════════════════════════════════════════════ */
static DWORD WINAPI pf_worker(LPVOID p)
{
    (void)p;
    pf_run(&g_report);                 /* read-only, by contract */
    InterlockedExchange(&g_pf_state, 2);
    return 0;
}

static void pf_start(void)
{
    if (InterlockedCompareExchange(&g_pf_state, 1, 0) != 0 &&
        InterlockedCompareExchange(&g_pf_state, 1, 2) != 2) return;
    g_pf_valid = 0;
    g_reveal   = 0;
    g_ticks    = 0;
    memset(&g_report, 0, sizeof g_report);
    HANDLE h = CreateThread(NULL, 0, pf_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else { pf_run(&g_report); InterlockedExchange(&g_pf_state, 2); }
}

/* The checklist the CHECKING page shows. Every id preflight can emit is
 * listed here; anything unrecognised still lands in the last group, so a
 * new check added to preflight.c can never be silently invisible. */
static const struct { const wchar_t *label; const wchar_t *note; const char *ids[12]; }
CHK[] = {
  { L"Permission to make changes", L"AurBridge must run as an administrator",
    { "not-elevated", NULL } },
  { L"How this PC starts up", L"UEFI start-up, and Secure Boot",
    { "firmware-uefi", "firmware-bios", "secure-boot-on", NULL } },
  { L"Power", L"Plugged in, with charge to spare",
    { "not-on-ac", "battery-low", NULL } },
  { L"Windows updates", L"Nothing half-installed and waiting for a restart",
    { "pending-reboot", NULL } },
  { L"Fast Startup", L"Windows must be fully off, not half-asleep",
    { "fast-startup", NULL } },
  { L"Drives, and their health", L"Every drive found, and its self-test history",
    { "dynamic-disk", "spanned-system-volume", "system-disk-unknown",
      "smart-bad-sectors", "smart-predict-failure", "smart-reallocated",
      "mbr-four-primaries", "system-disk-unreadable", "removable-attached",
      "no-disks", NULL } },
  { L"The Windows drive", L"Whether it is locked, healthy, and has room",
    { "bitlocker-system", "bitlocker-other", "volume-dirty",
      "system-not-ntfs", "insufficient-space", NULL } },
  { L"How the drives are connected", L"Some setups hide drives from AurOS",
    { "intel-rst", NULL } },
  { L"Memory", L"Enough to run the desktop comfortably",
    { "low-ram", NULL } },
  { L"Everything else", L"Any check added since this list was written",
    { NULL } },
};
#define N_CHK ((int)(sizeof CHK / sizeof CHK[0]))

/* Index of the worst result in a group, or -1 when the group had nothing
 * to say. Anything preflight emits that this list does not name falls
 * into the last group, so a new check can never be invisible here. */
static int chk_group_res(int g)
{
    int best = -1;
    for (int i = 0; i < g_report.n; i++) {
        const pf_result *x = &g_report.results[i];
        if (!strcmp(x->id, "ready")) continue;
        int mine = 0, claimed = 0;
        for (int k = 0; k < N_CHK; k++)
            for (int j = 0; CHK[k].ids[j]; j++)
                if (!strcmp(CHK[k].ids[j], x->id)) {
                    claimed = 1;
                    if (k == g) mine = 1;
                }
        if (!claimed && g == N_CHK - 1) mine = 1;   /* catch-all */
        if (mine && (best < 0 || x->sev > g_report.results[best].sev)) best = i;
    }
    return best;
}

static int chk_group_sev(int g)
{
    int i = chk_group_res(g);
    return i < 0 ? -1 : (int)g_report.results[i].sev;
}

static uint32_t sev_color(int sev)
{
    switch (sev) {
        case PF_BLOCK: return C_ERR;
        case PF_WARN:  return C_WARN;
        case PF_INFO:  return C_INFO;
        default:       return C_OK;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Navigation — the safety gate.
 *
 *  This function, not the absence of a button, is what makes a blocked
 *  machine unreachable from the backup gate onward.
 * ═══════════════════════════════════════════════════════════════════ */
static int consent_ok(void)
{
    return _wcsicmp(g_agree, AGREE_WORD) == 0;
}
static int choice_ok(void)
{
    return g_choice == 0 || (g_choice == 1 && g_ack_replace);
}

static int nav_allowed(page_id p)
{
    switch (p) {
    case PAGE_WELCOME:
    case PAGE_CHECKING:
        return 1;
    case PAGE_BLOCKED:
        return g_pf_valid && g_report.n_block > 0;
    default:
        break;
    }
    /* Hard gate. Nothing past the checklist exists for a machine we have
     * refused, and nothing exists at all until preflight has actually run.
     * R3/#3 in the red-team register: refusing is the product outcome. */
    if (!g_pf_valid || !pf_is_go(&g_report)) return 0;

    switch (p) {
    case PAGE_BACKUP:      return 1;
    case PAGE_CONSENT:     return g_ack_backup && g_ack_usb;
    case PAGE_CHOOSE:      return nav_allowed(PAGE_CONSENT) && consent_ok();
    case PAGE_DESKTOP:     return nav_allowed(PAGE_CHOOSE) && choice_ok();
    /* The archetype chooser has no gate of its own: one is always
     * selected (Rail by default), so there is nothing to withhold. */
    case PAGE_PERSONALIZE: return nav_allowed(PAGE_DESKTOP);
    case PAGE_READY:       return nav_allowed(PAGE_PERSONALIZE);
    case PAGE_PROGRESS:    return nav_allowed(PAGE_READY) && g_ready_confirm;
    default:               return 0;
    }
}

static int g_max_page;

static void goto_page(page_id p)
{
    if (!nav_allowed(p)) {
        /* Should be unreachable: the UI never offers a disallowed move.
         * Kept because "unreachable" is a claim, and this is the proof. */
        OutputDebugStringA("aurbridge: navigation refused\n");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g_page = p;
    if ((int)p > g_max_page) g_max_page = (int)p;
    g_focus = -1;
    g_focus_ring = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Phase engine — ALL STUBBED.
 *
 *  Not one line below touches a partition table, a volume, a boot entry
 *  or a file. Each stub logs the work the real phase will do and returns
 *  success. The real implementations land in their own translation unit
 *  with their own tests, per AURBRIDGE.md ("Still to build").
 * ═══════════════════════════════════════════════════════════════════ */
typedef enum { PH_PENDING = 0, PH_RUNNING, PH_DONE, PH_LATER, PH_FAILED } ph_state;

static const struct {
    const wchar_t *name;
    const wchar_t *desc;
    int after_restart;
} PHASES[] = {
 { L"Look at this PC once more",
   L"Every safety check runs again, right before we start. Nothing is written.", 0 },
 { L"Write down what you agreed to",
   L"Your answers, and a copy of this PC\u2019s unlock key if it has one.", 0 },
 { L"Build a way back",
   L"A rescue USB stick, and a rescue area on the drive. Windows is untouched by this step.", 0 },
 { L"Make room",
   L"The Windows drive is made smaller. Your Windows files stay where they are.", 0 },
 { L"Copy AurOS onto the drive",
   L"AurOS is written into the new space, then read back and checked, byte for byte.", 0 },
 { L"Add AurOS to the start-up menu",
   L"AurOS is offered once, at the next start. Windows stays the one that starts by default.", 0 },
 { L"Try it out on this PC",
   L"After the restart: Wi-Fi, screen brightness, sound and sleep are tested before anything is final.", 1 },
 { L"Your first look",
   L"You use the desktop and tell us it works.", 1 },
 { L"Bring your files across",
   L"Your documents and photos are copied over. Only now does anything become permanent.", 1 },
};
#define N_PHASES ((int)(sizeof PHASES / sizeof PHASES[0]))

static int   g_ph[N_PHASES];
static int   g_ph_cur = -1;
static float g_ph_prog;
static int   g_install_running, g_install_finished;

#define LOG_MAX 80
static wchar_t g_log[LOG_MAX][200];
static int     g_log_n;

static void stub_log(const wchar_t *fmt, ...)
{
    wchar_t line[200];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(line, 199, fmt, ap);
    va_end(ap);
    line[199] = 0;
    if (g_log_n < LOG_MAX) {
        wcscpy(g_log[g_log_n++], line);
    } else {
        memmove(g_log[0], g_log[1], sizeof g_log - sizeof g_log[0]);
        wcscpy(g_log[LOG_MAX - 1], line);
    }
    OutputDebugStringW(L"aurbridge STUB: ");
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
}

/* STUB — phase 0 INSPECT. The real one re-runs preflight and aborts on
 * any block. The wizard already does that before it gets here. */
static int stub_phase_inspect(void)
{
    stub_log(L"WOULD re-run preflight and abort on any PF_BLOCK");
    stub_log(L"WOULD record a machine.json snapshot for support");
    return 1;
}
/* STUB — phase 1 CONSENT. Proof-of-possession of the BitLocker key
 * (R1) and the signed consent record. */
static int stub_phase_consent(void)
{
    stub_log(L"WOULD ask for part of the 48-digit unlock key typed back (R1)");
    stub_log(L"WOULD offer to save or print that key");
    stub_log(L"WOULD suspend BitLocker with -RebootCount 0");
    return 1;
}
/* STUB — phase 2 PREPARE. Recovery USB + recovery partition. Consumes
 * free space only; Windows is not modified. */
static int stub_phase_prepare(void)
{
    stub_log(L"WOULD write the rescue USB stick (rescue kernel + tools)");
    stub_log(L"WOULD create a 600 MB FAT32 rescue partition in free space");
    stub_log(L"WOULD back up the GPT, the whole ESP and the Windows BCD into it");
    stub_log(L"WOULD install the 'Put Windows back' entry in the start-up menu");
    return 1;
}
/* STUB — phase 3 SHRINK. The one destructive step on the Windows side,
 * done offline from the staging environment with ntfsresize. */
static int stub_phase_shrink(void)
{
    stub_log(L"WOULD read StorageAccessAlignmentProperty (512e vs 4Kn)");
    stub_log(L"WOULD disable hibernation and the pagefile, and drop shadow copies");
    stub_log(L"WOULD run chkdsk and refuse on any correction (R9)");
    stub_log(L"WOULD shrink the NTFS volume with ntfsresize, offline");
    stub_log(L"WOULD write the new partition table as one sector write (R6)");
    return 1;
}
/* STUB — phase 4 WRITE. Raw image to the new partition, read back and
 * verified against its hash. */
static int stub_phase_write(void)
{
    stub_log(L"WOULD write auros-desktop.img to the new partition");
    stub_log(L"WOULD read every block back and compare it to the hash (R5)");
    /* The personalize answers travel this far as indices; the profile is
     * where they become text. The archetype is the one with a stable
     * identifier of its own (shells/<id>.shell), so it is named here. */
    stub_log(L"WOULD write shell_archetype=%hs into the image's profile",
             SHELLS[g_sel_shell].id);
    return 1;
}
/* STUB — phase 5 HANDOFF. Loader into the ESP, one-shot BootNext. */
static int stub_phase_handoff(void)
{
    stub_log(L"WOULD copy the loader into the ESP, never reformatting it (R12)");
    stub_log(L"WOULD set BootNext only - never BootOrder - so a failed boot");
    stub_log(L"         comes back to Windows on its own (R4)");
    return 1;
}

static int (*const PHASE_STUB[6])(void) = {
    stub_phase_inspect, stub_phase_consent, stub_phase_prepare,
    stub_phase_shrink,  stub_phase_write,   stub_phase_handoff
};

static void install_begin(void)
{
    g_log_n = 0;
    for (int i = 0; i < N_PHASES; i++)
        g_ph[i] = PHASES[i].after_restart ? PH_LATER : PH_PENDING;
    g_ph_cur = 0;
    g_ph_prog = 0.f;
    g_install_running = 1;
    g_install_finished = 0;
    stub_log(L"This build is a stub. Nothing on this PC is read for writing,");
    stub_log(L"opened for writing, or changed in any way.");
    g_ph[0] = PH_RUNNING;
    PHASE_STUB[0]();
}

/* Driven from WM_TIMER so the window keeps painting. */
static void install_tick(void)
{
    if (!g_install_running || g_ph_cur < 0) return;
    g_ph_prog += 0.085f;
    if (g_ph_prog < 1.f) return;
    g_ph_prog = 0.f;
    g_ph[g_ph_cur] = PH_DONE;
    g_ph_cur++;
    if (g_ph_cur >= 6) {          /* phases 6-8 happen after the restart */
        g_ph_cur = -1;
        g_install_running = 0;
        g_install_finished = 1;
        stub_log(L"Stub run complete. A real run would restart into AurOS here.");
        return;
    }
    g_ph[g_ph_cur] = PH_RUNNING;
    PHASE_STUB[g_ph_cur]();
}

/* ═══════════════════════════════════════════════════════════════════
 *  What Windows already knows about this user
 * ═══════════════════════════════════════════════════════════════════ */
#define OPT_MAX 10
static wchar_t g_langs[OPT_MAX][96]; static int g_n_langs;
static wchar_t g_kbds [OPT_MAX][96]; static int g_n_kbds;
static wchar_t g_tzs  [OPT_MAX][96];  static int g_n_tzs;

static void opt_push(wchar_t (*list)[96], int *n, const wchar_t *s)
{
    if (!s || !*s || *n >= OPT_MAX) return;
    for (int i = 0; i < *n; i++) if (!_wcsicmp(list[i], s)) return;
    wcsncpy(list[*n], s, 95); list[*n][95] = 0; (*n)++;
}

static void detect_defaults(void)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
        if (!GetLocaleInfoEx(name, LOCALE_SLOCALIZEDDISPLAYNAME, g_det_lang, 96))
            wcsncpy(g_det_lang, name, 95);
    }
    wchar_t klid[KL_NAMELENGTH];
    if (GetKeyboardLayoutNameW(klid)) {
        wchar_t sub[200];
        _snwprintf(sub, 199,
            L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\%s", klid);
        sub[199] = 0;
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            DWORD sz = sizeof g_det_kbd, type = 0;
            if (RegQueryValueExW(k, L"Layout Text", NULL, &type,
                                 (BYTE *)g_det_kbd, &sz) != ERROR_SUCCESS)
                g_det_kbd[0] = 0;
            RegCloseKey(k);
        }
    }
    TIME_ZONE_INFORMATION tzi;
    memset(&tzi, 0, sizeof tzi);
    if (GetTimeZoneInformation(&tzi) != TIME_ZONE_ID_INVALID)
        wcsncpy(g_det_tz, tzi.StandardName, 127);

    /* The value Windows is already using goes first and is preselected:
     * the common case should need no clicks at all. */
    opt_push(g_langs, &g_n_langs, g_det_lang);
    opt_push(g_langs, &g_n_langs, L"English (United States)");
    opt_push(g_langs, &g_n_langs, L"English (United Kingdom)");
    opt_push(g_langs, &g_n_langs, L"Español");
    opt_push(g_langs, &g_n_langs, L"Français");
    opt_push(g_langs, &g_n_langs, L"Deutsch");
    opt_push(g_langs, &g_n_langs, L"Português");
    opt_push(g_langs, &g_n_langs, L"Italiano");
    opt_push(g_langs, &g_n_langs, L"Polski");

    opt_push(g_kbds, &g_n_kbds, g_det_kbd);
    opt_push(g_kbds, &g_n_kbds, L"US");
    opt_push(g_kbds, &g_n_kbds, L"United Kingdom");
    opt_push(g_kbds, &g_n_kbds, L"Spanish");
    opt_push(g_kbds, &g_n_kbds, L"French (AZERTY)");
    opt_push(g_kbds, &g_n_kbds, L"German (QWERTZ)");
    opt_push(g_kbds, &g_n_kbds, L"Portuguese (Brazil)");

    opt_push(g_tzs, &g_n_tzs, g_det_tz);
    opt_push(g_tzs, &g_n_tzs, L"GMT Standard Time");
    opt_push(g_tzs, &g_n_tzs, L"Central European Time");
    opt_push(g_tzs, &g_n_tzs, L"Eastern Time (US & Canada)");
    opt_push(g_tzs, &g_n_tzs, L"Central Time (US & Canada)");
    opt_push(g_tzs, &g_n_tzs, L"Pacific Time (US & Canada)");

    if (!g_n_langs) opt_push(g_langs, &g_n_langs, L"English (United States)");
    if (!g_n_kbds)  opt_push(g_kbds,  &g_n_kbds,  L"US");
    if (!g_n_tzs)   opt_push(g_tzs, &g_n_tzs, L"GMT Standard Time");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Layout
 * ═══════════════════════════════════════════════════════════════════ */
static RECT g_card, g_body, g_foot;
static int  g_rail_w;

static void layout(void)
{
    int m = S(22);
    g_rail_w = S(252);
    g_card.left   = g_rail_w;
    g_card.top    = m;
    g_card.right  = g_cw - m;
    g_card.bottom = g_ch - m;

    /* WM_GETMINMAXINFO keeps the window above this, but a DPI change can
     * land in between: never hand DrawText an inverted rectangle. */
    if (g_card.right  < g_card.left + S(360)) g_card.right  = g_card.left + S(360);
    if (g_card.bottom < g_card.top  + S(280)) g_card.bottom = g_card.top  + S(280);

    int pad = S(44);
    g_foot.left   = g_card.left;
    g_foot.right  = g_card.right;
    g_foot.top    = g_card.bottom - S(92);
    g_foot.bottom = g_card.bottom;

    g_body.left   = g_card.left + pad;
    g_body.right  = g_card.right - pad;
    if (g_body.right < g_body.left + S(200)) g_body.right = g_body.left + S(200);
    g_body.top    = g_card.top + S(40);
    g_body.bottom = g_foot.top;
}

static void set_clip(int l, int t, int r, int b)
{
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > g_mw) r = g_mw;
    if (b > g_mh) b = g_mh;
    g_clip.left = l; g_clip.top = t; g_clip.right = r; g_clip.bottom = b;
    HRGN rgn = CreateRectRgn(l, t, r, b);
    SelectClipRgn(g_mdc, rgn);
    DeleteObject(rgn);
}
static void clip_reset(void) { set_clip(0, 0, g_mw, g_mh); }

/* ═══════════════════════════════════════════════════════════════════
 *  Shared page furniture
 * ═══════════════════════════════════════════════════════════════════ */
static void status_icon(float cx, float cy, float r, int state, int sev)
{
    if (state == 0) {                       /* waiting */
        stroke_circle(cx, cy, r, 1.4f, C_OVERLAY, 1.f);
        return;
    }
    if (state == 1) {                       /* working */
        for (int i = 0; i < 8; i++) {
            float a  = (float)i * 0.7853982f;
            float ph = (float)((g_ticks / 2 - i) & 7) / 7.f;
            fill_circle(cx + cosf(a) * r * 0.78f, cy + sinf(a) * r * 0.78f,
                        r * 0.17f, C_ACCENT, 0.12f + 0.75f * ph);
        }
        return;
    }
    uint32_t c = sev_color(sev);
    if (sev == PF_BLOCK) {
        fill_circle(cx, cy, r, c, 0.18f);
        stroke_circle(cx, cy, r, 1.5f, c, 0.9f);
        glyph_cross(cx, cy, r * 1.35f, c, 1.f);
    } else if (sev == PF_WARN) {
        fill_circle(cx, cy, r, c, 0.16f);
        stroke_circle(cx, cy, r, 1.5f, c, 0.85f);
        glyph_bang(cx, cy, r * 1.3f, c, 1.f);
    } else {
        fill_circle(cx, cy, r, c, 0.9f);
        glyph_check(cx, cy, r * 1.25f, C_BG, 1.f);
    }
}

static void draw_rail(void)
{
    int x = S(36);
    int y = S(40);

    fill_diamond((float)(x + S(8)), (float)(y + S(13)), (float)S(9), C_ACCENT, 1.f);
    text_draw(L"AurOS", g_f_h2, C_FG_HI, x + S(28), y, S(160), DT_LEFT);
    text_draw(L"AurBridge installer", g_f_small, C_SUBTLE,
              x + S(28), y + S(26), S(190), DT_LEFT);

    int cur = rail_index_for(g_page);
    int sy  = S(128);
    int step = S(46);
    for (int i = 0; i < N_RAIL; i++) {
        float cx = (float)(x + S(7));
        float cy = (float)(sy + i * step + S(9));
        if (i < N_RAIL - 1)
            aa_line(cx, cy + S(9), cx, cy + (float)step - S(9), 1.2f, C_OVERLAY, 1.f);

        int blocked = (g_page == PAGE_BLOCKED && i == cur);
        uint32_t c  = blocked ? C_ERR : C_ACCENT;
        const wchar_t *label = RAIL[i].label;
        if (blocked) label = L"Stopped here";

        if (i < cur) {
            fill_circle(cx, cy, (float)S(7), C_ACCENT, 0.85f);
            glyph_check(cx, cy, (float)S(9), C_BG, 1.f);
            text_draw(label, g_f_small, C_SUBTLE, x + S(28), sy + i * step,
                      g_rail_w - x - S(40), DT_LEFT);
        } else if (i == cur) {
            fill_circle(cx, cy, (float)S(12), c, 0.18f);
            fill_circle(cx, cy, (float)S(7), c, 1.f);
            if (blocked) glyph_cross(cx, cy, (float)S(9), C_BG, 1.f);
            text_draw(label, g_f_smallb, blocked ? C_ERR : C_FG_HI,
                      x + S(28), sy + i * step, g_rail_w - x - S(40), DT_LEFT);
        } else {
            stroke_circle(cx, cy, (float)S(6), 1.3f, C_OVERLAY, 1.f);
            text_draw(label, g_f_small, C_MUTED, x + S(28), sy + i * step,
                      g_rail_w - x - S(40), DT_LEFT);
        }
    }

    text_draw(L"aurbridge 0.1.0", g_f_tiny, C_MUTED, x, g_ch - S(52), S(200), DT_LEFT);
    text_draw(L"stub build — nothing is changed", g_f_tiny, C_MUTED,
              x, g_ch - S(36), S(220), DT_LEFT);
}

static void glyph_arc(float cx, float cy, float r, float a0, float a1,
                      float t, uint32_t c, float al)
{
    float px = 0.f, py = 0.f;
    for (int i = 0; i <= 16; i++) {
        float a = a0 + (a1 - a0) * (float)i / 16.f;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        if (i) aa_line(px, py, x, y, t, c, al);
        px = x; py = y;
    }
}
static void glyph_undo(float cx, float cy, float s, uint32_t c, float a)
{
    glyph_arc(cx, cy, s * 0.52f, 2.6f, 2.6f + 5.0f, s * 0.17f, c, a);
    float ex = cx + cosf(2.6f) * s * 0.52f, ey = cy + sinf(2.6f) * s * 0.52f;
    aa_line(ex, ey, ex + s * 0.26f, ey - s * 0.16f, s * 0.17f, c, a);
    aa_line(ex, ey, ex + s * 0.10f, ey + s * 0.28f, s * 0.17f, c, a);
}
static void glyph_search(float cx, float cy, float s, uint32_t c, float a)
{
    stroke_circle(cx - s * 0.10f, cy - s * 0.10f, s * 0.34f, s * 0.16f, c, a);
    aa_line(cx + s * 0.14f, cy + s * 0.14f, cx + s * 0.40f, cy + s * 0.40f,
            s * 0.17f, c, a);
}

/* icon + heading + body, the shape most of these pages are made of */
static int feature_row(int glyph, const wchar_t *title, const wchar_t *body,
                       int x, int y, int w, uint32_t tint)
{
    /* No tinted chip behind the glyph.
     *
     * A pale rounded square holding a thin outline icon is the single
     * most repeated shape in machine-made interfaces, and the desktop
     * had twenty of them before this. The glyph stands on its own,
     * drawn larger and at full strength; the column it sits in still
     * aligns the rows, so the container was doing nothing the layout
     * was not already doing. */
    int d  = S(38);
    int tx = x + d + S(18);
    int tw = w - (tx - x);
    float cx = (float)x + (float)d * 0.5f, cy = (float)y + (float)d * 0.5f;
    if (glyph == 0) glyph_check (cx, cy, (float)S(24), tint, 1.f);
    if (glyph == 1) glyph_undo  (cx, cy, (float)S(26), tint, 1.f);
    if (glyph == 2) glyph_search(cx, cy, (float)S(26), tint, 1.f);
    int th = text_draw(title, g_f_bodyb, C_FG_HI, tx, y + S(2), tw, DT_WORDBREAK);
    int bh = text_draw(body, g_f_body, C_SUBTLE, tx, y + S(4) + th, tw, DT_WORDBREAK);
    int h  = th + bh + S(4);
    return (h > d ? h : d) + S(22);
}

/* ═══════════════════════════════════════════════════════════════════
 *  1 · WELCOME
 * ═══════════════════════════════════════════════════════════════════ */
static int page_welcome(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(660) ? S(660) : w;

    y += text_draw(L"Let\u2019s put AurOS on this PC", g_f_title, C_FG_HI,
                   x, y, w, DT_WORDBREAK) + S(16);
    y += text_draw(L"AurOS is another desktop for your computer. It starts quickly, "
                   L"it stays out of your way, and it is free. This program puts it "
                   L"on this PC for you, without you having to understand any of it.",
                   g_f_body, C_FG, x, y, narrow, DT_WORDBREAK) + S(34);

    y += feature_row(0, L"Windows stays exactly where it is",
                     L"Your files, your programs and your settings are left alone. "
                     L"Each time you switch the PC on, you pick which one you want.",
                     x, y, narrow, C_ACCENT);
    y += feature_row(1, L"You can change your mind",
                     L"Before anything at all is changed, we build a rescue area on "
                     L"the drive and a rescue USB stick. One button puts Windows back "
                     L"the way it was.",
                     x, y, narrow, C_ACCENT);
    /* The first two rows are reassurances and share one colour, because
     * they are the same kind of statement. The third is a refusal, and
     * the caution colour there is MEANING rather than variety -- which
     * is the difference between a palette and a cycle. These three rows
     * used to run accent, accent-alt, warm, so the eye was told the
     * three claims differed in kind when only the last one does. */
    y += feature_row(2, L"We look at this PC first, and we will say no",
                     L"If there is anything here we cannot do safely, we stop and tell "
                     L"you why in plain words. Nothing is changed when we stop.",
                     x, y, narrow, C_WARM);
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  2 · CHECKING
 * ═══════════════════════════════════════════════════════════════════ */
static int page_checking(int x, int y, int w)
{
    int y0 = y;
    int done = g_pf_valid;

    y += text_draw(L"Checking this PC", g_f_title, C_FG_HI, x, y, w, DT_WORDBREAK) + S(10);
    y += text_draw(L"We only read. Nothing on this PC is written to or changed.",
                   g_f_body, C_SUBTLE, x, y, w, DT_SINGLELINE) + S(20);

    /* The answer goes above the list, not below it: it is the one thing
     * on this page the user is waiting for. */
    if (done && g_reveal >= N_CHK) {
        wchar_t msg[200];
        uint32_t c;
        if (g_report.n_block > 0) {
            _snwprintf(msg, 199, g_report.n_block == 1
                ? L"We have to stop. There is 1 thing here we cannot do safely."
                : L"We have to stop. There are %d things here we cannot do safely.",
                g_report.n_block);
            c = C_ERR;
        } else if (g_report.n_warn > 0) {
            _snwprintf(msg, 199, g_report.n_warn == 1
                ? L"All clear. There is 1 thing worth knowing about, coming up next."
                : L"All clear. There are %d things worth knowing about, coming up next.",
                g_report.n_warn);
            c = C_WARM;
        } else {
            wcscpy(msg, L"All clear. This PC can take AurOS.");
            c = C_ACCENT;
        }
        msg[199] = 0;
        fill_rr((float)x, (float)y, (float)w, (float)S(48), (float)S(10), c, 0.11f);
        stroke_rr((float)x, (float)y, (float)w, (float)S(48), (float)S(10), 1.f, c, 0.3f);
        RECT b = { x + S(18), y, x + w, y + S(48) };
        text_in(msg, g_f_bodyb, c, b, DT_SINGLELINE | DT_VCENTER);
        y += S(48) + S(20);
    } else if (!done) {
        y += text_draw(L"Reading\u2026", g_f_small, C_SUBTLE, x, y, w, DT_SINGLELINE) + S(16);
    }

    for (int i = 0; i < N_CHK; i++) {
        int sev   = done ? chk_group_sev(i) : -1;
        int state = !done ? 1 : (i < g_reveal ? 2 : 1);
        int res   = (done && i < g_reveal) ? chk_group_res(i) : -1;

        /* the last group is only interesting if something landed in it */
        if (i == N_CHK - 1 && done && sev < 0) continue;

        int rh = S(48);
        if (state == 2 && sev >= PF_WARN)
            fill_rr((float)x, (float)y, (float)w, (float)rh, (float)S(10),
                    sev_color(sev), 0.07f);

        status_icon((float)(x + S(18)), (float)(y + rh / 2), (float)S(11),
                    state, sev < 0 ? PF_PASS : sev);

        int tx = x + S(46);
        int tw = w - S(60);
        text_draw(CHK[i].label, g_f_bodyb,
                  state == 2 ? C_FG_HI : C_MUTED, tx, y + S(6), tw, DT_SINGLELINE);
        if (res >= 0 && sev >= PF_WARN) {
            wchar_t t[128];
            a2w(g_report.results[res].title, t, 128);
            text_draw(t, g_f_small, sev_color(sev), tx, y + S(25), tw, DT_SINGLELINE);
        } else {
            text_draw(CHK[i].note, g_f_small, state == 2 ? C_SUBTLE : C_MUTED,
                      tx, y + S(25), tw, DT_SINGLELINE);
        }
        y += rh + S(2);
    }
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  3 · BLOCKED
 *
 *  A refusal is a product outcome, not an error dialog. It gets the same
 *  care as the happy path: what we found, what it means for this person,
 *  and the one thing they can do about it. There is no continue button,
 *  no "advanced", no override, and nav_allowed() would refuse one anyway.
 * ═══════════════════════════════════════════════════════════════════ */
static int block_card(const pf_result *r, int x, int y, int w)
{
    wchar_t title[128], detail[600], remedy[600], risk[16];
    a2w(r->title, title, 128);
    a2w(r->detail, detail, 600);
    a2w(r->remedy, remedy, 600);
    risk[0] = 0;
    if (r->risk[0]) a2w(r->risk, risk, 16);

    int pad = S(22);
    int tx  = x + pad + S(12);
    int tw  = w - (tx - x) - pad - (risk[0] ? S(48) : 0);
    int bw  = w - (tx - x) - pad;

    int th = text_h(title, g_f_h3, tw, DT_WORDBREAK);
    int dh = detail[0] ? text_h(detail, g_f_body, bw, DT_WORDBREAK) : 0;

    int rpad = S(16);
    int rlab = S(20);
    int rh   = remedy[0]
        ? text_h(remedy, g_f_body, bw - rpad * 2, DT_WORDBREAK) + rlab + rpad * 2 + S(4)
        : 0;
    int h = pad + th + (dh ? dh + S(8) : 0) + (rh ? rh + S(18) : 0) + pad;

    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(14), C_SURFACE_HI, 0.75f);
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(14), 1.2f, C_ERR, 0.30f);
    fill_rr((float)x + 1.f, (float)(y + S(14)), (float)S(4), (float)(h - S(28)),
            (float)S(2), C_ERR, 0.95f);

    int cy = y + pad;
    cy += text_draw(title, g_f_h3, C_ERR, tx, cy, tw, DT_WORDBREAK);
    if (detail[0]) {
        cy += S(8);
        cy += text_draw(detail, g_f_body, C_FG, tx, cy, bw, DT_WORDBREAK);
    }
    if (remedy[0]) {
        cy += S(18);
        fill_rr((float)tx, (float)cy, (float)bw, (float)rh, (float)S(10),
                C_SURFACE, 0.92f);
        text_draw(L"WHAT TO DO", g_f_tiny, C_WARM, tx + rpad, cy + rpad, bw, DT_SINGLELINE);
        text_draw(remedy, g_f_body, C_FG_HI, tx + rpad, cy + rpad + rlab,
                  bw - rpad * 2, DT_WORDBREAK);
    }
    if (risk[0]) {
        int pw = text_w(risk, g_f_tiny) + S(16);
        int px = x + w - pad - pw;
        fill_rr((float)px, (float)(y + pad), (float)pw, (float)S(20),
                (float)S(10), C_ERR, 0.14f);
        RECT b = { px, y + pad, px + pw, y + pad + S(20) };
        text_in(risk, g_f_tiny, C_ERR, b, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    return h;
}

static int page_blocked(int x, int y, int w)
{
    int y0 = y;
    y += text_draw(L"We have stopped, and nothing has been changed",
                   g_f_title, C_FG_HI, x, y, w, DT_WORDBREAK) + S(14);
    y += text_draw(L"There is something about this PC that we cannot work around "
                   L"safely. This PC is exactly as it was a moment ago — no drive, "
                   L"no file and no setting has been touched.",
                   g_f_body, C_FG, x, y, w > S(680) ? S(680) : w, DT_WORDBREAK) + S(26);

    for (int i = 0; i < g_report.n; i++) {
        if (g_report.results[i].sev != PF_BLOCK) continue;
        y += block_card(&g_report.results[i], x, y, w) + S(14);
    }

    /* warnings are shown here too, quietly: if the user fixes the blocks
     * they will meet these next, and surprises are how trust is lost */
    int shown_warn_head = 0;
    for (int i = 0; i < g_report.n; i++) {
        if (g_report.results[i].sev != PF_WARN) continue;
        if (!shown_warn_head) {
            y += S(12);
            y += text_draw(L"Also worth knowing", g_f_smallb, C_WARM, x, y, w,
                           DT_SINGLELINE) + S(10);
            shown_warn_head = 1;
        }
        wchar_t t[128], d[600];
        a2w(g_report.results[i].title, t, 128);
        a2w(g_report.results[i].detail, d, 600);
        int th = text_draw(t, g_f_bodyb, C_WARM, x + S(16), y, w - S(32), DT_WORDBREAK);
        int dh = text_draw(d, g_f_small, C_SUBTLE, x + S(16), y + th + S(2),
                           w - S(32), DT_WORDBREAK);
        y += th + dh + S(16);
    }

    y += S(10);
    y += text_draw(L"There is no way past this screen. That is deliberate: it is the "
                   L"difference between an install we refused and a photo library we "
                   L"lost. Fix what is listed above and press Check again — or close "
                   L"this and carry on using Windows exactly as before.",
                   g_f_small, C_MUTED, x, y, w > S(680) ? S(680) : w, DT_WORDBREAK);
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  4 · BACKUP GATE
 * ═══════════════════════════════════════════════════════════════════ */
static int page_backup(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(700) ? S(700) : w;

    y += text_draw(L"Two things before we go on", g_f_title, C_FG_HI,
                   x, y, w, DT_WORDBREAK) + S(14);
    y += text_draw(L"Almost every install goes fine. The ones that do not, need "
                   L"these two things — and by then it is too late to get them. "
                   L"We cannot put back a photo that was only ever in one place.",
                   g_f_body, C_FG, x, y, narrow, DT_WORDBREAK) + S(28);

    y += draw_check(ID_CHK_BACKUP, &g_ack_backup,
        L"I have a copy of anything I would hate to lose, somewhere other than this PC.",
        L"An external drive, another computer, or an account like OneDrive, "
        L"iCloud or Google Drive. Photos and documents first.",
        x, y, narrow, C_ACCENT) + S(14);

    y += draw_check(ID_CHK_USB, &g_ack_usb,
        L"I have a USB stick of at least 4 GB that I am happy to erase.",
        L"We turn it into a rescue stick before anything is changed. If this PC "
        L"ever refuses to start, that stick is how you get Windows back. "
        L"You do not need to plug it in yet.",
        x, y, narrow, C_ACCENT) + S(24);

    y += text_draw(L"We ask for both because they cover different accidents. The "
                   L"rescue area on the drive handles \"AurOS will not start\". The "
                   L"USB stick handles \"this PC will not start at all\".",
                   g_f_small, C_MUTED, x, y, narrow, DT_WORDBREAK);
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  5 · CONSENT
 * ═══════════════════════════════════════════════════════════════════ */
static int section_head(const wchar_t *s, uint32_t c, int x, int y, int w)
{
    return text_draw(s, g_f_smallb, c, x, y, w, DT_SINGLELINE) + S(10);
}
static int bullet(const wchar_t *s, uint32_t dot, int x, int y, int w)
{
    fill_circle((float)(x + S(4)), (float)(y + S(9)), (float)S(3), dot, 1.f);
    return text_draw(s, g_f_body, C_FG, x + S(20), y, w - S(20), DT_WORDBREAK) + S(12);
}

static void draw_input(int id, const wchar_t *text, const wchar_t *ph,
                       int x, int y, int w, int h, int ok)
{
    int idx = w_add(id, W_INPUT, x, y, w, h, 1);
    int foc = w_focused(idx);
    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(10), C_BG, 0.85f);
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(10),
              (foc || ok) ? 1.8f : 1.2f,
              ok ? C_ACCENT : (foc ? C_ACCENT : C_OVERLAY), foc || ok ? 0.95f : 1.f);

    int tx = x + S(18);
    RECT b = { tx, y, x + w - S(40), y + h };
    if (text[0]) text_in(text, g_f_input, ok ? C_ACCENT : C_FG_HI, b,
                         DT_SINGLELINE | DT_VCENTER);
    else         text_in(ph, g_f_input, C_MUTED, b, DT_SINGLELINE | DT_VCENTER);

    if (foc && g_caret_on) {
        int cx = tx + (text[0] ? text_w(text, g_f_input) : 0) + S(2);
        fill_rr((float)cx, (float)(y + S(12)), (float)S(2), (float)(h - S(24)),
                1.f, C_ACCENT, 0.95f);
    }
    if (ok)
        glyph_check((float)(x + w - S(24)), (float)(y + h / 2), (float)S(13),
                    C_ACCENT, 1.f);
    if (foc) { RECT r = { x, y, x + w, y + h }; focus_ring(r, S(4), S(14)); }
}

static int page_consent(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(700) ? S(700) : w;

    y += text_draw(L"Exactly what is about to happen", g_f_title, C_FG_HI,
                   x, y, w, DT_WORDBREAK) + S(14);
    y += text_draw(L"Read this properly. It is the whole truth about what this "
                   L"program does to this PC, in order.",
                   g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(26);

    y += section_head(L"WHAT CHANGES", C_WARM, x, y, narrow);
    y += bullet(L"A rescue area is made on the drive first, out of space nobody is "
                L"using. Nothing else happens until that is done.", C_WARM, x, y, narrow);
    y += bullet(L"The part of the drive that Windows uses is made smaller, to free "
                L"up room. Windows files are not deleted and not moved off this PC.",
                C_WARM, x, y, narrow);
    y += bullet(L"A new, separate space is created in that freed-up room, and AurOS "
                L"is copied into it.", C_WARM, x, y, narrow);
    y += bullet(L"The start-up menu changes. From then on, switching this PC on asks "
                L"you which one you want.", C_WARM, x, y, narrow);
    y += S(10);

    y += section_head(L"WHAT STAYS", C_ACCENT, x, y, narrow);
    y += bullet(L"Windows, and everything in it: your files, your programs, your "
                L"settings, your desktop.", C_ACCENT, x, y, narrow);
    y += bullet(L"Windows keeps being the one that starts on its own, until the day "
                L"you tell us otherwise. If AurOS fails to start, this PC comes back "
                L"to Windows by itself.", C_ACCENT, x, y, narrow);
    y += S(10);

    y += section_head(L"HOW TO UNDO IT", C_ACCENT_ALT, x, y, narrow);
    y += bullet(L"Switch the PC on and choose \"Put Windows back\" in the menu. It "
                L"puts the drive back exactly as it is today. No USB stick, no second "
                L"computer, no phone call.", C_ACCENT_ALT, x, y, narrow);
    y += bullet(L"If this PC will not start at all, the rescue USB stick does the "
                L"same job.", C_ACCENT_ALT, x, y, narrow);
    y += S(16);

    fill_rr((float)x, (float)y, (float)narrow, (float)S(2), 1.f, C_OVERLAY, 1.f);
    y += S(24);

    int ok = consent_ok();
    y += text_draw(L"Type AGREE below to say you have read this.", g_f_bodyb,
                   ok ? C_ACCENT : C_FG_HI, x, y, narrow, DT_WORDBREAK) + S(6);
    y += text_draw(L"We ask you to type it rather than tick a box because a box is "
                   L"too easy to tick without reading.",
                   g_f_small, C_MUTED, x, y, narrow, DT_WORDBREAK) + S(14);

    draw_input(ID_INPUT_AGREE, g_agree, L"AGREE", x, y, S(260), S(54), ok);
    y += S(54);
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  6 · CHOOSE
 * ═══════════════════════════════════════════════════════════════════ */
static int choice_card(int id, int selected, const wchar_t *title,
                       const wchar_t *badge, const wchar_t *body,
                       uint32_t tint, int x, int y, int w)
{
    int pad = S(22);
    int rx  = x + pad + S(11);
    int tx  = x + pad + S(38);
    int tw  = w - (tx - x) - pad;
    int th  = text_h(title, g_f_h3, tw, DT_WORDBREAK);
    int bh  = text_h(body, g_f_body, tw, DT_WORDBREAK);
    int h   = pad * 2 + th + bh + S(8);

    int idx = w_add(id, W_CARD, x, y, w, h, 1);
    int hot = w_hot(idx);

    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(14), C_SURFACE_HI,
            selected ? 0.9f : (hot ? 0.6f : 0.35f));
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(14),
              selected ? 1.8f : 1.2f, selected ? tint : C_OVERLAY,
              selected ? 0.9f : 1.f);

    float cy = (float)(y + pad + S(10));
    if (selected) {
        stroke_circle((float)rx, cy, (float)S(10), 1.6f, tint, 1.f);
        fill_circle((float)rx, cy, (float)S(5), tint, 1.f);
    } else {
        stroke_circle((float)rx, cy, (float)S(10), 1.4f, hot ? C_SUBTLE : C_OVERLAY, 1.f);
    }

    int ty = y + pad;
    if (badge) {
        int pw = text_w(badge, g_f_tiny) + S(18);
        int px = x + w - pad - pw;
        fill_rr((float)px, (float)ty, (float)pw, (float)S(21), (float)S(10), tint, 0.18f);
        RECT b = { px, ty, px + pw, ty + S(21) };
        text_in(badge, g_f_tiny, tint, b, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        tw -= pw + S(12);
    }
    text_draw(title, g_f_h3, selected ? C_FG_HI : C_FG, tx, ty, tw, DT_WORDBREAK);
    text_draw(body, g_f_body, C_SUBTLE, tx, ty + th + S(8),
              w - (tx - x) - pad, DT_WORDBREAK);
    if (w_focused(idx)) { RECT b = { x, y, x + w, y + h }; focus_ring(b, S(4), S(18)); }
    return h;
}

static int page_choose(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(720) ? S(720) : w;

    y += text_draw(L"How would you like AurOS installed?", g_f_title, C_FG_HI,
                   x, y, w, DT_WORDBREAK) + S(14);
    y += text_draw(L"You can change this later by reinstalling, but not with one "
                   L"button — so take a moment.",
                   g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(24);

    y += choice_card(ID_CARD_DUAL, g_choice == 0,
        L"Keep Windows, and add AurOS beside it",
        L"RECOMMENDED",
        L"Both stay on this PC. Every time you switch it on you choose which one "
        L"you want, and Windows is the one that starts if you do not choose. "
        L"Nothing in Windows is deleted. AurOS needs about 28 GB of room.",
        C_ACCENT, x, y, narrow) + S(16);

    y += choice_card(ID_CARD_REPLACE, g_choice == 1,
        L"Replace Windows completely",
        NULL,
        L"Everything on this PC is erased: Windows, your programs, and every file "
        L"on this drive. Windows cannot be put back afterwards, and the rescue area "
        L"cannot bring your files back either. Only choose this if everything you "
        L"want is already copied somewhere else.",
        C_ERR, x, y, narrow) + S(16);

    if (g_choice == 1) {
        y += draw_check(ID_CHK_REPLACE, &g_ack_replace,
            L"I understand that everything on this PC will be erased, and that "
            L"nothing on it can be brought back.",
            L"Including Windows, your programs, your documents, your photos and "
            L"anything else on this drive.",
            x, y, narrow, C_ERR) + S(12);
    }
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  7 · DESKTOP — which of the six archetypes this PC will run
 *
 *  docs/SHELLS.md: "AurOS asks one question during setup that no other
 *  operating system asks: when you want to get to a different thing,
 *  what do you do?" This is that page. It is the single most
 *  consequential choice in the product, and the person making it has
 *  never chosen a desktop before and does not know the words for any of
 *  this — so the page is built out of pictures and tradeoffs, not names.
 *
 *  Three rules it is written to, all from that document:
 *    - Every card states its downside as loudly as its upside. The two
 *      strips are the same size, the same weight and the same colour of
 *      text; only the label differs.
 *    - Nothing here names another operating system, desktop or device.
 *      The behaviour is described instead.
 *    - Rail is preselected, because it is the only one in which a thing
 *      cannot be hidden; Workbench is marked as requiring learning, in
 *      those words, so nobody arrives at it by accident.
 * ═══════════════════════════════════════════════════════════════════ */

/* A tiny picture of the layout's shape, drawn with the same primitives
 * as the rest of the wizard. Six paragraphs about window management are
 * six paragraphs nobody reads; six shapes are read at a glance. The
 * frame is one screen, and `tint` marks the thing you are looking at. */
static void shell_diagram(int kind, float x, float y, float w, float h,
                          uint32_t tint)
{
    /* C_MUTED for the things, C_OVERLAY for the surfaces they sit on:
     * at 110x68 px an OVERLAY-on-BG shape is invisible. */
    const uint32_t dim = C_MUTED, bar = C_OVERLAY;
    const float r1 = (float)S(2), r2 = (float)S(3);

    fill_rr(x, y, w, h, (float)S(6), C_BG, 0.92f);
    stroke_rr(x, y, w, h, (float)S(6), 1.f, C_OVERLAY, 1.f);

    switch (kind) {
    case SD_RAIL:
        /* a row of cards, with the neighbours showing at both edges —
         * the absence of anywhere to hide is the whole archetype */
        fill_rr(x + w * 0.02f, y + h * 0.21f, w * 0.16f, h * 0.52f, r2, dim, 0.9f);
        fill_rr(x + w * 0.23f, y + h * 0.12f, w * 0.54f, h * 0.64f, r2, tint, 0.92f);
        fill_rr(x + w * 0.82f, y + h * 0.21f, w * 0.16f, h * 0.52f, r2, dim, 0.9f);
        for (int i = 0; i < 3; i++)
            fill_circle(x + w * (0.42f + 0.08f * (float)i), y + h * 0.88f,
                        h * 0.040f, i == 1 ? tint : dim, i == 1 ? 1.f : 0.7f);
        break;

    case SD_TASKBAR:
        /* two windows, one on top of the other, and a bar along the
         * bottom with a launcher at its left */
        fill_rr(x + w * 0.08f, y + h * 0.11f, w * 0.46f, h * 0.40f, r2, dim, 0.75f);
        fill_rr(x + w * 0.30f, y + h * 0.27f, w * 0.46f, h * 0.40f, r2, tint, 0.90f);
        fill_rr(x + w * 0.04f, y + h * 0.75f, w * 0.92f, h * 0.18f, r2, bar, 1.f);
        fill_rr(x + w * 0.065f, y + h * 0.785f, w * 0.075f, h * 0.11f, r1, tint, 1.f);
        for (int i = 0; i < 3; i++)
            fill_rr(x + w * (0.175f + 0.20f * (float)i), y + h * 0.785f,
                    w * 0.165f, h * 0.11f, r1, dim, 0.85f);
        break;

    case SD_TILES: {
        /* a page of big buttons */
        float cw = w * 0.25f, ch = h * 0.33f;
        for (int i = 0; i < 6; i++)
            fill_rr(x + w * 0.08f + (float)(i % 3) * (cw + w * 0.055f),
                    y + h * 0.13f + (float)(i / 3) * (ch + h * 0.12f),
                    cw, ch, r2, i == 0 ? tint : dim, i == 0 ? 0.92f : 0.8f);
        break;
    }

    case SD_DOCK:
        /* one window, and a strip of favourites that is the same shape
         * whatever is running: four equal squares, always centred */
        fill_rr(x + w * 0.14f, y + h * 0.09f, w * 0.72f, h * 0.53f, r2, dim, 0.8f);
        fill_rr(x + w * 0.17f, y + h * 0.71f, w * 0.66f, h * 0.21f, h * 0.105f,
                bar, 1.f);
        for (int i = 0; i < 4; i++)
            fill_rr(x + w * (0.215f + 0.147f * (float)i), y + h * 0.755f,
                    w * 0.105f, h * 0.12f, r1, tint, 0.88f);
        break;

    case SD_LOCKED:
        /* two big things and a lot of nothing. No bar, no dock, no
         * desktop and no sixth tile waiting off-screen: what is absent
         * from this picture is the archetype. */
        fill_rr(x + w * 0.08f, y + h * 0.14f, w * 0.40f, h * 0.72f, r2, tint, 0.85f);
        fill_rr(x + w * 0.52f, y + h * 0.14f, w * 0.40f, h * 0.72f, r2, tint, 0.45f);
        break;

    case SD_WORKBENCH: {
        /* panes that divide the screen instead of covering it, and the
         * several separate screens, along the foot */
        float g = w * 0.02f;
        fill_rr(x + w * 0.05f, y + h * 0.08f, w * 0.44f, h * 0.70f, r2, dim, 0.85f);
        fill_rr(x + w * 0.51f + g, y + h * 0.08f, w * 0.44f - g, h * 0.33f, r2,
                tint, 0.90f);
        fill_rr(x + w * 0.51f + g, y + h * 0.45f, w * 0.44f - g, h * 0.33f, r2,
                dim, 0.85f);
        for (int i = 0; i < 5; i++)
            fill_rr(x + w * (0.05f + 0.075f * (float)i), y + h * 0.86f,
                    w * 0.05f, h * 0.06f, r1, i == 0 ? tint : dim,
                    i == 0 ? 1.f : 0.6f);
        break;
    }
    default: break;
    }
}

/* One of the two strips at the foot of a card. They are deliberately
 * identical: same fill, same type, same C_FG body. Only the label and
 * its colour differ, because a downside set in small grey print is a
 * downside nobody reads, and a menu of upsides cannot be chosen from.
 *
 * `measure` returns the height without drawing, so the card's geometry
 * and its pixels come from this one function and cannot drift apart. */
static int trait_row(int good, const wchar_t *label, const wchar_t *body,
                     int x, int y, int w, int measure)
{
    uint32_t tint = good ? C_ACCENT : C_WARM;
    int pad = S(9);
    int lw  = S(80);
    int tx  = x + pad + S(24) + lw;
    int tw  = x + w - S(12) - tx;
    if (tw < S(120)) tw = S(120);
    int h = text_h(body, g_f_small, tw, DT_WORDBREAK) + pad * 2;
    if (h < S(34)) h = S(34);
    if (measure) return h;

    /* A rule down the left, not a tinted box.
     *
     * This is now the ONE mechanism the whole product uses to mark a
     * qualification: the refusal page already ruled each blocked reason
     * this way, and the website marks every claim's limit the same. A
     * tinted rounded box here was a third device doing the same job,
     * and three devices for one job is what makes an interface read as
     * assembled rather than designed. */
    fill_rr((float)x, (float)y, (float)S(4), (float)h, 0.f, tint, 1.f);
    float gx = (float)(x + S(4) + pad + S(9)), gy = (float)(y + pad + S(9));
    if (good) glyph_check(gx, gy, (float)S(12), tint, 1.f);
    else      glyph_bang (gx, gy, (float)S(12), tint, 1.f);
    text_draw(label, g_f_tiny, tint, x + S(4) + pad + S(24), y + pad + S(3), lw,
              DT_SINGLELINE);
    text_draw(body, g_f_small, C_FG, tx + S(4), y + pad, tw - S(4), DT_WORDBREAK);
    return h;
}

static int shell_card(int id, const shell_info *s, int selected,
                      int x, int y, int w)
{
    uint32_t tint = s->warn_tint ? C_WARM : C_ACCENT;
    int pad = S(16);
    int rx  = x + pad + S(11);                 /* the radio            */
    int dx  = x + pad + S(34);                 /* the picture          */
    int dw  = S(112), dh = S(68);
    int tx  = dx + dw + S(18);                 /* the words beside it  */
    int tw  = x + w - pad - tx;
    if (tw < S(180)) tw = S(180);

    /* The badge normally shares the name's line. A translation long
     * enough to leave the name unreadably narrow gets its own line
     * instead — it never squeezes or overlaps the name. */
    int bh = S(21);
    int bw = s->badge ? text_w(s->badge, g_f_tiny) + S(18) : 0;
    int badge_below = bw && bw > tw - S(170);
    int nw = (bw && !badge_below) ? tw - bw - S(12) : tw;

    int nh   = text_h(s->name, g_f_h3,   nw, DT_WORDBREAK);
    int wh   = text_h(s->what, g_f_body, tw, DT_WORDBREAK);
    int head = nh + (badge_below ? bh + S(6) : 0) + S(6) + wh;
    if (head < dh) head = dh;

    int sx = dx, sw = x + w - pad - dx;
    int b1 = trait_row(1, L"BEST FOR",  s->best_for, sx, 0, sw, 1);
    int b2 = trait_row(0, L"THE CATCH", s->tradeoff, sx, 0, sw, 1);
    int h  = pad + head + S(11) + b1 + S(5) + b2 + pad;

    int idx = w_add(id, W_CARD, x, y, w, h, 1);
    int hot = w_hot(idx);

    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(14), C_SURFACE_HI,
            selected ? 0.9f : (hot ? 0.6f : 0.35f));
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(14),
              selected ? 1.8f : 1.2f, selected ? tint : C_OVERLAY,
              selected ? 0.9f : 1.f);

    float cy = (float)(y + pad + S(12));
    if (selected) {
        stroke_circle((float)rx, cy, (float)S(10), 1.6f, tint, 1.f);
        fill_circle((float)rx, cy, (float)S(5), tint, 1.f);
    } else {
        stroke_circle((float)rx, cy, (float)S(10), 1.4f,
                      hot ? C_SUBTLE : C_OVERLAY, 1.f);
    }

    shell_diagram(s->diagram, (float)dx, (float)(y + pad), (float)dw, (float)dh,
                  tint);

    int wy = y + pad + nh + S(6);
    if (s->badge) {
        int px = badge_below ? tx : x + w - pad - bw;
        int py = badge_below ? y + pad + nh + S(4) : y + pad + S(2);
        fill_rr((float)px, (float)py, (float)bw, (float)bh,
                (float)S(10), tint, 0.18f);
        RECT b = { px, py, px + bw, py + bh };
        text_in(s->badge, g_f_tiny, tint, b, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        if (badge_below) wy += bh + S(6);
    }
    text_draw(s->name, g_f_h3, selected ? C_FG_HI : C_FG, tx, y + pad, nw,
              DT_WORDBREAK);
    text_draw(s->what, g_f_body, C_SUBTLE, tx, wy, tw, DT_WORDBREAK);

    int sy = y + pad + head + S(11);
    sy += trait_row(1, L"BEST FOR",  s->best_for, sx, sy, sw, 0) + S(5);
    trait_row(0, L"THE CATCH", s->tradeoff, sx, sy, sw, 0);

    if (w_focused(idx)) { RECT b = { x, y, x + w, y + h }; focus_ring(b, S(4), S(18)); }
    return h;
}

static int page_desktop(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(800) ? S(800) : w;

    y += text_draw(L"How should this computer work?", g_f_title, C_FG_HI,
                   x, y, w, DT_WORDBREAK) + S(12);
    y += text_draw(L"When you want to get to a different thing, what do you do? "
                   L"That one answer decides how a computer feels, more than "
                   L"anything else does — so we ask, rather than decide for you. "
                   L"You can change it afterwards, at any time.",
                   g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(12);
    y += text_draw(L"Each one below says what it is good at and what it is not. "
                   L"The second half is the one that tells them apart — and if "
                   L"none of it means much to you, leave the first one chosen.",
                   g_f_small, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(20);

    for (int i = 0; i < N_SHELLS; i++) {
        y += shell_card(ID_SHELL + i, &SHELLS[i], g_sel_shell == i, x, y, narrow);
        if (i < N_SHELLS - 1) y += S(14);
    }
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  8 · PERSONALIZE
 * ═══════════════════════════════════════════════════════════════════ */
static int chip_row(int base, wchar_t (*items)[96], int n, int sel,
                    int x, int y, int w)
{
    int cx = x, cy = y, gap = S(8), rowh = S(34);
    for (int i = 0; i < n; i++) {
        int cw = text_w(items[i], g_f_small) + S(28);
        if (cx > x && cx + cw > x + w) { cx = x; cy += rowh + gap; }
        draw_chip(base + i, items[i], cx, cy, i == sel);
        cx += cw + gap;
    }
    return cy + rowh - y;
}

static void theme_preview(float x, float y, float w, float h, const theme_info *t,
                          int selected)
{
    fill_rr(x, y, w, h, (float)S(10), t->bg, 1.f);
    /* a bar, a card and an accent pill: enough to read the theme at a glance */
    fill_rr(x + w * 0.08f, y + h * 0.14f, w * 0.84f, h * 0.16f,
            (float)S(4), t->surface, 1.f);
    fill_rr(x + w * 0.11f, y + h * 0.185f, w * 0.18f, h * 0.06f,
            (float)S(3), t->accent, 1.f);
    fill_rr(x + w * 0.08f, y + h * 0.40f, w * 0.52f, h * 0.42f,
            (float)S(6), t->surface, 1.f);
    fill_rr(x + w * 0.64f, y + h * 0.40f, w * 0.28f, h * 0.20f,
            (float)S(6), t->accent, 0.85f);
    fill_rr(x + w * 0.64f, y + h * 0.66f, w * 0.28f, h * 0.16f,
            (float)S(6), t->accent_alt, 0.75f);
    stroke_rr(x, y, w, h, (float)S(10), 1.f,
              selected ? t->accent : C_OVERLAY, selected ? 0.9f : 1.f);
}

static int theme_card(int id, const theme_info *t, int selected, int x, int y, int w)
{
    int ph = S(86);
    int h  = ph + S(58);
    int idx = w_add(id, W_CARD, x, y, w, h, 1);
    int hot = w_hot(idx);

    fill_rr((float)x - 1.f, (float)y - 1.f, (float)w + 2.f, (float)h + 2.f,
            (float)S(13), C_SURFACE_HI, selected ? 0.9f : (hot ? 0.6f : 0.3f));
    if (selected)
        stroke_rr((float)x - 1.f, (float)y - 1.f, (float)w + 2.f, (float)h + 2.f,
                  (float)S(13), 1.8f, C_ACCENT, 0.9f);

    theme_preview((float)(x + S(8)), (float)(y + S(8)), (float)(w - S(16)),
                  (float)(ph - S(8)), t, selected);

    text_draw(t->name, g_f_bodyb, selected ? C_FG_HI : C_FG,
              x + S(12), y + ph + S(4), w - S(24), DT_SINGLELINE);

    /* the swatch: bg, surface, accent, accent_alt, straight out of the
     * .theme file so the picker cannot drift from the real palette */
    uint32_t sw[4] = { t->bg, t->surface, t->accent, t->accent_alt };
    for (int i = 0; i < 4; i++) {
        float cx = (float)(x + S(16) + i * S(16));
        fill_circle(cx, (float)(y + ph + S(34)), (float)S(6), sw[i], 1.f);
        stroke_circle(cx, (float)(y + ph + S(34)), (float)S(6), 1.f, C_OVERLAY, 0.8f);
    }
    if (!t->dark)
        text_draw(L"light", g_f_tiny, C_MUTED, x + S(86), y + ph + S(28),
                  w - S(90), DT_SINGLELINE);
    if (w_focused(idx)) { RECT b = { x, y, x + w, y + h }; focus_ring(b, S(4), S(17)); }
    return h;
}

static int page_personalize(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(760) ? S(760) : w;

    y += text_draw(L"Make it yours", g_f_title, C_FG_HI, x, y, w, DT_WORDBREAK) + S(12);
    y += text_draw(L"We copied these from Windows, so they are probably right "
                   L"already. All of them can be changed later.",
                   g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(24);

    y += section_head(L"LANGUAGE", C_SUBTLE, x, y, narrow);
    y += chip_row(ID_LANG, g_langs, g_n_langs, g_sel_lang, x, y, narrow) + S(22);

    y += section_head(L"KEYBOARD", C_SUBTLE, x, y, narrow);
    y += chip_row(ID_KBD, g_kbds, g_n_kbds, g_sel_kbd, x, y, narrow) + S(22);

    y += section_head(L"TIME ZONE", C_SUBTLE, x, y, narrow);
    y += chip_row(ID_TZ, g_tzs, g_n_tzs, g_sel_tz, x, y, narrow) + S(22);

    y += section_head(L"LOOK", C_SUBTLE, x, y, narrow);
    {
        int gap = S(14);
        int cw  = (narrow - gap * (N_THEMES - 1)) / N_THEMES;
        int hh  = 0;
        for (int i = 0; i < N_THEMES; i++) {
            int h = theme_card(ID_THEME + i, &THEMES[i], g_sel_theme == i,
                               x + i * (cw + gap), y, cw);
            if (h > hh) hh = h;
        }
        y += hh + S(10);
    }
    y += text_draw(THEMES[g_sel_theme].desc, g_f_small, C_SUBTLE, x, y, narrow,
                   DT_WORDBREAK);
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  9 · READY
 * ═══════════════════════════════════════════════════════════════════ */
static const pf_disk *sys_disk(void)
{
    if (g_report.system_disk >= 0 && g_report.system_disk < g_report.n_disks)
        return &g_report.disks[g_report.system_disk];
    return NULL;
}
static const pf_volume *sys_volume(void)
{
    for (int i = 0; i < g_report.n_volumes; i++)
        if (g_report.volumes[i].mount[0] == 'C') return &g_report.volumes[i];
    return g_report.n_volumes ? &g_report.volumes[0] : NULL;
}

static int sum_row(const wchar_t *label, const wchar_t *value, uint32_t vc,
                   int x, int y, int w)
{
    int lw = S(170);
    int vh = text_draw(value, g_f_body, vc, x + lw, y, w - lw, DT_WORDBREAK);
    text_draw(label, g_f_small, C_SUBTLE, x, y + S(2), lw - S(14), DT_WORDBREAK);
    return vh + S(16);
}

static int page_ready(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(720) ? S(720) : w;
    wchar_t buf[256], sz[32];

    y += text_draw(L"Ready when you are", g_f_title, C_FG_HI, x, y, w,
                   DT_WORDBREAK) + S(12);
    y += text_draw(L"This is everything that is about to happen. Nothing has been "
                   L"changed on this PC yet.",
                   g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(26);

    const pf_disk *d = sys_disk();
    if (d) {
        wchar_t model[160];
        a2w(d->model[0] ? d->model : "(unnamed drive)", model, 160);
        human_size(d->size_bytes, sz, 32);
        _snwprintf(buf, 255, L"%s — %s", model, sz);
    } else {
        wcscpy(buf, L"the drive Windows starts from");
    }
    buf[255] = 0;
    y += sum_row(L"The drive we will use", buf, C_FG_HI, x, y, narrow);

    y += sum_row(L"What we will do",
                 g_choice == 0
                   ? L"Install AurOS next to Windows, and let you choose at start-up."
                   : L"Erase this PC completely and install only AurOS.",
                 g_choice == 0 ? C_FG : C_ERR, x, y, narrow);

    const pf_volume *v = sys_volume();
    if (g_choice == 0 && v) {
        human_size(v->free_bytes, sz, 32);
        _snwprintf(buf, 255, L"About 28 GB, taken from the %s of empty space on "
                             L"drive %hs. Your Windows files stay where they are.",
                   sz, v->mount);
        buf[255] = 0;
        y += sum_row(L"Room for AurOS", buf, C_FG, x, y, narrow);
    }

    y += sum_row(L"Windows",
                 g_choice == 0
                   ? L"Stays, with all of its files, and keeps starting by default "
                     L"until you say otherwise."
                   : L"Will be erased, along with everything else on this drive.",
                 g_choice == 0 ? C_ACCENT : C_ERR, x, y, narrow);

    y += sum_row(L"If something goes wrong",
                 L"A rescue area is built on the drive before anything else, and a "
                 L"rescue USB stick alongside it. Either one puts this PC back.",
                 C_FG, x, y, narrow);

    _snwprintf(buf, 255, L"%s  ·  %s keyboard  ·  %s",
               g_langs[g_sel_lang], g_kbds[g_sel_kbd], g_tzs[g_sel_tz]);
    buf[255] = 0;
    y += sum_row(L"Language and region", buf, C_FG, x, y, narrow);
    y += sum_row(L"How the desktop works", SHELLS[g_sel_shell].name,
                 C_FG, x, y, narrow);
    y += sum_row(L"Look", THEMES[g_sel_theme].name, C_FG, x, y, narrow);
    y += sum_row(L"How long",
                 L"About 40 minutes. This PC restarts once part way through, on its "
                 L"own.", C_FG, x, y, narrow);

    y += S(8);
    if (g_choice == 1) {
        int h = S(64);
        fill_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(12), C_ERR, 0.12f);
        stroke_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(12), 1.2f,
                  C_ERR, 0.4f);
        text_draw(L"Everything on this PC will be erased and cannot be brought back.",
                  g_f_bodyb, C_ERR, x + S(20), y + S(12), narrow - S(40), DT_WORDBREAK);
        text_draw(L"There is no way back from this one.",
                  g_f_small, C_ERR, x + S(20), y + S(36), narrow - S(40), DT_SINGLELINE);
        y += h + S(16);
    }

    if (d) {
        human_size(d->size_bytes, sz, 32);
        wchar_t model[160];
        a2w(d->model[0] ? d->model : "the drive Windows starts from", model, 160);
        _snwprintf(buf, 255, L"Yes: %s (%s) is the drive I want to change.", model, sz);
    } else {
        wcscpy(buf, L"Yes: the drive Windows starts from is the one I want to change.");
    }
    buf[255] = 0;
    y += draw_check(ID_CHK_READY, &g_ready_confirm, buf,
                    L"If this PC has more than one drive, check the name and the size "
                    L"above. Only this drive is touched.",
                    x, y, narrow, g_choice == 1 ? C_ERR : C_ACCENT) + S(18);

    y += text_draw(L"You can still stop. Nothing is changed until the rescue area is "
                   L"finished, and nothing is permanent until AurOS has started and "
                   L"you have told us it works.",
                   g_f_small, C_MUTED, x, y, narrow, DT_WORDBREAK);
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 * 10 · PROGRESS   (every phase below is a stub; see PHASE_STUB[])
 * ═══════════════════════════════════════════════════════════════════ */
static int page_progress(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(760) ? S(760) : w;

    y += text_draw(g_install_finished ? L"That is as far as this build goes"
                                      : L"Setting up AurOS",
                   g_f_title, C_FG_HI, x, y, w, DT_WORDBREAK) + S(12);

    /* Honesty banner. This build cannot change anything, and the screen
     * that looks the most like a real install is the one that must say so. */
    {
        int h = S(56);
        fill_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(12), C_WARM, 0.12f);
        stroke_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(12), 1.2f,
                  C_WARM, 0.35f);
        glyph_bang((float)(x + S(26)), (float)(y + h / 2), (float)S(15), C_WARM, 1.f);
        text_draw(L"Stub build: every step below is simulated. Nothing on this PC is "
                  L"opened for writing, and nothing is changed.",
                  g_f_small, C_WARM, x + S(48), y + S(11), narrow - S(70), DT_WORDBREAK);
        y += h + S(22);
    }

    for (int i = 0; i < N_PHASES; i++) {
        int st = g_ph[i];
        int rh = S(58);
        uint32_t tc = st == PH_DONE ? C_FG : (st == PH_RUNNING ? C_FG_HI : C_MUTED);

        if (st == PH_RUNNING)
            fill_rr((float)x, (float)y, (float)narrow, (float)rh, (float)S(10),
                    C_ACCENT, 0.07f);

        float cx = (float)(x + S(18)), cy = (float)(y + rh / 2);
        if (st == PH_DONE) {
            fill_circle(cx, cy, (float)S(11), C_ACCENT, 0.9f);
            glyph_check(cx, cy, (float)S(14), C_BG, 1.f);
        } else if (st == PH_RUNNING) {
            status_icon(cx, cy, (float)S(11), 1, PF_PASS);
        } else {
            stroke_circle(cx, cy, (float)S(10), 1.3f, C_OVERLAY, 1.f);
            wchar_t n[4]; _snwprintf(n, 3, L"%d", i + 1); n[3] = 0;
            RECT b = { x + S(8), y + rh / 2 - S(9), x + S(28), y + rh / 2 + S(9) };
            text_in(n, g_f_tiny, C_MUTED, b, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }

        int tx = x + S(46);
        text_draw(PHASES[i].name, g_f_bodyb, tc, tx, y + S(8),
                  narrow - S(160), DT_SINGLELINE);
        text_draw(PHASES[i].desc, g_f_small,
                  st == PH_PENDING || st == PH_LATER ? C_MUTED : C_SUBTLE,
                  tx, y + S(28), narrow - S(160), DT_SINGLELINE);

        if (st == PH_LATER) {
            const wchar_t *tag = L"after the restart";
            int pw = text_w(tag, g_f_tiny) + S(18);
            RECT b = { x + narrow - pw, y + S(14), x + narrow, y + S(35) };
            fill_rr((float)b.left, (float)b.top, (float)pw, (float)S(21),
                    (float)S(10), C_ACCENT_ALT, 0.12f);
            text_in(tag, g_f_tiny, C_ACCENT_ALT, b, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        if (st == PH_RUNNING) {
            int bw = S(96), bx = x + narrow - bw - S(6);
            fill_rr((float)bx, (float)(y + rh / 2 - S(3)), (float)bw, (float)S(6),
                    (float)S(3), C_OVERLAY, 1.f);
            fill_rr((float)bx, (float)(y + rh / 2 - S(3)),
                    (float)bw * clampf(g_ph_prog, 0.f, 1.f), (float)S(6),
                    (float)S(3), C_ACCENT, 1.f);
        }
        y += rh + S(2);
    }

    y += S(18);
    {
        int lines = 7;
        int lh = S(19);
        int h = lines * lh + S(28);
        fill_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(12), C_BG, 0.55f);
        stroke_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(12), 1.f,
                  C_OVERLAY, 1.f);
        int first = g_log_n > lines ? g_log_n - lines : 0;
        for (int i = first; i < g_log_n; i++)
            text_draw(g_log[i], g_f_tiny, C_MUTED, x + S(16),
                      y + S(14) + (i - first) * lh, narrow - S(32), DT_SINGLELINE);
        y += h;
    }
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Footer, and what the primary button means on each page
 * ═══════════════════════════════════════════════════════════════════ */
static int primary_enabled(void)
{
    switch (g_page) {
    case PAGE_WELCOME:     return 1;
    case PAGE_CHECKING:    return g_pf_valid && g_reveal >= N_CHK && pf_is_go(&g_report);
    case PAGE_BLOCKED:     return 1;
    case PAGE_BACKUP:      return nav_allowed(PAGE_CONSENT);
    case PAGE_CONSENT:     return nav_allowed(PAGE_CHOOSE);
    case PAGE_CHOOSE:      return nav_allowed(PAGE_DESKTOP);
    case PAGE_DESKTOP:     return nav_allowed(PAGE_PERSONALIZE);
    case PAGE_PERSONALIZE: return nav_allowed(PAGE_READY);
    case PAGE_READY:       return nav_allowed(PAGE_PROGRESS);
    case PAGE_PROGRESS:    return g_install_finished;
    default:               return 0;
    }
}

static const wchar_t *primary_label(void)
{
    switch (g_page) {
    case PAGE_WELCOME:     return L"Get started";
    case PAGE_CHECKING:    return L"Continue";
    case PAGE_BLOCKED:     return L"Check again";
    case PAGE_BACKUP:      return L"Continue";
    case PAGE_CONSENT:     return L"I agree — continue";
    case PAGE_CHOOSE:      return L"Continue";
    case PAGE_DESKTOP:     return L"Continue";
    case PAGE_PERSONALIZE: return L"Continue";
    case PAGE_READY:       return g_choice == 1 ? L"Erase and install"
                                                : L"Start installing";
    case PAGE_PROGRESS:    return L"Close";
    default:               return L"Continue";
    }
}

static const wchar_t *footer_hint(void)
{
    switch (g_page) {
    case PAGE_WELCOME:
        return L"Nothing is changed until you have read what will happen and said yes.";
    case PAGE_CHECKING:
        return L"Reading only. Nothing on this PC is written to.";
    case PAGE_BLOCKED:
        return L"Nothing has been changed. You can close this and keep using Windows.";
    case PAGE_BACKUP:
        return L"Both need to be true before we can go on.";
    case PAGE_CONSENT:
        return L"Type AGREE above to continue.";
    case PAGE_CHOOSE:
        return g_choice == 1 ? L"This choice erases everything on this PC."
                             : L"Windows is kept, and stays the default.";
    case PAGE_DESKTOP:
        return L"Any of these can be changed later.";
    case PAGE_PERSONALIZE:
        return L"All of this can be changed later.";
    case PAGE_READY:
        return L"Last chance to stop without anything having happened.";
    case PAGE_PROGRESS:
        return L"Stub build — nothing on this PC has been changed.";
    default: return L"";
    }
}

static int back_visible(void)
{
    switch (g_page) {
    case PAGE_WELCOME: case PAGE_BLOCKED: case PAGE_PROGRESS: return 0;
    default: return 1;
    }
}
static page_id back_target(void)
{
    switch (g_page) {
    case PAGE_CHECKING:    return PAGE_WELCOME;
    case PAGE_BACKUP:      return PAGE_CHECKING;
    case PAGE_CONSENT:     return PAGE_BACKUP;
    case PAGE_CHOOSE:      return PAGE_CONSENT;
    case PAGE_DESKTOP:     return PAGE_CHOOSE;
    case PAGE_PERSONALIZE: return PAGE_DESKTOP;
    case PAGE_READY:       return PAGE_PERSONALIZE;
    default:               return PAGE_WELCOME;
    }
}

static void draw_footer(void)
{
    int pad = S(44);
    aa_line((float)(g_card.left + pad), (float)g_foot.top,
            (float)(g_card.right - pad), (float)g_foot.top, 1.f, C_OVERLAY, 1.f);

    int bh = S(48);
    int by = g_foot.top + (g_foot.bottom - g_foot.top - bh) / 2;
    int bx = g_card.right - pad;

    const wchar_t *plab = primary_label();
    int pw = text_w(plab, g_f_bodyb) + S(56);
    if (pw < S(150)) pw = S(150);
    bx -= pw;
    uint32_t tone = (g_page == PAGE_READY && g_choice == 1) ? C_ERR : C_ACCENT;
    draw_button(ID_PRIMARY, plab, bx, by, pw, bh, 1, primary_enabled(), tone);

    if (g_page == PAGE_BLOCKED) {
        int sw = S(120);
        bx -= sw + S(12);
        draw_button(ID_QUIT, L"Close", bx, by, sw, bh, 0, 1, C_ACCENT);
    } else if (back_visible()) {
        int sw = S(110);
        bx -= sw + S(12);
        draw_button(ID_BACK, L"Back", bx, by, sw, bh, 0, 1, C_ACCENT);
    }

    RECT h = { g_card.left + pad, g_foot.top, bx - S(24), g_foot.bottom };
    if (h.right > h.left + S(80))
        text_in(footer_hint(), g_f_small, C_MUTED, h,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Render
 * ═══════════════════════════════════════════════════════════════════ */
static int g_want_focus_id;
static int g_hot_idx = -1;
static int g_settle;

static int page_dispatch(int x, int y, int w)
{
    switch (g_page) {
    case PAGE_WELCOME:     return page_welcome(x, y, w);
    case PAGE_CHECKING:    return page_checking(x, y, w);
    case PAGE_BLOCKED:     return page_blocked(x, y, w);
    case PAGE_BACKUP:      return page_backup(x, y, w);
    case PAGE_CONSENT:     return page_consent(x, y, w);
    case PAGE_CHOOSE:      return page_choose(x, y, w);
    case PAGE_DESKTOP:     return page_desktop(x, y, w);
    case PAGE_PERSONALIZE: return page_personalize(x, y, w);
    case PAGE_READY:       return page_ready(x, y, w);
    case PAGE_PROGRESS:    return page_progress(x, y, w);
    default:               return 0;
    }
}

static int scroll_max(void)
{
    int m = g_content_h - g_view_h;
    return m > 0 ? m : 0;
}

static void render(void)
{
    if (!g_px) return;
    layout();
    clip_reset();
    backdrop_blit();
    draw_rail();

    fill_rr((float)g_card.left, (float)g_card.top,
            (float)(g_card.right - g_card.left), (float)(g_card.bottom - g_card.top),
            (float)S(18), C_SURFACE, 0.94f);
    stroke_rr((float)g_card.left, (float)g_card.top,
              (float)(g_card.right - g_card.left), (float)(g_card.bottom - g_card.top),
              (float)S(18), 1.2f, C_OVERLAY, 0.9f);

    g_nw = 0;
    if (g_scroll[g_page] > scroll_max()) g_scroll[g_page] = scroll_max();
    set_clip(g_card.left + S(2), g_card.top + S(2), g_card.right - S(2), g_foot.top - S(2));
    g_view_h    = g_body.bottom - g_body.top;
    g_content_h = page_dispatch(g_body.left, g_body.top - g_scroll[g_page],
                                g_body.right - g_body.left);
    clip_reset();

    if (g_content_h > g_view_h) {
        /* fade into the card edge rather than shearing text at it */
        int fx = g_card.left + S(3), fw = g_card.right - g_card.left - S(6);
        int fh = S(22);
        if (g_scroll[g_page] > 0)
            for (int i = 0; i < fh; i++)
                fill_rr((float)fx, (float)(g_card.top + S(2) + i), (float)fw, 1.f,
                        0.f, C_SURFACE, 0.97f * (1.f - (float)i / (float)fh));
        if (g_scroll[g_page] < scroll_max())
            for (int i = 0; i < fh; i++)
                fill_rr((float)fx, (float)(g_foot.top - S(3) - i), (float)fw, 1.f,
                        0.f, C_SURFACE, 0.97f * (1.f - (float)i / (float)fh));

        int tx = g_card.right - S(13);
        int th = S(6);
        float frac = (float)g_view_h / (float)g_content_h;
        int hh = (int)((float)(g_body.bottom - g_body.top) * frac);
        if (hh < S(40)) hh = S(40);
        int room = (g_body.bottom - g_body.top) - hh;
        int off  = scroll_max() ? (int)((float)room * (float)g_scroll[g_page]
                                        / (float)scroll_max()) : 0;
        fill_rr((float)tx, (float)g_body.top, (float)th,
                (float)(g_body.bottom - g_body.top), (float)th * 0.5f,
                C_OVERLAY, 0.5f);
        fill_rr((float)tx, (float)(g_body.top + off), (float)th, (float)hh,
                (float)th * 0.5f, C_SUBTLE, 0.8f);
    }

    draw_footer();

    if (g_want_focus_id) {
        for (int i = 0; i < g_nw; i++)
            if (g_w[i].id == g_want_focus_id && g_w[i].enabled) { g_focus = i; break; }
        g_want_focus_id = 0;
    }
    if (g_focus >= g_nw) g_focus = -1;
    GdiFlush();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Actions
 * ═══════════════════════════════════════════════════════════════════ */
static void start_check(page_id next)
{
    g_check_next = next;
    g_settle = 0;
    pf_start();
    goto_page(PAGE_CHECKING);
}

static void check_advance(void)
{
    if (!g_pf_valid) return;
    if (g_report.n_block > 0) { goto_page(PAGE_BLOCKED); return; }
    if (g_check_next == PAGE_PROGRESS) {
        /* Re-ran preflight and it is still clean: this is the only place
         * the phase list is allowed to start. */
        if (!nav_allowed(PAGE_PROGRESS)) { goto_page(PAGE_READY); return; }
        install_begin();
        goto_page(PAGE_PROGRESS);
        return;
    }
    g_want_focus_id = ID_PRIMARY;      /* Continue is now the obvious move */
}

static void do_primary(void)
{
    if (!primary_enabled()) { MessageBeep(MB_ICONASTERISK); return; }
    switch (g_page) {
    case PAGE_WELCOME:     start_check(PAGE_BACKUP); break;
    case PAGE_BLOCKED:     start_check(PAGE_BACKUP); break;
    case PAGE_CHECKING:    check_advance(); if (g_page == PAGE_CHECKING)
                               goto_page(PAGE_BACKUP);
                           break;
    case PAGE_BACKUP:      goto_page(PAGE_CONSENT);
                           g_want_focus_id = ID_INPUT_AGREE; break;
    case PAGE_CONSENT:     goto_page(PAGE_CHOOSE); break;
    case PAGE_CHOOSE:      goto_page(PAGE_DESKTOP); break;
    case PAGE_DESKTOP:     goto_page(PAGE_PERSONALIZE); break;
    case PAGE_PERSONALIZE: goto_page(PAGE_READY); break;
    /* Re-run every safety check before the phase list starts, per
     * AURBRIDGE.md: destructive work re-runs preflight and aborts on
     * any block, however long the user spent on the pages in between. */
    case PAGE_READY:       start_check(PAGE_PROGRESS); break;
    case PAGE_PROGRESS:    PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

static void widget_activate(int id)
{
    switch (id) {
    case ID_PRIMARY: do_primary(); return;
    case ID_BACK:    goto_page(back_target()); return;
    case ID_QUIT:    PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return;
    case ID_CHK_BACKUP:  g_ack_backup   = !g_ack_backup;   return;
    case ID_CHK_USB:     g_ack_usb      = !g_ack_usb;      return;
    case ID_CHK_REPLACE: g_ack_replace  = !g_ack_replace;  return;
    case ID_CHK_READY:   g_ready_confirm= !g_ready_confirm;return;
    case ID_CARD_DUAL:   g_choice = 0; g_ack_replace = 0;  return;
    case ID_CARD_REPLACE:g_choice = 1;                     return;
    case ID_INPUT_AGREE: return;                 /* click just takes focus */
    default: break;
    }
    if (id >= ID_SHELL) { g_sel_shell = id - ID_SHELL; return; }
    if (id >= ID_THEME) { g_sel_theme = id - ID_THEME; return; }
    if (id >= ID_TZ)    { g_sel_tz    = id - ID_TZ;    return; }
    if (id >= ID_KBD)   { g_sel_kbd   = id - ID_KBD;   return; }
    if (id >= ID_LANG)  { g_sel_lang  = id - ID_LANG;  return; }
}

static void scroll_by(int dy)
{
    int m = scroll_max();
    g_scroll[g_page] += dy;
    if (g_scroll[g_page] > m) g_scroll[g_page] = m;
    if (g_scroll[g_page] < 0) g_scroll[g_page] = 0;
}

static void focus_visible(void)
{
    if (g_focus < 0 || g_focus >= g_nw) return;
    int id = g_w[g_focus].id;
    if (id == ID_PRIMARY || id == ID_BACK || id == ID_QUIT) return;
    RECT r = g_w[g_focus].r;
    if (r.top < g_body.top)        scroll_by(r.top - g_body.top - S(12));
    else if (r.bottom > g_body.bottom) scroll_by(r.bottom - g_body.bottom + S(12));
}

static void focus_step(int dir)
{
    if (g_nw <= 0) return;
    int i = (g_focus < 0) ? (dir > 0 ? -1 : 0) : g_focus;
    for (int n = 0; n < g_nw; n++) {
        i = (i + dir + g_nw) % g_nw;
        if (g_w[i].enabled) { g_focus = i; break; }
    }
    g_focus_ring = 1;
    focus_visible();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Device plumbing: DPI, fonts, back buffer
 * ═══════════════════════════════════════════════════════════════════ */
typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
typedef BOOL (WINAPI *PFN_SPDA)(void);
typedef UINT (WINAPI *PFN_GDFW)(HWND);
typedef UINT (WINAPI *PFN_GDFS)(void);
typedef HRESULT (WINAPI *PFN_DWMSWA)(HWND, DWORD, LPCVOID, DWORD);

static void dpi_opt_in(void)
{
    /* Newest API first, then the Win7 one, both through GetProcAddress so
     * the binary still loads on a machine that has neither. */
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_SPDAC f = (PFN_SPDAC)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (f && f((HANDLE)(INT_PTR)-4)) return;   /* PER_MONITOR_AWARE_V2 */
        PFN_SPDA g = (PFN_SPDA)(void *)GetProcAddress(u, "SetProcessDPIAware");
        if (g) g();
    }
}

static int dpi_for(HWND h)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_GDFW f = (PFN_GDFW)(void *)GetProcAddress(u, "GetDpiForWindow");
        if (f && h) { UINT d = f(h); if (d >= 72) return (int)d; }
        PFN_GDFS g = (PFN_GDFS)(void *)GetProcAddress(u, "GetDpiForSystem");
        if (g) { UINT d = g(); if (d >= 72) return (int)d; }
    }
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d >= 72 ? d : 96;
}

static void dark_titlebar(HWND h)
{
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (!m) return;
    PFN_DWMSWA f = (PFN_DWMSWA)(void *)GetProcAddress(m, "DwmSetWindowAttribute");
    BOOL on = TRUE;
    if (f) { f(h, 20, &on, sizeof on); f(h, 19, &on, sizeof on); }
    FreeLibrary(m);
}

/* Two faces, which IS the hierarchy.
 *
 * This had one face at essentially one weight: title, h2 and h3 were
 * all Segoe UI SemiBold at 30/20/17, so the only thing separating a
 * page title from a subheading was three points of size. That is not a
 * hierarchy, it is a size ramp.
 *
 * The display face is Georgia. It is on every Windows since 2000 and
 * every Mac, so nothing is downloaded and nothing falls back; Matthew
 * Carter drew it to hold up on bad screens, which is exactly the
 * hardware this installer runs on; and it is the same face the website
 * uses, so the thing a person downloads looks like the page they
 * downloaded it from. Segoe UI stays for text, where its job is to
 * disappear.
 *
 * The weights are deliberately uneven: the serif carries its emphasis
 * through SIZE at weight 400, and the sans carries its through WEIGHT
 * at a small size. Big-and-quiet against small-and-loud is contrast
 * that one face at 600/620/650 cannot produce. */
#define FACE_DISPLAY L"Georgia"
#define FACE_TEXT    L"Segoe UI"

static HFONT mkfont_face(int px, int weight, const wchar_t *face, int family)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       (BYTE)(VARIABLE_PITCH | family), face);
}
static HFONT mkfont(int px, int weight)
{
    return mkfont_face(px, weight, FACE_TEXT, FF_SWISS);
}
static HFONT mkdisplay(int px, int weight)
{
    return mkfont_face(px, weight, FACE_DISPLAY, FF_ROMAN);
}
static void fonts_free(void)
{
    HFONT *all[] = { &g_f_title, &g_f_h2, &g_f_h3, &g_f_body, &g_f_bodyb,
                     &g_f_small, &g_f_smallb, &g_f_tiny, &g_f_input };
    for (int i = 0; i < (int)(sizeof all / sizeof all[0]); i++)
        if (*all[i]) { DeleteObject(*all[i]); *all[i] = NULL; }
}
static void fonts_make(void)
{
    fonts_free();
    /* Display, large and quiet. */
    g_f_title  = mkdisplay(34, FW_NORMAL);
    g_f_h2     = mkdisplay(23, FW_NORMAL);
    /* Text, small and loud. The jump between the two IS the hierarchy. */
    g_f_h3     = mkfont(15, FW_BOLD);
    g_f_body   = mkfont(15, FW_NORMAL);
    g_f_bodyb  = mkfont(15, FW_SEMIBOLD);
    g_f_small  = mkfont(13, FW_NORMAL);
    g_f_smallb = mkfont(13, FW_BOLD);
    g_f_tiny   = mkfont(11, FW_BOLD);
    g_f_input  = mkfont(20, FW_SEMIBOLD);
}

static void backbuffer(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g_mdc && g_mw == w && g_mh == h) return;
    if (g_mdc) {
        SelectObject(g_mdc, g_moldbmp);
        DeleteObject(g_mbmp);
        DeleteDC(g_mdc);
        g_mdc = NULL; g_mbmp = NULL; g_px = NULL;
    }
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(NULL);
    g_mdc  = CreateCompatibleDC(sdc);
    g_mbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    if (sdc) ReleaseDC(NULL, sdc);
    if (g_mdc && g_mbmp) {
        g_moldbmp = (HBITMAP)SelectObject(g_mdc, g_mbmp);
        SetBkMode(g_mdc, TRANSPARENT);
        SetStretchBltMode(g_mdc, HALFTONE);
    }
    g_mw = w; g_mh = h;
    clip_reset();
}

/* Dev harness: dump the back buffer. Used by --shot/--selftest only. */
static int save_bmp(const char *path)
{
    if (!g_px) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t bytes = (uint32_t)g_mw * (uint32_t)g_mh * 4u;
    uint16_t tag = 0x4D42, zero = 0;
    uint32_t off = 14u + 40u, size = off + bytes;
    BITMAPINFOHEADER ih;
    memset(&ih, 0, sizeof ih);
    ih.biSize = sizeof ih; ih.biWidth = g_mw; ih.biHeight = -g_mh;
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fwrite(&tag, 2, 1, f); fwrite(&size, 4, 1, f);
    fwrite(&zero, 2, 1, f); fwrite(&zero, 2, 1, f); fwrite(&off, 4, 1, f);
    fwrite(&ih, 40, 1, f);
    fwrite(g_px, bytes, 1, f);
    fclose(f);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Window
 * ═══════════════════════════════════════════════════════════════════ */
static int g_paints;
static int g_selftest;
static char g_selftest_dir[MAX_PATH];

static void tick(void)
{
    g_ticks++;
    g_caret_on = ((g_ticks / 9) & 1) == 0;
    int need = 0;

    if (g_page == PAGE_CHECKING) {
        if (!g_pf_valid && InterlockedCompareExchange(&g_pf_state, 2, 2) == 2) {
            g_pf_valid = 1;
            g_reveal   = 0;
            g_settle   = 0;
        }
        if (g_pf_valid) {
            /* The stagger is presentation only: every row below shows the
             * result preflight actually returned, revealed in order. */
            if (g_reveal < N_CHK) { if ((g_ticks & 1) == 0) g_reveal++; }
            else if (++g_settle == 10) check_advance();
        }
        need = 1;
    } else if (g_page == PAGE_PROGRESS) {
        install_tick();
        need = 1;
    } else if (g_page == PAGE_CONSENT) {
        need = 1;                      /* caret blink */
    }

    if (g_selftest) {
        if (g_ticks == 6)  PostMessageW(g_hwnd, WM_KEYDOWN, VK_RETURN, 0);  /* Get started */
        if (g_ticks == 30) PostMessageW(g_hwnd, WM_KEYDOWN, VK_TAB, 0);     /* keyboard nav */
    }

    if (g_selftest && g_ticks > 34) {
        char path[MAX_PATH + 32];
        _snprintf(path, sizeof path - 1, "%s/selftest-window.bmp", g_selftest_dir);
        path[sizeof path - 1] = 0;
        save_bmp(path);
        _snprintf(path, sizeof path - 1, "%s/selftest.log", g_selftest_dir);
        path[sizeof path - 1] = 0;
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "window created   : %s\n", g_hwnd ? "yes" : "no");
            fprintf(f, "client size      : %dx%d @ %d dpi\n", g_cw, g_ch, g_dpi);
            fprintf(f, "WM_PAINT handled : %d\n", g_paints);
            fprintf(f, "back buffer      : %s\n", g_px ? "ok" : "MISSING");
            fprintf(f, "page             : %d\n", (int)g_page);
            fprintf(f, "preflight done   : %d (blocks=%d warns=%d results=%d)\n",
                    g_pf_valid, g_report.n_block, g_report.n_warn, g_report.n);
            fprintf(f, "widgets on page  : %d\n", g_nw);
            fprintf(f, "focus after Tab  : %d (ring=%d, id=%d)\n", g_focus, g_focus_ring,
                    (g_focus >= 0 && g_focus < g_nw) ? g_w[g_focus].id : -1);
            fprintf(f, "furthest page    : %d (%s)\n", g_max_page,
                    g_max_page >= (int)PAGE_BACKUP ? "PAST THE GATE"
                                                   : "gate held");
            fprintf(f, "nav_allowed backup..progress: ");
            for (int pg = (int)PAGE_BACKUP; pg < (int)PAGE_COUNT; pg++)
                fprintf(f, "%d", nav_allowed((page_id)pg));
            fprintf(f, "\n");
            fclose(f);
        }
        PostQuitMessage(0);
        return;
    }
    if (need && g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void on_char(wchar_t c)
{
    if (g_focus < 0 || g_focus >= g_nw) return;
    if (g_w[g_focus].id != ID_INPUT_AGREE) return;
    size_t n = wcslen(g_agree);
    if (c == 8) {                                  /* backspace */
        if (n) g_agree[n - 1] = 0;
    } else if (c >= 32 && c != 127) {
        if (n + 1 < (sizeof g_agree / sizeof g_agree[0])) {
            g_agree[n] = c;
            g_agree[n + 1] = 0;
        }
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        g_dpi  = dpi_for(h);
        fonts_make();
        dark_titlebar(h);
        /* The self-test drives its own clock so a slow emulated desktop
         * cannot stretch a 7-second script into minutes. */
        if (!g_selftest) SetTimer(h, 1, 60, NULL);
        return 0;

    case WM_SIZE:
        g_cw = LOWORD(lp); g_ch = HIWORD(lp);
        backbuffer(g_cw, g_ch);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(920);
        mm->ptMinTrackSize.y = S(640);
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpi = (int)HIWORD(wp);
        fonts_make();
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        backbuffer(g_cw, g_ch);
        render();
        g_paints++;
        if (g_px) BitBlt(dc, 0, 0, g_cw, g_ch, g_mdc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_TIMER:
        tick();
        return 0;

    case WM_MOUSEMOVE: {
        g_mouse.x = GET_X_LPARAM(lp); g_mouse.y = GET_Y_LPARAM(lp);
        int hot = w_find_at(g_mouse.x, g_mouse.y);
        if (hot != g_hot_idx) { g_hot_idx = hot; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_mouse.x = g_mouse.y = -1; g_hot_idx = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
        g_mouse.x = GET_X_LPARAM(lp); g_mouse.y = GET_Y_LPARAM(lp);
        g_mouse_down = 1;
        g_press_idx  = w_find_at(g_mouse.x, g_mouse.y);
        if (g_press_idx >= 0) { g_focus = g_press_idx; g_focus_ring = 0; }
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        g_mouse.x = GET_X_LPARAM(lp); g_mouse.y = GET_Y_LPARAM(lp);
        g_mouse_down = 0;
        ReleaseCapture();
        int up = w_find_at(g_mouse.x, g_mouse.y);
        if (up >= 0 && up == g_press_idx) widget_activate(g_w[up].id);
        g_press_idx = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL:
        scroll_by(-GET_WHEEL_DELTA_WPARAM(wp) * S(60) / WHEEL_DELTA);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_hot_idx >= 0 ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;

    case WM_CHAR:
        on_char((wchar_t)wp);
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_TAB:
            focus_step((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            break;
        case VK_RETURN:
            if (g_focus >= 0 && g_focus < g_nw && g_w[g_focus].kind == W_BUTTON)
                widget_activate(g_w[g_focus].id);
            else
                do_primary();
            break;
        case VK_SPACE:
            if (g_focus >= 0 && g_focus < g_nw && g_w[g_focus].kind != W_INPUT &&
                g_w[g_focus].kind != W_BUTTON)
                widget_activate(g_w[g_focus].id);
            else if (g_focus >= 0 && g_focus < g_nw && g_w[g_focus].kind == W_BUTTON)
                widget_activate(g_w[g_focus].id);
            break;
        case VK_ESCAPE:
            if (g_page == PAGE_PROGRESS && g_install_running) MessageBeep(MB_ICONASTERISK);
            else if (g_page == PAGE_WELCOME || g_page == PAGE_BLOCKED ||
                     g_page == PAGE_PROGRESS) PostMessageW(h, WM_CLOSE, 0, 0);
            else goto_page(back_target());
            break;
        case VK_LEFT:  focus_step(-1); break;
        case VK_RIGHT: focus_step(1);  break;
        case VK_UP:    scroll_by(-S(60)); break;
        case VK_DOWN:  scroll_by(S(60));  break;
        case VK_PRIOR: scroll_by(-g_view_h + S(40)); break;
        case VK_NEXT:  scroll_by(g_view_h - S(40));  break;
        case VK_HOME:  g_scroll[g_page] = 0; break;
        case VK_END:   g_scroll[g_page] = scroll_max(); break;
        default: break;
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_CLOSE:
        if (g_install_running) {
            if (MessageBoxW(h, L"Stop setting up AurOS?\n\nNothing on this PC has "
                               L"been changed, so it is safe to stop here.",
                            L"AurBridge", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        PostQuitMessage(0);
        return 0;

    default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Dev harness — screenshots.
 *
 *  Rendering only: no window is shown, no input is accepted, no phase
 *  stub is reached that the interactive path would not reach, and
 *  nav_allowed() is untouched. It exists so the pages can be reviewed
 *  as images on a machine that is not running Windows.
 * ═══════════════════════════════════════════════════════════════════ */
static void shot_save(const char *dir, const char *name)
{
    if (!g_shot_mode) return;          /* harness only, never the UI path */
    char p[MAX_PATH + 64];
    _snprintf(p, sizeof p - 1, "%s/%s.bmp", dir, name);
    p[sizeof p - 1] = 0;
    render();
    render();                 /* second pass settles deferred focus */
    save_bmp(p);
}

static int shot_run(const char *dir)
{
    g_shot_mode = 1;
    g_dpi = 96;
    g_cw = 1120; g_ch = 760;
    backbuffer(g_cw, g_ch);
    if (!g_px) return 2;
    fonts_make();
    detect_defaults();

    g_page = PAGE_WELCOME;  shot_save(dir, "1-welcome");

    g_page = PAGE_CHECKING; g_pf_valid = 0; g_reveal = 0;
    shot_save(dir, "2a-checking-running");

    pf_run(&g_report);                     /* read-only */
    g_pf_valid = 1; g_reveal = N_CHK;
    shot_save(dir, "2b-checking-done");

    if (g_report.n_block > 0) { g_page = PAGE_BLOCKED; shot_save(dir, "3-blocked"); }

    g_page = PAGE_BACKUP;   shot_save(dir, "4a-backup-empty");
    g_ack_backup = g_ack_usb = 1;
    shot_save(dir, "4b-backup-done");

    g_focus = 0; g_focus_ring = 1;          /* as if the user had pressed Tab */
    shot_save(dir, "4c-backup-keyboard-focus");
    g_focus = -1; g_focus_ring = 0;

    g_page = PAGE_CONSENT;  g_want_focus_id = ID_INPUT_AGREE;
    shot_save(dir, "5a-consent");
    g_scroll[PAGE_CONSENT] = 10000;
    wcscpy(g_agree, AGREE_WORD);
    shot_save(dir, "5b-consent-typed");

    g_page = PAGE_CHOOSE;   shot_save(dir, "6a-choose");
    g_choice = 1; shot_save(dir, "6b-choose-replace");
    g_choice = 0; g_ack_replace = 0;

    /* The archetype chooser opens on Rail; the foot of the list is where
     * the one that requires learning lives, so it gets its own frame. */
    g_page = PAGE_DESKTOP;  shot_save(dir, "7a-desktop");
    g_scroll[PAGE_DESKTOP] = 10000;
    shot_save(dir, "7b-desktop-end");
    g_sel_shell = N_SHELLS - 1;
    g_focus = N_SHELLS - 1; g_focus_ring = 1;   /* as if the user had tabbed */
    shot_save(dir, "7c-desktop-workbench");
    g_focus = -1; g_focus_ring = 0;
    g_sel_shell = SHELL_DEFAULT;
    g_scroll[PAGE_DESKTOP] = 0;

    g_page = PAGE_PERSONALIZE; shot_save(dir, "8-personalize");

    g_ready_confirm = 1;
    g_page = PAGE_READY;    shot_save(dir, "9-ready");

    install_begin();
    for (int i = 0; i < 30; i++) install_tick();
    g_page = PAGE_PROGRESS; shot_save(dir, "10-progress");
    for (int i = 0; i < 60; i++) install_tick();
    shot_save(dir, "10b-progress-end");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Dev harness — the navigation gate, tested.
 *
 *  "The user can never reach the backup gate once preflight has blocked"
 *  is a claim, so it gets a test. This calls the same nav_allowed(),
 *  goto_page() and check_advance() the mouse and keyboard handlers call.
 *  The synthetic pf_reports below are test fixtures and exist only in
 *  this function.
 * ═══════════════════════════════════════════════════════════════════ */
static FILE *g_nt;
static int   g_nt_fail;

static void nt_check(int ok, const char *what)
{
    if (!ok) g_nt_fail++;
    if (g_nt) fprintf(g_nt, "  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

static void nt_fake_report(int blocked)     /* test fixture, not a real scan */
{
    memset(&g_report, 0, sizeof g_report);
    g_report.system_disk = -1;
    if (blocked) {
        snprintf(g_report.results[0].id, sizeof g_report.results[0].id, "test-block");
        snprintf(g_report.results[0].title, sizeof g_report.results[0].title,
                 "synthetic blocking issue");
        g_report.results[0].sev = PF_BLOCK;
        g_report.n = 1;
        g_report.n_block = 1;
    } else {
        snprintf(g_report.results[0].id, sizeof g_report.results[0].id, "ready");
        g_report.results[0].sev = PF_PASS;
        g_report.n = 1;
    }
    g_pf_valid = 1;
    g_reveal   = N_CHK;
}

static int nav_test(const char *dir)
{
    char path[MAX_PATH + 32];
    _snprintf(path, sizeof path - 1, "%s/navtest.log", dir);
    path[sizeof path - 1] = 0;
    g_nt = fopen(path, "w");

    g_dpi = 96; g_cw = 1120; g_ch = 760;
    backbuffer(g_cw, g_ch);
    fonts_make();
    detect_defaults();

    if (g_nt) fprintf(g_nt, "AurBridge wizard - navigation gate test\n\n"
                            "1. preflight has not run yet\n");
    g_pf_valid = 0;
    memset(&g_report, 0, sizeof g_report);
    for (int p = (int)PAGE_BACKUP; p < (int)PAGE_COUNT; p++) {
        g_page = PAGE_WELCOME;
        goto_page((page_id)p);
        nt_check(!nav_allowed((page_id)p) && g_page == PAGE_WELCOME,
                 "page refused before preflight has run");
    }

    if (g_nt) fprintf(g_nt, "\n2. preflight blocked, user has answered everything\n");
    nt_fake_report(1);
    g_ack_backup = g_ack_usb = 1;
    wcscpy(g_agree, AGREE_WORD);
    g_choice = 0;
    g_ready_confirm = 1;
    for (int p = (int)PAGE_BACKUP; p < (int)PAGE_COUNT; p++) {
        g_page = PAGE_BLOCKED;
        goto_page((page_id)p);
        nt_check(!nav_allowed((page_id)p) && g_page == PAGE_BLOCKED,
                 "page refused while a block stands");
    }
    g_page = PAGE_CHECKING;
    check_advance();
    nt_check(g_page == PAGE_BLOCKED, "a blocked check lands on the refusal page");
    g_page = PAGE_CHECKING;
    g_check_next = PAGE_PROGRESS;          /* as if launched from Ready */
    check_advance();
    nt_check(g_page == PAGE_BLOCKED && !g_install_running,
             "a re-check before install refuses instead of starting phases");
    g_check_next = PAGE_BACKUP;

    if (g_nt) fprintf(g_nt, "\n3. preflight clean, gates open one at a time\n");
    nt_fake_report(0);
    g_ack_backup = g_ack_usb = 0;
    g_agree[0] = 0;
    g_choice = 0; g_ack_replace = 0; g_ready_confirm = 0;
    nt_check(nav_allowed(PAGE_BACKUP), "backup gate reachable on a clean report");
    nt_check(!nav_allowed(PAGE_CONSENT), "consent refused until both boxes are ticked");
    g_ack_backup = 1;
    nt_check(!nav_allowed(PAGE_CONSENT), "one box is not enough");
    g_ack_usb = 1;
    nt_check(nav_allowed(PAGE_CONSENT), "consent reachable with both boxes");
    nt_check(!nav_allowed(PAGE_CHOOSE), "choice refused until AGREE is typed");
    wcscpy(g_agree, L"agre");
    nt_check(!nav_allowed(PAGE_CHOOSE), "a near miss is still refused");
    wcscpy(g_agree, L"agree");
    nt_check(nav_allowed(PAGE_CHOOSE), "typed acknowledgement accepted (any case)");
    g_choice = 1;
    nt_check(!nav_allowed(PAGE_PERSONALIZE),
             "replace-Windows refused until its own box is ticked");
    g_ack_replace = 1;
    nt_check(nav_allowed(PAGE_PERSONALIZE), "replace-Windows accepted once acknowledged");
    g_choice = 0; g_ack_replace = 0;
    nt_check(!nav_allowed(PAGE_PROGRESS), "install refused until the drive is confirmed");
    g_ready_confirm = 1;
    nt_check(nav_allowed(PAGE_PROGRESS), "install reachable at the end of a clean run");

    if (g_nt) fprintf(g_nt, "\n4. a block appears after the user answered everything\n");
    nt_fake_report(1);
    for (int p = (int)PAGE_BACKUP; p < (int)PAGE_COUNT; p++)
        nt_check(!nav_allowed((page_id)p),
                 "every page past the checklist closes again");

    if (g_nt) {
        fprintf(g_nt, "\n%s (%d failures)\n", g_nt_fail ? "FAILED" : "ALL PASS", g_nt_fail);
        fclose(g_nt);
    }
    return g_nt_fail ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════ */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;

    /* Dev harness switches. They only render and report; none of them can
     * change anything on the machine, because nothing in this file can.
     * Gate them behind a build flag before the first signed release
     * anyway — a shipped installer should have no undocumented modes. */
    const char *shot = NULL, *navtest = NULL;
    for (int i = 1; i < __argc; i++) {
        if (!strcmp(__argv[i], "--shot") && i + 1 < __argc) shot = __argv[++i];
        else if (!strcmp(__argv[i], "--navtest") && i + 1 < __argc) navtest = __argv[++i];
        else if (!strcmp(__argv[i], "--selftest") && i + 1 < __argc) {
            g_selftest = 1;
            _snprintf(g_selftest_dir, sizeof g_selftest_dir - 1, "%s", __argv[++i]);
            g_selftest_dir[sizeof g_selftest_dir - 1] = 0;
        }
    }

    dpi_opt_in();
    if (shot) return shot_run(shot);
    if (navtest) return nav_test(navtest);

    detect_defaults();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"AurBridgeWizard";
    wc.hIcon         = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    g_dpi = dpi_for(NULL);
    RECT r = { 0, 0, S(1120), S(760) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Install AurOS",
                             WS_OVERLAPPEDWINDOW, sx, sy, ww, wh,
                             NULL, NULL, inst, NULL);
    if (!h) return 1;

    ShowWindow(h, g_selftest ? SW_SHOWNOACTIVATE : show);
    UpdateWindow(h);

    MSG msg;
    if (g_selftest) {
        int quit = 0;
        while (!quit) {
            for (int drained = 0; drained < 8; drained++) {
                if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) break;
                if (msg.message == WM_QUIT) { quit = 1; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit) break;
            tick();
            Sleep(10);
        }
        fonts_free();
        return 0;
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    fonts_free();
    return 0;
}
