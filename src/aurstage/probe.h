/* probe.h — phase 7: does this computer actually work under AurOS?
 *
 * THE SINGLE MOST VALUABLE MITIGATION IN THE PRODUCT, and the reason
 * the staging environment is the initramfs rather than a second
 * reboot. docs/AURBRIDGE.md:
 *
 *   it can test WiFi, backlight, audio and suspend on the real
 *   machine before anything is committed.
 *
 * NETWORK IS RECORDED, NOT REQUIRED (changed 2026-10-23).
 *
 * This phase once aborted when it found no wireless and no cable, on
 * the reasoning that a person stranded offline cannot reach help. The
 * 2026-10-23 journey audit found that reasoning backwards in practice:
 * the abort came AFTER the shrink, and it fired on most Wi-Fi-only
 * laptops -- because the initramfs carries no firmware, so the Wi-Fi
 * driver loaded but never registered a radio. The machines it stranded
 * are the machines this product is for.
 *
 * Two things fixed it. probe_run() now points the kernel's firmware
 * loader at the new root's /lib/firmware (see the symlink in probe.c),
 * so a card that has firmware actually comes up and can be tested; and
 * the installer carries the person's Wi-Fi key across (docs/AURBRIDGE.md
 * D2), so first boot is online with nothing typed. A machine with no
 * network at all is still a machine she can use. So the network is
 * MEASURED and written into the record, and it never decides whether
 * the install may proceed. The one thing still worth an abort is
 * PROBE_NO_DRIVERS -- the image and the running kernel disagreeing,
 * which is our fault and tests nothing about her machine.
 *
 * WHY IT RUNS AFTER THE WRITE AND BEFORE THE COMMIT. The firmware is
 * not in the initramfs -- linux-firmware is 700 MB to 1.5 GB and even
 * a WiFi-only subset is 150-400 MB, against an OEM ESP commonly 100 MB
 * and nearly full. So the probe mounts the root we have just written
 * and verified, read-only, and loads ITS modules and ITS
 * /lib/firmware. Everything at this point is still reversible: the old
 * partition table is in force, Windows still boots, and an abort here
 * costs the user nothing but time.
 *
 * "NO NETWORK" AND "NO DRIVERS AT ALL" ARE DIFFERENT ANSWERS
 *
 * An adversarial review caught this and it matters. If the staging
 * initramfs and the image disagree about the kernel -- AurBridge wrote
 * last week's initramfs to the ESP and today's image is on the stick,
 * or the profile is not the one the journal named -- then
 * /probe/lib/modules/<uname -r> does not exist, every modprobe fails
 * silently, and no wireless device ever appears. Reported as a
 * hardware verdict that is "this computer's WiFi does not work", which
 * is a lie about the machine and sends the user to buy a USB adapter
 * they do not need. It is a build fault, and it says so.
 */
#ifndef AUROS_PROBE_H
#define AUROS_PROBE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PROBE_OK = 0,
    PROBE_NO_NETWORK,    /* kept for the name; probe_judge no longer
                          * returns it -- no network is recorded in the
                          * result's fields and `why`, not a verdict.  */
    PROBE_NO_DRIVERS,    /* OUR fault: the image and the kernel differ*/
    PROBE_UNMOUNTABLE,   /* what we just wrote will not mount         */
} probe_verdict;

typedef struct {
    probe_verdict verdict;
    int  modules_loaded;
    int  kernel_matches;     /* /lib/modules/<uname -r> is in the image */
    int  wifi_devices;       /* wireless PHYs that appeared             */
    int  wired_up;           /* a wired link with carrier               */
    int  backlight;          /* a controllable screen brightness        */
    int  sound_cards;
    /* 1 the new root was unmounted, 2 it had to be detached lazily
     * (still busy after retries), 0 it is STILL MOUNTED -- which
     * probe_run() turns into a refusal, because the commit's e2fsck and
     * resize2fs must never run on a filesystem mounted elsewhere. */
    int  unmounted;
    char why[240];
    char remedy[240];
} probe_result;

/* Mount `root_dev` read-only, load its drivers, look at what came up,
 * unmount. Always fills `out`. Writes nothing to the device: the
 * mount is MS_RDONLY with noload, because a read-only ext4 mount
 * still replays the journal without it. */
void probe_run(const char *root_dev, probe_result *out);

/* The same, for a filesystem that is not a partition yet.
 *
 * Phase 7 runs BEFORE the commit, so the root AurOS has just been
 * written into has no partition entry and no device node -- that is
 * the whole point of the ordering. A loop device over the extent is
 * how it gets mounted without the table having changed. */
void probe_run_at(const char *disk_dev, uint64_t off, probe_result *out);

/* The two halves, separated so the second one can be driven from a
 * synthetic /sys.
 *
 * Whether this machine has wireless is not something a test can
 * arrange -- the container it runs in has whatever hardware it has --
 * and the decision being tested is not "can we see a WiFi card", it
 * is "what do we do when we cannot". That decision is worth a test
 * and the card is not. */
void probe_look(const char *sysroot, probe_result *out);
void probe_judge(probe_result *out);

const char *probe_verdict_name(probe_verdict v);

#endif
