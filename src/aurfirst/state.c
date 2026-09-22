/* state.c — what phase is this machine in, and the three answers. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aurfirst.h"

#define MAX_BOOTS 512

/* 1 there, 0 not there, -1 cannot tell.
 *
 * "Cannot tell" is a real answer and it used to be folded into "not
 * there". /var/lib/auros arriving later than the unit that reads it --
 * a separate /var, a slow mount -- then looked exactly like nobody
 * having answered, so the hold re-armed BootNext on every boot for
 * ever on a machine whose owner had said no. */
static int stamped(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

static int stamp(const char *path, char *why, size_t n)
{
    mkdir(AF_DIR, 0755);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(why, n, "AurOS could not write down your answer (%s)",
                 strerror(errno));
        return -1;
    }
    /* Something a person reading the disk can understand, not an empty
     * file whose meaning is its name. */
    const char *t = strrchr(path, '.');
    dprintf(fd, "%s\n", t && !strcmp(t, ".confirmed")
            ? "the person using this computer said AurOS works"
            : "the person using this computer said AurOS does not work");
    close(fd);
    return 0;
}

/* Our entry, found by its description. The installer wrote it; nothing
 * else on the machine is called that. Never "the entry we do not
 * recognise" -- that also selects the Fedora somebody installed last
 * month. */
static int find_entry(uint16_t *out)
{
    uint16_t nums[MAX_BOOTS];
    int n = af_boot_numbers(nums, MAX_BOOTS);
    for (int i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof name, "Boot%04X", nums[i]);
        uint8_t opt[4096];
        int len = af_var_get(name, opt, sizeof opt);
        if (len < 6) continue;
        char desc[256];
        af_desc_of(opt, len, desc, sizeof desc);
        if (strcmp(desc, AF_ENTRY_DESC) != 0) continue;
        *out = nums[i];
        return 1;
    }
    return 0;
}

/* -1 means "there is no BootOrder"; -2 means "there is one and it is
 * longer than this machine's code can represent", which is a refusal
 * further up rather than a shorter list. The buffer is sized from
 * MAX_BOOTS so the two bounds cannot drift apart. */
static int read_order(uint16_t *out, int max)
{
    uint8_t buf[MAX_BOOTS * 2];
    int len = af_var_get("BootOrder", buf, sizeof buf);
    if (len == -2) return -2;
    if (len < 2) return -1;
    int n = len / 2;
    if (n > max) return -2;
    for (int i = 0; i < n; i++)
        out[i] = (uint16_t)(buf[i * 2] | (buf[i * 2 + 1] << 8));
    return n;
}

int af_look(af_state *s)
{
    memset(s, 0, sizeof *s);
    int c = stamped(AF_CONFIRMED), d = stamped(AF_DECLINED);
    s->unknown   = (c < 0 || d < 0);
    s->confirmed = c > 0;
    s->declined  = d > 0;
    s->efivars   = af_efivars_present();
    if (!s->efivars) return 0;
    s->writable  = af_efivars_writable();
    s->have_entry = find_entry(&s->entry);

    uint8_t bn[8];
    int k = af_var_get("BootNext", bn, sizeof bn);
    if (k == 2 && s->have_entry) {
        uint16_t v = (uint16_t)(bn[0] | (bn[1] << 8));
        s->bootnext = (v == s->entry);
    }

    uint16_t order[MAX_BOOTS];
    int n = read_order(order, MAX_BOOTS);
    if (n > 0) {
        s->have_order = 1;
        if (s->have_entry) {
            for (int i = 0; i < n; i++)
                if (order[i] == s->entry) { s->in_order = 1; break; }
            s->is_default = (order[0] == s->entry);
        }
    }
    return 0;
}

static const char *phase_of(const af_state *s)
{
    if (!s->have_entry) return "not-converted";
    if (s->confirmed)   return "done";
    if (s->declined)    return "declined";
    return "asking";
}

void af_publish(const af_state *s)
{
    /* WRITTEN WHOLE, THEN MOVED. The desktop reads this file on its
     * first frame; one caught halfway through being written is one
     * with no phase line in it, and welcome.c would then ask nothing
     * on the one boot that mattered. */
    char tmp[512];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.new", AF_STATE_FILE)
            >= sizeof tmp)
        return;
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "phase=%s\n", phase_of(s));
    fprintf(f, "efivars=%s\n", s->efivars ? "yes" : "no");
    fprintf(f, "writable=%s\n", s->writable ? "yes" : "no");
    fprintf(f, "converted=%s\n", s->have_entry ? "yes" : "no");
    if (s->have_entry) fprintf(f, "entry=%04X\n", s->entry);
    fprintf(f, "is_default=%s\n", s->is_default ? "yes" : "no");
    fprintf(f, "bootnext=%s\n", s->bootnext ? "yes" : "no");
    fprintf(f, "confirmed=%s\n", s->confirmed ? "yes" : "no");
    fprintf(f, "declined=%s\n", s->declined ? "yes" : "no");
    fclose(f);
    /* Readable by the desktop's user, which is the whole point of it. */
    chmod(tmp, 0644);
    if (rename(tmp, AF_STATE_FILE) != 0) unlink(tmp);
}

