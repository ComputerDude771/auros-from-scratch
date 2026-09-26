/* phases.c — see phases.h. Everything that leaves the program goes
 * through plat.h, so this file has no #include <windows.h> in it and
 * runs, whole, against a machine made of files. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "inflate.h"
#include "phases.h"
#include "plat.h"
#include "format.h"

#define MIB (1024ull * 1024)

/* EFI System, in GPT's mixed-endian order. */
static const uint8_t ESP_TYPE_GUID[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };

const char *ab_phase_name(ab_phase p)
{
    switch (p) {
    case AB_INSPECT: return "looking at this computer";
    case AB_CONSENT: return "asking";
    case AB_PREPARE: return "preparing the memory stick";
    case AB_HANDOFF: return "getting ready to restart";
    case AB_N:       break;
    }
    return "?";
}

static void talk(ab_say say, void *ud, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (say) say(line, ud);
}

static uint64_t round_up(uint64_t v, uint64_t to)
{ return ((v + to - 1) / to) * to; }

/* ── how big the saved copy has to be ────────────────────────────── */

uint64_t ab_saved_bytes(uint64_t esp_bytes, uint32_t sector, int n_volumes)
{
    if (sector < 512) sector = 512;
    if (n_volumes < 1) n_volumes = 1;
    if (n_volumes > 8) n_volumes = 8;
    /* Mirrors src/aurstage/rescue.c's esp_and_table_bytes():
     *   its 4096-byte header, the protective record, two headers and
     *   two entry arrays, the whole ESP, and 8 KiB + two blocks per
     *   NTFS volume. Plus 32 MiB, because THIS number is computed on
     *   Windows from what Windows can see and THAT one is computed
     *   after the restart from the disk itself, and the second one
     *   arriving larger is a refusal the user has already sat through
     *   a reboot to be told about. */
    uint64_t arr = round_up(128ull * 128, sector);
    uint64_t need = 4096
                  + sector                 /* protective record       */
                  + 2 * (sector + arr)     /* both headers and arrays */
                  + esp_bytes
                  + (uint64_t)n_volumes * (8192 + 2ull * sector)
                  + 32 * MIB;
    return round_up(need, MIB);
}

/* ── where everything goes on the stick ──────────────────────────── */

int ab_stick_layout(uint64_t stick_bytes, uint32_t sector,
                    uint64_t image_bytes, uint64_t esp_bytes,
                    uint64_t *image_first, uint64_t *image_last,
                    uint64_t *record_first, uint64_t *record_last,
                    uint64_t *saved_first, uint64_t *saved_last,
                    char *why, size_t n)
{
    if (sector < 512 || sector > 4096 || (sector & (sector - 1))) {
        snprintf(why, n, "this memory stick uses a block size AurOS cannot "
                         "write to.");
        return -1;
    }
    uint64_t align = MIB / sector;
    if (!align) align = 1;
    uint64_t fu = fmt_gpt_first_usable(sector);
    uint64_t lu = fmt_gpt_last_usable(stick_bytes, sector);

    uint64_t img_bytes    = round_up(FMT_MANIFEST_BYTES + image_bytes, MIB);
    uint64_t record_bytes = 4 * MIB;
    uint64_t saved = ab_saved_bytes(esp_bytes, sector, 4);

    uint64_t first = round_up(fu, align);
    uint64_t need_blocks = (img_bytes + record_bytes + saved) / sector;
    if (lu < first || lu - first + 1 < need_blocks) {
        uint64_t have_mb = stick_bytes / MIB;
        uint64_t want_mb = (img_bytes + record_bytes + saved + 4 * MIB) / MIB;
        snprintf(why, n,
                 "this memory stick holds %llu MB and AurOS needs %llu MB on "
                 "it: the copy of AurOS itself, somewhere to keep notes while "
                 "it installs, and room to save this computer's Windows "
                 "start-up so it can be put back. Use a larger stick.",
                 (unsigned long long)have_mb, (unsigned long long)want_mb);
        return -1;
    }

    *image_first  = first;
    *image_last   = first + img_bytes / sector - 1;
    *record_first = *image_last + 1;
    *record_last  = *record_first + record_bytes / sector - 1;
    *saved_first  = *record_last + 1;
    /* The saved copy takes everything that is left rather than exactly
     * what was computed: the stick is ours, nothing else is going on
     * it, and a machine whose EFI partition turns out larger than
     * Windows reported is then still installable. */
    *saved_last   = lu;
    if (*saved_last < *saved_first + saved / sector - 1) {
        snprintf(why, n,
                 "this memory stick has no room left to save this computer's "
                 "Windows start-up. Use a larger stick.");
        return -1;
    }
    return 0;
}

/* ── reading the machine's disk through plat ─────────────────────── */

typedef struct { int disk; } rd_ctx;

static int rd_disk(void *ud, uint64_t off, void *buf, size_t n)
{
    rd_ctx *c = ud;
    char why[PLAT_WHY];
    return plat_read(c->disk, off, buf, n, why, sizeof why);
}

/* Defined with phase 2, where the questions belong, and called from
 * phase 0, where the answers are free. */
static int prepare_possible(const ab_choice *c, ab_machine *m,
                            char *why, size_t n);

/* ── phase 0: look, and refuse ───────────────────────────────────── */

static int phase_inspect(const ab_choice *c, pf_report *r, ab_machine *m,
                         ab_say say, void *ud, char *why, size_t n)
{
    talk(say, ud, "checking this computer");
    if (!pf_is_go(r)) {
        for (int i = 0; i < r->n; i++)
            if (r->results[i].sev == PF_BLOCK) {
                snprintf(why, n, "%s %s", r->results[i].detail,
                         r->results[i].remedy);
                return -1;
            }
        snprintf(why, n, "this computer cannot be converted.");
        return -1;
    }
    if (r->system_disk < 0 || r->system_disk >= r->n_disks) {
        snprintf(why, n, "AurOS could not tell which drive Windows is on.");
        return -1;
    }
    const pf_disk *d = &r->disks[r->system_disk];
    m->disk_index = d->index;
    snprintf(m->disk_serial, sizeof m->disk_serial, "%s", d->serial);
    snprintf(m->disk_model,  sizeof m->disk_model,  "%s", d->model);
    m->disk_bytes = d->size_bytes;
    m->logical_sector = d->logical_sector;
    m->esp_offset = d->esp_offset;
    m->esp_length = d->esp_length;

    if (!m->logical_sector) {
        /* docs/AURBRIDGE.md: always read StorageAccessAlignmentProperty
         * and BLOCK if it cannot be read. Assuming 512 on a 4Kn disk
         * makes every partition eight times too small. */
        snprintf(why, n,
                 "this computer will not say what size the blocks on its "
                 "drive are, and AurOS will not guess.");
        return -1;
    }
    if (!m->esp_length) {
        snprintf(why, n,
                 "this computer has no EFI partition, so there is no Windows "
                 "start-up to save and nothing to put back.");
        return -1;
    }

    if (r->system_volume < 0) {
        snprintf(why, n, "AurOS could not find the Windows drive.");
        return -1;
    }

    /* THE PARTITION'S EXTENT, OUT OF THE PARTITION TABLE, and not the
     * filesystem's size out of Windows.
     *
     * The first version of this took win_sectors from pf_volume, which
     * on Windows is what GetDiskFreeSpaceEx reports -- the size of the
     * FILESYSTEM. After the restart the staging environment compares
     * it against what /sys says the PARTITION is, and those two
     * numbers differ on every machine whose NTFS does not fill its
     * partition exactly. Every such install would have been refused
     * with "the Windows part of this disk is not where it was", which
     * is both wrong and frightening.
     *
     * So both numbers come from the table, which is also where the
     * other side reads them, and the partition number comes with them
     * rather than being guessed from a drive letter. */
    {
        rd_ctx rc0 = { m->disk_index };
        uint32_t ss = m->logical_sector;
        uint8_t hdr[4096];
        if (ss > sizeof hdr ||
            rd_disk(&rc0, ss, hdr, ss) != 0 ||
            memcmp(hdr, "EFI PART", 8) != 0) {
            snprintf(why, n,
                     "the way this drive is divided up could not be read.");
            return -1;
        }
        uint64_t elba = 0; uint32_t ne = 0, es = 0;
        for (int i = 7; i >= 0; i--) elba = (elba << 8) | hdr[72 + i];
        for (int i = 3; i >= 0; i--) ne = (ne << 8) | hdr[80 + i];
        for (int i = 3; i >= 0; i--) es = (es << 8) | hdr[84 + i];
        if (!ne || ne > 4096 || es < 128 || es > 4096) {
            snprintf(why, n,
                     "the way this drive is divided up could not be read.");
            return -1;
        }
        int found = 0;
        for (uint32_t k = 0; k < ne; k++) {
            uint8_t e[4096];
            if (rd_disk(&rc0, elba * ss + (uint64_t)k * es, e, es) != 0) break;
            int used = 0;
            for (int q = 0; q < 16; q++) if (e[q]) { used = 1; break; }
            if (!used) continue;
            uint64_t first = 0, last = 0;
            for (int i = 7; i >= 0; i--) first = (first << 8) | e[32 + i];
            for (int i = 7; i >= 0; i--) last  = (last  << 8) | e[40 + i];
            if (memcmp(e, ESP_TYPE_GUID, 16) == 0) {
                m->esp.number    = k + 1;
                m->esp.first_lba = first;
                m->esp.blocks    = last - first + 1;
                memcpy(m->esp.guid, e + 16, 16);
            }
            if (first * ss != r->system_offset) continue;
            /* IN 512-BYTE UNITS, ALWAYS. journal.h says so at length:
             * Linux reports a partition's start and length in /sys in
             * 512-byte units on every disk, 4Kn included, so dividing
             * by logical_sector here makes every 4Kn machine report
             * that Windows has moved. */
            m->win_start_lba = first * ss / 512;
            m->win_sectors   = (last - first + 1) * ss / 512;
            snprintf(m->win_part, sizeof m->win_part, "%u", (unsigned)(k + 1));
            /* THE VOLUME'S OWN SERIAL NUMBER, out of its boot sector.
             * journal.h calls the re-verification of these fields the
             * thing that makes a one-shot boot safe to arm at all, and
             * this one was hard-coded to zero with no comment -- a
             * field that presents as a safety check and is not one. */
            {
                uint8_t b[4096];
                if (ss <= sizeof b && rd_disk(&rc0, first * ss, b, ss) == 0 &&
                    memcmp(b + 3, "NTFS    ", 8) == 0) {
                    uint64_t v = 0;
                    for (int q = 7; q >= 0; q--) v = (v << 8) | b[0x48 + q];
                    m->win_ntfs_serial = v;
                }
            }
            found = 1;
        }
        if (!found) {
            snprintf(why, n,
                     "the Windows drive is not one of the parts this disk is "
                     "divided into, which should be impossible. AurOS will "
                     "not touch a computer it does not understand.");
            return -1;
        }
    }
    if (!m->esp.blocks) {
        snprintf(why, n,
                 "AurOS could not find this computer's EFI partition in its "
                 "partition table.");
        return -1;
    }

    rd_ctx rc = { m->disk_index };
    if (fmt_gpt_sha256(rd_disk, &rc, m->logical_sector,
                       m->gpt_sha256, sizeof m->gpt_sha256) != 0) {
        snprintf(why, n,
                 "the way this drive is divided up could not be read.");
        return -1;
    }
    talk(say, ud, "this computer's drive is %llu GB, %u-byte blocks",
         (unsigned long long)(m->disk_bytes / 1000000000ull),
         (unsigned)m->logical_sector);
    talk(say, ud, "Windows starts at block %llu and is %llu GB",
         (unsigned long long)m->win_start_lba,
         (unsigned long long)(m->win_sectors * 512 / 1000000000ull));

    /* THE NO-STICK MODE COUNTS THE IMAGE AS WELL. Preflight measured
     * what the Windows drive could give up before five gigabytes of
     * AurOS arrived on it, and in this mode they stay there through the
     * shrink -- the shrink cannot give back space a file is sitting in.
     * Asked here, where the answer costs nothing, rather than after the
     * download and the restart. */
    if (c->no_stick && r->system_volume >= 0) {
        const pf_volume *v = &r->volumes[r->system_volume];
        uint64_t have_img = 0;
        plat_file_size(c->image_path, &have_img);
        uint64_t img = c->image_expect ? c->image_expect
                                       : (6ull * 1024 * 1024 * 1024);
        uint64_t extra = have_img >= img ? 0 : img - have_img;
        if (v->shrink_measured && v->offline_shrinkable) {
            uint64_t give = v->offline_shrinkable > PF_WINDOWS_KEEP_BYTES
                          ? v->offline_shrinkable - PF_WINDOWS_KEEP_BYTES : 0;
            if (give < extra || give - extra < PF_AUROS_NEED_BYTES) {
                snprintf(why, n,
                         "without a memory stick, the copy of AurOS (%llu MB) "
                         "has to stay on the Windows drive while it is "
                         "installed, and then there is not enough room left: "
                         "about %llu MB could be freed and AurOS needs %llu MB. "
                         "Free up space in Windows, or install with a memory "
                         "stick.",
                         (unsigned long long)(img / MIB),
                         (unsigned long long)((give > extra ? give - extra : 0) / MIB),
                         (unsigned long long)(PF_AUROS_NEED_BYTES / MIB));
                return -1;
            }
        }
    }

    /* Asked here, where a refusal costs nothing. See the note on
     * prepare_possible(). */
    if (prepare_possible(c, m, why, n) != 0) return -1;
    return 0;
}

