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
 *               file. Built with the HOST compiler and never shipped,
 *               so the phase engine can be run end to end against the
 *               same synthetic machine tools/machine.sh gives the
 *               staging environment -- and the stick it produces is
 *               then the stick the installer is tested with.
 *
 *               It used to say "builds for Linux as well as for
 *               Windows", and it does not any more: its plat_fetch is
 *               a socket client behind #ifndef _WIN32, so a Windows
 *               link of it fails on an undefined symbol. Nothing
 *               builds it that way and nothing should -- build/aurbridge
 *               refuses to put the simulated platform into a shipped
 *               binary at all -- but a header that describes a
 *               property the tree does not have is a header somebody
 *               will rely on.
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
/* Add to the end of a file that already exists. The journal is four
 * hundred bytes going on the end of a 13 MB staging image, on an EFI
 * partition, on a ten-year-old laptop -- and the alternative is
 * reading the whole thing into memory and writing it back, which is
 * what this replaced. */
int plat_file_append(const char *to, const void *buf, size_t n,
                     char *why, size_t wn);
/* Remove a file this program made. 0 if it is gone afterwards,
 * including when it was never there. */
int plat_file_delete(const char *path, char *why, size_t wn);
/* Rename `from` to `to`, replacing `to`. On Windows a rename across
 * volumes becomes a copy and a delete, which is what moving an image
 * from beside the installer onto the Windows drive needs. */
int plat_file_rename(const char *from, const char *to, char *why, size_t wn);

/* How much room is left on the volume holding `path` -- which need not
 * exist yet; the DIRECTORY it would go in is what is asked about. 0 if
 * this computer will not say, which is not a refusal: a machine that
 * cannot answer is not a machine to stop. */
uint64_t plat_free_space(const char *path);

/* ── what the installer carries inside itself ────────────────────── */
/*
 * THE PRODUCT IS ONE FILE SOMEBODY DOUBLE-CLICKS, and for a long time
 * it was three: the wizard, plus a kernel and an initramfs that had to
 * be sitting in the same folder. A person who downloads one of three
 * files and double-clicks it gets "the copy of AurOS to install could
 * not be found", which is a true sentence about a mistake we made.
 *
 * The staging environment is about 28 MB, which is an ordinary size
 * for an installer, so it travels INSIDE the executable. On Windows
 * that is a PE resource -- the mechanism Authenticode already covers,
 * so signing and carrying a payload do not fight. The simulation has
 * no PE, so it reads the same names out of a directory, which is also
 * what a developer build does.
 *
 * `name` is one of the PAYLOAD_* names below. On success `path` holds
 * somewhere the caller may read the bytes from, and plat_payload_free()
 * removes anything that had to be unpacked to get there.
 */
#define PAYLOAD_KERNEL  "staging-kernel"
#define PAYLOAD_INITRD  "staging-initrd"
/* THE WAY IN UNDER SECURE BOOT. The staging kernel is signed by
 * Canonical, and firmware trusts Microsoft: started directly, as the
 * boot entry used to, a machine with Secure Boot on -- which is nearly
 * every Windows 10 and 11 PC -- refuses it, consumes BootNext, and
 * comes back to Windows with nothing done. So the entry starts the same
 * Microsoft-signed shim and Canonical-signed grub the installed system
 * boots through, and grub starts the kernel. Carried as three more
 * resources; see phase_handoff. */
#define PAYLOAD_SHIM    "staging-shim"
#define PAYLOAD_GRUB    "staging-grub"
#define PAYLOAD_MOKMGR  "staging-mokmgr"

int  plat_payload(const char *name, char *path, size_t pn,
                  char *why, size_t wn);
void plat_payload_free(void);
/* 1 if this binary is carrying the payload rather than expecting it in
 * a folder. The release build refuses to publish one that is not. */
int  plat_payload_embedded(void);
/* How many bytes of start-up files this binary carries (all five
 * payloads), or 0 if it carries none. Preflight measures the EFI
 * partition against this rather than against a guess: the guess (96 MB)
 * told a real Windows machine with 61 MB free that it did not have room
 * for the 33 MB that actually go there. */
uint64_t plat_payload_bytes(void);

