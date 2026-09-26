/* welcometest.c — the question, and what pressing each answer asks for.
 *
 * The welcome panel is the only thing in this product that asks
 * whether AurOS works, and the only place a person can say no. It
 * draws, so tools/targets.c measures it; what THAT cannot see is the
 * thing this file is about: which word each button writes, which
 * screen follows which answer, and -- the one that matters most --
 * that nothing is written at all unless something was pressed.
 *
 * It runs against a scratch directory rather than /run/auros, which is
 * what WELCOME_RUN and WELCOME_STATE are for. Nothing here is root and
 * nothing here touches an EFI variable: the panel's whole privilege is
 * writing one word into a directory its own user owns.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "../src/aurshell/shell.h"
#include "../src/aurshell/foot.h"
#include "../src/aurshell/welcome.h"

static int fail = 0, checked = 0;
static void ok(const char *what, int good)
{
    checked++;
    printf("    %-58s %s\n", what, good ? "ok" : "FAIL");
    if (!good) fail++;
}

static void wipe(void)
{
    unlink(WELCOME_RUN "/answer");
    unlink(WELCOME_ANSWER "/result");
    unlink(WELCOME_ANSWER "/import.log");
    unlink(WELCOME_STATE);
    unlink(WELCOME_FOUND);
}

/* The answer directory, when the fixture puts it inside WELCOME_RUN --
 * which tools/README.md's build line does. It is the root side's, not
 * something the panel made, and it used to be counted as the panel's
 * second file. */