/* ── phase 1: asking ─────────────────────────────────────────────── */

/* R1. A BitLocker-protected drive must not be touched by anybody who
 * cannot unlock it afterwards -- and the person who cannot is usually
 * the owner, who has never seen the key because it is in a Microsoft
 * account she signed into once in 2019. So she is made to go and get
 * it, and to type some of it back, BEFORE anything makes her need it. */
static int phase_consent(const ab_choice *c, pf_report *r, ab_machine *m,
                         ab_say say, void *ud, char *why, size_t n)
{
    if (!c->consent_given) {
        snprintf(why, n, "nobody has agreed to this yet.");
        return -1;
    }
    int locked = 0;
    if (r->system_volume >= 0)
        locked = r->volumes[r->system_volume].bitlocker == 1;
    if (locked) {
        /* Not a checksum of the key, and not a comparison against it
         * either: this program never has the key. It is proof that she
         * has gone and looked -- the length and the shape are what a
         * real 48-digit recovery key has, and typing eight digits of
         * one is something you cannot do without having found it. */
        size_t k = strlen(c->key_typed_back);
        int digits = 0;
        for (size_t i = 0; i < k; i++)
            if (c->key_typed_back[i] >= '0' && c->key_typed_back[i] <= '9')
                digits++;
        if (digits < 8) {
            snprintf(why, n,
                     "this computer's drive is locked by BitLocker. Find the "
                     "48-digit recovery key first -- it is in the Microsoft "
                     "account you sign in to Windows with, under Devices -- "
                     "and keep it somewhere you can reach from another "
                     "device. AurOS will not start until you have typed part "
                     "of it back here.");
            return -1;
        }
        talk(say, ud, "the unlock key has been found and checked");
        /* -RebootCount 0 suspends protection until it is turned back
         * on, rather than for one restart: the machine restarts more
         * than once before this is over, and a protector that
         * re-arms halfway leaves a drive nobody can read. */
        char tail[512];
        int rc = plat_run("manage-bde -protectors -disable C: -RebootCount 0",
                          tail, sizeof tail);
        if (rc != 0) {
            snprintf(why, n,
                     "BitLocker could not be paused on this computer (%s). "
                     "Nothing has been changed.", tail);
            return -1;
        }
        m->bitlocker_suspended = 1;
        talk(say, ud, "BitLocker is paused; it goes back on by itself");
    }
    talk(say, ud, "agreed: AurOS will be installed alongside Windows");
    return 0;
}

/* ── phase 2: the memory stick ───────────────────────────────────── */

static int find_stick(const ab_choice *c, ab_machine *m, char *why, size_t n)
{
    plat_disk d[16];
    int nd = plat_disks(d, 16);
    for (int i = 0; i < nd; i++) {
        if (strcmp(d[i].serial, c->stick_serial) != 0) continue;
        if (d[i].index == m->disk_index) {
            snprintf(why, n,
                     "the drive chosen for the memory stick is the drive "
                     "Windows is on. AurOS will not write to it.");
            return -1;
        }
        m->stick_index  = d[i].index;
        m->stick_bytes  = d[i].size_bytes;
        m->stick_sector = d[i].logical_sector;   /* 0 is a refusal above */
        return 0;
    }
    snprintf(why, n,
             "the memory stick you chose is not plugged in any more. Plug it "
             "back in and try again.");
    return -1;
}

/* EVERYTHING PHASE 2 CAN REFUSE FOR, ASKED IN PHASE 0.
 *
 * Phase 1 suspends BitLocker, and every refusal below it used to be
 * reachable afterwards: the stick unplugged, the image file missing,
 * the stick too small. A machine refused for any of those was left
 * with its encryption key in the clear while the screen said nothing
 * had been changed. None of these questions needs consent to have been
 * given, so none of them is asked after it. */
/* THE PROFILE ID HAS TO SURVIVE BOTH CHANNELS UNCHANGED.
 *
 * It goes onto the stick twice: raw, in the manifest, and through
 * JSON, in the journal. fmt_journal_json's esc() replaces every byte
 * outside printable ASCII with a space, so a profile id with an
 * accent or a tab in it arrives on the two sides DIFFERENT -- and the
 * staging environment then refuses a stick the same run wrote, on the
 * far side of the restart, saying it is for a different version of
 * AurOS. The manifest field is 64 bytes including its terminator.
 *
 * Every profile this repository has is lower-case ASCII with a dash
 * in it, so this refuses nothing that exists. It is here so that the
 * day somebody adds one it cannot pass, rather than passing as far as
 * the reboot. */
static int profile_ok(const char *p, char *why, size_t n)
{
    size_t len = p ? strlen(p) : 0;
    if (len == 0 || len > 63) {
        snprintf(why, n, "the version of AurOS to install was not named "
                         "in a way the installer can record.");
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (ch < 0x20 || ch > 0x7E || ch == '"' || ch == '\\') {
            snprintf(why, n, "the version of AurOS to install was not named "
                             "in a way the installer can record.");
            return -1;
        }
    }
    return 0;
}

static int prepare_possible_nostick(const ab_choice *c, char *why, size_t n)
{
    if (!c->kernel_path[0] && !plat_payload_embedded()) {
        snprintf(why, n,
                 "this copy of the installer is incomplete -- the part that "
                 "starts your computer is missing from it. Download it "
                 "again.");
        return -1;
    }
    uint64_t have = 0, alt = 0;
    plat_file_size(c->image_path, &have);
    if (c->image_alt_path[0]) plat_file_size(c->image_alt_path, &alt);
    int pieces = c->pieces_text && c->pieces_text[0];
    if (!have && !alt && !pieces && !c->image_url[0]) {
        snprintf(why, n, "the copy of AurOS to install could not be found, and "
                         "this installer does not know where to download it "
                         "from.");
        return -1;
    }
    if (pieces && (!c->image_sha256[0] || !c->image_expect)) {
        snprintf(why, n,
                 "this copy of the installer was not built correctly -- it "
                 "does not say what AurOS should look like. Download the "
                 "installer again.");
        return -1;
    }
    /* ROOM FOR THE DOWNLOAD, THE UNPACKED IMAGE, AND BOTH AT ONCE: the
     * pieces are only deleted once what they unpack to has been
     * checked. */
    if (pieces && have < c->image_expect) {
        static ab_pieces pc;
        if (ab_pieces_parse(c->pieces_text, &pc, why, n) != 0) return -1;
        uint64_t need = c->image_expect + pc.total;
        uint64_t free_now = plat_free_space(c->image_path);
        if (free_now && free_now < need) {
            snprintf(why, n,
                     "there is not enough room on the Windows drive to "
                     "download AurOS: it needs %llu MB free while it "
                     "downloads and unpacks, and there are %llu MB.",
                     (unsigned long long)(need / MIB),
                     (unsigned long long)(free_now / MIB));
            return -1;
        }
    }
    return 0;
}