int af_hold(const af_state *s, char *why, size_t n)
{
    /* A machine that cannot say whether it was answered is a machine
     * this leaves alone. Re-arming a one-shot every boot for ever is
     * the failure mode, and doing nothing costs at most one boot that
     * reaches Windows -- which is the safe direction. */
    if (s->unknown) return 1;
    if (s->confirmed || s->declined) return 1;
    if (!s->have_entry) return 1;
    /* Already the default: there is nothing for a one-shot to add, and
     * arming it every boot on a machine that has been confirmed would
     * be an NVRAM write per boot for ever. */
    if (s->is_default) return 1;
    if (s->bootnext) return 1;
    uint8_t v[2] = { (uint8_t)(s->entry & 0xFF), (uint8_t)(s->entry >> 8) };
    if (af_var_put("BootNext", v, sizeof v, why, n) != 0) return -1;
    uint8_t back[8];
    if (af_var_get("BootNext", back, sizeof back) != 2 ||
        memcmp(back, v, 2) != 0) {
        snprintf(why, n, "this computer's firmware did not keep what AurOS "
                         "asked it to start next time");
        return -1;
    }
    return 0;
}

/* ── the one function in this tree that writes BootOrder ───────────── */
int af_confirm(const af_state *s, char *why, size_t n)
{
    /* "NO ENTRY" AND "COULD NOT LOOK" ARE NOT THE SAME ANSWER, and
     * they used to be. have_entry is false when efivarfs is not
     * mounted, when opendir fails, and when every read fails -- and
     * every one of those stamped consent, returned success, and made
     * the desktop say "AurOS starts from now on" about a machine
     * whose BootOrder had never been touched. Worse, the stamp is
     * permanent, so the hold stopped re-arming and the machine fell
     * back to Windows for good while the desktop insisted otherwise
     * and never asked again. */
    if (!s->efivars) {
        snprintf(why, n, "AurOS cannot reach this computer's start-up "
                         "settings, so it cannot make itself what this "
                         "computer starts.");
        return -1;
    }
    if (!s->have_entry) {
        /* Nothing to promote. This is not a failure: it is what a
         * machine AurOS was installed onto directly, rather than
         * converted from Windows, looks like. Record the answer and
         * stop asking. */
        if (stamp(AF_CONFIRMED, why, n) != 0) return -1;
        return 1;
    }
    if (!s->writable) {
        snprintf(why, n, "this computer does not let AurOS change what it "
                         "starts by default");
        return -1;
    }

    uint16_t order[MAX_BOOTS], nw[MAX_BOOTS];
    int have = read_order(order, MAX_BOOTS);
    if (have == -2) {
        snprintf(why, n, "this computer's start-up menu is longer than AurOS "
                         "can safely rewrite, so it has been left alone.");
        return -1;
    }
    int k = 0;
    nw[k++] = s->entry;
    if (have > 0) {
        for (int i = 0; i < have && k < MAX_BOOTS; i++)
            if (order[i] != s->entry) nw[k++] = order[i];
    } else {
        /* NO BootOrder AT ALL, which some firmware ships with: it
         * enumerates for itself. Writing just our number would be a
         * BootOrder that hides every other entry on the machine --
         * Windows included -- which is the one thing this whole design
         * has spent its budget not doing. So it is built from every
         * Boot#### that exists, ours first and the rest in the order
         * they are numbered. */
        /* NOT EVERY Boot#### IS SOMETHING TO BOOT. The manufacturer's
         * diagnostics, the firmware setup entry, and anything marked
         * inactive or hidden are all Boot#### variables, and the UEFI
         * spec says none of them belongs in the ordinary start-up
         * sequence. In plain numeric order a diagnostics partition
         * lands ahead of Windows on a machine whose owner never asked
         * for it, and undoing that means the firmware menu this
         * product exists to keep her out of.
         *
         * SO THEY ARE ORDERED, NOT REMOVED, and the difference is the
         * whole safety of it.
         *
         * Removing them was the first version, and a review built the
         * machine it breaks: firmware with no BootOrder -- which is
         * firmware that has already shown it does not keep the boot
         * variables tidy -- and a Windows entry with
         * LOAD_OPTION_ACTIVE clear, which is how several vendors
         * record "the user switched this off" rather than deleting the
         * variable. Windows was filtered out, a BootOrder holding only
         * AurOS was written, this printed success and stamped the
         * answer permanently, and af_decline below then refused to
         * undo it because ours was the only entry left. The way back
         * was gone and the program had removed it.
         *
         * A BootOrder is a list of NUMBERS. Firmware skips a number
         * whose entry it will not start, so carrying one costs
         * nothing; leaving one out can cost somebody their other
         * operating system. Two passes: the ones the firmware says are
         * boot options, then the rest, each ascending. af_boot_bootable
         * returns -1 for "could not tell", and that goes with the
         * yeses -- not being able to read an entry is not evidence
         * against it. */
        uint16_t all[MAX_BOOTS];
        int an = af_boot_numbers(all, MAX_BOOTS);
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < an && k < MAX_BOOTS; i++) {
                if (all[i] == s->entry) continue;
                int refused = (af_boot_bootable(all[i]) == 0);
                if (pass == 0 && refused)  continue;   /* later */
                if (pass == 1 && !refused) continue;   /* already in */
                nw[k++] = all[i];
            }
    }

    uint8_t buf[MAX_BOOTS * 2];
    for (int i = 0; i < k; i++) {
        buf[i * 2]     = (uint8_t)(nw[i] & 0xFF);
        buf[i * 2 + 1] = (uint8_t)(nw[i] >> 8);
    }
    if (af_var_put("BootOrder", buf, (size_t)k * 2, why, n) != 0) return -1;

    /* READ IT BACK. Every write in the installer is read back and
     * compared; this one decides what the computer does when it is
     * switched on, so it gets the same treatment. */
    uint8_t back[MAX_BOOTS * 2];
    int got = af_var_get("BootOrder", back, sizeof back);
    if (got != k * 2 || memcmp(back, buf, (size_t)k * 2) != 0) {
        /* PUT IT BACK. The write landed or it did not; either way the
         * machine must not be left with a start-up order nobody chose
         * while the screen says nothing has been changed. The old one
         * is still in `order`, and if there was none there is nothing
         * to restore -- deleting it is what "none" means. */
        char ignored[200];
        if (have > 0) {
            uint8_t was[MAX_BOOTS * 2];
            for (int i = 0; i < have; i++) {
                was[i * 2]     = (uint8_t)(order[i] & 0xFF);
                was[i * 2 + 1] = (uint8_t)(order[i] >> 8);
            }
            af_var_put("BootOrder", was, (size_t)have * 2, ignored,
                       sizeof ignored);
        } else {
            af_var_del("BootOrder", ignored, sizeof ignored);
        }
        snprintf(why, n, "this computer's firmware did not keep the new "
                         "start-up order, so it has been put back as it was");
        return -1;
    }

    /* The one-shot has done its job and an armed BootNext that is
     * consumed weeks from now is a surprise. Its removal is not
     * allowed to fail the confirmation: BootOrder is down and correct,
     * and a stale BootNext pointing at the entry that is now the
     * default does no harm at all. */
    char ignored[200];
    af_var_del("BootNext", ignored, sizeof ignored);

    if (stamp(AF_CONFIRMED, why, n) != 0) return -1;
    return 0;
}

