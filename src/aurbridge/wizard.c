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
 *  NOTHING IN THIS FILE OPENS A HANDLE, and none should ever be added.
 *  The work is in phases.c, behind plat.h, on a worker thread. That is
 *  not tidiness: this file's code runs on the UI thread, so anything
 *  that blocks stops the window painting, and a wizard that appears to
 *  have crashed in the middle of preparing somebody's disk is the
 *  worst thing this program can do short of losing her files.
 *
 *  Before the restart, everything AurBridge has done is undoable, and
 *  closing the window undoes it — see install_cancel().
 *
 *  Three screens before the progress screen, and nothing to type on any
 *  of them (docs/handoff/SESSION-2026-10-23.md, D3): the first page with
 *  the check running on it, the look, Ready. Then one bar, and Restart.
 *
 *  Safety invariants enforced in code, not just in pixels:
 *    - nav_allowed() refuses every page past the first unless preflight
 *      has completed with zero PF_BLOCK results. The BLOCKED page has no
 *      continue affordance, and even if one were added by accident,
 *      nav_allowed() would refuse it.
 *    - "Install AurOS" re-runs preflight before the phase list is
 *      touched (AURBRIDGE.md: destructive subcommands re-run preflight
 *      and abort on any block).
 *
 *  Build: build/aurbridge.
 * ═══════════════════════════════════════════════════════════════════ */

/* Vista+ locale APIs (GetLocaleInfoEx); everything newer than that is
 * reached through GetProcAddress so the binary still loads on Windows 7. */
#ifndef WINVER
#define WINVER       0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

/* C, not C++, view of the COM interfaces: the screen-reader object at
 * the end of this file is an IAccessible written out by hand. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <oleacc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

#include "preflight.h"
#include "sbdb.h"
#include "phases.h"
#include "fix.h"

/* Baked in by build/aurbridge. A build that has not been told where
 * the image lives produces an installer that expects it to be sitting
 * beside itself, which is what a developer build is. */
#ifndef AUROS_IMAGE_URL
#define AUROS_IMAGE_URL     ""
#endif
#ifndef AUROS_IMAGE_SHA256
#define AUROS_IMAGE_SHA256  ""
#endif
#ifndef AUROS_IMAGE_BYTES
#define AUROS_IMAGE_BYTES   0ULL
#endif
/* WHERE THE IMAGE IS PUBLISHED IN PIECES, baked in by build/aurbridge
 * from AUROS_IMAGE_PIECES (see phases.h for the format). Empty in a
 * build that was not given one. */
#if defined(__has_include)
# if __has_include("aurbridge-baked.h")
#  include "aurbridge-baked.h"
# endif
#endif
#ifndef AUROS_PIECES_TEXT
#define AUROS_PIECES_TEXT   ""
#endif

/* ── Palette ──────────────────────────────────────────────────────────
 * The installer's own dark ground, with the teal it has always had for
 * the one button that matters (tools/e2e/vision.py finds that button by
 * this exact colour; change both or neither).
 *
 * EVERY COLOUR TEXT IS DRAWN IN IS AT LEAST 4.5:1 against every surface
 * it sits on (docs/EASY.md). C_MUTED used to be #55606E, 2.7:1 on the
 * card, and it carried the footer hints, the "test version" line and
 * the steps still to come -- the lines a person with presbyopia could
 * not read at all. It is now 5:1 on the lightest surface. */
#define C_BG          0x0B0E14u   /* bg          */
#define C_BG_ALT      0x10151Fu   /* bg_alt      */
#define C_SURFACE     0x161C28u   /* surface     */
#define C_SURFACE_HI  0x1F2735u   /* surface_hi  */
#define C_OVERLAY     0x3A4556u   /* hairlines and outlines, not text   */
#define C_MUTED       0x8E9AABu   /* quiet text: >= 5:1 on surface_hi   */
#define C_SUBTLE      0xAEB8C6u   /* secondary text: >= 7:1             */
#define C_FG          0xD9E0EBu   /* primary text                       */
#define C_FG_HI       0xF3F7FDu   /* headings                           */
#define C_ACCENT      0x7DD3C0u   /* THE primary button. Do not change. */
#define C_ACCENT_ALT  0xA78BFAu   /* accent_alt  */
#define C_WARM        0xF2B880u   /* accent_warm */
#define C_OK          0x7DD3C0u   /* ok          */
#define C_WARN        0xF2B880u   /* warn        */
#define C_ERR         0xF2788Du   /* err         */
#define C_INFO        0x82AAFFu   /* info        */

/* Channel extraction, so the backdrop can be DERIVED from the palette
 * above instead of restating it. */
#define CH_R(c) ((float)(((c) >> 16) & 0xFFu))
#define CH_G(c) ((float)(((c) >>  8) & 0xFFu))
#define CH_B(c) ((float)( (c)        & 0xFFu))

#define CR(c) RGB(((c)>>16)&0xFF, ((c)>>8)&0xFF, (c)&0xFF)

/* ── How it will look: the one question she is asked ──────────────────
 *
 * The owner's words (docs/handoff/SESSION-2026-10-23.md): "AurOS where
 * she picks the style first: like Windows / like a Mac / big simple
 * buttons". So the cards are named after what she already knows. The
 * rule in docs/SHELLS.md that no other system is named holds for
 * AurOS's own screens; here, on Windows, before she has seen AurOS,
 * "like Windows" is the one description she can use, and EASY.md (every
 * word is one she uses) outranks it.
 *
 * Each card is a desktop (shells/<shell>.shell) and a look
 * (themes/<theme>.theme). Both travel in ab_choice to
 * \EFI\AurOS\choices.conf as shell= and theme=, and
 * rootfs/usr/lib/auros/choices.sh applies them at first boot. The
 * picture on each card is drawn in that theme's own colours, so what
 * she picks is what she gets.
 *
 * Sandstone, the light theme, for all four: dark text on a light
 * ground is what Windows and the Mac show her today, and it is the
 * easier one to read for most older eyes. Big and simple is Rail,
 * because docs/SHELLS.md says it is the one for "someone who has never
 * been comfortable with a computer": nothing in it can be hidden.
 *
 * PHASE 2 ("other systems") is another kind of card that leads to a
 * page of its own instead of setting these two values: a fifth card, or
 * a line under the row, and nothing else needs to move. */
enum { PIC_WINDOWS = 0, PIC_MAC, PIC_SIMPLE, PIC_ONE };

typedef struct {
    const char    *shell;     /* shell_archetype: shells/<id>.shell     */
    const char    *theme;     /* theme: themes/<id>.theme               */
    const wchar_t *name;      /* the card's name, in her words          */
    const wchar_t *line;      /* one short line under it                */
    const wchar_t *ready;     /* said again on the Ready page           */
    int            picture;   /* PIC_*                                  */
} style_info;

/* THE NAMES ARE THE WEBSITE'S, word for word: she reads them there
 * first, and the installer must say the same thing. "One thing at a
 * time" is Tiles because that is Tiles' own name (shells/tiles.shell),
 * which AurOS's Settings shows her later; Big and simple is Rail. */
static const style_info STYLES[] = {
  { "taskbar", "sandstone", L"Like Windows",
    L"A bar along the bottom, just like now.",
    L"It will look like Windows.", PIC_WINDOWS },
  { "dock",    "sandstone", L"Like a Mac",
    L"Your programs in a row along the bottom.",
    L"It will look like a Mac.", PIC_MAC },
  { "rail",    "sandstone", L"Big and simple",
    L"Big buttons. Nothing ever gets hidden.",
    L"It will be big and simple.", PIC_SIMPLE },
  { "tiles",   "sandstone", L"One thing at a time",
    L"Each program fills the whole screen.",
    L"It will show one thing at a time.", PIC_ONE },
};
#define N_STYLES ((int)(sizeof STYLES / sizeof STYLES[0]))
/* What she knows: Windows. */
#define STYLE_DEFAULT 0

/* themes/sandstone.theme, for drawing the pictures. */
#define SS_BG       0xF4F0E8u
#define SS_WALL     0xE3D8C6u
#define SS_SURFACE  0xFFFFFFu
#define SS_SURF_HI  0xF0E9DCu
#define SS_LINE     0x877D6Du
#define SS_MUTED    0xC9BFAFu
#define SS_FG       0x38332Bu
#define SS_ACCENT   0xC2683Du
#define SS_ALT      0x4E7A6Bu
#define SS_WARMC    0xD9A441u
#define SS_INFO     0x3F6E9Eu
#define SS_ERR      0xB4453Cu
#define SS_PLUM     0x8A5C8Fu

/* ── Pages ────────────────────────────────────────────────────────── */
/* Three screens before the progress screen, and nothing to type on any
 * of them (docs/handoff/SESSION-2026-10-23.md, D3). BLOCKED is not a
 * step: it is what the first screen becomes when something stops it. */
typedef enum {
    PAGE_WELCOME = 0,     /* what AurOS is, and the check, together */
    PAGE_BLOCKED,         /* something stops it, and what we do      */
    PAGE_STYLE,           /* how would you like it to look?          */
    PAGE_READY,           /* what happens, and how long              */
    PAGE_PROGRESS,        /* one bar, then Restart now               */
    PAGE_COUNT
} page_id;

static const struct { const wchar_t *label; page_id first; } RAIL[] = {
    { L"Check this computer", PAGE_WELCOME  },
    { L"Pick a look",         PAGE_STYLE    },
    { L"Ready",               PAGE_READY    },
    { L"Install",             PAGE_PROGRESS },
};
#define N_RAIL ((int)(sizeof RAIL / sizeof RAIL[0]))

