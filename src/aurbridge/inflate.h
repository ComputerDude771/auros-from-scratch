/* inflate.h — gzip, read one way, for the image that arrives in pieces.
 *
 * WHY THE INSTALLER CARRIES A DECOMPRESSOR AT ALL. The desktop image is
 * five gigabytes and about a gigabyte and a half compressed, and the
 * people this product is for pay for the difference in hours. Where it
 * is published -- a branch of a git repository, as numbered pieces under
 * a hundred megabytes each -- serves each piece as it is, so the
 * compression has to be undone on the machine.
 *
 * WHY GZIP. It is the format every tool that builds this image already
 * has, its decoder is small enough to read in one sitting, and a decoder
 * bug here cannot become a bad install: the output is hashed as it is
 * written and compared against the SHA-256 the build baked into this
 * program, and gzip's own CRC-32 and length are checked on top. A
 * mistake in this file is a refusal before the restart, never a disk.
 *
 * PULL, NOT PUSH. The decoder asks for input when it needs it, so the
 * caller can hand it one piece after another without the decoder
 * knowing pieces exist, and without the state machine a push decoder
 * needs to stop in the middle of a Huffman code. */
#ifndef AURBRIDGE_INFLATE_H
#define AURBRIDGE_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Fill `buf` with up to `n` bytes. Set *got to how many; 0 means the
 * input has ended. Return non-zero to stop with an error (the callback
 * writes its own sentence into the `why` it was given via `ud`). */
typedef int (*gz_reader)(void *ud, uint8_t *buf, size_t n, size_t *got);

/* Take `n` bytes of output. Non-zero stops the decode. */
typedef int (*gz_writer)(void *ud, const uint8_t *buf, size_t n);

/* Decode ONE gzip member from `rd` to `wr`. Returns 0 when the member
 * ended cleanly and its CRC-32 and length both matched; *out_bytes is
 * how much was written. Anything after the member is not read. */
int gz_inflate(gz_reader rd, void *rud, gz_writer wr, void *wud,
               uint64_t *out_bytes, char *why, size_t n);

/* Known-answer checks against members built by hand: stored, fixed and
 * dynamic Huffman blocks, a back-reference across the window boundary,
 * a damaged CRC. 0 when every one passes. */
int gz_selftest(void);

#endif
