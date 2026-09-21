/* net.h — getting her onto her wifi, without a terminal.
 *
 * NetworkManager was installed, enabled and running from the very first
 * image this project produced. wireless-tools and wpasupplicant were in
 * packages_hardware next to it. Every piece of the machinery worked.
 * There was not one pixel between it and her.
 *
 * So a machine that came up connected stayed connected, and a machine
 * that came up on wifi it had never seen -- which is every machine, on
 * its first boot, in the house it was carried into -- stayed off the
 * network with no way at all to get on. The answer every other Linux
 * gives to that is `nmcli device wifi connect`, and docs/EASY.md rule 1
 * forbids it: there is no task in this product whose answer is "open a
 * terminal". Not for wifi, not as a fallback, not for advanced users.
 *
 * WHY IT LIVES IN THE BAND AND NOT IN AN ARCHETYPE
 *
 * For the same reason Help and the text size do. Six archetypes means
 * six places to forget it, and the one thing she needs before anything
 * else on the machine works cannot be the thing that is present in four
 * desktops out of six. The band is the one piece of furniture no
 * archetype owns, so this is reachable from all of them and cost none
 * of them an edit.
 *
 * WHY nmcli AND NOT D-BUS
 *
 * NetworkManager's D-Bus API is the real interface and libnm is the
 * real client, and using either would mean a new library on the image
 * and several hundred lines to reach the same four verbs: what is
 * there, what is in range, join this one, are we on. nmcli is a
 * supported, stable front end that ships with the daemon we already
 * install. We run it with execvp and a pipe -- never through a shell,
 * so a wifi name with a semicolon in it is a name and not an
 * instruction -- and read its output without blocking, because the
 * shell that paints the screen is the same thread.
 *
 * WHAT USING nmcli COSTS, SAID PLAINLY
 *
 * The password is handed to it as an argument, and an argument is
 * visible in /proc/<pid>/cmdline, which is world-readable. For the few
 * seconds the join takes, anything else running on the machine can read
 * the wifi password -- including the browser, and anything she has
 * installed.
 *
 * That is a real exposure and it is written here rather than left to be
 * found. Three things bound it. It lasts only as long as the join, not
 * the session. It happens once per network, because NetworkManager
 * keeps the password itself afterwards in a root-only file and this
 * panel never asks again for a network it has joined. And on the
 * machine this product is for, everything already runs as the same
 * person, so the browser could read her saved passwords out of her own
 * home directory with or without this.
 *
 * It is still the weaker half of the story, and the fix is to stop
 * using a command line for the one call that carries a secret: libnm's
 * AddAndActivateConnection takes it over D-Bus and never puts it in an
 * argument. That is the next change to this file, not a thing we have
 * decided is acceptable forever.
 *
 * WHAT SHE SEES, AND WHAT SHE NEVER SEES
 *
 * She sees the names printed on the back of people's routers, strongest
 * first, and a lock next to the ones that want a password. She never
 * sees SSID, WPA2, an interface name, a signal in dBm, or an error code
 * -- docs/EASY.md rule 2, enforced by tools/plainwords.sh.
 */
#ifndef AUROS_NET_H
#define AUROS_NET_H

#include "shell.h"

/* Opened and closed from the band. c->net_open is the flag; these are
 * what act on it. */
void net_opened(shell_ctx *c);     /* she just opened it: start looking */
void net_closed(shell_ctx *c);     /* she just closed it: stop, forget  */

/* The output of whatever nmcli is running, or -1 when nothing is.
 * main.c polls it alongside the input devices, so a scan that takes
 * four seconds costs no frames and the screen updates the moment an
 * answer arrives rather than on the next mouse move. */
int  net_fd(void);

/* Read what is ready and advance. Returns 1 if the screen changed.
 * Safe to call when there is nothing running. */
int  net_pump(shell_ctx *c);

/* Painted into the archetype's region, under the band, exactly like
 * the help panel: the way out is never covered by the thing she needs
 * to get out of. */
void net_paint(shell_ctx *c, surface *s, shell_fonts *f);
int  net_click(shell_ctx *c, int x, int y);
void net_motion(shell_ctx *c, int x, int y);
int  net_key(shell_ctx *c, int k);   /* 1 if this panel took the key */

