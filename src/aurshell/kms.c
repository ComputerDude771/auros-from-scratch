#include "kms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); return p; }

/* DRM's "get" ioctls are two-pass: call once with null pointers to
 * learn the counts, allocate, then call again to fill. Getting this
 * wrong is the most common way to end up reading uninitialised ids. */
static int get_resources(int fd, struct drm_mode_card_res *res,
                         uint32_t **conns, uint32_t **crtcs)
{
    memset(res, 0, sizeof *res);
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) < 0) return -1;
    if (res->count_connectors == 0 || res->count_crtcs == 0) return -1;

    *conns = xcalloc(res->count_connectors, sizeof **conns);
    *crtcs = xcalloc(res->count_crtcs, sizeof **crtcs);
    if (!*conns || !*crtcs) return -1;

    res->connector_id_ptr = (uint64_t)(uintptr_t)*conns;
    res->crtc_id_ptr      = (uint64_t)(uintptr_t)*crtcs;
    res->fb_id_ptr = 0;  res->count_fbs = 0;
    res->encoder_id_ptr = 0; res->count_encoders = 0;

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) < 0) return -1;
    return 0;
}

static int create_buffer(kms_display *d, int idx, int w, int h)
{
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof creq);
    creq.width = (uint32_t)w; creq.height = (uint32_t)h; creq.bpp = 32;
    if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        fprintf(stderr, "aurshell: CREATE_DUMB: %s\n", strerror(errno));
        return -1;
    }
    d->buf[idx].handle = creq.handle;
    d->buf[idx].pitch  = creq.pitch;
    d->buf[idx].size   = creq.size;

    struct drm_mode_fb_cmd fb;
    memset(&fb, 0, sizeof fb);
    fb.width = (uint32_t)w; fb.height = (uint32_t)h;
    fb.pitch = creq.pitch; fb.bpp = 32; fb.depth = 24; fb.handle = creq.handle;
    if (ioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        fprintf(stderr, "aurshell: ADDFB: %s\n", strerror(errno));
        return -1;
    }
    d->buf[idx].fb_id = fb.fb_id;

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = creq.handle;
    if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        fprintf(stderr, "aurshell: MAP_DUMB: %s\n", strerror(errno));
        return -1;
    }
    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     d->fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        fprintf(stderr, "aurshell: mmap scanout: %s\n", strerror(errno));
        return -1;
    }
    d->buf[idx].map = map;
    memset(map, 0, creq.size);
    return 0;
}

static kms_display *try_card(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return NULL;

    kms_display *d = xcalloc(1, sizeof *d);
    if (!d) { close(fd); return NULL; }
    d->fd = fd;

    struct drm_mode_card_res res;
    uint32_t *conns = NULL, *crtcs = NULL;
    if (get_resources(fd, &res, &conns, &crtcs) < 0) goto fail;

    /* First connected output with at least one mode wins. A laptop's
     * panel and an attached monitor both appear here; multi-head is a
     * later problem, but picking a DISCONNECTED one would leave the
     * user staring at a black screen with no error. */
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn;
        memset(&conn, 0, sizeof conn);
        conn.connector_id = conns[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.connection != 1 /* DRM_MODE_CONNECTED */ || conn.count_modes == 0) continue;

        struct drm_mode_modeinfo *modes = xcalloc(conn.count_modes, sizeof *modes);
        uint32_t *encs = xcalloc(conn.count_encoders ? conn.count_encoders : 1, sizeof *encs);
        uint32_t *props = xcalloc(conn.count_props ? conn.count_props : 1, sizeof *props);
        uint64_t *pvals = xcalloc(conn.count_props ? conn.count_props : 1, sizeof *pvals);
        if (!modes || !encs) { free(modes); free(encs); free(props); free(pvals); continue; }

        conn.modes_ptr      = (uint64_t)(uintptr_t)modes;
        conn.encoders_ptr   = (uint64_t)(uintptr_t)encs;
        conn.props_ptr      = (uint64_t)(uintptr_t)props;
        conn.prop_values_ptr= (uint64_t)(uintptr_t)pvals;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || conn.count_modes == 0) {
            free(modes); free(encs); free(props); free(pvals); continue;
        }

        /* modes[0] is the preferred mode by convention. */
        struct drm_mode_modeinfo *mode = &modes[0];

        /* Find a CRTC: the current encoder's if it has one, else the
         * first CRTC this connector's encoders can drive. */
        uint32_t crtc_id = 0;
        if (conn.encoder_id) {
            struct drm_mode_get_encoder enc;
            memset(&enc, 0, sizeof enc);
            enc.encoder_id = conn.encoder_id;
            if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
                crtc_id = enc.crtc_id;
        }
        if (!crtc_id) {
            for (uint32_t e = 0; e < conn.count_encoders && !crtc_id; e++) {
                struct drm_mode_get_encoder enc;
                memset(&enc, 0, sizeof enc);
                enc.encoder_id = encs[e];
                if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0) continue;
                for (uint32_t c = 0; c < res.count_crtcs; c++)
                    if (enc.possible_crtcs & (1u << c)) { crtc_id = crtcs[c]; break; }
            }
        }
        if (!crtc_id) { free(modes); free(encs); free(props); free(pvals); continue; }

        d->connector_id = conn.connector_id;
        d->crtc_id      = crtc_id;
        d->width        = mode->hdisplay;
        d->height       = mode->vdisplay;
        d->refresh_mhz  = mode->vrefresh * 1000;

        d->saved_mode = xcalloc(1, sizeof *mode);
        memcpy(d->saved_mode, mode, sizeof *mode);

        /* Remember the CRTC we found it in so the console can be put
         * back when the shell exits. */
        struct drm_mode_crtc old;
        memset(&old, 0, sizeof old);
        old.crtc_id = crtc_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &old) == 0) d->saved_crtc_id = old.fb_id;

        free(encs); free(props); free(pvals);

        if (create_buffer(d, 0, d->width, d->height) < 0 ||
            create_buffer(d, 1, d->width, d->height) < 0) { free(modes); goto fail; }

        struct drm_mode_crtc set;
        memset(&set, 0, sizeof set);
        set.crtc_id = crtc_id;
        set.fb_id   = d->buf[0].fb_id;
        set.set_connectors_ptr = (uint64_t)(uintptr_t)&d->connector_id;
        set.count_connectors   = 1;
        set.mode = *mode;
        set.mode_valid = 1;
        if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
            fprintf(stderr, "aurshell: SETCRTC: %s\n", strerror(errno));
            free(modes); goto fail;
        }
        free(modes);
        free(conns); free(crtcs);
        return d;
    }

