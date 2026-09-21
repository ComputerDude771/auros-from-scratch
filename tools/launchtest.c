/* launchtest.c — can she start a program by clicking on it?
 *
 * This is the first question a desktop has to answer, and this product
 * answered it "no" for its entire life without anyone noticing.
 *
 * shell_launch() -- the one function that asks the host to start a
 * program -- had zero callers. All six archetypes hand-rolled a window
 * slot instead: they filled in a title and a subtitle and stopped, so
 * every icon opened a rectangle with a name in it and nothing behind
 * the rectangle. tiles.c even carried a comment explaining that the
 * shell contract had no hook for starting anything; the hook was added
 * later and the comment, and the code under it, stayed.
 *
 * NOTHING CAUGHT IT, and the reason is worth writing down:
 *
 *   - tools/hittest.c proves a click is CONSUMED and that the frame
 *     changes afterwards. Both were true. A placeholder window is a
 *     visible change.
 *   - `aurshell --with-app CMD` renders the desktop with a real browser
 *     in it, and was used as the end-to-end proof. But --with-app calls
 *     aurwl_spawn() directly from main.c. It exercises the compositor
 *     and bypasses the entire click-to-launch path -- precisely the
 *     piece that was missing.
 *
 * A test that starts the program itself cannot discover that nothing
 * else does. So this one never spawns anything: it installs a recording
 * hook in shell_ctx.spawn, sweeps clicks across the screen, and asks
 * whether any of them reached the hook.
 *
 *   launchtest [-q]
 *
 * Exit 0 if every archetype can start a program by being clicked.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aurshell/shell.h"

#define STEP 7

extern const shell_layout layout_rail, layout_tiles, layout_locked,
                          layout_taskbar, layout_dock, layout_workbench;

static const shell_layout *ALL[] = {
    &layout_rail, &layout_tiles, &layout_locked,
    &layout_taskbar, &layout_dock, &layout_workbench,
};
static const char *IDS[] = { "rail","tiles","locked","taskbar","dock","workbench" };

/* What the host would have been asked to run. */
#define MAX_REC 64
static struct { char cmd[128]; } rec[MAX_REC];
static int n_rec;

static int recording_spawn(shell_ctx *c, const char *argv_blob, int n_args)
{
    (void)c; (void)n_args;
    if (n_rec < MAX_REC) {
        snprintf(rec[n_rec].cmd, sizeof rec[n_rec].cmd, "%s",
                 argv_blob ? argv_blob : "(null)");
        n_rec++;
    }
    return 0;                      /* pretend it started */
}

/* A context with applications that have something behind them. The
 * seeded table deliberately has an empty exec for its own Settings
 * entry, so this uses its own apps rather than shell_seed_apps(): the
 * question is whether a click reaches the host, and an app with nothing
 * to run could not answer it either way. */
static void seed(shell_ctx *c, const char *id, int n_wins)
{
    memset(c, 0, sizeof *c);
    theme_t t = {0};
    shell_theme_load(c, &t);
    snprintf(c->layout_id, sizeof c->layout_id, "%s", id);
    c->show_clock = 1;
    c->status_full = 1;
    c->allow_install = c->allow_settings = c->allow_theme_change = 1;
    c->allow_tty = 1;
    c->hover = -1;
    c->focus = -1;
    c->spawn = recording_spawn;

    static const char *NAMES[] = { "Internet", "Email", "Photos", "My Files",
                                   "Writing", "Music", "Calculator", "Help" };
    static const char *EXECS[] = { "/usr/bin/x-www-browser", "/usr/bin/mail",
                                   "/usr/bin/photos", "/usr/bin/files",
                                   "/usr/bin/write", "/usr/bin/music",
                                   "/usr/bin/calc", "/usr/bin/help" };
    c->n_apps = (int)(sizeof NAMES / sizeof NAMES[0]);
    for (int i = 0; i < c->n_apps; i++) {
        snprintf(c->apps[i].id,   sizeof c->apps[i].id,   "app%d", i);
        snprintf(c->apps[i].name, sizeof c->apps[i].name, "%s", NAMES[i]);
        snprintf(c->apps[i].hint, sizeof c->apps[i].hint, "Something to do");
        /* NUL-separated argv, terminated by a second NUL. */
        size_t l = strlen(EXECS[i]);
        memcpy(c->apps[i].exec, EXECS[i], l + 1);
        c->apps[i].exec[l + 1] = 0;
        c->apps[i].n_args = 1;
        c->apps[i].icon = (shell_icon)(i % 12);
        c->apps[i].pinned = (i < 4);
    }
    /* Some archetypes only offer to START something when nothing is
     * running; others need a window to exist before they draw anything
     * at all. Both cases are swept. */
    for (int i = 0; i < n_wins && i < c->n_apps; i++) {
        c->wins[i].app = i;
        snprintf(c->wins[i].title, sizeof c->wins[i].title, "%s", c->apps[i].name);
        c->n_wins++;
    }
    if (c->n_wins) c->focus = 0;
}

