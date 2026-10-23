#!/usr/bin/env python3
"""Draw src/aurbridge/auros.ico: the AurOS mark (the wizard's teal
diamond) on the installer's dark tile, at every size Windows asks for.

SmartScreen's "Run anyway", the administrator question and her Downloads
folder all show this file's icon and name. Without one they showed the
blank "unknown program" icon, which is the picture of a file nobody
should open. Drawn here, not in an editor, so it can be made again and
reviewed as code:  python3 src/aurbridge/auros-icon.py src/aurbridge/auros.ico
No libraries: plain struct and zlib."""
import struct, sys, zlib

TILE = (0x16, 0x1C, 0x28)
EDGE = (0x3A, 0x45, 0x56)
MARK = (0x7D, 0xD3, 0xC0)

def cover(s, x, y):
    """(alpha, rgb) of the pixel centre (x, y) on an s-pixel icon, 4x4 supersampled."""
    acc = [0.0, 0.0, 0.0, 0.0]
    n = 4
    for j in range(n):
        for i in range(n):
            px = (x + (i + 0.5) / n) / s
            py = (y + (j + 0.5) / n) / s
            # rounded tile, inset a little
            m, r = 0.03, 0.22
            dx = max(abs(px - 0.5) - (0.5 - m - r), 0.0)
            dy = max(abs(py - 0.5) - (0.5 - m - r), 0.0)
            d = (dx * dx + dy * dy) ** 0.5
            if d > r:
                continue
            col = EDGE if d > r - max(1.2 / s, 0.025) else TILE
            if abs(px - 0.5) + abs(py - 0.5) <= 0.29:
                col = MARK
            acc[0] += 1
            acc[1] += col[0]; acc[2] += col[1]; acc[3] += col[2]
    a = acc[0] / (n * n)
    if acc[0] == 0:
        return 0, (0, 0, 0)
    return a, tuple(int(c / acc[0] + 0.5) for c in acc[1:])

def bgra_rows(s):
    rows = []
    for y in range(s):
        row = bytearray()
        for x in range(s):
            a, (r, g, b) = cover(s, x, y)
            row += bytes((b, g, r, int(a * 255 + 0.5)))
        rows.append(bytes(row))
    return rows

def dib(s):
    rows = bgra_rows(s)
    hdr = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pix = b''.join(reversed(rows))                     # bottom-up
    mask_row = ((s + 31) // 32) * 4
    return hdr + pix + b'\0' * (mask_row * s)          # alpha does the masking

def png(s):
    rows = bgra_rows(s)
    out = bytearray()
    for r in rows:
        out += b'\0'
        for i in range(0, len(r), 4):
            out += bytes((r[i + 2], r[i + 1], r[i], r[i + 3]))
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', s, s, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(out), 9)) + chunk(b'IEND', b''))

def ico(sizes):
    blobs = [png(s) if s >= 256 else dib(s) for s in sizes]
    head = struct.pack('<HHH', 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    dirs = b''
    for s, b in zip(sizes, blobs):
        dirs += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(b), off)
        off += len(b)
    return head + dirs + b''.join(blobs)

if __name__ == '__main__':
    open(sys.argv[1] if len(sys.argv) > 1 else 'auros.ico', 'wb').write(
        ico([16, 20, 24, 32, 40, 48, 64, 256]))