static int prepare_possible(const ab_choice *c, ab_machine *m,
                            char *why, size_t n)
{
    if (profile_ok(c->profile, why, n) != 0) return -1;
    if (c->no_stick) return prepare_possible_nostick(c, why, n);
    if (find_stick(c, m, why, n) != 0) return -1;
    if (!m->stick_sector) {
        /* The same refusal the system disk gets, for the same reason.
         * A guessed 512 on a 4Kn USB device -- a USB-to-SATA enclosure,
         * a large USB SSD -- makes every partition on the stick eight
         * times wrong, and the failure surfaces twenty minutes later
         * as "the memory stick stopped accepting what was written to
         * it", after the whole image has been copied. */
        snprintf(why, n,
                 "the memory stick you chose will not say how large its "
                 "blocks are, and AurOS will not guess. Try a different "
                 "stick.");
        return -1;
    }
    /* THE STAGING ENVIRONMENT, CHECKED BEFORE ANYTHING ELSE.
     *
     * A build that was assembled wrongly carries no kernel, and the
     * only place that used to be discovered was phase 3 -- after the
     * shrink had been arranged, the stick had been erased and
     * BitLocker had been suspended. It costs a FindResource to ask
     * here, where nothing has happened yet. */
    if (!c->kernel_path[0] && !plat_payload_embedded()) {
        snprintf(why, n,
                 "this copy of the installer is incomplete -- the part that "
                 "starts your computer is missing from it. Download it "
                 "again.");
        return -1;
    }
    uint64_t image_bytes = 0;
    if (plat_file_size(c->image_path, &image_bytes) != 0 || !image_bytes) {
        /* NOT YET IS NOT THE SAME AS NOT AT ALL. An installer that
         * knows where to fetch the image has an image; what it does
         * not have is the twenty minutes, and asking for those is
         * phase 2's job. The size check below is then done against
         * what the build recorded rather than against a file that is
         * not there. */
        if (!c->image_url[0]) {
            snprintf(why, n, "the copy of AurOS to install could not be "
                             "found.");
            return -1;
        }
        image_bytes = c->image_expect ? c->image_expect
                                      : (6ull * 1024 * 1024 * 1024);
    }
    /* AND A PARTIAL DOWNLOAD IS NOT THE SIZE OF THE IMAGE. Resume is a
     * first-class feature here, so a 2 GB partial beside the installer
     * is an expected state -- and measuring it made phase 0 ask "does
     * the stick hold 2 GB?", say yes, and phase 2 refuse after the
     * download finished and after phase 1 had suspended BitLocker.
     * That is the class of failure this function exists to
     * eliminate. */
    if (c->image_expect > image_bytes) image_bytes = c->image_expect;
    /* AND SOMEWHERE TO PUT THE DOWNLOAD. Asked here, where refusing
     * costs nothing, rather than discovered forty minutes into a five
     * gigabyte transfer on a small cheap disk -- which is the disk the
     * people this product is for have. A machine that will not say how
     * much room it has is not stopped; a machine that says it has too
     * little is. */
    if (c->image_url[0] && c->image_expect) {
        uint64_t have_now = 0, free_now = plat_free_space(c->image_path);
        plat_file_size(c->image_path, &have_now);
        uint64_t still = c->image_expect > have_now
                       ? c->image_expect - have_now : 0;
        if (free_now && free_now < still) {
            snprintf(why, n,
                     "there is not enough room on this computer to download "
                     "AurOS: it needs %llu MB more and this computer has "
                     "%llu MB free.",
                     (unsigned long long)(still / MIB),
                     (unsigned long long)(free_now / MIB));
            return -1;
        }
    }

    uint64_t if_, il, rf, rl, sf, sl;
    return ab_stick_layout(m->stick_bytes, m->stick_sector, image_bytes,
                           m->esp_length, &if_, &il, &rf, &rl, &sf, &sl,
                           why, n);
}

/* ── getting the image, which is the download ─────────────────────── */

/* Exactly 64 hexadecimal characters, or it is not a hash.
 *
 * hex_eq compared 64 characters whatever was there, so a hash pasted
 * one character short into the build produced: download five
 * gigabytes, mismatch, start again, download five gigabytes, mismatch,
 * and then "something between here and AurOS is changing it -- try a
 * different network." She changes networks, buys a hotspot, and it
 * fails identically, because the fault is in our build. */
static int hex_ok(const char *h)
{
    int n = 0;
    for (; h[n]; n++) {
        char c = h[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return n == 64;
}

static int hex_eq(const unsigned char d[32], const char *want)
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        char a = H[d[i] >> 4], b = H[d[i] & 15];
        char x = want[i * 2], y = want[i * 2 + 1];
        if (x >= 'A' && x <= 'F') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F') y = (char)(y - 'A' + 'a');
        if (a != x || b != y) return 0;
    }
    return 1;
}

/* Hash the file where it lies. Slow -- it is five gigabytes -- so it
 * reports progress, and it is only ever done when there is a hash to
 * compare against. */
static int image_hash(const char *path, unsigned char out[32],
                      ab_progress prog, void *ud, char *why, size_t n)
{
    uint64_t total = 0;
    if (plat_file_size(path, &total) != 0 || !total) {
        snprintf(why, n, "the copy of AurOS to install could not be read.");
        return -1;
    }
    fmt_sha h; fmt_sha_start(&h);
    static uint8_t buf[1 << 20];
    uint64_t at = 0;
    while (at < total) {
        size_t take = total - at > sizeof buf ? sizeof buf : (size_t)(total - at);
        if (plat_file_read(path, at, buf, take, why, n) != 0) return -1;
        fmt_sha_feed(&h, buf, take);
        at += take;
        if (prog) prog((int)(at * 100 / total), ud);
    }
    fmt_sha_done(&h, out);
    return 0;
}

typedef struct { ab_progress prog; void *ud; } fetch_ctx;

static int fetch_progress(uint64_t got, uint64_t total, void *ud)
{
    fetch_ctx *f = ud;
    if (f && f->prog && total)
        f->prog((int)(got * 100 / total), f->ud);
    return 0;
}

/* ── the image, in pieces ──────────────────────────────────────────── */

static const char *skip_sp(const char *p)
{ while (*p == ' ' || *p == '\t') p++; return p; }

/* One word, up to whitespace or the end of the line. 0 if it did not
 * fit, which is a refusal: a truncated name is a different file. */
static int word(const char **pp, char *out, size_t n)
{
    const char *p = skip_sp(*pp);
    size_t k = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (k + 1 >= n) return 0;
        out[k++] = *p++;
    }
    out[k] = 0;
    *pp = p;
    return k > 0;
}

int ab_pieces_parse(const char *text, ab_pieces *out, char *why, size_t n)
{
    memset(out, 0, sizeof *out);
    const char *p = text ? text : "";
    int line = 0;
    while (*p) {
        line++;
        const char *eol = strchr(p, '\n');
        const char *next = eol ? eol + 1 : p + strlen(p);
        p = skip_sp(p);
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) { p = next; continue; }
        char kw[16];
        if (!word(&p, kw, sizeof kw)) goto bad;
        if (!strcmp(kw, "base")) {
            if (!word(&p, out->base, sizeof out->base)) goto bad;
        } else if (!strcmp(kw, "piece")) {
            if (out->n >= AB_MAX_PIECES) goto bad;
            ab_piece *q = &out->p[out->n];
            char num[32];
            if (!word(&p, q->name, sizeof q->name) ||
                !word(&p, num, sizeof num) ||
                !word(&p, q->sha256, sizeof q->sha256)) goto bad;
            char *e = NULL;
            q->bytes = strtoull(num, &e, 10);
            if (!e || *e || !q->bytes || !hex_ok(q->sha256)) goto bad;
            /* A name that could leave the directory it is saved in is
             * not a name. */
            if (strchr(q->name, '/') || strchr(q->name, '\\') ||
                strchr(q->name, ':') || strstr(q->name, "..")) goto bad;
            out->total += q->bytes;
            out->n++;
        } else {
            goto bad;
        }
        p = next;
    }
    if (!out->base[0] || out->n == 0) goto bad_empty;
    if (strncmp(out->base, "https://", 8) != 0 &&
        strncmp(out->base, "http://", 7) != 0) goto bad_empty;
    return 0;
bad:
    snprintf(why, n, "this copy of the installer was not built correctly -- "
                     "its list of where to download AurOS from is damaged "
                     "(line %d). Download the installer again.", line);
    return -1;
bad_empty:
    snprintf(why, n, "this copy of the installer was not built correctly -- "
                     "it does not say where to download AurOS from. Download "
                     "the installer again.");
    return -1;
}

/* Where the pieces go: the directory the image is going into. */
static void dir_of(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path);
    char *a = strrchr(out, '\\'), *b = strrchr(out, '/');
    char *s = a > b ? a : b;
    if (s) s[1] = 0; else out[0] = 0;
}

/* Is this piece here, whole, and the right bytes? */
static int piece_ok(const char *path, const ab_piece *q)
{
    uint64_t have = 0;
    if (plat_file_size(path, &have) != 0 || have != q->bytes) return 0;
    unsigned char d[32];
    char w[PLAT_WHY];
    if (image_hash(path, d, NULL, NULL, w, sizeof w) != 0) return 0;
    return hex_eq(d, q->sha256);
}

typedef struct {
    ab_progress prog; void *ud;
    uint64_t done, total;
    int lo, hi;
} span_prog;

static int piece_progress(uint64_t got, uint64_t total, void *ud)
{
    (void)total;
    span_prog *sp = ud;
    if (sp->prog && sp->total) {
        uint64_t at = sp->done + got;
        if (at > sp->total) at = sp->total;
        sp->prog(sp->lo + (int)((uint64_t)(sp->hi - sp->lo) * at / sp->total),
                 sp->ud);
    }
    return 0;
}

