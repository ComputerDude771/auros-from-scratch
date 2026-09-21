/* sorry.c — what she sees when the desktop does not start.
 *
 * Until this existed, aurshell.service said
 *
 *     OnFailure=getty@tty1.service
 *
 * and a comment above it explaining that this was better than leaving
 * the user with a black screen and no way in. It was better. It was
 * also a login prompt, on a machine whose owner has never been told
 * the account name, has never been told the password, and whose
 * password the image expires on purpose so that it cannot be used even
 * by someone who guesses it. docs/EASY.md rule 1: there is no task in
 * this product whose answer is "open a terminal". A black screen and a
 * console she cannot use are the same screen to her.
 *
 * So this is what she gets instead: one sentence about what happened,
 * one about what the machine is doing, one about what she can do. No
 * buttons, because rule 5 says nothing may require a gesture and
 * because a program that runs when the desktop has failed should not
 * also depend on reading her mouse. The unit restarts the desktop when
 * this exits, which is what makes "it will try again" true rather than
 * consoling.
 *
 * WHY IT IS ITS OWN PROGRAM
 *
 * Because aurshell is the thing that just failed. Running it again to
 * apologise for itself works only in the cases where it would have
 * worked anyway. This links three files -- the mode-setter, the
 * rasterizer and the font engine -- and does nothing else: no policy,
 * no theme parsing, no application scan, no compositor, no input. The
 * fewer things it needs, the more of the failures it can survive to
 * describe.
 *
 * It deliberately hardcodes its two colours, for the same reason the
 * band in foot.c does: the screen that explains a failure must not be
 * styled by a file that may be what failed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kms.h"
#include "draw.h"
#include "../common/font.h"
#include "../common/png.h"

#define INK   0xECEFF2u
#define DIM   0x9BA3ABu
#define PAPER 0x14171Bu
#define RULE  0x6E7680u

/* How long she reads it before the machine tries again. Long enough to
 * read three sentences without hurrying, short enough that a person
 * watching does not conclude it has stopped for good. */
#define DWELL_SECONDS 40

/* The fonts the image ships, most wanted first. Not the theme's: the
 * theme is one of the things that can be the reason we are here. */
static font *pick(float px)
{
    static const char *TRY[] = {
        "/usr/share/auros/fonts/ui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL
    };
    for (int i = 0; TRY[i]; i++) {
        font *f = font_load(TRY[i], px);
        if (f) return f;
    }
    return NULL;
}

static void line(surface *s, font *f, int x, int *y, const char *t,
                 uint32_t col, float a)
{
    if (!f) return;
    if (t && *t)
        font_draw(f, s->px, s->w, s->h, s->stride,
                  (float)x, (float)*y + font_ascent(f), t, col, a);
    *y += (int)(font_line_height(f) * 1.5f);
}

static void paint(surface *s);

int main(int argc, char **argv)
{
    const char *card = NULL, *png = NULL;
    int pw = 1024, ph = 600;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        /* Draw it to a file instead of a screen. This is the one screen
         * in the product that only appears when everything else has
         * gone wrong, so looking at it must not require arranging for
         * everything else to go wrong. */
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            pw = atoi(argv[++i]); ph = atoi(argv[++i]);
        }
    }

    if (png) {
        surface *o = surface_new(pw, ph);
        if (!o) return 1;
        paint(o);
        int rc = png_write_rgb(png, o->px, o->w, o->h);
        surface_free(o);
        if (rc < 0) { fprintf(stderr, "aursorry: cannot write %s\n", png); return 1; }
        printf("%dx%d -> %s\n", pw, ph, png);
        return 0;
    }

    kms_display *d = kms_open(card);
    if (!d) {
        /* No display either. Say it where a person with a serial cable
         * can read it, and do not spin. */
        fprintf(stderr, "aursorry: no display to draw on\n");
        sleep(DWELL_SECONDS);
        return 0;
    }

    surface *s = kms_back_surface(d);
    if (!s) { kms_close(d); return 0; }
    paint(s);
    kms_flip(d);

    /* Paint once and wait. There is nothing to animate and nothing to
     * read; a program in this position should do as little as it can. */
    struct timespec t = { DWELL_SECONDS, 0 };
    while (nanosleep(&t, &t) < 0) { }

    kms_close(d);
    return 0;
}

static void paint(surface *s)
{

    /* Scaled off the panel, so the words are the same size relative to
     * the screen on a 1024x600 netbook and a 1920x1080 laptop. The
     * floor matters more than the ceiling: this is the screen a person
     * reads when they are already worried. */
    float k = (float)s->h / 600.f;
    if (k < 1.f) k = 1.f;
    if (k > 2.2f) k = 2.2f;
    font *big  = pick(34.f * k);
    font *body = pick(19.f * k);

    surface_fill(s, 0xFF000000u | PAPER);

    int x = s->w / 8;
    int y = s->h / 4;
    line(s, big, x, &y, "This computer's desktop did not start.", INK, 1.f);

    if (big) y += (int)(10.f * k);
    draw_hrule(s, x, y, s->w - 2 * x, 1, RULE, 0.7f);
    if (body) y += (int)(font_line_height(body) * 1.2f);

    line(s, body, x, &y, "Nothing you did caused this, and nothing you", INK, 0.95f);
    line(s, body, x, &y, "have saved has been lost.", INK, 0.95f);
    y += (int)(12.f * k);
    line(s, body, x, &y, "The computer is going to try again on its own", INK, 0.95f);
    line(s, body, x, &y, "in a moment. You do not have to press anything.", INK, 0.95f);
    y += (int)(12.f * k);
    line(s, body, x, &y, "If this screen keeps coming back, hold the power", DIM, 0.9f);
    line(s, body, x, &y, "button until the computer goes quiet, wait, and", DIM, 0.9f);
    line(s, body, x, &y, "switch it on again. If it still happens, this one", DIM, 0.9f);
    line(s, body, x, &y, "needs somebody to look at it.", DIM, 0.9f);

    if (big)  font_free(big);
    if (body) font_free(body);
}
