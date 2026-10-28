/* sbdb.c — see sbdb.h. Reads only what it is handed. */
#include <stdio.h>
#include <string.h>

#include "sbdb.h"

/* EFI_CERT_X509_GUID a5c059a1-94e4-4aa7-87b5-ab155c2bf072, in the
 * mixed-endian order it has in memory. The other list types in a real
 * db -- SHA-256 hashes of single files, mostly -- say nothing about
 * which signers are trusted and are stepped over. */
static const unsigned char X509_GUID[16] = {
    0xa1,0x59,0xc0,0xa5, 0xe4,0x94, 0xa7,0x4a,
    0x87,0xb5, 0xab,0x15,0x5c,0x2b,0xf0,0x72 };

static uint32_t le32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ── just enough DER ─────────────────────────────────────────────────
 *
 * One element: its tag, and where its contents start and how long they
 * are, checked against what is left. Certificates only use one-byte
 * tags and definite lengths; anything else is not a certificate this
 * needs to understand, and says so by failing. */
typedef struct { unsigned tag; const unsigned char *v; size_t n; } der_el;

static int der_next(const unsigned char **p, size_t *left, der_el *e)
{
    const unsigned char *q = *p;
    size_t n = *left;
    if (n < 2 || (q[0] & 0x1f) == 0x1f) return -1;
    size_t len = q[1], hl = 2;
    if (len & 0x80) {
        size_t k = len & 0x7f;
        if (k == 0 || k > 4 || n < 2 + k) return -1;
        len = 0;
        for (size_t i = 0; i < k; i++) len = (len << 8) | q[2 + i];
        hl += k;
    }
    if (len > n - hl) return -1;
    e->tag = q[0]; e->v = q + hl; e->n = len;
    *p = q + hl + len;
    *left = n - hl - len;
    return 0;
}

static int der_enter(const der_el *e, unsigned tag,
                     const unsigned char **p, size_t *left)
{
    if (e->tag != tag) return -1;
    *p = e->v; *left = e->n;
    return 0;
}

/* The common name out of a Name: SEQUENCE OF SET OF SEQUENCE { OID,
 * value }. 2.5.4.3 is 06 03 55 04 03. BMPString is UTF-16BE, and only
 * its ASCII survives, which is all the names compared here have. */
static int name_cn(const der_el *name, char *cn, size_t cn_n)
{
    const unsigned char *p; size_t left;
    if (der_enter(name, 0x30, &p, &left) != 0) return -1;
    while (left) {
        der_el rdn, atv, oid, val;
        if (der_next(&p, &left, &rdn) != 0 || rdn.tag != 0x31) return -1;
        const unsigned char *r = rdn.v; size_t rl = rdn.n;
        while (rl) {
            if (der_next(&r, &rl, &atv) != 0 || atv.tag != 0x30) return -1;
            const unsigned char *a = atv.v; size_t al = atv.n;
            if (der_next(&a, &al, &oid) != 0 || oid.tag != 0x06) return -1;
            if (der_next(&a, &al, &val) != 0) return -1;
            if (oid.n != 3 || memcmp(oid.v, "\x55\x04\x03", 3) != 0) continue;
            size_t o = 0;
            if (val.tag == 0x1e) {                        /* BMPString */
                for (size_t i = 0; i + 1 < val.n && o + 1 < cn_n; i += 2)
                    cn[o++] = (val.v[i] == 0 && val.v[i + 1] >= 0x20 &&
                               val.v[i + 1] < 0x7f) ? (char)val.v[i + 1] : '?';
            } else if (val.tag == 0x0c || val.tag == 0x13 ||
                       val.tag == 0x14 || val.tag == 0x16) {
                for (size_t i = 0; i < val.n && o + 1 < cn_n; i++)
                    cn[o++] = (val.v[i] >= 0x20 && val.v[i] < 0x7f)
                              ? (char)val.v[i] : '?';
            } else {
                return -1;
            }
            cn[o] = 0;
            return 0;
        }
    }
    return -1;
}

/* THE SUBJECT, NOT THE ISSUER. A certificate Microsoft's third-party
 * key signed has that key's name in it too, as its issuer; finding the
 * name anywhere in the bytes would call a PC that trusts one of those
 * a PC that trusts the key. */