/* The reader gz_inflate pulls from: the pieces, in order, as one
 * stream. */
typedef struct {
    const ab_pieces *pc;
    const char *dir;
    int idx;
    uint64_t off;
    char *why; size_t wn;
} piece_reader;

static int piece_read(void *ud, uint8_t *buf, size_t n, size_t *got)
{
    piece_reader *r = ud;
    *got = 0;
    while (r->idx < r->pc->n && r->off == r->pc->p[r->idx].bytes) {
        r->idx++;
        r->off = 0;
    }
    if (r->idx >= r->pc->n) return 0;
    const ab_piece *q = &r->pc->p[r->idx];
    uint64_t left = q->bytes - r->off;
    size_t take = left < n ? (size_t)left : n;
    char path[900];
    snprintf(path, sizeof path, "%s%s", r->dir, q->name);
    if (plat_file_read(path, r->off, buf, take, r->why, r->wn) != 0) {
        snprintf(r->why, r->wn, "part %d of the downloaded AurOS could not "
                                "be read back.", r->idx + 1);
        return -1;
    }
    r->off += take;
    *got = take;
    return 0;
}

/* The writer: onto the end of the .part file, hashed as it goes, and
 * never past the size the build said the image is. */
typedef struct {
    const char *path;
    fmt_sha sha;
    uint64_t written, expect;
    span_prog sp;
    char *why; size_t wn;
} image_writer;

static int image_write(void *ud, const uint8_t *buf, size_t n)
{
    image_writer *w = ud;
    if (w->written + n > w->expect) {
        snprintf(w->why, w->wn, "the downloaded AurOS unpacks to more than it "
                                "should. It is not the one this installer "
                                "was built for.");
        return -1;
    }
    if (plat_file_append(w->path, buf, n, w->why, w->wn) != 0) {
        snprintf(w->why, w->wn, "the Windows drive ran out of room while "
                                "AurOS was being unpacked.");
        return -1;
    }
    fmt_sha_feed(&w->sha, buf, n);
    w->written += n;
    piece_progress(w->written, w->expect, &w->sp);
    return 0;
}

/* THE PIECES, FETCHED, CHECKED, JOINED, UNPACKED AND CHECKED AGAIN.
 *
 * Each piece is resumed rather than restarted when a connection drops,
 * and tried again from nothing if it arrives wrong. A piece already
 * here and right is not fetched at all, so pressing Start installing a
 * second time after a failure costs only what is missing. The pieces
 * are removed once the image they unpack to has matched the hash baked
 * into this program -- not before, so a failure while unpacking does
 * not cost the download. */
static int ensure_image_pieces(const ab_choice *c, int present,
                               ab_say say, ab_progress prog, void *ud,
                               char *why, size_t n)
{
    static ab_pieces pc;
    if (ab_pieces_parse(c->pieces_text, &pc, why, n) != 0) return -1;
    if (!hex_ok(c->image_sha256) || !c->image_expect) {
        snprintf(why, n,
                 "this copy of the installer was not built correctly -- it "
                 "does not say what AurOS should look like. Download the "
                 "installer again.");
        return -1;
    }
    unsigned char dig[32];
    if (present) {
        uint64_t have = 0;
        plat_file_size(c->image_path, &have);
        if (have == c->image_expect) {
            talk(say, ud, "checking the copy of AurOS already on this computer");
            if (image_hash(c->image_path, dig, prog, ud, why, n) == 0 &&
                hex_eq(dig, c->image_sha256))
                return 0;
        }
        talk(say, ud, "the copy of AurOS on this computer is not the right "
                      "one; getting it again");
    }

    char dir[600];
    dir_of(c->image_path, dir, sizeof dir);
    uint64_t done = 0;
    for (int i = 0; i < pc.n; i++) {
        const ab_piece *q = &pc.p[i];
        char path[900], url[512];
        snprintf(path, sizeof path, "%s%s", dir, q->name);
        if ((size_t)snprintf(url, sizeof url, "%s%s", pc.base, q->name)
                >= sizeof url) {
            snprintf(why, n, "an address in this installer is too long.");
            return -1;
        }
        if (piece_ok(path, q)) { done += q->bytes; continue; }
        talk(say, ud, "downloading AurOS, part %d of %d", i + 1, pc.n);
        int ok = 0;
        char last[PLAT_WHY] = "";
        for (int attempt = 0; attempt < 8 && !ok; attempt++) {
            uint64_t have = 0;
            plat_file_size(path, &have);
            /* Longer than it should be, or whole and wrong (piece_ok has
             * already said so): from nothing. Shorter: carry on. */
            if (have >= q->bytes) have = 0;
            if (have == 0 && plat_file_put(path, "", 0, why, n) != 0)
                return -1;
            if (have < q->bytes) {
                span_prog sp = { prog, ud, done, pc.total, 0, 60 };
                if (plat_fetch(url, path, piece_progress, &sp, last,
                               sizeof last) != 0) {
                    talk(say, ud, "the download stopped (%s); carrying on", last);
                    continue;
                }
            }
            if (piece_ok(path, q)) { ok = 1; break; }
            talk(say, ud, "part %d did not arrive intact; fetching it again",
                 i + 1);
        }
        if (!ok) {
            snprintf(why, n,
                     "part %d of AurOS could not be downloaded intact%s%s. "
                     "Check this computer is online and press Start "
                     "installing again -- what has already arrived is kept.",
                     i + 1, last[0] ? ": " : "", last);
            return -1;
        }
        done += q->bytes;
    }

    talk(say, ud, "unpacking AurOS (about %llu MB)",
         (unsigned long long)(c->image_expect / MIB));
    char part[640];
    snprintf(part, sizeof part, "%s.part", c->image_path);
    if (plat_file_put(part, "", 0, why, n) != 0) return -1;
    piece_reader rd = { &pc, dir, 0, 0, why, n };
    image_writer wr;
    memset(&wr, 0, sizeof wr);
    wr.path = part; wr.expect = c->image_expect; wr.why = why; wr.wn = n;
    wr.sp.prog = prog; wr.sp.ud = ud; wr.sp.total = c->image_expect;
    wr.sp.lo = 60; wr.sp.hi = 95;
    fmt_sha_start(&wr.sha);
    uint64_t out = 0;
    char w2[PLAT_WHY];
    if (gz_inflate(piece_read, &rd, image_write, &wr, &out, why, n) != 0) {
        plat_file_delete(part, w2, sizeof w2);
        return -1;
    }
    fmt_sha_done(&wr.sha, dig);
    if (wr.written != c->image_expect || !hex_eq(dig, c->image_sha256)) {
        plat_file_delete(part, w2, sizeof w2);
        /* EVERY PIECE MATCHED ITS OWN HASH, so the network did not do
         * this: what was published is not what this installer was
         * built to expect. That is our mistake, and the sentence says
         * so rather than sending her to find another network. */
        snprintf(why, n,
                 "the downloaded AurOS does not match what this installer "
                 "expects. This is a mistake in how this version was "
                 "published, not in your computer or your internet. Nothing "
                 "has been changed.");
        return -1;
    }
    if (plat_file_rename(part, c->image_path, why, n) != 0) return -1;
    for (int i = 0; i < pc.n; i++) {
        char path[900];
        snprintf(path, sizeof path, "%s%s", dir, pc.p[i].name);
        plat_file_delete(path, w2, sizeof w2);
    }
    if (prog) prog(95, ud);
    talk(say, ud, "AurOS is downloaded, unpacked and checked");
    return 0;
}

/*
 * THE IMAGE IS HERE, OR IT IS FETCHED, OR THIS REFUSES.
 *
 * Three states and no fourth:
 *
 *   it is beside the installer and hashes right     -> use it
 *   it is not, or does not, and there is a URL      -> fetch, resuming
 *   neither                                         -> refuse, in words
 *
 * A PARTIAL FILE IS RESUMED, NOT DELETED. A five gigabyte download
 * that starts again from zero because the wifi dropped at 90% is the
 * difference between a person finishing this and giving up, and it is
 * exactly the connection that drops that this product exists for. The
 * resume lives in plat_fetch, which asks from where it got to and
 * refuses a server that ignores the question.
 *
 * AND A FILE THAT HASHES WRONG IS TRIED ONCE MORE FROM NOTHING. A
 * resume onto a file that was corrupt to begin with resumes the
 * corruption; a second failure is a refusal rather than a loop.
 */
