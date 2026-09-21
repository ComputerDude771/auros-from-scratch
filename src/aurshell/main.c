/* ═══════════════════════════════════════════════════════════════════
 *  aurshell — the AurOS desktop shell
 *
 *  Paints directly to a DRM/KMS scanout buffer. Every colour, radius,
 *  gap and font comes from /etc/auros/shell.conf, which aurora
 *  regenerates from the active theme, so a reskin needs no rebuild --
 *  the shell re-reads the file on SIGHUP and repaints.
 *
 *  Input is read straight from evdev. There is no X11, no Wayland
 *  compositor and no Mesa anywhere in this path.
 * ═══════════════════════════════════════════════════════════════════ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <linux/input.h>

#include "draw.h"
#include "kms.h"
#include "../common/theme.h"
#include "../common/wall.h"
#include "../common/font.h"

#define MAX_KBD 8

static volatile sig_atomic_t want_reload = 0;
static volatile sig_atomic_t want_quit   = 0;
static void on_hup(int s)  { (void)s; want_reload = 1; }
static void on_term(int s) { (void)s; want_quit = 1; }

/* ── the shell's view of the theme ───────────────────────────────── */
typedef struct {
    theme_t  t;
    int      bar_h, radius, radius_sm, gap, margin, padding, border;
    int      blur_r, shadow_r, font_size, font_size_sm, font_size_lg;
    float    panel_a, shadow_a;
    uint32_t bg, bg_alt, surface_c, surface_hi, overlay, muted, subtle, fg, fg_hi;
    uint32_t accent, accent_alt, accent_warm, err;
    char     brand[64];
    char     font_sans[256], font_mono[256];
} shell_theme;

static void load_theme(shell_theme *s, const char *conf)
{
    memset(&s->t, 0, sizeof s->t);
    if (theme_load(&s->t, conf) < 0)
        fprintf(stderr, "aurshell: no %s — falling back to built-in defaults\n", conf);

    theme_t *t = &s->t;
    s->bar_h     = theme_int(t, "bar_height", 38);
    s->radius    = theme_int(t, "radius", 14);
    s->radius_sm = theme_int(t, "radius_sm", 8);
    s->gap       = theme_int(t, "gap", 12);
    s->margin    = theme_int(t, "margin", 14);
    s->padding   = theme_int(t, "padding", 14);
    s->border    = theme_int(t, "border", 2);
    s->blur_r    = theme_int(t, "blur_radius", 20);
    s->shadow_r  = theme_int(t, "shadow_radius", 28);
    s->font_size = theme_int(t, "font_size", 14);
    s->font_size_sm = theme_int(t, "font_size_sm", 12);
    s->font_size_lg = theme_int(t, "font_size_lg", 19);
    s->panel_a   = (float)theme_num(t, "opacity_panel", 0.88);
    s->shadow_a  = (float)theme_num(t, "shadow_opacity", 0.50);

    s->bg        = theme_color(t, "col_bg",         theme_color(t, "bg", 0x0B0E14));
    s->bg_alt    = theme_color(t, "col_bar_bg",     theme_color(t, "bg_alt", 0x10151F));
    s->surface_c = theme_color(t, "col_surface",    theme_color(t, "surface", 0x161C28));
    s->surface_hi= theme_color(t, "col_surface_hi", theme_color(t, "surface_hi", 0x1F2735));
    s->overlay   = theme_color(t, "col_overlay",    theme_color(t, "overlay", 0x2B3542));
    s->muted     = theme_color(t, "col_muted",      theme_color(t, "muted", 0x55606E));
    s->subtle    = theme_color(t, "col_subtle",     theme_color(t, "subtle", 0x8793A4));
    s->fg        = theme_color(t, "col_fg",         theme_color(t, "fg", 0xD4DCEA));
    s->fg_hi     = theme_color(t, "col_fg_hi",      theme_color(t, "fg_hi", 0xF3F7FD));
    s->accent    = theme_color(t, "col_accent",     theme_color(t, "accent", 0x7DD3C0));
    s->accent_alt= theme_color(t, "col_accent_alt", theme_color(t, "accent_alt", 0xA78BFA));
    s->accent_warm=theme_color(t, "col_accent_warm",theme_color(t, "accent_warm", 0xF2B880));
    s->err       = theme_color(t, "col_err",        theme_color(t, "err", 0xF2788D));

    snprintf(s->brand, sizeof s->brand, "%s", theme_str(t, "brand_text", "AurOS"));
    snprintf(s->font_sans, sizeof s->font_sans, "%s",
             theme_str(t, "font_sans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"));
    snprintf(s->font_mono, sizeof s->font_mono, "%s",
             theme_str(t, "font_mono", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"));
}

/* Theme files name a family; the shell needs a path. Try the named
 * file, then the usual places, then anything at all -- a desktop with
 * the wrong font is recoverable, a desktop with no text is not. */
static font *open_font(const char *named, float px)
{
    if (named && named[0] == '/') {
        font *f = font_load(named, px);
        if (f) return f;
    }
    static const char *fallbacks[] = {
        "/usr/share/auros/fonts/Inter.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        NULL
    };
    for (int i = 0; fallbacks[i]; i++) {
        font *f = font_load(fallbacks[i], px);
        if (f) return f;
    }
    return NULL;
}

/* ── evdev keyboards ─────────────────────────────────────────────── */
typedef struct { int fd[MAX_KBD]; int n; } kbd_set;

static int looks_like_keyboard(int fd)
{
    unsigned long evbits = 0, keybits[(KEY_MAX/(8*sizeof(long)))+1];
    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), &evbits) < 0) return 0;
    if (!(evbits & (1u << EV_KEY))) return 0;
    memset(keybits, 0, sizeof keybits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0) return 0;
    /* Require a few letter keys: mice and lid switches also report
     * EV_KEY, and grabbing those instead of the keyboard is a classic
     * "the desktop ignores my typing" bug. */
    int hits = 0;
    for (int k = KEY_Q; k <= KEY_P; k++)
        if (keybits[k / (8*sizeof(long))] & (1UL << (k % (8*sizeof(long))))) hits++;
    return hits > 5;
}

