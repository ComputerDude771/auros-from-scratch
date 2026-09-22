/* welcome.c — see welcome.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "welcome.h"
#include "draw.h"

#define NOTE_MAX  200
#define LINE_MAX  160

static struct {
    int  page;
    int  can_import;
    int  converted;         /* this machine came from a Windows      */
    int  asked;             /* a word is in flight                   */
    int  hover_act;
    int  sel;               /* which action the keyboard is on       */
    char note[NOTE_MAX];    /* what went wrong, in the answer's words */
    char line[LINE_MAX];    /* the last thing the import said        */
    long log_seen;          /* how much of the import log we have read */
} W = { .hover_act = -1, .sel = 0 };

/* ── reading the two files this panel is allowed to read ─────────── */

static int slurp(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, n - 1);
    close(fd);
    if (k < 0) return -1;
    buf[k] = 0;
    return (int)k;
}

/* value of `key=` in a key=value blob, or "" */
static void field(const char *blob, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = blob; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t i = 0;
            while (v[i] && v[i] != '\n' && i + 1 < n) { out[i] = v[i]; i++; }
            out[i] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

/* What aurfirst last said about this machine. */
static void read_state(char *phase, size_t n)
{
    char blob[1024];
    phase[0] = 0;
    if (slurp(WELCOME_STATE, blob, sizeof blob) < 0) return;
    field(blob, "phase", phase, n);
}

static int windows_worth_importing(void)
{
    struct stat st;
    /* ferry-detect writes JSON. An empty object is "nothing found",
     * and an absent file is a machine where the scan never ran -- both
     * mean there is nothing to offer, and offering it anyway is a
     * button that fails when pressed, which this product keeps
     * deciding is worse than no button. */
    if (stat(WELCOME_FOUND, &st) != 0) return 0;
    if (st.st_size < 16) return 0;
    char blob[4096];
    if (slurp(WELCOME_FOUND, blob, sizeof blob) < 0) return 0;
    return strstr(blob, "\"device\"") != NULL || strstr(blob, "\"windows\"") != NULL;
}

/* ── asking for something ────────────────────────────────────────── */

static int ask_for(const char *word)
{
    char path[512];
    snprintf(path, sizeof path, "%s/answer.result", WELCOME_RUN);
    unlink(path);                       /* last time's answer is not this one */
    snprintf(path, sizeof path, "%s/answer", WELCOME_RUN);
    /* O_EXCL: two presses in the same second must not make two
     * requests, and a request already sitting there is one the root
     * side has not picked up yet. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno == EEXIST) return 0;    /* already asked */
    if (fd < 0) return -1;
    ssize_t k = write(fd, word, strlen(word));
    close(fd);
    if (k != (ssize_t)strlen(word)) return -1;
    W.asked = 1;
    W.note[0] = 0;
    return 0;
}

/* The last non-empty line of the import log, so the screen says
 * something other than "please wait" for the ten minutes this can
 * take on a ten-year-old disk. */
static int read_import_line(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/import.log", WELCOME_RUN);
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (st.st_size == W.log_seen) return 0;
    W.log_seen = (long)st.st_size;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char tail[2048];
    off_t from = st.st_size > (off_t)sizeof tail ? st.st_size - (off_t)sizeof tail : 0;
    ssize_t k = pread(fd, tail, sizeof tail - 1, from);
    close(fd);
    if (k <= 0) return 0;
    tail[k] = 0;
    char *last = NULL;
    for (char *p = strtok(tail, "\n"); p; p = strtok(NULL, "\n"))
        if (*p) last = p;
    if (!last) return 0;
    snprintf(W.line, sizeof W.line, "%s", last);
    return 1;
}

/* ── the state machine ───────────────────────────────────────────── */

int welcome_init(shell_ctx *c)
{
    char phase[32];
    read_state(phase, sizeof phase);
    W.converted  = !strcmp(phase, "asking") || !strcmp(phase, "done") ||
                   !strcmp(phase, "declined");
    W.can_import = windows_worth_importing();
    W.page = W_ASK;
    W.sel = 0;
    /* ONLY WHILE THE QUESTION IS OPEN. A machine that answered it
     * yesterday, or was never converted from anything, sees nothing:
     * a desktop that greets you every morning is a desktop you stop
     * reading. */
    c->welcome_open = !strcmp(phase, "asking");
    return c->welcome_open;
}

int welcome_wait_ms(void) { return W.asked ? 200 : -1; }

int welcome_step(shell_ctx *c)
{
    if (!c->welcome_open) return 0;
    int dirty = 0;
    if (W.page == W_IMPORTING && read_import_line()) dirty = 1;
    if (!W.asked) return dirty;

    char path[512];
    snprintf(path, sizeof path, "%s/answer.result", WELCOME_RUN);
    char blob[1024];
    if (slurp(path, blob, sizeof blob) < 0) return dirty;

    char req[32], res[32], note[NOTE_MAX];
    field(blob, "request", req, sizeof req);
    field(blob, "result",  res, sizeof res);
    field(blob, "note",    note, sizeof note);
    /* A result file with no result line is one that was being written
     * while we read it. answer.sh writes it whole and renames, so this
     * should not happen -- and "should not happen" is why it is
     * checked rather than assumed. */
    if (!res[0]) return dirty;

    W.asked = 0;
    snprintf(W.note, sizeof W.note, "%s", note);
    unlink(path);

    if (!strcmp(res, "ok") || !strcmp(res, "partial")) {
        if (!strcmp(req, "confirm"))      W.page = W_CONFIRMED;
        else if (!strcmp(req, "decline")) W.page = W_DECLINED;
        else if (!strcmp(req, "import"))  W.page = W_IMPORTED;
    } else {
        W.page = W_TROUBLE;
    }
    W.sel = 0;
    return 1;
}

/* ── where everything is ─────────────────────────────────────────── */

enum { A_YES, A_LATER, A_NO, A_IMPORT, A_SKIP, A_CLOSE, A_AGAIN, A_N };
static const char *ACT_LABEL[A_N] = {
    "Yes, it all works",
    "Let me look first",
    "No, go back to Windows",
    "Bring my files across",
    "Not now",
    "Close",
    "Try again",
};

typedef struct {
    int  gx, cw;
    int  head_y, rule_y, sub_y, body_y;
    /* The last baseline the explanatory lines may use. On a small
     * screen at large text the buttons take the room and this goes
     * above body_y, which means no body lines are drawn at all. */
    int  body_max_y;
    rect acts[A_N]; int act[A_N]; int n_acts;
} welcome_geom;

static int clampi_(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static welcome_view view_now(void)
{
    welcome_view v = { W.page, W.can_import };
    return v;
}

static int actions_for(const welcome_view *v, int *which)
{
    int n = 0;
    switch (v->page) {
    case W_ASK:
        which[n++] = A_YES; which[n++] = A_LATER; which[n++] = A_NO;
        break;
    case W_WORKING:   break;
    case W_CONFIRMED:
        if (v->can_import) which[n++] = A_IMPORT;
        which[n++] = v->can_import ? A_SKIP : A_CLOSE;
        break;
    case W_IMPORTING: break;
    case W_IMPORTED:  which[n++] = A_CLOSE; break;
    case W_DECLINED:  which[n++] = A_CLOSE; break;
    case W_TROUBLE:   which[n++] = A_AGAIN; which[n++] = A_CLOSE; break;
    }
    return n;
}

static void welcome_layout(const shell_ctx *c, int sw, int sh,
                           const welcome_view *v, welcome_geom *g)
{
    memset(g, 0, sizeof *g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    g->gx = clampi_(sw / 14, 20, 110);
    g->cw = sw - 2 * g->gx;
    if (g->cw < 120) { g->gx = 8; g->cw = sw - 16; }

    g->head_y = (int)(52.f * k);
    g->rule_y = g->head_y + (int)(14.f * k);
    g->sub_y  = g->rule_y + (int)(30.f * k);
    g->body_y = g->sub_y + (int)(34.f * k);

    int act_h = (int)(34.f + 16.f * k); if (act_h < 44) act_h = 44;
    int gap   = (int)(10.f * k); if (gap < 8) gap = 8;
    int last_y = sh - (int)(16.f * k) - act_h;
    if (last_y < 0) last_y = 0;

    int which[A_N];
    int na = actions_for(v, which);
    g->n_acts = na;
    if (!na) return;

    /* A COLUMN WHEN THEY DO NOT FIT, not three squeezed buttons.
     * "No, go back to Windows" is a long sentence on purpose -- it is
     * the one press in this product that undoes an installation, and
     * it should read like one -- and at her largest text on a 1024
     * screen three of those across the page are 44 pixels of unreadable
     * each. docs/EASY.md rule 4 is a floor on the SHORTER side, which a
     * squeezed row meets while being unusable. */
    int want_w = 0;
    for (int i = 0; i < na; i++) {
        int w = (int)(60.f + 13.f * k * (float)strlen(ACT_LABEL[which[i]]));
        if (w > want_w) want_w = w;
    }
    int room = g->cw - (na - 1) * gap;
    int column = (na > 1) && (want_w * na > room);

    if (column) {
        /* THE ANSWERS COME FIRST AND THE EXPLANATION GIVES WAY.
         *
         * The first version clamped the stack's top down to just below
         * the body text, which at her largest text on a 1024x600
         * screen pushed the third button four pixels off the bottom of
         * the panel -- so "No, go back to Windows" could not be
         * pressed, on exactly the machine this product exists for, at
         * exactly the text size somebody who needs it would choose.
         * tools/targets.c found it within a minute of being pointed at
         * this panel, which is the entire argument for the geometry
         * contract these panels keep.
         *
         * So the floor is the SUBTITLE, not the body: three sentences
         * of reassurance are worth less than a reachable answer, and
         * `body_max_y` below tells the painter where to stop. */
        int min_y = g->sub_y + (int)(20.f * k);
        int bottom = (int)(16.f * k);
        int avail = sh - bottom - min_y;
        int stack_h = na * act_h + (na - 1) * gap;
        if (stack_h > avail && gap > 8) {
            gap = 8;
            stack_h = na * act_h + (na - 1) * gap;
        }
        if (stack_h > avail) {
            int fit = (avail - (na - 1) * gap) / na;
            if (fit < 44) fit = 44;          /* docs/EASY.md rule 4 wins */
            if (fit < act_h) act_h = fit;
            stack_h = na * act_h + (na - 1) * gap;
        }
        int y = sh - bottom - stack_h;
        if (y < min_y) y = min_y;
        g->body_max_y = y - (int)(8.f * k);
        for (int i = 0; i < na; i++) {
            g->acts[i] = (rect){ g->gx, y, g->cw, act_h };
            g->act[i]  = which[i];
            y += act_h + gap;
        }
    } else {
        g->body_max_y = last_y - (int)(8.f * k);
        int w = want_w;
        if (w * na > room) w = room / (na > 0 ? na : 1);
        if (w < 44) w = 44;
        int x = g->gx;
        for (int i = 0; i < na; i++) {
            g->acts[i] = (rect){ x, last_y, w, act_h };
            g->act[i]  = which[i];
            x += w + gap;
        }
    }
}

int welcome_targets(const shell_ctx *c, int sw, int sh,
                    const welcome_view *v, rect *out, int max)
{
    welcome_geom g;
    welcome_layout(c, sw, sh, v, &g);
    int n = 0;
    for (int i = 0; i < g.n_acts && n < max; i++) out[n++] = g.acts[i];
    return n;
}

void welcome_set_page(int page)
{ if (page >= 0 && page < W_PAGE_N) { W.page = page; W.sel = 0; } }
int  welcome_page(void) { return W.page; }

/* ── what it says ────────────────────────────────────────────────── */

static const char *head_of(int page)
{
    switch (page) {
    case W_ASK:       return "AurOS is on this computer";
    case W_WORKING:   return "One moment";
    case W_CONFIRMED: return "AurOS starts from now on";
    case W_IMPORTING: return "Bringing your files across";
    case W_IMPORTED:  return "Your files are here";
    case W_DECLINED:  return "Windows will start next time";
    default:          return "That did not work";
    }
}

static const char *sub_of(int page)
{
    switch (page) {
    case W_ASK:
        return "Have a look around. Nothing has been decided yet.";
    case W_WORKING:
        return "Writing this down.";
    case W_CONFIRMED:
        return "Windows is still here, and still on the menu when you "
               "switch on.";
    case W_IMPORTING:
        return "Your Windows drive is only being read, never written to.";
    case W_IMPORTED:
        return "Open Files to see them.";
    case W_DECLINED:
        return "Turn this computer off and on again to go back.";
    default:
        return "Nothing has been changed.";
    }
}

/* Three short lines, not a paragraph. docs/EASY.md. */
static int body_of(int page, int can_import, const char *lines[4])
{
    int n = 0;
    switch (page) {
    case W_ASK:
        lines[n++] = "Right now, switching this computer on still starts "
                     "Windows.";
        lines[n++] = "That stays true until you press Yes.";
        lines[n++] = "This will ask again next time, so there is no hurry.";
        break;
    case W_CONFIRMED:
        lines[n++] = "Switching this computer on will start AurOS.";
        if (can_import)
            lines[n++] = "Your documents, pictures and settings are still "
                         "on the Windows part of the disk.";
        else
            lines[n++] = "Windows is still on this disk and can be chosen "
                         "from the menu when you switch on.";
        break;
    case W_IMPORTED:
        lines[n++] = "Anything that could not come across is written down "
                     "in the report.";
        break;
    case W_DECLINED:
        lines[n++] = "AurOS is still on this disk. Nothing has been deleted.";
        lines[n++] = "You can start it again from the menu when you switch "
                     "on.";
        break;
    default: break;
    }
    return n;
}

static void paint_action(surface *s, shell_fonts *f, const shell_ctx *c,
                         rect r, const char *label, int hot, int primary,
                         int grave)
{
    uint32_t face = primary ? c->accent : (hot ? c->surface_hi : c->surface_c);
    draw_rect(s, r, face, 1.f);
    if (!primary) draw_frame(s, r, 1, grave ? c->err : c->subtle,
                             hot ? 0.9f : 0.5f);
    font *ft = f->mid ? f->mid : f->small;
    if (ft)
        shell_text_centred(s, ft, (float)r.x + r.w / 2.f,
                           shell_baseline(ft, (float)r.y, (float)r.h),
                           label, primary ? c->bg : (grave ? c->err : c->fg),
                           1.f);
}

void welcome_paint(shell_ctx *c, surface *s, shell_fonts *f)
{
    if (!c->welcome_open) return;
    int sw = s->w, sh = c->screen_h;
    if (sh <= 0 || sh > s->h) sh = s->h;

    welcome_view v = view_now();
    welcome_geom g;
    welcome_layout(c, sw, sh, &v, &g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    draw_rect(s, (rect){ 0, 0, sw, sh }, c->bg, 0.97f);

    font *head  = f->huge ? f->huge : (f->big ? f->big : f->mid);
    font *bodyf = f->mid ? f->mid : f->small;
    font *tiny  = f->small ? f->small : bodyf;

    if (head)
        shell_text(s, head, (float)g.gx, (float)g.head_y, head_of(W.page),
                   c->fg, 1.f);
    draw_hrule(s, g.gx, g.rule_y, g.cw, 1, c->fg, 0.28f);
    if (bodyf)
        shell_text_elided(s, bodyf, (float)g.gx, (float)g.sub_y, (float)g.cw,
                          sub_of(W.page), c->fg, 0.9f);

    const char *lines[4];
    int nl = body_of(W.page, W.can_import, lines);
    int y = g.body_y;
    for (int i = 0; i < nl && tiny; i++) {
        if (y > g.body_max_y) break;    /* the answers took the room */
        shell_text_elided(s, tiny, (float)g.gx, (float)y, (float)g.cw,
                          lines[i], c->fg, 0.7f);
        y += (int)(26.f * k);
    }
    /* Whatever the thing doing the work last said, and whatever went
     * wrong. Both in her half of the screen rather than in a log. */
    if (y > g.body_max_y) y = g.body_max_y;
    if (W.page == W_IMPORTING && W.line[0] && tiny && y >= g.body_y)
        shell_text_elided(s, tiny, (float)g.gx, (float)y, (float)g.cw,
                          W.line, c->fg, 0.55f);
    if (W.page == W_TROUBLE && W.note[0] && tiny && y >= g.body_y)
        shell_text_elided(s, tiny, (float)g.gx, (float)y, (float)g.cw,
                          W.note, c->fg, 0.75f);

    for (int i = 0; i < g.n_acts; i++) {
        int id = g.act[i];
        paint_action(s, f, c, g.acts[i], ACT_LABEL[id],
                     W.hover_act == id || W.sel == i,
                     id == A_YES || id == A_IMPORT,
                     id == A_NO);
    }
}

/* ── pressing it ─────────────────────────────────────────────────── */

static int hit(rect r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static int do_action(shell_ctx *c, int id)
{
    switch (id) {
    case A_YES:
        if (ask_for("confirm") == 0) { W.page = W_WORKING; W.sel = 0; }
        else { W.page = W_TROUBLE;
               snprintf(W.note, sizeof W.note,
                        "AurOS could not reach the part of itself that "
                        "changes this."); }
        return 1;
    case A_NO:
        if (ask_for("decline") == 0) { W.page = W_WORKING; W.sel = 0; }
        else { W.page = W_TROUBLE;
               snprintf(W.note, sizeof W.note,
                        "AurOS could not reach the part of itself that "
                        "changes this."); }
        return 1;
    case A_IMPORT:
        W.log_seen = 0; W.line[0] = 0;
        if (ask_for("import") == 0) { W.page = W_IMPORTING; W.sel = 0; }
        else { W.page = W_TROUBLE;
               snprintf(W.note, sizeof W.note,
                        "AurOS could not start bringing your files across."); }
        return 1;
    case A_AGAIN:
        W.page = W_ASK; W.sel = 0; W.note[0] = 0;
        return 1;
    case A_LATER:
    case A_SKIP:
    case A_CLOSE:
        c->welcome_open = 0;
        return 1;
    }
    return 0;
}

int welcome_click(shell_ctx *c, int x, int y)
{
    if (!c->welcome_open) return 0;
    int sw = c->screen_w, sh = c->screen_h;
    welcome_view v = view_now();
    welcome_geom g;
    welcome_layout(c, sw, sh, &v, &g);
    for (int i = 0; i < g.n_acts; i++)
        if (hit(g.acts[i], x, y)) return do_action(c, g.act[i]);
    /* A PRESS ANYWHERE ELSE DOES NOTHING, and is still consumed. This
     * is a question, not a notification: pressing the background to
     * make it go away is exactly the reflex that would answer it by
     * accident. */
    return 1;
}

void welcome_motion(shell_ctx *c, int x, int y)
{
    if (!c->welcome_open) return;
    welcome_view v = view_now();
    welcome_geom g;
    welcome_layout(c, c->screen_w, c->screen_h, &v, &g);
    int was = W.hover_act;
    W.hover_act = -1;
    for (int i = 0; i < g.n_acts; i++)
        if (hit(g.acts[i], x, y)) { W.hover_act = g.act[i]; break; }
    (void)was;
}

int welcome_key(shell_ctx *c, int k)
{
    if (!c->welcome_open) return 0;
    welcome_view v = view_now();
    welcome_geom g;
    welcome_layout(c, c->screen_w, c->screen_h, &v, &g);
    if (!g.n_acts) return 1;            /* nothing to press yet */

    /* evdev: 105 left, 106 right, 103 up, 108 down, 28 enter, 57 space,
     * 1 escape. Arrows in both axes because the actions are a row on a
     * wide screen and a column on a narrow one, and she does not know
     * which. */
    switch (k) {
    case 105: case 103:
        W.sel = (W.sel + g.n_acts - 1) % g.n_acts; return 1;
    case 106: case 108:
        W.sel = (W.sel + 1) % g.n_acts; return 1;
    case 28: case 57:
        do_action(c, g.act[clampi_(W.sel, 0, g.n_acts - 1)]); return 1;
    case 1:
        /* ESCAPE IS "LET ME LOOK FIRST", NEVER AN ANSWER. On every
         * other panel in this shell escape closes it and nothing is
         * decided; the same key here must mean the same thing, or the
         * habit she learned everywhere else answers the one question
         * that matters. */
        c->welcome_open = 0;
        return 1;
    }
    return 1;                           /* the panel eats the rest */
}