static int ensure_image(const ab_choice *c, ab_say say, ab_progress prog,
                        void *ud, char *why, size_t n)
{
    if (c->image_sha256[0] && !hex_ok(c->image_sha256)) {
        snprintf(why, n,
                 "this copy of the installer was not built correctly -- it "
                 "does not say properly what AurOS should look like. "
                 "Download the installer again.");
        return -1;
    }
    uint64_t have = 0;
    int present = plat_file_size(c->image_path, &have) == 0 && have > 0;

    /* ALREADY ON THIS COMPUTER, SOMEWHERE ELSE: beside the installer,
     * where a developer build expects it. Moved, not copied, when it is
     * on the same drive -- five gigabytes twice is a drive this product's
     * users do not have. */
    if (!present && c->image_alt_path[0] &&
        strcmp(c->image_alt_path, c->image_path) != 0) {
        uint64_t alt = 0;
        if (plat_file_size(c->image_alt_path, &alt) == 0 && alt > 0) {
            talk(say, ud, "moving the copy of AurOS beside the installer into "
                          "place");
            if (plat_file_rename(c->image_alt_path, c->image_path, why, n) != 0)
                return -1;
            present = 1;
            have = alt;
        }
    }
    if (c->pieces_text && c->pieces_text[0])
        return ensure_image_pieces(c, present, say, prog, ud, why, n);

    /* NOTHING TO CHECK IT AGAINST is a state with two sides. A file
     * already here and no hash is the developer arrangement and is
     * fine. A URL and no hash would mean downloading five gigabytes
     * and then comparing them with nothing -- which hex_eq answers
     * "different" to, twice, and then blames the network for. */
    if (!c->image_sha256[0]) {
        if (present) return 0;
        if (!c->image_url[0]) {
            snprintf(why, n, "the copy of AurOS to install could not be "
                             "found.");
            return -1;
        }
        snprintf(why, n,
                 "this copy of the installer does not say what AurOS should "
                 "look like, so it will not download five gigabytes and hope. "
                 "Download the installer again.");
        return -1;
    }

    unsigned char dig[32];
    if (present) {
        talk(say, ud, "checking the copy of AurOS on this computer");
        if (image_hash(c->image_path, dig, prog, ud, why, n) == 0 &&
            hex_eq(dig, c->image_sha256))
            return 0;
    }
    if (!c->image_url[0]) {
        if (present)
            snprintf(why, n,
                     "the copy of AurOS beside the installer is not the one "
                     "this installer was built for. Download the installer "
                     "again.");
        else
            snprintf(why, n, "the copy of AurOS to install could not be "
                             "found.");
        return -1;
    }

    fetch_ctx fc = { prog, ud };
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) {
            /* Second time, from nothing: a resume onto a file that was
             * already wrong resumes being wrong.
             *
             * AND THE TRUNCATE IS CHECKED. Ignored, a file that could
             * not be emptied -- open elsewhere, read-only, a sharing
             * violation -- turned "start again from nothing" into
             * another resume of the same corruption. */
            talk(say, ud, "that did not come down cleanly; starting again");
            if (plat_file_put(c->image_path, "", 0, why, n) != 0) return -1;
        }
        talk(say, ud, present || attempt
                          ? "downloading AurOS (this carries on if it stops)"
                          : "downloading AurOS -- about five gigabytes. This "
                            "can take a while and carries on if it stops.");
        /* A FAILED FETCH TAKES THE NEXT ATTEMPT, it does not end the
         * loop. It used to `return -1`, which made the second attempt
         * unreachable in exactly the case the comment above describes:
         * a complete-length file that hashes wrong asks for
         * `Range: bytes=<filesize>-`, the standards-correct answer is
         * 416, and the user was told to try again later -- for ever,
         * because nothing ever removed the bad file. */
        if (plat_fetch(c->image_url, c->image_path, fetch_progress, &fc,
                       why, n) != 0) {
            if (attempt == 0) { present = 0; continue; }
            return -1;
        }
        talk(say, ud, "checking what was downloaded");
        if (image_hash(c->image_path, dig, prog, ud, why, n) != 0) return -1;
        if (hex_eq(dig, c->image_sha256)) {
            talk(say, ud, "the download is complete and has been checked");
            return 0;
        }
        present = 0;
    }
    snprintf(why, n,
             "what was downloaded is not what it should be, twice. Something "
             "between here and AurOS is changing it -- try a different "
             "network.");
    return -1;
}

int ab_fetch_image(const ab_choice *c, ab_say say, ab_progress prog,
                   void *ud, char *why, size_t n)
{
    return ensure_image(c, say, prog, ud, why, n);
}

static int phase_prepare_nostick(const ab_choice *c, ab_machine *m,
                                 ab_say say, ab_progress prog, void *ud,
                                 char *why, size_t n);

