/* plat.h — everything AurBridge does that touches the world.
 *
 * WHY THERE IS A LAYER HERE AT ALL, when the whole program only ever
 * runs on Windows.
 *
 * The phase engine is the half of this product that decides what
 * happens to a stranger's disk before the restart. It cannot be tested
 * on the machine it is written on: there is no Windows here, wine does
 * not do raw disk handles or EFI variables, and the one thing that
 * must never be done to test it is to run it against a real computer
 * and see.
 *
 * So every call that leaves the program goes through this header, and
 * there are two implementations of it:
 *
 *   plat_win.c  the real one: CreateFileW on \\.\PhysicalDriveN,
 *               DeviceIoControl, SetFirmwareEnvironmentVariableExW,
 *               mountvol for the EFI partition.
 *   plat_sim.c  a machine made of ordinary files -- a directory with
 *               disk0.img, disk1.img, an esp/ tree and an efivars
 *               file. It builds for Linux as well as for Windows, so
 *               the phase engine can be run, end to end, against the
 *               same synthetic machine tools/machine.sh gives the
 *               staging environment -- and the stick it produces is
 *               then the stick the installer is tested with.
 *
 * That last sentence is the whole point. Before this, the test's
 * memory stick and journal were written by a python heredoc that
 * agreed with aurstage because both were written by the same person on
 * the same afternoon. Now the thing under test writes them.
 *
 * WHAT IS NOT BEHIND THIS LINE: anything destructive to the machine's
 * own disk. AurBridge never writes to it. Phases 0 to 3 write to the
 * memory stick, to files on the EFI partition, and to two firmware
 * variables, and that is the complete list -- which is why
 * plat_disk_write() refuses any disk that is not the nominated stick.
 */
#ifndef AURBRIDGE_PLAT_H
#define AURBRIDGE_PLAT_H

#include <stddef.h>
#include <stdint.h>

/* One sentence, in her words, whenever something returns non-zero. */
#define PLAT_WHY 400

/* ── disks ───────────────────────────────────────────────────────── */

typedef struct {
    int      index;            /* PhysicalDriveN                      */
    uint64_t size_bytes;
    uint32_t logical_sector;
    char     serial[64];
    char     model[128];
    int      removable;
} plat_disk;

/* How many disks, and what they are. Read-only; opens nothing
 * writable. */
int plat_disks(plat_disk *out, int max);

int plat_read(int disk_index, uint64_t off, void *buf, size_t n,
              char *why, size_t wn);

/* THE ONLY WRITE PATH TO A DISK IN THIS PROGRAM, and it refuses every
 * disk but the one plat_allow_write() has nominated. Nominating is a
 * separate call, made once, from the phase that has just checked the
 * serial number against what the user chose -- so that "write to the
 * stick" and "which disk is the stick" are two decisions in two places
 * rather than one argument that can be wrong. */
void plat_allow_write(int disk_index);
int  plat_write(int disk_index, uint64_t off, const void *buf, size_t n,
                char *why, size_t wn);
int  plat_flush(int disk_index, char *why, size_t wn);
/* Tell the operating system the disk's layout changed. */
int  plat_reread(int disk_index, char *why, size_t wn);

/* ── ordinary files ──────────────────────────────────────────────── */

int plat_file_size(const char *path, uint64_t *out);
int plat_file_read(const char *path, uint64_t off, void *buf, size_t n,
                   char *why, size_t wn);
/* Whole-file copy into a directory that may be on the EFI partition.
 * Creates the directories it needs. */
int plat_file_copy(const char *from, const char *to, char *why, size_t wn);
int plat_file_put(const char *to, const void *buf, size_t n,
                  char *why, size_t wn);

/* ── the EFI System Partition ────────────────────────────────────── */

/* Give the ESP a path we can write files into, and take it away again.
 * R12: it is never reformatted, and nothing here removes a file it did
 * not put there. `root` comes back as something to prefix a path with,
 * e.g. "S:" or "/mnt/esp". */
int  plat_esp_open(char *root, size_t n, char *why, size_t wn);
void plat_esp_close(void);

/* ── the firmware ────────────────────────────────────────────────── */

/* Find the boot entry whose description is exactly `desc`, or -1.
 *
 * BY DESCRIPTION, POSITIVELY. Never "the entries we did not record",
 * which also selects the Fedora somebody installed last month and the
 * vendor's diagnostics entry that was never in BootOrder. */
int plat_boot_find(const char *desc, uint16_t *num_out,
                   char *why, size_t wn);

/* Create (or replace) a boot entry pointing at `loader` on the ESP,
 * with `cmdline` as its optional data. Returns the Boot#### number. */
int plat_boot_make(const char *desc, const char *loader,
                   const char *cmdline, uint16_t *num_out,
                   char *why, size_t wn);

/* Set BootNext, which is one-shot: the firmware clears it as it uses
 * it, so a machine that fails to start AurOS comes back to Windows by
 * itself with nobody doing anything. BootOrder is never touched. */
int plat_boot_next(uint16_t num, char *why, size_t wn);
/* And take it away again, for an abort before the restart. */
int plat_boot_next_clear(char *why, size_t wn);

/* ── running something else ──────────────────────────────────────── */

/* `tail` gets the last of its output, which is what an error message
 * needs. Returns the exit status, or -1 if it would not start. */
int plat_run(const char *cmdline, char *tail, size_t n);

/* ── which implementation is this ────────────────────────────────── */

/* 1 when this is the simulated machine. Everything user-facing says
 * so, loudly: a simulation that could be mistaken for the real thing
 * is how somebody ends up believing a test result about a machine that
 * was never touched. */
int plat_is_sim(void);
const char *plat_name(void);

#endif
