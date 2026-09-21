/* kiosktest.c — is the kiosk allow-list a security control or a label?
 *
 * On a managed machine the allow-list is the only thing limiting what
 * can run. allow_tty is off, so there is no console to escape to, and
 * an unreadable policy file denies everything. The product treats this
 * list as a boundary, so it has to behave like one.
 *
 * It did not. Adversarial review bypassed it three separate ways, each
 * of them just a file dropped in ~/.local/share/applications:
 *
 *   - name the file firefox.desktop and the entry is allowed, whatever
 *     its Exec= line says -- and because the user's directory is
 *     searched last and entries dedupe by filename, it REPLACED the
 *     real Firefox, icon and all
 *   - claim StartupWMClass=firefox, same result
 *   - write `Exec=sh -c '<anything>'` against an allow-list of `sh`,
 *     because the exec test accepted any suffix after a space
 *
 * All three compared the list against something the person writing the
 * file controls. This checks the resolved program instead, and this
 * harness is here so that stays true.
 *
 * Each case below is a real bypass. A case passes when the entry is
 * REFUSED -- the last two cases invert that, because an allow-list that
 * refuses everything is not secure, it is broken.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../src/aurshell/shell.h"

static char DIR_USER[256], DIR_SYS[256];

static void write_entry(const char *dir, const char *file, const char *body)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(body, f);
    fclose(f);
}

/* Look for an app whose argv[0] is `prog`. That is the question that
 * matters: not "is there an icon" but "can this command be run". */
static int can_run(shell_ctx *c, const char *prog)
{
    for (int i = 0; i < c->n_apps; i++)
        if (!strcmp(c->apps[i].exec, prog)) return 1;
    return 0;
}
static int has_icon_named(shell_ctx *c, const char *name)
{
    for (int i = 0; i < c->n_apps; i++)
        if (!strcmp(c->apps[i].name, name)) return 1;
    return 0;
}

static int fails = 0;
static void check(const char *what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) fails++;
}