static int phase_prepare(const ab_choice *c, pf_report *r, ab_machine *m,
                         ab_say say, ab_progress prog, void *ud,
                         char *why, size_t n)
{
    (void)r;
    if (c->no_stick)
        return phase_prepare_nostick(c, m, say, prog, ud, why, n);
    if (find_stick(c, m, why, n) != 0) return -1;
    if (ensure_image(c, say, prog, ud, why, n) != 0) return -1;

    uint64_t image_bytes = 0;
    if (plat_file_size(c->image_path, &image_bytes) != 0 || !image_bytes) {
        snprintf(why, n, "the copy of AurOS to install could not be found.");
        return -1;
    }
    uint32_t ss = m->stick_sector;
    if (!ss) {
        snprintf(why, n,
                 "the memory stick will not say how large its blocks are.");
        return -1;
    }
    uint64_t if_, il, rf, rl, sf, sl;
    if (ab_stick_layout(m->stick_bytes, ss, image_bytes, m->esp_length,
                        &if_, &il, &rf, &rl, &sf, &sl, why, n) != 0)
        return -1;

    /* Where the root filesystem is INSIDE the image, read out of the
     * image's own partition table rather than from anything typed. */
    static uint8_t ihead[2 * 4096 + 16384];
    size_t hneed = 2 * 512 + 16384;
    if (plat_file_read(c->image_path, 0, ihead, hneed, why, n) != 0) {
        snprintf(why, n, "the copy of AurOS to install could not be read.");
        return -1;
    }
    uint64_t root_off = 0, root_len = 0;
    if (fmt_image_root_extent(ihead, hneed, 512, &root_off, &root_len,
                              why, n) != 0)
        return -1;
    /* AND THE PART THAT STARTS A COMPUTER, hashed for the same reason
     * the root is. The installer copies this extent onto the machine
     * and its read-back compares the disk with the STICK -- the same
     * bytes it just wrote -- so rot anywhere in the shim or in grub
     * was copied faithfully, verified faithfully, and reported as
     * success, and the machine then started nothing. Nothing in the
     * product had a number to compare those bytes against. */
    uint64_t esp_off = 0, esp_len = 0;
    if (fmt_image_esp_extent(ihead, hneed, 512, &esp_off, &esp_len,
                             why, n) != 0)
        return -1;
    if (esp_off > image_bytes || esp_len > image_bytes - esp_off) {
        snprintf(why, n,
                 "the copy of AurOS on this computer is incomplete -- the "
                 "part that starts a computer is not all there. Download it "
                 "again.");
        return -1;
    }
    /* AND IT HAS TO BE INSIDE THE FILE.
     *
     * A half-downloaded auros-desktop.img -- 4.9 GB of 5.2, with the
     * partition table at the front intact -- passed everything: the
     * streaming loop wrote only what existed and hashed only what it
     * wrote, and the read-back read the full root_len including the
     * tail that was never written. The hashes differed, and the user
     * was told her memory stick was faulty. She would have replaced
     * the stick until she gave up. */
    if (!root_len || root_off > image_bytes ||
        root_len > image_bytes - root_off) {
        snprintf(why, n,
                 "the copy of AurOS on this computer is incomplete -- it is "
                 "%llu MB and describes %llu MB of itself. Download it again.",
                 (unsigned long long)(image_bytes / MIB),
                 (unsigned long long)((root_off + root_len) / MIB));
        return -1;
    }

    talk(say, ud, "preparing the memory stick (everything on it is erased)");

    /* FROM HERE THE STICK IS WRITTEN TO, AND NOTHING ELSE EVER IS. */
    plat_allow_write(m->stick_index);

    static uint8_t head[2 * 4096 + 16384], tail[16384 + 4096];
    uint8_t guid[16];
    {
        /* The stick's own GUID, derived from what is going on it. */
        fmt_sha s; unsigned char dg[32];
        fmt_sha_start(&s);
        fmt_sha_feed(&s, c->stick_serial, strlen(c->stick_serial));
        fmt_sha_feed(&s, m->disk_serial, strlen(m->disk_serial));
        fmt_sha_done(&s, dg);
        memcpy(guid, dg, 16);
        guid[7] = (uint8_t)((guid[7] & 0x0F) | 0x40);
        guid[8] = (uint8_t)((guid[8] & 0x3F) | 0x80);
    }
    fmt_part parts[3] = {
        { FMT_GUID_IMAGE,  "AUROS-IMAGE",  if_, il },
        { FMT_GUID_RECORD, "AUROS-RECORD", rf,  rl },
        { FMT_GUID_SAVED,  "AUROS-SAVED",  sf,  sl },
    };
    if (fmt_gpt_build(m->stick_bytes, ss, guid, parts, 3, head, tail,
                      why, n) != 0)
        return -1;

    /* THE TABLE GOES DOWN LAST, not first. A stick interrupted while
     * the image is being copied then has no table saying it is an
     * AurOS stick, so nothing later mistakes a half-written one for a
     * finished one -- the same reason the saved copy's header is
     * written after what it describes. */
    m->image_part_off  = if_ * ss;
    m->image_part_len  = (il - if_ + 1) * ss;
    m->record_part_off = rf * ss;
    m->saved_part_off  = sf * ss;
    m->saved_part_len  = (sl - sf + 1) * ss;

    /* The image, streamed, hashing the root extent as it passes. */
    static uint8_t buf[1 << 20];
    fmt_sha rs, es;
    fmt_sha_start(&rs);
    fmt_sha_start(&es);
    uint64_t at = 0, dst = m->image_part_off + FMT_MANIFEST_BYTES;
    while (at < image_bytes) {
        size_t chunk = (size_t)(image_bytes - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (plat_file_read(c->image_path, at, buf, chunk, why, n) != 0)
            return -1;
        if (plat_write(m->stick_index, dst + at, buf, chunk, why, n) != 0)
            return -1;
        /* The part of this megabyte that is inside the root extent,
         * and the part inside the EFI one. Two windows over one pass,
         * because the image is five gigabytes and reading it twice to
         * hash it twice is twenty minutes of somebody's afternoon. */
        uint64_t lo = at > root_off ? at : root_off;
        uint64_t hi = at + chunk;
        if (hi > root_off + root_len) hi = root_off + root_len;
        if (hi > lo) fmt_sha_feed(&rs, buf + (lo - at), (size_t)(hi - lo));
        lo = at > esp_off ? at : esp_off;
        hi = at + chunk;
        if (hi > esp_off + esp_len) hi = esp_off + esp_len;
        if (hi > lo) fmt_sha_feed(&es, buf + (lo - at), (size_t)(hi - lo));
        at += chunk;
        if (prog) prog((int)(90 * at / image_bytes), ud);
    }
    unsigned char root_sha[32], esp_sha[32];
    fmt_sha_done(&rs, root_sha);
    fmt_sha_done(&es, esp_sha);

    /* The two areas that are read by magic number are zeroed, so that
     * whatever was on this stick last week cannot be believed. */
    memset(buf, 0, 4096);
    if (plat_write(m->stick_index, m->record_part_off, buf, 4096, why, n) != 0 ||
        plat_write(m->stick_index, m->saved_part_off,  buf, 4096, why, n) != 0)
        return -1;

    uint8_t man[FMT_MANIFEST_BYTES];
    fmt_manifest(man, image_bytes, root_off, root_len, 512, root_sha,
                 c->profile, esp_off, esp_len, esp_sha);
    if (plat_write(m->stick_index, m->image_part_off, man, sizeof man,
                   why, n) != 0)
        return -1;
    if (plat_flush(m->stick_index, why, n) != 0) return -1;

    if (plat_write(m->stick_index, 0, head, fmt_gpt_head_bytes(ss),
                   why, n) != 0)
        return -1;
    if (plat_write(m->stick_index, fmt_gpt_backup_lba(m->stick_bytes, ss) * ss,
                   tail, fmt_gpt_tail_bytes(ss), why, n) != 0)
        return -1;
    if (plat_flush(m->stick_index, why, n) != 0) return -1;
    plat_reread(m->stick_index, why, n);

    /* AND READ IT BACK. A stick nobody checked is a stick that fails
     * after the restart, on a machine with no Windows to complain
     * from. This is three minutes on USB 2 and it is worth every one. */
    talk(say, ud, "checking the memory stick");
    fmt_sha_start(&rs);
    at = 0;
    while (at < root_len) {
        size_t chunk = (size_t)(root_len - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (plat_read(m->stick_index,
                      m->image_part_off + FMT_MANIFEST_BYTES + root_off + at,
                      buf, chunk, why, n) != 0)
            return -1;
        fmt_sha_feed(&rs, buf, chunk);
        at += chunk;
        if (prog) prog(90 + (int)(10 * at / root_len), ud);
    }
    unsigned char back[32];
    fmt_sha_done(&rs, back);
    if (memcmp(back, root_sha, 32) != 0) {
        snprintf(why, n,
                 "this memory stick did not give back what was written to "
                 "it. Try a different one -- nothing on this computer has "
                 "been changed.");
        return -1;
    }
    talk(say, ud, "the memory stick is ready and has been checked");
    plat_release();
    return 0;
}

/* ── phase 2, without a memory stick ─────────────────────────────── */

/* NOTHING IS WRITTEN TO ANY DISK BUT AS FILES. The image is fetched or
 * found at image_path on the Windows drive, and the one thing this adds
 * is its manifest -- the same 4096 bytes a stick carries in front of
 * the image, with the same hashes -- as the file image_path +
 * ".manifest", read back and compared. After the restart the staging
 * environment reads both through a read-only mount (src/aurstage/
 * winvol.c) and checks every hash in it before it touches anything. */
static int phase_prepare_nostick(const ab_choice *c, ab_machine *m,
                                 ab_say say, ab_progress prog, void *ud,
                                 char *why, size_t n)
{
    (void)m;
    if (ensure_image(c, say, prog, ud, why, n) != 0) return -1;
    uint64_t image_bytes = 0;
    if (plat_file_size(c->image_path, &image_bytes) != 0 || !image_bytes) {
        snprintf(why, n, "the copy of AurOS to install could not be found.");
        return -1;
    }
    static uint8_t ihead[2 * 4096 + 16384];
    size_t hneed = 2 * 512 + 16384;
    if (plat_file_read(c->image_path, 0, ihead, hneed, why, n) != 0) {
        snprintf(why, n, "the copy of AurOS to install could not be read.");
        return -1;
    }
    uint64_t root_off = 0, root_len = 0, esp_off = 0, esp_len = 0;
    if (fmt_image_root_extent(ihead, hneed, 512, &root_off, &root_len,
                              why, n) != 0 ||
        fmt_image_esp_extent(ihead, hneed, 512, &esp_off, &esp_len,
                             why, n) != 0)
        return -1;
    if (!root_len || root_off > image_bytes || root_len > image_bytes - root_off ||
        !esp_len || esp_off > image_bytes || esp_len > image_bytes - esp_off) {
        snprintf(why, n,
                 "the copy of AurOS on this computer is incomplete. Delete the "
                 "AurOS folder on the Windows drive and try again.");
        return -1;
    }

    talk(say, ud, "noting down what the copy of AurOS must look like");
    static uint8_t buf[1 << 20];
    unsigned char root_sha[32], esp_sha[32];
    const struct { uint64_t off, len; unsigned char *out; int lo, hi; } X[2] = {
        { esp_off,  esp_len,  esp_sha,  95, 96 },
        { root_off, root_len, root_sha, 96, 100 },
    };
    for (int k = 0; k < 2; k++) {
        fmt_sha h; fmt_sha_start(&h);
        for (uint64_t at = 0; at < X[k].len; ) {
            size_t take = X[k].len - at > sizeof buf ? sizeof buf
                                                     : (size_t)(X[k].len - at);
            if (plat_file_read(c->image_path, X[k].off + at, buf, take,
                               why, n) != 0)
                return -1;
            fmt_sha_feed(&h, buf, take);
            at += take;
            if (prog) prog(X[k].lo + (int)((X[k].hi - X[k].lo) * at / X[k].len),
                           ud);
        }
        fmt_sha_done(&h, X[k].out);
    }

    uint8_t man[FMT_MANIFEST_BYTES], back[FMT_MANIFEST_BYTES];
    fmt_manifest(man, image_bytes, root_off, root_len, 512, root_sha,
                 c->profile, esp_off, esp_len, esp_sha);
    char mp[600];
    if ((size_t)snprintf(mp, sizeof mp, "%s.manifest", c->image_path)
            >= sizeof mp) {
        snprintf(why, n, "the folder AurOS is in has too long a name.");
        return -1;
    }
    if (plat_file_put(mp, man, sizeof man, why, n) != 0) return -1;
    if (plat_file_read(mp, 0, back, sizeof back, why, n) != 0 ||
        memcmp(back, man, sizeof man) != 0) {
        snprintf(why, n, "the note describing AurOS did not read back the way "
                         "it was written. Nothing has been changed.");
        return -1;
    }
    talk(say, ud, "AurOS is ready on the Windows drive; no memory stick is "
                  "used");
    return 0;
}

/* ── what she chose, for the installed system ────────────────────── */

/* A value that goes into choices.conf: printable, no '=' ambiguity is
 * possible because the reader takes everything after the first '=',
 * and no newline, which is the one character that could make one
 * answer into two. The installed side checks every value again against
 * what it actually has; this only keeps the file well-formed. */
static int choice_value_ok(const char *v)
{
    if (!v || !*v) return 0;
    for (const char *p = v; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) return 0;
    return strlen(v) < 96;
}

/* \EFI\AurOS\choices.conf. The EFI partition is the one place both
 * sides of the restart can reach that the install leaves exactly as it
 * found it, and it is written in both modes, stick or not. A failure
 * here is a note, not a refusal: AurOS installs with its defaults. */
static void write_choices(const ab_choice *c, const char *esp,
                          ab_say say, void *ud)
{
    char buf[1024], path[512], w[PLAT_WHY];
    int k = snprintf(buf, sizeof buf,
                     "# What was chosen in the AurOS installer. Applied once, at\n"
                     "# AurOS's first start (/usr/lib/auros/choices.sh).\n");
    const struct { const char *key, *val; } kv[] = {
        { "language", c->language }, { "keyboard", c->keyboard },
        { "timezone", c->timezone }, { "theme",    c->theme },
        { "shell",    c->shell_archetype },
    };
    for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++)
        if (choice_value_ok(kv[i].val) && k > 0 && (size_t)k < sizeof buf)
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s=%s\n",
                          kv[i].key, kv[i].val);
    if (k <= 0 || (size_t)k >= sizeof buf) return;
    snprintf(path, sizeof path, "%s/EFI/AurOS/choices.conf", esp);
    if (plat_file_put(path, buf, (size_t)k, w, sizeof w) != 0)
        talk(say, ud, "your language, keyboard and look could not be written "
                      "down (%s); AurOS will start with its own", w);
}

/* ── phase 3: the last thing before the restart ──────────────────── */

/* ROOM ON THE EFI PARTITION, ASKED BEFORE THE FIRST BYTE GOES THERE.
 * Preflight asked already, but of a number; this asks of the files about
 * to be copied and the partition as it is now. A copy that runs out of
 * room half way leaves a kernel with no initramfs beside it and a boot
 * entry that is never armed -- harmless, since nothing else has changed,
 * but a refusal with the sizes in it is what a person can act on.
 *
 * Files a previous attempt left under \EFI\AurOS are replaced, so their
 * bytes count as room; but a replacement is written beside the old file
 * and then moved over it, so the largest new file has to fit on top. */
static int esp_room(const char *esp, const char *kern, const char *init,
                    char *why, size_t n)
{
    static const char *mine[] = { "staging.efi", "staging.img", "shimx64.efi",
                                  "grubx64.efi", "mmx64.efi", "grub.cfg",
                                  "choices.conf" };
    static const char *extra[] = { PAYLOAD_SHIM, PAYLOAD_GRUB, PAYLOAD_MOKMGR };
    uint64_t want = 0, largest = 0, sz = 0, old = 0;
    char p[512], w2[PLAT_WHY];
    const char *have[2] = { kern, init };
    for (int i = 0; i < 2; i++) {
        sz = 0;
        if (plat_file_size(have[i], &sz) != 0) {
            snprintf(why, n, "the part of the installer that starts your "
                             "computer could not be measured.");
            return -1;
        }
        want += sz; if (sz > largest) largest = sz;
    }
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) {
        sz = 0;
        if (plat_payload(extra[i], p, sizeof p, w2, sizeof w2) == 0 &&
            plat_file_size(p, &sz) == 0) {
            want += sz; if (sz > largest) largest = sz;
        }
    }
    want += 1ULL << 20;                 /* grub.cfg, choices, FAT clusters */
    for (size_t i = 0; i < sizeof mine / sizeof mine[0]; i++) {
        snprintf(p, sizeof p, "%s/EFI/AurOS/%s", esp, mine[i]);
        sz = 0;
        if (plat_file_size(p, &sz) == 0) old += sz;
    }
    snprintf(p, sizeof p, "%s/EFI", esp);
    uint64_t free_now = plat_free_space(p);
    if (!free_now) return 0;            /* could not be asked; the copy will say */
    if (free_now + old < want || (old && free_now < largest)) {
        snprintf(why, n,
                 "the start-up partition does not have room for what AurOS "
                 "starts from: it needs %llu MB there and has %llu MB free. "
                 "Nothing has been changed.",
                 (unsigned long long)((want + MIB - 1) / MIB),
                 (unsigned long long)(free_now / MIB));
        return -1;
    }
    return 0;
}