fail:
    free(conns); free(crtcs);
    if (d) { free(d->saved_mode); close(d->fd); free(d); }
    return NULL;
}

kms_display *kms_open(const char *card)
{
    if (card) return try_card(card);
    static const char *cards[] = { "/dev/dri/card0", "/dev/dri/card1",
                                   "/dev/dri/card2", "/dev/dri/card3", NULL };
    for (int i = 0; cards[i]; i++) {
        kms_display *d = try_card(cards[i]);
        if (d) return d;
    }
    fprintf(stderr, "aurshell: no usable DRM device with a connected output\n");
    return NULL;
}

void kms_close(kms_display *d)
{
    if (!d) return;
    for (int i = 0; i < 2; i++) {
        if (d->buf[i].map) munmap(d->buf[i].map, d->buf[i].size);
        if (d->buf[i].fb_id) {
            uint32_t id = d->buf[i].fb_id;
            ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &id);
        }
        if (d->buf[i].handle) {
            struct drm_mode_destroy_dumb dreq;
            memset(&dreq, 0, sizeof dreq);
            dreq.handle = d->buf[i].handle;
            ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        }
    }
    free(d->saved_mode);
    close(d->fd);
    free(d);
}

surface *kms_back_surface(kms_display *d)
{
    int back = d->front ^ 1;
    static surface s;
    s.px     = (uint32_t *)d->buf[back].map;
    s.w      = d->width;
    s.h      = d->height;
    /* The scanout pitch is in bytes and is frequently padded well past
     * width*4 for alignment. Treating it as width would shear the whole
     * image diagonally. */
    s.stride = (int)(d->buf[back].pitch / 4);
    return &s;
}

int kms_flip(kms_display *d)
{
    int back = d->front ^ 1;
    struct drm_mode_crtc_page_flip flip;
    memset(&flip, 0, sizeof flip);
    flip.crtc_id = d->crtc_id;
    flip.fb_id   = d->buf[back].fb_id;
    flip.flags   = DRM_MODE_PAGE_FLIP_EVENT;

    if (ioctl(d->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0) {
        /* Some simple drivers (and simpledrm in particular) have no
         * page-flip. Fall back to a mode set, which is slower and can
         * tear but is universally supported -- better a working desktop
         * on old hardware than a black screen. */
        struct drm_mode_crtc set;
        memset(&set, 0, sizeof set);
        set.crtc_id = d->crtc_id;
        set.fb_id   = d->buf[back].fb_id;
        set.set_connectors_ptr = (uint64_t)(uintptr_t)&d->connector_id;
        set.count_connectors   = 1;
        if (d->saved_mode) { set.mode = *(struct drm_mode_modeinfo *)d->saved_mode; set.mode_valid = 1; }
        if (ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) return -1;
        d->front = back;
        return 0;
    }

    /* Drain the flip completion so the next frame does not paint into a
     * buffer the scanout engine is still reading. */
    struct pollfd pfd = { .fd = d->fd, .events = POLLIN };
    if (poll(&pfd, 1, 200) > 0) {
        char ev[1024];
        ssize_t n = read(d->fd, ev, sizeof ev);
        (void)n;
    }
    d->front = back;
    return 0;
}
