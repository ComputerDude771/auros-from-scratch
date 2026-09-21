/* aurwl.h — the Wayland server AurOS runs applications on.
 *
 * Until this existed, every window on an AurOS desktop was a picture of
 * a window. The shell painted a title bar, a body and a shadow, and
 * nothing was behind it, because there was no way for another process
 * to hand us pixels. A desktop that cannot run a browser is not a
 * desktop.
 *
 * WHY A WAYLAND SERVER AND NOT X11
 *   We already own the two hard ends of the pipeline: kms.c scans out,
 *   and the shell reads evdev. What was missing was only the middle --
 *   a protocol by which another process gives us a buffer and receives
 *   input. That is exactly what Wayland is. Taking X11 instead would
 *   mean adopting Xorg, its drivers and a window manager: millions of
 *   lines to solve a problem we had already solved at both ends, and an
 *   input and output model the rest of the world is leaving.
 *
 * WHY libwayland-server AND NOT wlroots
 *   wlroots would replace kms.c and our evdev handling -- the two
 *   pieces most specifically ours, and the two that make the shell come
 *   up on a 2013 laptop with no GPU driver worth the name. It would
 *   also pin us to its API breaks. libwayland-server is not a framework
 *   in that sense: it is the reference implementation of the wire
 *   format and object lifetimes, the way libc is the reference
 *   implementation of write(2). We take the wire, we keep the policy.
 *
 * WHAT THE SHELL STILL OWNS
 *   Everything visible. aurwl never draws. It hands the shell a
 *   `surface *` per window -- the same CPU pixel buffer type draw.c has
 *   always composited -- and takes back geometry and input. The six
 *   archetypes did not have to change to gain real windows, which is
 *   the test of whether the seam was in the right place.
 */
#ifndef AUROS_AURWL_H
#define AUROS_AURWL_H

#include <stdint.h>
#include <sys/types.h>
#include "../aurshell/draw.h"

typedef struct aurwl     aurwl;
typedef struct aurwl_win aurwl_win;

/* ── lifecycle ──────────────────────────────────────────────────── */

/* Creates the display and binds an auto-named socket in XDG_RUNTIME_DIR.
 * `w`/`h` and `refresh_mhz` describe the one output we advertise.
 * Returns NULL with a message on stderr. */
aurwl      *aurwl_create(int w, int h, int refresh_mhz);
void        aurwl_destroy(aurwl *c);

/* The value a client needs in WAYLAND_DISPLAY. aurwl_spawn() sets it. */
const char *aurwl_socket(const aurwl *c);

/* Poll this alongside the shell's input fds; call aurwl_dispatch() when
 * it is readable, and once per frame regardless so timers and flushes
 * are not starved. */
int         aurwl_fd(const aurwl *c);
void        aurwl_dispatch(aurwl *c);

/* Tell clients the output changed size (VT return, mode set). */
void        aurwl_resize_output(aurwl *c, int w, int h);

/* ── windows ────────────────────────────────────────────────────── */

/* Mapped toplevels, oldest first. The list is stable within a frame;
 * a dispatch may add or remove entries, so re-read indices after one. */
int         aurwl_window_count(const aurwl *c);
aurwl_win  *aurwl_window_at(const aurwl *c, int i);

/* Monotonic id, so the shell can match its own window records across
 * frames without holding a pointer over a dispatch. */
uint32_t    aurwl_win_id(const aurwl_win *w);
const char *aurwl_win_title(const aurwl_win *w);
const char *aurwl_win_app_id(const aurwl_win *w);

/* The window's current content: our own copy of the client's last
 * committed buffer, so it stays valid and unchanging for as long as the
 * shell needs to paint it. NULL before the first buffer arrives. */
surface    *aurwl_win_content(aurwl_win *w);

/* What the client asked for, before the shell imposes anything. Zero
 * until the first buffer. A layout may use it to size a new window
 * sensibly instead of guessing. */
void        aurwl_win_pref_size(const aurwl_win *w, int *w_out, int *h_out);

/* The shell's decision. Sent to the client as an xdg_toplevel configure;
 * repeated identical calls are dropped, so calling every frame is free.
 * A zero width or height lets the client pick that dimension. */
void        aurwl_win_configure(aurwl_win *w, int width, int height,
                                int activated, int maximized, int fullscreen);

/* Ask politely (the client may prompt about unsaved work), or don't. */
void        aurwl_win_close(aurwl_win *w);
void        aurwl_win_kill(aurwl_win *w);

/* ── popups ─────────────────────────────────────────────────────── */

/* Menus and dropdowns, which are separate surfaces the client positions
 * relative to a parent. The shell paints them above their parent and
 * must route input to them first, or a browser has no context menu. */
int         aurwl_win_is_popup(const aurwl_win *w);
aurwl_win  *aurwl_win_parent(const aurwl_win *w);
void        aurwl_win_popup_offset(const aurwl_win *w, int *x, int *y);

/* ── input ──────────────────────────────────────────────────────── */

/* Keyboard focus. Passing NULL takes focus back for the shell itself,
 * which is what a shell overlay or an empty desktop wants. */
void        aurwl_set_focus(aurwl *c, aurwl_win *w);
aurwl_win  *aurwl_focus(const aurwl *c);

/* Pointer, in surface-local coordinates. Pass NULL for `w` when the
 * pointer is over the shell's own furniture rather than a window. */
void        aurwl_pointer_motion(aurwl *c, aurwl_win *w, int sx, int sy, uint32_t time_ms);
void        aurwl_pointer_button(aurwl *c, uint32_t button, int pressed, uint32_t time_ms);
void        aurwl_pointer_axis(aurwl *c, int horizontal, double step, uint32_t time_ms);

/* Raw evdev keycode -- not an ASCII character. The client owns the
 * keymap we handed it and does its own translation, which is the only
 * way a Greek or Dvorak layout works in an application we did not
 * write. Returns 1 if a client consumed it. */
int         aurwl_key(aurwl *c, uint32_t evdev_code, int pressed, uint32_t time_ms);

/* Modifier state must be tracked even for keys the shell swallows, or
 * the client's idea of Shift drifts from the user's. */
void        aurwl_update_modifiers(aurwl *c);

/* ── frame ──────────────────────────────────────────────────────── */

/* Call after presenting. Fires frame callbacks, so clients that throttle
 * to our refresh -- which is every toolkit -- draw their next frame. */
void        aurwl_frame_done(aurwl *c, uint32_t time_ms);

/* ── launching ──────────────────────────────────────────────────── */

/* fork/exec with WAYLAND_DISPLAY, XDG_RUNTIME_DIR and a clean session
 * environment. `cmdline` is a /bin/sh command, because that is what a
 * .desktop Exec= line is. Returns the pid, or -1. */
pid_t       aurwl_spawn(aurwl *c, const char *cmdline);

/* Reap exited children. Call each frame; cheap when there are none. */
void        aurwl_reap(aurwl *c);

#endif