static int phase_handoff(const ab_choice *c, pf_report *r, ab_machine *m,
                         ab_say say, void *ud, char *why, size_t n)
{
    /* THE STAGING ENVIRONMENT, OUT OF THE INSTALLER ITSELF.
     *
     * A path in the choice means a developer running against a build
     * tree. Empty -- which is what a shipped installer has -- means it
     * is carried inside this executable, and plat_payload unpacks it.
     * Either way what comes out is a path, so the copy below does not
     * know which world it is in. */
    char kern[512], init[512];
    if (c->kernel_path[0]) {
        snprintf(kern, sizeof kern, "%s", c->kernel_path);
    } else if (plat_payload(PAYLOAD_KERNEL, kern, sizeof kern, why, n) != 0) {
        return -1;
    }
    if (c->initrd_path[0]) {
        snprintf(init, sizeof init, "%s", c->initrd_path);
    } else if (plat_payload(PAYLOAD_INITRD, init, sizeof init, why, n) != 0) {
        plat_payload_free();
        return -1;
    }

    char esp[256];
    if (plat_esp_open(esp, sizeof esp, why, n) != 0)
        { plat_payload_free(); return -1; }

    if (esp_room(esp, kern, init, why, n) != 0) {
        plat_esp_close();
        plat_payload_free();
        return -1;
    }

    char dst[512];
    snprintf(dst, sizeof dst, "%s/EFI/AurOS/staging.efi", esp);
    if (plat_file_copy(kern, dst, why, n) != 0) {
        plat_esp_close();
        plat_payload_free();
        return -1;
    }

    /* The journal, into a cpio, into a gzip, appended to the staging
     * image. The kernel unpacks several initramfs segments in turn, so
     * the staging environment finds /aurbridge/journal.json as if it
     * had always been in there. */
    m->run_id = (uint64_t)time(NULL);
    fmt_journal j;
    memset(&j, 0, sizeof j);
    snprintf(j.disk_serial, sizeof j.disk_serial, "%s", m->disk_serial);
    snprintf(j.disk_model,  sizeof j.disk_model,  "%s", m->disk_model);
    j.disk_bytes      = m->disk_bytes;
    j.logical_sector  = m->logical_sector;
    snprintf(j.win_part, sizeof j.win_part, "%s", m->win_part);
    j.win_start_lba   = m->win_start_lba;
    j.win_sectors     = m->win_sectors;
    j.win_ntfs_serial = m->win_ntfs_serial;
    snprintf(j.gpt_sha256, sizeof j.gpt_sha256, "%s", m->gpt_sha256);
    snprintf(j.stage, sizeof j.stage, "armed");
    snprintf(j.boot_from, sizeof j.boot_from, "esp");
    /* The one thing on the stick that says WHICH AurOS she asked for.
     * Without it the staging environment installs whichever image it
     * finds first, on a stick that may hold two. */
    snprintf(j.profile, sizeof j.profile, "%s", c->profile);
    snprintf(j.image_on, sizeof j.image_on, "%s",
             c->no_stick ? "windows" : "stick");
    j.run_id = m->run_id;
    j.written_unix = (uint64_t)time(NULL);

    char jtxt[2048];
    size_t jn = fmt_journal_json(&j, jtxt, sizeof jtxt);
    if (!jn) {
        snprintf(why, n, "the installer's note about this computer could not "
                         "be written.");
        plat_esp_close();
        plat_payload_free();
        return -1;
    }
    static uint8_t cpio[8192], gz[16384];
    size_t cn = fmt_cpio_one("aurbridge/journal.json", jtxt, jn,
                             cpio, sizeof cpio);
    size_t gn = cn ? fmt_gzip_store(cpio, cn, gz, sizeof gz) : 0;
    if (!gn) {
        snprintf(why, n, "the installer's note could not be packed.");
        plat_esp_close();
        plat_payload_free();
        return -1;
    }

    snprintf(dst, sizeof dst, "%s/EFI/AurOS/staging.img", esp);
    if (plat_file_copy(init, dst, why, n) != 0) {
        plat_esp_close();
        plat_payload_free();
        return -1;
    }
    {
        uint64_t have = 0;
        if (plat_file_size(dst, &have) != 0) {
            snprintf(why, n, "the staging environment could not be measured.");
            plat_esp_close();
            plat_payload_free();
            return -1;
        }
        /* APPENDED RATHER THAN REWRITTEN, which this comment claimed
         * while the code underneath it declared a 64 MB static buffer,
         * read the whole staging image into it, and wrote all 13 MB
         * back with CREATE_ALWAYS -- truncating a file on the EFI
         * partition to zero and putting it back, to add four hundred
         * bytes, on the machine this product exists for. The comment
         * was right and the code was not; now they agree. */
        (void)have;
        if (plat_file_append(dst, gz, gn, why, n) != 0) {
            plat_esp_close();
            plat_payload_free();
            return -1;
        }
    }
    /* ── THE WAY IN, WITH SECURE BOOT ON ─────────────────────────────
     *
     * The entry used to start staging.efi -- the kernel -- directly,
     * with its command line in the entry. The kernel is signed by
     * Canonical; firmware with Secure Boot on trusts Microsoft, so on
     * nearly every Windows 10 and 11 PC the firmware refused it,
     * consumed BootNext and started Windows, and the install simply
     * never happened. Nothing tested that path with Secure Boot on:
     * every end-to-end test handed QEMU the kernel with -kernel.
     *
     * So the entry starts shim -- Canonical's, signed by Microsoft, the
     * same file the installed system boots through -- which starts
     * Canonical's signed grub, which verifies and starts the kernel.
     *
     * ONLY UNDER \EFI\AurOS (R12). That grub has /EFI/ubuntu baked in as
     * its prefix, and an earlier version of this wrote its configuration
     * there too -- and refused any PC where a real Ubuntu already had
     * one. Neither was needed: loaded from \EFI\AurOS, it reads
     * \EFI\AurOS\grub.cfg first, beside itself, and does so even when a
     * real \EFI\ubuntu\grub.cfg is present (tools/nosticktest.sh boots
     * it that way, with Secure Boot on). So \EFI\ubuntu is left alone,
     * and a PC that dual-boots Ubuntu is not refused. */
    const char *extra = getenv("AURBRIDGE_STAGING_KARGS");  /* tests only */
    char shim[512], grub[512], mokm[512], w2[PLAT_WHY];
    int chain = plat_payload(PAYLOAD_SHIM, shim, sizeof shim, w2, sizeof w2) == 0 &&
                plat_payload(PAYLOAD_GRUB, grub, sizeof grub, w2, sizeof w2) == 0;
    int have_mm = chain &&
                  plat_payload(PAYLOAD_MOKMGR, mokm, sizeof mokm, w2, sizeof w2) == 0;
    if (chain) {
        /* A configuration an earlier test build of this installer put in
         * \EFI\ubuntu is ours to take away, and only that one: it says
         * so on its first line. Anything else there is somebody's. */
        char ucfg[512], back[16];
        snprintf(ucfg, sizeof ucfg, "%s/EFI/ubuntu/grub.cfg", esp);
        uint64_t have = 0;
        if (plat_file_size(ucfg, &have) == 0 && have >= 11 &&
            plat_file_read(ucfg, 0, back, 11, w2, sizeof w2) == 0 &&
            memcmp(back, "# AurBridge", 11) == 0)
            plat_file_delete(ucfg, w2, sizeof w2);
        char cfg[2048];
        int cl = snprintf(cfg, sizeof cfg,
            "# AurBridge: starts the AurOS installer, once. Written by the\n"
            "# AurOS installer, and safe to delete once AurOS is installed.\n"
            "set timeout=0\n"
            "set default=0\n"
            /* THE PARTITION GRUB WAS STARTED FROM, which is this one:
             * $cmdpath is "(hd0,gpt1)/EFI/AurOS". A search by file name
             * alone would take the first partition with an old
             * \EFI\AurOS\staging.efi on it -- a second EFI partition, a
             * second disk -- and the journal check would then refuse,
             * safely, on the wrong machine's copy. The search stays as
             * the fallback for a grub without regexp. */
            "if regexp --set=1:aurdev '^\\((.*)\\)' \"$cmdpath\"; then\n"
            "    set root=\"$aurdev\"\n"
            "fi\n"
            "if [ ! -f /EFI/AurOS/staging.efi ]; then\n"
            "    search --no-floppy --file --set=root /EFI/AurOS/staging.efi\n"
            "fi\n"
            "menuentry 'AurOS installer' {\n"
            "    linux /EFI/AurOS/staging.efi aurstage.install "
            "aurstage.profile=%s console=tty0%s%s\n"
            "    initrd /EFI/AurOS/staging.img\n"
            "}\n",
            c->profile, extra ? " " : "", extra ? extra : "");
        if (cl <= 0 || (size_t)cl >= sizeof cfg) {
            snprintf(why, n, "the start-up settings could not be written.");
            plat_esp_close(); plat_payload_free();
            return -1;
        }
        char d1[512], d2[512], d3[512], d4[512];
        snprintf(d1, sizeof d1, "%s/EFI/AurOS/shimx64.efi", esp);
        snprintf(d2, sizeof d2, "%s/EFI/AurOS/grubx64.efi", esp);
        snprintf(d3, sizeof d3, "%s/EFI/AurOS/mmx64.efi", esp);
        snprintf(d4, sizeof d4, "%s/EFI/AurOS/grub.cfg", esp);
        if (plat_file_copy(shim, d1, why, n) != 0 ||
            plat_file_copy(grub, d2, why, n) != 0 ||
            (have_mm && plat_file_copy(mokm, d3, why, n) != 0) ||
            plat_file_put(d4, cfg, (size_t)cl, why, n) != 0) {
            plat_esp_close(); plat_payload_free();
            return -1;
        }
    } else if (r && r->secure_boot == 1) {
        snprintf(why, n,
                 "this copy of the installer cannot start AurOS on a computer "
                 "with Secure Boot switched on, and this one has it on. "
                 "Download the installer again. Nothing has been changed.");
        plat_esp_close(); plat_payload_free();
        return -1;
    } else {
        talk(say, ud, "NOTE: this build starts the installer directly, which "
                      "only works with Secure Boot off");
    }
    write_choices(c, esp, say, ud);
    plat_esp_close();
    /* Anything that was unpacked to get here is gone again. The bytes
     * that matter are on the EFI partition now. */
    plat_payload_free();

    /* The boot entry, found by its own description and replaced, never
     * by "the entries we did not record" -- which also selects the
     * Fedora somebody installed last month. Through shim it carries no
     * command line at all: shim reads a non-empty one as the name of
     * the next thing to start, and the kernel's arguments are in
     * grub.cfg. */
    char cmdline[512];
    snprintf(cmdline, sizeof cmdline,
             "initrd=\\EFI\\AurOS\\staging.img aurstage.install "
             "aurstage.profile=%s console=tty0%s%s", c->profile,
             extra ? " " : "", extra ? extra : "");
    if (plat_boot_make("AurOS Installer", &m->esp,
                       chain ? "\\EFI\\AurOS\\shimx64.efi"
                             : "\\EFI\\AurOS\\staging.efi",
                       chain ? "" : cmdline, &m->boot_entry, why, n) != 0)
        return -1;

    /* BOOTNEXT AND NOT BOOTORDER. It is one-shot: the firmware clears
     * it as it uses it, so a machine that fails to start AurOS comes
     * back to Windows by itself, with nobody doing anything and
     * nothing to undo. That property is the whole of R4's answer and
     * it is why BootOrder is never touched. */
    if (plat_boot_next(m->boot_entry, why, n) != 0) return -1;
    m->bootnext_set = 1;
    talk(say, ud, "this computer will start AurOS once, the next time it is "
                  "switched on");
    talk(say, ud, "if anything goes wrong it comes back to Windows by itself");
    return 0;
}

