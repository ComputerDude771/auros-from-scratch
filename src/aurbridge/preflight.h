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
 *  Safety invariant: UNKNOWN IS NEVER OK. A check that cannot determine
 *  its answer blocks. Every query below therefore has a failure path
 *  that ends in a BLOCK, not in a silent zero.
 *
 *  Each check maps to a risk in docs/research/red-team.md (R-numbers in
 *  the check comments) so the register and the code cannot drift apart.
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef AURBRIDGE_PREFLIGHT_H
#define AURBRIDGE_PREFLIGHT_H

/* Nothing in this header needs a Windows type -- the structures below
 * are plain C -- and src/aurbridge/phases.c has to build for Linux as
 * well, so that the phase engine can be run against the simulated
 * machine in plat_sim.c. The include stays for every Windows
 * translation unit that includes this and then calls the API, and is
 * simply not there on a host that has no windows.h. */
#ifdef _WIN32
#  include <windows.h>
#endif
#include <stddef.h>
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
    int      is_removable;          /* removable bus OR removable media*/
    int      removable_media;       /* the RemovableMedia flag alone   */
    int      bus_type;              /* STORAGE_BUS_TYPE, -1 unknown    */
    int      is_recovery_stick;     /* the one stick pf_set_recovery_  */
                                    /* stick() nominated (R4)          */
    int      is_system;             /* holds the running Windows       */
    int      partition_style;       /* 0=MBR 1=GPT 2=RAW               */
    int      primary_partitions;    /* MBR only; 4 means no room (R8)  */
    int      is_dynamic;            /* LDM: MBR 0x42 or GPT LDM GUIDs  */
    int      is_storage_space;      /* Storage Spaces pool or vdisk    */
    uint32_t logical_sector;        /* bytes; 0 = could not read (R7)  */
    uint32_t physical_sector;       /* bytes; 0 = could not read       */
    uint64_t esp_offset;            /* byte offset of the ESP, 0 none  */
    uint64_t esp_length;
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
    uint64_t free_bytes;            /* free INSIDE the filesystem      */
    int      disk_index;
    int      bitlocker;             /* -1 unknown, 0 off, 1 protected  */
    int      dirty;                 /* NTFS dirty bit                  */
    int      hibernated;            /* hiberfil.sys present + sized    */

    /* R7: what the volume can actually give up, measured from $Bitmap
     * rather than inferred from free space. Only filled in for the
     * system volume; shrink_measured is 0 when the read failed, and a
     * 0 there is a BLOCK, never an assumption. */
    int      shrink_measured;
    uint64_t cluster_bytes;
    uint64_t used_bytes;            /* allocated clusters × cluster    */
    uint64_t shrink_floor_bytes;    /* last allocated byte + 1         */
    uint64_t offline_shrinkable;    /* ntfsresize bound — what we use  */
    uint64_t online_shrinkable;     /* Windows' own bound — reported   */
    uint64_t pagefile_bytes;
    uint64_t hiberfil_bytes;
} pf_volume;

typedef struct {
    pf_result results[PF_MAX_RESULTS];
    int       n;
    int       n_block, n_warn;

    /* Machine facts gathered along the way, for the wizard to render. */
    int       is_uefi;
    int       secure_boot;          /* -1 unknown, 0 off, 1 on         */
    int       is_admin;
    int       on_ac_power;          /* -1 unknown, 0 battery, 1 mains  */
    int       has_battery;          /* -1 unknown, 0 none, 1 present   */
    int       battery_percent;      /* -1 if no battery or unknown     */
    uint64_t  ram_bytes;

    pf_disk   disks[PF_MAX_DISKS];
    int       n_disks;
    pf_volume volumes[PF_MAX_VOLUMES];
    int       n_volumes;

    int       system_disk;          /* index into disks[]              */
    int       system_volume;        /* index into volumes[], -1 none   */
    uint64_t  system_offset;        /* byte offset of C: on that disk  */

    /* R12: the ESP we must write the staging environment into, and
     * must never reformat. esp_found is -1 until it has been looked
     * for, 0 when the system disk has none, 1 when measured. */
    int       esp_found;
    uint64_t  esp_size_bytes;
    uint64_t  esp_free_bytes;
    char      esp_volume[64];       /* "\\?\Volume{...}\", "" if none  */

    /* R1: third-party sector-level encryption. -1 unknown, 0 none,
     * 1 found. Unknown blocks; see check_encryption(). */
    int       fde_third_party;

    /* R4/R11: the nominated recovery stick, echoed so that a saved
     * report explains its own removable-media verdict. */
    char      recovery_serial[64];
    int       recovery_disk;        /* index into disks[], -1 = none   */
} pf_report;

/* What AurOS asks for, and what Windows must be left with -- the R7
 * numbers preflight.c's space check is built on, public so that the
 * no-stick mode (phases.c) can ask the same question again with the
 * image counted as well: in that mode five gigabytes of AurOS sit on
 * the Windows drive through the shrink, and preflight measured before
 * they arrived. */
#define PF_AUROS_NEED_BYTES   (28ULL * 1024 * 1024 * 1024)
#define PF_WINDOWS_KEEP_BYTES ( 8ULL * 1024 * 1024 * 1024)

/* ── the recovery USB (R4 · R11) ──────────────────────────────────────
 *
 * R11 wants every removable disk unplugged, so that writing to the
 * wrong target is physically impossible. R4 makes a recovery USB
 * mandatory and phase 2 has to write to it. Both cannot hold at once,
 * so exactly one stick is let through: the one the user nominates here.
 * Everything else removable still refuses.
 *
 * The nomination is by disk serial number because that is the only
 * identifier that survives a replug into a different port — drive
 * letters and PhysicalDriveN numbers do not. A disk whose serial is
 * empty, or too short to be an identifier, can never be nominated: we
 * would be unable to tell it from the user's backup drive.
 *
 * Intended use: run pf_run() once with no nomination, show the user the
 * removable disks the report found (model, size, serial), then call
 * this with the serial of the one they picked and run pf_run() again.
 * Pass NULL or "" to withdraw the nomination; that is the default.
 *
 * Nominating a stick does not make it safe to write to, and does not
 * check that it is present. It only stops preflight refusing because it
 * is plugged in. Preflight still never writes, so nothing reachable
 * from here can touch the stick.
 *
 * Not thread-safe against a concurrent pf_run(); set it before the
 * wizard's worker thread starts. */
void        pf_set_recovery_stick(const char *serial);

/* The current nomination. Never NULL; "" when none. */
const char *pf_recovery_stick(void);

/* Run every check. Read-only; safe to call at any time.
 *
 * Returns early with one block and no machine facts when the process is
 * not elevated, so consumers must not assume n_disks > 0 on a blocked
 * report. */
void pf_run(pf_report *r);

/* PF_OK only when there is not a single BLOCK. Warnings are the
 * wizard's problem, blocks are non-negotiable. */
int  pf_is_go(const pf_report *r);

/* Exercises the decision logic that does not need a real machine —
 * boot-sector classification, bus and power verdicts, serial matching,
 * the $Bitmap scan and the partition-type GUIDs — against constructed
 * inputs. Prints one line per case; returns 0 when all pass. Touches
 * no disk. Built into the library so CI can call it; preflight.c also
 * grows a main() for it under -DPF_SELFTEST. */
int  pf_selftest(void);

void pf_print_human(const pf_report *r);
void pf_print_json (const pf_report *r);

#endif