int sbdb_cert_cn(const unsigned char *der, size_t n, char *cn, size_t cn_n)
{
    if (!der || !cn || cn_n == 0) return -1;
    cn[0] = 0;
    const unsigned char *p = der; size_t left = n;
    der_el cert, tbs, e;
    if (der_next(&p, &left, &cert) != 0 || der_enter(&cert, 0x30, &p, &left) != 0)
        return -1;
    if (der_next(&p, &left, &tbs) != 0 || der_enter(&tbs, 0x30, &p, &left) != 0)
        return -1;
    if (der_next(&p, &left, &e) != 0) return -1;
    if (e.tag == 0xa0 && der_next(&p, &left, &e) != 0) return -1; /* version */
    if (e.tag != 0x02) return -1;                                  /* serial  */
    der_el alg, issuer, validity, subject;
    if (der_next(&p, &left, &alg) != 0 || alg.tag != 0x30) return -1;
    if (der_next(&p, &left, &issuer) != 0 || issuer.tag != 0x30) return -1;
    if (der_next(&p, &left, &validity) != 0 || validity.tag != 0x30) return -1;
    if (der_next(&p, &left, &subject) != 0 || subject.tag != 0x30) return -1;
    return name_cn(&subject, cn, cn_n);
}

static int listed(const char *cas, const char *cn)
{
    size_t k = strlen(cn);
    for (const char *l = cas; l && *l; ) {
        const char *nl = strchr(l, '\n');
        size_t ln = nl ? (size_t)(nl - l) : strlen(l);
        if (ln == k && k && memcmp(l, cn, k) == 0) return 1;
        l = nl ? nl + 1 : l + ln;
    }
    return 0;
}

int sbdb_trusts(const unsigned char *db, size_t n, const char *cas,
                char *seen, size_t sn)
{
    if (seen && sn) seen[0] = 0;
    if (!db) return -1;
    int found = 0, bad = 0;
    size_t off = 0;
    while (off < n) {
        if (n - off < 28) return -1;
        const unsigned char *l = db + off;
        uint32_t lsz = le32(l + 16), hsz = le32(l + 20), ssz = le32(l + 24);
        if (lsz < 28 || lsz > n - off || hsz > lsz - 28) return -1;
        size_t body = off + 28 + hsz, end = off + lsz;
        if (end > body && (ssz <= 16 || (end - body) % ssz)) return -1;
        if (memcmp(l, X509_GUID, 16) == 0) {
            for (size_t s = body; s < end; s += ssz) {
                char cn[128];
                if (sbdb_cert_cn(db + s + 16, ssz - 16, cn, sizeof cn) != 0) {
                    bad = 1;
                    continue;
                }
                if (listed(cas, cn)) found = 1;
                if (seen && sn) {
                    size_t u = strlen(seen);
                    if (u + 1 < sn)
                        snprintf(seen + u, sn - u, "%s%s", u ? "; " : "", cn);
                }
            }
        }
        off = end;
    }
    /* A certificate that would not parse might have been the one: then
     * nothing can be said, rather than "no". */
    return found ? 1 : bad ? -1 : 0;
}

/* ── the result preflight shows ──────────────────────────────────── */

static void put(pf_report *r, const char *id, pf_severity sev,
                const char *title, const char *detail, const char *remedy)
{
    if (r->n >= PF_MAX_RESULTS) return;
    pf_result *x = &r->results[r->n++];
    memset(x, 0, sizeof *x);
    snprintf(x->id,     sizeof x->id,     "%s", id);
    snprintf(x->risk,   sizeof x->risk,   "R15");
    snprintf(x->title,  sizeof x->title,  "%s", title);
    snprintf(x->detail, sizeof x->detail, "%s", detail);
    snprintf(x->remedy, sizeof x->remedy, "%s", remedy);
    x->sev = sev;
    if (sev == PF_BLOCK) r->n_block++;
    else if (sev == PF_WARN) r->n_warn++;
}

/* The key's name for a sentence: the first line of cas. */
static void first_ca(const char *cas, char *out, size_t n)
{
    const char *nl = cas ? strchr(cas, '\n') : NULL;
    size_t k = cas ? (nl ? (size_t)(nl - cas) : strlen(cas)) : 0;
    if (k >= n) k = n - 1;
    memcpy(out, cas ? cas : "", k);
    out[k] = 0;
}

/* Is a third-party key from ANOTHER year in what the PC trusts? The
 * names are Microsoft's: "Microsoft Corporation UEFI CA 2011",
 * "Microsoft UEFI CA 2023". Not its Windows keys, and not the one for
 * graphics cards' option ROMs, which never signs a boot loader. */
