/* screen.c — what the one restart looks like
 *
 * docs/issues/v3/05 item 5.5. After AurBridge restarts the computer,
 * the staging environment runs for somewhere between ten minutes and an
 * hour, and until this existed what a person saw for all of it was the
 * kernel's console: "aurstage: drive    ok, SMART says healthy",
 * " 40% 50% 60%", and an occasional sentence in among them. It could be
 * read. It was not something anybody who had never seen a console
 * should be asked to sit in front of while their computer's only copy
 * of Windows is being shrunk.
 *
 * So this paints a screen: where the install is, of seven steps; the
 * sentence aurstage last said, which is already written for her; a bar
 * when there is something to measure; and, from the moment the
 * Windows drive is touched until it is finished, "do not turn the
 * computer off" in a band nobody can miss. At the foot, small, the
 * last few raw lines -- because the thing to ask a tester for when
 * anything stops is a photo of the verdict= line, and it has to be on
 * the photo.
 *
 * HOW IT IS FED, AND WHY IT CANNOT HURT THE INSTALL
 *
 * aurstage is PID 1 and the only program that writes to a disk. It
 * starts this with a pipe on its standard input and copies every line
 * it says into the pipe as well as onto the console (boot.c). The
 * write end is non-blocking and SIGPIPE is ignored, so a screen that
 * hangs, crashes or never starts costs aurstage nothing: the lines are
 * dropped and the console still has them. This program opens no disk,
 * reads no disk, and takes no decision; it only listens.
 *
 * It lives outside src/aurstage on purpose. build/staging's write gate
 * says exactly one file in that directory may open a device writably;
 * a display device is not a disk, but a gate with an exception for
 * "devices that are not disks" is the exception nobody reviewed.
 *
 *   aurscreen                        paint to the first DRM device
 *   aurscreen --png F --size W H     read all of stdin, paint the last
 *                                    state to F (tools/screentest.sh)
 *   aurscreen --state                read all of stdin, print the state
 *   aurscreen --mode install|restore|dry   instead of /proc/cmdline
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../aurshell/kms.h"
#include "../aurshell/draw.h"
#include "../common/font.h"
#include "../common/png.h"

/* Hardcoded, like aursorry's: nothing here may depend on a theme file
 * the install has not written yet. Nocturne's ink and accent. */
#define PAPER  0x0B0E14u
#define INK    0xECEFF2u
#define DIM    0x8C95A0u
#define FAINT  0x4A525C
#define ACCENT 0x7DD3C0u
#define WARN   0xF0BA68u
#define BAD    0xF2788Du

enum { M_INSTALL, M_RESTORE, M_DRY };

/* The seven steps, and the sentence of aurstage's that starts each.
 * Matched on a prefix of a sentence install.c already says, so a step
 * is never claimed before the code that does it has started. */
static const struct { const char *starts, *label; } STEP[] = {
    { "",                                            "Checking this computer" },
    { "Saving this computer's Windows startup",      "Saving the way back to Windows" },
    { "Making room on the Windows drive",            "Making room on the Windows drive" },
    { "Copying AurOS onto this computer",            "Copying AurOS" },
    { "Making this computer able to start AurOS",    "Setting up how it starts" },
    { "Checking that this computer works under AurOS","Checking AurOS works here" },
    { "Writing the new layout",                      "Finishing" },
};
#define NSTEP ((int)(sizeof STEP / sizeof STEP[0]))
#define RISKY_FROM 2   /* "Making room": the part that cannot be undone */

#define NTAIL 4
typedef struct {
    int  mode;
    int  step;                  /* index into STEP                      */
    int  pct;                   /* -1: nothing to measure               */
    char para[4][160];          /* the sentence(s) aurstage last said   */
    int  npara, para_open;
    char tail[NTAIL][160];      /* the last raw lines, for a photo      */
    int  ntail;
    char verdict[96];           /* from "aurstage-report v1 verdict=..." */
    int  stopped;               /* a refusal or failure was reported    */
    int  handover;              /* the install finished                 */
    int  dirty;
} state;

