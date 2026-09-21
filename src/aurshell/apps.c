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
#include <string.h>
#include <unistd.h>
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

static int have_binary(const char *cmd)
{
    if (!*cmd) return 0;
    char first[256] = {0};
    /* Only the program name, and only if it is not quoted oddly; a
     * TryExec we cannot parse is treated as "no opinion" rather than as
     * a reason to hide a working application. */
    size_t i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < sizeof first - 1) { first[i] = cmd[i]; i++; }
    if (first[0] == '/') return access(first, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char buf[512];
    while (*path) {
        const char *sep = strchr(path, ':');
        size_t n = sep ? (size_t)(sep - path) : strlen(path);
        if (n && n < sizeof buf - strlen(first) - 2) {
            memcpy(buf, path, n); buf[n] = '/';
            snprintf(buf + n + 1, sizeof buf - n - 1, "%s", first);
            if (access(buf, X_OK) == 0) return 1;
        }
        if (!sep) break;
        path = sep + 1;
    }
    return 0;
}

static int read_entry(const char *path, entry *e, const char *lang, const char *ll)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    memset(e, 0, sizeof *e);
    e->name_score = 0;

    char line[1024];
    int in_group = 0, comment_score = 0;
    char try_exec[192] = {0};

    while (fgets(line, sizeof line, fp)) {
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
    if (try_exec[0] && !have_binary(try_exec)) return -1;
    if (!have_binary(e->exec)) return -1;
    return 0;
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

typedef struct { entry e; char file[128]; int rank; } found;

static int cmp_found(const void *a, const void *b)
{
    const found *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    return strcasecmp(x->e.name, y->e.name);
}

/* A kiosk's allow-list is an allow-list. It is checked here, where the
 * table is built, rather than at launch time, because an icon the user
 * can see and press and that then refuses is worse than no icon: it
 * teaches a room full of teenagers exactly where the edges are. */
static int allowed(const shell_ctx *c, const char *stem, const entry *e)
{
    if (c->deny_all_apps) return 0;
    if (!c->allowed_apps[0]) return 1;
    const char *p = c->allowed_apps;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != ',') p++;
        size_t n = (size_t)(p - s);
        if (!n) continue;
        if (!strncasecmp(stem, s, n) && strlen(stem) == n) return 1;
        if (e->wmclass[0] && !strncasecmp(e->wmclass, s, n) && strlen(e->wmclass) == n) return 1;
        if (!strncasecmp(e->exec, s, n) && (e->exec[n] == 0 || e->exec[n] == ' ')) return 1;
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
    if (home_dir[0]) dirs[n_dirs++] = home_dir;

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

            char stem[128];
            snprintf(stem, sizeof stem, "%.*s", (int)(len - 8), de->d_name);
            if (!allowed(c, stem, &e)) continue;

            /* Later directory wins, which is the whole reason the user
             * directory is searched last. */
            int slot = -1;
            for (int i = 0; i < n; i++) if (!strcmp(list[i].file, stem)) { slot = i; break; }
            if (slot < 0) { slot = n++; snprintf(list[slot].file, sizeof list[slot].file, "%s", stem); }
            list[slot].e = e;
            list[slot].rank = rank_of(&e);
        }
        closedir(dp);
    }

    if (!any_dir) { free(list); return -1; }
    qsort(list, (size_t)n, sizeof *list, cmp_found);

    uint32_t tints[3] = { c->accent, c->accent_alt, c->accent_warm };
    int keep = n < SHELL_MAX_APPS ? n : SHELL_MAX_APPS;
    for (int i = 0; i < keep; i++) {
        app_entry *a = &c->apps[i];
        memset(a, 0, sizeof *a);
        snprintf(a->id,   sizeof a->id,   "%s", list[i].file);
        snprintf(a->name, sizeof a->name, "%s", list[i].e.name);
        snprintf(a->hint, sizeof a->hint, "%s", list[i].e.comment);
        snprintf(a->exec, sizeof a->exec, "%s", list[i].e.exec);
        /* When an application does not declare its window class, the
         * desktop file's own name is what GTK and Qt report as app_id
         * -- which is how a window that appears gets matched back to
         * the icon that started it. */
        snprintf(a->wm_class, sizeof a->wm_class, "%s",
                 list[i].e.wmclass[0] ? list[i].e.wmclass : list[i].file);
        a->icon   = icon_for(&list[i].e);
        a->tint   = tints[i % 3];
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
        a->tint = c->subtle;
        c->n_apps = ++keep;
    }
    free(list);
    return keep;
}