int af_decline(const af_state *s, char *why, size_t n)
{
    /* CLEAR THE ONE-SHOT FIRST, then write the stamp. The other order
     * leaves a machine that has stopped re-arming and is still armed
     * once -- so the next start reaches AurOS after the person has
     * just said it does not work.
     *
     * AND A MACHINE WHERE IT CANNOT BE CLEARED IS A REFUSAL. The
     * guard used to be `efivars && writable && del(...)`, so on a
     * machine with efivarfs missing or read-only the whole clause
     * short-circuited, the stamp was written anyway, and the sequence
     * the paragraph above forbids is exactly what happened. */
    if (!s->efivars || !s->writable) {
        snprintf(why, n, "AurOS cannot reach this computer's start-up "
                         "settings, so it cannot undo them.");
        return -1;
    }
    if (af_var_del("BootNext", why, n) != 0) return -1;

    /* AND IF AUROS IS ALREADY THE DEFAULT, SAYING SO IS NOT ENOUGH.
     *
     * The screen this leads to says "Windows will start next time.
     * Turn this computer off and on again to go back." On a machine
     * where our entry is at the head of BootOrder -- a confirm that
     * was undone, a second run -- that was simply false, and it is the
     * one sentence the whole product rests on. Demote rather than
     * remove: her entry order is hers. */
    if (s->is_default && s->have_entry) {
        uint16_t order[MAX_BOOTS];
        int have = read_order(order, MAX_BOOTS);
        if (have < 0) {
            snprintf(why, n, "AurOS could not read this computer's start-up "
                             "order, so it cannot promise Windows is next.");
            return -1;
        }
        uint8_t buf[MAX_BOOTS * 2];
        int k = 0;
        for (int i = 0; i < have; i++)
            if (order[i] != s->entry) {
                buf[k * 2]     = (uint8_t)(order[i] & 0xFF);
                buf[k * 2 + 1] = (uint8_t)(order[i] >> 8);
                k++;
            }
        if (k == 0) {
            /* Ours is the only thing in the menu. Demoting it would
             * leave an empty BootOrder, which is a machine that starts
             * nothing -- so this says what is true instead. */
            snprintf(why, n, "AurOS is the only thing in this computer's "
                             "start-up menu, so it cannot put Windows first.");
            return -1;
        }
        buf[k * 2]     = (uint8_t)(s->entry & 0xFF);
        buf[k * 2 + 1] = (uint8_t)(s->entry >> 8);
        k++;
        if (af_var_put("BootOrder", buf, (size_t)k * 2, why, n) != 0) return -1;
    }

    if (stamp(AF_DECLINED, why, n) != 0) return -1;
    return 0;
}
