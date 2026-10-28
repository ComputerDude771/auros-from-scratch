#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

static void theme_put(theme_t *t, const char *k, const char *v)
{
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->pairs[i].key, k) == 0) {           /* override */
            snprintf(t->pairs[i].val, THEME_VAL_LEN, "%s", v);
            return;
        }
    }
    if (t->n >= THEME_MAX_KEYS) return;
    snprintf(t->pairs[t->n].key, THEME_KEY_LEN, "%s", k);
    snprintf(t->pairs[t->n].val, THEME_VAL_LEN, "%s", v);
    t->n++;
}

/* Scan one line for every `ident = value` on it. The .theme files pack
 * two assignments per line for the ANSI block, so a one-pair-per-line
 * parser would silently drop half the palette. */
static void theme_parse_line(theme_t *t, const char *line)
{
    const char *p = line;
    while (*p) {
        if (*p == '#') return;                    /* comment to EOL */
        if (!(isalpha((unsigned char)*p) || *p == '_')) { p++; continue; }

        const char *ks = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        size_t klen = (size_t)(p - ks);
        if (klen == 0 || klen >= THEME_KEY_LEN) continue;

        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;                  /* not an assignment */
        q++;
        while (*q == ' ' || *q == '\t') q++;

        char key[THEME_KEY_LEN];
        memcpy(key, ks, klen); key[klen] = '\0';

        char val[THEME_VAL_LEN];
        size_t vi = 0;
        if (*q == '"' || *q == '\'') {
            char quote = *q++;
            while (*q && *q != quote && vi < THEME_VAL_LEN - 1) val[vi++] = *q++;
            if (*q == quote) q++;
        } else {
            while (*q && *q != '#' && vi < THEME_VAL_LEN - 1) val[vi++] = *q++;
            while (vi > 0 && (val[vi-1] == ' ' || val[vi-1] == '\t' ||
                              val[vi-1] == '\r')) vi--;
        }
        val[vi] = '\0';
        theme_put(t, key, val);
        p = q;
    }
}

int theme_load(theme_t *t, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        theme_parse_line(t, line);
    }
    fclose(f);
    return 0;
}

const char *theme_str(const theme_t *t, const char *key, const char *fb)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->pairs[i].key, key) == 0) return t->pairs[i].val;
    return fb;
}

/* A theme is not only the file the build wrote: the shell prefers HER
 * copy of shell.conf, and anything she runs can write that. So the two
 * number readers return numbers every consumer can do arithmetic on.
 *
 * theme_int saturates instead of truncating: (int)strtol("9999999999")
 * is an implementation-defined wrap to some unrelated value, and
 * LONG_MAX narrowed to int is -1.
 *
 * theme_num refuses nan and inf -- strtod accepts both spellings, and no
 * theme value means either. A NaN opacity reached draw.c and became an
 * undefined float-to-int conversion in the compositor's own process
 * (found by tools/fuzz/fuzz_theme). The fallback is what an absent key
 * gives, which is the honest reading of a value that is not a number. */
int theme_int(const theme_t *t, const char *key, int fb)
{
    const char *v = theme_str(t, key, NULL);
    if (!v || !*v) return fb;
    long n = strtol(v, NULL, 0);
    if (n > INT_MAX) return INT_MAX;
    if (n < INT_MIN) return INT_MIN;
    return (int)n;
}

double theme_num(const theme_t *t, const char *key, double fb)
{
    const char *v = theme_str(t, key, NULL);
    if (!v || !*v) return fb;
    double d = strtod(v, NULL);
    return isfinite(d) ? d : fb;
}

uint32_t theme_color(const theme_t *t, const char *key, uint32_t fb)
{
    const char *v = theme_str(t, key, NULL);
    if (!v || !*v) return fb;
    if (v[0] == '#') v++;
    else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;

    char *end = NULL;
    unsigned long c = strtoul(v, &end, 16);
    if (end == v) return fb;
    return (uint32_t)(c & 0xFFFFFFu);
}