static void kbd_open_all(kbd_set *ks)
{
    ks->n = 0;
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ks->n < MAX_KBD) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char p[288];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (looks_like_keyboard(fd)) ks->fd[ks->n++] = fd;
        else close(fd);
    }
    closedir(d);
}

/* ── painting ────────────────────────────────────────────────────── */
static void paint_bar(surface *s, shell_theme *th, font *fs, font *fsm,
                      int nworkspaces, int active_ws)
{
    rect bar = { th->margin, th->margin, s->w - th->margin*2, th->bar_h };
    corners rc = corners_all((float)th->radius);

    draw_round_rect_shadow(s, bar, rc, (float)th->shadow_r * 0.7f, 0x000000,
                           th->shadow_a * 0.75f, 6);
    draw_blur_region(s, bar, th->blur_r);
    draw_round_rect(s, bar, rc, th->bg_alt, th->panel_a);
    draw_round_rect_border(s, bar, rc, 1.f, th->overlay, 0.75f);

    int cy = bar.y + th->bar_h / 2;
    draw_circle(s, (float)(bar.x + th->padding + 6), (float)cy, 6.f, th->accent, 1.f);

    int x = bar.x + th->padding + 26;
    if (fs) {
        float baseline = (float)cy + font_ascent(fs) * 0.5f - font_descent(fs) * 0.5f;
        font_draw(fs, s->px, s->w, s->h, (float)x, baseline, th->brand, th->fg_hi, 0.95f);
        x += (int)font_text_width(fs, th->brand) + 22;
    }

    /* Workspace pills: the active one is a wide capsule, so which
     * workspace you are on is legible at a glance and in peripheral
     * vision, without reading a number. */
    for (int i = 0; i < nworkspaces; i++) {
        int pw = (i == active_ws) ? 26 : 10;
        rect pill = { x, cy - 5, pw, 10 };
        draw_round_rect(s, pill, corners_all(5.f),
                        i == active_ws ? th->accent : th->muted,
                        i == active_ws ? 1.f : 0.45f);
        x += pw + 8;
    }

    /* Clock, right-aligned. */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char clock[32], date[48];
    strftime(clock, sizeof clock, "%H:%M", &tmv);
    strftime(date,  sizeof date,  "%a %d %b", &tmv);

    int rx = bar.x + bar.w - th->padding;
    if (fs) {
        float bl = (float)cy + font_ascent(fs) * 0.5f - font_descent(fs) * 0.5f;
        float cw = font_text_width(fs, clock);
        font_draw(fs, s->px, s->w, s->h, (float)rx - cw, bl, clock, th->fg_hi, 0.95f);
        rx -= (int)cw + 16;
        if (fsm) {
            float dw = font_text_width(fsm, date);
            font_draw(fsm, s->px, s->w, s->h, (float)rx - dw, bl, date, th->subtle, 0.8f);
            rx -= (int)dw + 18;
        }
    }
    for (int i = 0; i < 3; i++) {
        uint32_t c = (i == 0) ? th->accent : (i == 1) ? th->accent_warm : th->subtle;
        draw_circle(s, (float)(rx - i*20), (float)cy, 5.f, c, 0.8f);
    }
}

