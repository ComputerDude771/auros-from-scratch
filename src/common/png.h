/* png.h — minimal PNG writer with a self-contained DEFLATE encoder.
 * No zlib: the base system should not need a compression library just
 * to hand the user a picture of their wallpaper. */
#ifndef AUROS_PNG_H
#define AUROS_PNG_H
#include <stdint.h>
/* px is w*h packed 0x00RRGGBB. Returns 0 on success. */
int png_write_rgb(const char *path, const uint32_t *px, int w, int h);
#endif
