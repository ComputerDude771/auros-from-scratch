/* sessionenvtest.c — does the portal find the screen?
 *
 * A Flathub program asks for a file through xdg-desktop-portal, which
 * the user's systemd starts on the session bus. It inherits nothing
 * from aurshell; it knows where the screen is only if somebody told
 * the bus and systemd (dbus-update-activation-environment --systemd).
 * Nobody did, so "Open..." in a Flathub program opened nothing.
 *
 * run_publish_env() is that somebody. This puts a stand-in for
 * dbus-update-activation-environment first on PATH, which writes down
 * what it was told, and checks:
 *   - nothing is published while the shell is still on its private
 *     boot-time bus (the portal is not on that one);
 *   - once on logind's bus, exactly the four things the portal needs,
 *     with --systemd, are published -- including the Flathub data dirs;
 *   - and only once.
 *
 *   cc -O2 -std=gnu11 -o /tmp/sessionenvtest tools/sessionenvtest.c \
 *      src/aurshell/run.c -I src/aurshell && /tmp/sessionenvtest
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "run.h"

static int fails, checks;
static void ok(int c, const char *w) { checks++; if (!c) fails++; printf("  %-62s %s\n", w, c ? "ok" : "FAIL"); }

static char said[4096];
static int  read_said(const char *path)
{
    said[0] = 0;
    for (int i = 0; i < 200; i++) {               /* up to 2 s for the child */
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(said, 1, sizeof said - 1, f);
            said[n] = 0; fclose(f);
            if (n) return 1;
        }
        usleep(10000);
    }
    return 0;
}

int main(void)
{
    char root[64], bin[128], rt[128], rec[160], fake[200], sock[200];
    snprintf(root, sizeof root, "/tmp/sessionenvtest.%d", (int)getpid());
    snprintf(bin, sizeof bin, "%s/bin", root);
    snprintf(rt, sizeof rt, "%s/run", root);
    snprintf(rec, sizeof rec, "%s/said.txt", root);
    if (mkdir(root, 0700) || mkdir(bin, 0700) || mkdir(rt, 0700)) { perror(root); return 2; }

    snprintf(fake, sizeof fake, "%s/dbus-update-activation-environment", bin);
    FILE *f = fopen(fake, "w");
    fprintf(f, "#!/bin/sh\nfor a in \"$@\"; do printf '%%s\\n' \"$a\"; done >> '%s'\n", rec);
    fclose(f);
    chmod(fake, 0755);

    char path[400];
    snprintf(path, sizeof path, "%s:/usr/bin:/bin", bin);
    setenv("PATH", path, 1);
    setenv("XDG_RUNTIME_DIR", rt, 1);
    setenv("XDG_DATA_DIRS",
           "/home/her/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/share", 1);
    /* The private bus the unit's dbus-run-session made at boot. */
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/dbus-private-boot", 1);

    printf("does the portal find the screen?\n");
    ok(run_publish_env("wayland-1") == 0, "on the boot-time bus: nothing is published");
    usleep(200000);
    ok(access(rec, F_OK) != 0, "...and the helper was not started");

    /* logind makes /run/user/<uid>/bus; the shell adopts it. */
    snprintf(sock, sizeof sock, "%s/bus", rt);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%.100s", sock);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 2; }
    ok(run_adopt_user_bus() == 1, "the shell moves to logind's bus");
    ok(run_publish_env("wayland-1") == 1, "...and then publishes where the desktop is");
    ok(read_said(rec), "the helper ran");
    ok(strstr(said, "--systemd\n") != NULL, "with --systemd, so the user's systemd hears it too");
    ok(strstr(said, "WAYLAND_DISPLAY=wayland-1\n") != NULL, "WAYLAND_DISPLAY is the compositor's socket");
    ok(strstr(said, "XDG_CURRENT_DESKTOP=AurOS\n") != NULL, "XDG_CURRENT_DESKTOP=AurOS (auros-portals.conf)");
    ok(strstr(said, "XDG_SESSION_TYPE=wayland\n") != NULL, "XDG_SESSION_TYPE=wayland");
    ok(strstr(said, "XDG_DATA_DIRS=/home/her/.local/share/flatpak/exports/share:"
                    "/var/lib/flatpak/exports/share:/usr/share\n") != NULL,
       "XDG_DATA_DIRS with Flathub's directories in it");
    for (int i = 0; i < 20; i++) { run_reap(); usleep(10000); }
    ok(run_publish_env("wayland-1") == 0, "and only once");

    close(s);
    char rm[160]; snprintf(rm, sizeof rm, "rm -rf '%s'", root);
    if (system(rm) != 0) fprintf(stderr, "could not remove %s\n", root);
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
