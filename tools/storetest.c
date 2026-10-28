/* storetest.c — can she get to "Get more programs" from where she lands?
 *
 * The journey audit of 2026-10-23 (F4) found that on the default
 * desktop she could not: rail's Home showed six programs and the store
 * ranked seventh. Nothing failed, because nothing asked. This asks, the
 * way she would, in every archetype a person can choose (locked is a
 * kiosk and has no store by policy):
 *
 *   1. A machine is made out of a folder of .desktop files -- the
 *      programs this image ships, renamed as build/forge renames them,
 *      plus the store -- and, separately, the same machine after she
 *      has installed forty more. The second is the case that broke:
 *      the more she installs, the further down a ranked list the store
 *      falls.
 *   2. Each archetype's home is painted at four panel sizes and two
 *      text sizes, with the shell's text log on, so the test knows
 *      every word on the screen and where it is.
 *   3. The words "Get more programs", exactly -- a label cut to "Get
 *      more prog..." does not count -- are pressed. If they are not on
 *      the screen, every word that IS on the screen is pressed in turn
 *      and the screen it leads to is searched. The store must START
 *      (the shell asks for its command line) within two presses.
 *
 * Then a program is installed while the desktop is running -- a
 * Flathub entry, which is a symlink, appearing in a directory that did
 * not exist a moment ago -- and the watcher must notice, the rescan
 * must find it, the home must show it, the store must still be in its
 * place, and a window that was open must still be its own program.
 *
 *   cc -O2 -std=gnu11 -o /tmp/storetest tools/storetest.c \
 *      src/aurshell/apps.c src/aurshell/appwatch.c src/aurshell/foot.c \
 *      src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
 *      src/aurshell/layouts/[a-z]*.c src/common/theme.c src/common/font.c \
 *      -I src/aurshell -I src/common -lm && /tmp/storetest
 *
 * Run from the repository root (it reads the .shell files in shells/).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../src/aurshell/shell.h"
#include "../src/aurshell/foot.h"
#include "../src/aurshell/appwatch.h"

static int fails, checks;
static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) fails++;
    printf("  %-66s %s\n", what, cond ? "ok" : "FAIL");
}

/* ── the machine ─────────────────────────────────────────────────── */

static char root[256], sysdir[300], flatdir[400], flatapps[600];

static void entry(const char *dir, const char *id, const char *name,
                  const char *comment, const char *cats, const char *exec)
{
    char p[900];
    snprintf(p, sizeof p, "%s/%s.desktop", dir, id);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    /* Exec names the program by an argument the spawn hook can see:
     * `true <id>`, so every entry is a real, runnable command and the
     * test can tell which one was asked for. */
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nComment=%s\n"
               "Categories=%s\nExec=%s\n", name, comment, cats,
            exec ? exec : "");
    if (!exec) fprintf(f, "Exec=true %s\n", id);
    fclose(f);
}

/* What build/forge ships, by the names build/forge gives them. */
static void ship(const char *d)
{
    entry(d, "firefox", "Firefox", "Browse the World Wide Web", "Network;WebBrowser;", "true firefox %u");
    entry(d, "thunar", "Your files", "Everything you have saved, in folders", "System;FileTools;FileManager;", "true thunar");
    entry(d, "atril", "Documents", "Read PDFs and other documents", "GTK;Office;Viewer;", "true atril %U");
    entry(d, "org.xfce.mousepad", "Notes", "Write and read plain notes", "Utility;TextEditor;", "true mousepad %F");
    entry(d, "org.xfce.ristretto", "Photos", "Look at your pictures", "Graphics;Viewer;", "true ristretto %U");
    entry(d, "mpv", "Music and video", "Play a song or a film", "AudioVideo;Audio;Video;Player;", "true mpv %U");
    entry(d, "mate-calc", "Calculator", "Add up, and work things out", "Utility;Calculator;", "true mate-calc");
    entry(d, "system-config-printer", "Printers", "Set up a printer, or check one you have", "System;Settings;Printing;", "true printers");
    entry(d, "xarchiver", "Zip files", "Open a zip file, or make one", "Archiving;Utility;", "true xarchiver %f");
    /* The store, under the package's OWN name: the shell must call it
     * "Get more programs" whatever the package says. */
    entry(d, "org.gnome.Software", "Software", "Add, remove or update software on this computer",
          "GNOME;GTK;System;PackageManager;", "true org.gnome.Software %U");
}