static int other_third_party(const char *seen, const char *cas)
{
    char one[128];
    for (const char *p = seen; p && *p; ) {
        const char *e = strstr(p, "; ");
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k < sizeof one) {
            memcpy(one, p, k); one[k] = 0;
            if (!strncmp(one, "Microsoft", 9) && strstr(one, "UEFI CA") &&
                !strstr(one, "Windows") && !strstr(one, "Option ROM") &&
                !listed(cas, one))
                return 1;
        }
        p = e ? e + 2 : p + k;
    }
    return 0;
}

void sbdb_judge(pf_report *r, int secure_boot,
                int db_rc, const unsigned char *db, size_t n,
                int dbx_rc, const unsigned char *dbx, size_t xn,
                const char *cas)
{
    char detail[512], ca[64], seen[512];
    if (secure_boot < 0) return;            /* nothing known, nothing said */
    if (secure_boot == 0) {
        put(r, "secure-boot-off", PF_PASS, "Secure Boot is off",
            "AurOS starts with Secure Boot on or off. Nothing needs changing.", "");
        return;
    }
    if (!cas || !*cas) {
        /* A developer build without the signed shim and grub. Phase 3
         * would refuse it later; saying so now costs nothing. */
        put(r, "secure-boot-unsigned-build", PF_BLOCK,
            "This copy of the installer cannot start with Secure Boot on",
            "It was built without the signed start-up files that let AurOS "
            "start on a PC with Secure Boot on, like this one.",
            "Use a released copy of the AurOS installer.");
        return;
    }
    first_ca(cas, ca, sizeof ca);
    int t = db_rc == 0 ? sbdb_trusts(db, n, cas, seen, sizeof seen) : -1;
    /* REVOKED. A certificate in dbx is distrusted whatever db says. A
     * dbx that could not be read is not a reason to stop: it is almost
     * always hashes of single files, none of them ours. */
    int revoked = t == 1 && dbx_rc == 0 && sbdb_trusts(dbx, xn, cas, NULL, 0) == 1;
    if (t == 1 && !revoked) {
        snprintf(detail, sizeof detail,
                 "AurOS starts through the same Microsoft-signed start-up "
                 "files Ubuntu uses, and this PC's firmware trusts the key "
                 "they are signed with (%s). Nothing needs changing.", ca);
        put(r, "secure-boot-on", PF_PASS, "Secure Boot is on, and that is fine",
            detail, "");
    } else if (revoked || (t == 0 && other_third_party(seen, cas))) {
        snprintf(detail, sizeof detail,
                 "Secure Boot on this PC %s the key AurOS's start-up files "
                 "are signed with (%s)%s, so after the restart it would refuse "
                 "to start AurOS's installer. There is no setting to change: "
                 "this copy of the installer is what is too old for this PC.",
                 revoked ? "has withdrawn its trust in" : "does not trust",
                 ca, revoked ? "" : ", only Microsoft's newer one for other "
                                    "software");
        put(r, SBDB_NEWER_ID, PF_BLOCK, "This PC needs a newer AurOS installer",
            detail,
            "Nothing on this PC needs changing. Get the AurOS installer again "
            "later: one whose start-up files carry Microsoft's newer signature "
            "will start here.");
    } else if (t == 0) {
        snprintf(detail, sizeof detail,
                 "Secure Boot on this PC trusts Microsoft's key for Windows but "
                 "not Microsoft's key for other software (%s), which AurOS's "
                 "start-up files are signed with. Some Secured-core PCs come "
                 "this way. After the restart it would refuse to start AurOS's "
                 "installer, and nothing would install.%s%.120s%s",
                 ca, seen[0] ? " It trusts: " : " It trusts no signing keys at all.",
                 seen, seen[0] ? "." : "");
        put(r, SBDB_BLOCK_ID, PF_BLOCK, "This PC is set to start only Windows",
            detail,
            "Switch on one setting, then run this again. In Windows: Settings > "
            "System > Recovery (Windows 10: Update & Security > Recovery) > "
            "Advanced startup: Restart now > Troubleshoot > Advanced options > "
            "UEFI Firmware Settings. There, under Security > Secure Boot, switch "
            "on \"Allow Microsoft 3rd Party UEFI CA\" (Surface: \"Microsoft & 3rd "
            "party CA\"; Dell: \"Enable Microsoft UEFI CA\"). Leave Secure Boot "
            "on. Save and exit.");
    } else {
        /* COULD NOT LOOK. Not a refusal: almost every PC does trust the
         * key, and one that does not refuses shim at the restart and
         * starts Windows again -- the drive is not touched until the
         * installer has started. */
        put(r, "secure-boot-on", PF_INFO, "Secure Boot is on",
            "AurOS starts through Microsoft-signed start-up files, as Ubuntu "
            "does, so Secure Boot can stay on. This PC would not show which "
            "keys it trusts, so that could not be checked in advance; if it "
            "does not trust them, it starts Windows again after the restart "
            "and nothing on the drive has changed.", "");
    }
}

