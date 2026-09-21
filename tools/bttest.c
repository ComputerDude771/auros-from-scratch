/* bttest.c — what bluetoothctl says becomes what she sees.
 *
 * The panel drives a program whose output is text, and every claim it
 * makes on screen comes from parsing that text. Two of those claims
 * were wrong in ways nothing could see:
 *
 *   The "Connected" and "Used before" tags are drawn from two flags
 *   that NOTHING IN THE PROGRAM EVER SET. bt_parse_info() was written
 *   and called from nowhere, so the list said nothing about which
 *   headphones this computer already knows -- which is the first thing
 *   a person looks for, and the difference between pressing a row and
 *   waiting, and pressing it and being asked for a number printed on
 *   the side of a speaker.
 *
 *   A device that has never announced a name prints its address twice.
 *   Six rows reading FC-58-FA-21-03-9C is a list she cannot choose
 *   from.
 *
 * Real bluetoothctl output, from bluez 5.72 as shipped.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "../src/aurshell/bt.h"

static int fail = 0, checked = 0;
static void ok(const char *what, int good)
{
    checked++;
    printf("    %-56s %s\n", what, good ? "ok" : "FAIL");
    if (!good) fail++;
}

int main(void)
{
    printf("\nWhat bluetoothctl says becomes what she sees\n\n");

    printf("  the list\n");
    bt_dev d[BT_MAX_DEV];
    char all[] =
        "Device 00:1B:66:00:11:22 Ann's Headphones\n"
        "Device FC:58:FA:21:03:9C FC-58-FA-21-03-9C\n"
        "Device AA:BB:CC:DD:EE:FF Kitchen Speaker\n"
        "Device 00:1B:66:00:11:22 Ann's Headphones\n";
    int n = bt_parse_devices(all, d, BT_MAX_DEV);
    ok("three devices, the repeat folded into one", n == 3);
    ok("the one with a name keeps it", !strcmp(d[0].name, "Ann's Headphones"));
    ok("the one without a name is left blank, not given its address",
       d[1].name[0] == 0);
    ok("...and its address is still there to connect to",
       !strcmp(d[1].addr, "FC:58:FA:21:03:9C"));

    printf("\n  the tags\n");
    for (int i = 0; i < n; i++) {
        if (d[i].paired || d[i].connected) { ok("flags start clear", 0); break; }
    }
    ok("nothing is claimed before anything is read",
       !d[0].paired && !d[0].connected && !d[2].paired);

    char paired[] =
        "Device 00:1B:66:00:11:22 Ann's Headphones\n"
        "Device AA:BB:CC:DD:EE:FF Kitchen Speaker\n";
    ok("two of the three are known to this computer",
       bt_mark(paired, d, n, 0) == 2);
    ok("...and they are the right two",
       d[0].paired && !d[1].paired && d[2].paired);

    char conn[] = "Device 00:1B:66:00:11:22 Ann's Headphones\n";
    ok("one of them is talking to it now", bt_mark(conn, d, n, 1) == 1);
    ok("...and it is the right one",
       d[0].connected && !d[1].connected && !d[2].connected);
    ok("'used before' did not become 'connected'", d[2].paired && !d[2].connected);

    /* A filtered listing that names a device the full list did not --
     * a device that appeared between the two runs -- must not be
     * invented into the list. */
    char stranger[] = "Device 11:22:33:44:55:66 A Thing\n";
    ok("a device only the filtered list knows is not added",
       bt_mark(stranger, d, n, 1) == 0);

    /* An empty answer, which is what a machine with no paired devices
     * gives, and what a failed run gives too. */
    char none[] = "";
    ok("nothing said marks nothing", bt_mark(none, d, n, 0) == 0);
    ok("...and does not clear what was already known", d[0].paired);

    printf("\n  one device's own detail\n");
    bt_dev one; memset(&one, 0, sizeof one);
    char info[] = "Device 00:1B:66:00:11:22 (public)\n"
                  "\tName: Ann's Headphones\n"
                  "\tPaired: yes\n"
                  "\tBonded: yes\n"
                  "\tTrusted: no\n"
                  "\tConnected: yes\n";
    ok("info reads both", bt_parse_info(info, &one) == 0 &&
                          one.paired && one.connected);
    bt_dev two; memset(&two, 0, sizeof two);
    char no[] = "Device 00:1B:66:00:11:22 (public)\n"
                "\tPaired: no\n\tConnected: no\n";
    ok("and does not read a 'no' as a 'yes'",
       bt_parse_info(no, &two) == 0 && !two.paired && !two.connected);
    bt_dev gone; memset(&gone, 0, sizeof gone);
    char nd[] = "Device 00:1B:66:00:11:22 not available\n";
    ok("a device that has gone is reported as gone",
       bt_parse_info(nd, &gone) < 0);

    printf("\n");
    if (fail) {
        printf("%d of %d wrong. The panel is telling her something about\n",
               fail, checked);
        printf("her own headphones that is not true.\n");
        return 1;
    }
    printf("%d checks: the list, and the two tags on it, are read out of\n", checked);
    printf("what bluetoothctl actually prints.\n");
    return 0;
}
