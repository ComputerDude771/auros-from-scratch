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
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/inotify.h>

#include "shell.h"
#include "session.h"
#include "../aurwl/aurwl.h"
#include "pad.h"
#include "kms.h"
#include "../common/wall.h"
#include "../common/png.h"

#define MAX_INPUT_DEV 16

static volatile sig_atomic_t want_reload  = 0;
static volatile sig_atomic_t want_quit    = 0;
static volatile sig_atomic_t want_release = 0;
static volatile sig_atomic_t want_acquire = 0;
static void on_hup(int s)  { (void)s; want_reload = 1; }
static void on_term(int s) { (void)s; want_quit = 1; }
static void on_rel(int s)  { (void)s; want_release = 1; }
static void on_acq(int s)  { (void)s; want_acquire = 1; }

/* ── the console ─────────────────────────────────────────────────────
 *
 * A shell that paints to KMS is still sitting on a text console that
 * the kernel is driving, and that console keeps its own claims on the
 * keyboard and the VT. Left alone:
 *
 *   - every keystroke is echoed into the tty underneath our pixels,
 *     so a stray keypress scrolls a login prompt through the desktop;
 *   - Ctrl-Alt-Del reboots the machine;
 *   - Ctrl-Alt-F2 switches VT, which revokes DRM master. Every present
 *     after that silently fails and the display never comes back. On a
 *     school machine that is a three-key denial of service.
 *
 * So: put the VT in graphics mode, take the keyboard off the console's
 * translation layer, and ask the kernel to ASK US before switching away
 * (VT_PROCESS) rather than doing it behind our back. The signals are
 * the kernel's half of that conversation.
 *
 * Every one of these is restored on exit. A shell that leaves a console
 * in K_OFF is a machine with no keyboard. */
typedef struct {
    int fd;                    /* /dev/tty0, or -1 if we have no console */
    int saved_kbmode;
    int have_kbmode;
    int saved_kdmode;
    int have_kdmode;
    struct vt_mode saved_vtmode;
    int have_vtmode;
    int active;                /* 0 while another VT has the display     */
    int refuse_switch;         /* managed machine: no VT switching       */
} console;

static console g_con = { -1, 0, 0, 0, 0, {0,0,0,0,0}, 0, 1, 0 };

#define VT_RELSIG  SIGUSR1
#define VT_ACQSIG  SIGUSR2

static void console_take(console *k, int refuse_switch)
{
    k->active = 1;
    k->refuse_switch = refuse_switch;
    k->fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (k->fd < 0) {
        fprintf(stderr, "aurshell: no console (%s) — the tty keeps the "
                        "keyboard and VT switching stays live\n", strerror(errno));
        return;
    }

    if (ioctl(k->fd, KDGKBMODE, &k->saved_kbmode) == 0) k->have_kbmode = 1;
    /* K_OFF, not K_RAW: K_OFF stops the console translating keys at all
     * without us having to feed it. We read evdev directly, so the
     * console has no business seeing the keyboard. It also disables
     * Ctrl-Alt-Del and the VT-switch chords, which is the point. */
    if (ioctl(k->fd, KDSKBMODE, K_OFF) < 0) {
        k->have_kbmode = 0;
        fprintf(stderr, "aurshell: could not silence the console keyboard (%s)\n",
                strerror(errno));
    }

    if (ioctl(k->fd, KDGETMODE, &k->saved_kdmode) == 0) k->have_kdmode = 1;
    if (ioctl(k->fd, KDSETMODE, KD_GRAPHICS) < 0) k->have_kdmode = 0;

    struct vt_mode vm;
    if (ioctl(k->fd, VT_GETMODE, &vm) == 0) {
        k->saved_vtmode = vm; k->have_vtmode = 1;
        vm.mode   = VT_PROCESS;
        vm.waitv  = 0;
        vm.relsig = VT_RELSIG;
        vm.acqsig = VT_ACQSIG;
        if (ioctl(k->fd, VT_SETMODE, &vm) < 0) {
            k->have_vtmode = 0;
            fprintf(stderr, "aurshell: VT_SETMODE failed (%s) — a VT switch "
                            "will take the display away without warning\n",
                    strerror(errno));
        }
    }
}

/* The kernel is asking whether it may hand the display to another VT.
 * This is the conversation VT_PROCESS bought us, and it is the whole
 * reason a VT switch no longer leaves a dead screen behind:
 *
 *   VT_RELDISP 1  -- yes, take it. We drop DRM master first, stop
 *                    painting, and wait to be told we have it back.
 *   VT_RELDISP 0  -- no. The kernel abandons the switch.
 *
 * A managed machine refuses. That is what `allow_tty = no` has to mean
 * if it means anything: masking getty units is not enough, because the
 * switch itself is handled by the kernel's VT layer and nothing in
 * userspace has to cooperate for the display to be lost. Anywhere else
 * we say yes -- reaching a console is a feature on a personal machine,
 * and refusing would be us deciding what the owner may do with their
 * own computer. */
static void console_release(console *k, int drm_fd)
{
    if (k->fd < 0) return;
    if (k->refuse_switch) {
        ioctl(k->fd, VT_RELDISP, 0);
        return;
    }
    kms_drop_master(drm_fd);
    k->active = 0;
    ioctl(k->fd, VT_RELDISP, 1);
}

static void console_acquire(console *k, int drm_fd)
{
    if (k->fd < 0) return;
    ioctl(k->fd, VT_RELDISP, VT_ACKACQ);
    kms_set_master(drm_fd);
    k->active = 1;
}

static void console_give_back(console *k)
{
    if (k->fd < 0) return;
    if (k->have_vtmode) ioctl(k->fd, VT_SETMODE, &k->saved_vtmode);
    if (k->have_kdmode) ioctl(k->fd, KDSETMODE, k->saved_kdmode);
    if (k->have_kbmode) ioctl(k->fd, KDSKBMODE, k->saved_kbmode);
    close(k->fd);
    k->fd = -1;
}

