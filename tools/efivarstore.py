#!/usr/bin/env python3
"""efivarstore — read an OVMF variable store out of a flash file.

The end-to-end tests give QEMU a private copy of OVMF_VARS_4M.fd, so
whatever the installer writes to NVRAM is sitting in that file
afterwards. Nothing else in the tree can read it, and without it the
only thing a test can say about the boot entry is that the program
claimed to have written one.

THE FORMAT, because it is not obvious and getting it slightly wrong
gives an empty answer rather than an error:

  0                EFI_FIRMWARE_VOLUME_HEADER. ZeroVector[16],
                   FileSystemGuid[16], FvLength(8), "_FVH"(4),
                   Attributes(4), HeaderLength(2), ...
  HeaderLength     VARIABLE_STORE_HEADER: Signature GUID(16), Size(4),
                   Format(1), State(1), Reserved(2), Reserved1(4).
                   The signature GUID says which variable header
                   follows: the authenticated one (60 bytes) or the
                   plain one (32).
  +28              the variables, each header followed by its name in
                   UTF-16LE and then its data, the next header aligned
                   up to four bytes.

THE AUTHENTICATED HEADER'S FIELDS ARE NOT WHERE YOU WOULD GUESS, and
this was wrong for a whole afternoon: NameSize is at +36 and DataSize
at +40, BEFORE the vendor GUID at +44, not after it. Reading them
twelve bytes late gives a name that decodes correctly -- the name
starts at +60 either way -- and lengths in the billions, so the walk
finds its first variable, reports a plausible name, and then falls off
the end of the store. A parser that fails LOUDLY would have been
easier; this one has to be checked against a file somebody else wrote,
which is what --self-test does: it parses the OVMF store
Microsoft's keys ship in and asserts what has to be in it.

State is a byte whose bits are CLEARED as a variable moves through its
life. EDK2 accepts 0x3f (added) and 0x3e (added, in deleted
transition); 0x3d and 0x3c are deleted. Variables are APPENDED, never
rewritten in place, so a variable that has been updated appears
several times and the last live copy is the one the firmware uses --
BootOrder is in OVMF_VARS_4M.ms.fd five times.
"""
import struct, sys, uuid

AUTH_STORE = uuid.UUID('aaf32c78-947b-439a-a180-2e144ec37792')
PLAIN_STORE = uuid.UUID('ddcf3616-3275-4164-98b6-fe85707ffe7d')
GLOBAL = '8be4df61-93ca-11d2-aa0d-00e098032b8c'   # EFI_GLOBAL_VARIABLE
VAR_ADDED = 0x3f
VAR_IN_TRANSITION = 0x3e


def _guid(b):
    return uuid.UUID(bytes_le=bytes(b))


def read_store(path):
    """→ list of (name, guid, attributes, data), in store order."""
    blob = open(path, 'rb').read()
    if blob[40:44] != b'_FVH':
        raise SystemExit('not a firmware volume: %s' % path)
    hdr_len = struct.unpack_from('<H', blob, 48)[0]
    sig = _guid(blob[hdr_len:hdr_len + 16])
    if sig == AUTH_STORE:
        fmt, hsz = 'auth', 60
    elif sig == PLAIN_STORE:
        fmt, hsz = 'plain', 32
    else:
        raise SystemExit('unknown variable store signature %s' % sig)
    size = struct.unpack_from('<I', blob, hdr_len + 16)[0]
    end = min(len(blob), hdr_len + size)
    at = hdr_len + 28
    out = []
    while at + hsz <= end:
        start_id = struct.unpack_from('<H', blob, at)[0]
        if start_id != 0x55AA:
            break
        state = blob[at + 2]
        attrs = struct.unpack_from('<I', blob, at + 4)[0]
        if fmt == 'auth':
            name_sz, data_sz = struct.unpack_from('<II', blob, at + 36)
            vguid = _guid(blob[at + 44:at + 60])
        else:
            name_sz, data_sz = struct.unpack_from('<II', blob, at + 8)
            vguid = _guid(blob[at + 16:at + 32])
        if name_sz > 1024 or data_sz > (1 << 20):
            break
        nm = blob[at + hsz:at + hsz + name_sz]
        dt = blob[at + hsz + name_sz:at + hsz + name_sz + data_sz]
        if state in (VAR_ADDED, VAR_IN_TRANSITION):
            name = nm.decode('utf-16-le', 'replace').rstrip('\x00')
            out.append((name, str(vguid), attrs, dt))
        at += hsz + name_sz + data_sz
        at = (at + 3) & ~3
    return out