static void push_tail(state *s, const char *l)
{
    if (s->ntail == NTAIL) {
        memmove(s->tail[0], s->tail[1], sizeof s->tail[0] * (NTAIL - 1));
        s->ntail--;
    }
    snprintf(s->tail[s->ntail++], sizeof s->tail[0], "%s", l);
}

/* "drive    ok" and "         a remedy": aurstage's two-column detail
 * lines. Everything else that starts with a capital is a sentence
 * written for the person in front of the computer. */
static int is_detail(const char *t)
{
    if (t[0] == ' ') return 1;
    int i = 0;
    while (t[i] >= 'a' && t[i] <= 'z') i++;
    return i > 0 && i <= 8 && t[i] == ' ' && t[i + 1] == ' ';
}

static void feed(state *s, char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;

    if (!strncmp(line, "aurstage-progress ", 18)) {
        int p = atoi(line + 18);
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        if (p != s->pct) { s->pct = p; s->dirty = 1; }
        return;
    }
    const char *t = line;
    if (!strncmp(t, "aurstage: ", 10)) t += 10;
    if (*t) push_tail(s, t);
    s->dirty = 1;

    const char *v = strstr(t, "aurstage-report v1 verdict=");
    if (v) {
        v += 27;
        size_t k = strcspn(v, " ");
        if (k >= sizeof s->verdict) k = sizeof s->verdict - 1;
        memcpy(s->verdict, v, k); s->verdict[k] = 0;
        s->stopped = strstr(t, "record=refused") || strstr(t, "record=failed") ||
                     !strncmp(s->verdict, "no-", 3) || strstr(s->verdict, "fail") ||
                     strstr(s->verdict, "refus");
        s->para_open = 0;
        return;
    }
    if (strstr(t, "handing over to the system on")) { s->handover = 1; s->pct = -1; return; }

    if (!*t) { s->para_open = 0; return; }          /* a blank line ends one */
    if (!strncmp(t, "WARNING: ", 9) || is_detail(t)) return;
    if (!(t[0] >= 'A' && t[0] <= 'Z') && !s->para_open) return;

    if (!s->para_open) {
        s->npara = 0; s->para_open = 1;
        for (int i = 1; i < NSTEP; i++)
            if (!strncmp(t, STEP[i].starts, strlen(STEP[i].starts)) && i > s->step) {
                s->step = i; s->pct = -1;
            }
    }
    if (s->npara < 4) snprintf(s->para[s->npara++], sizeof s->para[0], "%s", t);
}

/* ── painting ───────────────────────────────────────────────────── */

