#!/usr/bin/python3
"""checks — what the othersys VM test asserts, as functions.

Every check returns a list of failures (empty = passed), so that
selftest.py can hand each one a broken input and watch it fail; a check
that has never been seen to fail has not been shown to check anything
(docs/handoff/TRAPS.md).

They read the evidence the way an outsider would: the disk image's own
bytes and GPT (not anything the installed system says about itself), and
the OVMF variable store file through tools/efivarstore.py (which has its
own self-test against edk2's bytes).
"""
import hashlib
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import efivarstore  # noqa: E402

SHIM = '605dab50-e046-4300-abb6-3dd810dd8b23'
MOK_REQUESTS = ('MokNew', 'MokAuth', 'MokDel', 'MokDelAuth', 'MokXNew', 'MokXAuth',
                'MokXDel', 'MokXDelAuth', 'MokPW', 'MokSB', 'MokDB', 'MokDBState', 'MokTML')


# ------------------------------------------------------------- the disk

def gpt(disk):
    tab = json.loads(subprocess.run(['sfdisk', '--json', disk], check=True,
                                    capture_output=True, text=True).stdout)['partitiontable']
    parts = {}
    for q in tab.get('partitions', []):
        parts[int(q['node'][len(disk):])] = q
    return tab, parts


def hash_range(path, offset, length):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        f.seek(offset)
        left = length
        while left:
            b = f.read(min(left, 16 << 20))
            if not b:
                break
            h.update(b)
            left -= len(b)
    return h.hexdigest()


def snapshot(disk, layout):
    """Everything Windows owns: each kept partition's bytes and its GPT
    entry (start, size, type, GUID, name, attribute bits), the disk GUID."""
    tab, parts = gpt(disk)
    ss = tab.get('sectorsize', 512)
    out = {'disk_guid': tab['id'].lower(), 'partitions': {}}
    for p in layout['partitions']:
        if p['role'] != 'keep':
            continue
        n = p['number']
        q = parts.get(n)
        if q is None:
            out['partitions'][str(n)] = None
            continue
        out['partitions'][str(n)] = {
            'start': q['start'], 'size': q['size'], 'type': q['type'].upper(),
            'uuid': q['uuid'].lower(), 'name': q.get('name', ''), 'attrs': q.get('attrs', ''),
            'sha256': hash_range(disk, q['start'] * ss, q['size'] * ss)}
    return out


def check_windows_intact(before, after):
    bad = []
    if before['disk_guid'] != after['disk_guid']:
        bad.append('disk GUID changed: %s -> %s' % (before['disk_guid'], after['disk_guid']))
    for n, b in sorted(before['partitions'].items()):
        a = after['partitions'].get(n)
        if a is None:
            bad.append('partition %s is gone' % n)
            continue
        for k in ('start', 'size', 'type', 'uuid', 'name', 'attrs', 'sha256'):
            if b[k] != a[k]:
                bad.append('partition %s %s changed: %s -> %s' % (n, k, b[k], a[k]))
    return bad


def check_iso_gone(disk, plan):
    """The temporary ISO partition is gone, and nothing with its GUID is left."""
    _tab, parts = gpt(disk)
    bad = []
    for n, q in parts.items():
        if q['uuid'].lower() == plan['iso_partuuid']:
            bad.append('partition %d is still the ISO partition (%s)' % (n, q.get('name')))
    return bad


def check_root_grew(disk, plan, before_sectors):
    _tab, parts = gpt(disk)
    q = parts.get(plan['root_number'])
    if q is None:
        return ['the Ubuntu partition is gone']
    if q['size'] <= before_sectors:
        return ['the Ubuntu partition did not get the ISO\'s space (%d sectors)' % q['size']]
    return []


# --------------------------------------------------------- the firmware

def live(vars_fd):
    return efivarstore.read_store(vars_fd)


def var(vars_, name, guid=efivarstore.GLOBAL):
    hits = [d for n, g, _a, d in vars_ if n == name and g == guid]
    return hits[-1] if hits else None


