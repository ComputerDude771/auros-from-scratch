/* nettest.c — does the wifi panel read what the machine actually says?
 *
 * The panel's job is to turn what nmcli printed into something she can
 * press, and that translation is the part most likely to be quietly
 * wrong. It cannot be checked by looking at a screen, because on the
 * developer's machine every network is well-behaved: one access point,
 * an ASCII name, a strong signal, and a failure that never happens.
 *
 * The cases that break it are all in somebody else's house.
 *
 *   A name with a colon in it. nmcli's terse format separates fields
 *   with ':' and escapes a literal one as "\:". Splitting on every
 *   colon truncates exactly the names that phones produce -- and the
 *   truncated name is then handed back to nmcli to join, which fails
 *   with a message about a network that does not exist.
 *
 *   Two access points for one house. A mesh, or a repeater, answers
 *   twice with the same name. Two identical rows is her being asked to
 *   choose between a thing and itself.
 *
 *   A network that announces no name at all. It has to not be there,
 *   rather than be an empty row she can press.
 *
 *   The one she is already on, buried at position nine because its
 *   signal happens to be lower than a neighbour's.
 *
 * And the failures, which are the whole of rule 8: every sentence she
 * is shown when something goes wrong is chosen by reading nmcli's own
 * sentence, so if that reading is wrong she gets the wrong sentence at
 * the worst possible moment.
 *
 *   cc -O2 -std=gnu11 -o /tmp/nettest tools/nettest.c src/aurshell/net.c \
 *      src/aurshell/draw.c src/aurshell/shellcommon.c src/aurshell/anim.c \
 *      src/aurshell/layouts/[*].c src/common/theme.c src/common/font.c -lm
 *   /tmp/nettest
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "../src/aurshell/net.h"

/* The device name is not what these cases are about. */
static char devbuf[32];
#define net_parse_devices_T(x) net_parse_devices((x), devbuf, sizeof devbuf)

static int fail = 0, checked = 0;