/* Forty more, ranked above the store (every category she would use),
 * which is what pushed it off the home screen. */
static void forty_more(const char *d)
{
    static const char *cats[] = { "Network;WebBrowser;", "Office;WordProcessor;",
                                  "Graphics;Viewer;", "AudioVideo;Player;" };
    for (int i = 0; i < 40; i++) {
        char id[32], nm[48], ex[64];
        snprintf(id, sizeof id, "extra%02d", i);
        snprintf(nm, sizeof nm, "Extra program %02d", i);
        snprintf(ex, sizeof ex, "true %s", id);
        entry(d, id, nm, "Something she installed", cats[i % 4], ex);
    }
}

/* ── the shell ───────────────────────────────────────────────────── */

static char spawned[64];          /* the id the last spawn asked for */
static int  spawn_hook(shell_ctx *c, const char *blob, int n)
{
    (void)c;
    spawned[0] = 0;
    if (n >= 2) snprintf(spawned, sizeof spawned, "%s", blob + strlen(blob) + 1);
    return 0;
}

static void ctx_new(shell_ctx *c, const char *layout, float scale)
{
    memset(c, 0, sizeof *c);
    theme_t t = {0};
    shell_theme_load(c, &t);
    char shf[128];
    snprintf(shf, sizeof shf, "shells/%s.shell", layout);
    if (shell_archetype_load(c, shf) < 0) {
        fprintf(stderr, "cannot read %s -- run this from the repository root\n", shf);
        exit(2);
    }
    c->allow_install = c->allow_settings = c->allow_theme_change = 1;
    c->allow_tty = c->allow_network = 1;
    c->text_scale = scale;
    c->focus = -1; c->hover = -1;
    c->mouse_x = c->mouse_y = -1;
    c->spawn = spawn_hook;
    shell_seed_apps(c);
    if (shell_scan_apps(c) <= 0) { fprintf(stderr, "the scan found nothing\n"); exit(2); }
}

typedef struct { surface *s; shell_fonts f; int w, h; } screen;

static void paint(shell_ctx *c, const shell_layout *L, screen *sc)
{
    surface body = *sc->s;
    body.h = c->screen_h;
    shell_text_log_n = 0;
    shell_text_log_on = 1;
    L->paint(c, &body, &sc->f, NULL);
    shell_text_log_on = 0;
}

/* Back to where she lands: a fresh archetype, nothing open. */
static void land(shell_ctx *c, const shell_layout *L, screen *sc)
{
    if (L->fini) L->fini(c);
    c->n_wins = 0; c->focus = -1; c->hover = -1;
    c->mouse_x = c->mouse_y = -1;
    c->settings_open = c->help_open = 0;
    c->screen_w = sc->w;
    c->screen_h = sc->h - foot_height(c);
    if (L->init) L->init(c);
    paint(c, L, sc);
}

static const shell_text_rec *find_words(const char *words)
{
    for (int i = 0; i < shell_text_log_n; i++)
        if (!strcmp(shell_text_log[i].s, words) && shell_text_log[i].w > 0)
            return &shell_text_log[i];
    return NULL;
}

/* Press the middle of some words. Returns 1 if the store was asked to
 * start. */
