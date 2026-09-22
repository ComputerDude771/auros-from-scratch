/* sha256.h — SHA-256, because the journal carries one and a field
 * nothing checks is a field that lies.
 *
 * AurBridge records a hash of the partition table it looked at. The
 * start-and-length check in journal.c catches a Windows partition
 * that has moved or been resized; this catches everything else about
 * the table -- a partition added, one removed, a type changed, a
 * recovery tool having rewritten the whole thing -- all of which
 * change where stage C is allowed to write and none of which the
 * narrower check can see.
 *
 * It is here rather than pulled in because the staging initramfs has
 * no OpenSSL in it and is not going to acquire one for this: the
 * whole image is 12 MB against an OEM ESP that is often 100 MB and
 * nearly full, and this is ninety lines.
 */
#ifndef AUROS_SHA256_H
#define AUROS_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    size_t   n;
    unsigned char buf[64];
} sha256;

void sha256_start(sha256 *s);
void sha256_feed(sha256 *s, const void *data, size_t n);
/* 32 raw bytes. */
void sha256_done(sha256 *s, unsigned char out[32]);
/* 64 lowercase hex characters and a terminator: `out` needs 65. */
void sha256_hex(const unsigned char digest[32], char *out, size_t n);

#endif