typedef struct { const char *name; const char *hint; } palette_item;

static void paint_palette(surface *s, shell_theme *th, font *fs, font *fsm,
                          const char *query, const palette_item *items, int n, int sel)
{
    int pw = s->w * 42 / 100; if (pw < 460) pw = 460; if (pw > 760) pw = 760;
    int rowh = 46;
    int ph = th->padding * 2 + 44 + 10 + n * rowh;
    rect pal = { (s->w - pw)/2, (s->h - ph)/2 - 40, pw, ph };
    corners rc = corners_all((float)th->radius);

    draw_round_rect_shadow(s, pal, rc, (float)th->shadow_r * 1.6f, 0x000000, th->shadow_a, 18);
    draw_blur_region(s, pal, th->blur_r + 8);
    draw_round_rect(s, pal, rc, th->surface_c, 0.94f);
    draw_round_rect_border(s, pal, rc, 1.f, th->overlay, 0.9f);

    rect inp = { pal.x + th->padding, pal.y + th->padding, pal.w - th->padding*2, 44 };
    draw_round_rect(s, inp, corners_all((float)th->radius_sm), th->bg, 0.9f);
    draw_round_rect_border(s, inp, corners_all((float)th->radius_sm), 1.f, th->accent, 0.55f);
    draw_circle(s, (float)(inp.x + 20), (float)(inp.y + 22), 7.f, th->subtle, 0.75f);

    if (fs) {
        float bl = (float)(inp.y + 22) + font_ascent(fs)*0.5f - font_descent(fs)*0.5f;
        const char *shown = (query && *query) ? query : "Type a command…";
        font_draw(fs, s->px, s->w, s->h, (float)(inp.x + 38), bl, shown,
                  (query && *query) ? th->fg_hi : th->muted, (query && *query) ? 0.95f : 0.7f);
        if (query && *query) {
            float qw = font_text_width(fs, query);
            draw_round_rect(s, (rect){ inp.x + 38 + (int)qw + 3, inp.y + 13, 2, 18 },
                            corners_all(1.f), th->accent, 0.95f);
        }
    }

    for (int i = 0; i < n; i++) {
        rect row = { pal.x + th->padding, pal.y + th->padding + 44 + 10 + i*rowh,
                     pal.w - th->padding*2, rowh - 6 };
        if (i == sel) {
            draw_round_rect(s, row, corners_all((float)th->radius_sm), th->surface_hi, 1.f);
            draw_round_rect(s, (rect){ row.x, row.y + 8, 3, row.h - 16 },
                            corners_all(1.5f), th->accent, 1.f);
        }
        draw_round_rect(s, (rect){ row.x + 14, row.y + (row.h-18)/2, 18, 18 }, corners_all(5.f),
                        i == sel ? th->accent : th->muted, i == sel ? 0.95f : 0.5f);
        if (fs) {
            float bl = (float)(row.y + row.h/2) + font_ascent(fs)*0.5f - font_descent(fs)*0.5f;
            font_draw(fs, s->px, s->w, s->h, (float)(row.x + 44), bl, items[i].name,
                      i == sel ? th->fg_hi : th->fg, i == sel ? 0.95f : 0.72f);
            if (fsm && items[i].hint) {
                float hw = font_text_width(fsm, items[i].hint);
                font_draw(fsm, s->px, s->w, s->h, (float)(row.x + row.w - 14 - hw), bl,
                          items[i].hint, th->muted, 0.6f);
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *conf = "/etc/auros/shell.conf";
    const char *card = NULL;
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--conf") && i+1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "--card") && i+1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = 1;   /* paint one frame and exit */
    }

    signal(SIGHUP,  on_hup);
    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);

    shell_theme th;
    load_theme(&th, conf);

    kms_display *disp = kms_open(card);
    if (!disp) return 1;
    fprintf(stderr, "aurshell: %dx%d on connector %u\n",
            disp->width, disp->height, disp->connector_id);

    font *fs  = open_font(th.font_sans, (float)th.font_size);
    font *fsm = open_font(th.font_sans, (float)th.font_size_sm);
    if (!fs) fprintf(stderr, "aurshell: no usable font — running without text\n");

    /* The wallpaper is expensive and static, so render it once into its
     * own surface and blit it each frame rather than regenerating. */
    surface *wall = surface_new(disp->width, disp->height);
    if (!wall) return 1;
    {
        uint32_t *tmp = malloc((size_t)disp->width * disp->height * sizeof *tmp);
        if (tmp) {
            wall_render(tmp, disp->width, disp->height, &th.t);
            for (int i = 0; i < disp->width * disp->height; i++)
                wall->px[i] = 0xFF000000u | tmp[i];
            free(tmp);
        }
    }

    kbd_set ks; kbd_open_all(&ks);
    fprintf(stderr, "aurshell: %d keyboard%s\n", ks.n, ks.n == 1 ? "" : "s");

    static const palette_item items[] = {
        { "Files",              "enter" },
        { "Web Browser",        "" },
        { "Terminal",           "ctrl+alt+t" },
        { "Settings",           "" },
        { "Change theme…",      "aurora" },
    };
    const int nitems = (int)(sizeof items / sizeof items[0]);

    int palette_open = once ? 1 : 0;
    int sel = 0, active_ws = 1;
    char query[128] = "";

    while (!want_quit) {
        if (want_reload) {
            want_reload = 0;
            load_theme(&th, conf);
            if (fs) { font_free(fs); fs = open_font(th.font_sans, (float)th.font_size); }
            if (fsm){ font_free(fsm); fsm = open_font(th.font_sans, (float)th.font_size_sm); }
            uint32_t *tmp = malloc((size_t)disp->width * disp->height * sizeof *tmp);
            if (tmp) {
                wall_render(tmp, disp->width, disp->height, &th.t);
                for (int i = 0; i < disp->width * disp->height; i++)
                    wall->px[i] = 0xFF000000u | tmp[i];
                free(tmp);
            }
            fprintf(stderr, "aurshell: theme reloaded\n");
        }

        surface *fb = kms_back_surface(disp);
        for (int y = 0; y < fb->h; y++)
            memcpy(fb->px + (size_t)y * fb->stride, wall->px + (size_t)y * wall->w,
                   (size_t)fb->w * sizeof *fb->px);

        paint_bar(fb, &th, fs, fsm, 5, active_ws);
        if (palette_open) paint_palette(fb, &th, fs, fsm, query, items, nitems, sel);

        kms_flip(disp);
        if (once) break;

        /* Idle at ~4 Hz: enough for the clock, and it leaves the CPU
         * alone on the battery-powered old laptops this targets. */
        struct pollfd pfd[MAX_KBD];
        for (int i = 0; i < ks.n; i++) { pfd[i].fd = ks.fd[i]; pfd[i].events = POLLIN; }
        int pr = poll(pfd, ks.n, 250);
        if (pr > 0) {
            for (int i = 0; i < ks.n; i++) {
                if (!(pfd[i].revents & POLLIN)) continue;
                struct input_event ev;
                while (read(ks.fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
                    if (ev.type != EV_KEY || ev.value == 0) continue;
                    switch (ev.code) {
                        case KEY_SPACE:  palette_open = !palette_open; sel = 0; query[0] = 0; break;
                        case KEY_ESC:    if (palette_open) palette_open = 0; else want_quit = 1; break;
                        case KEY_DOWN:   if (palette_open) sel = (sel + 1) % nitems; break;
                        case KEY_UP:     if (palette_open) sel = (sel + nitems - 1) % nitems; break;
                        case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5:
                            active_ws = ev.code - KEY_1; break;
                        default: break;
                    }
                }
            }
        }
    }

    for (int i = 0; i < ks.n; i++) close(ks.fd[i]);
    if (fs) font_free(fs);
    if (fsm) font_free(fsm);
    surface_free(wall);
    kms_close(disp);
    fprintf(stderr, "aurshell: exit\n");
    return 0;
}