static int press(shell_ctx *c, const shell_layout *L, const shell_text_rec *r)
{
    spawned[0] = 0;
    int x = r->x + r->w / 2, y = r->y + r->h / 2;
    if (L->motion) L->motion(c, x, y);
    c->mouse_x = x; c->mouse_y = y; c->mouse_down = 1;
    L->click(c, x, y);
    c->mouse_down = 0;
    if (L->step) for (int i = 0; i < 60; i++) L->step(c, 0.05f);
    return !strcmp(spawned, "org.gnome.Software");
}

/* How many presses from landing to the store starting: 1, 2, or 0 for
 * "not within two". `how` says which words. */
static int reach(shell_ctx *c, const shell_layout *L, screen *sc, char *how, size_t hn)
{
    land(c, L, sc);
    const shell_text_rec *r = find_words("Get more programs");
    if (r) {
        shell_text_rec keep = *r;
        if (press(c, L, &keep)) { snprintf(how, hn, "\"Get more programs\""); return 1; }
    }
    /* Not on the first screen, or not pressable there: every word on
     * the first screen, then the store on the screen it leads to. */
    land(c, L, sc);
    int n = shell_text_log_n;
    static shell_text_rec first[SHELL_TEXT_LOG_MAX];
    memcpy(first, shell_text_log, sizeof first[0] * (size_t)n);
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (!strcmp(first[j].s, first[i].s)) dup = 1;
        if (dup || !strcmp(first[i].s, "Get more programs")) continue;
        land(c, L, sc);
        press(c, L, &first[i]);
        paint(c, L, sc);
        const shell_text_rec *s2 = find_words("Get more programs");
        if (!s2) continue;
        shell_text_rec keep = *s2;
        if (press(c, L, &keep)) {
            snprintf(how, hn, "\"%.60s\", then \"Get more programs\"", first[i].s);
            return 2;
        }
    }
    snprintf(how, hn, "not within two presses");
    /* Say what WAS there, so a failure names its cause: usually the
     * words were drawn, but cut short. */
    land(c, L, sc);
    for (int i = 0; i < shell_text_log_n; i++)
        if (strstr(shell_text_log[i].s, "Get") || strstr(shell_text_log[i].s, "more"))
            printf("      on screen: \"%s\" at %d,%d (%dx%d)\n", shell_text_log[i].s,
                   shell_text_log[i].x, shell_text_log[i].y,
                   shell_text_log[i].w, shell_text_log[i].h);
    return 0;
}

