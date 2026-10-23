#!/usr/bin/python3
"""mkdisk — a disk as a Windows PC leaves it after AurOS's Windows side.

    mkdisk.py --iso ISO --variant ntfs|bitlocker --out DIR [--test-hooks DIR]
              [--skip-fixup] [--disk-gib 48]

Writes DIR/disk.raw (sparse), DIR/vars.fd (OVMF, Microsoft's keys, Secure
Boot on, with the firmware state Windows would leave), and the records
Windows would have made (layout.json, boot.json, choices.json), the seed
partition's contents (DIR/seed/), the password she would be shown
(DIR/password.txt) and a snapshot of everything Windows owns (before.json).

The layout is the one Windows 11 ships with, after AurOS shrank C: and
filled the freed space: the gap sits between C: and the Recovery
partition, so the new partitions are numbered 5-7 but lie before 4.

  1  EFI system partition   260 MiB  FAT32. \\EFI\\Microsoft\\Boot\\bootmgfw.efi
                                     is a STAND-IN: a signed loader that
                                     prints WINDOWS-STAND-IN-BOOTED and
                                     switches off, so "Windows starts" is
                                     something a test can see.
  2  Microsoft reserved      16 MiB  a random pattern in its first MiB
  3  Windows                 12 GiB  ntfs: NTFS with known files;
                                     bitlocker: random bytes with the
                                     -FVE-FS- signature (nothing may read it)
  5  AUROS-OS-ESP           512 MiB  FAT32 "CIDATA": the seed (plan.py)
  6  AUROS-OS-ROOT           28 GiB  empty
  7  AUROS-OS-ISO       ISO + 1 MiB  the official ISO, byte for byte
  4  Recovery               750 MiB  NTFS, attributes 0 and 63 set

Firmware: Boot0002 "Windows Boot Manager" -> partition 1, BootOrder
0002,0000,0001, Boot0003 "AurOS Installer" -> partition 5's shim, and
BootNext 0003: what AurOS's Windows side arms before the one restart.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import checks  # noqa: E402

MiB = 1 << 20
SYSTEM = os.path.join(REPO, 'src/othersys/systems/ubuntu-26.04.1-desktop.json')
PLAN = os.path.join(REPO, 'src/othersys/ubuntu/plan.py')
EFIVARSTORE = os.path.join(REPO, 'tools/efivarstore.py')
OVMF_VARS = '/usr/share/OVMF/OVMF_VARS_4M.ms.fd'

STANDIN_CFG = '''# TEST STAND-IN for Windows Boot Manager: a Microsoft-signed shim and
# Canonical's signed grub, so that "the firmware started Windows" shows.
serial --unit=0 --speed=115200
terminal_output --append serial
echo "WINDOWS-STAND-IN-BOOTED: Windows Boot Manager would start Windows here."
sleep 8
halt
'''


def sh(*cmd, **kw):
    print('+ ' + ' '.join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def put(disk, offset, src):
    sh('dd', 'if=' + src, 'of=' + disk, 'bs=4M', 'seek=%d' % offset, 'oflag=seek_bytes',
       'conv=notrunc,sparse', 'status=none')


def random_file(path, size):
    with open(path, 'wb') as f:
        left = size
        while left:
            n = min(left, 8 * MiB)
            f.write(os.urandom(n))
            left -= n


def fat(path, size, label, tree):
    """A FAT32 image holding TREE {dest: src or bytes}."""
    with open(path, 'wb') as f:
        f.truncate(size)
    sh('mkfs.vfat', '-F', '32', '-n', label, path, stdout=subprocess.DEVNULL)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    made = set()
    for dest in sorted(tree):
        parts = dest.strip('/').split('/')
        for i in range(1, len(parts)):
            d = '/'.join(parts[:i])
            if d not in made:
                sh('mmd', '-i', path, '::/' + d, env=env)
                made.add(d)
        src = tree[dest]
        if isinstance(src, bytes):
            tmp = path + '.tmp'
            with open(tmp, 'wb') as f:
                f.write(src)
            src = tmp
        sh('mcopy', '-i', path, src, '::/' + dest.strip('/'), env=env)
    if os.path.exists(path + '.tmp'):
        os.unlink(path + '.tmp')


def seed_tree(seed):
    tree = {}
    for d, _dirs, files in os.walk(seed):
        for fn in files:
            p = os.path.join(d, fn)
            tree['/' + os.path.relpath(p, seed)] = p
    return tree


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--variant', choices=('ntfs', 'bitlocker'), default='ntfs')
    ap.add_argument('--out', required=True)
    ap.add_argument('--test-hooks')
    ap.add_argument('--skip-fixup', action='store_true')
    ap.add_argument('--disk-gib', type=int, default=48)
    ap.add_argument('--system', default=SYSTEM, help='a src/othersys/systems/*.json')
    ap.add_argument('--iso-head-mib', type=int, default=0,
                    help='DEVELOPMENT ONLY: write just the ISO\'s first MiB (kernel, initrd, '
                         'directories) to smoke-test the boot chain on a machine without room')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, 'parts')
    os.makedirs(work, exist_ok=True)
    system = json.load(open(a.system))
    iso_bytes = os.path.getsize(a.iso)
    if iso_bytes != system['iso_bytes']:
        sys.exit('the ISO is %d bytes; the manifest says %d' % (iso_bytes, system['iso_bytes']))

    # what the verified ISO gives the Windows side: its three EFI files
    efi = os.path.join(work, 'efi')
    if os.path.exists(efi):
        shutil.rmtree(efi)
    os.makedirs(efi)
    for src in system['efi_files']:
        sh('xorriso', '-osirrox', 'on', '-indev', a.iso, '-extract', src,
           os.path.join(efi, os.path.basename(src)), stderr=subprocess.DEVNULL)

    disk = os.path.join(out, 'disk.raw')
    if os.path.exists(disk):
        os.unlink(disk)
    with open(disk, 'wb') as f:
        f.truncate(a.disk_gib << 30)
    total = (a.disk_gib << 30) // 512
    last_usable = total - 34
    rec_sectors = 750 * MiB // 512
    rec_start = ((last_usable + 1 - rec_sectors) // 2048) * 2048
    iso_mib = (iso_bytes + MiB - 1) // MiB + 1
    g = {n: str(uuid.uuid4()) for n in range(1, 8)}
    dg = str(uuid.uuid4())
    S = 2048
    p = {}
    p[1] = (S, 260 * MiB // 512)
    p[2] = (p[1][0] + p[1][1], 16 * MiB // 512)
    p[3] = (p[2][0] + p[2][1], (12 << 30) // 512)
    p[5] = (p[3][0] + p[3][1], 512 * MiB // 512)
    p[6] = (p[5][0] + p[5][1], (28 << 30) // 512)
    p[7] = (p[6][0] + p[6][1], iso_mib * MiB // 512)
    p[4] = (rec_start, rec_sectors)
    if p[7][0] + p[7][1] > rec_start:
        sys.exit('the disk is too small for this layout')
    types = {1: 'EF00', 2: '0C01', 3: '0700', 4: '2700', 5: 'EF00', 6: '8300', 7: '8300'}
    names = {1: 'EFI system partition', 2: 'Microsoft reserved partition',
             3: 'Basic data partition', 4: '', 5: 'AUROS-OS-ESP', 6: 'AUROS-OS-ROOT',
             7: 'AUROS-OS-ISO'}
    cmd = ['sgdisk', '-o', '-U', dg]
    for n in (1, 2, 3, 5, 6, 7, 4):
        start, size = p[n]
        cmd += ['-n', '%d:%d:%d' % (n, start, start + size - 1), '-t', '%d:%s' % (n, types[n]),
                '-u', '%d:%s' % (n, g[n])]
        if names[n]:
            cmd += ['-c', '%d:%s' % (n, names[n])]
    cmd += ['-A', '4:set:0', '-A', '4:set:63', disk]
    sh(*cmd, stdout=subprocess.DEVNULL)

    # 1: the machine's EFI partition, with a Windows Boot Manager stand-in
    fat(os.path.join(work, 'p1.img'), p[1][1] * 512, 'SYSTEM', {
        '/EFI/Microsoft/Boot/bootmgfw.efi': os.path.join(efi, 'bootx64.efi'),
        '/EFI/Microsoft/Boot/grubx64.efi': os.path.join(efi, 'grubx64.efi'),
        '/EFI/Microsoft/Boot/BCD': os.urandom(64 * 1024),
        '/EFI/Boot/bootx64.efi': os.path.join(efi, 'bootx64.efi'),
        '/EFI/Boot/grubx64.efi': os.path.join(efi, 'grubx64.efi'),
        '/boot/grub/grub.cfg': STANDIN_CFG.encode(),
    })
    put(disk, p[1][0] * 512, os.path.join(work, 'p1.img'))
    # 2: MSR, a pattern anything writing to it would disturb
    random_file(os.path.join(work, 'p2.img'), MiB)
    put(disk, p[2][0] * 512, os.path.join(work, 'p2.img'))
    # 3: Windows
    p3 = os.path.join(work, 'p3.img')
    if a.variant == 'ntfs':
        with open(p3, 'wb') as f:
            f.truncate(p[3][1] * 512)
        sh('mkntfs', '-Q', '-F', '-q', '-L', 'Windows', p3)
        for name, size in (('photos-2019.bin', 48 * MiB), ('letter-to-anna.txt', 4096),
                           ('pagefile.sys', 64 * MiB), ('hiberfil.sys', 32 * MiB)):
            src = os.path.join(work, name)
            random_file(src, size)
            sh('ntfscp', '-q', '-f', p3, src, name)
            os.unlink(src)
        put(disk, p[3][0] * 512, p3)
    else:
        # BitLocker-shaped: the volume header's OEM ID is "-FVE-FS-", and
        # everything after it is ciphertext as far as anyone can tell
        head = bytearray(os.urandom(256 * MiB))
        head[0:3] = b'\xeb\x58\x90'
        head[3:11] = b'-FVE-FS-'
        head[510:512] = b'\x55\xaa'
        with open(p3, 'wb') as f:
            f.write(head)
        put(disk, p[3][0] * 512, p3)
        tail = os.path.join(work, 'p3tail.img')
        random_file(tail, 16 * MiB)
        put(disk, (p[3][0] + p[3][1]) * 512 - 16 * MiB, tail)
        os.unlink(tail)
    os.unlink(p3)
    # 4: Recovery
    p4 = os.path.join(work, 'p4.img')
    with open(p4, 'wb') as f:
        f.truncate(p[4][1] * 512)
    sh('mkntfs', '-Q', '-F', '-q', '-L', 'Recovery', p4)
    src = os.path.join(work, 'Winre.wim')
    random_file(src, 32 * MiB)
    sh('ntfscp', '-q', '-f', p4, src, 'Winre.wim')
    os.unlink(src)
    put(disk, p[4][0] * 512, p4)
    os.unlink(p4)
    # 7: the official ISO, byte for byte
    if a.iso_head_mib:
        ranges = [(0, a.iso_head_mib * MiB)]
        r = subprocess.run(['xorriso', '-indev', a.iso, '-find', '/casper', '-maxdepth', '1',
                            '-type', 'f', '-exec', 'report_lba', '--'],
                           capture_output=True, text=True)
        for line in (r.stdout + r.stderr).splitlines():
            if line.startswith('File data lba:') and ('/casper/vmlinuz' in line or '/casper/initrd' in line):
                f = [x.strip() for x in line.split(':', 1)[1].split(',')]
                ranges.append((int(f[1]) * 2048, int(f[3])))
        with open(a.iso, 'rb') as src, open(disk, 'r+b') as dst:
            for off, ln in ranges:
                src.seek(off)
                dst.seek(p[7][0] * 512 + off)
                dst.write(src.read(ln))
        print('mkdisk: DEVELOPMENT disk: only %s of the ISO written' % ranges)
    else:
        put(disk, p[7][0] * 512, a.iso)

    # what Windows records about the disk (read back, not assumed)
    tab = json.loads(subprocess.run(['sfdisk', '--json', disk], check=True,
                                    capture_output=True, text=True).stdout)['partitiontable']
    roles = {1: 'keep', 2: 'keep', 3: 'keep', 4: 'keep', 5: 'target-esp', 6: 'target-root', 7: 'iso'}
    layout = {'disk_guid': tab['id'].lower(), 'sector_size': tab.get('sectorsize', 512),
              'last_lba': tab['lastlba'], 'partitions': []}
    for q in tab['partitions']:
        n = int(q['node'][len(disk):])
        layout['partitions'].append({'number': n, 'first_lba': q['start'], 'sectors': q['size'],
                                     'type': q['type'], 'guid': q['uuid'].lower(),
                                     'name': q.get('name', ''), 'attrs': q.get('attrs', ''),
                                     'role': roles[n]})
    json.dump(layout, open(os.path.join(out, 'layout.json'), 'w'), indent=1)

    # the firmware as AurOS's Windows side leaves it before the restart
    vars_ = os.path.join(out, 'vars.fd')
    shutil.copyfile(OVMF_VARS, vars_)
    ev = [sys.executable, '-I', EFIVARSTORE, vars_]
    sh(*ev, 'plant-hd', 'Boot0002', 'Windows Boot Manager', '\\EFI\\Microsoft\\Boot\\bootmgfw.efi', disk, '1')
    sh(*ev, 'set-order', '0002', '0000', '0001')
    sh(*ev, 'plant-hd', 'Boot0003', 'AurOS Installer', '\\EFI\\AurOS\\shimx64.efi', disk, '5')
    sh(*ev, 'set-next', '0003')
    boot = {'bootorder_before': ['0002', '0000', '0001'], 'windows_entry': '0002',
            'handoff_entry': '0003', 'handoff_desc': 'AurOS Installer'}
    json.dump(boot, open(os.path.join(out, 'boot.json'), 'w'), indent=1)
    choices = {'locale': 'en_US.UTF-8', 'keyboard': {'layout': 'us', 'variant': ''},
               'timezone': 'Europe/Amsterdam', 'username': 'grandma', 'realname': 'Grandma',
               'hostname': 'grandmas-pc', 'autologin': True}
    json.dump(choices, open(os.path.join(out, 'choices.json'), 'w'), indent=1)

    seed = os.path.join(out, 'seed')
    cmd = [sys.executable, '-I', PLAN, '--system', a.system, '--layout', os.path.join(out, 'layout.json'),
           '--boot', os.path.join(out, 'boot.json'), '--choices', os.path.join(out, 'choices.json'),
           '--efi-dir', efi, '--out', seed, '--password-out', os.path.join(out, 'password.txt')]
    if a.test_hooks:
        cmd += ['--test-hooks', a.test_hooks]
    if a.skip_fixup:
        cmd += ['--test-skip-fixup']
    sh(*cmd)
    fat(os.path.join(work, 'p5.img'), p[5][1] * 512, 'CIDATA', seed_tree(seed))
    put(disk, p[5][0] * 512, os.path.join(work, 'p5.img'))
    os.unlink(os.path.join(work, 'p5.img'))
    os.unlink(os.path.join(work, 'p1.img'))
    os.unlink(os.path.join(work, 'p2.img'))

    before = checks.snapshot(disk, layout)
    json.dump(before, open(os.path.join(out, 'before.json'), 'w'), indent=1)
    print('mkdisk: %s ready (%s); Windows owns %s' % (
        disk, a.variant, ', '.join(str(k) for k in sorted(before['partitions']))))


if __name__ == '__main__':
    main()
