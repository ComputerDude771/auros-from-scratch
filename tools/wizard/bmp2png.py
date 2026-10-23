#!/usr/bin/env python3
"""Convert 32bpp top-down (or bottom-up) BMPs to PNG. usage: bmp2png.py in.bmp... (writes .png beside, or into --out DIR)"""
import struct, zlib, sys, os

def conv(src, dst):
    d = open(src, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    assert bpp == 32, bpp
    top = h < 0; h = abs(h)
    rows = []
    for y in range(h):
        sy = y if top else h - 1 - y
        row = d[off + sy * w * 4: off + (sy + 1) * w * 4]
        out = bytearray(w * 3)
        out[0::3] = row[2::4]; out[1::3] = row[1::4]; out[2::3] = row[0::4]
        rows.append(b'\x00' + bytes(out))
    raw = b''.join(rows)
    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) \
        + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)

args = sys.argv[1:]
outdir = None
if args and args[0] == '--out':
    outdir = args[1]; args = args[2:]
for a in args:
    base = os.path.splitext(os.path.basename(a))[0] + '.png'
    dst = os.path.join(outdir or os.path.dirname(a), base)
    conv(a, dst)
    print(dst)