/* ── fetching the image ──────────────────────────────────────────── */
/*
 * The image is five gigabytes and does not go inside anything. It is
 * downloaded, once, into a file beside the installer -- and RESUMED
 * rather than restarted, because the people this product is for are on
 * the connections that drop.
 *
 * `have` is how many bytes are already in `dest`; the fetch continues
 * from there with a Range request and refuses a server that ignores
 * it rather than silently writing the first megabyte into the middle
 * of the file. `progress` returns non-zero to cancel.
 */
int plat_fetch(const char *url, const char *dest,
               int (*progress)(uint64_t got, uint64_t total, void *ud),
               void *ud, char *why, size_t wn);

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

/* WHICH PARTITION THE LOADER IS ON, which a boot entry cannot do
 * without.
 *
 * The first version of plat_boot_make() built a device path containing
 * only File(\EFI\AurOS\staging.efi) and an end node, on the reasoning
 * that naming the partition would mean naming one the staging
 * environment is about to change the table of. A review pointed out
 * two things: the firmware has nothing to resolve a bare file path
 * against -- LoadImage calls LocateDevicePath, which needs a device
 * node, so every machine would have consumed BootNext and loaded
 * nothing -- and the reasoning was wrong anyway, because the install
 * only ever carves partitions out of the gap the shrink makes and
 * leaves the EFI partition's entry exactly as it found it.
 *
 * So the short-form Hard Drive path is built, as efibootmgr does:
 * HD(number, GPT, partition-GUID, start, size) / File(...) / End. */
typedef struct {
    uint32_t number;        /* 1-based partition number               */
    uint64_t first_lba;
    uint64_t blocks;
    uint8_t  guid[16];      /* the PARTITION's unique GUID, as on disk */
} plat_partition;

/* Create (or replace) a boot entry pointing at `loader` on the
 * partition `on`, with `cmdline` as its optional data. Returns the
 * Boot#### number. */
int plat_boot_make(const char *desc, const plat_partition *on,
                   const char *loader, const char *cmdline,
                   uint16_t *num_out, char *why, size_t wn);

/* Let go of the memory stick: close the raw handle and unlock the
 * volumes that were dismounted to get it. Safe at any time, including
 * twice, including when nothing was ever taken.
 *
 * It exists because an aborted phase 2 used to leave the stick locked
 * and dismounted -- invisible in Explorer -- for the life of the
 * process, and left the next write pointed at the dead handle. */
void plat_release(void);

/* Set BootNext, which is one-shot: the firmware clears it as it uses
 * it, so a machine that fails to start AurOS comes back to Windows by
 * itself with nobody doing anything. BootOrder is never touched. */
int plat_boot_next(uint16_t num, char *why, size_t wn);
/* And take it away again, for an abort before the restart. */
int plat_boot_next_clear(char *why, size_t wn);

/* Secure Boot: -1 not known, 0 off, 1 on. On Windows, what Windows
 * itself records; in the simulation, the file secureboot ("1"). */
int plat_secure_boot(void);

/* One of the firmware's signature databases, "db" (the signers Secure
 * Boot trusts) or "dbx" (what it has revoked), as the variable's data
 * (src/aurbridge/sbdb.h reads them). Reading only; it needs
 * SeSystemEnvironmentPrivilege like everything else here. In the
 * simulation, the files db.bin and dbx.bin. */
int plat_efi_sigdb(const char *name, unsigned char *buf, size_t cap,
                   size_t *got, char *why, size_t wn);

/* ── running something else ──────────────────────────────────────── */

/* `tail` gets the last of its output, which is what an error message
 * needs. Returns the exit status, or -1 if it would not start. */
int plat_run(const char *cmdline, char *tail, size_t n);

/* ── the one restart ─────────────────────────────────────────────── */

/* Restart the computer, now. The wizard's last button, after phase 3:
 * the staging environment is armed with BootNext and this is the
 * restart it is waiting for. A restart, never a shut down -- with Fast
 * Startup on, Windows' "shut down" leaves the drive half-asleep, which
 * the staging environment refuses to touch. */
int plat_restart(char *why, size_t wn);

/* ── which implementation is this ────────────────────────────────── */

/* 1 when this is the simulated machine. Everything user-facing says
 * so, loudly: a simulation that could be mistaken for the real thing
 * is how somebody ends up believing a test result about a machine that
 * was never touched. */
int plat_is_sim(void);
const char *plat_name(void);

#endif