/* ── the selftest ─────────────────────────────────────────────────
 *
 * Certificates made here, byte by byte: only the fields the walk looks
 * at are real, which is the point -- a subject that parses, an issuer
 * that must NOT be mistaken for it, and lengths long enough to need the
 * long form. tools/secureboottest.sh runs the same code against the db
 * Microsoft's keys actually ship in. */

static size_t tlv(unsigned char *o, unsigned tag, const unsigned char *v, size_t n)
{
    size_t h = 0;
    o[h++] = (unsigned char)tag;
    if (n < 0x80) o[h++] = (unsigned char)n;
    else if (n < 0x100) { o[h++] = 0x81; o[h++] = (unsigned char)n; }
    else { o[h++] = 0x82; o[h++] = (unsigned char)(n >> 8); o[h++] = (unsigned char)n; }
    if (v != o + h) memmove(o + h, v, n);
    return h + n;
}

static size_t mk_name(unsigned char *o, const char *cn, unsigned str_tag)
{
    unsigned char a[512], b[512], c[512];
    size_t k = 0, t;
    if (strlen(cn) > 60) return 0;
    /* C=US, then CN */
    t = tlv(b, 0x06, (const unsigned char *)"\x55\x04\x06", 3);
    t += tlv(b + t, 0x13, (const unsigned char *)"US", 2);
    k = tlv(c, 0x30, b, t);
    k = tlv(a, 0x31, c, k);
    unsigned char v[512]; size_t vn = 0;
    if (str_tag == 0x1e) {
        for (const char *p = cn; *p; p++) { v[vn++] = 0; v[vn++] = (unsigned char)*p; }
    } else {
        vn = strlen(cn); memcpy(v, cn, vn);
    }
    t = tlv(b, 0x06, (const unsigned char *)"\x55\x04\x03", 3);
    t += tlv(b + t, str_tag, v, vn);
    size_t s = tlv(c, 0x30, b, t);
    k += tlv(a + k, 0x31, c, s);
    return tlv(o, 0x30, a, k);
}

static size_t mk_cert(unsigned char *o, const char *issuer, const char *subject,
                      unsigned str_tag)
{
    unsigned char t[1024], x[1024];
    size_t k = 0;
    unsigned char two = 2, one = 1;
    size_t vk = tlv(x, 0x02, &two, 1);
    k += tlv(t + k, 0xa0, x, vk);
    k += tlv(t + k, 0x02, &one, 1);
    k += tlv(t + k, 0x30, (const unsigned char *)"\x06\x01\x2a", 3);
    k += mk_name(t + k, issuer, 0x13);
    k += tlv(t + k, 0x30, (const unsigned char *)"", 0);
    k += mk_name(t + k, subject, str_tag);
    /* padding, standing in for the public key, so the lengths are long */
    unsigned char pad[200]; memset(pad, 0x5a, sizeof pad);
    k += tlv(t + k, 0x04, pad, sizeof pad);
    size_t tn = tlv(x, 0x30, t, k);
    tn += tlv(x + tn, 0x30, (const unsigned char *)"\x06\x01\x2a", 3);
    tn += tlv(x + tn, 0x03, (const unsigned char *)"\x00", 1);
    return tlv(o, 0x30, x, tn);
}

static size_t mk_list(unsigned char *o, const unsigned char *guid,
                      const unsigned char *data, size_t dn)
{
    uint32_t ssz = (uint32_t)(16 + dn), lsz = 28 + ssz;
    memcpy(o, guid, 16);
    for (int i = 0; i < 4; i++) {
        o[16 + i] = (unsigned char)(lsz >> (8 * i));
        o[20 + i] = 0;
        o[24 + i] = (unsigned char)(ssz >> (8 * i));
    }
    memset(o + 28, 0x77, 16);                        /* owner GUID */
    memcpy(o + 44, data, dn);
    return lsz;
}

