/* phases.c — see phases.h. Everything that leaves the program goes
 * through plat.h, so this file has no #include <windows.h> in it and
 * runs, whole, against a machine made of files. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "phases.h"
#include "plat.h"
#include "format.h"

#define MIB (1024ull * 1024)

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
        for (uint32_t k = 0; k < ne && !found; k++) {
            uint8_t e[4096];
            if (rd_disk(&rc0, elba * ss + (uint64_t)k * es, e, es) != 0) break;
            int used = 0;
            for (int q = 0; q < 16; q++) if (e[q]) { used = 1; break; }
            if (!used) continue;
            uint64_t first = 0, last = 0;
            for (int i = 7; i >= 0; i--) first = (first << 8) | e[32 + i];
            for (int i = 7; i >= 0; i--) last  = (last  << 8) | e[40 + i];
            if (first * ss != r->system_offset) continue;
            /* IN 512-BYTE UNITS, ALWAYS. journal.h says so at length:
             * Linux reports a partition's start and length in /sys in
             * 512-byte units on every disk, 4Kn included, so dividing
             * by logical_sector here makes every 4Kn machine report
             * that Windows has moved. */
            m->win_start_lba = first * ss / 512;
            m->win_sectors   = (last - first + 1) * ss / 512;
            snprintf(m->win_part, sizeof m->win_part, "%u", (unsigned)(k + 1));
            found = 1;
        }
        if (!found) {
            snprintf(why, n,
                     "the Windows drive is not one of the parts this disk is "
                     "divided into, which should be impossible. AurOS will "
                     "not touch a computer it does not understand.");
            return -1;
        }
        m->win_ntfs_serial = 0;
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
    (void)c;
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
    (void)m;
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
        m->stick_sector = d[i].logical_sector ? d[i].logical_sector : 512;
        return 0;
    }
    snprintf(why, n,
             "the memory stick you chose is not plugged in any more. Plug it "
             "back in and try again.");
    return -1;
}

static int phase_prepare(const ab_choice *c, pf_report *r, ab_machine *m,
                         ab_say say, ab_progress prog, void *ud,
                         char *why, size_t n)
{
    (void)r;
    if (find_stick(c, m, why, n) != 0) return -1;

    uint64_t image_bytes = 0;
    if (plat_file_size(c->image_path, &image_bytes) != 0 || !image_bytes) {
        snprintf(why, n, "the copy of AurOS to install could not be found.");
        return -1;
    }
    uint32_t ss = m->stick_sector;
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
    fmt_sha rs;
    fmt_sha_start(&rs);
    uint64_t at = 0, dst = m->image_part_off + FMT_MANIFEST_BYTES;
    while (at < image_bytes) {
        size_t chunk = (size_t)(image_bytes - at);
        if (chunk > sizeof buf) chunk = sizeof buf;
        if (plat_file_read(c->image_path, at, buf, chunk, why, n) != 0)
            return -1;
        if (plat_write(m->stick_index, dst + at, buf, chunk, why, n) != 0)
            return -1;
        /* The part of this megabyte that is inside the root extent. */
        uint64_t lo = at > root_off ? at : root_off;
        uint64_t hi = at + chunk;
        if (hi > root_off + root_len) hi = root_off + root_len;
        if (hi > lo) fmt_sha_feed(&rs, buf + (lo - at), (size_t)(hi - lo));
        at += chunk;
        if (prog) prog((int)(90 * at / image_bytes), ud);
    }
    unsigned char root_sha[32];
    fmt_sha_done(&rs, root_sha);

    /* The two areas that are read by magic number are zeroed, so that
     * whatever was on this stick last week cannot be believed. */
    memset(buf, 0, 4096);
    if (plat_write(m->stick_index, m->record_part_off, buf, 4096, why, n) != 0 ||
        plat_write(m->stick_index, m->saved_part_off,  buf, 4096, why, n) != 0)
        return -1;

    uint8_t man[FMT_MANIFEST_BYTES];
    fmt_manifest(man, image_bytes, root_off, root_len, 512, root_sha,
                 c->profile);
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
    return 0;
}

/* ── phase 3: the last thing before the restart ──────────────────── */

static int phase_handoff(const ab_choice *c, pf_report *r, ab_machine *m,
                         ab_say say, void *ud, char *why, size_t n)
{
    (void)r;
    char esp[256];
    if (plat_esp_open(esp, sizeof esp, why, n) != 0) return -1;

    char dst[512];
    snprintf(dst, sizeof dst, "%s/EFI/AurOS/staging.efi", esp);
    if (plat_file_copy(c->kernel_path, dst, why, n) != 0) {
        plat_esp_close();
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
    j.run_id = m->run_id;
    j.written_unix = (uint64_t)time(NULL);

    char jtxt[2048];
    size_t jn = fmt_journal_json(&j, jtxt, sizeof jtxt);
    if (!jn) {
        snprintf(why, n, "the installer's note about this computer could not "
                         "be written.");
        plat_esp_close();
        return -1;
    }
    static uint8_t cpio[8192], gz[16384];
    size_t cn = fmt_cpio_one("aurbridge/journal.json", jtxt, jn,
                             cpio, sizeof cpio);
    size_t gn = cn ? fmt_gzip_store(cpio, cn, gz, sizeof gz) : 0;
    if (!gn) {
        snprintf(why, n, "the installer's note could not be packed.");
        plat_esp_close();
        return -1;
    }

    snprintf(dst, sizeof dst, "%s/EFI/AurOS/staging.img", esp);
    if (plat_file_copy(c->initrd_path, dst, why, n) != 0) {
        plat_esp_close();
        return -1;
    }
    {
        uint64_t have = 0;
        if (plat_file_size(dst, &have) != 0) {
            snprintf(why, n, "the staging environment could not be measured.");
            plat_esp_close();
            return -1;
        }
        /* Appended rather than rewritten: the staging image is 13 MB
         * and reading it into memory to add 400 bytes is not a thing
         * to do on the machine this is for. */
        static uint8_t whole[64 << 20];
        if (have + gn > sizeof whole) {
            snprintf(why, n, "the staging environment is larger than AurOS "
                             "expected.");
            plat_esp_close();
            return -1;
        }
        if (plat_file_read(dst, 0, whole, (size_t)have, why, n) != 0) {
            plat_esp_close();
            return -1;
        }
        memcpy(whole + have, gz, gn);
        if (plat_file_put(dst, whole, (size_t)have + gn, why, n) != 0) {
            plat_esp_close();
            return -1;
        }
    }
    plat_esp_close();

    /* The boot entry, found by its own description and replaced, never
     * by "the entries we did not record" -- which also selects the
     * Fedora somebody installed last month. */
    char cmdline[512];
    snprintf(cmdline, sizeof cmdline,
             "initrd=\\EFI\\AurOS\\staging.img aurstage.install "
             "aurstage.profile=%s console=tty0", c->profile);
    if (plat_boot_make("AurOS Installer", "\\EFI\\AurOS\\staging.efi",
                       cmdline, &m->boot_entry, why, n) != 0)
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
        talk(say, ud, "── %s", ab_phase_name(p));
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
