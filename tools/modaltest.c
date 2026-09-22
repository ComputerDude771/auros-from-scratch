/* modaltest.c — is a panel actually modal?
 *
 * There are five things in this shell that cover the desktop: help,
 * the wifi panel, Settings, the headphones panel, and the question
 * that asks what to do with the machine. "Modal" is a promise with
 * three parts, and every one of them was broken in a different place:
 *
 *   1. ONLY ONE AT A TIME. Each branch of foot_click() used to clear
 *      the others by name, and the headphones panel -- which opens
 *      from inside Settings rather than from the band -- was on none
 *      of those lists. Settings, then Headphones, then Settings, and
 *      both were open: she saw one and pressed the other, because
 *      painting goes back to front and hit-testing goes front to back.
 *
 *   2. NOTHING BEHIND IT ANSWERS. Four places in the event loop
 *      decided whether to pass an event through, and each kept its own
 *      hand-written list of which panels were open. The wheel knew
 *      about three of the five, the right button about four, the key
 *      path about three, the button release about two. So with the
 *      power question on screen a person could scroll the application
 *      hidden behind it and press its keyboard shortcuts.
 *
 *   3. THERE IS A WAY OUT. docs/EASY.md rule 6. The power question had
 *      no key that dismissed it at all: a full-screen state, covering
 *      everything she was doing, with no way out for anyone not using
 *      a mouse.
 *
 * Parts 1 and 3 are behaviour and are exercised below. Part 2 lives
 * inside main()'s event loop, which cannot be linked into a harness --
 * so it is checked by reading main.c and insisting that the input path
 * names SHELL_PANEL_OPEN rather than any individual panel. That is a
 * blunt check, and it is the one that would have caught all four
 * drifts, because the drift IS the individual name.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/aurshell/shell.h"
#include "../src/aurshell/foot.h"

static int fail = 0, checked = 0;

static void ok(const char *what, int good)
{
    checked++;
    printf("    %-58s %s\n", what, good ? "ok" : "FAIL");
    if (!good) fail++;
}

/* Every flag SHELL_PANEL_OPEN is supposed to cover, by name and by
 * address, so the two can be checked against each other. */
/* THE ONES THE BAND OPENS. The mutual-exclusion sweep below drives
 * foot_open_only(), so it can only ask about panels that have a button
 * -- and the welcome question deliberately does not: it opens itself,
 * once per session, until it has been answered. It is in flag_any()
 * instead, because SHELL_PANEL_OPEN must still cover it. */
static int *flag_of(shell_ctx *c, int i)
{
    switch (i) {
    case 0: return &c->help_open;
    case 1: return &c->net_open;
    case 2: return &c->settings_open;
    case 3: return &c->bt_open;
    case 4: return &c->power_open;
    }
    return NULL;
}

/* Every flag that covers the desktop, whoever opens it. */
static int *flag_any(shell_ctx *c, int i)
{
    if (i < 5) return flag_of(c, i);
    if (i == 5) return &c->welcome_open;
    return NULL;
}
static const char *NAMES[6] = { "help", "wifi", "settings",
                                "headphones", "the power question",
                                "the welcome question" };
#define N_PANELS 5        /* the ones the band opens */
#define N_FLAGS  6        /* every one that covers the desktop */

static void fresh(shell_ctx *c)
{
    memset(c, 0, sizeof *c);
    theme_t t = {0};
    shell_theme_load(c, &t);
    c->text_scale = 1.f;
    c->foot_hover = -1;
    c->screen_w = 1024;
    c->screen_h = 600 - foot_height(c);
    c->allow_settings = 1;
    c->allow_network  = 1;
}

/* ── 1. one at a time ───────────────────────────────────────────── */