/* ── the run ─────────────────────────────────────────────────────── */

int ab_run(ab_phase upto, const ab_choice *c, pf_report *r, ab_machine *m,
           ab_say say, ab_progress prog, void *ud, char *why, size_t n)
{
    pf_report own;
    if (!r) { pf_run(&own); r = &own; }
    memset(m, 0, sizeof *m);
    m->disk_index = -1; m->stick_index = -1;

    if (plat_is_sim())
        talk(say, ud, "NOTE: this is %s, not a real computer.", plat_name());

    for (ab_phase p = AB_INSPECT; p <= upto && p < AB_N; p++) {
        talk(say, ud, "── %s",
             p == AB_PREPARE && c->no_stick ? "getting AurOS ready"
                                            : ab_phase_name(p));
        int rc;
        switch (p) {
        case AB_INSPECT: rc = phase_inspect(c, r, m, say, ud, why, n); break;
        case AB_CONSENT: rc = phase_consent(c, r, m, say, ud, why, n); break;
        case AB_PREPARE: rc = phase_prepare(c, r, m, say, prog, ud, why, n); break;
        case AB_HANDOFF: rc = phase_handoff(c, r, m, say, ud, why, n); break;
        default: rc = -1; break;
        }
        if (rc != 0) {
            ab_abort(m, say, ud);
            return -1;
        }
    }
    return 0;
}

void ab_abort(ab_machine *m, ab_say say, void *ud)
{
    if (!m) return;
    /* AND WHATEVER WAS UNPACKED GOES. plat_win.c says the payload "is
     * removed again on the way out, and a leftover from a crash is 28
     * MB in a directory Windows cleans" -- a failed run is not a
     * crash, and it is the ordinary path. Six error paths in
     * phase_handoff and every abort used to leave both files behind.
     * It is idempotent, so the ones that already free it can stay. */
    plat_payload_free();
    /* THE STICK IS LET GO OF FIRST. An aborted phase 2 used to leave
     * it locked and dismounted for the life of the process -- gone
     * from Explorer, and with the next write still pointed at the dead
     * handle, so a user who swapped sticks wrote to the old one. */
    plat_release();

    /* AND BITLOCKER GOES BACK ON.
     *
     * Phase 1 suspends it with -RebootCount 0, which means "until
     * somebody turns it back on" -- and nothing ever did. Every phase-2
     * refusal is downstream of the suspension, including the one whose
     * message says in as many words that nothing on this computer has
     * been changed, and each of them left the machine with its key in
     * the clear on disk, permanently. It is also, flatly, a write to
     * the computer's own disk, which phases.h says these phases never
     * make. */
    if (m->bitlocker_suspended) {
        char tail[512];
        if (plat_run("manage-bde -protectors -enable C:", tail, sizeof tail) == 0) {
            m->bitlocker_suspended = 0;
            talk(say, ud, "BitLocker is switched back on");
        } else {
            talk(say, ud, "BitLocker could NOT be switched back on (%s). Open "
                          "Windows, search for BitLocker, and choose Resume "
                          "protection -- until you do, this computer's drive "
                          "is not encrypted.", tail);
        }
    }
    if (m->bootnext_set) {
        char why[PLAT_WHY];
        if (plat_boot_next_clear(why, sizeof why) == 0) {
            m->bootnext_set = 0;
            talk(say, ud, "this computer will start Windows as usual");
        } else {
            talk(say, ud, "AurOS could not take back the one-time start-up "
                          "setting (%s). Nothing on this computer has been "
                          "changed, and if AurOS does start it will come "
                          "straight back to Windows.", why);
        }
    }
    /* Nothing else is undone because nothing else was done. The files
     * on the EFI partition are in a directory AurOS made and are
     * harmless; the memory stick is a memory stick. */
}

/* ── the selftest ────────────────────────────────────────────────── */

int ab_selftest(void)
{
    int bad = 0;
    char why[400];
    uint64_t a, b, cf, d, e, f;

    /* A 16 GB stick, a 5 GB image, a 100 MiB EFI partition. */
    if (ab_stick_layout(16ull * 1000 * 1000 * 1000, 512,
                        5ull * 1024 * 1024 * 1024, 100 * MIB,
                        &a, &b, &cf, &d, &e, &f, why, sizeof why) != 0) {
        fprintf(stderr, "phases: an ordinary stick was refused: %s\n", why);
        bad++;
    } else {
        if (a % (MIB / 512)) { fprintf(stderr, "phases: the image partition is not megabyte-aligned\n"); bad++; }
        if (b < a || cf != b + 1 || d < cf || e != d + 1 || f < e) {
            fprintf(stderr, "phases: the three partitions are out of order\n");
            bad++;
        }
        if ((b - a + 1) * 512 < 5ull * 1024 * 1024 * 1024 + FMT_MANIFEST_BYTES) {
            fprintf(stderr, "phases: the image partition is too small for the image\n");
            bad++;
        }
        if ((f - e + 1) * 512 < ab_saved_bytes(100 * MIB, 512, 4)) {
            fprintf(stderr, "phases: the saved copy has nowhere near enough room\n");
            bad++;
        }
        if (f > fmt_gpt_last_usable(16ull * 1000 * 1000 * 1000, 512)) {
            fprintf(stderr, "phases: the last partition runs past the table\n");
            bad++;
        }
    }

    /* A 4 GB stick and the same 5 GB image must be a refusal that says
     * a number, not a truncated write. */
    if (ab_stick_layout(4ull * 1000 * 1000 * 1000, 512,
                        5ull * 1024 * 1024 * 1024, 100 * MIB,
                        &a, &b, &cf, &d, &e, &f, why, sizeof why) == 0) {
        fprintf(stderr, "phases: a stick too small for the image was accepted\n");
        bad++;
    } else if (!strstr(why, "MB")) {
        fprintf(stderr, "phases: the too-small refusal does not say a size\n");
        bad++;
    }

    /* A gigabyte EFI partition -- an OEM one -- must still fit on a
     * 16 GB stick beside a 5 GB image. */
    if (ab_stick_layout(16ull * 1000 * 1000 * 1000, 512,
                        5ull * 1024 * 1024 * 1024, 1024 * MIB,
                        &a, &b, &cf, &d, &e, &f, why, sizeof why) != 0) {
        fprintf(stderr, "phases: an OEM gigabyte EFI partition did not fit: %s\n", why);
        bad++;
    }

    /* 4Kn, where every block is eight of the other kind. */
    if (ab_stick_layout(16ull * 1000 * 1000 * 1000, 4096,
                        5ull * 1024 * 1024 * 1024, 100 * MIB,
                        &a, &b, &cf, &d, &e, &f, why, sizeof why) != 0) {
        fprintf(stderr, "phases: a 4Kn stick was refused: %s\n", why);
        bad++;
    } else if ((b - a + 1) * 4096 < 5ull * 1024 * 1024 * 1024) {
        fprintf(stderr, "phases: on 4Kn the image partition is too small\n");
        bad++;
    }

    /* And the saved copy must never be smaller than what the staging
     * environment will ask for after the restart. */
    {
        uint32_t ss = 512;
        uint64_t arr = ((128ull * 128) + ss - 1) / ss * ss;
        uint64_t stage_wants = 4096 + ss + 2 * (ss + arr) + 100 * MIB
                             + 4ull * (8192 + 2 * ss);
        if (ab_saved_bytes(100 * MIB, ss, 4) < stage_wants) {
            fprintf(stderr, "phases: the saved copy is smaller than the "
                            "installer will ask for\n");
            bad++;
        }
    }

    if (!bad) fprintf(stderr, "phases: the stick layout holds up\n");
    return bad;
}
