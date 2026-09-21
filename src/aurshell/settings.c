/* settings.c — see settings.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "settings.h"
#include "power.h"
#include "foot.h"
#include "draw.h"
#include "run.h"

/* ── what is on the main page ───────────────────────────────────────
 *
 * In the order she would look for them. Brightness first because it is
 * the one a person reaches for in a dark room at the moment they can
 * least afford to hunt. */
enum {
    R_SCREEN,      /* brightness              */
    R_SOUND,       /* volume                  */
    R_WORDS,       /* the text scale           */
    R_BATTERY,     /* a readout, not a control */
    R_BT,          /* opens the headphones panel */
    R_TIME,        /* opens a page             */
    R_SHELL,       /* opens a page             */
    R_LOOK,        /* opens a page             */
    R_N
};

/* Which of the top three a machine actually has. A desktop with no
 * backlight must not be shown a brightness slider that does nothing;
 * that is the whole of this product's recurring bug, stated as a
 * layout rule. */
static int row_applies(int row, const set_view *v)
{
    if (row == R_SCREEN)  return v->backlight;
    if (row == R_SOUND)   return v->sound;
    if (row == R_BATTERY) return v->battery;
    return 1;
}

int settings_rows_for(const set_view *v)
{
    if (v->page != SET_PAGE_MAIN) return v->n_rows;
    int n = 0;
    for (int r = 0; r < R_N; r++) if (row_applies(r, v)) n++;
    return n;
}

/* The row index on screen -> which R_* it is. */
static int row_at(const set_view *v, int i)
{
    if (v->page != SET_PAGE_MAIN) return i;
    for (int r = 0; r < R_N; r++) {
        if (!row_applies(r, v)) continue;
        if (i-- == 0) return r;
    }
    return -1;
}

/* ── what the three pages offer ─────────────────────────────────────
 *
 * Each is a list of choices with one of them current. Press one and it
 * takes effect; there is no Apply and no confirmation, because both of
 * those are a second thing to understand and every one of these is
 * undone by pressing a different row.
 */
#define MAX_ROWS 40
#define CHOICE_LEN 72

typedef struct {
    char id[CHOICE_LEN];     /* what to write down          */
    char label[CHOICE_LEN];  /* what she reads              */
    char note[CHOICE_LEN];   /* one line under it, or empty */
} choice;

/* The places most people are. Not every zone the world has -- there
 * are six hundred of those and she is looking for her own town, not
 * doing geography. The list is cities because "Europe/London" is a
 * path and "London" is a place. */
static const struct { const char *zone, *city, *where; } ZONES[] = {
    { "Europe/London",       "London",        "United Kingdom"  },
    { "Europe/Dublin",       "Dublin",        "Ireland"         },
    { "Europe/Paris",        "Paris",         "France"          },
    { "Europe/Berlin",       "Berlin",        "Germany"         },
    { "Europe/Madrid",       "Madrid",        "Spain"           },
    { "Europe/Rome",         "Rome",          "Italy"           },
    { "Europe/Amsterdam",    "Amsterdam",     "Netherlands"     },
    { "Europe/Lisbon",       "Lisbon",        "Portugal"        },
    { "Europe/Warsaw",       "Warsaw",        "Poland"          },
    { "Europe/Stockholm",    "Stockholm",     "Sweden"          },
    { "Europe/Athens",       "Athens",        "Greece"          },
    { "Europe/Kyiv",         "Kyiv",          "Ukraine"         },
    { "Europe/Moscow",       "Moscow",        "Russia"          },
    { "America/New_York",    "New York",      "eastern USA"     },
    { "America/Chicago",     "Chicago",       "central USA"     },
    { "America/Denver",      "Denver",        "mountain USA"    },
    { "America/Los_Angeles", "Los Angeles",   "western USA"     },
    { "America/Toronto",     "Toronto",       "Canada"          },
    { "America/Vancouver",   "Vancouver",     "Canada"          },
    { "America/Mexico_City", "Mexico City",   "Mexico"          },
    { "America/Sao_Paulo",   "Sao Paulo",     "Brazil"          },
    { "America/Bogota",      "Bogota",        "Colombia"        },
    { "Africa/Lagos",        "Lagos",         "Nigeria"         },
    { "Africa/Cairo",        "Cairo",         "Egypt"           },
    { "Africa/Johannesburg", "Johannesburg",  "South Africa"    },
    { "Africa/Nairobi",      "Nairobi",       "Kenya"           },
    { "Asia/Dubai",          "Dubai",         "UAE"             },
    { "Asia/Karachi",        "Karachi",       "Pakistan"        },
    { "Asia/Kolkata",        "Kolkata",       "India"           },
    { "Asia/Dhaka",          "Dhaka",         "Bangladesh"      },
    { "Asia/Bangkok",        "Bangkok",       "Thailand"        },
    { "Asia/Jakarta",        "Jakarta",       "Indonesia"       },
    { "Asia/Shanghai",       "Shanghai",      "China"           },
    { "Asia/Hong_Kong",      "Hong Kong",     ""                },
    { "Asia/Tokyo",          "Tokyo",         "Japan"           },
    { "Asia/Seoul",          "Seoul",         "South Korea"     },
    { "Asia/Manila",         "Manila",        "Philippines"     },
    { "Australia/Sydney",    "Sydney",        "Australia"       },
    { "Australia/Perth",     "Perth",         "Australia"       },
    { "Pacific/Auckland",    "Auckland",      "New Zealand"     },
};
#define N_ZONES ((int)(sizeof ZONES / sizeof ZONES[0]))

