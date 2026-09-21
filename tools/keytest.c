/* keytest.c — can she type her wifi password?
 *
 * Every text field in this product used to read one hardcoded table:
 *
 *     { 16, "qwertyuiop" }, { 30, "asdfghjkl" }, { 44, "zxcvbnm" }
 *
 * with a comment above it saying a real session got its characters from
 * the keymap instead. No such path existed. The consequences were not
 * subtle once you look for them: no capital letters, so a password with
 * one in it could not be entered; no digits with Shift, so no ! or ?;
 * and on a French keyboard every letter was wrong, on a machine whose
 * profile said keyboard_layout="fr" because the profile field was also
 * read by nobody.
 *
 * This harness proves the three claims that replaced it.
 *
 *   1. A key resolves through the real keymap, with modifiers. Shift+a
 *      is A. Shift+1 is !. Caps Lock is not Shift.
 *   2. Keys that are not text produce none. Return, Tab, Escape and
 *      Backspace all have a control character in the keymap, and a
 *      password field that inserted them would show a box where the
 *      person expected something to happen.
 *   3. The layout is the one the machine is configured with -- read
 *      from /etc/default/keyboard, the file the rest of the system
 *      already uses and build/forge already writes.
 *
 * It needs no display and no client: aurwl_key_utf8() is answerable
 * from the keymap alone.
 *
 *   cc -O2 -std=gnu11 -o /tmp/keytest tools/keytest.c src/aurwl/aurwl.c \
 *      src/aurshell/draw.c build/gen/[*]-protocol.c -Ibuild/gen \
 *      $(pkg-config --cflags --libs wayland-server xkbcommon) -lm
 *   /tmp/keytest
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/aurwl/aurwl.h"

/* evdev, as the kernel numbers them. Spelled out rather than included
 * so the numbers this harness asserts against are visible in it. */
#define K_1      2
#define K_Q     16
#define K_W     17
#define K_A     30
#define K_Z     44
#define K_ENTER 28
#define K_ESC    1
#define K_BKSP  14
#define K_TAB   15
#define K_SPACE 57
#define K_LSHIFT 42
#define K_CAPS  58
#define K_UP   103

static int fail = 0, checked = 0;
static char scratch[64];

static const char *shown(const char *s)
{
    if (!*s) return "(nothing)";
    snprintf(scratch, sizeof scratch, "\"%s\"", s);
    return scratch;
}

static void expect(aurwl *c, const char *what, int key, const char *want)
{
    char got[8];
    aurwl_key_utf8(c, (uint32_t)key, got, sizeof got);
    checked++;
    printf("    %-38s %-12s", what, shown(got));
    if (strcmp(got, want)) {
        char w[64]; snprintf(w, sizeof w, "%s", shown(want));
        printf("  FAIL (wanted %s)\n", w);
        fail++;
    } else printf("  ok\n");
}

/* Hold a modifier the way the keyboard does: press it, ask, release. */
static void hold(aurwl *c, int mod)   { aurwl_key(c, (uint32_t)mod, 1, 1); }
static void release(aurwl *c, int mod){ aurwl_key(c, (uint32_t)mod, 0, 2); }

static void write_kb(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(body, f);
    fclose(f);
}

/* One compositor per layout: the keymap is built once, at create. */
static aurwl *with_layout(const char *path, const char *body)
{
    write_kb(path, body);
    setenv("AUROS_KB_FILE", path, 1);
    aurwl *c = aurwl_create(1024, 600, 60000);
    if (!c) { fprintf(stderr, "could not create a compositor\n"); exit(2); }
    return c;
}