static void one_at_a_time(void)
{
    printf("  only one thing covers the desktop at a time\n");
    for (int a = 0; a < N_PANELS; a++) {
        shell_ctx c; fresh(&c);
        foot_open_only(&c, flag_of(&c, a));
        int n = 0;
        for (int i = 0; i < N_PANELS; i++) n += *flag_of(&c, i) ? 1 : 0;
        char w[96];
        snprintf(w, sizeof w, "opening %s opens exactly one thing", NAMES[a]);
        ok(w, n == 1 && *flag_of(&c, a));
        snprintf(w, sizeof w, "...and SHELL_PANEL_OPEN sees %s", NAMES[a]);
        ok(w, SHELL_PANEL_OPEN(&c) != 0);
    }

    /* The case that actually happened: a second panel opened while the
     * first is up. */
    for (int a = 0; a < N_PANELS; a++)
        for (int b = 0; b < N_PANELS; b++) {
            if (a == b) continue;
            shell_ctx c; fresh(&c);
            foot_open_only(&c, flag_of(&c, a));
            foot_open_only(&c, flag_of(&c, b));
            int n = 0;
            for (int i = 0; i < N_PANELS; i++) n += *flag_of(&c, i) ? 1 : 0;
            if (n != 1 || !*flag_of(&c, b)) {
                printf("    %s over %s leaves %d open           FAIL\n",
                       NAMES[b], NAMES[a], n);
                fail++;
            }
            checked++;
        }
    printf("    %-58s %s\n", "every pair of panels, in both orders", "ok");

    shell_ctx c; fresh(&c);
    ok("with nothing open, nothing is covering the desktop",
       !SHELL_PANEL_OPEN(&c));
}

/* ── 2. the way out, from the keyboard ──────────────────────────── */

#define K_ESC 1
#define K_RET 28
#define K_UP  103
#define K_DN  108

static void a_way_out(void)
{
    printf("\n  every one of them can be left without a mouse\n");

    shell_ctx c; fresh(&c);
    foot_open_only(&c, &c.help_open);
    ok("Escape closes help", foot_key(&c, K_ESC) && !c.help_open);

    fresh(&c);
    foot_open_only(&c, &c.help_open);
    ok("so does any other key, the way any click does",
       foot_key(&c, 30 /* 'a' */) && !c.help_open);

    fresh(&c);
    foot_open_only(&c, &c.power_open);
    ok("Escape closes the power question",
       foot_key(&c, K_ESC) && !c.power_open);
    ok("...and asks the machine to do nothing", c.want_power_off == 0);

    /* A Return with nothing chosen must not turn the machine off. */
    fresh(&c);
    foot_open_only(&c, &c.power_open);
    foot_key(&c, K_RET);
    ok("Return with nothing chosen does nothing at all",
       c.power_open && c.want_power_off == 0);

    /* Arrow down onto the first choice, then act on it. The id it
     * produces must be the id a CLICK on that same rectangle produces,
     * or the keyboard and the pointer are two different programs. */
    fresh(&c);
    foot_open_only(&c, &c.power_open);
    rect pb[FOOT_POWER_MAX]; int pw[FOOT_POWER_MAX];
    int pn = foot_power_buttons(&c, c.screen_w,
                                c.screen_h + foot_height(&c), pb, pw);
    ok("the power question offers something to choose", pn > 0);

    for (int i = 0; i < pn; i++) {
        shell_ctx k; fresh(&k);
        foot_open_only(&k, &k.power_open);
        for (int j = 0; j <= i; j++) foot_key(&k, K_DN);
        foot_key(&k, K_RET);

        shell_ctx m; fresh(&m);
        foot_open_only(&m, &m.power_open);
        foot_click(&m, pb[i].x + pb[i].w / 2, pb[i].y + pb[i].h / 2);

        char w[96];
        snprintf(w, sizeof w, "choice %d: the keyboard and the mouse agree", i + 1);
        ok(w, k.want_power_off == m.want_power_off &&
              k.power_open == m.power_open);
    }

    /* Up from nothing wraps to the last choice, which is the way out.
     * A person who presses Up first must not land on "Turn it off". */
    fresh(&c);
    foot_open_only(&c, &c.power_open);
    foot_key(&c, K_UP);
    ok("Up from nothing lands on the last choice, not the first",
       c.power_sel == pw[pn - 1]);

    /* While it is up, EVERY key is taken. One that fell through would
     * reach an application she cannot see. */
    fresh(&c);
    foot_open_only(&c, &c.power_open);
    int leaked = 0;
    for (int k = 1; k < 200; k++) {
        shell_ctx t; fresh(&t);
        foot_open_only(&t, &t.power_open);
        if (!foot_key(&t, k)) leaked++;
    }
    ok("no key escapes the power question", leaked == 0);
    (void)leaked;

    /* And the highlight survives the next event. foot_motion() runs
     * after every input event and rebuilds foot_hover from the pointer,
     * so without care the selection made with the arrow keys is erased
     * by the very next keypress -- including the Return meant to act
     * on it. */
    fresh(&c);
    foot_open_only(&c, &c.power_open);
    foot_key(&c, K_DN);
    int sel = c.power_sel;
    foot_motion(&c, 0, 0);                    /* pointer in the corner */
    ok("the pointer moving elsewhere does not erase what she chose",
       c.power_sel == sel && c.foot_hover == -100 - sel);
}

