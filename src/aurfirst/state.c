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

static int stamped(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
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

static int read_order(uint16_t *out, int max)
{
    uint8_t buf[1024];
    int len = af_var_get("BootOrder", buf, sizeof buf);
    if (len < 2) return -1;
    int n = len / 2;
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
        out[i] = (uint16_t)(buf[i * 2] | (buf[i * 2 + 1] << 8));
    return n;
}

int af_look(af_state *s)
{
    memset(s, 0, sizeof *s);
    s->confirmed = stamped(AF_CONFIRMED);
    s->declined  = stamped(AF_DECLINED);
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

int af_hold(const af_state *s, char *why, size_t n)
{
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
        uint16_t all[MAX_BOOTS];
        int an = af_boot_numbers(all, MAX_BOOTS);
        for (int i = 0; i < an && k < MAX_BOOTS; i++)
            if (all[i] != s->entry) nw[k++] = all[i];
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
        snprintf(why, n, "this computer's firmware did not keep the new "
                         "start-up order");
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
     * just said it does not work. */
    if (s->efivars && s->writable && af_var_del("BootNext", why, n) != 0)
        return -1;
    if (stamp(AF_DECLINED, why, n) != 0) return -1;
    return 0;
}