int main(void)
{
    /* aurwl binds a socket in XDG_RUNTIME_DIR; it does not need to be
     * connected to, only to exist. */
    if (!getenv("XDG_RUNTIME_DIR")) {
        static char d[] = "/tmp/keytest-XXXXXX";
        if (!mkdtemp(d)) { perror("mkdtemp"); return 2; }
        setenv("XDG_RUNTIME_DIR", d, 1);
    }
    char kbpath[256];
    snprintf(kbpath, sizeof kbpath, "%s/keyboard", getenv("XDG_RUNTIME_DIR"));

    printf("can she type her wifi password?\n\n");

    /* ── 1. modifiers, on the layout most machines ship with ─────── */
    aurwl *us = with_layout(kbpath, "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"us\"\n"
                                    "XKBVARIANT=\"\"\nBACKSPACE=\"guess\"\n");
    printf("  a US keyboard\n");
    expect(us, "a",                        K_A, "a");
    expect(us, "1",                        K_1, "1");
    expect(us, "space",                K_SPACE, " ");

    hold(us, K_LSHIFT);
    expect(us, "Shift held, a",            K_A, "A");
    expect(us, "Shift held, 1",            K_1, "!");
    release(us, K_LSHIFT);
    expect(us, "Shift released, a again",  K_A, "a");

    /* Caps Lock capitalises letters and does NOT shift digits. A field
     * that treated it as Shift would turn 1 into !, which is how a
     * password typed with Caps on fails in a way nobody can see. */
    aurwl_key(us, K_CAPS, 1, 3); aurwl_key(us, K_CAPS, 0, 4);
    printf("\n  with Caps Lock on\n");
    expect(us, "a",                        K_A, "A");
    expect(us, "1 (Caps is not Shift)",    K_1, "1");
    aurwl_key(us, K_CAPS, 1, 5); aurwl_key(us, K_CAPS, 0, 6);

    /* ── 2. keys that are not text ───────────────────────────────── */
    printf("\n  keys that are not text\n");
    expect(us, "Return",               K_ENTER, "");
    expect(us, "Tab",                    K_TAB, "");
    expect(us, "Escape",                 K_ESC, "");
    expect(us, "Backspace",             K_BKSP, "");
    expect(us, "Up arrow",                K_UP, "");
    aurwl_destroy(us);

    /* ── 3. the layout the machine is configured with ────────────── */
    printf("\n  a French keyboard, from /etc/default/keyboard\n");
    aurwl *fr = with_layout(kbpath, "# written by build/forge\n"
                                    "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"fr\"\n"
                                    "XKBVARIANT=\"\"\nBACKSPACE=\"guess\"\n");
    /* AZERTY: the three keys that move are the whole point. */
    expect(fr, "the key marked Q on a US board", K_Q, "a");
    expect(fr, "the key marked W on a US board", K_W, "z");
    expect(fr, "the key marked Z on a US board", K_Z, "w");
    hold(fr, K_LSHIFT);
    expect(fr, "Shift held, that same key",      K_Q, "A");
    release(fr, K_LSHIFT);
    aurwl_destroy(fr);

    /* Unquoted values and stray whitespace, because /etc/default/
     * keyboard is edited by people and by other programs. */
    printf("\n  the same file written without quotes\n");
    aurwl *de = with_layout(kbpath, "XKBLAYOUT=de\n  XKBVARIANT = \n");
    expect(de, "the key marked Y on a US board", K_Z, "y");
    aurwl_destroy(de);

    /* A layout that does not exist must leave a WORKING keyboard, not
     * a dead one. Wrong language beats no keys at all. xkbcommon prints
     * its own complaint about the missing file first; that complaint is
     * what this case exists to survive. */
    printf("\n  a layout name that does not exist"
           " (xkbcommon complains above; it should)\n");
    aurwl *bad = with_layout(kbpath, "XKBLAYOUT=\"not-a-real-layout\"\n");
    expect(bad, "a still types something",       K_A, "a");
    aurwl_destroy(bad);

    unlink(kbpath);
    printf("\n");
    if (fail) {
        printf("%d of %d wrong. A password with a capital in it, or any\n",
               fail, checked);
        printf("keyboard that is not American, does not work.\n");
        return 1;
    }
    printf("%d checks: keys resolve through the real keymap, in the\n", checked);
    printf("layout the machine is configured with.\n");
    return 0;
}
