#!/usr/bin/env python3
# Build a minimal but structurally valid Windows `regf` hive.
#
# Ferry's own hive reader has to be tested against something, and a real
# NTUSER.DAT cannot live in a source tree (it is somebody's registry).
# This writes the smallest file that exercises every path the reader
# takes: nested keys via an `lf` leaf, ASCII and UTF-16 names, REG_SZ,
# REG_EXPAND_SZ, REG_DWORD, REG_MULTI_SZ, REG_BINARY, and an inline
# (<= 4 byte) value.
#
# Test fixture only. Nothing in Ferry writes hives.
import struct, sys, json

BASE = 0x1000

class Hive:
    def __init__(self):
        self.cells = bytearray()          # everything after the hbin header

    def alloc(self, payload: bytes) -> int:
        size = (len(payload) + 4 + 7) & ~7
        off = len(self.cells) + 32        # cell offsets are past the hbin hdr
        self.cells += struct.pack('<i', -size)
        self.cells += payload
        self.cells += b'\0' * (size - 4 - len(payload))
        return off

def enc_name(name):
    """Return (bytes, ascii_flag). Mirrors what Windows does: Latin-1 when
    it fits, UTF-16LE otherwise."""
    try:
        return name.encode('latin-1'), True
    except UnicodeEncodeError:
        return name.encode('utf-16-le'), False

def build_value(h, name, vtype, data):
    nb, is_ascii = enc_name(name)
    if len(data) <= 4 and len(data) > 0:
        dlen = 0x80000000 | len(data)
        doff = int.from_bytes(data.ljust(4, b'\0'), 'little')
    else:
        dlen = len(data)
        doff = h.alloc(data)
    vk = struct.pack('<2sHIIIHH', b'vk', len(nb), dlen, doff, vtype,
                     1 if is_ascii else 0, 0) + nb
    return h.alloc(vk)

def build_key(h, name, spec):
    values = spec.get('_values', {})
    subs = {k: v for k, v in spec.items() if k != '_values'}

    sub_offs = [(k, build_key(h, k, v)) for k, v in subs.items()]
    val_offs = [build_value(h, n, t, d) for n, (t, d) in values.items()]

    subkey_list = 0xffffffff
    if sub_offs:
        lf = struct.pack('<2sH', b'lf', len(sub_offs))
        for k, off in sub_offs:
            lf += struct.pack('<I4s', off, k[:4].encode('latin-1', 'replace').ljust(4, b'\0'))
        subkey_list = h.alloc(lf)

    value_list = 0xffffffff
    if val_offs:
        value_list = h.alloc(b''.join(struct.pack('<I', o) for o in val_offs))

    nb, is_ascii = enc_name(name)
    nk  = struct.pack('<2sHQ', b'nk', 0x20 if is_ascii else 0x00, 0)
    nk += struct.pack('<I', 0)              # access bits
    nk += struct.pack('<I', 0xffffffff)     # parent
    nk += struct.pack('<II', len(sub_offs), 0)
    nk += struct.pack('<II', subkey_list, 0xffffffff)
    nk += struct.pack('<II', len(val_offs), value_list)
    nk += struct.pack('<I', 0xffffffff)     # security descriptor
    nk += struct.pack('<I', 0xffffffff)     # class name
    nk += struct.pack('<IIIII', 0, 0, 0, 0, 0)   # largest-* + workvar
    nk += struct.pack('<HH', len(nb), 0)
    nk += nb
    return h.alloc(nk)

def write_hive(path, tree):
    h = Hive()
    root = build_key(h, 'ROOT', tree)

    pad = (-len(h.cells)) % (4096 - 32)
    h.cells += b'\0' * pad
    if pad >= 4:                                  # free cell marks the slack
        struct.pack_into('<i', h.cells, len(h.cells) - pad, pad)

    hbin = struct.pack('<4sIIQI', b'hbin', 0, len(h.cells) + 32, 0, 0)
    hbin = hbin.ljust(32, b'\0')

    hdr = bytearray(4096)
    struct.pack_into('<4sIIQIIIII', hdr, 0, b'regf', 1, 1, 0, 1, 3, 0, 1, root)
    struct.pack_into('<I', hdr, 0x28, len(h.cells) + 32)
    struct.pack_into('<I', hdr, 0x2c, 1)
    hdr[0x30:0x30 + 2 * len('ferrytest')] = 'ferrytest'.encode('utf-16-le')
    csum = 0
    for i in range(0, 0x1fc, 4):
        csum ^= struct.unpack_from('<I', hdr, i)[0]
    struct.pack_into('<I', hdr, 0x1fc, csum if csum not in (0, 0xffffffff) else 1)

    with open(path, 'wb') as f:
        f.write(bytes(hdr) + hbin + bytes(h.cells))

def sz(s):   return (1, s.encode('utf-16-le') + b'\0\0')
def exp(s):  return (2, s.encode('utf-16-le') + b'\0\0')
def dw(n):   return (4, struct.pack('<I', n))
def multi(l):return (7, b''.join(x.encode('utf-16-le') + b'\0\0' for x in l) + b'\0\0')
def binary(b): return (3, b if isinstance(b, bytes) else b.encode('latin-1'))

if __name__ == '__main__':
    # spec is JSON: {"Key\\Sub": {"ValueName": ["sz", "text"], ...}}
    out, spec_json = sys.argv[1], sys.argv[2]
    tree = {}
    for keypath, vals in json.loads(spec_json).items():
        node = tree
        for part in keypath.split('\\'):
            node = node.setdefault(part, {})
        v = node.setdefault('_values', {})
        for name, (kind, data) in vals.items():
            v[name] = {'sz': sz, 'exp': exp, 'dword': dw,
                       'multi': multi, 'binary': binary}[kind](data)
    write_hive(out, tree)