/* Room for every zone above, plus the one this machine is set to when
 * that is not one of them -- which on a shipped image is ALWAYS, since
 * they are built on UTC and nobody lives there.
 *
 * It was exactly N_ZONES. The prepended "what this computer uses now"
 * row therefore pushed the LAST city off the end -- silently, because
 * add_choice() simply returns when the table is full -- on every image
 * that has ever been built. Pacific/Auckland was not on the list and
 * nothing anywhere said so. Sized FROM the list now, so adding a city
 * cannot bring it back. */
#define CHOICE_MAX (N_ZONES + 8)


static struct {
    int page;
    int first_row;
    int sel;             /* the row under the pointer, -1 for none   */
    int drag_row;        /* the slider being dragged, -1 for none    */
    int hover_act;

    /* Read when the panel opens, so a slider starts where the machine
     * actually is rather than where we last left it. */
    power_battery bat;
    int backlight;
    int volume;
    int muted;

    /* The current page's choices, read when the page opens. */
    choice ch[CHOICE_MAX];
    int    n_ch;
    int    cur_ch;             /* which one is in force, -1 unknown */
    char   said[CHOICE_LEN];   /* one line of "this is now done"    */
    uint32_t reload_at;        /* when to reread, 0 for "not waiting" */
} S = { .sel = -1, .drag_row = -1, .hover_act = -1, .cur_ch = -1 };

int settings_dragging(void) { return S.drag_row >= 0; }

static set_view view_now(const shell_ctx *c)
{
    set_view v;
    memset(&v, 0, sizeof v);
    v.page      = S.page;
    v.first_row = S.first_row;
    v.battery   = S.bat.present;
    v.backlight = S.backlight >= 0;
    v.sound     = S.volume >= 0;
    v.n_rows    = S.n_ch;
    (void)c;
    return v;
}

/* ── filling a page ─────────────────────────────────────────────── */

static void add_choice(const char *id, const char *label, const char *note)
{
    if (S.n_ch >= CHOICE_MAX) return;
    snprintf(S.ch[S.n_ch].id,    CHOICE_LEN, "%s", id);
    snprintf(S.ch[S.n_ch].label, CHOICE_LEN, "%s", label);
    snprintf(S.ch[S.n_ch].note,  CHOICE_LEN, "%s", note ? note : "");
    S.n_ch++;
}

/* One field out of a .shell or .theme file: KEY="value". Small and
 * deliberate rather than a parser -- these files are ours. */
static int field_of(const char *path, const char *key, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) || line[klen] != '=') continue;
        char *v = line + klen + 1;
        if (*v == '"') v++;
        char *end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == '"')) *--end = 0;
        snprintf(out, n, "%s", v);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/* Where a symlink points, by basename. */
static void link_name(const char *path, char *out, size_t n)
{
    out[0] = 0;
    /* Deliberately no bigger than what it is copied into: these are
     * our own symlinks, and a path longer than this is not one of
     * ours. */
    char buf[CHOICE_LEN];
    ssize_t k = readlink(path, buf, sizeof buf - 1);
    if (k <= 0) return;
    buf[k] = 0;
    const char *b = strrchr(buf, '/');
    snprintf(out, n, "%s", b ? b + 1 : buf);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

/* Which theme is in force: hers if she has chosen one, otherwise the
 * machine's. Read from the file rather than remembered, because the
 * band's own reload can change it underneath us. */
static int current_theme_id(char *out, size_t n)
{
    out[0] = 0;
    const char *home = getenv("HOME");
    char path[512];
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) {
            if (!home || !*home) continue;
            snprintf(path, sizeof path, "%s/.config/auros/theme", home);
        } else {
            snprintf(path, sizeof path, "%s", "/etc/auros/theme");
        }
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(out, (int)n, f)) {
            size_t l = strlen(out);
            while (l && (out[l-1] == '\n' || out[l-1] == '\r' ||
                         out[l-1] == ' ')) out[--l] = 0;
        }
        fclose(f);
        if (out[0]) return 0;
    }
    return -1;
}

/* Where the archetypes and themes live. Overridable for the same two
 * reasons the battery's directory is: a harness has to be able to
 * render this against a tree that is not the running machine's, and a
 * technician has to be able to point a machine at a different set
 * without rebuilding it. */
static const char *dir_of(const char *var, const char *def)
{
    const char *v = getenv(var);
    return (v && *v) ? v : def;
}