/* ── 3. nothing behind it answers ───────────────────────────────── */

/* shell.h, read as text: every `int something_open;` field in the
 * context struct, and the body of the SHELL_PANEL_OPEN macro. The
 * first is the truth about how many ways the desktop can be covered;
 * the second is what the shell believes about it. */
#define MAX_OPEN 32
static int open_fields(char names[MAX_OPEN][64], char *macro, size_t mn)
{
    FILE *f = fopen("src/aurshell/shell.h", "r");
    if (!f) f = fopen("../src/aurshell/shell.h", "r");
    if (!f) return -1;

    char line[1024];
    int n = 0, in_macro = 0;
    size_t used = 0;
    macro[0] = 0;

    while (fgets(line, sizeof line, f)) {
        if (in_macro) {
            size_t l = strlen(line);
            if (used + l < mn) { memcpy(macro + used, line, l); used += l;
                                 macro[used] = 0; }
            if (!strstr(line, "\\")) in_macro = 0;
            continue;
        }
        if (strstr(line, "#define SHELL_PANEL_OPEN")) {
            size_t l = strlen(line);
            if (used + l < mn) { memcpy(macro + used, line, l); used += l;
                                 macro[used] = 0; }
            if (strstr(line, "\\")) in_macro = 1;
            continue;
        }
        /* `    int   help_open;` -- and nothing else. The macro body
         * itself contains `_open` too, which is why it is consumed
         * above rather than fallen through to here. */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "int", 3) != 0) continue;
        p += 3;
        if (*p != ' ' && *p != '\t') continue;
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_') p++;
        if (*p != ';') continue;
        size_t l = (size_t)(p - start);
        if (l < 6 || l >= 64) continue;
        if (strncmp(start + l - 5, "_open", 5) != 0) continue;
        if (n >= MAX_OPEN) { fclose(f); return -2; }
        memcpy(names[n], start, l); names[n][l] = 0;
        n++;
    }
    fclose(f);
    return n;
}

static void panel_flags_are_covered(void)
{
    char names[MAX_OPEN][64], macro[2048];
    int n = open_fields(names, macro, sizeof macro);

    if (n == -1) { printf("    (run me from the top of the repository)            SKIP\n");
                   return; }
    if (n < 0 || n == 0 || !macro[0]) {
        ok("shell.h names the panel flags and the macro", 0);
        return;
    }

    /* Each field is named by the macro. */
    int missing = 0;
    for (int i = 0; i < n; i++) {
        char want[80];
        snprintf(want, sizeof want, "->%.62s", names[i]);
        if (!strstr(macro, want)) {
            printf("      SHELL_PANEL_OPEN does not cover %s\n", names[i]);
            missing++;
        }
    }
    ok("SHELL_PANEL_OPEN names every *_open field in shell.h", !missing);

    /* And nothing the struct does not have -- a renamed field that
     * stayed in the macro would otherwise pass the check above while
     * covering nothing. */
    int stray = 0;
    for (const char *p = strstr(macro, "->"); p; p = strstr(p + 2, "->")) {
        const char *q = p + 2;
        const char *s = q;
        while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
               (*q >= '0' && *q <= '9') || *q == '_') q++;
        size_t l = (size_t)(q - s);
        if (l == 0 || l >= 64) continue;
        char got[64];
        memcpy(got, s, l); got[l] = 0;
        int found = 0;
        for (int i = 0; i < n; i++) if (!strcmp(got, names[i])) found = 1;
        if (!found) { printf("      SHELL_PANEL_OPEN names %s, which shell.h has not\n", got);
                      stray++; }
    }
    ok("and names nothing shell.h does not have", !stray);

    /* And this test knows about all of them. N_FLAGS and flag_any()
     * are hand-written here; if shell.h grows a seventh panel, the
     * loop below would silently keep testing six. */
    char w[96];
    snprintf(w, sizeof w, "and this test covers all %d of them", n);
    ok(w, n == N_FLAGS);
}