def load_option(data):
    """Pull apart an EFI_LOAD_OPTION. → dict, or None if malformed."""
    if len(data) < 6:
        return None
    attrs, dp_len = struct.unpack_from('<IH', data, 0)
    i = 6
    desc = []
    while i + 1 < len(data):
        c = struct.unpack_from('<H', data, i)[0]
        i += 2
        if c == 0:
            break
        desc.append(chr(c))
    dp = data[i:i + dp_len]
    out = {'attributes': attrs, 'description': ''.join(desc),
           'part_guid': None, 'part_number': None, 'path': None,
           'nodes': []}
    j = 0
    while j + 4 <= len(dp):
        t, st, ln = dp[j], dp[j + 1], struct.unpack_from('<H', dp, j + 2)[0]
        if ln < 4 or j + ln > len(dp):
            return None
        out['nodes'].append((t, st, ln))
        if t == 0x7F:
            break
        if t == 0x04 and st == 0x01 and ln == 42:
            out['part_number'] = struct.unpack_from('<I', dp, j + 4)[0]
            out['part_first'] = struct.unpack_from('<Q', dp, j + 8)[0]
            out['part_blocks'] = struct.unpack_from('<Q', dp, j + 16)[0]
            out['part_guid'] = str(_guid(dp[j + 24:j + 40]))
        if t == 0x04 and st == 0x04:
            raw = dp[j + 4:j + ln]
            out['path'] = raw.decode('utf-16-le', 'replace').rstrip('\x00')
        j += ln
    return out


def store_offsets(blob):
    """→ (header_length, format, header_size, store_end, first_free)."""
    if blob[40:44] != b'_FVH':
        raise SystemExit('not a firmware volume')
    hdr_len = struct.unpack_from('<H', blob, 48)[0]
    sig = _guid(blob[hdr_len:hdr_len + 16])
    hsz = 60 if sig == AUTH_STORE else 32
    size = struct.unpack_from('<I', blob, hdr_len + 16)[0]
    end = min(len(blob), hdr_len + size)
    at = hdr_len + 28
    while at + hsz <= end:
        if struct.unpack_from('<H', blob, at)[0] != 0x55AA:
            break
        if hsz == 60:
            name_sz, data_sz = struct.unpack_from('<II', blob, at + 36)
        else:
            name_sz, data_sz = struct.unpack_from('<II', blob, at + 8)
        at = (at + hsz + name_sz + data_sz + 3) & ~3
    return hdr_len, hsz, end, at


VAR_DELETED = 0x3c


def retire(blob, name, guid):
    """Mark every live copy of NAME/GUID deleted, in place.

    WITHOUT THIS, PLANTING A VARIABLE THAT ALREADY EXISTS LEAVES TWO
    LIVE COPIES, and which one the firmware believes is a property of
    its variable driver rather than of anything this test controls.
    Planting BootOrder into a store that already had one produced
    exactly that. A variable is updated in EDK2 the same way: the old
    record's State has bits cleared until it reads as deleted, and the
    new one is appended. 0x3c is fully deleted -- both the
    in-transition bit and the deleted bit cleared."""
    hdr_len, hsz, end, _at = store_offsets(blob)
    at = hdr_len + 28
    want_g = uuid.UUID(guid).bytes_le
    n = 0
    while at + hsz <= end:
        if struct.unpack_from('<H', blob, at)[0] != 0x55AA:
            break
        state = blob[at + 2]
        if hsz == 60:
            name_sz, data_sz = struct.unpack_from('<II', blob, at + 36)
            vg = bytes(blob[at + 44:at + 60])
        else:
            name_sz, data_sz = struct.unpack_from('<II', blob, at + 8)
            vg = bytes(blob[at + 16:at + 32])
        if name_sz > 1024 or data_sz > (1 << 20):
            break
        nm = bytes(blob[at + hsz:at + hsz + name_sz])
        nm = nm.decode('utf-16-le', 'replace').rstrip('\x00')
        if state in (VAR_ADDED, VAR_IN_TRANSITION) and nm == name \
                and vg == want_g:
            blob[at + 2] = VAR_DELETED
            n += 1
        at += hsz + name_sz + data_sz
        at = (at + 3) & ~3
    return n