int sbdb_selftest(void)
{
    int bad = 0;
    unsigned char db[4096], c[1200];
    char seen[256], cn[128];
    const char *CAS = "Microsoft Corporation UEFI CA 2011\n";
    static const unsigned char SHA256_GUID[16] = {
        0x26,0x16,0xc4,0xc1, 0x4c,0x50, 0x92,0x40,
        0xac,0xa9, 0x41,0xf9,0x36,0x93,0x43,0x28 };
    unsigned char h32[32]; memset(h32, 0x11, sizeof h32);

#define CHECK(cond, what) do { if (!(cond)) { \
        fprintf(stderr, "sbdb: %s\n", what); bad++; } } while (0)

    size_t cl = mk_cert(c, "Microsoft Corporation Third Party Marketplace Root",
                        "Microsoft Corporation UEFI CA 2011", 0x13);
    CHECK(cl > 256, "the test certificate is too short to need long lengths");
    CHECK(sbdb_cert_cn(c, cl, cn, sizeof cn) == 0 &&
          !strcmp(cn, "Microsoft Corporation UEFI CA 2011"),
          "the subject of a certificate was not read");

    /* 1. the ordinary PC: a hash list, the Windows key, the third-party key */
    size_t n = mk_list(db, SHA256_GUID, h32, sizeof h32);
    cl = mk_cert(c, "Microsoft Root Certificate Authority 2010",
                 "Microsoft Windows Production PCA 2011", 0x0c);
    n += mk_list(db + n, X509_GUID, c, cl);
    cl = mk_cert(c, "Microsoft Corporation Third Party Marketplace Root",
                 "Microsoft Corporation UEFI CA 2011", 0x13);
    n += mk_list(db + n, X509_GUID, c, cl);
    CHECK(sbdb_trusts(db, n, CAS, seen, sizeof seen) == 1,
          "an ordinary PC's db was not found to trust the third-party key");
    CHECK(!strcmp(seen, "Microsoft Windows Production PCA 2011; "
                        "Microsoft Corporation UEFI CA 2011"),
          "the list of what the PC trusts is wrong");

    /* 2. Secured-core: only the Windows key */
    n = mk_list(db, SHA256_GUID, h32, sizeof h32);
    cl = mk_cert(c, "Microsoft Root Certificate Authority 2010",
                 "Microsoft Windows Production PCA 2011", 0x0c);
    n += mk_list(db + n, X509_GUID, c, cl);
    CHECK(sbdb_trusts(db, n, CAS, seen, sizeof seen) == 0,
          "a Windows-only db was taken to trust the third-party key");

    /* 3. the name as ISSUER only, of a certificate it signed */
    cl = mk_cert(c, "Microsoft Corporation UEFI CA 2011",
                 "Some Vendor Secure Boot Signing", 0x13);
    size_t m = mk_list(db + n, X509_GUID, c, cl);
    CHECK(sbdb_trusts(db, n + m, CAS, NULL, 0) == 0,
          "the key's name as an issuer was taken for the key itself");

    /* 4. the 2023 key, as a BMPString, found when it is what is asked for */
    cl = mk_cert(c, "Microsoft RSA Devices Root CA 2021",
                 "Microsoft UEFI CA 2023", 0x1e);
    m = mk_list(db + n, X509_GUID, c, cl);
    CHECK(sbdb_trusts(db, n + m, CAS, NULL, 0) == 0,
          "the 2023 key was taken for the 2011 one");
    CHECK(sbdb_trusts(db, n + m, "Microsoft Corporation UEFI CA 2011\n"
                                 "Microsoft UEFI CA 2023\n", NULL, 0) == 1,
          "the 2023 key was not found when a shim signed with it asks");

    /* 5. damage: a list longer than the variable, a list too short */
    CHECK(sbdb_trusts(db, n - 1, CAS, NULL, 0) == -1,
          "a cut-off db was read as if it were whole");
    unsigned char tiny[20] = {0};
    CHECK(sbdb_trusts(tiny, sizeof tiny, CAS, NULL, 0) == -1,
          "twenty bytes were read as a db");
    CHECK(sbdb_trusts(db, 0, CAS, NULL, 0) == 0,
          "an empty db was not an empty answer");

    /* 6. a certificate that will not parse, in a list that does: no answer */
    n = mk_list(db, X509_GUID, (const unsigned char *)"\x30\x05\x02\x01", 4);
    CHECK(sbdb_trusts(db, n, CAS, NULL, 0) == -1,
          "an unreadable certificate was counted as a no");

    /* 7. what preflight says, and that it fits where it is shown */
    pf_report r;
    memset(&r, 0, sizeof r);
    n = mk_list(db, X509_GUID, c, mk_cert(c, "x", "Microsoft Windows Production PCA 2011", 0x13));
    sbdb_judge(&r, 1, 0, db, n, -1, NULL, 0, CAS);
    CHECK(r.n == 1 && r.n_block == 1 && !strcmp(r.results[0].id, SBDB_BLOCK_ID),
          "a Windows-only PC with Secure Boot on was not stopped");
    CHECK(strstr(r.results[0].remedy, "Save and exit.") != NULL,
          "the setting's explanation does not fit on its card");
    CHECK(strstr(r.results[0].detail, "It trusts: Microsoft Windows Production PCA 2011.") != NULL,
          "the card does not say what the PC does trust");
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 1, -1, NULL, 0, -1, NULL, 0, CAS);
    CHECK(r.n == 1 && r.n_block == 0 && r.results[0].sev == PF_INFO,
          "a db that could not be read stopped the install");
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 0, -1, NULL, 0, -1, NULL, 0, CAS);
    CHECK(r.n == 1 && r.n_block == 0 && r.results[0].sev == PF_PASS,
          "Secure Boot off was not fine");
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 1, 0, db, n, -1, NULL, 0, "");
    CHECK(r.n_block == 1, "a build without signed start-up files was let through");

    /* 8. the other no: Microsoft's 2023 third-party key, not the 2011 one */
    unsigned char dbx[2048];
    n = mk_list(db, X509_GUID, c, mk_cert(c, "x", "Microsoft Windows Production PCA 2011", 0x13));
    n += mk_list(db + n, X509_GUID, c, mk_cert(c, "x", "Microsoft UEFI CA 2023", 0x13));
    n += mk_list(db + n, X509_GUID, c, mk_cert(c, "x", "Microsoft Option ROM UEFI CA 2023", 0x13));
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 1, 0, db, n, -1, NULL, 0, CAS);
    CHECK(r.n_block == 1 && !strcmp(r.results[0].id, SBDB_NEWER_ID),
          "a PC trusting only the 2023 key was told to switch a setting on");
    /* ...and the option-ROM key alone is not a third-party key for this */
    n = mk_list(db, X509_GUID, c, mk_cert(c, "x", "Microsoft Windows Production PCA 2011", 0x13));
    n += mk_list(db + n, X509_GUID, c, mk_cert(c, "x", "Microsoft Option ROM UEFI CA 2023", 0x13));
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 1, 0, db, n, -1, NULL, 0, CAS);
    CHECK(r.n_block == 1 && !strcmp(r.results[0].id, SBDB_BLOCK_ID),
          "the option-ROM key was taken for the third-party one");

    /* 9. revoked: trusted in db, withdrawn in dbx */
    n = mk_list(db, X509_GUID, c, mk_cert(c, "x", "Microsoft Corporation UEFI CA 2011", 0x13));
    size_t xn = mk_list(dbx, SHA256_GUID, h32, sizeof h32);
    xn += mk_list(dbx + xn, X509_GUID, c, mk_cert(c, "x", "Microsoft Corporation UEFI CA 2011", 0x13));
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 1, 0, db, n, 0, dbx, xn, CAS);
    CHECK(r.n_block == 1 && !strcmp(r.results[0].id, SBDB_NEWER_ID),
          "a key revoked in dbx was still trusted");
    /* ...and a dbx of hashes, or one that would not read, changes nothing */
    xn = mk_list(dbx, SHA256_GUID, h32, sizeof h32);
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 1, 0, db, n, 0, dbx, xn, CAS);
    CHECK(r.n_block == 0 && r.results[0].sev == PF_PASS,
          "a dbx of single-file hashes stopped the install");
    memset(&r, 0, sizeof r);
    sbdb_judge(&r, 1, 0, db, n, -1, NULL, 0, CAS);
    CHECK(r.n_block == 0 && r.results[0].sev == PF_PASS,
          "a dbx that could not be read stopped the install");
#undef CHECK
    return bad;
}
