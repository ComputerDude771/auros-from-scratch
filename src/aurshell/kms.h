/* kms.h — direct output to a DRM/KMS display.
 *
 * The shell paints straight to the scanout buffer: no X11, no Wayland
 * compositor, no Mesa. That keeps the whole pixel pipeline ours, which
 * is what makes theming total rather than "as much as the toolkit
 * allows", and it means the desktop starts on anything the kernel can
 * set a mode on -- which on ten-year-old hardware is a real advantage.
 *
 * Deliberately built on raw ioctls against the kernel's DRM UAPI rather
 * than libdrm: the ABI here is the stable kernel interface, the calls
 * we need are a handful, and it removes a build dependency from the one
 * component that must come up before anything else does.
 */
#ifndef AUROS_KMS_H
#define AUROS_KMS_H

#include <stdint.h>
#include "draw.h"

typedef struct {
    int      fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t saved_crtc_id;
    int      width, height;
    uint32_t refresh_mhz;

    /* Double buffered: paint into back, flip, swap. Painting into the
     * buffer currently being scanned out is what produces tearing. */
    struct {
        uint32_t handle, fb_id, pitch;
        uint64_t size;
        uint8_t *map;
    } buf[2];
    int      front;
    void    *saved_mode;      /* drm_mode_modeinfo, restored on close */
    uint32_t dpms_prop;       /* the connector's DPMS property, or 0  */
    int      dpms_looked;     /* ...and whether we have been to look  */
} kms_display;

/* Opens the first connected output on the given card (NULL = try
 * /dev/dri/card0..card3). Returns NULL with a message on stderr. */
kms_display *kms_open(const char *card);
void         kms_close(kms_display *d);

/* The back buffer, wrapped as a drawing surface. */
surface     *kms_back_surface(kms_display *d);

/* Present the back buffer. Blocks until the flip completes so the
 * caller cannot paint into a buffer still being scanned out. */
int          kms_flip(kms_display *d);

/* Power the panel down, and back up. Returns -1 on a driver with no
 * DPMS property at all (simpledrm has none), in which case the caller
 * should paint black instead -- worth less, since the backlight stays
 * on, and not nothing. */
int          kms_screen_off(kms_display *d, int off);

/* Give the display up to another virtual terminal, and take it back.
 * Only one process is DRM master; a VT switch that skips these leaves
 * the shell painting into a buffer nothing scans out. */
int          kms_drop_master(int fd);
int          kms_set_master(int fd);

#endif
