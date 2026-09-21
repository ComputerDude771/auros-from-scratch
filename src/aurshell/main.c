/* ═══════════════════════════════════════════════════════════════════
 *  aurshell — the AurOS desktop shell
 *
 *  Paints directly to a DRM/KMS scanout buffer. No X11, no Wayland
 *  compositor, no Mesa anywhere in this path.
 *
 *  This file owns the machinery only: the display, input, the frame
 *  loop and the theme. WHAT is drawn belongs entirely to the selected
 *  archetype (one file per layout under src/aurshell/layouts),
 *  chosen by a .shell file. That separation is the product: shipping
 *  a differently-behaving desktop is picking a different .shell, not
 *  writing code.
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
#include <sys/ioctl.h>
#include <linux/input.h>

#include "shell.h"
#include "kms.h"
#include "../common/wall.h"
#include "../common/png.h"

#define MAX_INPUT_DEV 16

static volatile sig_atomic_t want_reload = 0;
static volatile sig_atomic_t want_quit   = 0;
static void on_hup(int s)  { (void)s; want_reload = 1; }
static void on_term(int s) { (void)s; want_quit = 1; }

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

static void load_fonts(shell_fonts *f, const shell_ctx *c)
{
    const char *p = theme_str(&c->theme, "font_sans", "");
    int base = theme_int(&c->theme, "font_size", 14);
    f->huge  = open_font(p, (float)base * 2.6f);
    f->big   = open_font(p, (float)base * 1.7f);
    f->mid   = open_font(p, (float)base * 1.2f);
    f->small = open_font(p, (float)base);
}
static void free_fonts(shell_fonts *f)
{
    if (f->huge) font_free(f->huge);
    if (f->big) font_free(f->big);
    if (f->mid) font_free(f->mid);
    if (f->small) font_free(f->small);
    memset(f, 0, sizeof *f);
}

/* ── input ───────────────────────────────────────────────────────────
 * Keyboards and pointers are told apart by the events they advertise.
 * Mice and lid switches also report EV_KEY, so a keyboard must show a
 * spread of letter keys; a pointer must show relative or absolute axes.
 * Grabbing the wrong device is how a desktop ends up ignoring the mouse
 * -- which is exactly the state this shell was in until now. */
typedef struct { int fd[MAX_INPUT_DEV]; int kind[MAX_INPUT_DEV]; int n; } input_set;
enum { DEV_KBD = 1, DEV_REL = 2, DEV_ABS = 3 };

static int has_bit(const unsigned long *b, int bit)
{ return (b[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1; }

static int classify(int fd)
{
    unsigned long ev = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), &ev) < 0) return 0;

    if (ev & (1u << EV_REL)) {
        unsigned long rel[(REL_MAX / (8 * sizeof(long))) + 1];
        memset(rel, 0, sizeof rel);
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel) >= 0 &&
            has_bit(rel, REL_X) && has_bit(rel, REL_Y)) return DEV_REL;
    }
    if (ev & (1u << EV_ABS)) {
        unsigned long abs_[(ABS_MAX / (8 * sizeof(long))) + 1];
        memset(abs_, 0, sizeof abs_);
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_), abs_) >= 0 &&
            has_bit(abs_, ABS_X) && has_bit(abs_, ABS_Y)) return DEV_ABS;
    }
    if (ev & (1u << EV_KEY)) {
        unsigned long key[(KEY_MAX / (8 * sizeof(long))) + 1];
        memset(key, 0, sizeof key);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0) return 0;
        int hits = 0;
        for (int k = KEY_Q; k <= KEY_P; k++) if (has_bit(key, k)) hits++;
        if (hits > 5) return DEV_KBD;
    }
    return 0;
}

static void input_open_all(input_set *s)
{
    s->n = 0;
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s->n < MAX_INPUT_DEV) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char p[288];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        int k = classify(fd);
        if (k) { s->fd[s->n] = fd; s->kind[s->n] = k; s->n++; }
        else close(fd);
    }
    closedir(d);
}

/* Absolute devices report in their own units; scale to the screen. */
static void abs_range(int fd, int axis, int *lo, int *hi)
{
    struct input_absinfo ai;
    *lo = 0; *hi = 0;
    if (ioctl(fd, EVIOCGABS(axis), &ai) == 0 && ai.maximum > ai.minimum)
        { *lo = ai.minimum; *hi = ai.maximum; }
}

