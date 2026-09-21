/* session.h — see session.c. The only header that knows both the
 * compositor and the shell exist. */
#ifndef AUROS_SESSION_H
#define AUROS_SESSION_H

#include "shell.h"
#include "anim.h"

/* Named, not included: shell.h must stay a header the archetypes can
 * read without knowing a compositor exists. */
typedef struct aurwl_win aurwl_win;

/* Reconcile the compositor's window list into c->wins, adopt
 * placeholders, and configure clients. Call once per frame, after
 * dispatching the compositor and before painting. */
void session_sync(shell_ctx *c, void (*present)(shell_ctx *, int),
                  void (*removed)(shell_ctx *, int));

/* Route a pointer or key to the window under it. Each returns 1 if a
 * client took the event, in which case the archetype must not also act
 * on it -- two responses to one click is worse than none. */
int  session_motion(shell_ctx *c, int x, int y);
int  session_button(shell_ctx *c, int x, int y, uint32_t button, int pressed);
int  session_scroll(shell_ctx *c, int x, int y, int horizontal, double step);
int  session_key(shell_ctx *c, int code, int pressed);

/* The compositor window a slot's `wid` stands for, or NULL if that
 * window has gone. Every caller that holds a window across even one
 * frame must go through this rather than keeping a pointer: a client
 * can be destroyed between any two frames, and a stale aurwl_win* is
 * a use-after-free reachable by waiting. */
aurwl_win *session_win(shell_ctx *c, uint32_t wid);

/* Menus and dropdowns, painted above everything the archetype drew. */
void session_paint_popups(shell_ctx *c, surface *fb);

/* Assign to shell_ctx.spawn. */
int  session_spawn(shell_ctx *c, const char *argv_blob, int n_args);

#endif
