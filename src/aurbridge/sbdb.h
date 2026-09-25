/* sbdb.h — does this PC's firmware trust what AurOS starts with?
 *
 * With Secure Boot on, the firmware starts only files signed by a
 * certificate in its "db" variable. AurOS's installer restarts through
 * shim, which Microsoft signs with its third-party key -- "Microsoft
 * Corporation UEFI CA 2011" today -- and almost every PC sold with
 * Windows lists that key. Some do not: a number of Secured-core PCs
 * (some Surface, some Lenovo) ship listing only Microsoft's Windows
 * key, and on those the firmware refuses shim, BootNext is spent, and
 * the PC starts Windows again with nothing installed and nothing said.
 *
 * So preflight reads db -- from Windows, before anything is changed --
 * and asks whether any certificate in it has the subject name of a key
 * the shim we carry is signed with. Those names are read off the shim
 * at build time (build/aurbridge, from `sbverify --list`) and baked in
 * as AUROS_SHIM_CAS, so a newer shim signed with Microsoft's 2023 key
 * is checked for that key without anybody editing this file.
 *
 * Portable C, no platform calls: the same code runs in the Windows
 * installer, in the simulation, and in the selftest.
 */
#ifndef AURBRIDGE_SBDB_H
#define AURBRIDGE_SBDB_H

#include <stddef.h>
#include <stdint.h>

#include "preflight.h"

/* db is the variable's data as GetFirmwareEnvironmentVariable returns
 * it: an array of EFI_SIGNATURE_LISTs, WITHOUT efivarfs's leading four
 * bytes of attributes. cas is one certificate subject name per line.
 *
 *   1   an X.509 certificate in db has one of those subject names
 *   0   db was read to the end and none does
 *  -1   db is not a well-formed list; nothing can be said
 *
 * seen, when given, gets the subject names of every certificate in db,
 * separated by "; " -- for the log, and for the person who asks what
 * their PC does trust. */
int sbdb_trusts(const unsigned char *db, size_t n, const char *cas,
                char *seen, size_t sn);

/* The subject common name of one DER certificate, or -1. */
int sbdb_cert_cn(const unsigned char *der, size_t n, char *cn, size_t cn_n);

/* The preflight result for Secure Boot, for both preflights (the
 * Windows one and pf_sim.c): secure_boot is -1/0/1; db_rc and dbx_rc
 * what reading each returned (0 read, else could not); cas the baked
 * names. Appends one pf_result to r.
 *
 * TWO DIFFERENT NOS. A PC that trusts no third-party key at all has
 * one firmware setting off, and the card says which (and the wizard
 * draws it). A PC that trusts a DIFFERENT third-party key -- Microsoft's
 * 2023 one but not the 2011 one this shim is signed with, or the key
 * revoked in dbx -- has nothing to switch on: this installer is what is
 * too old for it, and the card says that instead. */
void sbdb_judge(pf_report *r, int secure_boot,
                int db_rc, const unsigned char *db, size_t n,
                int dbx_rc, const unsigned char *dbx, size_t xn,
                const char *cas);

/* The one id the wizard draws a picture for, and the other no. */
#define SBDB_BLOCK_ID "secure-boot-ca"
#define SBDB_NEWER_ID "secure-boot-ca-newer"

int sbdb_selftest(void);

#endif