def plant(path, name, guid, data):
    """Put one variable into a store, in place, REPLACING any live copy.

    Only the tests use this: to put a boot entry into NVRAM that the
    Windows half of the product would have put there, and to set the
    machine up in the state an install leaves it -- AurOS in the menu,
    Windows still first, BootNext pointing at AurOS. Variables are
    appended in this format and never rewritten, so adding one is
    writing past the last and retiring whatever it replaces.
    """
    blob = bytearray(open(path, 'rb').read())
    retire(blob, name, guid)
    _hdr_len, hsz, end, at = store_offsets(blob)
    nm = name.encode('utf-16-le') + b'\x00\x00'
    hdr = bytearray(hsz)
    struct.pack_into('<H', hdr, 0, 0x55AA)
    hdr[2] = VAR_ADDED
    hdr[3] = 0xFF
    struct.pack_into('<I', hdr, 4, 0x07)        # NV | BS | RT
    if hsz == 60:
        struct.pack_into('<II', hdr, 36, len(nm), len(data))
        hdr[44:60] = uuid.UUID(guid).bytes_le
    else:
        struct.pack_into('<II', hdr, 8, len(nm), len(data))
        hdr[16:32] = uuid.UUID(guid).bytes_le
    rec = bytes(hdr) + nm + data
    rec += b'\xff' * ((4 - len(rec) % 4) % 4)
    if at + len(rec) > end:
        raise SystemExit('no room left in the variable store')
    blob[at:at + len(rec)] = rec
    open(path, 'wb').write(bytes(blob))


def make_load_option(desc, path):
    """The smallest believable EFI_LOAD_OPTION: a File() node and an
    End node, with no Hard Drive node. Deliberately not the shape the
    product writes -- this stands in for an entry made by something
    else, and the test only ever asks whether it went away."""
    d = desc.encode('utf-16-le') + b'\x00\x00'
    f = path.encode('utf-16-le') + b'\x00\x00'
    file_node = struct.pack('<BBH', 0x04, 0x04, 4 + len(f)) + f
    end_node = struct.pack('<BBH', 0x7F, 0xFF, 4)
    dp = file_node + end_node
    return struct.pack('<IH', 1, len(dp)) + d + dp


def gpt_partition(image, number):
    """(first_lba, blocks, unique_guid_bytes_le) of partition NUMBER
    (1-based) in a GPT disk image, read from the image itself so that
    the boot entry cannot disagree with the disk it points into."""
    f = open(image, 'rb')
    f.seek(512)
    h = f.read(92)
    if h[0:8] != b'EFI PART':
        raise SystemExit('%s has no GPT at LBA 1' % image)
    ent_lba, n_ent, ent_sz = struct.unpack_from('<QII', h, 72)
    if not 1 <= number <= n_ent:
        raise SystemExit('no partition %d' % number)
    f.seek(ent_lba * 512 + (number - 1) * ent_sz)
    e = f.read(ent_sz)
    first, last = struct.unpack_from('<QQ', e, 32)
    if e[0:16] == b'\x00' * 16:
        raise SystemExit('partition %d is unused' % number)
    return first, last - first + 1, bytes(e[16:32])


def make_hd_load_option(desc, path, number, first, blocks, part_guid_le):
    """An EFI_LOAD_OPTION the way the product writes one:
    HD(number, GPT, partition-GUID, start, size) / File(path) / End.

    A bare File() node -- which is all make_load_option builds -- is
    answered by the firmware with EFI_NOT_FOUND, because LoadImage has
    no device to resolve the path against. That was the product's own
    first bug here and it is not repeated in the test that checks it."""
    d = desc.encode('utf-16-le') + b'\x00\x00'
    hd = struct.pack('<BBH', 0x04, 0x01, 42)
    hd += struct.pack('<IQQ', number, first, blocks)
    hd += part_guid_le
    hd += struct.pack('<BB', 0x02, 0x02)     # GPT, GUID signature
    f = path.encode('utf-16-le') + b'\x00\x00'
    file_node = struct.pack('<BBH', 0x04, 0x04, 4 + len(f)) + f
    end_node = struct.pack('<BBH', 0x7F, 0xFF, 4)
    dp = hd + file_node + end_node
    return struct.pack('<IH', 1, len(dp)) + d + dp