static void ok(const char *what, int cond)
{
    checked++;
    printf("    %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fail++;
}

/* net_parse_* consume the buffer they are given, so every case gets
 * its own copy. */
static char *dup_(const char *s, char *into, size_t n)
{
    snprintf(into, n, "%s", s);
    return into;
}

int main(void)
{
    char buf[4096];
    net_ap ap[24];
    char saved[8][NET_NAME_MAX];

    printf("does the wifi panel read what the machine actually says?\n\n");

    /* ── the ordinary case, so a pass below means something ──────── */
    printf("  an ordinary house\n");
    int n = net_parse_list(dup_(
        "*:Home:82:WPA2\n"
        " :Neighbour:41:WPA1 WPA2\n"
        " :CoffeeShop:22:\n", buf, sizeof buf), ap, 24, NULL, 0);
    ok("three networks are three rows", n == 3);
    ok("the one we are on is first", n > 0 && ap[0].in_use &&
                                     !strcmp(ap[0].name, "Home"));
    ok("the rest are strongest first", n == 3 && ap[1].signal > ap[2].signal);
    ok("a network with nothing in the last field is open",
       n == 3 && !ap[2].secure);
    ok("the others want a password", n == 3 && ap[1].secure);

    /* ── a name with a colon in it ───────────────────────────────── */
    printf("\n  a name with a colon in it\n");
    n = net_parse_list(dup_(
        " :Ann's iPhone\\: 5G:70:WPA2\n", buf, sizeof buf), ap, 24, NULL, 0);
    ok("it is one network, not two", n == 1);
    ok("its whole name survives",
       n == 1 && !strcmp(ap[0].name, "Ann's iPhone: 5G"));
    ok("and the signal after it is still read", n == 1 && ap[0].signal == 70);

    /* A backslash of its own, which nmcli writes as "\\". */
    n = net_parse_list(dup_(" :back\\\\slash:50:WPA2\n", buf, sizeof buf),
                       ap, 24, NULL, 0);
    ok("a backslash in a name survives too",
       n == 1 && !strcmp(ap[0].name, "back\\slash"));

    /* ── two access points, one house ────────────────────────────── */
    printf("\n  a house with two boxes answering\n");
    n = net_parse_list(dup_(
        " :Home:35:WPA2\n"
        " :Home:88:WPA2\n"
        " :Home:12:WPA2\n", buf, sizeof buf), ap, 24, NULL, 0);
    ok("she is shown one network, not three", n == 1);
    ok("with the strongest reading of it", n == 1 && ap[0].signal == 88);

    /* ── a network with no name ──────────────────────────────────── */
    printf("\n  a network that does not say its name\n");
    n = net_parse_list(dup_(
        " ::64:WPA2\n"
        " :Real:30:WPA2\n", buf, sizeof buf), ap, 24, NULL, 0);
    ok("it is not in the list at all", n == 1);
    ok("and the real one still is", n == 1 && !strcmp(ap[0].name, "Real"));

    /* ── ones we have joined before ──────────────────────────────── */
    printf("\n  networks this computer has joined before\n");
    int ns = net_parse_saved(dup_(
        "Home:802-11-wireless\n"
        "Wired connection 1:802-3-ethernet\n"
        "Ann's iPhone\\: 5G:802-11-wireless\n", buf, sizeof buf),
        saved, 8);
    ok("only the wifi ones are remembered", ns == 2);
    ok("a name with a colon is remembered whole",
       ns == 2 && !strcmp(saved[1], "Ann's iPhone: 5G"));

    n = net_parse_list(dup_(
        " :Home:50:WPA2\n"
        " :Somewhere:80:WPA2\n", buf, sizeof buf), ap, 24,
        (const char (*)[NET_NAME_MAX])saved, ns);
    int home = -1;
    for (int i = 0; i < n; i++) if (!strcmp(ap[i].name, "Home")) home = i;
    ok("the one we know is marked known", home >= 0 && ap[home].known);
    ok("the one we do not is not", n == 2 && !ap[home == 0 ? 1 : 0].known);

    /* ── is there wifi in this machine at all ────────────────────── */
    printf("\n  what this computer has\n");
    ok("a laptop with wifi", net_parse_devices_T(dup_(
        "wlp3s0:wifi:connected\n"
        "enp0s25:ethernet:unavailable\n"
        "lo:loopback:unmanaged\n", buf, sizeof buf)) == 1);
    ok("a desktop with only a cable", net_parse_devices_T(dup_(
        "enp0s25:ethernet:connected\n"
        "lo:loopback:unmanaged\n", buf, sizeof buf)) == 0);
    /* Three answers, not two. "nmcli did not answer" must never be
     * read as "this computer has no wifi": on first boot she can open
     * the panel before NetworkManager has finished starting, and a
     * laptop with a perfectly good card would be told it had none. */
    ok("a machine that reports nothing at all", net_parse_devices_T(dup_(
        "", buf, sizeof buf)) == -1);
    /* Stop depends on knowing what the radio is called: killing nmcli
     * does not cancel a join NetworkManager has already been asked to
     * make, so Stop disconnects the device by name. */
    net_parse_devices(dup_("enp0s25:ethernet:connected\n"
                           "wlp3s0:wifi:disconnected\n", buf, sizeof buf),
                      devbuf, sizeof devbuf);
    ok("the radio's name comes back with the answer",
       !strcmp(devbuf, "wlp3s0"));
    ok("the network service is not running yet", net_parse_devices_T(dup_(
        "Error: NetworkManager is not running.\n", buf, sizeof buf)) == -1);
    ok("a laptop whose wifi is switched off still HAS wifi",
       net_parse_devices_T(dup_(
        "wlp3s0:wifi:unavailable\n"
        "lo:loopback:unmanaged\n", buf, sizeof buf)) == 1);

    /* ── the sentences she is shown when it goes wrong ───────────── */
    printf("\n  what she is told when it does not work\n");
    ok("a password that was not accepted",
       net_parse_trouble("Error: Connection activation failed: "
                         "(7) Secrets were required, but not provided.")
       == T_PASSWORD);
    ok("a password rejected the other way nmcli says it",
       net_parse_trouble("Error: 802-11-wireless-security.psk: "
                         "property is invalid.") == T_PASSWORD);
    ok("a network that has gone",
       net_parse_trouble("Error: No network with SSID 'Home' found.")
       == T_GONE);
    /* This used to be asserted as T_GONE, whose sentence is "<name> is
     * not there any more" -- telling a machine with no wifi hardware
     * that its network had moved. The test locked the wrong answer in,
     * which is the specific way a harness can make a defect permanent. */
    ok("a machine with no wifi in it",
       net_parse_trouble("Error: No Wi-Fi device found.") == T_NOWIFI);
    ok("the network service stopped mid-way",
       net_parse_trouble("Error: NetworkManager is not running.")
       == T_NOANSWER);
    ok("a machine that will not allow it",
       net_parse_trouble("Error: Not authorized to control networking.")
       == T_NOTALLOWED);
    ok("anything else",
       net_parse_trouble("Error: Connection activation failed: (1) unknown")
       == T_OTHER);
    /* Order matters: a wrong password also mentions "failed", and a
     * refusal also mentions "Error". Whichever is checked first wins,
     * so the specific ones have to come before the vague ones. */
    ok("a wrong password is not reported as something else",
       net_parse_trouble("Error: Connection activation failed: (7) Secrets "
                         "were required, but not provided.") != T_OTHER);

    printf("\n");
    if (fail) {
        printf("%d of %d wrong. She is either shown a network she cannot\n",
               fail, checked);
        printf("join, or told the wrong thing about why it did not work.\n");
        return 1;
    }
    printf("%d checks: what the machine says becomes what she sees\n", checked);
    return 0;
}
