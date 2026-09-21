/* font.h — self-contained anti-aliased TrueType/OpenType rasteriser.
 *
 * The shell needs text before it needs a toolkit, and pulling FreeType
 * into the base system drags in a build-time dependency chain that the
 * ISO has no room for. This renders outlines directly: sfnt parsing,
 * UTF-8, `glyf` quadratics with composite glyphs, `CFF ` cubics,
 * kerning, and an exact-area scanline rasteriser, in one .c file with
 * no dependency beyond libm.
 *
 * It deliberately does NOT do: hinting (the bytecode interpreter is a
 * VM we do not want to expose to untrusted fonts), `CFF2` variable
 * fonts (a different charstring dialect behind the same containers --
 * detected and refused rather than misparsed), GPOS, or shaping. Latin
 * and Cyrillic UI strings at 10-72px are the target; anything needing a
 * shaper is out of scope by design.
 *
 * Every offset inside a font file is attacker-controlled data, so each
 * read is bounds-checked against the file size. A corrupt font makes
 * font_load() return NULL or makes individual glyphs render blank; it
 * never reads out of bounds and never aborts.
 */
#ifndef AUROS_FONT_H
#define AUROS_FONT_H

#include <stdint.h>

typedef struct font font;

/* Load a font file and fix the size at `px` pixels per em. Size is
 * baked in at load because the glyph cache holds rasterised bitmaps;
 * two sizes means two font handles, which is what a UI wants anyway.
 *
 * .ttf, .otf and .ttc all go through this one call and behave
 * identically afterwards: which outline format a file uses is decided
 * from its tables, never from its extension, and never surfaces here.
 * Nothing a caller can ask distinguishes a `glyf` font from a `CFF `
 * one, which is the point -- the theme names a font, not a format.
 *
 * Returns NULL for an unreadable, unsupported or malformed file. */
font *font_load(const char *path, float px);
void  font_free(font *f);

/* Vertical metrics in pixels, from `hhea`. Both ascent and descent are
 * returned as positive distances from the baseline (up and down), which
 * is the sign convention layout code actually wants; the raw table
 * stores descent as a negative number. */
float font_ascent(const font *f);
float font_descent(const font *f);
/* ascent + descent + lineGap: the recommended baseline-to-baseline step. */
float font_line_height(const font *f);

/* Advance width of a UTF-8 string in pixels, kerning included. Not
 * const because it populates the glyph cache. */
float font_text_width(font *f, const char *utf8);

/* Blend text into a 32-bit 0x00RRGGBB framebuffer. (x, y) is the pen
 * origin on the BASELINE, not the top-left of the text box. `color` is
 * 0x00RRGGBB; `alpha` is an extra 0..1 multiplier over glyph coverage.
 * Clips to the framebuffer; out-of-range coordinates are not an error. */
void  font_draw(font *f, uint32_t *px, int w, int h,
                float x, float y, const char *utf8, uint32_t color, float alpha);

#endif