def self_test():
    """Parse a store somebody else wrote and assert what is in it.

    A parser checked only against files this program's own siblings
    produced is a parser that agrees with itself. OVMF's shipped
    Microsoft-keyed variable store is written by edk2 and by
    virt-fw-vars, contains the four Secure Boot variables, and holds
    BootOrder several times over -- which is the property that made the
    walk's superseded-entry handling worth having.
    """
    path = '/usr/share/OVMF/OVMF_VARS_4M.ms.fd'
    try:
        vars_ = read_store(path)
    except SystemExit as e:
        print('cannot read %s: %s' % (path, e))
        return 2
    names = [n for n, _g, _a, _d in vars_]
    bad = 0
    for want in ('PK', 'KEK', 'db', 'dbx'):
        if want not in names:
            print('FAIL  %s is not in %s' % (want, path)); bad = 1
    boots = [n for n in names if n.startswith('Boot') and len(n) == 8]
    if not boots:
        print('FAIL  no Boot#### entries found'); bad = 1
    for name, _g, _a, data in vars_:
        if name in boots:
            if load_option(data) is None:
                print('FAIL  %s does not decode as a load option' % name)
                bad = 1
    if not bad:
        print('ok  %d variables, including %s and %d boot entries'
              % (len(vars_), ', '.join(n for n in ('PK', 'KEK', 'db', 'dbx')),
                 len(boots)))
    return bad


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--self-test':
        raise SystemExit(self_test())
    path = sys.argv[1]
    what = sys.argv[2] if len(sys.argv) > 2 else 'list'
    vars_ = read_store(path)
    if what == 'list':
        for name, guid, attrs, data in vars_:
            print('%-24s %s %d bytes' % (name, guid, len(data)))
        return
    if what == 'bootorder':
        live = [d for n, _g, _a, d in vars_ if n == 'BootOrder']
        if live:
            data = live[-1]
            print(' '.join('%04X' % v for v in
                           struct.unpack('<%dH' % (len(data) // 2), data)))
        return
    if what == 'bootnext':
        live = [d for n, _g, _a, d in vars_ if n == 'BootNext' and len(d) == 2]
        if live:
            print('%04X' % struct.unpack('<H', live[-1])[0])
        return
    if what == 'entry':
        want = sys.argv[3]
        for name, _g, _a, data in reversed(vars_):
            if not (name.startswith('Boot') and len(name) == 8):
                continue
            try:
                int(name[4:], 16)
            except ValueError:
                continue
            lo = load_option(data)
            if not lo or lo['description'] != want:
                continue
            print('num=%s' % name[4:])
            print('path=%s' % lo['path'])
            print('part_guid=%s' % lo['part_guid'])
            print('part_number=%s' % lo['part_number'])
            print('part_first=%s' % lo.get('part_first'))
            print('part_blocks=%s' % lo.get('part_blocks'))
            print('nodes=%s' % ','.join('%02x/%02x' % (t, s)
                                        for t, s, _l in lo['nodes']))
            return
        sys.exit(1)
    if what == 'plant':
        # plant VARS.fd Boot0007 "Some Description" \\EFI\\x\\y.efi
        slot, desc, loader = sys.argv[3], sys.argv[4], sys.argv[5]
        plant(path, slot, GLOBAL, make_load_option(desc, loader))
        return
    if what == 'plant-hd':
        # plant-hd VARS.fd Boot0002 "AurOS" \\EFI\\AurOS\\shimx64.efi DISK.img 1
        slot, desc, loader = sys.argv[3], sys.argv[4], sys.argv[5]
        image, number = sys.argv[6], int(sys.argv[7])
        first, blocks, g = gpt_partition(image, number)
        plant(path, slot, GLOBAL,
              make_hd_load_option(desc, loader, number, first, blocks, g))
        return
    if what == 'plant-hd-raw':
        # plant-hd-raw VARS.fd Boot0001 "AurOS" \\EFI\\AurOS\\shimx64.efi \
        #              NUMBER FIRST BLOCKS PART-GUID
        # An entry for a partition that is not on any disk here: what
        # "Put Windows back" leaves in the firmware after it deletes
        # the partition the entry names.
        slot, desc, loader = sys.argv[3], sys.argv[4], sys.argv[5]
        number, first, blocks = (int(sys.argv[6]), int(sys.argv[7]),
                                 int(sys.argv[8]))
        g = uuid.UUID(sys.argv[9]).bytes_le
        plant(path, slot, GLOBAL,
              make_hd_load_option(desc, loader, number, first, blocks, g))
        return
    if what == 'set-order':
        # set-order VARS.fd 0000 0002
        nums = [int(x, 16) for x in sys.argv[3:]]
        plant(path, 'BootOrder', GLOBAL,
              b''.join(struct.pack('<H', n) for n in nums))
        return
    if what == 'set-next':
        # set-next VARS.fd 0002
        plant(path, 'BootNext', GLOBAL, struct.pack('<H', int(sys.argv[3], 16)))
        return
    sys.exit('unknown request %s' % what)


if __name__ == '__main__':
    main()