/* Keep the pointer on the screen at the moment it moves, not at the end
 * of the batch. A relative device can accumulate well past the edge
 * inside one drain, and a click dispatched at that coordinate misses
 * every target while the cursor is drawn at the edge -- which is
 * precisely the dock and taskbar case, where users shove the mouse into
 * the edge on purpose. */
static void clamp_pointer(shell_ctx *c, int w, int h)
{
    if (c->mouse_x < 0) c->mouse_x = 0;
    if (c->mouse_y < 0) c->mouse_y = 0;
    if (c->mouse_x >= w) c->mouse_x = w - 1;
    if (c->mouse_y >= h) c->mouse_y = h - 1;
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
 *
 * Reading evdev directly means classifying devices ourselves, and the
 * classification is where naive shells go wrong. Three rules, each
 * learned from a device that breaks the obvious version:
 *
 * 1. A device is not ONE thing. A wireless keyboard with a built-in
 *    trackpad is a single event node that is both a keyboard and a
 *    pointer. `kind` is therefore a bitmask, not an enum -- as an enum,
 *    such a device is classified as a pointer and every keystroke is
 *    silently dropped.
 *
 * 2. A touchpad is NOT an absolute device, even though it reports
 *    ABS_X/ABS_Y. Every Synaptics and Elan pad does. Treated as
 *    absolute, the pointer teleports to wherever on the pad the finger
 *    lands. INPUT_PROP_POINTER vs INPUT_PROP_DIRECT is the real
 *    discriminator: DIRECT means the user touches the thing they are
 *    pointing at (a touchscreen), POINTER means they do not (a pad).
 *
 * 3. BTN_TOUCH is only a click on a DIRECT device. On a touchpad it
 *    fires on every finger-down and finger-up, so honouring it there
 *    means the user cannot move the pointer without clicking.
 */
typedef struct {
    int fd[MAX_INPUT_DEV];
    int kind[MAX_INPUT_DEV];
    /* Axis ranges read once at open. EVIOCGABS per motion event is an
     * ioctl per sample on a device that can report at 250 Hz. */
    int ax_lo[MAX_INPUT_DEV], ax_hi[MAX_INPUT_DEV];
    int ay_lo[MAX_INPUT_DEV], ay_hi[MAX_INPUT_DEV];
    pad_state pad[MAX_INPUT_DEV];
    char name[MAX_INPUT_DEV][32];
    int n;
    int notify_fd;             /* inotify on /dev/input, for hotplug   */
} input_set;

static int has_bit(const unsigned long *b, int bit)
{ return (b[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1; }

/* Read what the kernel knows about a device and hand the decision to
 * pad_classify(), which is pure and therefore tested. */
static int classify(int fd)
{
    unsigned long ev = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), &ev) < 0) return 0;

    dev_caps d;
    memset(&d, 0, sizeof d);

    unsigned long key[(KEY_MAX / (8 * sizeof(long))) + 1];
    memset(key, 0, sizeof key);
    int have_key = (ev & (1u << EV_KEY)) &&
                   ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) >= 0;
    if (have_key) {
        d.has_btn_finger = has_bit(key, BTN_TOOL_FINGER);
        for (int k = KEY_Q; k <= KEY_P; k++) if (has_bit(key, k)) d.letter_keys++;
    }

    unsigned long prop[(INPUT_PROP_MAX / (8 * sizeof(long))) + 1];
    memset(prop, 0, sizeof prop);
    ioctl(fd, EVIOCGPROP(sizeof prop), prop);   /* absent on old kernels */
    d.prop_direct = has_bit(prop, INPUT_PROP_DIRECT);

    if (ev & (1u << EV_REL)) {
        unsigned long rel[(REL_MAX / (8 * sizeof(long))) + 1];
        memset(rel, 0, sizeof rel);
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel) >= 0)
            d.has_rel_xy = has_bit(rel, REL_X) && has_bit(rel, REL_Y);
    }
    if (ev & (1u << EV_ABS)) {
        unsigned long abs_[(ABS_MAX / (8 * sizeof(long))) + 1];
        memset(abs_, 0, sizeof abs_);
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_), abs_) >= 0)
            d.has_abs_xy = has_bit(abs_, ABS_X) ||
                           has_bit(abs_, ABS_MT_POSITION_X);
    }
    return pad_classify(&d);
}

/* Absolute devices report in their own units; scale to the screen. */
static void abs_range(int fd, int axis, int *lo, int *hi)
{
    struct input_absinfo ai;
    *lo = 0; *hi = 0;
    if (ioctl(fd, EVIOCGABS(axis), &ai) == 0 && ai.maximum > ai.minimum)
        { *lo = ai.minimum; *hi = ai.maximum; }
}

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int pad_event_kind(int code)
{
    switch (code) {
    case BTN_TOUCH:            return PAD_TOUCH;
    case BTN_TOOL_FINGER:      return PAD_FINGER;
    case BTN_TOOL_DOUBLETAP:   return PAD_DOUBLETAP;
    case BTN_TOOL_TRIPLETAP:   return PAD_TRIPLETAP;
    case BTN_TOOL_QUADTAP:     return PAD_QUADTAP;
    default:                   return -1;
    }
}

static int input_have(const input_set *s, const char *node)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(s->name[i], node)) return 1;
    return 0;
}

/* Opens any /dev/input/eventN we do not already hold. Safe to call
 * repeatedly; that is how hotplug works. Returns how many were added. */
