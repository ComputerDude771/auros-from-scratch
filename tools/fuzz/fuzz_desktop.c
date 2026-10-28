/* fuzz_desktop.c -- the .desktop scanner against files someone else wrote.
 *
 * ~/.local/share/applications and Flathub's export directories are
 * written by programs, not by us, and every file in them is parsed by
 * the shell before the first frame. src/aurshell/apps.c reads them
 * (localised keys, groups, over-long lines), strips field codes, splits
 * Exec= into an argument vector by the XDG quoting rules, drops
 * flatpak's forwarding marks, resolves the program through PATH,
 * applies the kiosk allow/deny lists, ranks, keeps the store, and
 * copies the result into the shell's fixed-size app table.
 *
 * This harness includes apps.c itself (so nothing is re-implemented),
 * writes the fuzzer's bytes as up to three desktop files into a private
 * directory named by AUROS_APP_DIRS -- the same override storetest uses
 * -- and runs the real shell_scan_apps() and shell_rescan_apps() under
 * a policy the first input byte picks.
 *
 * Input: [policy byte][file 0] \f [file 1] \f [file 2]
 * File 1 is called org.gnome.Software.desktop so the store-keeping
 * path is reachable.
 */
#define _GNU_SOURCE
#include "../../src/aurshell/apps.c"

#include <stdint.h>
#include <sys/stat.h>

static char dir[256];
static const char *const NAMES[3] = {
    "fuzz.desktop", "org.gnome.Software.desktop", "z-other.desktop",
};

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    snprintf(dir, sizeof dir, "%s/fuzz-desktop-XXXXXX", base);
    if (!mkdtemp(dir)) abort();
    setenv("AUROS_APP_DIRS", dir, 1);
    /* So Exec=sh, Exec=true and friends resolve the way they would on
     * the machine, and nothing outside the system directories does. */
    setenv("PATH", "/usr/bin:/bin", 1);
    setenv("LANG", "pt_BR.UTF-8", 1);
    setenv("XDG_CURRENT_DESKTOP", "AurOS", 1);
    return 0;
}

static void put(int i, const uint8_t *p, size_t n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, NAMES[i]);
    if (!p) { unlink(path); return; }
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    if (n && fwrite(p, 1, n, f) != n) abort();
    fclose(f);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!dir[0]) LLVMFuzzerInitialize(NULL, NULL);
    if (size < 1 || size > 128 * 1024) return 0;
    uint8_t policy = data[0];
    const uint8_t *p = data + 1, *end = data + size;

    for (int i = 0; i < 3; i++) {
        if (p > end) { put(i, NULL, 0); continue; }
        const uint8_t *q = memchr(p, '\f', (size_t)(end - p));
        if (!q) q = end;
        put(i, p, (size_t)(q - p));
        p = q + 1;
    }

    static shell_ctx c;
    memset(&c, 0, sizeof c);
    c.allow_install  = !!(policy & 1);
    c.kiosk          = !!(policy & 2);
    c.allow_settings = !!(policy & 4);
    if (policy & 8)  snprintf(c.allowed_apps, sizeof c.allowed_apps, "sh,/usr/bin/true firefox");
    if (policy & 16) snprintf(c.blocked_apps, sizeof c.blocked_apps, "true");
    c.deny_all_apps  = !!(policy & 32);
    c.hover = -1;
    c.fg = 0xD4DCEA;

    if (shell_scan_apps(&c) < 0) return 0;
    for (int i = 0; i < c.n_apps; i++) {
        app_entry *a = &c.apps[i];
        /* Walk the argument vector the way launch does: NUL-separated
         * tokens ending in an empty one, n_args of them. */
        const char *t = a->exec;
        for (int k = 0; k < a->n_args && t < a->exec + sizeof a->exec; k++)
            t += strnlen(t, (size_t)(a->exec + sizeof a->exec - t)) + 1;
        (void)strlen(a->name); (void)strlen(a->id); (void)strlen(a->wm_class);
    }
    /* A window still open on whatever is in slot 0, then a rescan: the
     * path the shell takes when a program is installed or removed. */
    c.n_wins = 1;
    c.wins[0].app = c.n_apps ? 0 : -1;
    int added[8];
    shell_rescan_apps(&c, added, 8);
    return 0;
}