static void fill_page(shell_ctx *c)
{
    S.n_ch = 0;
    S.cur_ch = -1;
    S.said[0] = 0;

    if (S.page == SET_PAGE_TIME) {
        char cur[CHOICE_LEN] = {0};
        link_name("/etc/localtime", cur, sizeof cur);   /* .../Europe/London */
        char full[CHOICE_LEN] = {0};
        char buf[512];
        ssize_t k = readlink("/etc/localtime", buf, sizeof buf - 1);
        if (k > 0) {
            buf[k] = 0;
            const char *z = strstr(buf, "zoneinfo/");
            if (z) snprintf(full, sizeof full, "%s", z + 9);
        }
        /* Whatever this machine is set to NOW goes first, even when it
         * is not one of the places below -- and the shipped images are
         * built on UTC, which is not a place anybody lives. A list of
         * forty cities with nothing marked on it tells her nothing
         * about where her computer thinks it is, which is the only
         * question she came here with. */
        int known = 0;
        for (int i = 0; i < N_ZONES; i++)
            if (full[0] && !strcmp(full, ZONES[i].zone)) known = 1;
        if (full[0] && !known) {
            const char *leaf = strrchr(full, '/');
            char label[CHOICE_LEN];
            snprintf(label, sizeof label, "%s", leaf ? leaf + 1 : full);
            for (char *q = label; *q; q++) if (*q == '_') *q = ' ';
            add_choice(full, label, "what this computer uses now");
            S.cur_ch = 0;
        }
        for (int i = 0; i < N_ZONES; i++) {
            add_choice(ZONES[i].zone, ZONES[i].city, ZONES[i].where);
            if (full[0] && !strcmp(full, ZONES[i].zone)) S.cur_ch = S.n_ch - 1;
        }
        (void)cur;
        return;
    }

    if (S.page == SET_PAGE_SHELL) {
        static const char *IDS[] = { "rail", "tiles", "locked", "taskbar",
                                     "dock", "workbench", NULL };
        char active[CHOICE_LEN] = {0};
        const char *home = getenv("HOME");
        char userlink[512] = {0};
        if (home && *home)
            snprintf(userlink, sizeof userlink, "%s/.config/auros/active.shell", home);
        if (userlink[0]) link_name(userlink, active, sizeof active);
        if (!active[0]) link_name("/etc/auros/shell/active.shell", active, sizeof active);
        if (!active[0]) snprintf(active, sizeof active, "%s", c->layout_id);

        for (int i = 0; IDS[i]; i++) {
            char path[256], name[CHOICE_LEN], plain[CHOICE_LEN];
            snprintf(path, sizeof path, "%s/%s.shell",
                     dir_of("AUROS_SHELLS", "/usr/share/auros/shells"), IDS[i]);
            if (field_of(path, "shell_name", name, sizeof name) < 0) continue;
            if (field_of(path, "shell_tagline", plain, sizeof plain) < 0) plain[0] = 0;
            add_choice(IDS[i], name, plain);
            if (!strcmp(active, IDS[i])) S.cur_ch = S.n_ch - 1;
        }
        return;
    }

    if (S.page == SET_PAGE_LOOK) {
        static const char *IDS[] = { "nocturne", "sandstone", "moss",
                                     "synthwave", NULL };
        char cur[CHOICE_LEN] = {0};
        current_theme_id(cur, sizeof cur);
        for (int i = 0; IDS[i]; i++) {
            char path[256], name[CHOICE_LEN], note[CHOICE_LEN];
            snprintf(path, sizeof path, "%s/%s.theme",
                     dir_of("AUROS_THEME_DIR", "/usr/share/auros/themes"), IDS[i]);
            if (field_of(path, "theme_name", name, sizeof name) < 0)
                snprintf(name, sizeof name, "%s", IDS[i]);
            if (field_of(path, "theme_description", note, sizeof note) < 0)
                note[0] = 0;
            add_choice(IDS[i], name, note);
            if (cur[0] && !strcmp(cur, IDS[i])) S.cur_ch = S.n_ch - 1;
        }
        return;
    }
}

void settings_opened(shell_ctx *c)
{
    (void)c;
    S.page = SET_PAGE_MAIN;
    S.first_row = 0;
    S.sel = -1;
    S.drag_row = -1;
    S.hover_act = -1;
    /* Ask the machine, rather than trusting what we last set: she may
     * have used the keys on the keyboard since, or the battery may
     * have moved on. */
    power_refresh();
    power_battery_read(&S.bat);
    S.backlight = power_brightness();
    S.volume    = power_volume();
    S.muted     = power_muted();
}

void settings_closed(shell_ctx *c) { (void)c; S.drag_row = -1; }

/* ── where everything is ────────────────────────────────────────────
 *
 * ONE function, called by painting AND hit-testing, taking a view
 * rather than reading the live state -- the same contract as the wifi
 * panel's, and for the same reason: tools/targets.c has to be able to
 * ask about a machine it is not.
 */
enum { A_CLOSE, A_BACK, A_MORE, A_N };
static const char *ACT_LABEL[A_N] = { "Close", "Go back", "Show more" };

/* Per row, in reading order: minus, the track, plus. A row that is a
 * readout has none; a row that opens a page is one big target. */
enum { T_MINUS, T_TRACK, T_PLUS, T_PER_ROW };

typedef struct {
    int  gx, cw;
    int  head_y, rule_y, sub_y;
    int  list_y, row_h, n_vis;
    rect rows[MAX_ROWS];          int n_rows;
    rect ctl[MAX_ROWS][T_PER_ROW];
    int  has_ctl[MAX_ROWS];
    rect acts[A_N]; int act[A_N]; int n_acts;
} set_geom;