static int rail_index_for(page_id p)
{
    if (p == PAGE_BLOCKED) return 0;            /* stopped at the check */
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
             g_f_smallb, g_f_tiny, g_f_btn;

static RECT  g_clip;                   /* clip for hand-rasterised pixels */

/* input */
static POINT g_mouse = { -1, -1 };
static int   g_mouse_down = 0;
static int   g_press_idx = -1;
static int   g_focus = -1;             /* index into g_w[]                */
static int   g_focus_ring = 0;         /* draw rings only after Tab/arrow */

/* scrolling */
static int   g_scroll[PAGE_COUNT];
static int   g_content_h, g_view_h;    /* measured during paint           */

/* preflight */
static pf_report    g_report;          /* the last finished check (UI thread only) */
static pf_report    g_pf_new;          /* the one being run (worker only)          */
static volatile LONG g_pf_state;       /* 0 idle · 1 running · 2 has a result      */
static int          g_pf_valid;        /* g_report holds a real result     */
static int          g_checking;        /* a check is running right now    */
static int          g_reveal;          /* check items revealed            */
static int          g_ticks;           /* animation clock (60 ms)          */
static page_id      g_check_next = PAGE_STYLE;   /* where a clean check goes */

/* her one answer */
static int   g_sel_style = STYLE_DEFAULT;
/* D10: her Wi-Fi comes across unless she unticks it. */
static int   g_bring_wifi = 1;

/* Language, keyboard and time zone: what Windows already uses, read at
 * start-up (detect_defaults) and handed over without a question. The
 * lists keep a few more so --navtest can prove the values map. */
#define OPT_MAX 10
static int   g_sel_lang = 0, g_sel_kbd = 0, g_sel_tz = 0;
static wchar_t g_langs[OPT_MAX][96]; static char g_lang_v[OPT_MAX][64]; static int g_n_langs;
static wchar_t g_kbds [OPT_MAX][96]; static char g_kbd_v [OPT_MAX][64]; static int g_n_kbds;
static wchar_t g_tzs  [OPT_MAX][96]; static char g_tz_v  [OPT_MAX][96]; static int g_n_tzs;

/* detected-from-Windows defaults, filled in at startup */
static wchar_t g_det_lang[96], g_det_kbd[96], g_det_tz[128];

/* dev harness only; never set by anything the user can click */
static int   g_shot_mode = 0;
static int   g_nav_mode  = 0;
static int   g_selftest  = 0;

/* SET ONLY BY "Restart now" (or its countdown), and by Windows itself
 * ending the session. Closing the window before the restart takes
 * everything back (install_cancel); restarting is the one way of leaving
 * it that keeps the one-shot start-up setting. */
static int   g_restarting = 0;

/* The restart at the end: by itself after a minute she can see counting
 * down, or now, or later ("Not yet" stops the count). */
#define COUNTDOWN_SECONDS 60
static int       g_cd_on;              /* counting                        */
static int       g_cd_offered;         /* the count has been started once */
static ULONGLONG g_cd_start;
static int       g_cd_left = COUNTDOWN_SECONDS;

/* ═══════════════════════════════════════════════════════════════════
 *  Small helpers
 * ═══════════════════════════════════════════════════════════════════ */
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void a2w(const char *a, wchar_t *w, int cch)
{
    if (!MultiByteToWideChar(CP_ACP, 0, a, -1, w, cch)) { w[0] = 0; }
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
 *
 *  THREE KINDS, AND NO MORE. There used to be a text box (for typing
 *  AGREE), check boxes that had to be ticked to go on (a backup she
 *  probably did not have) and chips. A button and a picture card are
 *  what this installer asks her to use; a toggle is there only to say
 *  no to something already said yes to for her (D10, her Wi-Fi), and it
 *  never holds the way on. --navtest fails on anything else.
 * ═══════════════════════════════════════════════════════════════════ */
typedef enum { W_BUTTON, W_CARD, W_TOGGLE } wkind;

typedef struct {
    int id; wkind kind; RECT r; int enabled;
    const wchar_t *name;      /* what a screen reader says it is (static text) */
} widget;

static widget g_w[96];
static int    g_nw;

enum {
    ID_NONE = 0,
    ID_PRIMARY, ID_BACK, ID_QUIT, ID_NOT_YET, ID_WIFI,
    ID_STYLE = 500
};

static int w_add(int id, wkind k, int x, int y, int w, int h, int enabled)
{
    if (g_nw >= (int)(sizeof g_w / sizeof g_w[0])) return -1;
    widget *p = &g_w[g_nw];
    p->id = id; p->kind = k; p->enabled = enabled; p->name = NULL;
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
              (float)radius, (float)S(3), C_FG_HI, 0.95f);
}

/* returns the widget index */
static int draw_button(int id, const wchar_t *label, int x, int y, int w, int h,
                       int primary, int enabled, uint32_t tone)
{
    int idx = w_add(id, W_BUTTON, x, y, w, h, enabled);
    if (idx >= 0) g_w[idx].name = label;
    int hot = w_hot(idx);
    int down = hot && g_mouse_down && g_press_idx == idx;
    if (down) { y += S(1); h -= S(1); }
    float r = (float)S(12);
    RECT box = { x, y, x + w, y + h };

    if (!enabled) {
        fill_rr((float)x, (float)y, (float)w, (float)h, r, C_SURFACE_HI, 0.9f);
        stroke_rr((float)x, (float)y, (float)w, (float)h, r, 1.2f, C_OVERLAY, 1.f);
        text_in(label, g_f_btn, C_MUTED, box,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    } else if (primary) {
        /* The primary action is the only solid block of the accent on
         * the page, which is what makes the eye (and the end-to-end
         * test) land on it. Solid, not translucent: 0.92 over the card
         * made it a slightly different teal on every surface. */
        fill_rr((float)x, (float)y, (float)w, (float)h, r, tone,
                down ? 0.82f : 1.0f);
        if (hot && !down)
            stroke_rr((float)x, (float)y, (float)w, (float)h, r, (float)S(2),
                      C_FG_HI, 0.9f);
        text_in(label, g_f_btn, C_BG, box, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    } else {
        fill_rr((float)x, (float)y, (float)w, (float)h, r, C_SURFACE_HI,
                down ? 1.0f : (hot ? 1.0f : 0.85f));
        stroke_rr((float)x, (float)y, (float)w, (float)h, r, hot ? 2.f : 1.4f,
                  hot ? C_SUBTLE : C_MUTED, 1.f);
        text_in(label, g_f_btn, C_FG_HI, box,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    if (w_focused(idx)) focus_ring(box, S(4), S(16));
    return idx;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Preflight plumbing
 *
 *  The worker fills its own report and the UI thread copies it over
 *  when it is finished, so the page on screen never reads a report
 *  that is half written -- which it used to, for the second check
 *  behind "Start installing".
 * ═══════════════════════════════════════════════════════════════════ */
static DWORD WINAPI pf_worker(LPVOID p)
{
    (void)p;
    pf_run(&g_pf_new);                 /* read-only, by contract */
    InterlockedExchange(&g_pf_state, 2);
    return 0;
}

static void pf_start(void)
{
    if (InterlockedCompareExchange(&g_pf_state, 1, 0) != 0) return;
    g_checking = 1;
    memset(&g_pf_new, 0, sizeof g_pf_new);
    HANDLE h = CreateThread(NULL, 0, pf_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else { pf_run(&g_pf_new); InterlockedExchange(&g_pf_state, 2); }
}

/* 1 when a check has just finished and g_report now holds it. */
static int pf_poll(void)
{
    if (InterlockedCompareExchange(&g_pf_state, 0, 2) != 2) return 0;
    memcpy(&g_report, &g_pf_new, sizeof g_report);
    g_pf_valid = 1;
    g_checking = 0;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Fixing what the check found, instead of describing it
 * ═══════════════════════════════════════════════════════════════════
 *
 * The page a stopped check lands on used to be a report: every problem in
 * a paragraph, a "WHAT TO DO" box, and a Check again button. It is a list
 * now -- the problem and what happens about it, one line each (fix.h) --
 * with one button that fixes everything a program can fix, and a check
 * that runs again by itself every few seconds, so that plugging in the
 * charger or taking out a USB stick is all somebody has to do. */
enum { FS_NONE = 0, FS_RUNNING, FS_DONE, FS_FAILED };
#define FIX_MAX 24
static struct { char id[48]; int state; int after; wchar_t why[200]; } g_fx[FIX_MAX];
static int           g_fx_n;
static volatile LONG g_fix_busy;       /* the fix worker is running        */
static pf_report     g_bg_report;      /* the quiet re-check's own copy    */
static volatile LONG g_bg_state;       /* 0 idle, 1 running, 2 has a result */
static int           g_bg_ticks;
static int           g_resumed;        /* opened again after our restart  */

static int fx_index(const char *id)
{
    for (int i = 0; i < g_fx_n; i++) if (!strcmp(g_fx[i].id, id)) return i;
    if (g_fx_n >= FIX_MAX) return -1;
    snprintf(g_fx[g_fx_n].id, sizeof g_fx[0].id, "%s", id);
    g_fx[g_fx_n].state = FS_NONE;
    g_fx[g_fx_n].after = 0;
    g_fx[g_fx_n].why[0] = 0;
    return g_fx_n++;
}

static fix_kind kind_of(const pf_result *r)
{
    const fix_info *f = fix_lookup(r->id);
    if (!f) return FIX_UNKNOWN;
    /* A restart is offered once. Still there after it: nothing we can do. */
    if (f->kind == FIX_RESTART && g_resumed) return FIX_CANNOT;
    return f->kind;
}

/* What the one button on the stop page would do: 1 if anything shown is
 * something a program can fix, 2 if one of those needs a restart. */
static int fixable_now(void)
{
    int any = 0;
    for (int i = 0; i < g_report.n; i++) {
        if (g_report.results[i].sev != PF_BLOCK) continue;
        fix_kind k = kind_of(&g_report.results[i]);
        /* Something nothing can fix is there too: fixing the rest (and
         * restarting somebody's PC for it) would change nothing. */
        if (k == FIX_CANNOT) return 0;
        if (k == FIX_RESTART) any = 2;
        if (k == FIX_AUTO && !any) any = 1;
    }
    return any;
}

static DWORD WINAPI fix_worker(LPVOID p)
{
    (void)p;
    /* The ids were copied out on the UI thread before this started; the
     * report itself is only ever touched there. */
    int restart = 0;
    for (int i = 0; i < g_fx_n; i++) {
        const fix_info *f = fix_lookup(g_fx[i].id);
        if (!f) continue;
        if (f->kind == FIX_RESTART) { restart = !g_resumed; continue; }
        if (f->kind != FIX_AUTO) continue;
        g_fx[i].state = FS_RUNNING;
        char why[200] = "";
        if (fix_run(g_fx[i].id, why, sizeof why) == 0) g_fx[i].state = FS_DONE;
        else {
            MultiByteToWideChar(CP_UTF8, 0, why, -1, g_fx[i].why, 199);
            g_fx[i].state = FS_FAILED;
        }
    }
    /* LAST, AND ONCE: every other fix is done first so the restart is the
     * only one, and the installer opens again after it (fix.h). */
    if (restart) {
        char why[200] = "";
        for (int i = 0; i < g_fx_n; i++) {
            const fix_info *f = fix_lookup(g_fx[i].id);
            if (f && f->kind == FIX_RESTART) g_fx[i].state = FS_RUNNING;
        }
        g_restarting = 1;
        if (fix_restart_and_continue(why, sizeof why) != 0) {
            g_restarting = 0;
            for (int i = 0; i < g_fx_n; i++) {
                const fix_info *f = fix_lookup(g_fx[i].id);
                if (f && f->kind == FIX_RESTART) {
                    MultiByteToWideChar(CP_UTF8, 0, why, -1, g_fx[i].why, 199);
                    g_fx[i].state = FS_FAILED;
                }
            }
        }
    }
    InterlockedExchange(&g_fix_busy, 0);
    g_bg_ticks = 1000;                 /* check again straight away */
    return 0;
}

static int any_cannot(void)
{
    for (int i = 0; i < g_report.n; i++)
        if (g_report.results[i].sev == PF_BLOCK && kind_of(&g_report.results[i]) == FIX_CANNOT)
            return 1;
    return 0;
}

static void fix_start(void)
{
    if (!fixable_now()) return;
    if (g_selftest || g_nav_mode || g_shot_mode) return;   /* harnesses never fix */
    if (InterlockedCompareExchange(&g_fix_busy, 1, 0) != 0) return;
    for (int i = 0; i < g_report.n; i++) {
        if (g_report.results[i].sev != PF_BLOCK) continue;
        fix_kind k = kind_of(&g_report.results[i]);
        if (k != FIX_AUTO && k != FIX_RESTART) continue;
        int j = fx_index(g_report.results[i].id);
        if (j >= 0) { g_fx[j].state = FS_NONE; g_fx[j].after = 0; g_fx[j].why[0] = 0; }
    }
    HANDLE h = CreateThread(NULL, 0, fix_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_fix_busy, 0);
}

static DWORD WINAPI bg_worker(LPVOID p)
{
    (void)p;
    pf_run(&g_bg_report);              /* read-only, by contract */
    InterlockedExchange(&g_bg_state, 2);
    return 0;
}

/* The quiet re-check, from tick(): every few seconds while the stop page
 * is up and nothing is being fixed. When it comes back clean the page
 * moves on by itself. */
static int bg_recheck_tick(void)
{
    if (InterlockedCompareExchange(&g_bg_state, 0, 2) == 2) {
        memcpy(&g_report, &g_bg_report, sizeof g_report);
        g_pf_valid = 1;
        g_bg_ticks = 0;
        /* a fix that has run, and been checked since */
        for (int i = 0; i < g_fx_n; i++) if (g_fx[i].state == FS_DONE) g_fx[i].after = 1;
        return 1;
    }
    if (g_fix_busy || g_bg_state != 0) return 0;
    if (++g_bg_ticks < 50) return 0;   /* about three seconds */
    g_bg_ticks = 0;
    if (InterlockedCompareExchange(&g_bg_state, 1, 0) != 0) return 0;
    memset(&g_bg_report, 0, sizeof g_bg_report);
    HANDLE h = CreateThread(NULL, 0, bg_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_bg_state, 0);
    return 0;
}

/* What the first page shows while the check runs: five things she can
 * recognise, each ticked as its answer comes in. Every id preflight
 * can emit belongs to one of them; anything unrecognised lands in the
 * last, so a new check can never be silently invisible here. */
static const struct { const wchar_t *label; const char *ids[24]; } CHK[] = {
  { L"Power",
    { "not-on-ac", "battery-low", "power-state-unknown", "no-battery", NULL } },
  { L"Space",
    { "insufficient-space", "space-unmeasurable", "space-measured",
      "shrink-online-limited", "esp-low-space", "esp-space", NULL } },
  { L"Drive",
    { "dynamic-disk", "dynamic-disk-other", "storage-spaces", "storage-spaces-other",
      "spanned-system-volume", "system-disk-unknown", "smart-bad-sectors",
      "smart-predict-failure", "smart-reallocated", "mbr-four-primaries",
      "layout-unreadable", "sector-size-unknown", "sector-size",
      "system-disk-removable", "system-disk-unreadable", "removable-attached",
      "removable-no-serial", "disk-unidentified", "no-disks", "intel-rst",
      "bitlocker-system", "bitlocker-other", "bitlocker-unknown", NULL } },
  { L"Start-up",
    { "firmware-uefi", "firmware-bios", "secure-boot-on", "secure-boot-off",
      SBDB_BLOCK_ID, SBDB_NEWER_ID, "secure-boot-unsigned-build",
      "esp-missing", "esp-unreadable", "arm-pc", NULL } },
  { L"Windows",
    { NULL } },                                /* and everything else */
};
#define N_CHK ((int)(sizeof CHK / sizeof CHK[0]))

static int chk_group_of(const char *id)
{
    for (int k = 0; k < N_CHK; k++)
        for (int j = 0; CHK[k].ids[j]; j++)
            if (!strcmp(CHK[k].ids[j], id)) return k;
    return N_CHK - 1;
}

/* The worst severity in a group, PF_PASS when it had nothing to say. */
static int chk_group_sev(int g)
{
    int sev = PF_PASS;
    for (int i = 0; i < g_report.n; i++)
        if (chk_group_of(g_report.results[i].id) == g &&
            (int)g_report.results[i].sev > sev)
            sev = (int)g_report.results[i].sev;
    return sev;
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
 *  machine unreachable past the first page.
 * ═══════════════════════════════════════════════════════════════════ */
static int style_ok(void)
{
    return g_sel_style >= 0 && g_sel_style < N_STYLES;
}

static int nav_allowed(page_id p)
{
    switch (p) {
    case PAGE_WELCOME:
        return 1;
    case PAGE_BLOCKED:
        return g_pf_valid && g_report.n_block > 0;
    default:
        break;
    }
    /* Hard gate. Nothing past the first page exists for a machine we
     * have refused, and nothing exists at all until preflight has run.
     * R3/#3 in the red-team register: refusing is the product outcome. */
    if (!g_pf_valid || !pf_is_go(&g_report)) return 0;

    switch (p) {
    case PAGE_STYLE:    return 1;
    case PAGE_READY:    return style_ok();
    /* Reachable only through check_advance(), after a check run when
     * she pressed "Install AurOS" has come back clean. */
    case PAGE_PROGRESS: return nav_allowed(PAGE_READY) && !g_checking;
    default:            return 0;
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
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════
 *  The phases, as the engine runs them.
 *
 *  The progress page no longer lists these -- it shows one bar and one
 *  sentence (page_progress) -- but install_begin() and install_tick()
 *  keep their state per phase, so the table stays, and its words stay
 *  true: three of them used to promise things nothing did ("about 5 GB",
 *  "sleep is tested", "only now does anything become permanent").
 * ═══════════════════════════════════════════════════════════════════ */
typedef enum { PH_PENDING = 0, PH_RUNNING, PH_DONE, PH_LATER, PH_FAILED } ph_state;

static const struct {
    const wchar_t *name;
    const wchar_t *desc;
    int after_restart;
} PHASES[] = {
 { L"Look at this computer once more",
   L"Every safety check runs again, right before we start.", 0 },
 { L"Write down what was chosen",
   L"Her answers, for after the restart.", 0 },
 { L"Get AurOS",
   L"AurOS is downloaded and checked, piece by piece.", 0 },
 { L"Add AurOS to the start-up menu",
   L"AurOS is offered once, at the next start.", 0 },
 { L"Make room",
   L"Room is made beside Windows. Her files stay where they are.", 1 },
 { L"Copy AurOS onto the drive",
   L"AurOS is written into the new space, then checked.", 1 },
 { L"Try it out on this computer",
   L"AurOS checks it can see the drive before anything is final.", 1 },
 { L"Your first look",
   L"AurOS asks whether everything works.", 1 },
 { L"Bring your files across",
   L"Documents and photos can be copied over.", 1 },
};
#define N_PHASES ((int)(sizeof PHASES / sizeof PHASES[0]))

static int   g_ph[N_PHASES];
static int   g_ph_cur = -1;
static float g_ph_prog;
static int   g_install_running, g_install_finished;
/* Everything before the restart is done and the one-shot is armed:
 * closing the window now would take it all back (install_cancel), so
 * WM_CLOSE asks first. */
static int   g_install_armed;

#define LOG_MAX 80
static wchar_t g_log[LOG_MAX][200];
static int     g_log_n;


/* ═══════════════════════════════════════════════════════════════════
 *  Driving the real thing
 *
 *  What used to be here was six functions that logged what a real
 *  phase WOULD do and returned success, with install_tick() advancing
 *  a fake progress bar over them on a timer. The engine is in phases.c
 *  now, so this is what is left: a worker thread, two callbacks, and
 *  a rule.
 *
 *  THE RULE: NOTHING IN THIS FILE OPENS A HANDLE. Not a disk, not a
 *  file, not a firmware variable. Everything that touches the world is
 *  behind plat.h, and the reason is not tidiness -- it is that the
 *  window procedure runs on the UI thread and anything that blocks it
 *  stops the window painting, so the temptation to "just read this one
 *  thing here" produces a program that appears to have crashed in the
 *  middle of preparing somebody's disk.
 *
 *  So: ab_run() on its own thread, and it reports back through say()
 *  and progress(), which do nothing but copy into the log and set two
 *  numbers under a lock. The UI reads them on its timer as before.
 * ═══════════════════════════════════════════════════════════════════ */

static CRITICAL_SECTION g_ab_lock;
static int    g_ab_lock_ready;
static HANDLE g_ab_thread;
static ab_machine g_ab_machine;
static ab_choice  g_ab_choice;
static char   g_ab_why[400];
static volatile LONG g_ab_state;   /* 0 idle, 1 running, 2 done, 3 failed */
static int    g_ab_phase;          /* which ab_phase is running           */
static int    g_ab_pct;

static void ab_lock_init(void)
{ if (!g_ab_lock_ready) { InitializeCriticalSection(&g_ab_lock); g_ab_lock_ready = 1; } }

/* The engine talks in UTF-8 because it also builds for a host that is
 * not Windows. The log is wide. This is the only place they meet. */
static void ab_say_cb(const char *line, void *ud)
{
    (void)ud;
    wchar_t w[400];
    MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 399);
    w[399] = 0;
    EnterCriticalSection(&g_ab_lock);
    if (g_log_n < LOG_MAX) wcscpy(g_log[g_log_n++], w);
    else {
        memmove(g_log[0], g_log[1], sizeof g_log - sizeof g_log[0]);
        wcscpy(g_log[LOG_MAX - 1], w);
    }
    /* A line beginning with the box-drawing rule is the engine saying
     * it has moved on to the next phase. Matching on that rather than
     * counting lines: the number of lines a phase prints is not a
     * contract and changing it should not silently desynchronise the
     * tick marks from the work. */
    if (line[0] == (char)0xE2)              /* the UTF-8 lead byte of ── */
        g_ab_phase++;
    LeaveCriticalSection(&g_ab_lock);
}

static void ab_prog_cb(int pct, void *ud)
{ (void)ud; EnterCriticalSection(&g_ab_lock); g_ab_pct = pct;
  LeaveCriticalSection(&g_ab_lock); }

/* Where this program is, so that the image and the staging environment
 * beside it can be named without asking the user where she put them. */
/* BOTH CONVERSIONS ARE CHECKED, and neither used to be.
 *
 * GetModuleFileNameW truncates silently when the path does not fit and
 * reports the buffer size, so a deep install directory produced a
 * DIFFERENT directory that looked plausible. WideCharToMultiByte
 * returns 0 and writes nothing when the UTF-8 form does not fit --
 * 260 wide characters of CJK can need 780 bytes and `dir` holds 519 --
 * leaving uninitialised stack to be formatted into a path the
 * installer then tries to read five gigabytes from.
 *
 * An empty `out` is the honest answer to both, and every caller
 * already treats it as "not there". */
static void beside_me(char *out, size_t n, const char *leaf)
{
    out[0] = 0;
    wchar_t w[MAX_PATH];
    DWORD k = GetModuleFileNameW(NULL, w, MAX_PATH);
    if (k == 0 || k >= MAX_PATH) return;    /* 0, or truncated */
    w[k] = 0;
    for (DWORD i = k; i > 0; i--)
        if (w[i - 1] == L'\\' || w[i - 1] == L'/') { w[i] = 0; break; }
    char dir[MAX_PATH * 4];
    int dn = WideCharToMultiByte(CP_UTF8, 0, w, -1, dir, sizeof dir, NULL, NULL);
    if (dn <= 0) return;
    dir[sizeof dir - 1] = 0;
    if (_snprintf(out, n - 1, "%s%s", dir, leaf) < 0) { out[0] = 0; return; }
    out[n - 1] = 0;
}

static HANDLE g_dl_thread;             /* defined with download_begin() */

static DWORD WINAPI ab_worker(LPVOID p)
{
    (void)p;
    /* WAIT FOR THE EARLY DOWNLOAD FIRST, so nothing writes \AurOS\ while
     * ab_run's ensure_image() reads or rewrites it. This wait is on the
     * worker thread, not the UI thread, so the window keeps painting;
     * and because the download ran while she read the pages, it is
     * usually already finished and this returns at once. A failed early
     * fetch is not fatal -- ab_run's own ensure_image() retries it. */
    if (g_dl_thread) {
        WaitForSingleObject(g_dl_thread, INFINITE);
        CloseHandle(g_dl_thread);
        g_dl_thread = NULL;
    }
    int rc = ab_run(AB_HANDOFF, &g_ab_choice, &g_report, &g_ab_machine,
                    ab_say_cb, ab_prog_cb, NULL, g_ab_why, sizeof g_ab_why);
    InterlockedExchange(&g_ab_state, rc == 0 ? 2 : 3);
    return 0;
}

/* WHAT SHE CHOSE, as values the installed system can act on: the look
 * from the one question she is asked (STYLES[]), and what Windows
 * already knows about her language, keyboard and time zone
 * (detect_defaults) -- asked of Windows, not of her. Its own function so
 * that --navtest can read back exactly what the engine is handed. */
/* D10, D11: OFFERED ONLY WHEN THE ENGINE CAN DO IT. phases.h defines
 * AB_CHOICE_BRING_WIFI and AB_CHOICE_BITLOCKER_PAUSE when ab_choice has
 * bring_wifi and bitlocker_pause and the engine acts on them; until then
 * the box is not drawn and the drive lock is not mentioned, because a
 * promise nothing keeps is worse than no promise. */
static int wifi_offered(void)
{
#ifdef AB_CHOICE_BRING_WIFI
    return g_report.wifi_connected;
#else
    return 0;
#endif
}

static int bitlocker_paused_here(void)
{
#ifdef AB_CHOICE_BITLOCKER_PAUSE
    for (int i = 0; i < g_report.n_volumes; i++)
        if (g_report.volumes[i].bitlocker == 1) return 1;
#endif
    return 0;
}

static void choices_from_page(ab_choice *c)
{
    int s = style_ok() ? g_sel_style : STYLE_DEFAULT;
    _snprintf(c->shell_archetype, sizeof c->shell_archetype - 1, "%s", STYLES[s].shell);
    _snprintf(c->theme, sizeof c->theme - 1, "%s", STYLES[s].theme);
    _snprintf(c->language, sizeof c->language - 1, "%s", g_lang_v[g_sel_lang]);
    _snprintf(c->keyboard, sizeof c->keyboard - 1, "%s", g_kbd_v[g_sel_kbd]);
    _snprintf(c->timezone, sizeof c->timezone - 1, "%s", g_tz_v[g_sel_tz]);
#ifdef AB_CHOICE_BRING_WIFI
    c->bring_wifi = wifi_offered() && g_bring_wifi;
#endif
#ifdef AB_CHOICE_BITLOCKER_PAUSE
    c->bitlocker_pause = bitlocker_paused_here();
#endif
}

/* WHERE THE IMAGE IS, OR WILL BE -- the part of the choice that does
 * not depend on anything she picks on the pages, so it can be filled in
 * the moment the check passes and the early download (download_begin)
 * can start while she reads. Both install_begin() and download_begin()
 * fill their own ab_choice with this, so the two never share a struct
 * one writes while the other reads. */
static void choice_image_source(ab_choice *c)
{
    memset(c, 0, sizeof *c);
    _snprintf(c->profile, sizeof c->profile - 1, "%s", "desktop");
    /* NO MEMORY STICK. See page_backup() and docs/AURBRIDGE.md,
     * "Installing without a memory stick". */
    c->no_stick = 1;
    c->stick_serial[0] = 0;
    /* WHERE THE IMAGE GOES: \AurOS\ on the drive Windows started from,
     * because that is the volume the staging environment will mount
     * after the restart -- the journal names that partition and no
     * other. An image already sitting beside the installer is moved
     * there rather than downloaded again. */
    {
        char sd[16] = "C:";
        DWORD k = GetEnvironmentVariableA("SystemDrive", sd, sizeof sd);
        if (k == 0 || k >= sizeof sd || sd[1] != ':') snprintf(sd, sizeof sd, "C:");
        sd[2] = 0;
        _snprintf(c->image_path, sizeof c->image_path - 1,
                  "%s\\AurOS\\auros-desktop.img", sd);
    }
    beside_me(c->image_alt_path, sizeof c->image_alt_path, "auros-desktop.img");
    c->pieces_text = AUROS_PIECES_TEXT;
    /* WHERE TO GET IT, BAKED IN WHEN THIS WAS BUILT. Empty in a
     * developer build, which then requires the image to be sitting
     * there already -- the arrangement this product had before it was
     * one file. */
    _snprintf(c->image_url, sizeof c->image_url - 1, "%s", AUROS_IMAGE_URL);
    _snprintf(c->image_sha256, sizeof c->image_sha256 - 1, "%s", AUROS_IMAGE_SHA256);
    c->image_expect = AUROS_IMAGE_BYTES;
    /* AND THE STAGING ENVIRONMENT IS INSIDE THIS FILE. Leaving these
     * empty is what says so: phases.c unpacks them from the
     * executable's own resources. */
    c->kernel_path[0] = 0;
    c->initrd_path[0] = 0;
}

/* ── the early download ──────────────────────────────────────────────
 *
 * The five-gigabyte download is the floor of the whole install, and it
 * used to start only after every page was read and the final button
 * pressed -- so the wait came AFTER the reading, not during it. The
 * 2026-10-23 audit put ~6 minutes of staring at a bar where it did not
 * need to be. So the moment the check passes, this starts fetching the
 * image into \AurOS\ in the background, keeping the machine awake, while
 * she reads. It writes nothing but that one file (and its pieces),
 * which is non-destructive and resumable -- closing the window leaves a
 * partial download behind that the next run simply continues.
 *
 * It has its OWN ab_choice and its OWN worker thread. install_begin()'s
 * worker waits for this one to finish before ab_run() touches the
 * image, so the two never write \AurOS\ at once; and if the early fetch
 * failed, ab_run()'s own ensure_image() retries it, so the early
 * download is an optimisation, never a dependency. */
/* g_dl_thread is declared above ab_worker, which waits on it. */
static ab_choice    g_dl_choice;
static volatile LONG g_dl_state;   /* 0 idle, 1 running, 2 done, 3 failed */
static int          g_dl_started;
static char         g_dl_why[400];

static DWORD WINAPI dl_worker(LPVOID p)
{
    (void)p;
    /* The same callbacks as the install: the lines and the percentage
     * land in the shared log, so when she reaches the progress page the
     * download is already reflected there. ab_worker does not run
     * concurrently with this -- it waits for this thread first. */
    int rc = ab_fetch_image(&g_dl_choice, ab_say_cb, ab_prog_cb, NULL,
                            g_dl_why, sizeof g_dl_why);
    InterlockedExchange(&g_dl_state, rc == 0 ? 2 : 3);
    return 0;
}

static void download_begin(void)
{
    if (g_dl_started) return;              /* once per run of the wizard */
    g_dl_started = 1;
    ab_lock_init();
    choice_image_source(&g_dl_choice);
    /* Nothing to fetch in a developer build with no address baked in;
     * the image is expected beside the installer, and ab_run handles
     * that. Only start the background fetch when there is somewhere to
     * fetch from. */
    if (!g_dl_choice.pieces_text[0] && !g_dl_choice.image_url[0]) return;
    plat_keep_awake();
    InterlockedExchange(&g_dl_state, 1);
    g_dl_thread = CreateThread(NULL, 0, dl_worker, NULL, 0, NULL);
    if (!g_dl_thread) { InterlockedExchange(&g_dl_state, 0); g_dl_started = 0; }
}

static void install_begin(void)
{
    ab_lock_init();
    g_log_n = 0;
    for (int i = 0; i < N_PHASES; i++)
        g_ph[i] = PHASES[i].after_restart ? PH_LATER : PH_PENDING;
    g_ph_cur = 0;
    g_ph_prog = 0.f;
    g_ab_phase = -1;
    g_ab_pct = 0;
    g_ab_why[0] = 0;
    g_install_running = 1;
    g_install_finished = 0;

    choice_image_source(&g_ab_choice);
    choices_from_page(&g_ab_choice);
    /* SET BY A PERSON AND NOTHING ELSE. Reaching this function means
     * she has read the page that says what happens and pressed the
     * button on it; phases.c refuses without it, and that refusal is
     * the last line of defence if this screen is ever reorganised. */
    g_ab_choice.consent_given = 1;

    /* The machine stays awake from here through the restart even if the
     * early download never started (a developer build, or she moved
     * faster than download_begin). Idempotent. */
    plat_keep_awake();

    InterlockedExchange(&g_ab_state, 1);
    g_ab_thread = CreateThread(NULL, 0, ab_worker, NULL, 0, NULL);
    if (!g_ab_thread) {
        InterlockedExchange(&g_ab_state, 3);
        _snprintf(g_ab_why, sizeof g_ab_why - 1,
                  "this computer would not start the installer.");
    }
    g_ph[0] = PH_RUNNING;
}

/* Driven from WM_TIMER so the window keeps painting. It reads what the
 * worker has published and never waits for it. */
static void install_tick(void)
{
    if (!g_install_running) return;
    LONG st = g_ab_state;
    int phase, pct;
    EnterCriticalSection(&g_ab_lock);
    phase = g_ab_phase; pct = g_ab_pct;
    LeaveCriticalSection(&g_ab_lock);

    if (phase < 0) phase = 0;
    if (phase >= AB_N) phase = AB_N - 1;
    for (int i = 0; i < phase && i < N_PHASES; i++)
        if (!PHASES[i].after_restart) g_ph[i] = PH_DONE;
    if (phase < N_PHASES && !PHASES[phase].after_restart)
        g_ph[phase] = PH_RUNNING;
    g_ph_cur = phase;
    g_ph_prog = pct / 100.f;

    if (st == 2) {
        for (int i = 0; i < N_PHASES; i++)
            if (!PHASES[i].after_restart) g_ph[i] = PH_DONE;
        g_ph_cur = -1;
        g_install_running = 0;
        g_install_finished = 1;
        g_install_armed = 1;
    } else if (st == 3) {
        if (phase < N_PHASES) g_ph[phase] = PH_FAILED;
        g_ph_cur = -1;
        g_install_running = 0;
        g_install_finished = 1;
        wchar_t w[400];
        MultiByteToWideChar(CP_UTF8, 0,
                            g_ab_why[0] ? g_ab_why : "it stopped", -1, w, 399);
        w[399] = 0;
        EnterCriticalSection(&g_ab_lock);
        if (g_log_n < LOG_MAX) wcscpy(g_log[g_log_n++], w);
        LeaveCriticalSection(&g_ab_lock);
    }
}

/* If the window is closed before the restart, everything that was done
 * is undone -- which before the restart is all of it. */
static void install_cancel(void)
{
    if (g_ab_state == 1) return;      /* a phase is mid-write; let it finish */
    ab_abort(&g_ab_machine, ab_say_cb, NULL);
    /* Give the machine's sleep timers back: the run is over, whether it
     * got as far as the engine or only started the background download.
     * A partial download left in \AurOS\ is harmless and resumes. */
    plat_let_sleep();
}

/* ═══════════════════════════════════════════════════════════════════
 *  What Windows already knows about this user
 * ═══════════════════════════════════════════════════════════════════ */

static void opt_push(wchar_t (*list)[96], char (*vals)[64], int *n,
                     const wchar_t *label, const char *value)
{
    if (!label || !*label || *n >= OPT_MAX) return;
    for (int i = 0; i < *n; i++) if (!_wcsicmp(list[i], label)) return;
    wcsncpy(list[*n], label, 95); list[*n][95] = 0;
    snprintf(vals[*n], 64, "%s", value ? value : "");
    (*n)++;
}
/* The time-zone values are longer ("windows:" and a Windows key name),
 * so they get their own width. */
static void tz_push(const wchar_t *label, const char *value)
{
    if (!label || !*label || g_n_tzs >= OPT_MAX) return;
    for (int i = 0; i < g_n_tzs; i++) if (!_wcsicmp(g_tzs[i], label)) return;
    wcsncpy(g_tzs[g_n_tzs], label, 95); g_tzs[g_n_tzs][95] = 0;
    snprintf(g_tz_v[g_n_tzs], sizeof g_tz_v[0], "%s", value ? value : "");
    g_n_tzs++;
}

typedef DWORD (WINAPI *PFN_GDTZI)(PDYNAMIC_TIME_ZONE_INFORMATION);

static void detect_defaults(void)
{
    /* LANGUAGE: Windows' locale name, "en-GB", is a glibc one, en_GB,
     * with a hyphen for an underscore -- when it has that shape. A name
     * with a script in it ("zh-Hans-CN") keeps its label and gets no
     * value: first boot then leaves the language to Ferry, which reads
     * it out of Windows itself, rather than guessing. */
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    char det_lang_v[64] = "";
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
        if (!GetLocaleInfoEx(name, LOCALE_SLOCALIZEDDISPLAYNAME, g_det_lang, 96))
            wcsncpy(g_det_lang, name, 95);
        char a[LOCALE_NAME_MAX_LENGTH];
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, a, sizeof a, NULL, NULL) > 0) {
            size_t l = strlen(a);
            int ok = (l == 5 || l == 6) && a[l - 3] == '-';
            for (size_t i = 0; ok && i < l; i++) {
                char ch = a[i];
                if (i < l - 3) ok = ch >= 'a' && ch <= 'z';
                else if (i > l - 3) ok = ch >= 'A' && ch <= 'Z';
            }
            if (ok) {
                a[l - 3] = '_';
                snprintf(det_lang_v, sizeof det_lang_v, "%s.UTF-8", a);
            }
        }
    }
    /* KEYBOARD: the layout's id (KLID), which Ferry's table maps to an
     * XKB layout on the other side. The label is Windows' own name for
     * it, out of the registry. */
    wchar_t klid[KL_NAMELENGTH];
    char det_kbd_v[64] = "";
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
        char a[16];
        if (WideCharToMultiByte(CP_UTF8, 0, klid, -1, a, sizeof a, NULL, NULL) > 0 &&
            strlen(a) == 8)
            snprintf(det_kbd_v, sizeof det_kbd_v, "klid:%s", a);
    }
    /* TIME ZONE: Windows' key name ("Pacific Standard Time"), which is
     * what the CLDR table on the other side is keyed by. It is in the
     * DYNAMIC structure, which is Vista and later, reached by
     * GetProcAddress so the binary still loads on anything older. */
    char det_tz_v[96] = "";
    {
        TIME_ZONE_INFORMATION tzi;
        memset(&tzi, 0, sizeof tzi);
        if (GetTimeZoneInformation(&tzi) != TIME_ZONE_ID_INVALID)
            wcsncpy(g_det_tz, tzi.StandardName, 127);
        PFN_GDTZI gd = (PFN_GDTZI)(void (*)(void))GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetDynamicTimeZoneInformation");
        DYNAMIC_TIME_ZONE_INFORMATION d;
        memset(&d, 0, sizeof d);
        char a[160];
        if (gd && gd(&d) != TIME_ZONE_ID_INVALID && d.TimeZoneKeyName[0] &&
            WideCharToMultiByte(CP_UTF8, 0, d.TimeZoneKeyName, -1, a, sizeof a,
                                NULL, NULL) > 0)
            snprintf(det_tz_v, sizeof det_tz_v, "windows:%s", a);
    }

    /* The value Windows is already using goes first and is preselected:
     * the common case should need no clicks at all. */
    opt_push(g_langs, g_lang_v, &g_n_langs, g_det_lang, det_lang_v);
    opt_push(g_langs, g_lang_v, &g_n_langs, L"English (United States)", "en_US.UTF-8");
    opt_push(g_langs, g_lang_v, &g_n_langs, L"English (United Kingdom)", "en_GB.UTF-8");
    opt_push(g_langs, g_lang_v, &g_n_langs, L"Español", "es_ES.UTF-8");
    opt_push(g_langs, g_lang_v, &g_n_langs, L"Français", "fr_FR.UTF-8");
    opt_push(g_langs, g_lang_v, &g_n_langs, L"Deutsch", "de_DE.UTF-8");
    opt_push(g_langs, g_lang_v, &g_n_langs, L"Português (Brasil)", "pt_BR.UTF-8");
    opt_push(g_langs, g_lang_v, &g_n_langs, L"Italiano", "it_IT.UTF-8");
    opt_push(g_langs, g_lang_v, &g_n_langs, L"Polski", "pl_PL.UTF-8");

    opt_push(g_kbds, g_kbd_v, &g_n_kbds, g_det_kbd, det_kbd_v);
    opt_push(g_kbds, g_kbd_v, &g_n_kbds, L"US", "xkb:us");
    opt_push(g_kbds, g_kbd_v, &g_n_kbds, L"United Kingdom", "xkb:gb");
    opt_push(g_kbds, g_kbd_v, &g_n_kbds, L"Spanish", "xkb:es");
    opt_push(g_kbds, g_kbd_v, &g_n_kbds, L"French (AZERTY)", "xkb:fr");
    opt_push(g_kbds, g_kbd_v, &g_n_kbds, L"German (QWERTZ)", "xkb:de");
    opt_push(g_kbds, g_kbd_v, &g_n_kbds, L"Portuguese (Brazil)", "xkb:br");

    tz_push(g_det_tz, det_tz_v);
    tz_push(L"London", "iana:Europe/London");
    tz_push(L"Paris, Berlin, Madrid, Rome", "iana:Europe/Paris");
    tz_push(L"New York (Eastern)", "iana:America/New_York");
    tz_push(L"Chicago (Central)", "iana:America/Chicago");
    tz_push(L"Los Angeles (Pacific)", "iana:America/Los_Angeles");

    if (!g_n_langs) opt_push(g_langs, g_lang_v, &g_n_langs, L"English (United States)", "en_US.UTF-8");
    if (!g_n_kbds)  opt_push(g_kbds, g_kbd_v, &g_n_kbds, L"US", "xkb:us");
    if (!g_n_tzs)   tz_push(L"London", "iana:Europe/London");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Layout
 *
 *  Sized for the screens she has: a 1366x768 laptop gives this window
 *  a client area of about 1120x690, and so does a 1920x1080 one at
 *  150%. Every page fits in that without scrolling; a smaller screen
 *  scrolls, and the buttons stay put.
 * ═══════════════════════════════════════════════════════════════════ */
static RECT g_card, g_body, g_foot;
static int  g_rail_w;

static void layout(void)
{
    int m = S(18);
    g_rail_w = S(262);
    g_card.left   = g_rail_w;
    g_card.top    = m;
    g_card.right  = g_cw - m;
    g_card.bottom = g_ch - m;

    /* WM_GETMINMAXINFO keeps the window above this, but a DPI change can
     * land in between: never hand DrawText an inverted rectangle. */
    if (g_card.right  < g_card.left + S(360)) g_card.right  = g_card.left + S(360);
    if (g_card.bottom < g_card.top  + S(280)) g_card.bottom = g_card.top  + S(280);

    int pad = S(40);
    g_foot.left   = g_card.left;
    g_foot.right  = g_card.right;
    g_foot.top    = g_card.bottom - S(96);
    g_foot.bottom = g_card.bottom;

    g_body.left   = g_card.left + pad;
    g_body.right  = g_card.right - pad;
    if (g_body.right < g_body.left + S(200)) g_body.right = g_body.left + S(200);
    g_body.top    = g_card.top + S(32);
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
        stroke_circle(cx, cy, r, 1.6f, C_MUTED, 1.f);
        return;
    }
    if (state == 1) {                       /* working */
        for (int i = 0; i < 8; i++) {
            float a  = (float)i * 0.7853982f;
            float ph = (float)((g_ticks / 2 - i) & 7) / 7.f;
            fill_circle(cx + cosf(a) * r * 0.78f, cy + sinf(a) * r * 0.78f,
                        r * 0.18f, C_ACCENT, 0.15f + 0.8f * ph);
        }
        return;
    }
    uint32_t c = sev_color(sev);
    if (sev == PF_BLOCK) {
        fill_circle(cx, cy, r, c, 0.18f);
        stroke_circle(cx, cy, r, 1.6f, c, 0.95f);
        glyph_cross(cx, cy, r * 1.35f, c, 1.f);
    } else if (sev == PF_WARN) {
        fill_circle(cx, cy, r, c, 0.16f);
        stroke_circle(cx, cy, r, 1.6f, c, 0.9f);
        glyph_bang(cx, cy, r * 1.3f, c, 1.f);
    } else {
        fill_circle(cx, cy, r, C_OK, 1.f);
        glyph_check(cx, cy, r * 1.25f, C_BG, 1.f);
    }
}

static void draw_rail(void)
{
    int x = S(32);
    int y = S(36);

    fill_diamond((float)(x + S(9)), (float)(y + S(15)), (float)S(10), C_ACCENT, 1.f);
    text_draw(L"AurOS", g_f_h2, C_FG_HI, x + S(30), y, S(170), DT_LEFT);
    text_draw(L"Setup", g_f_small, C_SUBTLE, x + S(30), y + S(32), S(170), DT_LEFT);

    int cur = rail_index_for(g_page);
    int sy  = S(140);
    int step = S(58);
    int lw = g_rail_w - x - S(34);
    for (int i = 0; i < N_RAIL; i++) {
        float cx = (float)(x + S(9));
        float cy = (float)(sy + i * step + S(12));
        if (i < N_RAIL - 1)
            aa_line(cx, cy + S(12), cx, cy + (float)step - S(12), 1.4f, C_OVERLAY, 1.f);

        int blocked = (g_page == PAGE_BLOCKED && i == cur);
        uint32_t c  = blocked ? C_WARM : C_ACCENT;
        int ty = sy + i * step;

        if (i < cur) {
            fill_circle(cx, cy, (float)S(9), C_ACCENT, 1.f);
            glyph_check(cx, cy, (float)S(11), C_BG, 1.f);
            text_draw(RAIL[i].label, g_f_body, C_SUBTLE, x + S(32), ty, lw, DT_WORDBREAK);
        } else if (i == cur) {
            fill_circle(cx, cy, (float)S(14), c, 0.20f);
            fill_circle(cx, cy, (float)S(9), c, 1.f);
            text_draw(RAIL[i].label, g_f_bodyb, blocked ? C_WARM : C_FG_HI,
                      x + S(32), ty, lw, DT_WORDBREAK);
        } else {
            stroke_circle(cx, cy, (float)S(8), 1.6f, C_MUTED, 1.f);
            text_draw(RAIL[i].label, g_f_body, C_MUTED, x + S(32), ty, lw, DT_WORDBREAK);
        }
    }
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
    glyph_arc(cx, cy, s * 0.52f, 2.6f, 2.6f + 5.0f, s * 0.15f, c, a);
    float ex = cx + cosf(2.6f) * s * 0.52f, ey = cy + sinf(2.6f) * s * 0.52f;
    aa_line(ex, ey, ex + s * 0.26f, ey - s * 0.16f, s * 0.15f, c, a);
    aa_line(ex, ey, ex + s * 0.10f, ey + s * 0.28f, s * 0.15f, c, a);
}
static void glyph_clock(float cx, float cy, float s, uint32_t c, float a)
{
    stroke_circle(cx, cy, s * 0.48f, s * 0.13f, c, a);
    aa_line(cx, cy, cx, cy - s * 0.30f, s * 0.12f, c, a);
    aa_line(cx, cy, cx + s * 0.22f, cy + s * 0.08f, s * 0.12f, c, a);
}
static void glyph_drive(float cx, float cy, float s, uint32_t c, float a)
{
    stroke_rr(cx - s * 0.50f, cy - s * 0.30f, s, s * 0.60f, s * 0.12f, s * 0.12f, c, a);
    fill_circle(cx + s * 0.28f, cy + s * 0.05f, s * 0.08f, c, a);
    aa_line(cx - s * 0.32f, cy + s * 0.05f, cx + s * 0.05f, cy + s * 0.05f, s * 0.09f, c, a);
}
static void glyph_look(float cx, float cy, float s, uint32_t c, float a)
{
    stroke_rr(cx - s * 0.50f, cy - s * 0.38f, s, s * 0.70f, s * 0.08f, s * 0.11f, c, a);
    fill_rr(cx - s * 0.42f, cy + s * 0.14f, s * 0.84f, s * 0.12f, s * 0.04f, c, a);
    aa_line(cx - s * 0.18f, cy + s * 0.48f, cx + s * 0.18f, cy + s * 0.48f, s * 0.11f, c, a);
}

/* A glyph, and a sentence beside it. The shape the Ready page is made of. */
enum { G_CHECK = 0, G_UNDO, G_CLOCK, G_DRIVE, G_LOOK };
static int glyph_row(int glyph, const wchar_t *text, uint32_t tint,
                     int x, int y, int w)
{
    int d  = S(34);
    int tx = x + d + S(18);
    int tw = w - (tx - x);
    float cx = (float)x + (float)d * 0.5f, cy = (float)y + S(13);
    switch (glyph) {
    case G_CHECK: glyph_check(cx, cy, (float)S(24), tint, 1.f); break;
    case G_UNDO:  glyph_undo (cx, cy, (float)S(26), tint, 1.f); break;
    case G_CLOCK: glyph_clock(cx, cy, (float)S(26), tint, 1.f); break;
    case G_DRIVE: glyph_drive(cx, cy, (float)S(28), tint, 1.f); break;
    case G_LOOK:  glyph_look (cx, cy, (float)S(26), tint, 1.f); break;
    default: break;
    }
    int th = text_draw(text, g_f_body, C_FG, tx, y, tw, DT_WORDBREAK);
    return (th > S(28) ? th : S(28)) + S(18);
}

/* ═══════════════════════════════════════════════════════════════════
 *  1 · WELCOME, AND THE CHECK
 *
 *  What AurOS is, what happens to Windows, and the check -- running by
 *  itself, on the same screen, from the moment the window opens. She
 *  used to read a page, press Get started, and watch a second page
 *  check; now the check is done while she reads the two sentences.
 * ═══════════════════════════════════════════════════════════════════ */

/* What a check result means to her, for warnings: one line each. */
static const wchar_t *warn_line(const pf_result *r, wchar_t *buf, int cch)
{
    const wchar_t *n = fix_note(r->id);
    if (n) return n;
    a2w(r->title, buf, cch);
    return buf;
}

static int check_panel(int x, int y, int w)
{
    int pad = S(22);
    int first_done = g_pf_valid;
    int revealed = first_done ? g_reveal : 0;
    int all = first_done && !g_checking && g_reveal >= N_CHK;
    int go = all && pf_is_go(&g_report);

    /* measure: heading, chips, and while running one quiet line */
    int head_h = S(36);
    int chip_h = S(40);
    const wchar_t *note = L"This only looks. Nothing on this computer is changed.";
    int note_h = all ? 0 : text_h(note, g_f_body, w - pad * 2, DT_WORDBREAK) + S(12);
    int h = pad + head_h + S(16) + chip_h + S(6) + note_h + pad - S(6);

    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(16), C_SURFACE_HI, 1.f);
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(16), 1.4f,
              go ? C_ACCENT : C_OVERLAY, go ? 0.9f : 1.f);

    float icx = (float)(x + pad + S(17)), icy = (float)(y + pad + head_h / 2);
    const wchar_t *head;
    uint32_t hc = C_FG_HI;
    if (!all) {
        status_icon(icx, icy, (float)S(16), 1, PF_PASS);
        head = L"Looking at this computer…";
    } else if (go) {
        status_icon(icx, icy, (float)S(17), 2, PF_PASS);
        head = L"This computer can take AurOS.";
    } else {
        status_icon(icx, icy, (float)S(17), 2, PF_WARN);
        head = L"Something needs sorting out first.";
        hc = C_WARM;
    }
    RECT hb = { x + pad + S(48), y + pad, x + w - pad, y + pad + head_h };
    text_in(head, g_f_h3, hc, hb, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    int cx = x + pad, cy = y + pad + head_h + S(16);
    for (int i = 0; i < N_CHK; i++) {
        int cw = text_w(CHK[i].label, g_f_body) + S(56);
        if (cx > x + pad && cx + cw > x + w - pad) break;     /* never wraps at 18 px */
        int state = i < revealed ? 2 : 1;
        int sev = state == 2 ? chk_group_sev(i) : PF_PASS;
        fill_rr((float)cx, (float)cy, (float)cw, (float)chip_h, (float)chip_h * 0.5f,
                C_SURFACE, 1.f);
        status_icon((float)(cx + S(21)), (float)(cy + chip_h / 2), (float)S(10), state,
                    sev == PF_INFO ? PF_PASS : sev);
        RECT b = { cx + S(40), cy, cx + cw, cy + chip_h };
        text_in(CHK[i].label, g_f_body, state == 2 ? C_FG_HI : C_SUBTLE, b,
                DT_SINGLELINE | DT_VCENTER);
        cx += cw + S(10);
    }
    if (!all)
        text_draw(note, g_f_body, C_SUBTLE, x + pad, cy + chip_h + S(16),
                  w - pad * 2, DT_WORDBREAK);
    return h;
}

static int page_welcome(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(720) ? S(720) : w;

    y += text_draw(L"Let’s put AurOS on this computer", g_f_title, C_FG_HI,
                   x, y, w, DT_WORDBREAK) + S(14);
    y += text_draw(L"AurOS is a free and simple new way to use this computer.",
                   g_f_body, C_FG, x, y, narrow, DT_WORDBREAK) + S(8);
    y += text_draw(L"Windows stays, with all your files. If AurOS isn’t for you, "
                   L"you can go back to Windows.",
                   g_f_body, C_FG, x, y, narrow, DT_WORDBREAK) + S(24);

    y += check_panel(x, y, narrow) + S(18);

    /* Anything worth knowing that does not stop it: said here, one line
     * each, on the page she is looking at. ("Coming up next" used to
     * promise a page that did not exist.) */
    if (g_pf_valid && !g_checking && g_reveal >= N_CHK && pf_is_go(&g_report) &&
        g_report.n_warn > 0) {
        y += text_draw(L"Good to know", g_f_bodyb, C_WARM, x, y, narrow,
                       DT_SINGLELINE) + S(8);
        for (int i = 0; i < g_report.n; i++) {
            const pf_result *r = &g_report.results[i];
            if (r->sev != PF_WARN) continue;
            wchar_t buf[128];
            const wchar_t *line = warn_line(r, buf, 128);
            fill_circle((float)(x + S(6)), (float)(y + S(12)), (float)S(4), C_WARM, 1.f);
            y += text_draw(line, g_f_body, C_FG, x + S(22), y, narrow - S(22),
                           DT_WORDBREAK) + S(8);
        }
    }
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  1b · BLOCKED
 *
 *  A refusal is a product outcome, not an error dialog. It gets the same
 *  care as the happy path: what we found, what it means for this person,
 *  and the one thing they can do about it. There is no continue button,
 *  no "advanced", no override, and nav_allowed() would refuse one anyway.
 * ═══════════════════════════════════════════════════════════════════ */
/* THE ONE SETTING, DRAWN. A refusal whose remedy is a screen the
 * person has never seen needs more than a sentence, so the Secure Boot
 * card carries a sketch: the way there from Windows, and the firmware's
 * own screen with the one line to change marked. Every maker's screen
 * is a little different; the sketch uses the commonest words and the
 * card's text names the others. draw=0 measures only. */
static int sb_figure(int x, int y, int w, int draw)
{
    static const wchar_t *const way[] = {
        L"Settings", L"System", L"Recovery", L"Advanced startup: Restart now",
        L"Troubleshoot", L"Advanced options", L"UEFI Firmware Settings",
    };
    const int nway = (int)(sizeof way / sizeof way[0]);
    int y0 = y;
    int chip_h = S(26), pad = S(10), gap = S(8);
    int sep_w = text_w(L"\u203A", g_f_small) + S(10);

    if (draw) text_draw(L"THE WAY THERE, FROM WINDOWS", g_f_tiny, C_WARM,
                        x, y, w, DT_SINGLELINE);
    y += S(16) + S(6);
    int cx = x;
    for (int i = 0; i < nway; i++) {
        int cw = text_w(way[i], g_f_small) + pad * 2;
        if (cx > x && cx + cw > x + w) { cx = x; y += chip_h + gap; }
        if (draw) {
            int last = i == nway - 1;
            fill_rr((float)cx, (float)y, (float)cw, (float)chip_h, (float)S(8),
                    C_SURFACE, 0.95f);
            if (last)
                stroke_rr((float)cx, (float)y, (float)cw, (float)chip_h,
                          (float)S(8), 1.2f, C_WARM, 0.85f);
            RECT b = { cx, y, cx + cw, y + chip_h };
            text_in(way[i], g_f_small, last ? C_WARM : C_FG_HI, b,
                    DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            if (i < nway - 1) {
                RECT sb = { cx + cw, y, cx + cw + sep_w, y + chip_h };
                text_in(L"\u203A", g_f_small, C_SUBTLE, sb,
                        DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }
        }
        cx += cw + sep_w;
    }
    y += chip_h + S(18);

    if (draw) text_draw(L"THEN, ON THE COMPUTER'S OWN SETTINGS SCREEN", g_f_tiny, C_WARM,
                        x, y, w, DT_SINGLELINE);
    y += S(16) + S(6);
    int row = S(30), sw = w > S(560) ? S(560) : w;
    int sh = row * 4 + S(10);
    if (draw) {
        fill_rr((float)x, (float)y, (float)sw, (float)sh, (float)S(6), C_BG, 1.f);
        stroke_rr((float)x, (float)y, (float)sw, (float)sh, (float)S(6), 1.f,
                  C_OVERLAY, 1.f);
        fill_rr((float)x + 1.f, (float)y + 1.f, (float)sw - 2.f, (float)row,
                (float)S(5), C_INFO, 0.16f);
        RECT hb = { x + S(12), y, x + sw - S(12), y + row };
        text_in(L"Security  \u203A  Secure Boot", g_f_smallb, C_INFO, hb,
                DT_SINGLELINE | DT_VCENTER);
        static const struct { const wchar_t *k, *v; int mark; } R[] = {
            { L"Secure Boot",                        L"[Enabled]   leave it on", 0 },
            { L"Allow Microsoft 3rd Party UEFI CA",  L"[Disabled]  \u2192  [Enabled]", 1 },
            { L"Secure Boot Mode",                   L"[Standard]", 0 },
        };
        for (int i = 0; i < 3; i++) {
            int ry = y + row * (i + 1) + S(4);
            if (R[i].mark) {
                fill_rr((float)(x + S(6)), (float)ry, (float)(sw - S(12)),
                        (float)(row - S(2)), (float)S(4), C_WARM, 0.14f);
                stroke_rr((float)(x + S(6)), (float)ry, (float)(sw - S(12)),
                          (float)(row - S(2)), (float)S(4), 1.2f, C_WARM, 0.85f);
            }
            RECT kb = { x + S(16), ry, x + sw / 2 + S(40), ry + row - S(2) };
            RECT vb = { x + sw / 2 + S(48), ry, x + sw - S(14), ry + row - S(2) };
            text_in(R[i].k, R[i].mark ? g_f_smallb : g_f_small,
                    R[i].mark ? C_FG_HI : C_SUBTLE, kb,
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            text_in(R[i].v, R[i].mark ? g_f_smallb : g_f_small,
                    R[i].mark ? C_WARM : C_SUBTLE, vb, DT_SINGLELINE | DT_VCENTER);
        }
    }
    y += sh + S(10);
    const wchar_t *cap =
        L"A sketch: every maker's screen looks a little different. Then choose "
        L"Save and Exit (often F10), and run this installer again. Secure Boot "
        L"stays on.";
    y += draw ? text_draw(cap, g_f_small, C_SUBTLE, x, y, w, DT_WORDBREAK)
              : text_h(cap, g_f_small, w, DT_WORDBREAK);
    return y - y0;
}

static int block_card(const pf_result *r, int x, int y, int w)
{
    wchar_t title[128], detail[600], remedy[600];
    a2w(r->title, title, 128);
    a2w(r->detail, detail, 600);
    a2w(r->remedy, remedy, 600);

    int pad = S(22);
    int tx  = x + pad + S(12);
    int tw  = w - (tx - x) - pad;
    int bw  = w - (tx - x) - pad;

    int th = text_h(title, g_f_h3, tw, DT_WORDBREAK);
    int dh = detail[0] ? text_h(detail, g_f_body, bw, DT_WORDBREAK) : 0;

    int rpad = S(16);
    int rlab = S(20);
    int rh   = remedy[0]
        ? text_h(remedy, g_f_body, bw - rpad * 2, DT_WORDBREAK) + rlab + rpad * 2 + S(4)
        : 0;
    int fig = !strcmp(r->id, SBDB_BLOCK_ID);
    int fh  = fig ? sb_figure(tx, 0, bw, 0) : 0;
    int h = pad + th + (dh ? dh + S(8) : 0) + (rh ? rh + S(18) : 0)
          + (fh ? fh + S(18) : 0) + pad;

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
        cy += rh;
    }
    if (fh) sb_figure(tx, cy + S(18), bw, 1);
    return h;
}

static const pf_volume *sys_volume(void);
static int disk_is_slow(void);

/* HOW LONG SWITCHING ENCRYPTION OFF TAKES HERE. Windows decrypts the
 * whole drive in the background: on an SSD about a quarter of an hour to
 * an hour per 256 GB, on a hard disk two to six hours per terabyte
 * (docs/research/journey-audit-2026-10-23.md, section 2). Said in her
 * units, before she presses the button, so "it takes a while" is a
 * number. (The fast path replaces this with a suspend; until then.) */
static void decrypt_time(wchar_t *out, int cch)
{
    const pf_volume *v = sys_volume();
    double gb = v && v->size_bytes ? (double)v->size_bytes / 1e9 : 256.0;
    int slow = disk_is_slow();
    double lo = gb * (slow == 1 ? 0.12 : 0.06), hi = gb * (slow == 0 ? 0.25 : 0.36);
    if (hi < 60.0) {
        int l = (int)(lo / 5.0) * 5, h = ((int)hi + 9) / 10 * 10;
        if (l < 10) l = 10;
        if (h <= l) h = l + 10;
        _snwprintf(out, (size_t)cch, L"%d to %d minutes", l, h);
    } else {
        int l = (int)(lo / 60.0 + 0.5), h = (int)(hi / 60.0 + 0.99);
        if (l < 1) l = 1;
        if (h <= l) h = l + 1;
        if (l == 1 && h == 2) wcscpy(out, L"one to two hours");
        else _snwprintf(out, (size_t)cch, L"%d to %d hours", l, h);
    }
    out[cch - 1] = 0;
}

static int is_bitlocker(const char *id)
{
    return !strcmp(id, "bitlocker-system") || !strcmp(id, "bitlocker-other");
}

static int page_blocked(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(760) ? S(760) : w;
    int cannot = 0, other = 0;
    for (int i = 0; i < g_report.n; i++) {
        if (g_report.results[i].sev != PF_BLOCK) continue;
        fix_kind k = kind_of(&g_report.results[i]);
        if (k == FIX_CANNOT) cannot++; else other++;
    }
    (void)other;
    y += text_draw(!cannot ? L"Almost there. A few things first"
                           : L"AurOS can\u2019t go on this computer yet",
                   g_f_title, C_FG_HI, x, y, w, DT_WORDBREAK) + S(10);
    y += text_draw(L"Nothing on this computer has been changed.",
                   g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(22);

    /* What cannot be fixed, and only that, when there is any: it is the
     * answer, and a charger or a full drive beside it is beside the point
     * (and made the page too long to read). */
    for (int pass = 0; pass < (cannot ? 1 : 2); pass++)
    for (int i = 0; i < g_report.n; i++) {
        const pf_result *r = &g_report.results[i];
        if (r->sev != PF_BLOCK) continue;
        if ((kind_of(r) == FIX_CANNOT) != (pass == 0)) continue;
        /* The Secure Boot setting keeps its card: it has a picture of the
         * one switch to find, and a picture is the fix. */
        if (!strcmp(r->id, SBDB_BLOCK_ID)) {
            y += block_card(r, x, y, narrow) + S(12);
            continue;
        }
        const fix_info *f = fix_lookup(r->id);
        wchar_t prob[160], fix[600];
        if (f) {
            wcsncpy(prob, f->problem, 159); prob[159] = 0;
            wcsncpy(fix, f->fix, 599);      fix[599] = 0;
        } else {
            /* A check this table has never heard of still gets one
             * short line, never the check's own paragraph. */
            a2w(r->title, prob, 160);
            wcscpy(fix, L"AurOS stops here. Nothing was changed.");
        }
        uint32_t dot = C_ERR;
        int j = -1;
        for (int k = 0; k < g_fx_n; k++) if (!strcmp(g_fx[k].id, r->id)) j = k;
        fix_kind k = kind_of(r);
        if (cannot && k != FIX_CANNOT && f) {
            wcsncpy(fix, f->kind == FIX_WAIT ? f->fix
                         : L"We\u2019ll fix this one once the red ones are sorted.", 599);
            fix[599] = 0;
        }
        if (is_bitlocker(r->id) && k == FIX_AUTO && !cannot) {
            wchar_t t[64];
            decrypt_time(t, 64);
            _snwprintf(fix, 599, L"We switch encryption off. On this computer that takes "
                                 L"about %ls; leave it on and plugged in.", t);
            fix[599] = 0;
        }
        if (k == FIX_AUTO || k == FIX_RESTART) dot = C_WARM;
        if (k == FIX_WAIT) dot = C_ACCENT_ALT;
        if (f && f->kind == FIX_RESTART && k == FIX_CANNOT)
            wcscpy(fix, L"Restarting didn\u2019t clear it, so AurOS can\u2019t go on yet.");
        if (j >= 0 && g_fx[j].state == FS_RUNNING) {
            wcscpy(fix, f && f->kind == FIX_RESTART
                        ? L"Restarting Windows now. This installer opens again by itself."
                        : L"Fixing it now\u2026");
            dot = C_ACCENT;
        } else if (j >= 0 && g_fx[j].state == FS_DONE && g_fx[j].after && f && f->after) {
            wcsncpy(fix, f->after, 599); fix[599] = 0;
            if (is_bitlocker(r->id)) {
                wchar_t t[64];
                decrypt_time(t, 64);
                _snwprintf(fix, 599, L"Encryption is being switched off. That takes about "
                                     L"%ls here. Leave it on; we carry on by ourselves.", t);
                fix[599] = 0;
            }
            dot = C_ACCENT;
        } else if (j >= 0 && g_fx[j].state == FS_DONE) {
            wcscpy(fix, L"Done. Checking again\u2026");
            dot = C_ACCENT;
        } else if (j >= 0 && g_fx[j].state == FS_FAILED) {
            _snwprintf(fix, 599, L"We couldn\u2019t fix this: %ls", g_fx[j].why);
            fix[599] = 0;
        }
        int ph = text_h(prob, g_f_bodyb, narrow - S(58), DT_WORDBREAK);
        int fh = text_h(fix, g_f_body, narrow - S(58), DT_WORDBREAK);
        int h = S(14) + ph + S(4) + fh + S(14);
        fill_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(12),
                C_SURFACE_HI, 1.f);
        fill_rr((float)(x + S(16)), (float)(y + S(20)), (float)S(12), (float)S(12),
                (float)S(6), dot, 1.f);
        text_draw(prob, g_f_bodyb, C_FG_HI, x + S(40), y + S(14), narrow - S(58),
                  DT_WORDBREAK);
        text_draw(fix, g_f_body, C_SUBTLE, x + S(40), y + S(14) + ph + S(4),
                  narrow - S(58), DT_WORDBREAK);
        y += h + S(10);
    }

    /* AND WHAT SHE CAN DO INSTEAD. "AurOS can't go on this computer"
     * and a Close button is the end of the road unless the page says
     * where the road goes: one plain sentence for each reason, and the
     * same one only once. */
    if (cannot) {
        const wchar_t *said[8];
        int n_said = 0;
        y += S(8);
        y += text_draw(L"What you can do", g_f_bodyb, C_FG_HI, x, y, narrow,
                       DT_SINGLELINE) + S(8);
        for (int i = 0; i < g_report.n && n_said < 8; i++) {
            const pf_result *r = &g_report.results[i];
            if (r->sev != PF_BLOCK || kind_of(r) != FIX_CANNOT) continue;
            if (!strcmp(r->id, SBDB_BLOCK_ID)) continue;       /* its card says it */
            const wchar_t *s = fix_instead(r->id);
            if (!s) s = L"Keep using Windows as it is. Nothing on this computer was changed.";
            int dup = 0;
            for (int k = 0; k < n_said; k++) if (said[k] == s || !wcscmp(said[k], s)) dup = 1;
            if (dup) continue;
            said[n_said++] = s;
            fill_circle((float)(x + S(6)), (float)(y + S(12)), (float)S(4), C_ACCENT, 1.f);
            y += text_draw(s, g_f_body, C_FG, x + S(22), y, narrow - S(22),
                           DT_WORDBREAK) + S(10);
        }
    }
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  2 · HOW WOULD YOU LIKE IT TO LOOK?
 *
 *  Three pictures of a screen, each drawn in the colours of the AurOS
 *  it gives her, at whatever size the window is -- they are shapes, not
 *  bitmaps, so a 150% laptop gets them as crisp as a 100% one. One is
 *  chosen before she arrives (the one like what she knows), so "Next"
 *  is always a right answer.
 * ═══════════════════════════════════════════════════════════════════ */

/* the wallpaper and frame every picture shares */
static void pic_screen(float x, float y, float w, float h)
{
    fill_rr(x, y, w, h, (float)S(8), SS_BG, 1.f);
    /* themes/sandstone.theme's mesh: two soft washes of its own colours */
    GdiFlush();
    RECT keep = g_clip;
    int cl = (int)x > g_clip.left ? (int)x : g_clip.left;
    int ct = (int)y > g_clip.top ? (int)y : g_clip.top;
    int cr = (int)(x + w) < g_clip.right ? (int)(x + w) : g_clip.right;
    int cb = (int)(y + h) < g_clip.bottom ? (int)(y + h) : g_clip.bottom;
    g_clip.left = cl; g_clip.top = ct; g_clip.right = cr; g_clip.bottom = cb;
    fill_circle(x + w * 0.85f, y + h * 0.15f, h * 0.75f, SS_WALL, 0.75f);
    fill_circle(x + w * 0.05f, y + h * 1.05f, h * 0.60f, SS_ALT, 0.10f);
    g_clip = keep;
}

static void pic_frame(float x, float y, float w, float h)
{
    stroke_rr(x, y, w, h, (float)S(8), (float)S(2), C_BG, 1.f);
}

/* a window: white, a title strip, and a few lines of "text" */
static void pic_window(float x, float y, float w, float h, int traffic, int front)
{
    float r = (float)S(4);
    fill_rr(x, y, w, h, r, SS_SURFACE, 1.f);
    stroke_rr(x, y, w, h, r, 1.f, SS_LINE, front ? 0.9f : 0.5f);
    float th = h * 0.16f;
    fill_rr(x + 1.f, y + 1.f, w - 2.f, th, r, SS_SURF_HI, 1.f);
    if (traffic) {
        float d = th * 0.24f;
        fill_circle(x + th * 0.55f, y + th * 0.55f, d, SS_ERR, 1.f);
        fill_circle(x + th * 1.15f, y + th * 0.55f, d, SS_WARMC, 1.f);
        fill_circle(x + th * 1.75f, y + th * 0.55f, d, SS_ALT, 1.f);
    } else {
        float d = th * 0.30f;
        for (int i = 0; i < 3; i++)
            fill_rr(x + w - th * (0.9f + 0.8f * (float)i), y + th * 0.35f,
                    d, d, 1.f, SS_LINE, 0.6f);
    }
    if (!front) return;
    for (int i = 0; i < 3; i++)
        fill_rr(x + w * 0.08f, y + th + h * (0.14f + 0.15f * (float)i),
                w * (i == 2 ? 0.45f : 0.75f), h * 0.06f, 1.f, SS_MUTED, 1.f);
}

/* a little coloured app symbol */
static const uint32_t PIC_APPS[] = { SS_ACCENT, SS_INFO, SS_ALT, SS_WARMC, SS_PLUM, SS_ERR };

static void style_picture(int kind, float x, float y, float w, float h)
{
    GdiFlush();
    RECT keep = g_clip;
    if ((int)x > g_clip.left) g_clip.left = (int)x;
    if ((int)y > g_clip.top) g_clip.top = (int)y;
    if ((int)(x + w) < g_clip.right) g_clip.right = (int)(x + w);
    if ((int)(y + h) < g_clip.bottom) g_clip.bottom = (int)(y + h);
    pic_screen(x, y, w, h);
    switch (kind) {
    case PIC_WINDOWS: {
        /* two windows, and the bar along the bottom with its button at
         * the left: the shape of every Windows screen she has used */
        pic_window(x + w * 0.07f, y + h * 0.08f, w * 0.50f, h * 0.52f, 0, 0);
        pic_window(x + w * 0.30f, y + h * 0.20f, w * 0.60f, h * 0.56f, 0, 1);
        float bh = h * 0.15f, by = y + h - bh;
        fill_rr(x + 1.f, by, w - 2.f, bh - 1.f, 0.f, SS_SURFACE, 1.f);
        aa_line(x + 1.f, by, x + w - 1.f, by, 1.f, SS_LINE, 0.7f);
        fill_rr(x + w * 0.02f, by + bh * 0.18f, w * 0.17f, bh * 0.64f, (float)S(3),
                SS_ACCENT, 1.f);
        for (int i = 0; i < 3; i++) {
            float bx = x + w * (0.22f + 0.14f * (float)i);
            fill_rr(bx, by + bh * 0.18f, w * 0.12f, bh * 0.64f, (float)S(3),
                    i == 1 ? SS_SURF_HI : SS_BG, 1.f);
            fill_circle(bx + w * 0.03f, by + bh * 0.5f, bh * 0.16f, PIC_APPS[i + 1], 1.f);
        }
        fill_rr(x + w * 0.86f, by + bh * 0.30f, w * 0.10f, bh * 0.16f, 1.f, SS_LINE, 0.8f);
        fill_rr(x + w * 0.88f, by + bh * 0.56f, w * 0.06f, bh * 0.12f, 1.f, SS_LINE, 0.5f);
        break;
    }
    case PIC_MAC: {
        /* a thin bar at the top, one window, and the row of pictures */
        fill_rr(x + 1.f, y + 1.f, w - 2.f, h * 0.07f, 0.f, SS_SURFACE, 0.9f);
        fill_rr(x + w * 0.03f, y + h * 0.022f, w * 0.10f, h * 0.03f, 1.f, SS_LINE, 0.7f);
        pic_window(x + w * 0.12f, y + h * 0.14f, w * 0.76f, h * 0.56f, 1, 1);
        float dw = w * 0.66f, dh = h * 0.17f;
        float dx = x + (w - dw) * 0.5f, dy = y + h - dh - h * 0.05f;
        fill_rr(dx, dy, dw, dh, dh * 0.35f, SS_SURFACE, 0.92f);
        stroke_rr(dx, dy, dw, dh, dh * 0.35f, 1.f, SS_LINE, 0.6f);
        int n = 6;
        float s = dh * 0.66f, gap = (dw - s * (float)n) / (float)(n + 1);
        for (int i = 0; i < n; i++)
            fill_rr(dx + gap + (s + gap) * (float)i, dy + (dh - s) * 0.5f, s, s,
                    s * 0.25f, PIC_APPS[i], 1.f);
        break;
    }
    case PIC_SIMPLE: {
        /* one big page of big buttons, with the next one peeking in at
         * the edge: Rail's Home */
        fill_rr(x + w * 0.94f, y + h * 0.16f, w * 0.10f, h * 0.66f, (float)S(4),
                SS_SURFACE, 0.8f);
        fill_rr(x - w * 0.04f, y + h * 0.16f, w * 0.10f, h * 0.66f, (float)S(4),
                SS_SURFACE, 0.8f);
        float cx = x + w * 0.10f, cy = y + h * 0.07f, cw = w * 0.80f, ch = h * 0.80f;
        fill_rr(cx, cy, cw, ch, (float)S(5), SS_SURFACE, 1.f);
        stroke_rr(cx, cy, cw, ch, (float)S(5), (float)S(1) + 0.5f, SS_ACCENT, 0.9f);
        fill_rr(cx + cw * 0.06f, cy + ch * 0.07f, cw * 0.46f, ch * 0.07f, 1.f, SS_FG, 0.85f);
        float gx = cx + cw * 0.06f, gy = cy + ch * 0.22f;
        float tw = cw * 0.88f / 3.f, tg = cw * 0.025f, tht = ch * 0.34f;
        for (int i = 0; i < 6; i++) {
            float tx = gx + (float)(i % 3) * tw, ty = gy + (float)(i / 3) * (tht + ch * 0.04f);
            fill_rr(tx, ty, tw - tg, tht, (float)S(3), SS_BG, 1.f);
            stroke_rr(tx, ty, tw - tg, tht, (float)S(3), 1.f, SS_LINE, 0.35f);
            fill_circle(tx + (tw - tg) * 0.5f, ty + tht * 0.42f, tht * 0.22f, PIC_APPS[i], 1.f);
            fill_rr(tx + (tw - tg) * 0.28f, ty + tht * 0.74f, (tw - tg) * 0.44f, tht * 0.08f,
                    1.f, SS_MUTED, 1.f);
        }
        for (int i = 0; i < 3; i++)
            fill_circle(x + w * (0.46f + 0.04f * (float)i), y + h * 0.94f, h * 0.018f,
                        i == 0 ? SS_ACCENT : SS_LINE, 1.f);
        break;
    }
    case PIC_ONE: {
        /* one program filling the screen, and the one big Home button */
        float bh = h * 0.20f;
        fill_rr(x + 1.f, y + 1.f, w - 2.f, h * 0.08f, 0.f, SS_SURFACE, 0.9f);
        fill_rr(x + w * 0.05f, y + h * 0.12f, w * 0.90f, h - bh - h * 0.15f, (float)S(4),
                SS_SURFACE, 1.f);
        stroke_rr(x + w * 0.05f, y + h * 0.12f, w * 0.90f, h - bh - h * 0.15f, (float)S(4),
                  1.f, SS_LINE, 0.5f);
        fill_circle(x + w * 0.50f, y + h * 0.36f, h * 0.12f, SS_INFO, 1.f);
        fill_rr(x + w * 0.32f, y + h * 0.54f, w * 0.36f, h * 0.05f, 1.f, SS_MUTED, 1.f);
        fill_rr(x + w * 0.38f, y + h * 0.62f, w * 0.24f, h * 0.04f, 1.f, SS_MUTED, 0.7f);
        float pw = w * 0.36f, ph = bh * 0.62f;
        fill_rr(x + (w - pw) * 0.5f, y + h - bh + (bh - ph) * 0.5f, pw, ph, ph * 0.5f,
                SS_ACCENT, 1.f);
        fill_rr(x + w * 0.44f, y + h - bh * 0.5f - 1.f, w * 0.12f, 2.f, 1.f, SS_SURFACE, 1.f);
        break;
    }
    default: break;
    }
    g_clip = keep;
    pic_frame(x, y, w, h);
}

static int style_card_h(int i, int w)
{
    int pad = S(12);
    int pw = w - pad * 2, ph = pw * 10 / 16;
    return pad + ph + S(14) + text_h(STYLES[i].name, g_f_h3, pw, DT_WORDBREAK) + S(6)
         + text_h(STYLES[i].line, g_f_body, pw, DT_WORDBREAK) + S(16);
}

static void style_card(int i, int x, int y, int w, int h)
{
    const style_info *s = &STYLES[i];
    int sel = (g_sel_style == i);
    int idx = w_add(ID_STYLE + i, W_CARD, x, y, w, h, 1);
    if (idx >= 0) g_w[idx].name = s->name;
    int hot = w_hot(idx);
    int pad = S(12);
    int pw = w - pad * 2, ph = pw * 10 / 16;

    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(16), C_SURFACE_HI,
            sel ? 1.f : (hot ? 0.95f : 0.7f));
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(16),
              sel ? (float)S(3) : (hot ? 2.f : 1.4f),
              sel ? C_ACCENT : (hot ? C_SUBTLE : C_OVERLAY), 1.f);

    style_picture(s->picture, (float)(x + pad), (float)(y + pad), (float)pw, (float)ph);

    if (sel) {
        float r = (float)S(15);
        float bx = (float)(x + w - pad) - r - (float)S(6), by = (float)(y + pad) + r + (float)S(6);
        fill_circle(bx, by, r + (float)S(2), C_BG, 1.f);
        fill_circle(bx, by, r, C_ACCENT, 1.f);
        glyph_check(bx, by, r * 1.2f, C_BG, 1.f);
    }
    int ty = y + pad + ph + S(14);
    ty += text_draw(s->name, g_f_h3, sel ? C_FG_HI : C_FG, x + pad, ty, pw, DT_WORDBREAK) + S(6);
    text_draw(s->line, g_f_body, C_SUBTLE, x + pad, ty, pw, DT_WORDBREAK);
    if (w_focused(idx)) { RECT b = { x, y, x + w, y + h }; focus_ring(b, S(5), S(20)); }
}

static int page_style(int x, int y, int w)
{
    int y0 = y;
    y += text_draw(L"Pick the look you want", g_f_title, C_FG_HI,
                   x, y, w, DT_WORDBREAK) + S(10);
    y += text_draw(L"Pick the one that feels most like home. You can change it "
                   L"later, in Settings.",
                   g_f_body, C_SUBTLE, x, y, w, DT_WORDBREAK) + S(24);

    int gap = S(14);
    int n = N_STYLES;
    /* All in a row while each picture is still big enough to read; two
     * by two on a narrower window, and one under the other below that. */
    int across = n;
    while (across > 1 && (w - gap * (across - 1)) / across < S(170)) across = across > 2 ? 2 : 1;
    int cw = (w - gap * (across - 1)) / across;
    if (across == 1 && cw > S(420)) cw = S(420);
    int h = 0;
    for (int i = 0; i < n; i++) { int t = style_card_h(i, cw); if (t > h) h = t; }
    for (int i = 0; i < n; i++) {
        int cx = x + (i % across) * (cw + gap);
        int cy = y + (i / across) * (h + gap);
        style_card(i, cx, cy, cw, h);
    }
    int rows = (n + across - 1) / across;
    y += rows * h + (rows - 1) * gap;
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  3 · READY
 *
 *  Under eighty words: what stays, where AurOS goes, the one restart,
 *  the way back, and how long it takes on THIS computer. One button.
 *  No word to type, no box to tick, no drive model number.
 * ═══════════════════════════════════════════════════════════════════ */
static const pf_disk *sys_disk(void)
{
    /* system_disk is a PhysicalDrive number, not a position in disks[]:
     * the two differ as soon as a number is skipped. */
    for (int i = 0; i < g_report.n_disks; i++)
        if (g_report.disks[i].index == g_report.system_disk) return &g_report.disks[i];
    return NULL;
}

/* The size on the box it came in: a "256 GB" drive is 238 GB to
 * Windows' arithmetic, and 256 is the number she has seen written. */
static void size_words(uint64_t b, wchar_t *out, int cch)
{
    double gb = (double)b / 1e9;
    if (gb >= 999.5) {
        double tb = gb / 1000.0;
        if (tb - floor(tb) < 0.05 || tb >= 10) _snwprintf(out, (size_t)cch, L"%.0f TB", tb);
        else _snwprintf(out, (size_t)cch, L"%.1f TB", tb);
    } else {
        _snwprintf(out, (size_t)cch, L"%.0f GB", gb);
    }
    out[cch - 1] = 0;
}

/* ── How long, honestly ────────────────────────────────────────────
 *
 * The download is most of it (and size is known: the piece list baked
 * into this installer says it to the byte). After that it depends on
 * the drive: an SSD makes room and writes AurOS in a few minutes, a
 * hard disk takes several times as long, and preflight asks the drive
 * which it is (seek_penalty). These are for the install as the fast
 * path does it (docs/handoff/SESSION-2026-10-23.md, D1): the slow work
 * in Windows while the download runs, about a minute after the restart. */
#define ETA_LINK_BPS        3000000.0  /* 24 Mbit/s, until one is measured */
#define ETA_SSD_WORK_MIN    4
#define ETA_HDD_WORK_MIN    20
#define ETA_SSD_AFTER_MIN   3          /* the restart, the check, first start */
#define ETA_HDD_AFTER_MIN   6
#define DL_BYTES_FALLBACK   1800000000ULL

static uint64_t download_bytes(void)
{
    static int known = -1;
    static uint64_t bytes;
    if (known < 0) {
        static ab_pieces p;
        char why[200];
        known = 0;
        if (AUROS_PIECES_TEXT[0] &&
            ab_pieces_parse(AUROS_PIECES_TEXT, &p, why, sizeof why) == 0 && p.total) {
            bytes = p.total;
            known = 1;
        }
    }
    return known ? bytes : DL_BYTES_FALLBACK;
}

/* -1 unknown, 0 SSD (no seek penalty), 1 a spinning disk */
static int disk_is_slow(void)
{
    const pf_disk *d = sys_disk();
    return d ? d->seek_penalty : -1;
}

static int after_restart_min(void)
{
    int s = disk_is_slow();
    return s == 1 ? ETA_HDD_AFTER_MIN : s == 0 ? ETA_SSD_AFTER_MIN : ETA_HDD_AFTER_MIN;
}

/* HER INTERNET, MEASURED. The download starts the moment the check
 * passes (download_begin), so by the Ready page there is usually a rate
 * to go on instead of an assumed one. Percent per second, 0 = not yet. */
static ULONGLONG g_dlm_t0;
static int       g_dlm_p0 = -1;
static double    g_dlm_rate;

static void dl_measure(ULONGLONG now)
{
    if (g_dl_state != 1 || !g_ab_lock_ready) return;
    EnterCriticalSection(&g_ab_lock);
    int pct = g_ab_pct;
    LeaveCriticalSection(&g_ab_lock);
    if (g_dlm_p0 < 0 || pct < g_dlm_p0) { g_dlm_t0 = now; g_dlm_p0 = pct; return; }
    if (now - g_dlm_t0 >= 8000 && pct - g_dlm_p0 >= 1)
        g_dlm_rate = (double)(pct - g_dlm_p0) / ((double)(now - g_dlm_t0) / 1000.0);
}

static void eta_range(int *lo, int *hi)
{
    /* The download: assumed (a wide range), measured (a narrow one), or
     * already done. */
    double dl = (double)download_bytes() / ETA_LINK_BPS / 60.0;
    double dl_lo = 0.5, dl_hi = 1.6;
    if (g_dl_state == 2) {
        dl = 0.0;
    } else if (g_dl_state == 1 && g_dlm_rate > 0.0 && g_ab_lock_ready) {
        EnterCriticalSection(&g_ab_lock);
        int pct = g_ab_pct;
        LeaveCriticalSection(&g_ab_lock);
        dl = (100.0 - (double)pct) / g_dlm_rate / 60.0;
        dl_lo = 0.9; dl_hi = 1.3;
    }
    int s = disk_is_slow();
    double lo_m = dl * dl_lo + (s == 1 ? ETA_HDD_WORK_MIN + ETA_HDD_AFTER_MIN
                                       : ETA_SSD_WORK_MIN + ETA_SSD_AFTER_MIN);
    double hi_m = dl * dl_hi + (s == 0 ? ETA_SSD_WORK_MIN * 1.5 + ETA_SSD_AFTER_MIN
                                       : ETA_HDD_WORK_MIN * 1.5 + ETA_HDD_AFTER_MIN);
    int l = (int)(lo_m / 5.0) * 5;
    if (l < 5) l = 5;
    int h = (int)ceil(hi_m / 5.0) * 5;
    if (h > 30) h = (int)ceil(hi_m / 15.0) * 15;
    if (h <= l) h = l + 5;
    *lo = l; *hi = h;
}

static const pf_volume *sys_volume(void)
{
    if (g_report.system_volume >= 0 && g_report.system_volume < g_report.n_volumes)
        return &g_report.volumes[g_report.system_volume];
    return NULL;
}

/* HOW MUCH AurOS TAKES, AND WHAT WINDOWS KEEPS, the way the install
 * will decide it (shrink_pick_target in src/aurstage/shrink.c): Windows
 * keeps 15% of its drive free, and never less than 20 GiB; AurOS takes
 * what is left of the free space, up to 40 GiB, and never less than it
 * needs. Said in GiB with "GB" on them, which is what Windows' own
 * Explorer shows her. 0 for both when the space was not measured. */
#define GIB (1024ULL * 1024 * 1024)
static void space_split(uint64_t *auros, uint64_t *windows_free)
{
    *auros = 0; *windows_free = 0;
    const pf_volume *v = sys_volume();
    if (!v || !v->shrink_measured) return;
    uint64_t reclaimable = v->offline_shrinkable;
    uint64_t reserve = v->size_bytes / 100 * 15;
    if (reserve < 20 * GIB) reserve = 20 * GIB;
    uint64_t headroom = reclaimable > reserve ? reclaimable - reserve : 0;
    uint64_t share = headroom < 40 * GIB ? headroom : 40 * GIB;
    if (share < PF_AUROS_NEED_BYTES) share = PF_AUROS_NEED_BYTES;
    if (share > reclaimable) share = reclaimable;
    *auros = share;
    *windows_free = reclaimable - share;
}

/* A tick box she can untick, ticked for her. The whole row is the
 * target, at least 44 px tall. */
static int toggle_row(int id, int on, const wchar_t *title, const wchar_t *sub,
                      int x, int y, int w)
{
    int pad = S(12), box = S(26);
    int tx = x + pad + box + S(16), tw = w - (tx - x) - pad;
    int th = text_h(title, g_f_bodyb, tw, DT_WORDBREAK);
    int sh = sub ? text_h(sub, g_f_body, tw, DT_WORDBREAK) : 0;
    int h = pad * 2 + th + (sub ? sh + S(2) : 0);
    if (h < S(52)) h = S(52);
    int idx = w_add(id, W_TOGGLE, x, y, w, h, 1);
    if (idx >= 0) g_w[idx].name = title;
    int hot = w_hot(idx);
    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(12), C_SURFACE_HI,
            hot ? 1.f : 0.75f);
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(12), 1.2f,
              hot ? C_SUBTLE : C_OVERLAY, 1.f);
    float bx = (float)(x + pad), by = (float)(y + (h - box) / 2);
    if (on) {
        fill_rr(bx, by, (float)box, (float)box, (float)S(6), C_ACCENT, 1.f);
        glyph_check(bx + box * 0.5f, by + box * 0.5f, (float)box * 0.62f, C_BG, 1.f);
    } else {
        stroke_rr(bx, by, (float)box, (float)box, (float)S(6), 2.f, C_SUBTLE, 1.f);
    }
    int ty = y + (h - th - (sub ? sh + S(2) : 0)) / 2;
    text_draw(title, g_f_bodyb, C_FG_HI, tx, ty, tw, DT_WORDBREAK);
    if (sub) text_draw(sub, g_f_body, C_SUBTLE, tx, ty + th + S(2), tw, DT_WORDBREAK);
    if (w_focused(idx)) { RECT b = { x, y, x + w, y + h }; focus_ring(b, S(4), S(16)); }
    return h;
}

static int page_ready(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(720) ? S(720) : w;
    wchar_t buf[300], sz[32];

    y += text_draw(L"Ready to install", g_f_title, C_FG_HI, x, y, w,
                   DT_WORDBREAK) + S(26);

    y += glyph_row(G_CHECK, L"Windows and all your files stay on this computer.",
                   C_ACCENT, x, y, narrow);

    const pf_disk *d = sys_disk();
    wchar_t drive[64] = L"";
    if (d && d->size_bytes) {
        size_words(d->size_bytes, sz, 32);
        _snwprintf(drive, 63, L" (%ls)", sz);
        drive[63] = 0;
    }
    uint64_t take, keep;
    space_split(&take, &keep);
    if (take)
        _snwprintf(buf, 299, L"AurOS takes %d GB beside Windows on this computer\u2019s "
                             L"own drive%ls. Windows keeps %d GB free.",
                   (int)((take + GIB / 2) / GIB), drive, (int)(keep / GIB));
    else
        _snwprintf(buf, 299, L"AurOS takes up to 40 GB beside Windows on this "
                             L"computer\u2019s own drive%ls. Windows keeps at least "
                             L"20 GB free.", drive);
    buf[299] = 0;
    y += glyph_row(G_DRIVE, buf, C_ACCENT, x, y, narrow);

    y += glyph_row(G_LOOK, STYLES[g_sel_style].ready, C_ACCENT, x, y, narrow);

    /* D11, in her words, on a PC whose drive is locked. */
    if (bitlocker_paused_here())
        y += glyph_row(G_CHECK, L"Windows’ drive lock is paused for one restart and "
                                L"turns itself back on. Nothing is unlocked or removed.",
                       C_ACCENT, x, y, narrow);

    y += glyph_row(G_UNDO, L"The computer restarts once. Then AurOS asks if everything "
                           L"works. Say No, and you are back in Windows.",
                   C_ACCENT, x, y, narrow);

    int lo, hi;
    eta_range(&lo, &hi);
    _snwprintf(buf, 299, L"It takes about %d to %d minutes.%ls", lo, hi,
               disk_is_slow() == 1
                 ? L" This computer has an older, slower kind of drive." : L"");
    buf[299] = 0;
    y += glyph_row(G_CLOCK, buf, C_ACCENT, x, y, narrow);

    /* D10: her Wi-Fi, ticked for her, said by its own name. */
    if (wifi_offered()) {
        static wchar_t title[200];
        wchar_t net[128];
        MultiByteToWideChar(CP_UTF8, 0, g_report.wifi_name, -1, net, 127);
        net[127] = 0;
        if (net[0]) _snwprintf(title, 199, L"Bring my Wi-Fi across (%ls)", net);
        else        wcscpy(title, L"Bring my Wi-Fi across");
        title[199] = 0;
        y += S(4);
        y += toggle_row(ID_WIFI, g_bring_wifi, title,
                        L"So AurOS is online the moment it starts.", x, y, narrow);
    }
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  4 · PROGRESS
 *
 *  One bar, one sentence, and how long is left -- measured, not
 *  guessed, once there is enough to measure. Then "Restart now", and
 *  a restart by itself after a minute she can watch count down.
 * ═══════════════════════════════════════════════════════════════════ */

/* How much of the work before the restart each phase is. The download
 * is nearly all of it. A phase the engine adds later gets a small share
 * rather than none, so the bar never stalls at the end of one. */
static float phase_weight(int ph)
{
    switch (ph) {
    case AB_INSPECT: return 2.f;
    case AB_CONSENT: return 1.f;
    case AB_PREPARE: return 90.f;
    case AB_HANDOFF: return 7.f;
    default:         return 5.f;
    }
}

static float overall_fraction(int phase, int pct)
{
    float total = 0.f, done = 0.f;
    for (int i = 0; i < AB_N; i++) total += phase_weight(i);
    if (phase < 0) return 0.f;
    if (phase >= AB_N) return 1.f;
    for (int i = 0; i < phase; i++) done += phase_weight(i);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    done += phase_weight(phase) * (float)pct / 100.f;
    return total > 0.f ? done / total : 0.f;
}

/* A sample every few seconds; the rate is taken over the last minute
 * or so, so a stall shows and a burst does not promise too much. */
#define ETA_SAMPLES 24
static struct { ULONGLONG t; float f; } g_eta_s[ETA_SAMPLES];
static int       g_eta_n, g_eta_head;
static int       g_eta_min = -1;       /* minutes left, -1 not known yet */
static ULONGLONG g_eta_shown;

static void eta_reset(void) { g_eta_n = 0; g_eta_head = 0; g_eta_min = -1; g_eta_shown = 0; }

static void eta_sample(float f, ULONGLONG now)
{
    int last = (g_eta_head + ETA_SAMPLES - 1) % ETA_SAMPLES;
    if (g_eta_n && now - g_eta_s[last].t < 3000) return;
    g_eta_s[g_eta_head].t = now; g_eta_s[g_eta_head].f = f;
    g_eta_head = (g_eta_head + 1) % ETA_SAMPLES;
    if (g_eta_n < ETA_SAMPLES) g_eta_n++;
    int first = (g_eta_head + ETA_SAMPLES - g_eta_n) % ETA_SAMPLES;
    ULONGLONG dt = now - g_eta_s[first].t;
    float df = f - g_eta_s[first].f;
    if (dt < 10000 || df < 0.004f) return;
    double rate = (double)df / ((double)dt / 1000.0);
    double rem = (1.0 - (double)f) / rate / 60.0 + after_restart_min();
    int m = (int)ceil(rem);
    if (g_eta_min < 0 || now - g_eta_shown > 5000) { g_eta_min = m; g_eta_shown = now; }
}

static void eta_words(int m, wchar_t *out, int cch)
{
    if (m <= 1)       wcscpy(out, L"About a minute left");
    else if (m < 10)  _snwprintf(out, (size_t)cch, L"About %d minutes left", m);
    else if (m < 60)  _snwprintf(out, (size_t)cch, L"About %d minutes left", ((m + 4) / 5) * 5);
    else              wcscpy(out, L"More than an hour left");
    out[cch - 1] = 0;
}

static const wchar_t *stage_sentence(int phase)
{
    switch (phase) {
    case AB_INSPECT: return L"Checking this computer once more.";
    case AB_CONSENT: return L"Getting ready.";
    case AB_PREPARE: return L"Downloading AurOS, and checking every piece of it.";
    case AB_HANDOFF: return L"Getting the restart ready.";
    default:         return L"Setting up AurOS.";
    }
}

/* THE BAR NEVER GOES BACKWARDS. The early download (download_begin)
 * reports through the same callbacks before the engine's phases start,
 * and the engine then starts again at its first phase: shown as it is,
 * the bar would fall back to nothing just after the longest wait. */
static float g_bar_f;

static int early_download_running(void) { return g_dl_state == 1; }

static int install_ok(void)     { return g_install_finished && g_ab_state == 2; }
static int install_failed(void) { return g_install_finished && g_ab_state == 3; }

static void read_engine(int *phase, int *pct)
{
    *phase = 0; *pct = 0;
    if (!g_ab_lock_ready) return;
    EnterCriticalSection(&g_ab_lock);
    *phase = g_ab_phase; *pct = g_ab_pct;
    LeaveCriticalSection(&g_ab_lock);
}

static int page_progress(int x, int y, int w)
{
    int y0 = y;
    int narrow = w > S(720) ? S(720) : w;
    wchar_t buf[520];

    if (install_ok()) {
        y += text_draw(L"Ready to restart", g_f_title, C_FG_HI, x, y, w,
                       DT_WORDBREAK) + S(18);
        y += text_draw(L"Everything is ready. The computer restarts once to finish "
                       L"setting up AurOS.", g_f_body, C_FG, x, y, narrow,
                       DT_WORDBREAK) + S(10);
        y += text_draw(L"Save anything you’re working on first.", g_f_bodyb,
                       C_FG_HI, x, y, narrow, DT_WORDBREAK) + S(22);
        int h = S(74);
        fill_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(14),
                C_SURFACE_HI, 1.f);
        stroke_rr((float)x, (float)y, (float)narrow, (float)h, (float)S(14), 1.4f,
                  g_cd_on ? C_WARM : C_OVERLAY, 1.f);
        glyph_clock((float)(x + S(38)), (float)(y + h / 2), (float)S(28),
                    g_cd_on ? C_WARM : C_SUBTLE, 1.f);
        if (g_cd_on)
            _snwprintf(buf, 519, L"It restarts by itself in %d second%ls.", g_cd_left,
                       g_cd_left == 1 ? L"" : L"s");
        else
            wcscpy(buf, L"It will wait for you. Press Restart now when you’re ready.");
        buf[519] = 0;
        RECT b = { x + S(72), y, x + narrow - S(18), y + h };
        text_in(buf, g_cd_on ? g_f_h3 : g_f_body, g_cd_on ? C_WARM : C_FG, b,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        y += h + S(22);
        y += text_draw(L"When it comes back, AurOS asks if everything works. "
                       L"Say No, and you are back in Windows.",
                       g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK);
        return y - y0;
    }

    if (install_failed()) {
        y += text_draw(L"The install stopped", g_f_title, C_FG_HI, x, y, w,
                       DT_WORDBREAK) + S(18);
        y += text_draw(L"It stopped before the restart. Your files are safe.",
                       g_f_body, C_FG, x, y, narrow, DT_WORDBREAK) + S(16);
        wchar_t why[400];
        MultiByteToWideChar(CP_UTF8, 0, g_ab_why[0] ? g_ab_why : "it stopped", -1,
                            why, 399);
        why[399] = 0;
        _snwprintf(buf, 519, L"What happened: %ls", why);
        buf[519] = 0;
        y += text_draw(buf, g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK) + S(16);
        y += text_draw(L"Press Try again. If it stops again, close this and try "
                       L"another day.", g_f_body, C_FG, x, y, narrow, DT_WORDBREAK);
        return y - y0;
    }

    int phase, pct;
    read_engine(&phase, &pct);
    if (phase < 0) phase = 0;
    if (early_download_running() && phase < AB_PREPARE) phase = AB_PREPARE;
    float f = overall_fraction(phase, pct);
    if (f > g_bar_f) g_bar_f = f;
    f = g_bar_f;

    y += text_draw(L"Setting up AurOS", g_f_title, C_FG_HI, x, y, w,
                   DT_WORDBREAK) + S(22);
    y += text_draw(stage_sentence(phase), g_f_bodyb, C_FG_HI, x, y,
                   narrow, DT_WORDBREAK) + S(16);

    /* The bar. Under 30 px tall at any scale the e2e driver runs at,
     * so its teal is never taken for the button. */
    int bh = S(14);
    fill_rr((float)x, (float)y, (float)narrow, (float)bh, (float)bh * 0.5f,
            C_OVERLAY, 1.f);
    float fw = (float)narrow * clampf(f, 0.f, 1.f);
    if (fw > (float)bh) fill_rr((float)x, (float)y, fw, (float)bh, (float)bh * 0.5f,
                                C_ACCENT, 1.f);
    y += bh + S(14);

    if (g_eta_min >= 0) eta_words(g_eta_min, buf, 520);
    else wcscpy(buf, L"Working out how long this takes…");
    y += text_draw(buf, g_f_body, C_SUBTLE, x, y, narrow, DT_SINGLELINE) + S(28);

    y += text_draw(L"You can leave it — it carries on by itself.", g_f_body, C_FG,
                   x, y, narrow, DT_WORDBREAK) + S(8);
    y += text_draw(L"When it is ready, the computer restarts once. AurOS starts "
                   L"after that.", g_f_body, C_SUBTLE, x, y, narrow, DT_WORDBREAK);
    return y - y0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Footer, and what the primary button means on each page
 * ═══════════════════════════════════════════════════════════════════ */
static int welcome_passed(void)
{
    return g_pf_valid && !g_checking && g_reveal >= N_CHK && pf_is_go(&g_report);
}

static int primary_visible(void)
{
    if (g_page == PAGE_PROGRESS) return g_install_finished;
    return 1;
}

static int primary_enabled(void)
{
    switch (g_page) {
    case PAGE_WELCOME:  return welcome_passed();
    case PAGE_BLOCKED:  return any_cannot() || (!g_fix_busy && !g_checking);
    case PAGE_STYLE:    return nav_allowed(PAGE_READY);
    case PAGE_READY:    return !g_checking && nav_allowed(PAGE_PROGRESS);
    case PAGE_PROGRESS: return g_install_finished && !g_checking;
    default:            return 0;
    }
}

static const wchar_t *primary_label(void)
{
    switch (g_page) {
    case PAGE_WELCOME:  return L"Next";
    /* WHEN IT CANNOT GO ON, THE ONE BUTTON IS CLOSE. "Check again" on an
     * ARM laptop or a BIOS PC is a loop with a teal button on it. The
     * page still checks by itself, in case it was wrong. */
    case PAGE_BLOCKED:  return any_cannot() ? L"Close"
                             : g_checking ? L"Checking…"
                             : g_fix_busy ? L"Fixing…"
                             : fixable_now() ? L"Fix these for me" : L"Check again";
    case PAGE_STYLE:    return L"Next";
    case PAGE_READY:    return g_checking ? L"Checking…" : L"Install AurOS";
    case PAGE_PROGRESS: return install_failed() ? (g_checking ? L"Checking…"
                                                              : L"Try again")
                                                : L"Restart now";
    default:            return L"Next";
    }
}

static const wchar_t *footer_hint(void)
{
    switch (g_page) {
    case PAGE_WELCOME:
        return welcome_passed() ? L"Next, you pick how it looks."
                                : L"This only looks at your computer.";
    case PAGE_BLOCKED:
        return any_cannot()       ? L"Nothing was changed. You can close this."
             : fixable_now() == 2 ? L"It restarts once; this opens again by itself."
             : fixable_now()      ? L"When it’s fixed, we carry on by ourselves."
                                  : L"We check again every few seconds, by ourselves.";
    case PAGE_STYLE:
        return L"The one with the tick is the one you get.";
    case PAGE_READY:
        return L"Closing this before the restart puts everything back.";
    case PAGE_PROGRESS:
        return install_ok()     ? L"Save your work first."
             : install_failed() ? L"You can try again now, or another day."
                                : L"It keeps the computer awake until it is done.";
    default: return L"";
    }
}

static int back_visible(void)
{
    /* Not while "Install AurOS" is checking: that button was pressed. */
    return g_page == PAGE_STYLE || (g_page == PAGE_READY && !g_checking);
}
static page_id back_target(void)
{
    switch (g_page) {
    case PAGE_STYLE: return PAGE_WELCOME;
    case PAGE_READY: return PAGE_STYLE;
    default:         return g_page;
    }
}

static void draw_footer(void)
{
    int pad = S(40);
    aa_line((float)(g_card.left + pad), (float)g_foot.top,
            (float)(g_card.right - pad), (float)g_foot.top, 1.f, C_OVERLAY, 1.f);

    int bh = S(56);
    int by = g_foot.top + (g_foot.bottom - g_foot.top - bh) / 2;
    int bx = g_card.right - pad;

    if (primary_visible()) {
        const wchar_t *plab = primary_label();
        int pw = text_w(plab, g_f_btn) + S(64);
        if (pw < S(190)) pw = S(190);
        bx -= pw;
        draw_button(ID_PRIMARY, plab, bx, by, pw, bh, 1, primary_enabled(), C_ACCENT);
    }

    const wchar_t *second = NULL;
    int sid = 0;
    if (g_page == PAGE_BLOCKED && !any_cannot())      { second = L"Close";   sid = ID_QUIT; }
    else if (back_visible())                          { second = L"Back";    sid = ID_BACK; }
    else if (g_page == PAGE_PROGRESS && install_ok() && g_cd_on)
                                                      { second = L"Not yet"; sid = ID_NOT_YET; }
    else if (g_page == PAGE_PROGRESS && install_failed())
                                                      { second = L"Close";   sid = ID_QUIT; }
    if (second) {
        int sw = text_w(second, g_f_btn) + S(56);
        if (sw < S(130)) sw = S(130);
        bx -= sw + S(14);
        draw_button(sid, second, bx, by, sw, bh, 0, 1, C_ACCENT);
    }

    RECT h = { g_card.left + pad, g_foot.top, bx - S(20), g_foot.bottom };
    if (h.right > h.left + S(80))
        text_in(footer_hint(), g_f_small, C_SUBTLE, h,
                DT_WORDBREAK | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    case PAGE_WELCOME:  return page_welcome(x, y, w);
    case PAGE_BLOCKED:  return page_blocked(x, y, w);
    case PAGE_STYLE:    return page_style(x, y, w);
    case PAGE_READY:    return page_ready(x, y, w);
    case PAGE_PROGRESS: return page_progress(x, y, w);
    default:            return 0;
    }
}

static int scroll_max(void)
{
    int m = g_content_h - g_view_h;
    return m > 0 ? m : 0;
}

static void acc_after_render(void);

static void render(void)
{
    if (!g_px) return;
    layout();
    clip_reset();
    backdrop_blit();
    draw_rail();

    fill_rr((float)g_card.left, (float)g_card.top,
            (float)(g_card.right - g_card.left), (float)(g_card.bottom - g_card.top),
            (float)S(18), C_SURFACE, 1.f);
    stroke_rr((float)g_card.left, (float)g_card.top,
              (float)(g_card.right - g_card.left), (float)(g_card.bottom - g_card.top),
              (float)S(18), 1.2f, C_OVERLAY, 0.9f);

    g_nw = 0;
    if (g_scroll[g_page] > scroll_max()) g_scroll[g_page] = scroll_max();
    set_clip(g_card.left + S(2), g_card.top + S(2), g_card.right - S(2), g_foot.top - S(2));
    g_view_h    = g_body.bottom - g_body.top;
    g_content_h = page_dispatch(g_body.left, g_body.top - g_scroll[g_page],
                                g_body.right - g_body.left) + S(16);
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

        int tx = g_card.right - S(16);
        int th = S(10);
        float frac = (float)g_view_h / (float)g_content_h;
        int hh = (int)((float)(g_body.bottom - g_body.top) * frac);
        if (hh < S(44)) hh = S(44);
        int room = (g_body.bottom - g_body.top) - hh;
        int off  = scroll_max() ? (int)((float)room * (float)g_scroll[g_page]
                                        / (float)scroll_max()) : 0;
        fill_rr((float)tx, (float)g_body.top, (float)th,
                (float)(g_body.bottom - g_body.top), (float)th * 0.5f,
                C_OVERLAY, 0.6f);
        fill_rr((float)tx, (float)(g_body.top + off), (float)th, (float)hh,
                (float)th * 0.5f, C_SUBTLE, 0.9f);
    }

    draw_footer();

    if (g_want_focus_id) {
        for (int i = 0; i < g_nw; i++)
            if (g_w[i].id == g_want_focus_id && g_w[i].enabled) { g_focus = i; break; }
        g_want_focus_id = 0;
    }
    if (g_focus >= g_nw) g_focus = -1;
    GdiFlush();
    acc_after_render();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Actions
 * ═══════════════════════════════════════════════════════════════════ */

/* A check, run in the background. Where it goes when it comes back
 * clean is `next`: the style page from the first screen, or the
 * install itself from "Install AurOS" -- which re-runs every check
 * before anything starts (AURBRIDGE.md: destructive work re-runs
 * preflight and aborts on any block, however long the pages took). */
static void start_check(page_id next)
{
    g_check_next = next;
    g_settle = 0;
    pf_start();
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* The check came back clean on the first screen. THE FAST PATH'S EARLY
 * DOWNLOAD STARTS HERE (docs/handoff/SESSION-2026-10-23.md, D1): from
 * this moment until she presses Install AurOS, the download can run
 * behind the pages. Called once per clean result. */
static void check_passed(void)
{
}

static void check_advance(void)
{
    if (!g_pf_valid || g_checking) return;
    if (g_report.n_block > 0) { goto_page(PAGE_BLOCKED); return; }
    if (g_check_next == PAGE_PROGRESS) {
        /* Only from the page whose button asked for it. She may have gone
         * Back while the check ran; a clean answer then starts nothing. */
        if (g_page != PAGE_READY && g_page != PAGE_PROGRESS) {
            g_check_next = PAGE_STYLE;
            return;
        }
        /* Re-ran preflight and it is still clean: this is the only place
         * the phase list is allowed to start. */
        if (!nav_allowed(PAGE_PROGRESS)) { goto_page(PAGE_READY); return; }
        if (g_nav_mode) { g_page = PAGE_PROGRESS; return; }   /* --navtest: no engine */
        eta_reset();
        g_bar_f = 0.f;
        install_begin();
        goto_page(PAGE_PROGRESS);
        return;
    }
    /* Clean: the first screen says so, with Next. From the stop page
     * too, when what stopped it has been sorted out. */
    g_reveal = N_CHK;
    if (g_page != PAGE_WELCOME) goto_page(PAGE_WELCOME);
    check_passed();
    /* THE CHECK PASSED. Start fetching the image now, in the background,
     * so the five-gigabyte download runs while she reads the pages
     * instead of after. It writes only \AurOS\ (non-destructive,
     * resumable), and install_begin's worker waits for it before doing
     * anything else. Nothing has been consented to yet, and nothing
     * destructive happens here. */
    /* Not from a harness: --navtest, --shot and --selftest only render
     * and report, and a release build has an address to fetch from. */
    if (!g_nav_mode && !g_shot_mode && !g_selftest) download_begin();
    g_want_focus_id = ID_PRIMARY;      /* Next is now the obvious move */
}

static void do_restart(void)
{
    g_cd_on = 0;
    if (g_nav_mode || g_shot_mode) return;
    char why[PLAT_WHY];
    g_restarting = 1;
    if (plat_restart(why, sizeof why) != 0) {
        g_restarting = 0;
        wchar_t w[PLAT_WHY];
        MultiByteToWideChar(CP_UTF8, 0, why, -1, w, PLAT_WHY);
        w[PLAT_WHY - 1] = 0;
        MessageBoxW(g_hwnd, w, L"AurOS Setup", MB_OK | MB_ICONWARNING);
    }
}

/* Seconds left on the restart count, from when it started. */
static int countdown_left(ULONGLONG start, ULONGLONG now)
{
    long long left = COUNTDOWN_SECONDS - (long long)((now - start) / 1000ULL);
    return left < 0 ? 0 : (int)left;
}

static void countdown_start(ULONGLONG now)
{
    g_cd_on = 1;
    g_cd_offered = 1;
    g_cd_start = now;
    g_cd_left = COUNTDOWN_SECONDS;
}

static void countdown_cancel(void) { g_cd_on = 0; }

static void select_style(int i)
{
    if (i < 0) i = 0;
    if (i >= N_STYLES) i = N_STYLES - 1;
    g_sel_style = i;
}

static void do_primary(void)
{
    if (!primary_visible() || !primary_enabled()) { MessageBeep(MB_ICONASTERISK); return; }
    switch (g_page) {
    case PAGE_WELCOME:  goto_page(PAGE_STYLE); g_want_focus_id = ID_PRIMARY; break;
    case PAGE_BLOCKED:  if (any_cannot()) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                        else if (fixable_now()) fix_start();
                        else start_check(PAGE_STYLE);
                        break;
    case PAGE_STYLE:    goto_page(PAGE_READY); g_want_focus_id = ID_PRIMARY; break;
    case PAGE_READY:    start_check(PAGE_PROGRESS); break;
    case PAGE_PROGRESS:
        if (install_ok()) { do_restart(); break; }
        if (install_failed()) {
            /* As if she had closed it and opened it again: what the
             * stopped attempt did is taken back, then a fresh check,
             * then the install. */
            install_cancel();
            g_install_finished = 0;
            start_check(PAGE_PROGRESS);
        }
        break;
    default: break;
    }
}

static void widget_activate(int id)
{
    switch (id) {
    case ID_PRIMARY: do_primary(); return;
    case ID_BACK:    goto_page(back_target()); return;
    case ID_QUIT:    PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return;
    case ID_NOT_YET: countdown_cancel(); return;
    case ID_WIFI:    g_bring_wifi = !g_bring_wifi; return;
    default: break;
    }
    if (id >= ID_STYLE && id < ID_STYLE + N_STYLES) { select_style(id - ID_STYLE); return; }
}

/* The part of the screen a window may use (without the taskbar), on the
 * monitor `h` is on, or the main one before there is a window. */
static int work_area(HWND h, RECT *wa)
{
    HMONITOR mon = h ? MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)
                     : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi)) { *wa = mi.rcWork; return 1; }
    return SystemParametersInfoW(SPI_GETWORKAREA, 0, wa, 0) ? 1 : 0;
}

/* The scroll bar: where its track is, and whether it is being dragged.
 * A bar that only moved with the wheel was a bar people could not use:
 * a touchpad without scrolling, or somebody who expects to drag it. */
static int g_drag_bar = 0, g_drag_y0 = 0, g_drag_s0 = 0;

static int on_scroll_track(int x, int y)
{
    if (g_content_h <= g_view_h) return 0;
    int tx = g_card.right - S(13);
    return x >= tx - S(8) && x <= tx + S(14) && y >= g_body.top && y <= g_body.bottom;
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
    if (id == ID_PRIMARY || id == ID_BACK || id == ID_QUIT || id == ID_NOT_YET) return;
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
                     &g_f_small, &g_f_smallb, &g_f_tiny, &g_f_btn };
    for (int i = 0; i < (int)(sizeof all / sizeof all[0]); i++)
        if (*all[i]) { DeleteObject(*all[i]); *all[i] = NULL; }
}
static void fonts_make(void)
{
    fonts_free();
    /* SIZES ARE FOR HER EYES (docs/EASY.md, rule 3). Every sentence she
     * reads is 18 px at 96 dpi or more -- it was 15, with 13 and 11 for
     * the hints -- and the only smaller type is the footer hint (16) and
     * the labels on the firmware sketch a helper reads (15). */
    /* Display, large and quiet. */
    g_f_title  = mkdisplay(36, FW_NORMAL);
    g_f_h2     = mkdisplay(25, FW_NORMAL);
    /* Text. */
    g_f_h3     = mkfont(21, FW_BOLD);
    g_f_body   = mkfont(18, FW_NORMAL);
    g_f_bodyb  = mkfont(18, FW_SEMIBOLD);
    g_f_small  = mkfont(16, FW_NORMAL);
    g_f_smallb = mkfont(16, FW_BOLD);
    g_f_tiny   = mkfont(15, FW_BOLD);
    g_f_btn    = mkfont(20, FW_SEMIBOLD);
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
 *  For a screen reader
 *
 *  Everything here is drawn by hand, so to Narrator the window was one
 *  nameless rectangle: nothing to read, nothing to press. This answers
 *  WM_GETOBJECT with a small MSAA object instead. The page has a name
 *  (its heading, and where it stands), and every button and look card
 *  on it is a child with its own name, role, state, place and default
 *  action -- the same widget list the mouse and keyboard use, so what
 *  is announced is what is there. When the page changes, the object
 *  says so, and points at the button Enter would press.
 * ═══════════════════════════════════════════════════════════════════ */
static const IID ACC_IID_IUnknown   = { 0x00000000, 0x0000, 0x0000,
                                       { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const IID ACC_IID_IDispatch  = { 0x00020400, 0x0000, 0x0000,
                                       { 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 } };
static const IID ACC_IID_IAccessible = { 0x618736E0, 0x3C3D, 0x11CF,
                                       { 0x81,0x0C,0x00,0xAA,0x00,0x38,0x9B,0x71 } };

#define WM_ACC_ACTIVATE (WM_APP + 7)

static IAccessible *g_acc_std;         /* Windows' own, for parent and siblings */
static wchar_t      g_acc_name[600];   /* the page's name, as last announced    */
static int          g_acc_page = -1;

/* The page, in a sentence or two: what she would read first. Stable
 * while the page stands still -- no seconds, no minutes -- so that a
 * screen reader is told when something changed, not every second. */
static void acc_page_name(wchar_t *out, int n)
{
    out[0] = 0;
    switch (g_page) {
    case PAGE_WELCOME:
        _snwprintf(out, (size_t)n, L"Let’s put AurOS on this computer. %ls",
                   welcome_passed() ? L"This computer can take AurOS. Press Enter for Next."
                   : (g_pf_valid && !g_checking && !pf_is_go(&g_report))
                       ? L"Something needs sorting out first."
                       : L"Looking at this computer. This only looks; nothing is changed.");
        break;
    case PAGE_BLOCKED: {
        int k = _snwprintf(out, (size_t)n, L"%ls. ", any_cannot()
                           ? L"AurOS can’t go on this computer yet"
                           : L"Almost there. A few things first");
        for (int i = 0; i < g_report.n && k > 0 && k < n - 200; i++) {
            const pf_result *r = &g_report.results[i];
            if (r->sev != PF_BLOCK) continue;
            const fix_info *f = fix_lookup(r->id);
            if (f) k += _snwprintf(out + k, (size_t)(n - k), L"%ls. ", f->problem);
        }
        break;
    }
    case PAGE_STYLE:
        _snwprintf(out, (size_t)n, L"Pick the look you want. Chosen: %ls.",
                   STYLES[style_ok() ? g_sel_style : 0].name);
        break;
    case PAGE_READY: {
        int lo, hi;
        eta_range(&lo, &hi);
        _snwprintf(out, (size_t)n, L"Ready to install. Windows and all your files stay "
                   L"on this computer. %ls The computer restarts once. It takes about "
                   L"%d to %d minutes. Press Enter to install AurOS.",
                   STYLES[style_ok() ? g_sel_style : 0].ready, lo, hi);
        break;
    }
    case PAGE_PROGRESS:
        if (install_ok())
            wcsncpy(out, L"Ready to restart. Save anything you’re working on. It "
                         L"restarts by itself in a minute; press Enter to restart now, "
                         L"or Escape for not yet.", (size_t)n);
        else if (install_failed())
            wcsncpy(out, L"The install stopped before the restart. Your files are safe. "
                         L"Press Enter to try again.", (size_t)n);
        else
            wcsncpy(out, L"Setting up AurOS. You can leave it; it carries on by itself.",
                    (size_t)n);
        break;
    default: break;
    }
    out[n - 1] = 0;
}

static int acc_primary_index(void)
{
    for (int i = 0; i < g_nw; i++) if (g_w[i].id == ID_PRIMARY) return i;
    return -1;
}

/* -1 the page itself, 0.. a widget, -2 not one of ours */
static int acc_child(VARIANT v)
{
    if (v.vt != VT_I4) return -2;
    if (v.lVal == CHILDID_SELF) return -1;
    if (v.lVal >= 1 && v.lVal <= g_nw) return (int)v.lVal - 1;
    return -2;
}

static BSTR acc_str(const wchar_t *s) { return s ? SysAllocString(s) : NULL; }

static HRESULT STDMETHODCALLTYPE acc_QueryInterface(IAccessible *t, REFIID riid, void **pv)
{
    if (IsEqualIID(riid, &ACC_IID_IUnknown) || IsEqualIID(riid, &ACC_IID_IDispatch) ||
        IsEqualIID(riid, &ACC_IID_IAccessible)) {
        *pv = t;
        return S_OK;
    }
    *pv = NULL;
    return E_NOINTERFACE;
}
/* One object for the life of the window: nothing to count, nothing to free. */
static ULONG STDMETHODCALLTYPE acc_AddRef(IAccessible *t)  { (void)t; return 2; }
static ULONG STDMETHODCALLTYPE acc_Release(IAccessible *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE acc_GetTypeInfoCount(IAccessible *t, UINT *n)
{ (void)t; *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE acc_GetTypeInfo(IAccessible *t, UINT i, LCID l, ITypeInfo **p)
{ (void)t; (void)i; (void)l; *p = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE acc_GetIDsOfNames(IAccessible *t, REFIID r, LPOLESTR *s,
                                                   UINT c, LCID l, DISPID *d)
{ (void)t; (void)r; (void)s; (void)c; (void)l; (void)d; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE acc_Invoke(IAccessible *t, DISPID d, REFIID r, LCID l, WORD f,
                                            DISPPARAMS *p, VARIANT *v, EXCEPINFO *e, UINT *a)
{ (void)t; (void)d; (void)r; (void)l; (void)f; (void)p; (void)v; (void)e; (void)a;
  return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE acc_get_accParent(IAccessible *t, IDispatch **p)
{
    (void)t;
    if (g_acc_std) return g_acc_std->lpVtbl->get_accParent(g_acc_std, p);
    *p = NULL;
    return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE acc_get_accChildCount(IAccessible *t, LONG *n)
{ (void)t; *n = g_nw; return S_OK; }
static HRESULT STDMETHODCALLTYPE acc_get_accChild(IAccessible *t, VARIANT v, IDispatch **p)
{ (void)t; *p = NULL; return acc_child(v) == -2 ? E_INVALIDARG : S_FALSE; }

static HRESULT STDMETHODCALLTYPE acc_get_accName(IAccessible *t, VARIANT v, BSTR *s)
{
    (void)t;
    int c = acc_child(v);
    if (c == -2) return E_INVALIDARG;
    if (c == -1) { wchar_t b[600]; acc_page_name(b, 600); *s = acc_str(b); return S_OK; }
    *s = acc_str(g_w[c].name);
    return *s ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE acc_get_accValue(IAccessible *t, VARIANT v, BSTR *s)
{ (void)t; (void)v; *s = NULL; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE acc_get_accDescription(IAccessible *t, VARIANT v, BSTR *s)
{
    (void)t;
    int c = acc_child(v);
    if (c == -2) return E_INVALIDARG;
    if (c >= 0 && g_w[c].id >= ID_STYLE && g_w[c].id < ID_STYLE + N_STYLES) {
        *s = acc_str(STYLES[g_w[c].id - ID_STYLE].line);
        return S_OK;
    }
    *s = c == -1 ? acc_str(footer_hint()) : NULL;
    return *s ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE acc_get_accRole(IAccessible *t, VARIANT v, VARIANT *r)
{
    (void)t;
    int c = acc_child(v);
    if (c == -2) return E_INVALIDARG;
    r->vt = VT_I4;
    r->lVal = c == -1 ? ROLE_SYSTEM_CLIENT
            : g_w[c].kind == W_CARD ? ROLE_SYSTEM_RADIOBUTTON
            : g_w[c].kind == W_TOGGLE ? ROLE_SYSTEM_CHECKBUTTON : ROLE_SYSTEM_PUSHBUTTON;
    return S_OK;
}
static int acc_is_focused(int c)
{
    if (GetFocus() != g_hwnd) return 0;
    if (g_focus >= 0) return c == g_focus;
    return c == acc_primary_index();     /* what Enter presses */
}
static HRESULT STDMETHODCALLTYPE acc_get_accState(IAccessible *t, VARIANT v, VARIANT *r)
{
    (void)t;
    int c = acc_child(v);
    if (c == -2) return E_INVALIDARG;
    r->vt = VT_I4;
    if (c == -1) {
        r->lVal = STATE_SYSTEM_FOCUSABLE | (GetFocus() == g_hwnd ? STATE_SYSTEM_FOCUSED : 0);
        return S_OK;
    }
    LONG st = STATE_SYSTEM_FOCUSABLE;
    if (!g_w[c].enabled) st |= STATE_SYSTEM_UNAVAILABLE;
    if (acc_is_focused(c)) st |= STATE_SYSTEM_FOCUSED;
    if (g_w[c].kind == W_CARD && g_w[c].id - ID_STYLE == g_sel_style)
        st |= STATE_SYSTEM_CHECKED | STATE_SYSTEM_SELECTED;
    if (g_w[c].id == ID_WIFI && g_bring_wifi) st |= STATE_SYSTEM_CHECKED;
    r->lVal = st;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE acc_get_accHelp(IAccessible *t, VARIANT v, BSTR *s)
{ (void)t; (void)v; *s = NULL; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE acc_get_accHelpTopic(IAccessible *t, BSTR *f, VARIANT v, LONG *id)
{ (void)t; (void)v; *f = NULL; *id = 0; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE acc_get_accKeyboardShortcut(IAccessible *t, VARIANT v, BSTR *s)
{
    (void)t;
    int c = acc_child(v);
    if (c == -2) return E_INVALIDARG;
    *s = (c >= 0 && g_w[c].id == ID_PRIMARY) ? acc_str(L"Enter")
       : (c >= 0 && (g_w[c].id == ID_BACK || g_w[c].id == ID_NOT_YET)) ? acc_str(L"Escape")
       : NULL;
    return *s ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE acc_get_accFocus(IAccessible *t, VARIANT *r)
{
    (void)t;
    if (GetFocus() != g_hwnd) { r->vt = VT_EMPTY; return S_FALSE; }
    int c = g_focus >= 0 ? g_focus : acc_primary_index();
    r->vt = VT_I4;
    r->lVal = c >= 0 ? c + 1 : CHILDID_SELF;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE acc_get_accSelection(IAccessible *t, VARIANT *r)
{
    (void)t;
    for (int i = 0; i < g_nw; i++)
        if (g_w[i].kind == W_CARD && g_w[i].id - ID_STYLE == g_sel_style) {
            r->vt = VT_I4; r->lVal = i + 1; return S_OK;
        }
    r->vt = VT_EMPTY;
    return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE acc_get_accDefaultAction(IAccessible *t, VARIANT v, BSTR *s)
{
    (void)t;
    int c = acc_child(v);
    if (c == -2) return E_INVALIDARG;
    *s = c < 0 ? NULL : acc_str(g_w[c].kind == W_CARD ? L"Choose"
                              : g_w[c].kind == W_TOGGLE ? (g_bring_wifi ? L"Untick" : L"Tick")
                              : L"Press");
    return *s ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE acc_accSelect(IAccessible *t, LONG flags, VARIANT v)
{
    (void)t; (void)flags;
    int c = acc_child(v);
    if (c < 0) return E_INVALIDARG;
    PostMessageW(g_hwnd, WM_ACC_ACTIVATE, (WPARAM)g_w[c].id, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE acc_accLocation(IAccessible *t, LONG *x, LONG *y, LONG *w,
                                                 LONG *h, VARIANT v)
{
    (void)t;
    int c = acc_child(v);
    if (c == -2) return E_INVALIDARG;
    RECT r;
    if (c == -1) GetClientRect(g_hwnd, &r); else r = g_w[c].r;
    POINT p = { r.left, r.top };
    ClientToScreen(g_hwnd, &p);
    *x = p.x; *y = p.y; *w = r.right - r.left; *h = r.bottom - r.top;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE acc_accNavigate(IAccessible *t, LONG dir, VARIANT from, VARIANT *r)
{
    (void)t;
    int c = acc_child(from);
    r->vt = VT_EMPTY;
    if (c == -2) return E_INVALIDARG;
    int to = -2;
    if (c == -1) {
        if (dir == NAVDIR_FIRSTCHILD) to = 0;
        else if (dir == NAVDIR_LASTCHILD) to = g_nw - 1;
        else if (g_acc_std) return g_acc_std->lpVtbl->accNavigate(g_acc_std, dir, from, r);
    } else {
        if (dir == NAVDIR_NEXT || dir == NAVDIR_DOWN || dir == NAVDIR_RIGHT) to = c + 1;
        if (dir == NAVDIR_PREVIOUS || dir == NAVDIR_UP || dir == NAVDIR_LEFT) to = c - 1;
    }
    if (to < 0 || to >= g_nw) return S_FALSE;
    r->vt = VT_I4; r->lVal = to + 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE acc_accHitTest(IAccessible *t, LONG x, LONG y, VARIANT *r)
{
    (void)t;
    POINT p = { x, y };
    ScreenToClient(g_hwnd, &p);
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    if (!PtInRect(&cr, p)) { r->vt = VT_EMPTY; return S_FALSE; }
    int i = w_find_at(p.x, p.y);
    r->vt = VT_I4;
    r->lVal = i >= 0 ? i + 1 : CHILDID_SELF;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE acc_accDoDefaultAction(IAccessible *t, VARIANT v)
{
    (void)t;
    int c = acc_child(v);
    if (c < 0) return c == -1 ? DISP_E_MEMBERNOTFOUND : E_INVALIDARG;
    if (!g_w[c].enabled) return E_FAIL;
    /* On the window's own thread, between messages, as a click is. */
    PostMessageW(g_hwnd, WM_ACC_ACTIVATE, (WPARAM)g_w[c].id, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE acc_put_accName(IAccessible *t, VARIANT v, BSTR s)
{ (void)t; (void)v; (void)s; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE acc_put_accValue(IAccessible *t, VARIANT v, BSTR s)
{ (void)t; (void)v; (void)s; return E_NOTIMPL; }

static IAccessibleVtbl ACC_VTBL = {
    acc_QueryInterface, acc_AddRef, acc_Release,
    acc_GetTypeInfoCount, acc_GetTypeInfo, acc_GetIDsOfNames, acc_Invoke,
    acc_get_accParent, acc_get_accChildCount, acc_get_accChild, acc_get_accName,
    acc_get_accValue, acc_get_accDescription, acc_get_accRole, acc_get_accState,
    acc_get_accHelp, acc_get_accHelpTopic, acc_get_accKeyboardShortcut,
    acc_get_accFocus, acc_get_accSelection, acc_get_accDefaultAction,
    acc_accSelect, acc_accLocation, acc_accNavigate, acc_accHitTest,
    acc_accDoDefaultAction, acc_put_accName, acc_put_accValue,
};
static IAccessible g_acc = { &ACC_VTBL };

/* After each paint: when the page (or where it stands) changed, say so,
 * and point at the button Enter would press. */
static void acc_after_render(void)
{
    if (!g_hwnd || g_shot_mode || g_nav_mode) return;
    wchar_t b[600];
    acc_page_name(b, 600);
    if (wcscmp(b, g_acc_name) != 0) {
        wcscpy(g_acc_name, b);
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, g_hwnd, OBJID_CLIENT, CHILDID_SELF);
    }
    if (g_acc_page != (int)g_page) {
        g_acc_page = (int)g_page;
        int p = acc_primary_index();
        if (p >= 0 && GetFocus() == g_hwnd)
            NotifyWinEvent(EVENT_OBJECT_FOCUS, g_hwnd, OBJID_CLIENT, p + 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Window
 * ═══════════════════════════════════════════════════════════════════ */
static int g_paints;
static char g_selftest_dir[MAX_PATH];

/* The stop page's quiet re-check came back clean. It goes back to the
 * first screen, which then says so -- NEVER straight into the install,
 * even when the stop came from the check behind "Install AurOS": she
 * presses that button again, with her look still chosen. */
static void blocked_cleared(void)
{
    g_check_next = PAGE_STYLE;
    check_advance();
}

static void selftest_log(void)
{
    char path[MAX_PATH + 32];
    _snprintf(path, sizeof path - 1, "%s/selftest-window.bmp", g_selftest_dir);
    path[sizeof path - 1] = 0;
    save_bmp(path);
    _snprintf(path, sizeof path - 1, "%s/selftest.log", g_selftest_dir);
    path[sizeof path - 1] = 0;
    FILE *f = fopen(path, "w");
    if (!f) return;
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
            g_max_page >= (int)PAGE_STYLE ? "PAST THE GATE" : "gate held");
    /* WHAT A SCREEN READER GETS, asked the way one asks: through
     * oleacc, from the window, not by calling our own object. */
    {
        IAccessible *a = NULL;
        char nm[400] = "", bt[120] = "";
        long kids = -1;
        if (AccessibleObjectFromWindow(g_hwnd, (DWORD)OBJID_CLIENT, &ACC_IID_IAccessible,
                                       (void **)&a) == S_OK && a) {
            VARIANT self; self.vt = VT_I4; self.lVal = CHILDID_SELF;
            BSTR s = NULL;
            if (a->lpVtbl->get_accName(a, self, &s) == S_OK && s)
                WideCharToMultiByte(CP_UTF8, 0, s, -1, nm, sizeof nm, NULL, NULL);
            SysFreeString(s);
            LONG n = 0;
            if (a->lpVtbl->get_accChildCount(a, &n) == S_OK) kids = n;
            int pi = acc_primary_index();
            if (pi >= 0) {
                VARIANT ch; ch.vt = VT_I4; ch.lVal = pi + 1;
                s = NULL;
                if (a->lpVtbl->get_accName(a, ch, &s) == S_OK && s)
                    WideCharToMultiByte(CP_UTF8, 0, s, -1, bt, sizeof bt, NULL, NULL);
                SysFreeString(s);
            }
            a->lpVtbl->Release(a);
        }
        fprintf(f, "screen reader    : \"%s\", %ld parts, button \"%s\"\n", nm, kids, bt);
    }
    fprintf(f, "nav_allowed style..progress: ");
    for (int pg = (int)PAGE_STYLE; pg < (int)PAGE_COUNT; pg++)
        fprintf(f, "%d", nav_allowed((page_id)pg));
    fprintf(f, "\n");
    fclose(f);
}

static void tick(void)
{
    g_ticks++;
    int need = 0;
    dl_measure(GetTickCount64());

    if (pf_poll()) {
        g_settle = 0;
        if (g_page == PAGE_WELCOME && g_check_next != PAGE_PROGRESS) {
            g_reveal = 0;                      /* the ticks come in one by one */
        } else if (g_page == PAGE_BLOCKED && g_report.n_block == 0) {
            blocked_cleared();
        } else {
            g_reveal = N_CHK;
            check_advance();
        }
        need = 1;
    }
    if (g_checking) need = 1;

    if (g_page == PAGE_WELCOME && g_pf_valid && !g_checking) {
        /* The stagger is presentation only: every tick shows the result
         * preflight actually returned, revealed in order. */
        if (g_reveal < N_CHK) { if ((g_ticks & 1) == 0) g_reveal++; need = 1; }
        else if (g_settle >= 0 && ++g_settle == 8) { check_advance(); need = 1; }
    } else if (g_page == PAGE_PROGRESS) {
        install_tick();
        if (g_install_running) {
            int ph, pc;
            read_engine(&ph, &pc);
            if (ph < 0) ph = 0;
            if (early_download_running() && ph < AB_PREPARE) ph = AB_PREPARE;
            float f = overall_fraction(ph, pc);
            if (f > g_bar_f) g_bar_f = f;
            eta_sample(g_bar_f, GetTickCount64());
        }
        if (install_ok() && !g_cd_offered && !g_selftest) {
            countdown_start(GetTickCount64());
            FlashWindow(g_hwnd, TRUE);
        }
        if (g_cd_on) {
            g_cd_left = countdown_left(g_cd_start, GetTickCount64());
            if (g_cd_left == 0) do_restart();
        }
        need = 1;
    } else if (g_page == PAGE_BLOCKED && !g_selftest) {
        if (bg_recheck_tick() && g_report.n_block == 0) blocked_cleared();
        need = 1;
    }

    /* --selftest: wait for the check, then press Enter (Next) and Tab,
     * the way a person with a keyboard would. */
    if (g_selftest) {
        static int step = 0, at = 0;
        int settled = g_pf_valid && !g_checking &&
                      (g_page == PAGE_BLOCKED ||
                       (g_page == PAGE_WELCOME && g_reveal >= N_CHK && g_settle > 8));
        if (step == 0 && settled) {
            step = 1; at = g_ticks;
            /* NEVER ON THE STOP PAGE: Enter there is "Fix these for me",
             * which can empty folders or restart the machine running the
             * test. Only a clean first page gets its Next pressed. */
            if (g_page == PAGE_WELCOME)
                PostMessageW(g_hwnd, WM_KEYDOWN, VK_RETURN, 0);
        } else if (step == 1 && g_ticks > at + 6) {
            step = 2; at = g_ticks;
            PostMessageW(g_hwnd, WM_KEYDOWN, VK_TAB, 0);
        } else if ((step == 2 && g_ticks > at + 6) || g_ticks > 9000) {
            selftest_log();
            PostQuitMessage(0);
            return;
        }
    }
    if (need && g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Every key, in one place, so --navtest presses the same ones. */
static void on_key(WPARAM k)
{
    int fk = (g_focus >= 0 && g_focus < g_nw) ? (int)g_w[g_focus].kind : -1;
    switch (k) {
    case VK_TAB:
        focus_step((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
        break;
    /* ENTER AND SPACE PRESS THE BIG BUTTON, unless she has tabbed to
     * another one. On a picture card they choose it, and go on. */
    case VK_RETURN:
    case VK_SPACE:
        if (fk == W_TOGGLE) {
            if (k == VK_SPACE) widget_activate(g_w[g_focus].id);
            else do_primary();
        }
        else if (fk == W_BUTTON) widget_activate(g_w[g_focus].id);
        else if (fk == W_CARD) { widget_activate(g_w[g_focus].id); do_primary(); }
        else do_primary();
        break;
    /* ESC NEVER DESTROYS ANYTHING: it stops the restart count, or goes
     * back a page, and that is all. It used to close the window. */
    case VK_ESCAPE:
        if (g_cd_on) countdown_cancel();
        else if (back_visible()) goto_page(back_target());
        break;
    case VK_LEFT:
    case VK_RIGHT:
        if (g_page == PAGE_STYLE) {
            select_style(g_sel_style + (k == VK_RIGHT ? 1 : -1));
            g_want_focus_id = ID_STYLE + g_sel_style;
            g_focus_ring = 1;
        } else focus_step(k == VK_RIGHT ? 1 : -1);
        break;
    case VK_UP:    scroll_by(-S(60)); break;
    case VK_DOWN:  scroll_by(S(60));  break;
    case VK_PRIOR: scroll_by(-g_view_h + S(40)); break;
    case VK_NEXT:  scroll_by(g_view_h - S(40));  break;
    case VK_HOME:  g_scroll[g_page] = 0; break;
    case VK_END:   g_scroll[g_page] = scroll_max(); break;
    default: break;
    }
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
         * cannot stretch a short script into minutes. */
        if (!g_selftest) SetTimer(h, 1, 60, NULL);
        return 0;

    case WM_SIZE:
        g_cw = LOWORD(lp); g_ch = HIWORD(lp);
        backbuffer(g_cw, g_ch);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(900);
        mm->ptMinTrackSize.y = S(600);
        /* Never bigger than the screen: a minimum taller than a small
         * laptop's screen put the bottom of every page, and the buttons,
         * where nobody could reach them. */
        RECT wa;
        if (work_area(h, &wa)) {
            if (mm->ptMinTrackSize.x > wa.right - wa.left) mm->ptMinTrackSize.x = wa.right - wa.left;
            if (mm->ptMinTrackSize.y > wa.bottom - wa.top) mm->ptMinTrackSize.y = wa.bottom - wa.top;
        }
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

    case WM_GETOBJECT:
        if ((LONG)lp == (LONG)OBJID_CLIENT) {
            if (!g_acc_std)
                CreateStdAccessibleObject(h, OBJID_CLIENT, &ACC_IID_IAccessible,
                                          (void **)&g_acc_std);
            return LresultFromObject(&ACC_IID_IAccessible, wp, (IUnknown *)&g_acc);
        }
        break;

    case WM_ACC_ACTIVATE:
        /* a screen reader pressed a button or chose a card */
        for (int i = 0; i < g_nw; i++)
            if (g_w[i].id == (int)wp && g_w[i].enabled) { widget_activate((int)wp); break; }
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        g_mouse.x = GET_X_LPARAM(lp); g_mouse.y = GET_Y_LPARAM(lp);
        if (g_drag_bar) {
            int room = g_view_h > 0 ? g_view_h : 1;
            int mx = scroll_max();
            g_scroll[g_page] = g_drag_s0 + (int)((long long)(g_mouse.y - g_drag_y0)
                                                 * g_content_h / room);
            if (g_scroll[g_page] > mx) g_scroll[g_page] = mx;
            if (g_scroll[g_page] < 0) g_scroll[g_page] = 0;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
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
        if (on_scroll_track(g_mouse.x, g_mouse.y)) {
            /* a press on the track jumps there, then drags from there */
            int mx = scroll_max();
            int span = g_body.bottom - g_body.top;
            if (span > 0) g_scroll[g_page] = (int)((long long)(g_mouse.y - g_body.top) * mx / span);
            if (g_scroll[g_page] > mx) g_scroll[g_page] = mx;
            if (g_scroll[g_page] < 0) g_scroll[g_page] = 0;
            g_drag_bar = 1; g_drag_y0 = g_mouse.y; g_drag_s0 = g_scroll[g_page];
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        g_mouse_down = 1;
        g_press_idx  = w_find_at(g_mouse.x, g_mouse.y);
        if (g_press_idx >= 0) { g_focus = g_press_idx; g_focus_ring = 0; }
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        g_mouse.x = GET_X_LPARAM(lp); g_mouse.y = GET_Y_LPARAM(lp);
        if (g_drag_bar) {
            g_drag_bar = 0;
            ReleaseCapture();
            return 0;
        }
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

    case WM_KEYDOWN:
        on_key(wp);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_CLOSE:
        if (g_install_running) {
            if (MessageBoxW(h, L"Stop setting up AurOS?\n\nWindows and your files "
                               L"are not affected either way.",
                            L"AurOS Setup", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2)
                    != IDYES)
                return 0;
        }
        /* READY, AND ABOUT TO BE THROWN AWAY. After the last step the
         * window's X used to close it and take back the restart without
         * a word. */
        if (g_install_armed && !g_restarting) {
            if (MessageBoxW(h, L"Close without restarting?\n\nAurOS is ready, but "
                               L"closing now takes it back, and this computer "
                               L"starts Windows as usual.\n\nTo finish, choose No, "
                               L"then press Restart now.",
                            L"AurOS Setup", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                    != IDYES)
                return 0;
        }
        DestroyWindow(h);
        return 0;

    /* WINDOWS IS ENDING THE SESSION -- a restart from the Start menu, or
     * ours. Either way the restart is happening, and it is the one the
     * armed setting is for. */
    case WM_ENDSESSION:
        if (wp) g_restarting = 1;
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        /* Take back the one-shot start-up setting, if it was set. Not
         * when the window is going BECAUSE of the restart. */
        if (!g_restarting) install_cancel();
        PostQuitMessage(0);
        return 0;

    default: break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Dev harness — fixtures, screenshots and the navigation test.
 *
 *  Rendering only: no window is shown, no input is accepted, no phase
 *  is started, and nav_allowed() is untouched. The reports below are
 *  test fixtures and exist only here.
 * ═══════════════════════════════════════════════════════════════════ */
static void fx_add(const char *id, pf_severity sev, const char *title)
{
    if (g_report.n >= PF_MAX_RESULTS) return;
    pf_result *x = &g_report.results[g_report.n++];
    memset(x, 0, sizeof *x);
    snprintf(x->id, sizeof x->id, "%s", id);
    snprintf(x->title, sizeof x->title, "%s", title);
    snprintf(x->remedy, sizeof x->remedy, "a synthetic remedy");
    x->sev = sev;
    if (sev == PF_BLOCK) g_report.n_block++;
    else if (sev == PF_WARN) g_report.n_warn++;
}

/* A 2017 laptop: one 256 GB drive, SSD or hard disk. */
static void fx_machine(int hdd)
{
    memset(&g_report, 0, sizeof g_report);
    g_report.system_disk = 0;
    g_report.n_disks = 1;
    g_report.disks[0].index = 0;
    g_report.disks[0].is_system = 1;
    g_report.disks[0].size_bytes = 256060514304ULL;
    g_report.disks[0].seek_penalty = hdd;
    snprintf(g_report.disks[0].model, sizeof g_report.disks[0].model,
             "SAMSUNG MZ7LN256HAJQ-000H1");
    /* C:, 237 GiB, 90 GiB of it in use */
    g_report.n_volumes = 1;
    g_report.system_volume = 0;
    snprintf(g_report.volumes[0].mount, sizeof g_report.volumes[0].mount, "C:");
    snprintf(g_report.volumes[0].fs, sizeof g_report.volumes[0].fs, "NTFS");
    g_report.volumes[0].size_bytes = 237ULL * GIB;
    g_report.volumes[0].used_bytes = 90ULL * GIB;
    g_report.volumes[0].offline_shrinkable = 147ULL * GIB;
    g_report.volumes[0].shrink_measured = 1;
    fx_add("firmware-uefi", PF_PASS, "UEFI firmware");
    fx_add("space-measured", PF_INFO, "Free space measured");
    g_pf_valid = 1;
    g_checking = 0;
    g_reveal   = N_CHK;
}

static void fx_ready(void) { fx_add("ready", PF_PASS, "This PC is ready"); }

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

static void shot_size(int w, int h, int dpi)
{
    g_dpi = dpi; g_cw = w; g_ch = h;
    backbuffer(g_cw, g_ch);
    fonts_make();
}

static int shot_run(const char *dir)
{
    g_shot_mode = 1;
    /* A 1366x768 laptop, or a 1920x1080 one at 150%: the window's
     * client area is about 1120x690 on both. */
    shot_size(1120, 690, 96);
    if (!g_px) return 2;
    detect_defaults();
    ab_lock_init();

    g_page = PAGE_WELCOME;
    memset(&g_report, 0, sizeof g_report);
    g_pf_valid = 0; g_checking = 1; g_ticks = 5;
    shot_save(dir, "1a-welcome-checking");

    fx_machine(0); fx_ready();
    g_checking = 0; g_reveal = 2;
    shot_save(dir, "1b-welcome-checking-half");
    g_reveal = N_CHK;
    g_focus = -1; g_want_focus_id = 0;
    shot_save(dir, "1c-welcome-ready");

    fx_machine(0);
    fx_add("removable-attached", PF_WARN, "A USB drive is plugged in");
    fx_ready();
    shot_save(dir, "1d-welcome-good-to-know");

    fx_machine(0);
    fx_add("not-on-ac", PF_BLOCK, "This PC is running on battery");
    fx_add("insufficient-space", PF_BLOCK, "Not enough free space");
    fx_add("pending-reboot", PF_BLOCK, "Windows has updates waiting for a restart");
    g_page = PAGE_BLOCKED; shot_save(dir, "2a-blocked-fixable");

    fx_machine(0);
    fx_add("firmware-bios", PF_BLOCK, "This PC uses legacy BIOS boot");
    fx_add("not-on-ac", PF_BLOCK, "This PC is running on battery");
    g_page = PAGE_BLOCKED; shot_save(dir, "2b-blocked-bios");

    fx_machine(0);
    fx_add("arm-pc", PF_BLOCK, "This PC has an ARM processor");
    g_page = PAGE_BLOCKED; shot_save(dir, "2e-blocked-arm");

    fx_machine(0);
    fx_add("intel-rst", PF_BLOCK, "AurOS can't see this PC's drive");
    g_page = PAGE_BLOCKED; shot_save(dir, "2f-blocked-intel-nvme");

    fx_machine(1);
    fx_add("bitlocker-system", PF_BLOCK, "This drive is encrypted with BitLocker");
    g_page = PAGE_BLOCKED; shot_save(dir, "2g-blocked-encrypted-hard-disk");

    /* A Secured-core PC, which trusts nothing but Windows: the one
     * refusal that carries a picture, judged by the same sbdb_judge()
     * preflight calls. */
    memset(&g_report, 0, sizeof g_report);
    g_report.system_disk = -1;
    sbdb_judge(&g_report, 1, 0, (const unsigned char *)"", 0, -1, NULL, 0,
               "Microsoft Corporation UEFI CA 2011\n");
    g_page = PAGE_BLOCKED; shot_save(dir, "2c-blocked-secure-boot");
    g_scroll[PAGE_BLOCKED] = 10000;
    shot_save(dir, "2d-blocked-secure-boot-picture");
    g_scroll[PAGE_BLOCKED] = 0;

    fx_machine(0); fx_ready();
    g_page = PAGE_STYLE; g_sel_style = STYLE_DEFAULT;
    shot_save(dir, "3a-style");
    g_sel_style = 1;
    g_page = PAGE_STYLE; g_focus = -1; g_want_focus_id = ID_STYLE + 1; g_focus_ring = 1;
    shot_save(dir, "3b-style-mac-keyboard");
    g_focus = -1; g_focus_ring = 0; g_sel_style = STYLE_DEFAULT;

    g_page = PAGE_READY; shot_save(dir, "4a-ready");
    fx_machine(1); fx_ready(); g_sel_style = 2;
    g_page = PAGE_READY; shot_save(dir, "4b-ready-hard-disk");
    fx_machine(0); fx_ready(); g_sel_style = STYLE_DEFAULT;

    g_page = PAGE_PROGRESS;
    g_install_running = 1; g_install_finished = 0;
    g_ab_phase = AB_PREPARE; g_ab_pct = 41;
    g_eta_min = 12;
    shot_save(dir, "5a-progress");
    g_ab_phase = AB_INSPECT; g_ab_pct = 0; g_eta_min = -1; g_bar_f = 0.f;
    shot_save(dir, "5b-progress-starting");

    g_install_running = 0; g_install_finished = 1;
    InterlockedExchange(&g_ab_state, 2);
    g_cd_on = 1; g_cd_offered = 1; g_cd_left = 52;
    shot_save(dir, "5c-restart-countdown");
    g_cd_on = 0;
    shot_save(dir, "5d-restart-not-yet");

    InterlockedExchange(&g_ab_state, 3);
    snprintf(g_ab_why, sizeof g_ab_why,
             "part 7 of AurOS could not be downloaded intact. Check that the "
             "internet is working, then press Try again.");
    shot_save(dir, "5e-stopped");
    InterlockedExchange(&g_ab_state, 0);
    g_install_finished = 0;

    /* The same pages on a 1920x1080 laptop at 150%, where every shape
     * is drawn at its real size rather than scaled. */
    shot_size(1680, 1035, 144);
    g_page = PAGE_WELCOME; shot_save(dir, "hidpi-1c-welcome-ready");
    g_page = PAGE_STYLE;   shot_save(dir, "hidpi-3a-style");
    g_page = PAGE_READY;   shot_save(dir, "hidpi-4a-ready");

    /* A 1024x600 netbook, the smallest screen AurOS supports: the page
     * scrolls, the buttons stay. */
    shot_size(1024, 530, 96);
    g_page = PAGE_STYLE;   shot_save(dir, "small-3a-style");
    return 0;
}

static FILE *g_nt;
static int   g_nt_fail;

static void nt_check(int ok, const char *what)
{
    if (!ok) g_nt_fail++;
    if (g_nt) fprintf(g_nt, "  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

static void nt_fake_report(int blocked)     /* test fixture, not a real scan */
{
    fx_machine(0);
    if (blocked) fx_add("test-block", PF_BLOCK, "synthetic blocking issue");
    else fx_ready();
}

/* Every widget on the page in front of it is a button or a picture
 * card, at least 44 px on its short side at 96 dpi (docs/EASY.md,
 * rule 4). A page that asks her to type, or to tick a box, fails. */
static int nt_page_is_plain(void)
{
    render();
    for (int i = 0; i < g_nw; i++) {
        if (g_w[i].kind != W_BUTTON && g_w[i].kind != W_CARD &&
            !(g_w[i].kind == W_TOGGLE && g_w[i].id == ID_WIFI)) return 0;
        int w = g_w[i].r.right - g_w[i].r.left, h = g_w[i].r.bottom - g_w[i].r.top;
        if ((w < h ? w : h) < S(44)) return 0;
    }
    return 1;
}

static int nav_test(const char *dir)
{
    char path[MAX_PATH + 32];
    _snprintf(path, sizeof path - 1, "%s/navtest.log", dir);
    path[sizeof path - 1] = 0;
    g_nt = fopen(path, "w");

    g_nav_mode = 1;
    g_dpi = 96; g_cw = 1120; g_ch = 690;
    backbuffer(g_cw, g_ch);
    fonts_make();
    detect_defaults();
    ab_lock_init();

    if (g_nt) fprintf(g_nt, "AurBridge wizard - navigation gate test\n\n"
                            "1. preflight has not run yet\n");
    g_pf_valid = 0;
    memset(&g_report, 0, sizeof g_report);
    for (int p = (int)PAGE_STYLE; p < (int)PAGE_COUNT; p++) {
        g_page = PAGE_WELCOME;
        goto_page((page_id)p);
        nt_check(!nav_allowed((page_id)p) && g_page == PAGE_WELCOME,
                 "page refused before preflight has run");
    }
    nt_check(!primary_enabled(), "Next waits for the check");

    if (g_nt) fprintf(g_nt, "\n2. preflight blocked\n");
    nt_fake_report(1);
    for (int p = (int)PAGE_STYLE; p < (int)PAGE_COUNT; p++) {
        g_page = PAGE_BLOCKED;
        goto_page((page_id)p);
        nt_check(!nav_allowed((page_id)p) && g_page == PAGE_BLOCKED,
                 "page refused while a block stands");
    }
    g_page = PAGE_WELCOME;
    check_advance();
    nt_check(g_page == PAGE_BLOCKED, "a blocked check lands on the stop page");
    g_page = PAGE_READY;
    g_check_next = PAGE_PROGRESS;          /* as if launched from Install AurOS */
    check_advance();
    nt_check(g_page == PAGE_BLOCKED && !g_install_running,
             "a re-check before install refuses instead of starting phases");
    nt_fake_report(0);
    g_check_next = PAGE_PROGRESS;
    blocked_cleared();
    nt_check(g_page == PAGE_WELCOME && !g_install_running && g_check_next == PAGE_STYLE,
             "a stop that clears by itself goes back to the first page, never into the install");

    if (g_nt) fprintf(g_nt, "\n3. preflight clean: one press per page, nothing typed\n");
    nt_fake_report(0);
    g_page = PAGE_WELCOME; g_sel_style = STYLE_DEFAULT;
    nt_check(primary_enabled() && !wcscmp(primary_label(), L"Next"),
             "the first page offers Next once the check is clean");
    on_key(VK_RETURN);
    nt_check(g_page == PAGE_STYLE, "Enter presses Next: the look page");
    nt_check(primary_enabled() && !wcscmp(primary_label(), L"Next"),
             "a look is already chosen, so Next works straight away");
    on_key(VK_SPACE);
    nt_check(g_page == PAGE_READY, "Space presses Next too: the Ready page");
    nt_check(primary_enabled() && !wcscmp(primary_label(), L"Install AurOS"),
             "Ready's one button is Install AurOS, with nothing to tick or type first");
    nt_check(nav_allowed(PAGE_PROGRESS), "the install is reachable from Ready as it stands");
    g_page = PAGE_STYLE;                   /* she went Back while it checked */
    g_check_next = PAGE_PROGRESS;
    check_advance();
    nt_check(g_page == PAGE_STYLE && !g_install_running,
             "a clean re-check after she went Back starts nothing");
    g_page = PAGE_READY;
    g_check_next = PAGE_PROGRESS;
    check_advance();                       /* what the re-check does when clean */
    nt_check(g_page == PAGE_PROGRESS, "a clean re-check starts the install");
    g_page = PAGE_WELCOME;

    if (g_nt) fprintf(g_nt, "\n4. nothing to type, nothing to tick, nothing small\n");
    {
        static const struct { page_id p; const char *name; } P[] = {
            { PAGE_WELCOME, "welcome" }, { PAGE_STYLE, "look" },
            { PAGE_READY, "ready" }, { PAGE_PROGRESS, "progress" },
        };
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
            char what[128];
            nt_fake_report(0);
            g_page = P[i].p;
            snprintf(what, sizeof what, "the %s page has only buttons and pictures, all >= 44 px",
                     P[i].name);
            nt_check(nt_page_is_plain(), what);
            snprintf(what, sizeof what, "the %s page fits a 1366x768 laptop without scrolling",
                     P[i].name);
            nt_check(g_content_h <= g_view_h, what);
        }
        nt_fake_report(1);
        g_page = PAGE_BLOCKED;
        nt_check(nt_page_is_plain(), "the stop page has only buttons");
        g_page = PAGE_PROGRESS;
        g_install_finished = 1;
        InterlockedExchange(&g_ab_state, 2);
        g_cd_on = 1; g_cd_left = 60;
        nt_check(nt_page_is_plain(), "the restart page has only buttons");
        nt_check(g_content_h <= g_view_h, "the restart page fits without scrolling");
        nt_check(!wcscmp(primary_label(), L"Restart now"), "its big button is Restart now");
        g_cd_on = 0; g_install_finished = 0;
        InterlockedExchange(&g_ab_state, 0);
    }

    if (g_nt) fprintf(g_nt, "\n5. the look she picks reaches the installed system\n");
    {
        nt_fake_report(0);
        nt_check(!strcmp(STYLES[STYLE_DEFAULT].shell, "taskbar"),
                 "the one chosen to begin with is the one like Windows");
        /* The website promises these four, by these names. */
        static const wchar_t *const promised[] = { L"Like Windows", L"Like a Mac",
                                                   L"Big and simple", L"One thing at a time" };
        int names = N_STYLES == 4;
        for (int i = 0; names && i < 4; i++) names = !wcscmp(STYLES[i].name, promised[i]);
        nt_check(names, "the four looks carry the names the website shows");
        int ok = 1;
        for (int i = 0; i < N_STYLES; i++) {
            ab_choice c;
            memset(&c, 0, sizeof c);
            g_page = PAGE_STYLE;
            render();
            widget_activate(ID_STYLE + i);         /* a click on its picture */
            choices_from_page(&c);
            char what[160];
            snprintf(what, sizeof what, "card %d (%s, %s) travels as shell=%s theme=%s",
                     i, STYLES[i].shell, STYLES[i].theme, c.shell_archetype, c.theme);
            int one = g_sel_style == i && c.shell_archetype[0] && c.theme[0] &&
                      !strcmp(c.shell_archetype, STYLES[i].shell) &&
                      !strcmp(c.theme, STYLES[i].theme);
            nt_check(one, what);
            ok &= one;
        }
        g_sel_style = 0;
        g_page = PAGE_STYLE;
        on_key(VK_RIGHT);
        nt_check(g_sel_style == 1, "the arrow keys move the choice");
        on_key(VK_LEFT); on_key(VK_LEFT);
        nt_check(g_sel_style == 0, "and stop at the first one");
        g_sel_style = STYLE_DEFAULT;
        (void)ok;
    }
    {
        detect_defaults();
        int es = -1, kes = -1, lon = -1;
        for (int i = 0; i < g_n_langs; i++) if (!wcscmp(g_langs[i], L"Español")) es = i;
        for (int i = 0; i < g_n_kbds; i++)  if (!wcscmp(g_kbds[i], L"Spanish")) kes = i;
        for (int i = 0; i < g_n_tzs; i++)   if (!wcscmp(g_tzs[i], L"London")) lon = i;
        nt_check(es >= 0 && kes >= 0 && lon >= 0, "the language values are all known");
        if (es >= 0 && kes >= 0 && lon >= 0) {
            g_sel_lang = es; g_sel_kbd = kes; g_sel_tz = lon;
            ab_choice c;
            memset(&c, 0, sizeof c);
            choices_from_page(&c);
            nt_check(!strcmp(c.language, "es_ES.UTF-8"), "Español travels as es_ES.UTF-8");
            nt_check(!strcmp(c.keyboard, "xkb:es"), "the Spanish keyboard travels as xkb:es");
            nt_check(!strcmp(c.timezone, "iana:Europe/London"), "London travels as Europe/London");
        }
        g_sel_lang = g_sel_kbd = g_sel_tz = 0;
        ab_choice c;
        memset(&c, 0, sizeof c);
        choices_from_page(&c);
        nt_check(!strcmp(c.language, g_lang_v[0]) && !strcmp(c.timezone, g_tz_v[0]),
                 "with no question asked, what Windows uses is what travels");
    }

    if (g_nt) fprintf(g_nt, "\n6. a block appears after the user answered everything\n");
    nt_fake_report(1);
    for (int p = (int)PAGE_STYLE; p < (int)PAGE_COUNT; p++)
        nt_check(!nav_allowed((page_id)p), "every page past the first closes again");

    if (g_nt) fprintf(g_nt, "\n7. the stop page fixes what it can, in a few words\n");
    {
        int ok = 1, n = 0;
        for (const fix_info *f; (f = fix_at(n)) != NULL; n++) {
            if (!f->problem[0] || !f->fix[0] || wcslen(f->problem) > 60 || wcslen(f->fix) > 90)
                ok = 0;
            if (f->kind == FIX_AUTO && (!f->after || !f->after[0])) ok = 0;
            if (f->kind == FIX_UNKNOWN) ok = 0;
        }
        nt_check(ok && n > 10, "every known stop has one short line, and one short fix");
        static const char *warns[] = { "removable-attached", "low-ram", "smart-reallocated",
                                       "dynamic-disk-other", "storage-spaces-other",
                                       "fde-container-software", NULL };
        int wok = 1;
        for (int i = 0; warns[i]; i++) {
            const wchar_t *l = fix_note(warns[i]);
            if (!l || !l[0] || wcslen(l) > 110) wok = 0;
        }
        nt_check(wok, "every warning the check can give has one short line");
    }
    {
        static const char *ids[] = { "not-on-ac", "insufficient-space", "removable-no-serial",
                                     "test-block", "firmware-bios" };
        fx_machine(0);
        for (int i = 0; i < 5; i++) fx_add(ids[i], PF_BLOCK, "a synthetic problem");
        g_report.n = 2 + 4; g_report.n_block = 4;
        g_page = PAGE_BLOCKED;
        nt_check(fixable_now() == 1 && !wcscmp(primary_label(), L"Fix these for me"),
                 "something a program can fix: the button fixes it");
        snprintf(g_report.results[3].id, sizeof g_report.results[3].id, "pending-reboot");
        nt_check(fixable_now() == 2, "a waiting restart is something the button does");
        g_scroll[PAGE_BLOCKED] = 0;
        render();
        nt_check(g_content_h > 0 && g_content_h <= g_view_h,
                 "four problems a program or she can fix fit on the page without scrolling");
        g_report.n = 2 + 5; g_report.n_block = 5;
        nt_check(fixable_now() == 0 && !wcscmp(primary_label(), L"Close"),
                 "next to something unfixable, nothing is fixed, nothing restarts, "
                 "and the one button is Close");
        g_scroll[PAGE_BLOCKED] = 0;
        render();
        nt_check(g_content_h > 0 && g_content_h <= g_view_h,
                 "when it cannot go on, the reason and what to do instead fit without scrolling");
        g_view_h = 300; g_content_h = 900; g_scroll[PAGE_BLOCKED] = 0;
        scroll_by(10000);
        nt_check(g_scroll[PAGE_BLOCKED] == 600, "a long page scrolls to its end, and no further");
        g_scroll[PAGE_BLOCKED] = 0;
    }

    if (g_nt) fprintf(g_nt, "\n8. the restart: by itself after a minute, or now, or later\n");
    {
        countdown_start(1000);
        nt_check(countdown_left(1000, 1000) == 60 && countdown_left(1000, 31000) == 30 &&
                 countdown_left(1000, 61000) == 0 && countdown_left(1000, 999999) == 0,
                 "it counts sixty seconds, and stops at nought");
        nt_fake_report(0);
        g_page = PAGE_PROGRESS; g_install_finished = 1;
        InterlockedExchange(&g_ab_state, 2);
        render();
        int has_not_yet = 0;
        for (int i = 0; i < g_nw; i++) if (g_w[i].id == ID_NOT_YET) has_not_yet = 1;
        nt_check(has_not_yet, "while it counts there is a Not yet button");
        widget_activate(ID_NOT_YET);
        nt_check(!g_cd_on, "Not yet stops the count");
        countdown_start(1000);
        on_key(VK_ESCAPE);
        nt_check(!g_cd_on && g_page == PAGE_PROGRESS, "Esc stops the count, and nothing else");
        g_install_finished = 0;
        InterlockedExchange(&g_ab_state, 0);
    }

    if (g_nt) fprintf(g_nt, "\n9. Esc never closes anything\n");
    {
        nt_fake_report(0);
        g_page = PAGE_READY;   on_key(VK_ESCAPE);
        nt_check(g_page == PAGE_STYLE, "Esc on Ready goes back to the looks");
        on_key(VK_ESCAPE);
        nt_check(g_page == PAGE_WELCOME, "Esc on the looks goes back to the first page");
        on_key(VK_ESCAPE);
        nt_check(g_page == PAGE_WELCOME, "Esc on the first page does nothing");
        MSG msg;
        int closed = 0;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            if (msg.message == WM_CLOSE) closed = 1;
        nt_check(!closed, "no key asked the window to close");
    }

    if (g_nt) fprintf(g_nt, "\n10. how long, said before she says yes\n");
    {
        int lo, hi;
        fx_machine(0); fx_ready(); eta_range(&lo, &hi);
        char what[96];
        snprintf(what, sizeof what, "an SSD laptop: %d to %d minutes", lo, hi);
        nt_check(lo >= 5 && hi <= 30 && lo < hi, what);
        fx_machine(1); fx_ready(); eta_range(&lo, &hi);
        snprintf(what, sizeof what, "a hard disk: %d to %d minutes, longer", lo, hi);
        nt_check(lo >= 20 && hi > 30 && lo < hi, what);
        int slo, shi;
        fx_machine(0); fx_ready(); eta_range(&slo, &shi);
        InterlockedExchange(&g_dl_state, 2);
        eta_range(&lo, &hi);
        snprintf(what, sizeof what, "with the download already done: %d to %d minutes", lo, hi);
        nt_check(hi < shi && lo >= 5, what);
        InterlockedExchange(&g_dl_state, 0);
    }

    if (g_nt) fprintf(g_nt, "\n12. her Wi-Fi: offered only when there is some, and when it travels\n");
    {
        nt_fake_report(0);
        g_page = PAGE_READY; g_bring_wifi = 1;
        render();
        int box = 0;
        for (int i = 0; i < g_nw; i++) if (g_w[i].id == ID_WIFI) box = 1;
        nt_check(!box, "not on Wi-Fi: no Wi-Fi box");
        g_report.wifi_connected = 1;
        snprintf(g_report.wifi_name, sizeof g_report.wifi_name, "HomeNet");
        render();
        box = -1;
        for (int i = 0; i < g_nw; i++) if (g_w[i].id == ID_WIFI) box = i;
#ifdef AB_CHOICE_BRING_WIFI
        nt_check(box >= 0 && g_bring_wifi, "on Wi-Fi: the box is there, ticked for her");
        nt_check(box < 0 || (g_w[box].r.bottom - g_w[box].r.top) >= S(44),
                 "the whole row is a target at least 44 px tall");
        widget_activate(ID_WIFI);
        ab_choice c;
        memset(&c, 0, sizeof c);
        choices_from_page(&c);
        nt_check(!g_bring_wifi && c.bring_wifi == 0, "unticked, the engine is told not to");
        nt_check(primary_enabled() && nav_allowed(PAGE_PROGRESS),
                 "unticked, Install AurOS still works: the box never holds the way on");
        widget_activate(ID_WIFI);
        memset(&c, 0, sizeof c);
        choices_from_page(&c);
        nt_check(g_bring_wifi && c.bring_wifi == 1, "ticked again, the engine is told to");
        nt_check(g_content_h <= g_view_h, "with the box, Ready still fits without scrolling");
#else
        nt_check(box < 0, "on Wi-Fi, but the engine cannot carry it yet: no box, no promise");
#endif
        g_report.wifi_connected = 0;
        g_bring_wifi = 1;
    }

    if (g_nt) fprintf(g_nt, "\n11. a screen reader can read every page and find its button\n");
    {
        static const page_id P[] = { PAGE_WELCOME, PAGE_STYLE, PAGE_READY, PAGE_PROGRESS };
        int named = 1, buttons = 1;
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
            nt_fake_report(0);
            g_page = P[i];
            g_install_finished = (P[i] == PAGE_PROGRESS);
            InterlockedExchange(&g_ab_state, P[i] == PAGE_PROGRESS ? 2 : 0);
            render();
            VARIANT self; self.vt = VT_I4; self.lVal = CHILDID_SELF;
            BSTR s = NULL;
            if (g_acc.lpVtbl->get_accName(&g_acc, self, &s) != S_OK || !s || wcslen(s) < 10)
                named = 0;
            SysFreeString(s);
            int p = acc_primary_index();
            if (p < 0) { buttons = 0; continue; }
            VARIANT ch; ch.vt = VT_I4; ch.lVal = p + 1;
            VARIANT role; role.vt = VT_EMPTY;
            s = NULL;
            g_acc.lpVtbl->get_accName(&g_acc, ch, &s);
            g_acc.lpVtbl->get_accRole(&g_acc, ch, &role);
            if (!s || wcscmp(s, primary_label()) || role.lVal != ROLE_SYSTEM_PUSHBUTTON)
                buttons = 0;
            SysFreeString(s);
        }
        nt_check(named, "every page has a name a screen reader reads");
        nt_check(buttons, "every page's big button is a button, by its own label");
        nt_fake_report(0);
        g_page = PAGE_STYLE; g_sel_style = 1;
        render();
        int card = -1;
        for (int i = 0; i < g_nw; i++) if (g_w[i].id == ID_STYLE + 1) card = i;
        VARIANT ch; ch.vt = VT_I4; ch.lVal = card + 1;
        VARIANT st; st.vt = VT_EMPTY;
        BSTR s = NULL;
        g_acc.lpVtbl->get_accName(&g_acc, ch, &s);
        g_acc.lpVtbl->get_accState(&g_acc, ch, &st);
        nt_check(card >= 0 && s && !wcscmp(s, L"Like a Mac") && (st.lVal & STATE_SYSTEM_CHECKED),
                 "each look is read by its name, and the chosen one as chosen");
        SysFreeString(s);
        g_sel_style = STYLE_DEFAULT;
        g_install_finished = 0;
        InterlockedExchange(&g_ab_state, 0);
    }

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
     * change anything on the machine. */
    const char *shot = NULL, *navtest = NULL;
    int resume = 0;
    for (int i = 1; i < __argc; i++) {
        if (!strcmp(__argv[i], "--resume")) resume = 1;
        else if (!strcmp(__argv[i], "--shot") && i + 1 < __argc) shot = __argv[++i];
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

    /* ONE INSTALLER AT A TIME. After a restart the installer opens itself
     * (fix.h); somebody who also double-clicks it must not get two
     * installers working on one disk. The second one says so and goes. */
    HANDLE one = CreateMutexW(NULL, TRUE, L"Local\\AurOSInstaller");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS && !g_selftest) {
        MessageBoxW(NULL, L"AurOS Setup is already open.", L"AurOS Setup",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    /* The task that opened this after a restart has done its job; take it
     * away first, whether this start came from it or from a double-click. */
    if (!g_selftest) fix_resume_done();

    detect_defaults();
    /* COM on this thread, for the screen-reader object (WM_GETOBJECT). */
    CoInitialize(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"AurBridgeWizard";
    /* The installer's own icon (resource 1, build/aurbridge), and
     * Windows' plain one in a build that has none. */
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hIconSm       = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    g_dpi = dpi_for(NULL);
    RECT r = { 0, 0, S(1120), S(760) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    /* Fit the screen, taskbar excluded. Taller than the screen, the page
     * had nothing to scroll and its buttons could not be pressed. */
    RECT wa;
    if (!work_area(NULL, &wa)) SetRect(&wa, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                       GetSystemMetrics(SM_CYSCREEN));
    if (ww > wa.right - wa.left) ww = wa.right - wa.left;
    if (wh > wa.bottom - wa.top) wh = wa.bottom - wa.top;
    int sx = wa.left + ((wa.right - wa.left) - ww) / 2;
    int sy = wa.top + ((wa.bottom - wa.top) - wh) / 2;

    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Install AurOS",
                             WS_OVERLAPPEDWINDOW, sx, sy, ww, wh,
                             NULL, NULL, inst, NULL);
    if (!h) return 1;

    ShowWindow(h, g_selftest ? SW_SHOWNOACTIVATE : show);
    UpdateWindow(h);
    /* THE CHECK STARTS BY ITSELF, on the first page, while she reads.
     * Opened again after a restart it asked for, it is where it left off. */
    if (resume && !g_selftest) {
        g_resumed = 1;
        SetForegroundWindow(h);
    }
    start_check(PAGE_STYLE);

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
