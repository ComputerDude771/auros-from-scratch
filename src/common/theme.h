/* theme.h — AurOS runtime theme store.
 *
 * Parses the key=value files aurora generates (/etc/auros/shell.conf)
 * and the .theme source files themselves. Deliberately a flat string
 * map rather than a fixed struct: adding a theme key must never
 * require touching C code, only a template.
 */
#ifndef AUROS_THEME_H
#define AUROS_THEME_H

#include <stdint.h>

#define THEME_MAX_KEYS 256
#define THEME_KEY_LEN   48
#define THEME_VAL_LEN  192

typedef struct {
    char key[THEME_KEY_LEN];
    char val[THEME_VAL_LEN];
} theme_pair;

typedef struct {
    theme_pair pairs[THEME_MAX_KEYS];
    int n;
} theme_t;

/* Load a key=value file. Returns 0 on success, -1 if unreadable.
 * Handles: quoted and bare values, several pairs on one line, and
 * '#' comments outside quotes. Later keys override earlier ones, so
 * loading a parent then a child gives inheritance for free. */
int      theme_load(theme_t *t, const char *path);

const char *theme_str  (const theme_t *t, const char *key, const char *fallback);
int         theme_int  (const theme_t *t, const char *key, int fallback);
double      theme_num  (const theme_t *t, const char *key, double fallback);
/* Accepts #RRGGBB, 0xRRGGBB or bare RRGGBB. Returns 0x00RRGGBB. */
uint32_t    theme_color(const theme_t *t, const char *key, uint32_t fallback);

#endif
