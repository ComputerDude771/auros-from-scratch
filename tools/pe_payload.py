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

    tools/pe_payload.py list     EXE
    tools/pe_payload.py get      EXE ID OUTFILE
    tools/pe_payload.py manifest EXE            print the RT_MANIFEST
    tools/pe_payload.py check    EXE-or-XML     is it a manifest Windows loads?

`check` exists because the first real Windows PC refused to start the
wizard: "its side-by-side configuration is incorrect". Its manifest had
`--` inside an XML comment, which XML forbids and Windows' manifest
parser enforces. Wine loaded it anyway, so every test passed. A manifest
Windows cannot parse means the program cannot start, before any of its
code runs, so the check is strict: a well-formed document, one
<assembly>, and the requireAdministrator the wizard depends on.
"""
import struct, sys

RT_RCDATA = 10
RT_MANIFEST = 24


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
        return self.resources(RT_RCDATA)

    def resources(self, rtype):
        """→ {id: (offset, length)} for every resource of one type."""
        if not self.res_rva:
            return {}
        root = self.off(self.res_rva)
        found = {}
        for tid, sub in self._entries(root):
            if tid & 0x80000000 or tid != rtype:
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


def check_manifest(data):
    """→ None if Windows can load this manifest, else why not."""
    import xml.dom.minidom
    import xml.parsers.expat
    try:
        doc = xml.dom.minidom.parseString(data)
    except xml.parsers.expat.ExpatError as e:
        return 'not well-formed XML: %s' % e
    root = doc.documentElement
    if root.localName != 'assembly' or \
            root.namespaceURI != 'urn:schemas-microsoft-com:asm.v1':
        return 'the root element is not <assembly> in asm.v1'
    levels = [e.getAttribute('level') for e in
              doc.getElementsByTagName('requestedExecutionLevel')]
    if levels != ['requireAdministrator']:
        return 'requestedExecutionLevel is %r, not requireAdministrator' % levels
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    what, path = sys.argv[1], sys.argv[2]
    if what == 'check':
        data = open(path, 'rb').read()
        if data[:2] == b'MZ':
            man = PE(path).resources(RT_MANIFEST)
            if list(man) != [1]:
                raise SystemExit('%s: expected one manifest, id 1; found %s'
                                 % (path, sorted(man) or 'none'))
            off, ln = man[1]
            data = PE(path).b[off:off + ln]
        why = check_manifest(data)
        if why:
            raise SystemExit('%s: Windows would refuse to start this: %s'
                             % (path, why))
        print('%s: manifest ok' % path)
        return
    pe = PE(path)
    res = pe.rcdata()
    if what == 'list':
        for rid in sorted(res):
            print('%d %d' % (rid, res[rid][1]))
        return
    if what == 'manifest':
        man = pe.resources(RT_MANIFEST)
        if 1 not in man:
            raise SystemExit(1)
        off, ln = man[1]
        sys.stdout.buffer.write(pe.b[off:off + ln])
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