/* The input loop, read as text. Every place that decides whether an
 * event reaches what is behind a panel must ask SHELL_PANEL_OPEN, not
 * name panels. Four of them named panels, and all four were wrong. */
static void nothing_behind(void)
{
    printf("\n  nothing behind a panel answers an event\n");

    FILE *f = fopen("src/aurshell/main.c", "r");
    if (!f) f = fopen("../src/aurshell/main.c", "r");
    if (!f) {
        printf("    (run me from the top of the repository)            SKIP\n");
        return;
    }
    char line[1024];
    int lineno = 0, offenders = 0;
    /* A panel named anywhere in main.c, with two exemptions:
     *
     *   the DISPATCH chain, which has to name them -- that is what it
     *   is for -- and is recognised by the panel's own handler being
     *   on the same line;
     *
     *   the TRANSITION blocks, which notice a panel opening or closing
     *   so they can start and stop its work, and are recognised by the
     *   `_open_last` bookkeeping or the _opened/_closed call.
     *
     * Everything else that names a panel is a guard, and a guard that
     * names panels is the drift this file exists to catch. Scanning
     * the whole file rather than a region is deliberate: the region
     * markers are exactly the sort of thing that goes stale. */
    static const char *F[] = { "c.help_open", "c.net_open",
                               "c.settings_open", "c.bt_open",
                               "c.power_open" };
    static const char *EXEMPT[] = {
        "foot_key", "net_key", "settings_key", "bt_key",
        "net_click", "settings_click", "bt_click",
        "_open_last", "_opened(", "_closed(",
    };
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (strchr(line, '#')) continue;                 /* not a guard */
        int names = 0;
        for (size_t i = 0; i < sizeof F / sizeof F[0]; i++)
            if (strstr(line, F[i])) names = 1;
        if (!names) continue;
        int exempt = 0;
        for (size_t i = 0; i < sizeof EXEMPT / sizeof EXEMPT[0]; i++)
            if (strstr(line, EXEMPT[i])) exempt = 1;
        if (exempt) continue;

        printf("    main.c:%d names a panel in a guard:                FAIL\n",
               lineno);
        printf("      %s", line);
        printf("      use SHELL_PANEL_OPEN(&c) -- a list kept in five\n"
               "      places is a list that will be kept in four.\n");
        offenders++;
    }
    fclose(f);
    checked++;
    if (offenders) fail++;
    else printf("    %-58s %s\n",
                "every guard in main.c asks SHELL_PANEL_OPEN", "ok");

    /* And the macro itself covers every flag there is.
     *
     * This used to be a loop over the six flags named below and a
     * comment claiming the macro was therefore complete, which it did
     * not check at all: flag_any() is a hand-written list, so a
     * SEVENTH panel forgotten there would have been exactly as
     * invisible as the sixth was before somebody noticed it. A test
     * whose coverage comes from the same hand that wrote the thing
     * under test is a test that agrees with itself.
     *
     * So the list is taken from shell.h instead -- every `int
     * something_open;` in the context struct -- and three things are
     * insisted on: the macro names each one, the macro names nothing
     * else, and the count matches N_FLAGS below. Add a panel and
     * forget the macro, and this fails by name. */
    panel_flags_are_covered();

    shell_ctx c;
    for (int i = 0; i < N_FLAGS; i++) {
        fresh(&c);
        *flag_any(&c, i) = 1;
        char w[96];
        snprintf(w, sizeof w, "SHELL_PANEL_OPEN covers %s", NAMES[i]);
        ok(w, SHELL_PANEL_OPEN(&c) != 0);
    }
}

int main(void)
{
    printf("\nIs a panel actually modal?\n\n");
    one_at_a_time();
    a_way_out();
    nothing_behind();

    printf("\n");
    if (fail) {
        printf("%d of %d wrong. Something is on screen that she can see\n",
               fail, checked);
        printf("through, press through, or cannot get out of.\n");
        return 1;
    }
    printf("%d checks: a panel covers the desktop, takes every event,\n", checked);
    printf("and can always be left.\n");
    return 0;
}