static void archetypes(void)
{
    static const char *ids[] = { "rail", "tiles", "taskbar", "dock", "workbench" };
    static const int sizes[][2] = { {1024, 600}, {1280, 800}, {1366, 768}, {1920, 1080} };
    static const float scales[] = { 1.0f, 1.5f };

    for (int many = 0; many < 2; many++) {
        if (many) forty_more(sysdir);
        printf("\n%s:\n", many ? "after she has installed forty more programs"
                              : "the programs this image ships");
        for (size_t a = 0; a < sizeof ids / sizeof ids[0]; a++) {
            const shell_layout *L = shell_layout_by_id(ids[a]);
            for (size_t z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
                for (size_t k = 0; k < sizeof scales / sizeof scales[0]; k++) {
                    static shell_ctx c;
                    ctx_new(&c, ids[a], scales[k]);
                    screen sc = { surface_new(sizes[z][0], sizes[z][1]), {0},
                                  sizes[z][0], sizes[z][1] };
                    shell_fonts_load(&sc.f, &c);
                    char how[200], what[320];
                    int p = reach(&c, L, &sc, how, sizeof how);
                    snprintf(what, sizeof what, "%-9s %4dx%-4d text %.1fx: %d press%s, %s",
                             ids[a], sc.w, sc.h, scales[k], p, p == 1 ? "" : "es", how);
                    ok(p >= 1 && p <= 2, what);
                    if (L->fini) L->fini(&c);
                    shell_fonts_free(&sc.f);
                    surface_free(sc.s);
                }
        }
    }
}

/* ── the table itself ────────────────────────────────────────────── */

static void table(void)
{
    printf("\nthe app table:\n");
    static shell_ctx c;
    ctx_new(&c, "rail", 1.f);
    int st = shell_store_app(&c);
    ok(st >= 0, "the store is found among the programs");
    if (st < 0) return;
    ok(!strcmp(c.apps[st].name, "Get more programs"),
       "it is called \"Get more programs\", not what the package calls it");
    ok(c.apps[st].icon == ICON_STORE, "it has its own mark");
    ok(!c.apps[st].pinned, "it is not one of the dock's favourites (it has its own button)");

    int pick[8];
    for (int cap = 2; cap <= 8; cap++) {
        int n = shell_home_pick(&c, cap, pick);
        char w[96];
        snprintf(w, sizeof w, "a home with room for %d ends with the store", cap);
        ok(n == cap && pick[n - 1] == st, w);
    }

    c.allow_install = 0;
    shell_scan_apps(&c);
    ok(shell_store_app(&c) < 0, "a machine where she may not install has no store at all");
    c.allow_install = 1;
    c.kiosk = 1;
    shell_scan_apps(&c);
    ok(shell_store_app(&c) < 0, "...and neither does a kiosk");
}

/* ── installing a program while the desktop runs ─────────────────── */

static int64_t ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void install(void)
{
    printf("\na program is installed while the desktop is running:\n");
    static shell_ctx c;
    ctx_new(&c, "rail", 1.f);
    /* Calculator, because it ranks BELOW where VLC will land: its index
     * in the table moves when VLC arrives, and the window has to move
     * with it. (Firefox's would not, and a check on it passed with the
     * remapping deleted.) */
    int calc = -1;
    for (int i = 0; i < c.n_apps; i++) if (!strcmp(c.apps[i].id, "mate-calc")) calc = i;
    int wi = shell_launch(&c, calc);
    ok(wi >= 0, "Calculator is open before the install");

    int fd = appwatch_open();
    ok(fd >= 0, "the watcher starts");
    /* Flathub's directory does not exist yet: it is made by the first
     * Flathub install, and the watcher must be waiting in its parent. */
    struct stat st;
    ok(stat(flatapps, &st) != 0, "(Flathub's directory does not exist yet)");

    /* What flatpak does: the export directory, then a SYMLINK into the
     * program's own deployment. */
    char deploy[700], target[900], link[900];
    snprintf(deploy, sizeof deploy, "%s/app-vlc", root);
    mkdir(deploy, 0755);
    entry(deploy, "org.videolan.VLC", "VLC media player", "Read, capture, broadcast your multimedia streams",
          "AudioVideo;Player;Recorder;",
          "true org.videolan.VLC --file-forwarding @@u %U @@");
    snprintf(target, sizeof target, "%s/org.videolan.VLC.desktop", deploy);
    char mk[1024];
    snprintf(mk, sizeof mk, "%s/share", flatdir);
    mkdir(flatdir, 0755); mkdir(mk, 0755); mkdir(flatapps, 0755);
    snprintf(link, sizeof link, "%s/org.videolan.VLC.desktop", flatapps);
    if (symlink(target, link) != 0) perror("symlink");

    /* Wait for it the way the shell's loop does: poll, pump, due. */
    int due = 0;
    int64_t t0 = ms();
    while (!due && ms() - t0 < 6000) {
        struct pollfd p = { fd, POLLIN, 0 };
        int w = appwatch_wait_ms();
        poll(&p, 1, w >= 0 && w < 200 ? w : 200);
        appwatch_pump();
        due = appwatch_due();
    }
    char w1[96];
    snprintf(w1, sizeof w1, "the watcher asks for a rescan (after %d ms)", (int)(ms() - t0));
    ok(due, w1);

    int added[8];
    int k = shell_rescan_apps(&c, added, 8);
    ok(k == 1, "the rescan finds exactly one new program");
    int vlc = (k >= 1) ? added[0] : -1;
    ok(vlc >= 0 && !strcmp(c.apps[vlc].id, "org.videolan.VLC"),
       "it is VLC, read through Flathub's symlink");
    if (vlc >= 0) {
        int fresh = c.apps[vlc].fresh > 0;
        ok(fresh, "it is marked as just installed");
        /* @@u and @@ are flatpak's markers, not arguments. */
        const char *a = c.apps[vlc].exec;
        int marks = 0;
        for (int i = 0; i < c.apps[vlc].n_args; i++, a += strlen(a) + 1)
            if (!strncmp(a, "@@", 2)) marks++;
        ok(marks == 0, "its command line has no @@ markers left in it");
    }
    int moved = 0;
    for (int i = 0; i < c.n_apps; i++)
        if (!strcmp(c.apps[i].id, "mate-calc")) moved = (i != calc);
    ok(moved, "(Calculator's place in the table did move)");
    ok(wi < c.n_wins && c.wins[wi].app >= 0 &&
       !strcmp(c.apps[c.wins[wi].app].id, "mate-calc"),
       "the open window is still Calculator, though the table moved");

    /* And on Home, where she will look -- with the store still last. */
    const shell_layout *L = shell_layout_by_id("rail");
    screen sc = { surface_new(1280, 800), {0}, 1280, 800 };
    shell_fonts_load(&sc.f, &c);
    c.n_wins = 0; c.focus = -1;
    land(&c, L, &sc);
    ok(find_words("VLC media player") != NULL, "rail's Home shows VLC straight away");
    const shell_text_rec *s1 = find_words("VLC media player");
    const shell_text_rec *s2 = find_words("Get more programs");
    ok(s1 && s2 && s2->y > s1->y, "...above \"Get more programs\", which has not moved");
    shell_text_rec keep;
    if (s1) {
        keep = *s1;
        press(&c, L, &keep);
        ok(!strcmp(spawned, "org.videolan.VLC"), "pressing it starts it");
    }
    if (L->fini) L->fini(&c);
    shell_fonts_free(&sc.f);
    surface_free(sc.s);

    /* Removing it is noticed too, and the table goes back. */
    unlink(link);
    due = 0; t0 = ms();
    while (!due && ms() - t0 < 6000) {
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 200);
        appwatch_pump();
        due = appwatch_due();
    }
    ok(due, "removing it is noticed");
    shell_rescan_apps(&c, added, 8);
    int still = 0;
    for (int i = 0; i < c.n_apps; i++) if (!strcmp(c.apps[i].id, "org.videolan.VLC")) still = 1;
    ok(!still, "...and it is gone from the table");
    appwatch_close();
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/storetest.%d", (int)getpid());
    snprintf(sysdir, sizeof sysdir, "%s/usr-share-applications", root);
    snprintf(flatdir, sizeof flatdir, "%s/flatpak-exports", root);
    snprintf(flatapps, sizeof flatapps, "%s/share/applications", flatdir);
    if (mkdir(root, 0700) != 0 || mkdir(sysdir, 0755) != 0) { perror(root); return 2; }
    char dirs[1400];
    snprintf(dirs, sizeof dirs, "%s:%s", sysdir, flatapps);
    setenv("AUROS_APP_DIRS", dirs, 1);
    setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
    ship(sysdir);

    printf("is \"Get more programs\" on every home screen?\n");
    table();
    install();
    archetypes();

    char rm[400];
    snprintf(rm, sizeof rm, "rm -rf '%s'", root);
    if (system(rm) != 0) fprintf(stderr, "could not remove %s\n", root);

    printf("\n%d checks, %d failed\n", checks, fails);
    if (!fails)
        printf("\"Get more programs\" is within two presses of every home,\n"
               "and a program installed now is on Home now.\n");
    return fails ? 1 : 0;
}
