/* bt.h — her headphones, her mouse, her keyboard.
 *
 * The same problem the wifi had, one layer along. A laptop's pointing
 * device, its headphones and increasingly its keyboard are Bluetooth,
 * and a machine that cannot pair one is a machine somebody has to go
 * and find a cable for. There was no Bluetooth stack on the image at
 * all, and therefore nothing to put a panel in front of.
 *
 * It is deliberately built to the same shape as src/aurshell/net.c --
 * a band button, a list of things in range, press one, a sentence when
 * it does not work -- because they are the same act to her: "connect
 * this computer to that thing". Two panels that do the same kind of
 * job and behave differently are two things to learn.
 *
 * WHY bluetoothctl AND NOT THE D-BUS API
 *
 * Same answer as nmcli's, for the same reasons: BlueZ's real interface
 * is D-Bus and using it means a library, a connection and several
 * hundred lines to reach four verbs. bluetoothctl ships with the
 * daemon we install. It is run with execvp and a pipe, never through a
 * shell -- a speaker called `;rm -rf ~` is a name somebody chose, and
 * it arrives here as bytes.
 *
 * WHAT PAIRING COSTS HER
 *
 * Nothing to type, in the ordinary case. Most headphones and mice pair
 * with no code at all; the ones that want a number are answered with
 * the agent's default, which is what every phone does. A device that
 * genuinely needs a person to compare two numbers is told plainly that
 * it cannot be paired from here, rather than being pretended at.
 */
#ifndef AUROS_BT_H
#define AUROS_BT_H

#include "shell.h"

void bt_opened(shell_ctx *c);
void bt_closed(shell_ctx *c);

int  bt_fd(void);            /* what bluetoothctl is saying, or -1 */
int  bt_pump(shell_ctx *c);  /* 1 if the screen changed             */

void bt_paint(shell_ctx *c, surface *s, shell_fonts *f);
int  bt_click(shell_ctx *c, int x, int y);
void bt_motion(shell_ctx *c, int x, int y);
int  bt_key(shell_ctx *c, int k);
void bt_reap(void);
void bt_fini(void);

/* ── reading what bluetoothctl said ─────────────────────────────────
 *
 * Pure, for the same reason net.c's parsing is: it is the part most
 * likely to be quietly wrong, and it cannot be exercised by standing
 * in a room with a pair of headphones.
 */
#define BT_NAME_MAX 64

typedef struct {
    char addr[20];               /* AA:BB:CC:DD:EE:FF                  */
    char name[BT_NAME_MAX];
    int  paired;                 /* this computer already knows it     */
    int  connected;              /* and is talking to it now           */
} bt_dev;

int bt_parse_devices(char *out, bt_dev *devs, int max);
int bt_parse_info(char *out, bt_dev *d);   /* one device's detail */

/* Which screen. */
enum { BT_LIST, BT_JOINING, BT_JOINED, BT_TROUBLE, BT_PAGE_N };
enum { BTT_FAILED, BTT_NORADIO, BTT_NOTOOL, BTT_OFF, BTT_N };

typedef struct {
    int page, trouble, n_devs, first_row;
} bt_view;

int bt_targets(const shell_ctx *c, int sw, int sh, const bt_view *v,
               rect *out, int max);

#endif