/* Sweep clicks across the screen and count how many distinct programs
 * the host was asked to start. Each probe gets a fresh context, because
 * a click mutates layout state and the second probe would otherwise be
 * measuring the first one's side effects. */
static int sweep(const shell_layout *L, const char *id, int w, int h,
                 int n_wins, char *first_cmd, size_t fc)
{
    int launched = 0;
    for (int y = 0; y < h; y += STEP)
        for (int x = 0; x < w; x += STEP) {
            shell_ctx c;
            seed(&c, id, n_wins);
            c.screen_w = w; c.screen_h = h;
            if (L->init) L->init(&c);
            n_rec = 0;
            c.mouse_x = x; c.mouse_y = y; c.mouse_down = 1;
            if (L->click) L->click(&c, x, y);
            c.mouse_down = 0;
            if (L->fini) L->fini(&c);
            if (n_rec > 0) {
                if (!launched && first_cmd) snprintf(first_cmd, fc, "%s", rec[0].cmd);
                launched += n_rec;
            }
        }
    return launched;
}

int main(int argc, char **argv)
{
    int verbose = !(argc > 1 && !strcmp(argv[1], "-q"));
    int fail = 0;

    printf("can a program be started by clicking on it?\n");
    for (size_t k = 0; k < sizeof ALL / sizeof ALL[0]; k++) {
        const shell_layout *L = ALL[k];
        char cmd[128] = "";
        /* Twice: with nothing running, and with three windows already
         * open. An archetype that only launches from an empty desktop,
         * or only once something is already there, is broken in a way
         * one sweep would miss. */
        int cold = sweep(L, IDS[k], 1366, 768, 0, cmd, sizeof cmd);
        int warm = sweep(L, IDS[k], 1366, 768, 3, cmd[0] ? NULL : cmd, sizeof cmd);
        /* And on the smallest panel we support, where a control may have
         * been squeezed out of existence. */
        int small = sweep(L, IDS[k], 1024, 600, 0, NULL, 0);

        int ok = (cold + warm) > 0 && small > 0;
        if (verbose) {
            printf("  %-10s %5d cold, %5d with windows open, %5d at 1024x600",
                   IDS[k], cold, warm, small);
            if (ok) printf("   ok -> %s\n", cmd[0] ? cmd : "(a program)");
            else    printf("\n");
        }
        if (!ok) {
            printf("   FAIL %s: clicking never asks the host to start anything%s\n",
                   IDS[k], small == 0 && (cold + warm) > 0
                           ? " at 1024x600" : "");
            fail++;
        }
    }

    printf("\n");
    if (fail) {
        printf("%d archetype%s cannot start a program.\n", fail, fail == 1 ? "" : "s");
        printf("Every icon in %s opens a rectangle with a name in it and nothing\n",
               fail == 1 ? "it" : "them");
        printf("behind the rectangle. See shell_launch() in src/aurshell/shellcommon.c.\n");
        return 1;
    }
    printf("every archetype can start a program by being clicked\n");
    return 0;
}
