#!/usr/bin/env python3
"""pe_payload — read what an installer says it carries, out of the PE.

`build/aurbridge` puts the staging kernel and initramfs into the
executable as RCDATA resources, because that is the mechanism
Authenticode covers: a signature is a certificate table at the end of
the file, so anything appended after signing is outside the signature
and anything appended before it moves when the signature lands. A
resource is inside the image the signature is computed over.

Which means "does the installer actually carry it" is a question about
the PE resource directory, and this is what answers it -- in another
language, without asking the program that wrote it.

    tools/pe_payload.py list  EXE
    tools/pe_payload.py get   EXE ID OUTFILE
"""
import struct, sys

RT_RCDATA = 10


def _u16(b, o): return struct.unpack_from('<H', b, o)[0]
def _u32(b, o): return struct.unpack_from('<I', b, o)[0]


class PE:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        if self.b[:2] != b'MZ':
            raise SystemExit('%s is not a Windows program' % path)
        pe = _u32(self.b, 0x3C)
        if self.b[pe:pe + 4] != b'PE\0\0':
            raise SystemExit('%s has no PE header' % path)
        coff = pe + 4
        nsec = _u16(self.b, coff + 2)
        opt_sz = _u16(self.b, coff + 16)
        opt = coff + 20
        magic = _u16(self.b, opt)
        # 0x20b is PE32+, where the data directories start 16 bytes later.
        ddir = opt + (112 if magic == 0x20b else 96)
        self.res_rva = _u32(self.b, ddir + 2 * 8)
        self.sections = []
        s = opt + opt_sz
        for i in range(nsec):
            o = s + i * 40
            name = self.b[o:o + 8].rstrip(b'\0').decode('ascii', 'replace')
            va, vsz = _u32(self.b, o + 12), _u32(self.b, o + 8)
            raw, rptr = _u32(self.b, o + 16), _u32(self.b, o + 20)
            self.sections.append((name, va, vsz, raw, rptr))

    def off(self, rva):
        for _n, va, _vsz, raw, rptr in self.sections:
            if va <= rva < va + max(raw, _vsz):
                return rptr + (rva - va)
        raise SystemExit('rva %#x is in no section' % rva)

    def _entries(self, off):
        n = _u16(self.b, off + 12) + _u16(self.b, off + 14)
        out = []
        for i in range(n):
            e = off + 16 + i * 8
            out.append((_u32(self.b, e), _u32(self.b, e + 4)))
        return out

    def rcdata(self):
        """→ {id: (offset, length)} for every RT_RCDATA resource."""
        if not self.res_rva:
            return {}
        root = self.off(self.res_rva)
        found = {}
        for tid, sub in self._entries(root):
            if tid & 0x80000000 or tid != RT_RCDATA:
                continue
            tdir = root + (sub & 0x7FFFFFFF)
            for rid, rsub in self._entries(tdir):
                if rid & 0x80000000:
                    continue
                ldir = root + (rsub & 0x7FFFFFFF)
                for _lid, ldata in self._entries(ldir):
                    d = root + (ldata & 0x7FFFFFFF)
                    found[rid] = (self.off(_u32(self.b, d)), _u32(self.b, d + 4))
        return found


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    what, path = sys.argv[1], sys.argv[2]
    pe = PE(path)
    res = pe.rcdata()
    if what == 'list':
        for rid in sorted(res):
            print('%d %d' % (rid, res[rid][1]))
        return
    if what == 'get':
        rid = int(sys.argv[3])
        if rid not in res:
            raise SystemExit(1)
        off, ln = res[rid]
        open(sys.argv[4], 'wb').write(pe.b[off:off + ln])
        return
    raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