static font *pick(float px, int bold)
{
    static const char *R[] = { "/usr/share/auros/fonts/ui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL };
    static const char *B[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL };
    const char **t = bold ? B : R;
    for (int i = 0; t[i]; i++) { font *f = font_load(t[i], px); if (f) return f; }
    return NULL;
}

static void text(surface *s, font *f, int x, int y, const char *t, uint32_t c, float a)
{
    if (f && t && *t)
        font_draw(f, s->px, s->w, s->h, s->stride, (float)x, (float)y + font_ascent(f), t, c, a);
}

static const char *title_of(const state *st)
{
    if (st->mode == M_RESTORE)
        return st->stopped ? "Windows was not put back" : "Putting Windows back";
    if (st->mode == M_DRY) return "Checking this computer";
    if (st->stopped) return "AurOS was not installed";
    if (st->handover) return "AurOS is installed";
    return "Installing AurOS";
}

static void paint(surface *s, const state *st)
{
    float k = (float)s->h / 720.f;
    if (k < 0.85f) k = 0.85f;
    if (k > 2.4f) k = 2.4f;
    font *h1 = pick(38.f * k, 1), *body = pick(20.f * k, 0),
         *small = pick(15.f * k, 0), *mono = pick(13.f * k, 0);

    surface_fill(s, 0xFF000000u | PAPER);
    int L = (int)(s->w * 0.09f), W = s->w - 2 * L;
    int y = (int)(s->h * 0.11f);

    /* the mark: the same diamond as the website and the boot menu */
    float cx = (float)L + 14.f * k, cy = (float)y + 14.f * k, r = 13.f * k;
    for (int dy = -(int)r; dy <= (int)r; dy++) {
        int half = (int)(r - (dy < 0 ? -dy : dy));
        draw_rect(s, (rect){ (int)cx - half, (int)cy + dy, 2 * half + 1, 1 }, ACCENT, 1.f);
    }
    text(s, small, L + (int)(40 * k), y + (int)(5 * k), "AurOS", DIM, 1.f);
    y += (int)(56 * k);

    uint32_t tc = st->stopped ? BAD : INK;
    text(s, h1, L, y, title_of(st), tc, 1.f);
    y += h1 ? (int)(font_line_height(h1) * 1.35f) : 40;

    /* what aurstage last said, in its own words */
    for (int i = 0; i < st->npara; i++) {
        text(s, body, L, y, st->para[i], st->stopped ? INK : INK, 0.92f);
        y += body ? (int)(font_line_height(body) * 1.35f) : 24;
    }
    y += (int)(18 * k);

    /* the bar */
    if (st->pct >= 0 && !st->stopped) {
        int bh = (int)(8 * k);
        draw_round_rect(s, (rect){ L, y, W, bh }, corners_all(bh / 2.f), FAINT, 1.f);
        int fw = (int)((long)W * st->pct / 100);
        if (fw > 0)
            draw_round_rect(s, (rect){ L, y, fw, bh }, corners_all(bh / 2.f), ACCENT, 1.f);
        char p[16]; snprintf(p, sizeof p, "%d%%", st->pct);
        text(s, small, L, y + bh + (int)(8 * k), p, DIM, 1.f);
        y += bh + (int)(40 * k);
    } else {
        y += (int)(24 * k);
    }

    /* the steps, for an install: done, now, to come */
    if (st->mode == M_INSTALL) {
        for (int i = 0; i < NSTEP; i++) {
            int done = i < st->step || st->handover;
            int now = i == st->step && !st->handover;
            uint32_t c = done ? ACCENT : now ? (st->stopped ? BAD : INK) : DIM;
            float a = done || now ? 1.f : 0.6f;
            float dx = (float)L + 7.f * k, dy = (float)y + 11.f * k;
            if (done) draw_circle(s, dx, dy, 6.f * k, ACCENT, 1.f);
            else if (now) { draw_circle(s, dx, dy, 6.f * k, c, 1.f);
                            draw_circle(s, dx, dy, 3.f * k, PAPER, 1.f); }
            else draw_circle(s, dx, dy, 3.f * k, FAINT, 1.f);
            text(s, small, L + (int)(26 * k), y + (int)(2 * k), STEP[i].label, c, a);
            y += small ? (int)(font_line_height(small) * 1.55f) : 22;
        }
    }

    /* do not turn it off: from the first irreversible write to the end */
    int risky = st->mode == M_RESTORE ? !st->stopped && st->verdict[0] == 0
              : st->mode == M_INSTALL && st->step >= RISKY_FROM && !st->stopped && !st->handover;
    int foot = s->h - (int)(28 * k) - NTAIL * (mono ? (int)font_line_height(mono) : 16);
    if (risky) {
        int bh = (int)(46 * k), by = foot - bh - (int)(22 * k);
        draw_round_rect(s, (rect){ L, by, W, bh }, corners_all(8.f * k), WARN, 0.14f);
        draw_rect(s, (rect){ L, by, (int)(4 * k), bh }, WARN, 1.f);
        text(s, body, L + (int)(18 * k), by + (bh - (body ? (int)font_line_height(body) : 20)) / 2,
             "Do not turn the computer off. It will restart by itself.", WARN, 1.f);
    }

    /* the raw lines, small, for a photo */
    draw_hrule(s, L, foot - (int)(10 * k), W, 1, FAINT, 0.6f);
    for (int i = 0; i < st->ntail; i++) {
        uint32_t c = strstr(st->tail[i], "verdict=") ? (st->stopped ? BAD : ACCENT) : DIM;
        text(s, mono, L, foot, st->tail[i], c, 0.85f);
        foot += mono ? (int)font_line_height(mono) : 16;
    }

    if (h1) font_free(h1);
    if (body) font_free(body);
    if (small) font_free(small);
    if (mono) font_free(mono);
}

static int mode_from_cmdline(void)
{
    char b[4096] = "";
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { ssize_t n = read(fd, b, sizeof b - 1); if (n > 0) b[n] = 0; close(fd); }
    if (strstr(b, "aurstage.restore")) return M_RESTORE;
    if (strstr(b, "aurstage.install")) return M_INSTALL;
    return M_DRY;
}

static volatile sig_atomic_t quit;
static void on_term(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv)
{
    const char *png = NULL;
    int pw = 1280, ph = 720, dump = 0, mode = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { pw = atoi(argv[++i]); ph = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--state")) dump = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            mode = !strcmp(m, "restore") ? M_RESTORE : !strcmp(m, "install") ? M_INSTALL : M_DRY;
        }
    }
    state st;
    memset(&st, 0, sizeof st);
    st.pct = -1;
    st.mode = mode >= 0 ? mode : mode_from_cmdline();

    /* Off-screen: everything on stdin, then one picture or one report. */
    if (png || dump) {
        char line[1024];
        while (fgets(line, sizeof line, stdin)) feed(&st, line);
        if (dump) {
            printf("mode=%d step=%d label=%s pct=%d stopped=%d handover=%d verdict=%s\n",
                   st.mode, st.step, STEP[st.step].label, st.pct, st.stopped,
                   st.handover, st.verdict[0] ? st.verdict : "-");
            for (int i = 0; i < st.npara; i++) printf("para=%s\n", st.para[i]);
        }
        if (png) {
            surface *o = surface_new(pw, ph);
            if (!o) return 1;
            paint(o, &st);
            int rc = png_write_rgb(png, o->px, o->w, o->h);
            surface_free(o);
            if (rc < 0) { fprintf(stderr, "aurscreen: cannot write %s\n", png); return 1; }
        }
        return 0;
    }

    signal(SIGTERM, on_term);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    kms_display *d = kms_open(NULL);
    if (!d) return 0;               /* no display: the console has it all */

    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    char buf[8192]; size_t have = 0;
    st.dirty = 1;
    int open_in = 1;
    while (!quit) {
        if (st.dirty) {
            surface *s = kms_back_surface(d);
            if (s) { paint(s, &st); kms_flip(d); }
            st.dirty = 0;
        }
        struct pollfd p = { 0, POLLIN, 0 };
        if (!open_in || poll(&p, 1, 500) <= 0) {
            if (!open_in) { struct timespec t = { 1, 0 }; nanosleep(&t, NULL); }
            continue;
        }
        ssize_t n = read(0, buf + have, sizeof buf - 1 - have);
        if (n == 0) { open_in = 0; continue; }   /* aurstage is gone; keep the last picture */
        if (n < 0) { if (errno != EAGAIN && errno != EINTR) open_in = 0; continue; }
        have += (size_t)n;
        buf[have] = 0;
        char *a = buf, *nl;
        while ((nl = strchr(a, '\n'))) {
            *nl = 0;
            feed(&st, a);
            a = nl + 1;
        }
        have = strlen(a);
        memmove(buf, a, have + 1);
        if (have >= sizeof buf - 2) have = 0;    /* a line that long is noise */
    }
    kms_close(d);
    return 0;
}
