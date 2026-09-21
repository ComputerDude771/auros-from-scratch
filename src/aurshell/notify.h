/* notify.h — when a program has something to tell her.
 *
 * Nothing in this image implemented org.freedesktop.Notifications, and
 * that is not a missing nicety. It is the one way a Linux desktop
 * application has of saying anything outside its own window, and every
 * toolkit on the machine calls it:
 *
 *   the browser     a download finished, or failed
 *   her files       the USB stick can be taken out now
 *   the store       the program you asked for is installed
 *   printers        out of paper; the job was sent
 *
 * With no server on the bus, every one of those calls comes back
 * "org.freedesktop.DBus.Error.ServiceUnknown" and the application
 * shrugs. She takes the USB stick out too early, waits for a download
 * that finished ten minutes ago, and presses Install a second time.
 * Nothing anywhere says anything. It is the same shape as every other
 * bug this product keeps finding in itself, one layer out: a thing
 * that looks like it works and does not.
 *
 * WHY THE SHELL AND NOT A PACKAGE
 *
 * The two usual answers do not fit. dunst is X11. mako wants
 * wlr-layer-shell, which this compositor does not implement and should
 * not have to. Both would draw in their own colours, at their own text
 * size, in their own words -- on a machine whose whole argument is
 * that those three things are hers to set.
 *
 * So the shell is the server. It already owns the screen, the theme,
 * her chosen text size and the word list in docs/EASY.md.
 *
 * WHY libdbus
 *
 * The same call the compositor made about libwayland-server. The wire
 * format is not the interesting part, and hand-rolling a parser for
 * a{sv} variants arriving from every program on the machine is a
 * security surface, not a craft exercise. Everything a person SEES
 * here is this program's.
 *
 * TIME AND docs/EASY.md RULE 5
 *
 * A notification goes away by itself, and rule 5 says nothing may be
 * time-limited. It does not break the rule for the same reason the
 * indicator does not: the rule exists so nothing she NEEDS is on a
 * timer. What a notification says is never the only way to learn it --
 * the download is in her Downloads folder either way. What it does
 * have is a long life (eight seconds, not two) and a press-anywhere
 * dismissal, because something that vanishes while she is still
 * reading it is worse than nothing.
 */
#ifndef AUROS_NOTIFY_H
#define AUROS_NOTIFY_H

#include "shell.h"

#define NOTIFY_MAX      4      /* on screen at once; the rest queue   */
#define NOTIFY_SUMMARY  128
#define NOTIFY_BODY     320
#define NOTIFY_APP      64
/* docs/EASY.md rule 4: everything she has to press is at least this
 * on its shorter side, at 1024x600. A card IS the press -- pressing it
 * anywhere dismisses it -- so this is the card's floor. */
#define NOTIFY_TARGET   44

/* Open the connection and claim the name, if it is not already
 * claimed. Idempotent and rate-limited, so the host may call it every
 * frame: it retries until it succeeds, because the bus the desktop
 * ends up on is not the bus that exists when the shell starts (see
 * run.c). Returns 1 while the service is ours. */
int  notify_open(void);
/* Give the name up -- used when the shell moves to logind's bus and
 * has to re-announce itself there. */
void notify_close(void);
void notify_fini(void);

/* For the host's poll(). -1 when there is no connection. */
int  notify_fd(void);
/* Read whatever arrived. 1 if the screen has something new to show. */
int  notify_pump(shell_ctx *c);
/* Once per pass: retire the ones whose time is up. 1 if changed. */
int  notify_step(shell_ctx *c);

/* How many are showing. The host repaints while this is not zero. */
int  notify_showing(void);

/* ── the geometry contract ──────────────────────────────────────────
 *
 * The same one the band and the wifi panel keep: a pure function of a
 * VIEW and a panel size, so tools/targets.c can measure every card at
 * every text size on a machine with no D-Bus at all. A geometry
 * function that can only be asked about the state the machine happens
 * to be in is a geometry function nothing can check. */
typedef struct {
    int n;              /* how many cards are up, 0..NOTIFY_MAX */
    float text_scale;   /* hers */
    int   foot_h;       /* the band's height, which they sit above */
} notify_view;

typedef struct {
    rect card[NOTIFY_MAX];
    int  n;
} notify_geom;

void notify_layout(int sw, int sh, const notify_view *v, notify_geom *g);
/* The live view, for the host and for a harness that wants the real
 * one rather than an invented one. */
void notify_view_now(const shell_ctx *c, notify_view *v);

int  notify_targets(const shell_ctx *c, int sw, int sh,
                    const notify_view *v, rect *out, int max);

/* Painted above the archetype and below the band -- the way out is
 * never covered by them. */
void notify_paint(shell_ctx *c, surface *s, shell_fonts *f);
/* A press. Returns 1 if it landed on a card, which dismisses it. */
int  notify_click(shell_ctx *c, int x, int y);

/* Put one up from inside the shell itself, in her words. The battery
 * warning and "this program would not start" are notifications, not
 * indicator flashes: they are things she may want to read twice. */
void notify_local(const char *summary, const char *body);

#endif