def order(vars_):
    d = var(vars_, 'BootOrder')
    return ['%04X' % x for x in struct.unpack('<%dH' % (len(d) // 2), d)] if d else []


def bootnext(vars_):
    d = var(vars_, 'BootNext')
    return '%04X' % struct.unpack('<H', d[:2])[0] if d and len(d) >= 2 else None


def entries(vars_):
    out = {}
    for n, g, _a, d in vars_:
        if g == efivarstore.GLOBAL and n.startswith('Boot') and len(n) == 8:
            try:
                int(n[4:], 16)
            except ValueError:
                continue
            out[n[4:].upper()] = efivarstore.load_option(d)
    return out


def ubuntu_entry(vars_, esp_partuuid):
    hits = [k for k, lo in entries(vars_).items()
            if lo and lo['part_guid'] == esp_partuuid and lo['path']
            and lo['path'].lower().endswith('\\efi\\ubuntu\\shimx64.efi')]
    return sorted(hits)


def check_ubuntu_entry(vars_, esp_partuuid):
    hits = ubuntu_entry(vars_, esp_partuuid)
    if len(hits) != 1:
        return ['expected one entry for \\EFI\\ubuntu\\shimx64.efi on Ubuntu\'s own EFI '
                'partition, found %s' % (hits or 'none')]
    return []


def check_windows_first(vars_, windows):
    o = order(vars_)
    if not o:
        return ['there is no BootOrder']
    if o[0] != windows:
        lo = entries(vars_).get(o[0])
        return ['BootOrder starts with %s (%s), not Windows Boot Manager %s; order %s'
                % (o[0], lo and lo['description'], windows, ','.join(o))]
    return []


def check_ubuntu_first_windows_kept(vars_, ubuntu, windows):
    o = order(vars_)
    bad = []
    if not o or o[0] != ubuntu:
        bad.append('BootOrder does not start with Ubuntu %s: %s' % (ubuntu, ','.join(o)))
    if windows not in o:
        bad.append('Windows Boot Manager %s is not in BootOrder any more: %s' % (windows, ','.join(o)))
    if windows not in entries(vars_):
        bad.append('Boot%s (Windows Boot Manager) is gone' % windows)
    return bad


def check_bootnext(vars_, want):
    got = bootnext(vars_)
    if got != want:
        return ['BootNext is %s, expected %s' % (got, want)]
    return []


def check_entry_gone(vars_, num, desc):
    lo = entries(vars_).get(num)
    if lo and lo['description'] == desc:
        return ['Boot%s "%s" is still there' % (num, desc)]
    return []


AUROS = 'b89b3dc4-fc68-4749-a5e5-f3eb5f51d569'


def check_answer_recorded(vars_, want):
    """The variable the Windows side reads to learn what she answered."""
    d = var(vars_, 'AurOSOtherSysAnswer', AUROS)
    if d is None:
        return ['AurOSOtherSysAnswer is not in the firmware: Windows cannot learn she said %s' % want]
    if not d.startswith(('answer=%s\n' % want).encode()):
        return ['AurOSOtherSysAnswer says %r, expected answer=%s' % (d[:40], want)]
    return []


def check_no_mok(vars_):
    pending = [n for n in MOK_REQUESTS if var(vars_, n, SHIM) is not None]
    if pending:
        return ['MokManager has work queued for the next start: %s' % ', '.join(pending)]
    return []


# ------------------------------------------------------------ the screen

def screen_text(png):
    r = subprocess.run(['tesseract', png, '-', '--psm', '3'], capture_output=True, text=True)
    return r.stdout


def check_not_mokmanager(png, text=None):
    """MokManager's screen is shim's blue firmware-looking one; its first
    page says "Perform MOK management". Looked for by text, and by colour."""
    bad = []
    text = screen_text(png) if text is None else text
    t = ' '.join(text.split()).lower()
    for phrase in ('perform mok management', 'enroll mok', 'mok management', 'shim uefi key management'):
        if phrase in t:
            bad.append('the screen says "%s" (MokManager)' % phrase)
            break
    try:
        from PIL import Image
        im = Image.open(png).convert('RGB').resize((160, 100))
        px = list(im.getdata())
        blue = sum(1 for r, g, b in px if b > 120 and r < 60 and g < 60)
        if blue > 0.5 * len(px):
            bad.append('%d%% of the screen is MokManager blue' % (100 * blue // len(px)))
    except ImportError:
        pass
    return bad


def check_marker(serial_text, marker):
    if marker not in serial_text:
        return ['"%s" never appeared on the serial port' % marker]
    return []


if __name__ == '__main__':
    what = sys.argv[1]
    if what == 'snapshot':
        print(json.dumps(snapshot(sys.argv[2], json.load(open(sys.argv[3]))), indent=1))
    elif what == 'screen':
        r = check_not_mokmanager(sys.argv[2])
        print('\n'.join(r) or 'ok')
        sys.exit(1 if r else 0)
    else:
        sys.exit('unknown check %s' % what)