static int input_scan(input_set *s, int grab)
{
    int added = 0;
    DIR *d = opendir("/dev/input");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && s->n < MAX_INPUT_DEV) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        if (input_have(s, e->d_name)) continue;
        char p[288];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        int k = classify(fd);
        if (!k) { close(fd); continue; }
        int i = s->n;
        s->fd[i] = fd; s->kind[i] = k;
        s->ax_lo[i] = s->ax_hi[i] = s->ay_lo[i] = s->ay_hi[i] = 0;
        if (k & (DEV_ABS | DEV_PAD)) {
            abs_range(fd, ABS_X, &s->ax_lo[i], &s->ax_hi[i]);
            abs_range(fd, ABS_Y, &s->ay_lo[i], &s->ay_hi[i]);
        }
        pad_reset(&s->pad[i], s->ax_lo[i], s->ax_hi[i],
                              s->ay_lo[i], s->ay_hi[i]);
        snprintf(s->name[i], sizeof s->name[i], "%.31s", e->d_name);
        /* Kiosk: take the device away from everyone else, so a key
         * combination cannot reach another reader. This is what makes
         * the lockdown a property of the system rather than of us
         * choosing not to act on a keystroke. */
        if (grab) ioctl(fd, EVIOCGRAB, 1);
        s->n++; added++;
    }
    closedir(d);
    return added;
}

static void input_drop(input_set *s, int i)
{
    close(s->fd[i]);
    s->n--;
    if (i != s->n) {
        s->fd[i]    = s->fd[s->n];    s->kind[i]  = s->kind[s->n];
        s->ax_lo[i] = s->ax_lo[s->n]; s->ax_hi[i] = s->ax_hi[s->n];
        s->ay_lo[i] = s->ay_lo[s->n]; s->ay_hi[i] = s->ay_hi[s->n];
        s->pad[i] = s->pad[s->n];
        memcpy(s->name[i], s->name[s->n], sizeof s->name[i]);
    }
}

