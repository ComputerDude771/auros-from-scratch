/* winvol.h — the Windows drive, mounted READ-ONLY, for the no-stick
 * mode and for nothing else.
 *
 * WHY THIS FILE IS A DEPARTURE, and what keeps it narrow.
 *
 * The staging environment was built never to mount the Windows volume:
 * boot.c does not even load ntfs3, "because a module that is not loaded
 * cannot be mounted by accident", and image.h explains why the image
 * lives on the stick instead of on C:. All of that is still the design.
 *
 * The no-stick mode exists because a person asked for an install with
 * no memory stick, having been told what it costs. In that mode the
 * image is a file AurBridge left in \AurOS\ on the Windows drive, and
 * the only way to read a file inside NTFS without writing an NTFS
 * reader is the kernel's. So:
 *
 *   - ntfs3 is loaded HERE, on this path, and nowhere else.
 *   - The mount is MS_RDONLY, and statvfs() is asked afterwards whether
 *     the kernel agrees; a mount that is not read-only is undone and
 *     refused.
 *   - It is never mounted while ntfsresize runs: the installer mounts,
 *     reads, and unmounts before the shrink, and mounts again only to
 *     copy the image into space the shrink has already freed.
 *   - An unmount that fails is a refusal, not a warning. ntfsresize on
 *     a mounted volume is the one thing this environment must never do.
 */
#ifndef AUROS_WINVOL_H
#define AUROS_WINVOL_H
#include <stddef.h>

/* Mount `windev` read-only at a fixed place under /run/aurstage and
 * return that place in `root`. Returns 0, or -1 with a sentence. */
int  winvol_mount(const char *windev, char *root, size_t rn,
                  char *why, size_t n);

/* Unmount it. Returns 0 when nothing is mounted afterwards, including
 * when nothing was. -1 with a sentence if it is still mounted. */
int  winvol_umount(char *why, size_t n);

/* 1 while this program has it mounted. */
int  winvol_mounted(void);

#endif