static int is_answer_dir(const char *name)
{
    const char *ans = strrchr(WELCOME_ANSWER, '/');
    return ans && !strncmp(WELCOME_ANSWER, WELCOME_RUN "/", strlen(WELCOME_RUN) + 1)
               && !strcmp(name, ans + 1);
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static int slurp(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t k = fread(out, 1, n - 1, f);
    fclose(f);
    out[k] = 0;
    return (int)k;
}

static void machine(shell_ctx *c)
{
    memset(c, 0, sizeof *c);
    theme_t t; memset(&t, 0, sizeof t);
    shell_theme_load(c, &t);
    c->text_scale = 1.f;
    c->screen_w = 1366;
    c->screen_h = 768 - foot_height(c);
}

/* Press the action whose label is `want`, wherever the layout put it.
 * Pressing by LABEL rather than by index: the whole point of the
 * column fallback is that the buttons move, and a test that pressed
 * "the second one" would follow them silently into the wrong answer. */
static int press(shell_ctx *c, int page, int can_import, const char *want)
{
    welcome_set_page(page);
    welcome_view v = { page, can_import };
    rect b[16];
    int n = welcome_targets(c, c->screen_w, c->screen_h, &v, b, 16);
    /* welcome_targets gives rectangles, not labels, so the press is
     * driven through the same click path the shell uses and the answer
     * is read from what it asked for. Each rectangle is tried in turn
     * and the one that produced the expected word is the answer. */
    for (int i = 0; i < n; i++) {
        unlink(WELCOME_RUN "/answer");
        welcome_set_page(page);
        c->welcome_open = 1;
        welcome_click(c, b[i].x + b[i].w / 2, b[i].y + b[i].h / 2);
        char got[64];
        if (slurp(WELCOME_RUN "/answer", got, sizeof got) > 0 &&
            !strcmp(got, want))
            return 1;
        if (!want[0] && !c->welcome_open) return 1;   /* a close button */
    }
    return 0;
}

int main(void)
{
    mkdir(WELCOME_RUN, 0700);
    /* The root side's directory, which the answers are put into. The
     * build line in tools/README.md places it inside WELCOME_RUN; a
     * test that needs it and does not make it died on its first
     * put() with "No such file or directory". */
    mkdir(WELCOME_ANSWER, 0755);
    wipe();

    printf("\nThe question, and what each answer asks for\n\n");

    shell_ctx c; machine(&c);

    printf("  when it appears at all\n");
    welcome_init(&c);
    ok("a machine with no state file is not asked anything",
       c.welcome_open == 0);
    put(WELCOME_STATE, "phase=done\nconverted=yes\n");
    welcome_init(&c);
    ok("a machine that already answered is not asked again",
       c.welcome_open == 0);
    put(WELCOME_STATE, "phase=declined\n");
    welcome_init(&c);
    ok("nor one that said no", c.welcome_open == 0);
    put(WELCOME_STATE, "phase=not-converted\n");
    welcome_init(&c);
    ok("nor one AurOS was installed onto directly",
       c.welcome_open == 0);
    put(WELCOME_STATE, "phase=asking\n");
    welcome_init(&c);
    ok("and one that is waiting IS asked", c.welcome_open == 1);
    ok("...on the question, not halfway through it",
       welcome_page() == W_ASK);

    printf("\n  and what nothing has asked for\n");
    struct stat st;
    ok("opening it writes no request at all",
       stat(WELCOME_RUN "/answer", &st) != 0);

    printf("\n  the three answers\n");
    ok("\"Yes, it all works\" asks for confirm",
       press(&c, W_ASK, 0, "confirm"));
    ok("...and then says so on screen", welcome_page() == W_WORKING);
    ok("\"No, go back to Windows\" asks for decline",
       press(&c, W_ASK, 0, "decline"));
    /* The import is offered only when firstboot.sh found a Windows to
     * read, so the fixture has to have found one. This is what
     * ferry-detect writes. */
    put(WELCOME_FOUND,
        "{\"device\":\"/dev/sda2\",\"windows\":\"10\",\"clean\":true}\n");
    put(WELCOME_STATE, "phase=asking\n");
    welcome_init(&c);
    ok("\"Bring my files across\" asks for import",
       press(&c, W_CONFIRMED, 1, "import"));

    /* A machine with no Windows worth reading must not be offered an
     * import: a button that fails when pressed is worse than no
     * button, which is the rule this product keeps relearning. */
    unlink(WELCOME_FOUND);
    put(WELCOME_STATE, "phase=asking\n");
    welcome_init(&c);
    welcome_set_page(W_CONFIRMED);
    {
        welcome_view v = { W_CONFIRMED, 0 };
        rect b[16];
        int n = welcome_targets(&c, c.screen_w, c.screen_h, &v, b, 16);
        int asked = 0;
        for (int i = 0; i < n; i++) {
            unlink(WELCOME_RUN "/answer");
            welcome_set_page(W_CONFIRMED);
            c.welcome_open = 1;
            welcome_click(&c, b[i].x + b[i].w / 2, b[i].y + b[i].h / 2);
            char got[64];
            if (slurp(WELCOME_RUN "/answer", got, sizeof got) > 0) asked = 1;
        }
        ok("with no Windows on the disk, nothing offers to import from it",
           n > 0 && !asked);
    }

    printf("\n  letting go without answering\n");
    welcome_set_page(W_ASK);
    c.welcome_open = 1;
    unlink(WELCOME_RUN "/answer");
    welcome_key(&c, 1);                  /* escape */
    ok("escape closes it", c.welcome_open == 0);
    ok("...and answers nothing", stat(WELCOME_RUN "/answer", &st) != 0);
    welcome_set_page(W_ASK);
    c.welcome_open = 1;
    welcome_click(&c, 4, 4);             /* the background */
    ok("pressing the background does nothing", c.welcome_open == 1);
    ok("...and asks for nothing", stat(WELCOME_RUN "/answer", &st) != 0);
    ok("...and is still consumed, so it cannot reach what is behind it",
       welcome_click(&c, 4, 4) == 1);

    /* A DIFFERENT BUTTON, and this check could not fail until it was.
     *
     * It pressed b[0], which for this page IS "Yes, it all works" --
     * so the assertion held whether the in-flight request had been
     * preserved or overwritten with an identical "confirm". A review
     * changed ask_for's O_EXCL to O_TRUNC, so a second press really
     * did replace the request, and this still printed ok. Pressing
     * "No, go back to Windows" and requiring "confirm" to survive is
     * the same check with something to catch. */
    printf("\n  and one request at a time\n");
    unlink(WELCOME_RUN "/answer");
    press(&c, W_ASK, 0, "confirm");
    welcome_set_page(W_ASK);
    c.welcome_open = 1;
    {
        welcome_view v = { W_ASK, 0 };
        rect b[16];
        int n = welcome_targets(&c, c.screen_w, c.screen_h, &v, b, 16);
        int last = n > 0 ? n - 1 : 0;      /* A_NO, the last of the three */
        welcome_click(&c, b[last].x + b[last].w / 2,
                      b[last].y + b[last].h / 2);
        char got[64];
        int k = slurp(WELCOME_RUN "/answer", got, sizeof got);
        ok("a second press while one is in flight does not replace it",
           n >= 2 && k > 0 && !strcmp(got, "confirm"));
    }

    printf("\n  reading the answer back\n");
    unlink(WELCOME_RUN "/answer");
    press(&c, W_ASK, 0, "confirm");
    c.welcome_open = 1;
    ok("nothing moves while there is no answer yet",
       welcome_step(&c) == 0 && welcome_page() == W_WORKING);
    put(WELCOME_ANSWER "/result", "request=confirm\nresult=ok\n");
    ok("an answer moves it on", welcome_step(&c) == 1);
    ok("...to the screen that offers to bring her files",
       welcome_page() == W_CONFIRMED);

    unlink(WELCOME_RUN "/answer");
    press(&c, W_ASK, 0, "decline");
    c.welcome_open = 1;
    put(WELCOME_ANSWER "/result", "request=decline\nresult=ok\n");
    welcome_step(&c);
    ok("saying no reaches the screen that says Windows is next",
       welcome_page() == W_DECLINED);

    unlink(WELCOME_RUN "/answer");
    press(&c, W_ASK, 0, "confirm");
    c.welcome_open = 1;
    put(WELCOME_ANSWER "/result",
        "request=confirm\nresult=failed\nnote=this computer would not take it\n");
    welcome_step(&c);
    ok("a refusal reaches the screen that says so, not the one that lies",
       welcome_page() == W_TROUBLE);

    /* A result file caught halfway through being written has no
     * result line. answer.sh writes it whole and renames, so this is
     * the belt to that braces -- and the failure it prevents is the
     * panel saying "done" because it read an empty file. */
    unlink(WELCOME_RUN "/answer");
    press(&c, W_ASK, 0, "confirm");
    c.welcome_open = 1;
    put(WELCOME_ANSWER "/result", "request=confirm\n");
    ok("a half-written answer is not an answer",
       welcome_step(&c) == 0 && welcome_page() == W_WORKING);

    /* THE WHOLE OF ITS PRIVILEGE, checked rather than described. A
     * panel that quietly wrote somewhere else would be a panel whose
     * comment about never needing to be root had stopped being true. */
    printf("\n  and the panel never needs to be root\n");
    {
        unlink(WELCOME_RUN "/answer");
        unlink(WELCOME_ANSWER "/result");
        DIR *d = opendir(WELCOME_RUN);
        int before = 0;
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.' && !is_answer_dir(e->d_name)) before++;
        if (d) closedir(d);
        press(&c, W_ASK, 0, "confirm");
        welcome_step(&c);
        d = opendir(WELCOME_RUN);
        int after = 0, only_answer = 1;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || is_answer_dir(e->d_name)) continue;
            after++;
            if (strcmp(e->d_name, "answer") && strcmp(e->d_name, "first.state") &&
                strcmp(e->d_name, "found.json"))
                only_answer = 0;
        }
        if (d) closedir(d);
        ok("one press leaves one new file behind, and it is the request",
           after == before + 1 && only_answer);
    }

    /* AN ANSWER TO ANOTHER QUESTION IS NOT AN ANSWER. The panel did
     * not used to remember what it asked, so a result left over from
     * a previous session -- or one a second request produced -- was
     * acted on, and a confirm of unknown outcome showed "Your files
     * are here. Open Files to see them." */
    printf("\n  and an answer to something it did not ask\n");
    unlink(WELCOME_RUN "/answer");
    press(&c, W_ASK, 0, "confirm");
    c.welcome_open = 1;
    put(WELCOME_ANSWER "/result", "request=import\nresult=ok\n");
    ok("a result for another request is left alone",
       welcome_step(&c) == 0 && welcome_page() == W_WORKING);
    put(WELCOME_ANSWER "/result", "request=confirm\nresult=zzz\n");
    welcome_step(&c);
    ok("and a result nothing recognises is trouble, not silence",
       welcome_page() == W_TROUBLE);

    wipe();
    printf("\n");
    if (fail) {
        printf("%d of %d wrong. This is the only place anybody can say\n",
               fail, checked);
        printf("whether AurOS works, or say no to it.\n");
        return 1;
    }
    printf("%d checks: it appears only while the question is open, each\n",
           checked);
    printf("button asks for one word, and closing it answers nothing.\n");
    return 0;
}