/* ── software cursor ─────────────────────────────────────────────────
 * Drawn by us because there is no compositor to do it. The dark outline
 * is not decoration: without it the pointer vanishes over a pale
 * wallpaper on a light theme, and a pointer you cannot find is the
 * fastest way to make someone believe the machine has frozen. */
static void paint_cursor(surface *s, int x, int y, uint32_t fill, uint32_t edge)
{
    if (x < 0 || y < 0) return;
    static const char *glyph[] = {
        "X.........",
        "XX........",
        "X#X.......",
        "X##X......",
        "X###X.....",
        "X####X....",
        "X#####X...",
        "X######X..",
        "X#######X.",
        "X####XXXXX",
        "X##X#X....",
        "X#X.X#X...",
        "XX..X#X...",
        "X....X#X..",
        ".....XXX..",
    };
    for (int r = 0; r < 15; r++)
        for (int c = 0; glyph[r][c]; c++) {
            char g = glyph[r][c];
            if (g == '.') continue;
            draw_blend_px(s, x + c, y + r, g == '#' ? fill : edge, 1.f);
        }
}

/* ── a starter set of things the machine can do ──────────────────── */
static void seed_apps(shell_ctx *c)
{
    const struct { const char *id, *name, *hint; shell_icon ic; uint32_t t; int pin; } A[] = {
      { "web",   "Internet",   "Browse the web",        ICON_GLOBE,    0x7DD3C0, 1 },
      { "mail",  "Email",      "Read your messages",    ICON_MAIL,     0x82AAFF, 1 },
      { "photo", "Photos",     "Pictures and videos",   ICON_PHOTOS,   0xA78BFA, 1 },
      { "files", "My Files",   "Documents you saved",   ICON_FILES,    0xF2B880, 1 },
      { "write", "Writing",    "Letters and notes",     ICON_TEXT,     0x6FD8DC, 0 },
      { "music", "Music",      "Songs and radio",       ICON_MUSIC,    0xF2788D, 0 },
      { "calc",  "Calculator", "Do sums",               ICON_CALC,     0x9BE8D8, 0 },
      { "set",   "Settings",   "Change how this works", ICON_SETTINGS, 0x8793A4, 1 },
      { "help",  "Help",       "Show me how",           ICON_HELP,     0x6FD8DC, 0 },
    };
    c->n_apps = (int)(sizeof A / sizeof A[0]);
    for (int i = 0; i < c->n_apps; i++) {
        snprintf(c->apps[i].id,   sizeof c->apps[i].id,   "%s", A[i].id);
        snprintf(c->apps[i].name, sizeof c->apps[i].name, "%s", A[i].name);
        snprintf(c->apps[i].hint, sizeof c->apps[i].hint, "%s", A[i].hint);
        c->apps[i].icon = A[i].ic;
        c->apps[i].tint = A[i].t;
        c->apps[i].pinned = A[i].pin;
    }
}

static void build_wallpaper(surface **wall, int w, int h, const theme_t *t)
{
    if (*wall) surface_free(*wall);
    *wall = surface_new(w, h);
    if (!*wall) return;
    uint32_t *tmp = malloc((size_t)w * h * sizeof *tmp);
    if (!tmp) return;
    wall_render(tmp, w, h, t);
    for (int i = 0; i < w * h; i++) (*wall)->px[i] = 0xFF000000u | tmp[i];
    free(tmp);
}

static void load_policy(shell_ctx *c, const char *path)
{
    theme_t p = {0};
    c->allow_install = c->allow_settings = c->allow_theme_change = 1;
    c->kiosk = 0;
    if (theme_load(&p, path) < 0) return;
    c->allow_install      = strcmp(theme_str(&p, "allow_user_install",    "yes"), "no") != 0;
    c->allow_settings     = strcmp(theme_str(&p, "allow_settings_change", "yes"), "no") != 0;
    c->allow_theme_change = strcmp(theme_str(&p, "allow_theme_change",    "yes"), "no") != 0;
    c->kiosk              = strcmp(theme_str(&p, "kiosk_mode",            "no"),  "yes") == 0;
}

