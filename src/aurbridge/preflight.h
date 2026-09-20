/* ═══════════════════════════════════════════════════════════════════
 *  AurBridge preflight — decide whether this machine may be touched.
 *
 *  Design rule: REFUSE FIRST. A refused install costs one user. A failed
 *  install costs a user, their data, a support case, and possibly a
 *  lawsuit. Refusing is a first-class product outcome here, not an error
 *  dialog — every BLOCK carries a plain-language remedy.
 *
 *  Nothing in this header writes to a disk. Preflight is strictly
 *  read-only; the destructive phases live elsewhere and refuse to start
 *  unless a preflight report says PF_OK.
 *
 *  Each check maps to a risk in docs/research/red-team.md (R-numbers in
 *  the check comments) so the register and the code cannot drift apart.
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef AURBRIDGE_PREFLIGHT_H
#define AURBRIDGE_PREFLIGHT_H

#include <windows.h>
#include <stdint.h>

#define PF_MAX_RESULTS 64
#define PF_MAX_DISKS   16
#define PF_MAX_VOLUMES 32

typedef enum {
    PF_PASS  = 0,   /* checked, fine                                  */
    PF_INFO  = 1,   /* worth telling the user, not an obstacle        */
    PF_WARN  = 2,   /* proceed only with explicit informed consent    */
    PF_BLOCK = 3    /* hard stop. No override. No "advanced" checkbox */
} pf_severity;

typedef struct {
    char        id[40];        /* stable machine-readable code         */
    char        risk[8];       /* red-team register id, e.g. "R1"      */
    pf_severity sev;
    char        title[96];
    char        detail[512];   /* what we actually found               */
    char        remedy[512];   /* what the user must do about it       */
} pf_result;

typedef struct {
    int      index;                 /* PhysicalDriveN                  */
    char     model[128];
    char     serial[64];
    uint64_t size_bytes;
    int      is_removable;
    int      is_system;             /* holds the running Windows       */
    int      partition_style;       /* 0=MBR 1=GPT 2=RAW               */
    int      primary_partitions;    /* MBR only; 4 means no room (R8)  */
    int      smart_ok;              /* -1 unknown, 0 bad, 1 good       */
    uint32_t smart_reallocated;
    uint32_t smart_pending;
    uint32_t smart_uncorrectable;
    uint32_t smart_power_on_hours;
} pf_disk;

typedef struct {
    char     mount[8];              /* "C:"                            */
    char     fs[16];                /* "NTFS"                          */
    uint64_t size_bytes;
    uint64_t free_bytes;
    int      disk_index;
    int      bitlocker;             /* -1 unknown, 0 off, 1 protected  */
    int      dirty;                 /* NTFS dirty bit                  */
    int      hibernated;            /* hiberfil.sys present + sized    */
} pf_volume;

typedef struct {
    pf_result results[PF_MAX_RESULTS];
    int       n;
    int       n_block, n_warn;

    /* Machine facts gathered along the way, for the wizard to render. */
    int       is_uefi;
    int       secure_boot;          /* -1 unknown, 0 off, 1 on         */
    int       is_admin;
    int       on_ac_power;
    int       battery_percent;      /* -1 if no battery                */
    uint64_t  ram_bytes;

    pf_disk   disks[PF_MAX_DISKS];
    int       n_disks;
    pf_volume volumes[PF_MAX_VOLUMES];
    int       n_volumes;

    int       system_disk;          /* index into disks[]              */
} pf_report;

/* Run every check. Read-only; safe to call at any time. */
void pf_run(pf_report *r);

/* PF_OK only when there is not a single BLOCK. Warnings are the
 * wizard's problem, blocks are non-negotiable. */
int  pf_is_go(const pf_report *r);

void pf_print_human(const pf_report *r);
void pf_print_json (const pf_report *r);

#endif