static void input_open_all(input_set *s, int grab)
{
    memset(s, 0, sizeof *s);
    s->notify_fd = -1;
#ifdef IN_NONBLOCK
    /* Hotplug. Without this, a mouse plugged in after the desktop
     * appears does nothing until a reboot -- and if we win the race
     * against udev at boot, we can come up with no input at all. */
    s->notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s->notify_fd >= 0 &&
        inotify_add_watch(s->notify_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(s->notify_fd); s->notify_fd = -1;
    }
#endif
    input_scan(s, grab);
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

/* Policy fails CLOSED, and says so.
 *
 * The obvious version returns silently on a missing or empty file and
 * leaves the permissive defaults in place. That turns a truncated write
 * on a failing disk, or one typo in a deployment script, into a school
 * laptop that quietly boots as a fully open desktop -- with nothing in
 * the log to say why. A restriction that evaporates when its config is
 * unreadable is not a restriction.
 *
 * `present` is what distinguishes "no policy was ever installed" (a
 * personal machine: open, as intended) from "a policy file exists but
 * we could not read or parse it" (something is wrong: lock down). */
static void load_policy(shell_ctx *c, const char *path)
{
    theme_t p = {0};
    c->allow_install = c->allow_settings = c->allow_theme_change = 1;
    c->allow_tty = 1;
    c->kiosk = 0;
    c->allowed_apps[0] = 0;
    c->deny_all_apps = 0;

    struct stat st;
    int present = (stat(path, &st) == 0);

    if (theme_load(&p, path) < 0) {
        if (present) {
            fprintf(stderr, "aurshell: %s exists but could not be read — "
                            "locking down\n", path);
            c->allow_install = c->allow_settings = c->allow_theme_change = 0;
            c->allow_tty = 0;
            c->kiosk = 1;
            /* Failing closed has to include the applications, or a
             * machine whose policy file is corrupt keeps its lockdown
             * flags and loses the only thing that limited what can be
             * launched on it. */
            c->deny_all_apps = 1;
        } else {
            fprintf(stderr, "aurshell: no %s — unmanaged machine, "
                            "no restrictions\n", path);
        }
        return;
    }
    if (p.n == 0) {
        fprintf(stderr, "aurshell: %s is empty or unparseable — locking down\n",
                path);
        c->allow_install = c->allow_settings = c->allow_theme_change = 0;
        c->allow_tty = 0;
        c->kiosk = 1;
        c->deny_all_apps = 1;
        return;
    }
    c->allow_install      = strcmp(theme_str(&p, "allow_user_install",    "yes"), "no") != 0;
    c->allow_settings     = strcmp(theme_str(&p, "allow_settings_change", "yes"), "no") != 0;
    c->allow_theme_change = strcmp(theme_str(&p, "allow_theme_change",    "yes"), "no") != 0;
    c->allow_tty          = strcmp(theme_str(&p, "allow_tty",             "yes"), "no") != 0;
    c->kiosk              = strcmp(theme_str(&p, "kiosk_mode",            "no"),  "yes") == 0;
    snprintf(c->allowed_apps, sizeof c->allowed_apps, "%s",
             theme_str(&p, "allowed_apps", ""));
}

int main(int argc, char **argv)
{
    const char *conf   = "/etc/auros/shell.conf";
    const char *shellf = "/etc/auros/shell/active.shell";
    const char *policy = "/etc/auros/policy.conf";
    const char *card = NULL, *png_out = NULL;
    int png_w = 1600, png_h = 900, nopen = 0, once = 0, frames = 1;
    int mouse_x0 = -1, mouse_y0 = -1, input_test = 0;
    const char *with_app = NULL; int app_wait = 12;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--conf")   && i+1 < argc) conf   = argv[++i];
        else if (!strcmp(argv[i], "--shell")  && i+1 < argc) shellf = argv[++i];
        else if (!strcmp(argv[i], "--policy") && i+1 < argc) policy = argv[++i];
        else if (!strcmp(argv[i], "--card")   && i+1 < argc) card   = argv[++i];
        else if (!strcmp(argv[i], "--png")    && i+1 < argc) png_out = argv[++i];
        else if (!strcmp(argv[i], "--size")   && i+2 < argc) { png_w = atoi(argv[++i]); png_h = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--open")   && i+1 < argc) nopen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mouse")  && i+2 < argc) { mouse_x0 = atoi(argv[++i]); mouse_y0 = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--frames") && i+1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--with-app") && i+1 < argc) with_app = argv[++i];
        else if (!strcmp(argv[i], "--app-wait") && i+1 < argc) app_wait = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--input-test")) input_test = 1;
        else if (!strcmp(argv[i], "--help")) {
            fputs("aurshell [--shell FILE] [--conf FILE] [--policy FILE]\n"
                  "         [--card /dev/dri/cardN] [--png OUT --size W H]\n"
                  "         [--frames N] [--open N] [--once] [--input-test]\n"
                  "         [--with-app CMD [--app-wait SECONDS]]\n"
                  "  --with-app  start the compositor, launch CMD, wait for its\n"
                  "              window and render the desktop with the real\n"
                  "              application in it. This is how the shell is\n"
                  "              checked against actual software without a\n"
                  "              screen: the pixels in the window came from\n"
                  "              another process or the test is worthless.\n"
                  "  --input-test  print every input device this machine has,\n"
                  "              how the shell classifies it, and every event\n"
                  "              it produces, with the pointer position each\n"
                  "              one results in. Run this first when someone\n"
                  "              says the mouse does not work.\n"
                  "  --frames N  with --png: paint N times and report the median\n"
                  "              paint in ms, excluding startup and wallpaper.\n"
                  "              This is the number that matters on old hardware.\n", stderr);
            return 0;
        }
    }

    signal(SIGHUP,  on_hup);
    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);
    signal(VT_RELSIG, on_rel);
    signal(VT_ACQSIG, on_acq);

    theme_t t = {0};
    if (theme_load(&t, conf) < 0)
        fprintf(stderr, "aurshell: no %s — using built-in defaults\n", conf);

    shell_ctx c;
    memset(&c, 0, sizeof c);
    shell_theme_load(&c, &t);

    /* Policy BEFORE the archetype, because it decides what a missing
     * archetype should fall back to. */
    load_policy(&c, policy);

    if (shell_archetype_load(&c, shellf) < 0) {
        /* The safe default depends on what the machine is for. On a
         * managed machine it is `locked`, because falling back to an
         * open desktop is the failure a school cannot tolerate. On an
         * unmanaged one it is `rail`, the only archetype in which a
         * thing cannot be hidden. */
        const char *fb = c.kiosk ? "locked" : "rail";
        fprintf(stderr, "aurshell: no %s — defaulting to the %s archetype\n",
                shellf, fb);
        snprintf(c.layout_id, sizeof c.layout_id, "%s", fb);
        c.show_clock = 1;
    }
    /* The seeded table is the fallback, not the source of truth: it is
     * what a still render and a machine with no desktop files get. What
     * is actually installed wins, because "install an application and
     * it appears" is a promise the shell cannot keep from a table
     * compiled into it. */
    shell_seed_apps(&c);
    int scanned = shell_scan_apps(&c);
    if (scanned > 0)
        fprintf(stderr, "aurshell: %d application%s installed\n",
                scanned, scanned == 1 ? "" : "s");
    else
        fprintf(stderr, "aurshell: no desktop entries found — "
                        "showing the built-in placeholder set\n");

    c.mouse_x = mouse_x0;
    c.mouse_y = mouse_y0;
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
        if (png_w <= 0 || png_h <= 0 ||
            (long long)png_w * png_h > 64LL * 1024 * 1024) {
            fprintf(stderr, "aurshell: --size %dx%d is not a usable image\n",
                    png_w, png_h);
            free_fonts(&f); return 1;
        }
        surface *s = surface_new(png_w, png_h), *wall = NULL;
        if (!s) { fprintf(stderr, "aurshell: out of memory\n");
                  free_fonts(&f); return 1; }
        build_wallpaper(&wall, png_w, png_h, &t);
        c.screen_w = png_w; c.screen_h = png_h;
        if (L->init) L->init(&c);

        /* Offscreen, but with real applications in it. Everything below
         * -- the compositor, the reconcile, the routing -- is the same
         * code the booted machine runs; only the destination differs. */
        if (with_app) {
            if (!getenv("XDG_RUNTIME_DIR")) {
                char rd[64];
                snprintf(rd, sizeof rd, "/run/user/%u", (unsigned)getuid());
                if (mkdir(rd, 0700) < 0 && errno != EEXIST)
                    snprintf(rd, sizeof rd, "%s", "/tmp");
                setenv("XDG_RUNTIME_DIR", rd, 1);
            }
            c.wl = aurwl_create(png_w, png_h, 60000);
            if (!c.wl) { fprintf(stderr, "aurshell: no compositor\n");
                         free_fonts(&f); return 1; }
            c.spawn = session_spawn;
            fprintf(stderr, "aurshell: WAYLAND_DISPLAY=%s, starting %s\n",
                    aurwl_socket(c.wl), with_app);
            if (aurwl_spawn(c.wl, with_app) < 0) {
                fprintf(stderr, "aurshell: could not start it\n");
                free_fonts(&f); return 1;
            }
            struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
            for (;;) {
                struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
                uint32_t el = (uint32_t)((tn.tv_sec - t0.tv_sec) * 1000
                                       + (tn.tv_nsec - t0.tv_nsec) / 1000000);
                if (el > (uint32_t)app_wait * 1000u) break;
                struct pollfd wp = { aurwl_fd(c.wl), POLLIN, 0 };
                poll(&wp, 1, 16);
                aurwl_dispatch(c.wl);
                aurwl_reap(c.wl);
                session_sync(&c, L->present);
                aurwl_frame_done(c.wl, el);
                /* Paint each pass: the tracker records where windows
                 * landed, and a client that never learns its size keeps
                 * redrawing the same first frame. */
                /* Animations have to run here too, or a carousel that
                 * was asked to scroll to a new window never arrives and
                 * the render shows the window halfway off the screen --
                 * which looks like a placement bug and is a missing
                 * clock tick. */
                if (L->step) L->step(&c, 0.016f);
                draw_track_reset();
                L->paint(&c, s, &f, wall);
                if (c.n_wins > 0 && !aurwl_focus(c.wl)) {
                    int n = aurwl_window_count(c.wl);
                    if (n) aurwl_set_focus(c.wl, aurwl_window_at(c.wl, 0));
                }
            }
            fprintf(stderr, "aurshell: %d window%s on the desktop\n",
                    c.n_wins, c.n_wins == 1 ? "" : "s");
            for (int i = 0; i < c.n_wins; i++)
                fprintf(stderr, "  %-40s %s\n", c.wins[i].title,
                        c.wins[i].content ? "live" : "no content");
        }

        if (L->motion && c.mouse_x >= 0) L->motion(&c, c.mouse_x, c.mouse_y);

        /* Paint-only timing. The whole process also builds a wallpaper
         * and loads fonts, which happen once at login and drown the
         * number that actually decides whether the desktop feels alive. */
        if (frames < 1) frames = 1;
        double *ms = frames > 1 ? malloc((size_t)frames * sizeof *ms) : NULL;
        for (int fr = 0; fr < frames; fr++) {
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            draw_track_reset();
            L->paint(&c, s, &f, wall);
            session_paint_popups(&c, s);
            clock_gettime(CLOCK_MONOTONIC, &b);
            if (ms) ms[fr] = (double)(b.tv_sec - a.tv_sec) * 1e3
                           + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
        }
        if (ms) {
            for (int i = 1; i < frames; i++)      /* insertion sort: tiny n */
                for (int j = i; j > 0 && ms[j] < ms[j-1]; j--) {
                    double t = ms[j]; ms[j] = ms[j-1]; ms[j-1] = t;
                }
            fprintf(stderr, "aurshell: %s %dx%d  paint median %.1f ms  "
                            "(min %.1f, max %.1f, n=%d)\n",
                    L->id, png_w, png_h, ms[frames/2], ms[0], ms[frames-1], frames);
            free(ms);
        }
        /* The cursor belongs in a screenshot too: it is part of the
         * frame the user sees, and leaving it out once cost an hour of
         * chasing a pointer bug that a screenshot would have shown. */
        if (c.mouse_x >= 0 && c.mouse_y >= 0)
            paint_cursor(s, c.mouse_x, c.mouse_y,
                         theme_color(&t, "col_fg_hi", 0xF3F7FD),
                         theme_color(&t, "col_bg",    0x0B0E14));

        uint32_t *o = malloc((size_t)png_w * png_h * sizeof *o);
        if (!o) { fprintf(stderr, "aurshell: out of memory\n");
                  surface_free(s); surface_free(wall); free_fonts(&f); return 1; }
        for (size_t i = 0; i < (size_t)png_w * png_h; i++) o[i] = s->px[i] & 0xFFFFFFu;
        int rc = png_write_rgb(png_out, o, png_w, png_h);
        free(o); surface_free(s); surface_free(wall);
        if (L->fini) L->fini(&c);
        free_fonts(&f);
        fprintf(stderr, "aurshell: wrote %s (%dx%d, %s)\n", png_out, png_w, png_h, L->id);
        return rc;
    }

    /* ── the diagnostic ─────────────────────────────────────────── */
    if (input_test) {
        /* Line-buffered: a diagnostic that shows nothing until it is
         * killed is not a diagnostic, and this one is meant to be piped
         * into a support ticket. */
        setvbuf(stdout, NULL, _IOLBF, 0);
        input_set in;
        input_open_all(&in, 0);
        printf("%d device%s the shell will listen to "
               "(anything not listed was examined and ignored):\n\n",
               in.n, in.n == 1 ? "" : "s");
        for (int i = 0; i < in.n; i++) {
            char nm[128] = "?";
            ioctl(in.fd[i], EVIOCGNAME(sizeof nm), nm);
            printf("  /dev/input/%-9s %-38s", in.name[i], nm);
            int k = in.kind[i];
            printf(" %s%s%s%s%s\n",
                   (k & DEV_KBD)    ? "keyboard "    : "",
                   (k & DEV_REL)    ? "mouse "       : "",
                   (k & DEV_PAD)    ? "touchpad "    : "",
                   (k & DEV_DIRECT) ? "touchscreen " :
                   (k & DEV_ABS)    ? "tablet "      : "",
                   k ? "" : "(ignored)");
            if (k & (DEV_ABS | DEV_PAD))
                printf("  %-9s   %-38s x %d..%d, y %d..%d\n", "",
                       "", in.ax_lo[i], in.ax_hi[i], in.ay_lo[i], in.ay_hi[i]);
        }
        if (!in.n) printf("  none. Check that this user can read "
                          "/dev/input/event* (the `input` group).\n");
        printf("\nMove the pointer and press things. Ctrl-C to stop.\n"
               "A pointer that does not change below is the bug.\n\n");

        shell_ctx d;
        memset(&d, 0, sizeof d);
        d.mouse_x = 640; d.mouse_y = 400;
        while (!want_quit) {
            struct pollfd pfd[MAX_INPUT_DEV];
            for (int i = 0; i < in.n; i++) { pfd[i].fd = in.fd[i]; pfd[i].events = POLLIN; }
            if (poll(pfd, in.n, 1000) <= 0) continue;
            for (int i = in.n - 1; i >= 0; i--) {
                if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    printf("  %-9s WENT AWAY\n", in.name[i]);
                    input_drop(&in, i); continue;
                }
                if (!(pfd[i].revents & POLLIN)) continue;
                struct input_event ev;
                while (read(in.fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
                    const char *t = ev.type == EV_REL ? "REL" :
                                    ev.type == EV_ABS ? "ABS" :
                                    ev.type == EV_KEY ? "KEY" :
                                    ev.type == EV_SYN ? "SYN" : "???";
                    if (ev.type == EV_SYN) {
                        if ((in.kind[i] & DEV_PAD) && ev.code == SYN_REPORT)
                            pad_synced(&in.pad[i]);
                        continue;
                    }
                    if (ev.type == EV_REL) {
                        if (ev.code == REL_X) d.mouse_x += ev.value;
                        if (ev.code == REL_Y) d.mouse_y += ev.value;
                        clamp_pointer(&d, 1280, 800);
                    } else if (ev.type == EV_ABS && (in.kind[i] & DEV_PAD)) {
                        int axis = (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y);
                        int step = pad_delta(&in.pad[i], axis, ev.value);
                        if (axis) d.mouse_y += step; else d.mouse_x += step;
                        clamp_pointer(&d, 1280, 800);
                    } else if (ev.type == EV_ABS && (in.kind[i] & DEV_ABS)) {
                        if (ev.code == ABS_X && in.ax_hi[i] > in.ax_lo[i])
                            d.mouse_x = (int)((int64_t)(ev.value - in.ax_lo[i]) * 1280
                                              / (in.ax_hi[i] - in.ax_lo[i] + 1));
                        if (ev.code == ABS_Y && in.ay_hi[i] > in.ay_lo[i])
                            d.mouse_y = (int)((int64_t)(ev.value - in.ay_lo[i]) * 800
                                              / (in.ay_hi[i] - in.ay_lo[i] + 1));
                        clamp_pointer(&d, 1280, 800);
                    }
                    printf("  %-9s %s code %-4d value %-8d -> pointer %4d,%4d\n",
                           in.name[i], t, ev.code, ev.value, d.mouse_x, d.mouse_y);
                    fflush(stdout);
                }
            }
        }
        for (int i = 0; i < in.n; i++) close(in.fd[i]);
        if (in.notify_fd >= 0) close(in.notify_fd);
        free_fonts(&f);
        return 0;
    }

    kms_display *disp = kms_open(card);
    if (!disp) return 1;
    fprintf(stderr, "aurshell: %dx%d on connector %u\n",
            disp->width, disp->height, disp->connector_id);

    /* The console owns the keyboard and the VT until we say otherwise.
     * Without this the shell is painting over a text console that is
     * still echoing every keystroke, Ctrl-Alt-Del still reboots, and
     * Ctrl-Alt-F2 still switches away -- taking DRM master with it and
     * leaving a dead display behind. On a managed machine that is a
     * three-key denial of service; on any machine it is a bug. */
    console_take(&g_con, !c.allow_tty);

    c.screen_w = disp->width;
    c.screen_h = disp->height;

    /* A Wayland socket has to live somewhere a client can find it. The
     * shell is a system service, not a user session, so nothing has set
     * XDG_RUNTIME_DIR for us -- and without it aurwl has nowhere to
     * bind and not one application can start. */
    if (!getenv("XDG_RUNTIME_DIR")) {
        char rd[64];
        snprintf(rd, sizeof rd, "/run/user/%u", (unsigned)getuid());
        if (mkdir(rd, 0700) < 0 && errno != EEXIST)
            snprintf(rd, sizeof rd, "%s", "/tmp");
        setenv("XDG_RUNTIME_DIR", rd, 1);
        fprintf(stderr, "aurshell: XDG_RUNTIME_DIR was unset — using %s\n", rd);
    }
    c.wl = aurwl_create(disp->width, disp->height, disp->refresh_mhz);
    if (c.wl) {
        c.spawn = session_spawn;
        fprintf(stderr, "aurshell: applications may connect on WAYLAND_DISPLAY=%s\n",
                aurwl_socket(c.wl));
    } else {
        /* The desktop still works; it just cannot run anything. Saying
         * so plainly beats a machine where every icon silently does
         * nothing and nobody can tell why. */
        fprintf(stderr, "aurshell: NO COMPOSITOR — applications cannot start\n");
    }

    surface *wall = NULL;
    build_wallpaper(&wall, disp->width, disp->height, &t);
    if (L->init) L->init(&c);

    input_set in;
    input_open_all(&in, c.kiosk);
    int n_kbd = 0, n_ptr = 0;
    for (int i = 0; i < in.n; i++) {
        if (in.kind[i] & DEV_KBD)            n_kbd++;
        if (in.kind[i] & (DEV_REL | DEV_ABS)) n_ptr++;
    }
    fprintf(stderr, "aurshell: %d keyboard%s, %d pointer%s%s%s\n",
            n_kbd, n_kbd == 1 ? "" : "s", n_ptr, n_ptr == 1 ? "" : "s",
            c.kiosk ? ", grabbed (kiosk)" : "",
            in.notify_fd >= 0 ? ", hotplug on" : ", NO HOTPLUG");
    if (!n_ptr) fprintf(stderr, "aurshell: no pointer found — "
                                "keyboard only until one is plugged in\n");

    /* Start the pointer centred so it is findable on the first frame. */
    c.mouse_x = disp->width / 2;
    c.mouse_y = disp->height / 2;

    uint32_t cur_fill = theme_color(&t, "col_fg_hi", 0xF3F7FD);
    uint32_t cur_edge = theme_color(&t, "col_bg",    0x0B0E14);

    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);

    /* Damage tracking. A desktop that repaints four times a second
     * forever keeps a fanless machine warm and its battery flat for no
     * benefit whatsoever: the pixels are identical. Repaint when
     * something actually changed -- input, an animation, a new minute
     * on the clock, a theme reload, or losing and regaining the VT. */
    int dirty = 1, last_min = -1;
    uint32_t last_damage = 0;
    int super_down = 0;

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
                dirty = 1;
            }
        }

        /* A VT switch away means another process owns the display.
         * Painting into a buffer nothing scans out is wasted work, and
         * the ioctls fail anyway, so stop until we are back. */
        if (want_release) {
            want_release = 0;
            console_release(&g_con, disp->fd);
        }
        if (want_acquire) {
            want_acquire = 0;
            console_acquire(&g_con, disp->fd);
            dirty = 1;
        }
        if (!g_con.active) {
            struct pollfd idle = { -1, 0, 0 };
            poll(&idle, 0, 120);
            continue;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - last.tv_sec)
                 + (float)(now.tv_nsec - last.tv_nsec) / 1e9f;
        last = now;
        if (dt > 0.25f) dt = 0.25f;          /* a stall must not teleport */

        /* Clients first: a window that arrived, moved or repainted has
         * to be in the list before the archetype lays the list out. */
        if (c.wl) {
            aurwl_dispatch(c.wl);
            aurwl_reap(c.wl);
            int before = c.n_wins;
            session_sync(&c, L->present);
            uint32_t seq = aurwl_damage_seq(c.wl);
            if (seq != last_damage || c.n_wins != before) { last_damage = seq; dirty = 1; }
        }

        int animating = L->step ? L->step(&c, dt) : 0;
        if (animating) dirty = 1;

        if (c.show_clock) {
            time_t tt = time(NULL);
            struct tm tm_;
            if (localtime_r(&tt, &tm_) && tm_.tm_min != last_min) {
                last_min = tm_.tm_min; dirty = 1;
            }
        }

        if (dirty) {
            surface *fb = kms_back_surface(disp);
            /* Cleared before the archetype paints, so the rectangles it
             * records are this frame's and input routes against what is
             * on screen rather than what was. */
            draw_track_reset();
            L->paint(&c, fb, &f, wall);
            session_paint_popups(&c, fb);
            paint_cursor(fb, c.mouse_x, c.mouse_y, cur_fill, cur_edge);
            /* A failed flip is not cosmetic: it means we no longer own
             * the display. Say so once rather than painting into the
             * void for the rest of the session. */
            if (kms_flip(disp) < 0 && g_con.active) {
                static int moaned = 0;
                if (!moaned++) fprintf(stderr, "aurshell: display present failed "
                                               "— lost DRM master?\n");
            }
            /* Every toolkit throttles itself to this. Without it a
             * client draws exactly one frame and then waits forever,
             * which looks like an application that has hung. */
            if (c.wl) {
                struct timespec ft; clock_gettime(CLOCK_MONOTONIC, &ft);
                aurwl_frame_done(c.wl, (uint32_t)(ft.tv_sec * 1000u + ft.tv_nsec / 1000000u));
            }
            dirty = 0;
        }
        if (once) break;

        /* Animating: poll briefly so the next frame is soon. Idle: wait
         * a full second; with damage tracking there is nothing to do
         * until an event arrives, and the clock is handled above. */
        struct pollfd pfd[MAX_INPUT_DEV + 2];
        int np = 0;
        for (int i = 0; i < in.n; i++) { pfd[np].fd = in.fd[i]; pfd[np].events = POLLIN; np++; }
        int noti = -1;
        if (in.notify_fd >= 0) { noti = np; pfd[np].fd = in.notify_fd;
                                 pfd[np].events = POLLIN; np++; }
        int wlfd = -1;
        if (c.wl) { wlfd = np; pfd[np].fd = aurwl_fd(c.wl); pfd[np].events = POLLIN; np++; }
        /* With a client on screen the wait is short: it is drawing, and
         * the next thing to happen is its next buffer, not a keystroke. */
        int wait_ms = animating ? 8 : (c.n_wins > 0 ? 16 : 1000);
        if (poll(pfd, np, wait_ms) <= 0) continue;
        (void)wlfd;

        if (noti >= 0 && (pfd[noti].revents & POLLIN)) {
            char buf[4096];
            while (read(in.notify_fd, buf, sizeof buf) > 0) { }
            int added = input_scan(&in, c.kiosk);
            if (added) {
                fprintf(stderr, "aurshell: %d input device%s appeared\n",
                        added, added == 1 ? "" : "s");
                dirty = 1;
            }
        }

        /* Walk backwards: dropping a device compacts the array, so a
         * forward walk would skip the entry moved into the hole. */
        for (int i = in.n - 1; i >= 0; i--) {
            short re = pfd[i].revents;
            /* POLLERR/POLLHUP arrive whether or not we asked for them,
             * and the kernel keeps reporting them forever once a device
             * is gone. Skipping the fd without closing it turns poll()
             * into a busy loop -- a shipped desktop pinning a core at
             * 100% because someone unplugged a mouse. */
            if (re & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "aurshell: input device %s went away\n", in.name[i]);
                input_drop(&in, i);
                continue;
            }
            if (!(re & POLLIN)) continue;

            struct input_event ev;
            ssize_t got;
            while ((got = read(in.fd[i], &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                if (ev.type == EV_SYN) {
                    /* The kernel dropped events because we were too slow
                     * painting. Anything we think is held down may not
                     * be; the safe assumption is nothing is. */
                    if (ev.code == SYN_DROPPED) c.mouse_down = 0;
                    else if (ev.code == SYN_REPORT && (in.kind[i] & DEV_PAD))
                        pad_synced(&in.pad[i]);
                    continue;
                }
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) c.mouse_x += ev.value;
                    if (ev.code == REL_Y) c.mouse_y += ev.value;
                    /* A browser that cannot scroll is a poster of a
                     * browser, so the wheel is routed even though no
                     * archetype has ever used it. */
                    if (ev.code == REL_WHEEL || ev.code == REL_HWHEEL)
                        session_scroll(&c, c.mouse_x, c.mouse_y,
                                       ev.code == REL_HWHEEL, -(double)ev.value);
                    clamp_pointer(&c, disp->width, disp->height);
                    dirty = 1;
                } else if (ev.type == EV_ABS && (in.kind[i] & DEV_ABS)) {
                    /* +1 because the range is inclusive: a tap on the
                     * rightmost column reports `maximum`, and dividing
                     * by (hi - lo) maps that to exactly `width` -- one
                     * pixel off the screen, missing every target. */
                    if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                        int lo = in.ax_lo[i], hi = in.ax_hi[i];
                        if (hi > lo)
                            c.mouse_x = (int)((int64_t)(ev.value - lo) * disp->width
                                              / (hi - lo + 1));
                    } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                        int lo = in.ay_lo[i], hi = in.ay_hi[i];
                        if (hi > lo)
                            c.mouse_y = (int)((int64_t)(ev.value - lo) * disp->height
                                              / (hi - lo + 1));
                    }
                    clamp_pointer(&c, disp->width, disp->height);
                    dirty = 1;
                } else if (ev.type == EV_ABS && (in.kind[i] & DEV_PAD)) {
                    int axis = (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y);
                    if (ev.code == ABS_X || ev.code == ABS_Y ||
                        ev.code == ABS_MT_POSITION_X || ev.code == ABS_MT_POSITION_Y) {
                        int step = pad_delta(&in.pad[i], axis, ev.value);
                        if (step) {
                            if (axis) c.mouse_y += step; else c.mouse_x += step;
                            clamp_pointer(&c, disp->width, disp->height);
                            dirty = 1;
                        }
                    }
                } else if (ev.type == EV_KEY && (in.kind[i] & DEV_PAD) &&
                           pad_event_kind(ev.code) >= 0) {
                    if (pad_button(&in.pad[i], pad_event_kind(ev.code),
                                   ev.value != 0, now_ms())) {
                        /* A tap is a press and a release in one go, so
                         * the layout sees the same sequence a physical
                         * click produces and drags still work. */
                        c.mouse_down = 1;
                        if (L->click)  L->click(&c, c.mouse_x, c.mouse_y);
                        c.mouse_down = 0;
                        if (L->motion) L->motion(&c, c.mouse_x, c.mouse_y);
                    }
                    dirty = 1;
                } else if (ev.type == EV_KEY) {
                    /* BTN_TOUCH is a click only where the user is
                     * touching the thing they are pointing at. On a
                     * touchpad it fires on every finger-down and lift,
                     * so honouring it there means the pointer cannot be
                     * moved without clicking something. */
                    int is_btn = (ev.code == BTN_LEFT) ||
                                 (ev.code == BTN_TOUCH && (in.kind[i] & DEV_DIRECT));
                    if (is_btn) {
                        c.mouse_down = (ev.value != 0);
                        /* Dispatch on PRESS. Release would let a user
                         * abort a mis-click by sliding off the target,
                         * which is nicer -- but every archetype that
                         * supports dragging starts the drag in click()
                         * and ends it in motion() when the button comes
                         * up, so a release-dispatched click sets and
                         * cancels the drag in the same instant. Press
                         * is also what every other desktop does. */
                        /* A click that lands on an application's own
                         * pixels is that application's. Letting the
                         * archetype also act on it is how a desktop
                         * ends up closing a window because the user
                         * pressed a button inside it. */
                        int taken = session_button(&c, c.mouse_x, c.mouse_y,
                                                   BTN_LEFT, ev.value != 0);
                        if (!taken) {
                            if (ev.value && L->click) L->click(&c, c.mouse_x, c.mouse_y);
                            if (!ev.value && L->motion) L->motion(&c, c.mouse_x, c.mouse_y);
                        }
                        dirty = 1;
                    } else if (ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                        /* Context menus arrive as popups, which is why
                         * the right button is worth forwarding even
                         * though no archetype has a use for it. */
                        session_button(&c, c.mouse_x, c.mouse_y, ev.code, ev.value != 0);
                        dirty = 1;
                    } else if (in.kind[i] & DEV_KBD) {
                        if (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA)
                            super_down = (ev.value != 0);

                        if (ev.value && ev.code == KEY_ESC && c.kiosk) continue;

                        /* Super belongs to the desktop, always. Without
                         * one chord the shell keeps for itself, a
                         * full-screen application is a machine the user
                         * cannot get out of -- which on a kiosk is the
                         * whole product and everywhere else is a trap. */
                        int consumed = 0;
                        if (!super_down)
                            consumed = session_key(&c, ev.code, ev.value != 0);
                        if (!consumed && ev.value && L->key) L->key(&c, ev.code);
                        dirty = 1;
                    }
                }
            }
            /* A device that vanished between poll() and read() reports
             * ENODEV rather than a POLLHUP we have already consumed. */
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                fprintf(stderr, "aurshell: input device %s failed (%s)\n",
                        in.name[i], strerror(errno));
                input_drop(&in, i);
                continue;
            }
            session_motion(&c, c.mouse_x, c.mouse_y);
            if (L->motion) L->motion(&c, c.mouse_x, c.mouse_y);
        }
    }

    if (c.wl) aurwl_destroy(c.wl);
    for (int i = 0; i < in.n; i++) { ioctl(in.fd[i], EVIOCGRAB, 0); close(in.fd[i]); }
    if (in.notify_fd >= 0) close(in.notify_fd);
    console_give_back(&g_con);
    if (L->fini) L->fini(&c);
    free_fonts(&f);
    surface_free(wall);
    kms_close(disp);
    fprintf(stderr, "aurshell: exit\n");
    return 0;
}