int main(int argc, char **argv)
{
    const char *conf   = "/etc/auros/shell.conf";
    const char *shellf = "/etc/auros/shell/active.shell";
    const char *policy = "/etc/auros/policy.conf";
    const char *card = NULL, *png_out = NULL;
    int png_w = 1600, png_h = 900, nopen = 0, once = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--conf")   && i+1 < argc) conf   = argv[++i];
        else if (!strcmp(argv[i], "--shell")  && i+1 < argc) shellf = argv[++i];
        else if (!strcmp(argv[i], "--policy") && i+1 < argc) policy = argv[++i];
        else if (!strcmp(argv[i], "--card")   && i+1 < argc) card   = argv[++i];
        else if (!strcmp(argv[i], "--png")    && i+1 < argc) png_out = argv[++i];
        else if (!strcmp(argv[i], "--size")   && i+2 < argc) { png_w = atoi(argv[++i]); png_h = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--open")   && i+1 < argc) nopen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--help")) {
            fputs("aurshell [--shell FILE] [--conf FILE] [--policy FILE]\n"
                  "         [--card /dev/dri/cardN] [--png OUT --size W H] [--once]\n", stderr);
            return 0;
        }
    }

    signal(SIGHUP,  on_hup);
    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);

    theme_t t = {0};
    if (theme_load(&t, conf) < 0)
        fprintf(stderr, "aurshell: no %s — using built-in defaults\n", conf);

    shell_ctx c;
    memset(&c, 0, sizeof c);
    shell_theme_load(&c, &t);
    if (shell_archetype_load(&c, shellf) < 0) {
        /* Rail is the safe default: it is the only archetype in which a
         * thing cannot be hidden, so an unreadable config degrades to
         * the shell that is hardest to get lost in. */
        fprintf(stderr, "aurshell: no %s — defaulting to the rail archetype\n", shellf);
        snprintf(c.layout_id, sizeof c.layout_id, "rail");
        c.show_clock = 1;
    }
    load_policy(&c, policy);
    seed_apps(&c);
    c.mouse_x = c.mouse_y = -1;
    c.hover = -1;
    c.focus = -1;

    for (int i = 0; i < nopen && i < SHELL_MAX_WINS; i++) {
        c.wins[i].app = i % c.n_apps;
        snprintf(c.wins[i].title, sizeof c.wins[i].title, "%s", c.apps[c.wins[i].app].name);
        c.n_wins++;
    }
    if (c.n_wins) c.focus = 0;

    const shell_layout *L = shell_layout_by_id(c.layout_id);
    fprintf(stderr, "aurshell: archetype '%s' (%s)\n", L->id, c.shell_name);

    shell_fonts f;
    load_fonts(&f, &c);
    if (!f.small) fprintf(stderr, "aurshell: no usable font — running without text\n");

    /* ── headless: one frame to a PNG, through the identical path ── */
    if (png_out) {
        surface *s = surface_new(png_w, png_h), *wall = NULL;
        build_wallpaper(&wall, png_w, png_h, &t);
        if (L->init) L->init(&c);
        L->paint(&c, s, &f, wall);
        uint32_t *o = malloc((size_t)png_w * png_h * sizeof *o);
        for (int i = 0; i < png_w * png_h; i++) o[i] = s->px[i] & 0xFFFFFFu;
        int rc = png_write_rgb(png_out, o, png_w, png_h);
        free(o); surface_free(s); surface_free(wall);
        if (L->fini) L->fini(&c);
        free_fonts(&f);
        fprintf(stderr, "aurshell: wrote %s (%dx%d, %s)\n", png_out, png_w, png_h, L->id);
        return rc;
    }

    kms_display *disp = kms_open(card);
    if (!disp) return 1;
    fprintf(stderr, "aurshell: %dx%d on connector %u\n",
            disp->width, disp->height, disp->connector_id);

    surface *wall = NULL;
    build_wallpaper(&wall, disp->width, disp->height, &t);
    if (L->init) L->init(&c);

    input_set in;
    input_open_all(&in);
    int n_kbd = 0, n_ptr = 0;
    for (int i = 0; i < in.n; i++) (in.kind[i] == DEV_KBD) ? n_kbd++ : n_ptr++;
    fprintf(stderr, "aurshell: %d keyboard%s, %d pointer%s\n",
            n_kbd, n_kbd == 1 ? "" : "s", n_ptr, n_ptr == 1 ? "" : "s");

    /* Start the pointer centred so it is findable on the first frame. */
    c.mouse_x = disp->width / 2;
    c.mouse_y = disp->height / 2;

    uint32_t cur_fill = theme_color(&t, "col_fg_hi", 0xF3F7FD);
    uint32_t cur_edge = theme_color(&t, "col_bg",    0x0B0E14);

    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);

    while (!want_quit) {
        if (want_reload) {
            want_reload = 0;
            theme_t nt = {0};
            if (theme_load(&nt, conf) == 0) {
                t = nt;
                shell_theme_load(&c, &t);
                free_fonts(&f);
                load_fonts(&f, &c);
                build_wallpaper(&wall, disp->width, disp->height, &t);
                cur_fill = theme_color(&t, "col_fg_hi", 0xF3F7FD);
                cur_edge = theme_color(&t, "col_bg",    0x0B0E14);
                fprintf(stderr, "aurshell: theme reloaded\n");
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - last.tv_sec)
                 + (float)(now.tv_nsec - last.tv_nsec) / 1e9f;
        last = now;
        if (dt > 0.25f) dt = 0.25f;          /* a stall must not teleport */

        int animating = L->step ? L->step(&c, dt) : 0;

        surface *fb = kms_back_surface(disp);
        L->paint(&c, fb, &f, wall);
        paint_cursor(fb, c.mouse_x, c.mouse_y, cur_fill, cur_edge);
        kms_flip(disp);
        if (once) break;

        /* Animating: poll briefly so the next frame is soon. Idle: wait
         * up to a quarter second, which is enough for a clock and
         * leaves the CPU alone on a fanless machine. */
        struct pollfd pfd[MAX_INPUT_DEV];
        for (int i = 0; i < in.n; i++) { pfd[i].fd = in.fd[i]; pfd[i].events = POLLIN; }
        if (poll(pfd, in.n, animating ? 8 : 250) <= 0) continue;

        for (int i = 0; i < in.n; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            struct input_event ev;
            while (read(in.fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) c.mouse_x += ev.value;
                    if (ev.code == REL_Y) c.mouse_y += ev.value;
                } else if (ev.type == EV_ABS) {
                    int lo, hi;
                    if (ev.code == ABS_X) {
                        abs_range(in.fd[i], ABS_X, &lo, &hi);
                        if (hi > lo) c.mouse_x = (int)((int64_t)(ev.value - lo) * disp->width  / (hi - lo));
                    } else if (ev.code == ABS_Y) {
                        abs_range(in.fd[i], ABS_Y, &lo, &hi);
                        if (hi > lo) c.mouse_y = (int)((int64_t)(ev.value - lo) * disp->height / (hi - lo));
                    }
                } else if (ev.type == EV_KEY) {
                    if (ev.code == BTN_LEFT || ev.code == BTN_TOUCH) {
                        c.mouse_down = (ev.value != 0);
                        /* Act on release, not press: it is the gesture
                         * people can abort by sliding off the target. */
                        if (!ev.value && L->click) L->click(&c, c.mouse_x, c.mouse_y);
                    } else if (ev.value && in.kind[i] == DEV_KBD) {
                        if (ev.code == KEY_ESC && c.kiosk) continue;  /* no escape hatch */
                        if (L->key) L->key(&c, ev.code);
                    }
                }
            }
        }

        if (c.mouse_x < 0) c.mouse_x = 0;
        if (c.mouse_y < 0) c.mouse_y = 0;
        if (c.mouse_x >= disp->width)  c.mouse_x = disp->width - 1;
        if (c.mouse_y >= disp->height) c.mouse_y = disp->height - 1;
        if (L->motion) L->motion(&c, c.mouse_x, c.mouse_y);
    }

    for (int i = 0; i < in.n; i++) close(in.fd[i]);
    if (L->fini) L->fini(&c);
    free_fonts(&f);
    surface_free(wall);
    kms_close(disp);
    fprintf(stderr, "aurshell: exit\n");
    return 0;
}
