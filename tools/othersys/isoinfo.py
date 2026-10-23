#!/usr/bin/python3
"""isoinfo — what a published ISO says about itself, over HTTP ranges.

    isoinfo.py URL [--get /PATH OUT]...

Reads only the bytes it needs (volume descriptors, a few directories, the
files asked for): the ISO 9660 UUID that grub and blkid compute (from the
volume's modification date), the volume label, casper's install sources,
and any file by path, through the Joliet tree. For pinning a new system's
manifest without downloading gigabytes.
"""
import json
import subprocess
import sys

SECTOR = 2048


class ISO:
    def __init__(self, url):
        self.url = url

    def read(self, off, n):
        r = subprocess.run(['curl', '-fsS', '--retry', '3', '-r', '%d-%d' % (off, off + n - 1), self.url],
                           capture_output=True, check=True)
        if len(r.stdout) != n:
            raise SystemExit('short read at %d' % off)
        return r.stdout

    def descriptors(self):
        out = []
        for lba in range(16, 32):
            d = self.read(lba * SECTOR, SECTOR)
            if d[1:6] != b'CD001':
                break
            out.append(d)
            if d[0] == 255:
                break
        return out

    def root(self):
        ds = self.descriptors()
        pvd = [d for d in ds if d[0] == 1][0]
        jol = [d for d in ds if d[0] == 2 and d[88:91] in (b'%/@', b'%/C', b'%/E')]
        self.pvd = pvd
        src = jol[0] if jol else pvd
        self.joliet = bool(jol)
        rec = src[156:156 + 34]
        return int.from_bytes(rec[2:6], 'little'), int.from_bytes(rec[10:14], 'little')

    def listdir(self, lba, size):
        data = self.read(lba * SECTOR, size)
        at, out = 0, {}
        while at < len(data):
            ln = data[at]
            if ln == 0:
                at = (at // SECTOR + 1) * SECTOR
                continue
            rec = data[at:at + ln]
            nl = rec[32]
            raw = rec[33:33 + nl]
            if raw not in (b'\x00', b'\x01'):
                name = raw.decode('utf-16-be') if self.joliet else raw.decode('ascii')
                name = name.split(';')[0]
                out[name.lower()] = (int.from_bytes(rec[2:6], 'little'),
                                     int.from_bytes(rec[10:14], 'little'), bool(rec[25] & 2))
            at += ln
        return out

    def find(self, path):
        lba, size = self.root()
        for part in [p for p in path.strip('/').split('/') if p]:
            ent = self.listdir(lba, size).get(part.lower())
            if ent is None:
                raise SystemExit('%s: no %s' % (path, part))
            lba, size, _isdir = ent
        return lba, size

    def get(self, path):
        lba, size = self.find(path)
        return self.read(lba * SECTOR, size)

    def info(self):
        self.root()
        p = self.pvd
        d = p[830:846].decode('ascii')
        return {'label': p[40:72].decode('ascii').strip(),
                'fsuuid': '%s-%s-%s-%s-%s-%s-%s' % (d[0:4], d[4:6], d[6:8], d[8:10], d[10:12], d[12:14], d[14:16]),
                'blocks': int.from_bytes(p[80:84], 'little')}


def main():
    iso = ISO(sys.argv[1])
    info = iso.info()
    try:
        info['install_sources'] = iso.get('/casper/install-sources.yaml').decode()
    except SystemExit as e:
        info['install_sources'] = str(e)
    args = sys.argv[2:]
    while args[:1] == ['--get']:
        open(args[2], 'wb').write(iso.get(args[1]))
        args = args[3:]
    print(json.dumps(info, indent=1))


if __name__ == '__main__':
    main()
