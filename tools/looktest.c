/* looktest.c — when she picks a look in Settings, do her programs follow?
 *
 * Settings rendered only the desktop's shell.conf into her directory.
 * Her programs import the MACHINE's gtk.css and read light-or-dark from
 * a settings.ini the build wrote once, so after "Now using Sandstone."
 * the next program she opened was in the old colours and dark on a
 * light desktop.
 *
 * This drives Settings by its words, the way she does -- "How it looks",
 * then the name of a look -- with the real aurora rendering into a
 * pretend home, and checks what a GTK program starting afterwards would
 * read: its gtk.css import, the rendered colours, and
 * gtk-application-prefer-dark-theme. Then the other way, light to dark;
 * and a gtk.css she wrote herself must be left alone.
 *
 *   cc -O2 -std=gnu11 -o /tmp/looktest tools/looktest.c \
 *      src/aurshell/settings.c src/aurshell/power.c src/aurshell/run.c \
 *      src/aurshell/foot.c src/aurshell/welcome.c src/aurshell/draw.c \
 *      src/aurshell/shellcommon.c src/aurshell/anim.c \
 *      src/aurshell/layouts/[a-z]*.c src/common/theme.c src/common/font.c \
 *      -I src/aurshell -I src/common -lm && /tmp/looktest   # repo root
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/aurshell/shell.h"
#include "../src/aurshell/settings.h"
#include "../src/aurshell/foot.h"

static int fails, checks;
static void ok(int c, const char *w) { checks++; if (!c) fails++; printf("  %-62s %s\n", w, c ? "ok" : "FAIL"); }

static shell_ctx C;
static shell_fonts F;
static surface *SURF;

static void paint(void)
{
    shell_text_log_n = 0; shell_text_log_on = 1;
    settings_paint(&C, SURF, &F);
    shell_text_log_on = 0;
}
static int press(const char *words)
{
    paint();
    for (int i = 0; i < shell_text_log_n; i++)
        if (!strcmp(shell_text_log[i].s, words)) {
            int x = shell_text_log[i].x + shell_text_log[i].w / 2;
            int y = shell_text_log[i].y + shell_text_log[i].h / 2;
            settings_motion(&C, x, y);
            return settings_click(&C, x, y);
        }
    return 0;
}
static int file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, needle) != NULL;
}
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); }
    fputs(text, f); fclose(f);
}

int main(void)
{
    char home[64], cwd[512], path[1200], p[900];
    if (!getcwd(cwd, sizeof cwd)) return 2;
    snprintf(home, sizeof home, "/tmp/looktest.%d", (int)getpid());
    if (mkdir(home, 0700)) { perror(home); return 2; }
    setenv("HOME", home, 1);
    snprintf(path, sizeof path, "%s/src/aurora:/usr/bin:/bin", cwd);
    setenv("PATH", path, 1);
    snprintf(p, sizeof p, "%s/themes", cwd);           setenv("AUROS_THEME_DIR", p, 1);
    setenv("AURORA_THEMES", p, 1);
    snprintf(p, sizeof p, "%s/themes/templates", cwd); setenv("AURORA_TEMPLATES", p, 1);
    snprintf(p, sizeof p, "%s/shells", cwd);           setenv("AUROS_SHELLS", p, 1);

    /* Her account as build/forge's stage 5 leaves it: Nocturne, dark. */
    for (int v = 3; v <= 4; v++) {
        snprintf(p, sizeof p, "%s/.config", home); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/.config/gtk-%d.0", home, v); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/.config/gtk-%d.0/gtk.css", home, v);
        put(p, "/* Written by forge. The one line that makes the desktop's colours\n"
               "   reach the programs running on it. */\n"
               "@import url(\"file:///etc/auros/gtk/gtk.css\");\n");
        snprintf(p, sizeof p, "%s/.config/gtk-%d.0/settings.ini", home, v);
        put(p, "[Settings]\ngtk-application-prefer-dark-theme=1\ngtk-font-name=Inter 11\n");
    }

    theme_t t = {0};
    shell_theme_load(&C, &t);
    C.allow_settings = C.allow_theme_change = C.allow_network = 1;
    C.text_scale = 1.f; C.foot_hover = -1;
    snprintf(C.layout_id, sizeof C.layout_id, "rail");
    C.screen_w = 1280; C.screen_h = 800 - foot_height(&C);
    shell_fonts_load(&F, &C);
    SURF = surface_new(1280, 800);

    printf("when she picks a look in Settings, do her programs follow?\n");
    C.settings_open = 1;
    settings_opened(&C);
    ok(press("How it looks"), "\"How it looks\" opens");
    ok(press("Sandstone"), "she presses \"Sandstone\"");
    char g3[600], g4[600], ini3[600], ini4[600], mine[600], want[700];
    snprintf(g3, sizeof g3, "%s/.config/gtk-3.0/gtk.css", home);
    snprintf(g4, sizeof g4, "%s/.config/gtk-4.0/gtk.css", home);
    snprintf(ini3, sizeof ini3, "%s/.config/gtk-3.0/settings.ini", home);
    snprintf(ini4, sizeof ini4, "%s/.config/gtk-4.0/settings.ini", home);
    snprintf(mine, sizeof mine, "%s/.config/auros/gtk/gtk.css", home);
    snprintf(want, sizeof want, "@import url(\"file://%s/.config/auros/gtk/gtk.css\");", home);
    struct stat st;
    ok(stat(mine, &st) == 0 && st.st_size > 100, "her own copy of the programs' colours is rendered");
    ok(file_has(g3, want) && file_has(g4, want), "GTK 3 and 4 programs import her copy, not the machine's");
    ok(file_has(ini3, "gtk-application-prefer-dark-theme=0\n") &&
       file_has(ini4, "gtk-application-prefer-dark-theme=0\n"), "and are light, as Sandstone is");
    ok(file_has(ini3, "gtk-font-name=Inter 11\n"), "the rest of settings.ini is kept");

    ok(press("Nocturne"), "she presses \"Nocturne\"");
    ok(file_has(ini3, "gtk-application-prefer-dark-theme=1\n") &&
       file_has(ini4, "gtk-application-prefer-dark-theme=1\n"), "and her programs are dark again");

    /* Something she wrote herself is hers. */
    put(g3, "@import url(\"file:///etc/auros/gtk/gtk.css\");\nwindow { font-size: 20px; }\n");
    ok(press("Sandstone"), "with a gtk.css of her own, she presses \"Sandstone\"");
    ok(file_has(g3, "font-size: 20px"), "her own gtk.css is left exactly as she wrote it");

    char rm[200]; snprintf(rm, sizeof rm, "rm -rf '%s'", home);
    if (system(rm) != 0) fprintf(stderr, "could not remove %s\n", home);
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