static int clampi_(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int row_is_slider(const set_view *v, int which)
{
    if (v->page != SET_PAGE_MAIN) return 0;
    return which == R_SCREEN || which == R_SOUND || which == R_WORDS;
}

static void set_layout(const shell_ctx *c, int sw, int sh,
                       const set_view *v, set_geom *g)
{
    memset(g, 0, sizeof *g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    g->gx = clampi_(sw / 14, 20, 110);
    g->cw = sw - 2 * g->gx;
    if (g->cw < 200) { g->gx = 8; g->cw = sw - 16; }

    g->head_y = (int)(46.f * k);
    g->rule_y = g->head_y + (int)(12.f * k);
    g->sub_y  = g->rule_y + (int)(24.f * k);

    int act_h = (int)(34.f + 16.f * k); if (act_h < 44) act_h = 44;
    int act_w = (int)(100.f + 50.f * k);
    int gap   = (int)(10.f * k); if (gap < 8) gap = 8;
    int last_y = sh - (int)(14.f * k) - act_h;
    if (last_y < 0) last_y = 0;

    /* Taller than the wifi panel's rows, because these hold a control
     * rather than a name. The control is what sets the floor: a minus
     * and a plus at 44px with room to breathe. */
    /* Tall enough for a 44px control with room around it, and not a
     * pixel taller: every extra pixel here is a row she has to page to
     * reach, and a settings screen that pages is one she gives up on. */
    g->row_h = (int)(38.f + 28.f * k); if (g->row_h < 56) g->row_h = 56;
    g->list_y = g->sub_y + (int)(14.f * k);

    int space = last_y - (int)(14.f * k) - g->list_y;
    g->n_vis = space / g->row_h;
    if (g->n_vis < 1) g->n_vis = 1;
    if (g->n_vis > MAX_ROWS) g->n_vis = MAX_ROWS;

    int total = settings_rows_for(v);
    int from = clampi_(v->first_row, 0, total > 0 ? total - 1 : 0);
    for (int i = 0; i < g->n_vis && from + i < total; i++) {
        rect r = { g->gx, g->list_y + i * g->row_h, g->cw, g->row_h };
        g->rows[g->n_rows] = r;

        int which = row_at(v, from + i);
        if (row_is_slider(v, which)) {
            /* Reading order, left to right:
             *
             *   name .............  45%  [-] [==== track ====] [+]
             *
             * The number sits BEFORE the minus, not after the plus.
             * The first version put it after, where it was painted
             * straight through the plus button -- which is both
             * illegible and a press she would aim at the number and
             * land on the control. */
            int bs = (int)(44.f + 10.f * k);          /* the +/- boxes */
            if (bs < 44) bs = 44;
            int gap = (int)(10.f * k); if (gap < 8) gap = 8;
            int cy = r.y + (r.h - bs) / 2;
            int right = r.x + r.w;

            rect plus = { right - bs, cy, bs, bs };
            int tw = (int)(230.f * k);
            /* The track gives way before the buttons do: they have a
             * 44px floor and it does not. */
            int most = plus.x - gap - (r.x + (int)(120.f * k)) - gap - bs;
            if (tw > most) tw = most;
            if (tw < 24) tw = 24;
            rect track = { plus.x - gap - tw, cy, tw, bs };
            rect minus = { track.x - gap - bs, cy, bs, bs };
            if (minus.x < r.x) {            /* a very narrow panel */
                minus.x = r.x;
                track.x = minus.x + bs + gap;
                track.w = plus.x - gap - track.x;
                if (track.w < 8) track.w = 8;
            }
            g->ctl[g->n_rows][T_MINUS] = minus;
            g->ctl[g->n_rows][T_TRACK] = track;
            g->ctl[g->n_rows][T_PLUS]  = plus;
            g->has_ctl[g->n_rows] = 1;
        }
        g->n_rows++;
    }

    int acts_y = g->list_y + g->n_rows * g->row_h + (int)(16.f * k);
    if (acts_y > last_y) acts_y = last_y;
    if (acts_y < g->sub_y + (int)(20.f * k)) acts_y = g->sub_y + (int)(20.f * k);

    int which[A_N], na = 0;
    if (v->page != SET_PAGE_MAIN) which[na++] = A_BACK;
    if (total > g->n_vis) which[na++] = A_MORE;
    which[na++] = A_CLOSE;
    if (na > 0) {
        int room = g->cw - (na - 1) * gap;
        if (act_w * na > room) act_w = room / na;
        if (act_w < 44) act_w = 44;
    }
    int ax = g->gx;
    for (int i = 0; i < na; i++) {
        g->acts[i] = (rect){ ax, acts_y, act_w, act_h };
        g->act[i]  = which[i];
        ax += act_w + gap;
    }
    g->n_acts = na;
}

/* ── what each row says ─────────────────────────────────────────── */

static const char *row_name(int which)
{
    switch (which) {
    case R_SCREEN:  return "How bright the screen is";
    case R_SOUND:   return "How loud the sound is";
    case R_WORDS:   return "How big the words are";
    case R_BATTERY: return "Battery";
    case R_BT:      return "Headphones and mice";
    case R_TIME:    return "The time and date";
    case R_SHELL:   return "How this computer works";
    case R_LOOK:    return "How it looks";
    }
    return "";
}

/* The right-hand side of a row that is not a slider. */
static void row_value(const shell_ctx *c, int which, char *out, size_t n)
{
    out[0] = 0;
    switch (which) {
    case R_BATTERY:
        if (!S.bat.present) { snprintf(out, n, "%s", "None in this computer"); break; }
        if (S.bat.percent < 0) { snprintf(out, n, "%s", "Cannot tell"); break; }
        if (S.bat.charging)
            snprintf(out, n, "%d%% and filling up", S.bat.percent);
        else if (S.bat.plugged)
            snprintf(out, n, "%d%%, plugged in", S.bat.percent);
        else if (S.bat.minutes > 90)
            snprintf(out, n, "%d%%, about %d hours left",
                     S.bat.percent, (S.bat.minutes + 30) / 60);
        else if (S.bat.minutes > 0)
            snprintf(out, n, "%d%%, about %d minutes left",
                     S.bat.percent, S.bat.minutes);
        else
            snprintf(out, n, "%d%%", S.bat.percent);
        break;
    case R_BT:
        snprintf(out, n, "%s", "Connect one without a cable");
        break;
    case R_TIME: {
        time_t t = time(NULL);
        struct tm tm_;
        if (localtime_r(&t, &tm_)) strftime(out, n, "%H:%M on %A", &tm_);
        break;
    }
    case R_SHELL:
        snprintf(out, n, "%s", c->shell_name[0] ? c->shell_name : c->layout_id);
        break;
    case R_LOOK: {
        /* The theme she is using, by its own name -- not the brand,
         * which is the same on every theme and told her nothing. */
        char id[CHOICE_LEN] = {0};
        if (current_theme_id(id, sizeof id) == 0) {
            char path[256];
            snprintf(path, sizeof path, "%s/%s.theme",
                     dir_of("AUROS_THEME_DIR", "/usr/share/auros/themes"), id);
            if (field_of(path, "theme_name", out, n) < 0)
                snprintf(out, n, "%s", id);
        } else {
            snprintf(out, n, "%s", c->brand[0] ? c->brand : "");
        }
        break;
    }
    }
}

static int slider_value(const shell_ctx *c, int which)
{
    if (which == R_SCREEN) return S.backlight;
    if (which == R_SOUND)  return S.muted ? 0 : S.volume;
    if (which == R_WORDS) {
        /* Her text scale, shown as a percentage of the size it came
         * at, which is the only frame of reference she has. */
        float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;
        return (int)((k - 0.80f) / (2.00f - 0.80f) * 100.f + 0.5f);
    }
    return 0;
}

/* ── painting ───────────────────────────────────────────────────── */

static void paint_action(surface *s, shell_fonts *f, const shell_ctx *c,
                         rect r, const char *label, int hot, int primary)
{
    draw_rect(s, r, primary ? c->fg : (hot ? c->surface_hi : c->surface_c), 1.f);
    if (!primary) draw_frame(s, r, 1, c->subtle, hot ? 0.9f : 0.5f);
    font *ft = f->mid ? f->mid : f->small;
    if (!ft) return;
    shell_text_centred(s, ft, (float)r.x + r.w / 2.f,
                       shell_baseline(ft, (float)r.y, (float)r.h),
                       label, primary ? c->bg : c->fg, 1.f);
}

static void paint_step(surface *s, shell_fonts *f, const shell_ctx *c,
                       rect r, const char *glyph, int hot)
{
    draw_rect(s, r, hot ? c->surface_hi : c->surface_c, 1.f);
    draw_frame(s, r, 1, c->subtle, 0.6f);
    font *ft = f->mid ? f->mid : f->small;
    if (ft)
        shell_text_centred(s, ft, (float)r.x + r.w / 2.f,
                           shell_baseline(ft, (float)r.y, (float)r.h),
                           glyph, c->fg, 1.f);
}

void settings_paint(shell_ctx *c, surface *s, shell_fonts *f)
{
    if (!c->settings_open) return;
    int sw = s->w, sh = c->screen_h;
    if (sh <= 0 || sh > s->h) sh = s->h;

    set_view v = view_now(c);
    set_geom g;
    set_layout(c, sw, sh, &v, &g);
    float k = (c->text_scale > 0.1f) ? c->text_scale : 1.f;

    draw_rect(s, (rect){ 0, 0, sw, sh }, c->bg, 0.97f);

    font *head = f->huge ? f->huge : (f->big ? f->big : f->mid);
    font *name = f->mid ? f->mid : f->small;
    font *bodyf = f->small ? f->small : f->mid;
    font *tiny = f->tiny ? f->tiny : bodyf;

    const char *title = S.page == SET_PAGE_TIME  ? "Where you are"
                      : S.page == SET_PAGE_SHELL ? "How this works"
                      : S.page == SET_PAGE_LOOK  ? "How it looks"
                                                 : "Settings";
    if (head)
        shell_text(s, head, (float)g.gx, (float)g.head_y, title, c->fg, 1.f);
    draw_hrule(s, g.gx, g.rule_y, g.cw, 1, c->fg, 0.28f);

    const char *sub = S.said[0] ? S.said
        : S.page == SET_PAGE_TIME  ? "Press the place closest to you."
        : S.page == SET_PAGE_SHELL ? "Press one. The desktop changes straight away."
        : S.page == SET_PAGE_LOOK  ? "Press one to try it."
                                   : "Anything you change here you can change back.";
    if (bodyf)
        shell_text_elided(s, bodyf, (float)g.gx, (float)g.sub_y, (float)g.cw,
                          sub, c->fg, 0.85f);

    int total = settings_rows_for(&v);
    int from = clampi_(S.first_row, 0, total > 0 ? total - 1 : 0);
    for (int i = 0; i < g.n_rows; i++) {
        rect r = g.rows[i];
        int which = row_at(&v, from + i);
        if (S.sel == from + i) draw_rect(s, r, c->surface_hi, 1.f);
        draw_hrule(s, r.x, r.y + r.h - 1, r.w, 1, c->fg, 0.12f);

        float by = shell_baseline(name, (float)r.y, (float)r.h);

        /* A page of choices: the name, one line about it, and a mark
         * on the one that is in force. */
        if (v.page != SET_PAGE_MAIN) {
            int idx = from + i;
            if (idx < 0 || idx >= S.n_ch) continue;
            const char *mark = (idx == S.cur_ch) ? "Now" : NULL;
            float mw = (mark && tiny) ? shell_text_w(tiny, mark) : 0.f;
            if (mark && tiny)
                shell_text(s, tiny, (float)(r.x + r.w) - mw, by, mark,
                           c->accent, 1.f);
            if (name)
                shell_text_elided(s, name, (float)r.x + 4.f,
                                  by - (S.ch[idx].note[0] ? 9.f * k : 0.f),
                                  (float)r.w - mw - 24.f,
                                  S.ch[idx].label, c->fg, 1.f);
            if (tiny && S.ch[idx].note[0])
                shell_text_elided(s, tiny, (float)r.x + 4.f,
                                  by + (float)(15.f * k),
                                  (float)r.w - mw - 24.f,
                                  S.ch[idx].note, c->fg, 0.55f);
            continue;
        }

        if (name)
            shell_text_elided(s, name, (float)r.x + 4.f, by,
                              (float)(g.has_ctl[i]
                                      ? g.ctl[i][T_MINUS].x - r.x
                                        - (g.ctl[i][T_MINUS].x - r.x
                                           > r.w * 2 / 5 ? 64 : 12)
                                      : r.w * 3 / 5),
                              row_name(which), c->fg, 1.f);

        if (g.has_ctl[i]) {
            int val = slider_value(c, which);
            paint_step(s, f, c, g.ctl[i][T_MINUS], "−",
                       S.hover_act == -2 - (i * T_PER_ROW + T_MINUS));
            paint_step(s, f, c, g.ctl[i][T_PLUS], "+",
                       S.hover_act == -2 - (i * T_PER_ROW + T_PLUS));

            rect t = g.ctl[i][T_TRACK];
            int gh = (int)(12.f * k); if (gh < 10) gh = 10;
            rect groove = { t.x, t.y + (t.h - gh) / 2, t.w, gh };
            draw_rect(s, groove, c->fg, 0.18f);
            rect fill = groove;
            fill.w = groove.w * clampi_(val, 0, 100) / 100;
            if (fill.w > 0) draw_rect(s, fill, c->accent, 1.f);
            /* A handle, so it reads as something you can take hold of
             * rather than a progress bar that is happening to you. */
            int hx = groove.x + fill.w;
            rect grip = { hx - gh / 2, t.y, gh, t.h };
            if (grip.x < t.x) grip.x = t.x;
            if (grip.x + grip.w > t.x + t.w) grip.x = t.x + t.w - grip.w;
            draw_rect(s, grip, c->fg, 1.f);

            /* The number, unless printing it would squeeze the name
             * down to nothing. At the largest text on the smallest
             * panel "How bright the screen is" became "How brigh...",
             * and a row she cannot read is worse than a bar without a
             * figure on it -- the bar already says roughly where she
             * is, and the name says what of. */
            int name_room = g.ctl[i][T_MINUS].x - r.x;
            if (tiny && name_room > r.w * 2 / 5) {
                char n[8];
                snprintf(n, sizeof n, "%d%%", clampi_(val, 0, 100));
                float nw = shell_text_w(tiny, n);
                shell_text(s, tiny,
                           (float)g.ctl[i][T_MINUS].x - nw - 12.f,
                           by, n, c->fg, 0.6f);
            }
        } else {
            char val[96];
            row_value(c, which, val, sizeof val);
            if (tiny && val[0]) {
                float w = shell_text_w(tiny, val);
                shell_text_elided(s, tiny, (float)(r.x + r.w) - w - 4.f, by,
                                  (float)r.w / 2.f, val, c->fg, 0.6f);
            }
        }
    }

    if (total > g.n_rows && tiny) {
        char more[48];
        snprintf(more, sizeof more, "%d of %d", from + g.n_rows, total);
        float w = shell_text_w(tiny, more);
        shell_text(s, tiny, (float)(g.gx + g.cw) - w, (float)g.sub_y,
                   more, c->fg, 0.5f);
    }

    for (int i = 0; i < g.n_acts; i++)
        paint_action(s, f, c, g.acts[i], ACT_LABEL[g.act[i]],
                     S.hover_act == g.act[i], 0);
}

/* ── input ──────────────────────────────────────────────────────── */

static int in_rect(rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Everything that changes a value goes through here, so that the
 * keyboard, the buttons and the track cannot drift apart. */
static void apply(shell_ctx *c, int which, int val)
{
    val = clampi_(val, 0, 100);
    switch (which) {
    case R_SCREEN: {
        int got = power_brightness_set(val);
        if (got >= 0) S.backlight = got;
        break;
    }
    case R_SOUND:
        power_volume_set(val);
        S.volume = val;
        if (val > 0) S.muted = 0;
        break;
    case R_WORDS: {
        float k = 0.80f + (2.00f - 0.80f) * (float)val / 100.f;
        /* Through the band's own saved setting, so the two controls
         * for the same thing cannot disagree or forget each other. */
        c->text_scale = k;
        c->text_changed = 1;
        foot_save_text_scale(k);
        break;
    }
    }
}

/* ── making a choice happen ─────────────────────────────────────────
 *
 * Every one of these writes into HER config directory, never into
 * /etc. /etc/auros belongs to whoever set the computer up, and
 * policy.conf lives there: a user who could write that directory could
 * lift their own lock-down. The one exception is the timezone, which
 * is genuinely the machine's and goes through the permission service.
 */
static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static int user_path(char *out, size_t n, const char *leaf)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    char dir[448];
    snprintf(dir, sizeof dir, "%s/.config", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.config/auros", home);
    mkdir(dir, 0755);
    snprintf(out, n, "%s/%s", dir, leaf);
    return 0;
}

static void choose(shell_ctx *c, int idx)
{
    if (idx < 0 || idx >= S.n_ch) return;
    const char *id = S.ch[idx].id;

    /* WAITING FOR THE ANSWER, NOT FOR THE FORK.
     *
     * Both of these used to say "done" if a process had been created.
     * run_detached() returns 0 the instant fork() succeeds, so a
     * timedatectl that policy refused and an aurora that could not
     * write the file both reported success -- which means the only two
     * pages in this panel that change anything were incapable of
     * saying that they had not. */
    if (S.page == SET_PAGE_TIME) {
        const char *argv[] = { "timedatectl", "set-timezone", id, NULL };
        int rc = run_status(argv, 2500);
        if (rc == 0) {
            S.cur_ch = idx;
            snprintf(S.said, sizeof S.said, "The clock is now set for %s.",
                     S.ch[idx].label);
        } else if (rc < 0) {
            snprintf(S.said, sizeof S.said, "%s",
                     "This is taking a while. Check the clock in a moment.");
        } else {
            snprintf(S.said, sizeof S.said, "%s",
                     "This computer would not let the clock be changed.");
        }
        return;
    }

    if (S.page == SET_PAGE_SHELL) {
        if (!c->allow_settings) return;
        char link[512], target[256];
        if (user_path(link, sizeof link, "active.shell") < 0) return;
        snprintf(target, sizeof target, "%s/%s.shell",
                 dir_of("AUROS_SHELLS", "/usr/share/auros/shells"), id);
        unlink(link);
        if (symlink(target, link) < 0) {
            snprintf(S.said, sizeof S.said, "%s",
                     "That could not be saved. Nothing has changed.");
            return;
        }
        S.cur_ch = idx;
        /* The shell rereads everything on SIGHUP, which is the same
         * path a theme change takes. Sending it to ourselves is how
         * the change happens without restarting anything. */
        c->want_reload = 1;
        snprintf(S.said, sizeof S.said, "This computer now works like %s.",
                 S.ch[idx].label);
        return;
    }

    if (S.page == SET_PAGE_LOOK) {
        if (!c->allow_theme_change) return;
        char statef[512], conf[512], cache[512];
        if (user_path(statef, sizeof statef, "theme") < 0) return;
        if (user_path(conf,   sizeof conf,   "shell.conf") < 0) return;
        if (user_path(cache,  sizeof cache,  "cache") < 0) return;

        FILE *f = fopen(statef, "w");
        if (!f) {
            snprintf(S.said, sizeof S.said, "%s",
                     "That could not be saved. Nothing has changed.");
            return;
        }
        fprintf(f, "%s\n", id);
        fclose(f);

        /* aurora renders a theme into a shell.conf, and every template
         * names an absolute path under /etc/auros -- which is root's,
         * and must stay root's, because policy.conf lives beside them
         * and a user who could write it could lift their own
         * lock-down.
         *
         * AURORA_STATE only moves aurora's note of which theme is
         * current. It does NOT move the rendered files, so this used
         * to set that one variable, render into a directory she cannot
         * write, and report "Now using Sandstone." while nothing
         * whatsoever had changed. AURORA_OUTDIR is what moves the
         * output; AURORA_ONLY renders the one file of eleven that the
         * desktop reads, because she is watching the screen while it
         * happens. */
        char dir[512];
        user_path(dir, sizeof dir, "");
        size_t dl = strlen(dir);
        if (dl && dir[dl-1] == '/') dir[dl-1] = 0;
        setenv("AURORA_STATE",  dir, 1);
        setenv("AURORA_OUTDIR", dir, 1);
        setenv("AURORA_ONLY",   "shell.conf", 1);
        setenv("AURORA_CACHE", cache, 1);
        const char *argv[] = { "aurora", "set", id, NULL };
        int rc = run_status(argv, 2500);
        if (rc > 0) {
            snprintf(S.said, sizeof S.said, "%s",
                     "The look could not be changed just now.");
            return;
        }
        S.cur_ch = idx;
        if (rc == 0) {
            /* It has finished and the file is on disk, so the reload
             * can happen on this pass rather than on a timer. */
            c->want_reload = 1;
        } else {
            /* Still running after two and a half seconds. Give it a
             * moment more and reload anyway: the file may well appear,
             * and a reload of an unchanged file costs one repaint. */
            S.reload_at = now_ms() + 700;
        }
        snprintf(S.said, sizeof S.said, "Now using %s.", S.ch[idx].label);
        return;
    }
}

static int step_of(int which)
{
    /* The words move in the band's steps, so pressing + here and
     * Bigger down there feel like the same control. */
    return which == R_WORDS ? 12 : 5;
}

void settings_motion(shell_ctx *c, int x, int y)
{
    if (!c->settings_open) { S.hover_act = -1; return; }

    set_view v = view_now(c);
    set_geom g;
    set_layout(c, c->screen_w, c->screen_h, &v, &g);
    int from = clampi_(S.first_row, 0, 0x7fffffff);

    /* A drag in progress owns the pointer until the button comes up. */
    if (S.drag_row >= 0) {
        if (!c->mouse_down) { S.drag_row = -1; return; }
        for (int i = 0; i < g.n_rows; i++) {
            if (from + i != S.drag_row || !g.has_ctl[i]) continue;
            rect t = g.ctl[i][T_TRACK];
            int val = t.w > 0 ? (x - t.x) * 100 / t.w : 0;
            apply(c, row_at(&v, from + i), val);
            return;
        }
        return;
    }

    S.hover_act = -1;
    S.sel = -1;
    for (int i = 0; i < g.n_acts; i++)
        if (in_rect(g.acts[i], x, y)) { S.hover_act = g.act[i]; return; }
    for (int i = 0; i < g.n_rows; i++) {
        if (g.has_ctl[i]) {
            for (int t = 0; t < T_PER_ROW; t++)
                if (t != T_TRACK && in_rect(g.ctl[i][t], x, y)) {
                    S.hover_act = -2 - (i * T_PER_ROW + t);
                    S.sel = from + i;
                    return;
                }
        }
        if (in_rect(g.rows[i], x, y)) { S.sel = from + i; return; }
    }
}

int settings_click(shell_ctx *c, int x, int y)
{
    if (!c->settings_open) return 0;
    if (y >= c->screen_h) return 0;             /* the band's */

    set_view v = view_now(c);
    set_geom g;
    set_layout(c, c->screen_w, c->screen_h, &v, &g);
    int total = settings_rows_for(&v);
    int from = clampi_(S.first_row, 0, total > 0 ? total - 1 : 0);

    for (int i = 0; i < g.n_acts; i++) {
        if (!in_rect(g.acts[i], x, y)) continue;
        switch (g.act[i]) {
        case A_CLOSE: c->settings_open = 0; break;
        case A_BACK:  S.page = SET_PAGE_MAIN; S.first_row = 0;
                      S.sel = -1; S.n_ch = 0; S.said[0] = 0; break;
        case A_MORE:
            S.first_row += g.n_vis;
            if (S.first_row >= total) S.first_row = 0;
            break;
        }
        return 1;
    }

    for (int i = 0; i < g.n_rows; i++) {
        int which = row_at(&v, from + i);
        if (g.has_ctl[i]) {
            if (in_rect(g.ctl[i][T_MINUS], x, y)) {
                apply(c, which, slider_value(c, which) - step_of(which));
                return 1;
            }
            if (in_rect(g.ctl[i][T_PLUS], x, y)) {
                apply(c, which, slider_value(c, which) + step_of(which));
                return 1;
            }
            if (in_rect(g.ctl[i][T_TRACK], x, y)) {
                /* One press anywhere on the track goes there. Dragging
                 * from that press also works and is never required --
                 * a control that can only be dragged is a control for
                 * steady hands. */
                rect t = g.ctl[i][T_TRACK];
                apply(c, which, t.w > 0 ? (x - t.x) * 100 / t.w : 0);
                S.drag_row = from + i;
                return 1;
            }
        }
        if (in_rect(g.rows[i], x, y)) {
            if (v.page != SET_PAGE_MAIN) { choose(c, from + i); return 1; }
            /* Bluetooth is its own panel rather than a page here.
             * It is the same act as joining a wifi -- find a thing,
             * press it, wait -- so it is the same panel shape, and two
             * screens that do the same kind of job must not behave
             * differently. Settings is the index; the panel is the
             * thing. */
            if (which == R_BT) foot_open_only(c, &c->bt_open);
            if (which == R_TIME)  { S.page = SET_PAGE_TIME;  S.first_row = 0;
                                    S.sel = -1; fill_page(c); }
            if (which == R_SHELL) { S.page = SET_PAGE_SHELL; S.first_row = 0;
                                    S.sel = -1; fill_page(c); }
            if (which == R_LOOK)  { S.page = SET_PAGE_LOOK;  S.first_row = 0;
                                    S.sel = -1; fill_page(c); }
            return 1;
        }
    }
    return 1;      /* the panel swallows everything inside it */
}

int settings_key(shell_ctx *c, int k)
{
    if (!c->settings_open) return 0;

    if (k == 1) {                                   /* Escape */
        if (S.page != SET_PAGE_MAIN) {
            S.page = SET_PAGE_MAIN; S.first_row = 0;
            S.sel = -1; S.n_ch = 0; S.said[0] = 0;
        } else c->settings_open = 0;
        return 1;
    }

    set_view v = view_now(c);
    int total = settings_rows_for(&v);
    if (total <= 0) return 1;

    if (k == 103 || k == 108) {                     /* up / down */
        if (S.sel < 0) S.sel = S.first_row;
        else S.sel += (k == 108) ? 1 : -1;
        S.sel = clampi_(S.sel, 0, total - 1);
        set_geom g;
        set_layout(c, c->screen_w, c->screen_h, &v, &g);
        if (S.sel < S.first_row) S.first_row = S.sel;
        else if (g.n_vis > 0 && S.sel >= S.first_row + g.n_vis)
            S.first_row = S.sel - g.n_vis + 1;
        return 1;
    }
    if (k == 28 && S.sel >= 0 && S.page != SET_PAGE_MAIN) {   /* Enter */
        choose(c, S.sel);
        return 1;
    }
    if (k == 28 && S.sel >= 0) {                              /* Enter */
        int which = row_at(&v, S.sel);
        if (which == R_BT) foot_open_only(c, &c->bt_open);
        if (which == R_TIME)  { S.page = SET_PAGE_TIME;  S.first_row = 0;
                                S.sel = -1; fill_page(c); }
        if (which == R_SHELL) { S.page = SET_PAGE_SHELL; S.first_row = 0;
                                S.sel = -1; fill_page(c); }
        if (which == R_LOOK)  { S.page = SET_PAGE_LOOK;  S.first_row = 0;
                                S.sel = -1; fill_page(c); }
        return 1;
    }
    if ((k == 105 || k == 106) && S.sel >= 0) {     /* left / right */
        int which = row_at(&v, S.sel);
        if (row_is_slider(&v, which)) {
            int d = (k == 106) ? step_of(which) : -step_of(which);
            apply(c, which, slider_value(c, which) + d);
        }
        return 1;
    }
    return 1;      /* the panel owns the keyboard while it is up */
}

/* Called once per pass of the main loop. Returns 1 if the screen has
 * something new to show. This is where a change that could not take
 * effect immediately -- a theme still being written to disk -- becomes
 * a reload, rather than in painting, which must not have consequences. */
int settings_step(shell_ctx *c)
{
    if (!S.reload_at) return 0;
    if ((int32_t)(now_ms() - S.reload_at) < 0) return 0;
    S.reload_at = 0;
    c->want_reload = 1;
    return 1;
}

/* ── what a harness can measure ─────────────────────────────────── */

int settings_targets(const shell_ctx *c, int sw, int sh, const set_view *v,
                     rect *out, int max)
{
    set_geom g;
    set_layout(c, sw, sh, v, &g);
    int n = 0;
    for (int i = 0; i < g.n_rows && n < max; i++) {
        if (g.has_ctl[i]) {
            for (int t = 0; t < T_PER_ROW && n < max; t++)
                out[n++] = g.ctl[i][t];
        } else if (n < max) {
            out[n++] = g.rows[i];
        }
    }
    for (int i = 0; i < g.n_acts && n < max; i++) out[n++] = g.acts[i];
    return n;
}