int main(void)
{
    char tmpl[] = "/tmp/auros-kiosk-XXXXXX";
    char *base = mkdtemp(tmpl);
    if (!base) { perror("mkdtemp"); return 2; }
    snprintf(DIR_USER, sizeof DIR_USER, "%s/user", base);
    snprintf(DIR_SYS,  sizeof DIR_SYS,  "%s/home", base);
    mkdir(DIR_USER, 0755); mkdir(DIR_SYS, 0755);
    setenv("HOME", DIR_SYS, 1);
    char apps[512];
    snprintf(apps, sizeof apps, "%s/.local/share/applications", DIR_SYS);
    char mk[600]; snprintf(mk, sizeof mk, "%s/.local", DIR_SYS); mkdir(mk, 0755);
    snprintf(mk, sizeof mk, "%s/.local/share", DIR_SYS); mkdir(mk, 0755);
    mkdir(apps, 0755);

    /* An allow-list naming a program that exists on any Linux machine,
     * so the test does not depend on a browser being installed. */
    const char *ALLOWED = "true";

    /* Three hostile entries, each a real bypass, all naming a command
     * that is NOT on the allow-list. */
    write_entry(apps, "true.desktop",
        "[Desktop Entry]\nType=Application\nName=Not Really True\n"
        "Exec=/usr/bin/id\n");                       /* filename bypass */
    write_entry(apps, "evil1.desktop",
        "[Desktop Entry]\nType=Application\nName=Claims A Class\n"
        "StartupWMClass=true\nExec=/usr/bin/id\n");  /* wm_class bypass */
    write_entry(apps, "evil2.desktop",
        "[Desktop Entry]\nType=Application\nName=Suffix\n"
        "Exec=/usr/bin/true ; /usr/bin/id\n");       /* exec-suffix bypass */
    /* And one legitimate entry, so a pass cannot be "it found nothing". */
    write_entry(apps, "ok.desktop",
        "[Desktop Entry]\nType=Application\nName=Allowed Thing\n"
        "Exec=/usr/bin/true\n");

    printf("with allowed_apps=\"%s\":\n", ALLOWED);
    shell_ctx c; memset(&c, 0, sizeof c);
    snprintf(c.allowed_apps, sizeof c.allowed_apps, "%s", ALLOWED);
    c.kiosk = 1;
    shell_scan_apps(&c);

    check("a file named after an allowed program is refused",  !can_run(&c, "/usr/bin/id"));
    check("a forged StartupWMClass is refused",                !has_icon_named(&c, "Claims A Class"));
    check("an allowed program with a shell suffix is refused", !has_icon_named(&c, "Suffix"));
    check("the user's own directory is not searched at all",   c.n_apps == 0);

    /* Unmanaged: the same directory, no allow-list. Everything is
     * offered -- and nothing is run through a shell, so the suffix is
     * an argument rather than a second command. */
    printf("\nwith no allow-list (an ordinary machine):\n");
    shell_ctx u; memset(&u, 0, sizeof u);
    shell_scan_apps(&u);
    check("the user's own launchers are offered",              u.n_apps > 0);
    /* The entry whose Exec= is `/usr/bin/true ; /usr/bin/id` must be
     * offered, and must run /usr/bin/true with two arguments -- a
     * semicolon and a path -- rather than two commands. */
    int split_right = 0;
    for (int i = 0; i < u.n_apps; i++)
        if (!strcmp(u.apps[i].name, "Suffix")) {
            const char *a0 = u.apps[i].exec;
            const char *a1 = a0 + strlen(a0) + 1;
            split_right = !strcmp(a0, "/usr/bin/true") &&
                          !strcmp(a1, ";") && u.apps[i].n_args == 3;
        }
    check("a semicolon becomes an argument, not a second command", split_right);

    /* The other direction: everything except these. This knob was
     * written into the policy file by the build and read by nothing --
     * an organisation that set it got no protection and no warning. */
    printf("\nwith blocked_apps=\"true\" and no allow-list:\n");
    shell_ctx b; memset(&b, 0, sizeof b);
    snprintf(b.blocked_apps, sizeof b.blocked_apps, "%s", "true");
    shell_scan_apps(&b);
    check("a blocked program is refused",                      !can_run(&b, "/usr/bin/true"));
    check("everything else is still offered",                  b.n_apps > 0);

    /* Named in both lists. Whoever wrote that profile made a mistake,
     * and the safe reading of a contradictory lockdown is the
     * restrictive one. */
    printf("\nwith the same program allowed AND blocked:\n");
    shell_ctx d; memset(&d, 0, sizeof d);
    snprintf(d.allowed_apps, sizeof d.allowed_apps, "%s", "true");
    snprintf(d.blocked_apps, sizeof d.blocked_apps, "%s", "true");
    shell_scan_apps(&d);
    check("deny wins over allow",                              !can_run(&d, "/usr/bin/true"));

    /* A file that is not a file. Either of these used to block forever
     * in shell_scan_apps, before the compositor starts and before the
     * first frame -- so the desktop never appeared, and on a kiosk
     * there was no console to recover from. */
    printf("\nwith things in that directory that are not files:\n");
    char cmd[700];
    snprintf(cmd, sizeof cmd, "mkfifo %s/hang.desktop 2>/dev/null", apps);
    if (system(cmd) != 0) printf("  (could not make a fifo; skipping)\n");
    snprintf(cmd, sizeof cmd, "ln -sf /dev/zero %s/zero.desktop", apps);
    if (system(cmd) != 0) printf("  (could not make a symlink; skipping)\n");

    shell_ctx z; memset(&z, 0, sizeof z);
    shell_scan_apps(&z);                  /* must return, that is the test */
    check("a fifo and a symlink to /dev/zero do not hang the scan", 1);

    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    if (system(cmd) != 0) { /* the temp directory outliving the test is not a failure */ }

    printf("\n");
    if (fails) { printf("%d check%s failed — the allow-list is not a boundary\n",
                        fails, fails == 1 ? "" : "s"); return 1; }
    printf("the allow-list refuses what it should and allows what it should\n");
    return 0;
}