/* Is this machine on a network at all? Answered from the kernel's own
 * routing table rather than by running anything, so the band can ask it
 * every frame for nothing. */
int  net_online(void);

/* Collect whatever nmcli process has finished. Swept once per pass of
 * the main loop, beside the compositor's own -- each subsystem waits
 * for its own children, because one that reaps another's leaves that
 * one unable to learn how its child ended or to signal it safely. */
void net_reap(void);

void net_fini(void);

/* ── reading what nmcli said ────────────────────────────────────────
 *
 * Separated out and made PURE -- text in, rows out, no state, no
 * child, no daemon -- for the same reason the layout is: it is the
 * part most likely to be quietly wrong, and a function that can only
 * be exercised by standing in a room with a wifi router is a function
 * nothing checks. tools/nettest.c feeds it the awkward cases: a
 * network whose name contains a colon, two access points answering for
 * one house, a network that does not announce a name at all.
 *
 * They take a writable buffer because nmcli's terse format escapes
 * separators ("\:") and unescaping happens in place; the text is
 * consumed, not borrowed.
 */
#define NET_NAME_MAX 64

typedef struct {
    char name[NET_NAME_MAX];
    int  signal;     /* 0..100, as nmcli reports it  */
    int  secure;     /* wants a password             */
    int  in_use;     /* this is the one we are on    */
    int  known;      /* we have joined it before     */
} net_ap;

int net_parse_list(char *terse, net_ap *out, int max,
                   const char saved[][NET_NAME_MAX], int n_saved);
int net_parse_saved(char *terse, char out[][NET_NAME_MAX], int max);
int net_parse_devices(char *terse);         /* 1 if this machine has wifi */
int net_parse_trouble(const char *output);  /* one of T_*                 */

/* ── measuring it ───────────────────────────────────────────────────
 *
 * Which screen of the panel, and how much is on it. The layout is a
 * pure function of this and the panel size, so a harness can ask about
 * a screen the machine is not currently showing -- which is the only
 * way every state gets checked against docs/EASY.md rule 4 without a
 * radio, a daemon and a person pressing things.
 */
/* Which screen of the panel she is on. Every one of these is a state
 * she can get OUT of -- docs/EASY.md rule 6 -- which is why each has
 * its own explicit way back rather than relying on her finding the
 * band again. Declared here rather than in net.c so that the harness
 * and the panel count the same screens; two copies of a list like this
 * is two places to remember. */
enum {
    P_LIST,        /* here are the networks (possibly still looking) */
    P_PASSWORD,    /* type the password for the one she picked       */
    P_JOINING,     /* nmcli is trying                                */
    P_JOINED,      /* it worked                                      */
    P_TROUBLE,     /* it did not, and `trouble` says what to say     */
    P_N
};

/* Why it did not work. Each maps to one sentence that says what
 * happened in terms of what she was trying to do, never names a
 * component, and always says what she can do next -- rule 8. */
enum {
    T_PASSWORD,    /* the password was not accepted      */
    T_GONE,        /* the network was not there any more */
    T_NOTALLOWED,  /* this machine will not let us       */
    T_NOWIFI,      /* there is no wifi of its own        */
    T_NOTOOL,      /* the program that does it is missing*/
    /* The machine did not answer. NOT the same as "there is no wifi
     * in this computer", and telling her the second when the first is
     * true is the worst sentence this panel can produce: on first boot
     * she can easily open it before the network service has finished
     * starting, and a laptop with a perfectly good wifi card would say
     * it had none and offer her nothing but Close. */
    T_NOANSWER,
    T_OTHER,
    T_N
};

typedef struct {
    int page;        /* one of P_*                                */
    int trouble;     /* one of T_*, when page is P_TROUBLE         */
    int n_aps;       /* how many networks are in the list         */
    int first_row;   /* paging: the row at the top                */
} net_view;

/* Every rectangle this panel expects her to press, for that view.
 * Returns how many it wrote. */
int  net_targets(const shell_ctx *c, int sw, int sh, const net_view *v,
                 rect *out, int max);

#endif
