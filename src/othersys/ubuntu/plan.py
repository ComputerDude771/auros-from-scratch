#!/usr/bin/python3
"""plan — what AurOS's Windows side writes for an official Ubuntu install.

Given what Windows recorded (the disk's GPT, every partition on it and
which of them AurOS made for Ubuntu; BootOrder before the restart and the
one-shot entry it armed; her language, keyboard, time zone and name),
this writes the contents of the small FAT partition that the restart
starts from (label CIDATA):

  EFI/AurOS/shimx64.efi   Ubuntu's own Microsoft-signed shim, from the ISO
  EFI/AurOS/grubx64.efi   Canonical's signed grub, from the ISO
  EFI/AurOS/mmx64.efi     shim's MokManager, from the ISO (shim wants it)
  boot/grub/grub.cfg      starts the ISO's kernel from the ISO partition
  user-data, meta-data    cloud-init NoCloud: the autoinstall answer file
  othersys/               the helper the answer file runs (othersys.py)

Nothing here is a modified Ubuntu: the ISO is written to its partition
byte for byte as Canonical published it (and verified against their
signed SHA256SUMS first), and the three EFI files are copied out of it
unchanged. What AurOS adds is an answer file and a helper, the documented
way to drive Ubuntu's installer.

THE ANSWER FILE IS THE FENCE. Ubuntu's installer (curtin, storage
config version 2) treats the partition list as the WHOLE disk: an
existing partition that is not listed with `preserve: true` is
superblock-wiped and removed from the table, and a preserved one that is
listed without its type GUID is written back WITHOUT a type (curtin
copies the GUID, name and attribute bits of a preserved partition, but
not its type). So every partition on the disk is listed, each with its
exact offset, size, type and GUID, and curtin re-verifies offset and size
before it writes anything. Only the two partitions AurOS made are
formatted. This program refuses to write a plan that breaks any of that.

Written in Python for the proof; the Windows side (C) must produce the
same bytes. Every output is plain text (the answer file is JSON, which
is YAML), so porting it is substitution, not YAML.
"""
import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
import subprocess
import sys

ESP = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
LINUX = '0FC63DAF-8483-4772-8E79-3D69D8477DE4'
HERE = os.path.dirname(os.path.abspath(__file__))
ROLES = ('keep', 'target-esp', 'target-root', 'iso')
NAMES = {'target-esp': 'AUROS-OS-ESP', 'target-root': 'AUROS-OS-ROOT', 'iso': 'AUROS-OS-ISO'}
WORDS = ('apple maple river candle garden window pebble meadow harbor violet '
         'lantern biscuit willow sparrow orchard button kettle cobalt').split()


class Refuse(Exception):
    pass


def check_layout(layout, system):
    """Every rule the answer file depends on. Raises Refuse."""
    ss = layout['sector_size']
    parts = layout['partitions']
    byrole = {}
    for p in parts:
        r = p.get('role')
        if r not in ROLES:
            raise Refuse('partition %d has no role (%r): every partition on the disk must '
                         'be listed, or Ubuntu\'s installer would delete it' % (p['number'], r))
        for k in ('number', 'first_lba', 'sectors', 'type', 'guid'):
            if p.get(k) in (None, ''):
                raise Refuse('partition %d has no %s: the installer would write it back '
                             'without one' % (p['number'], k))
        byrole.setdefault(r, []).append(p)
    for r in ('target-esp', 'target-root', 'iso'):
        if len(byrole.get(r, [])) != 1:
            raise Refuse('need exactly one %s partition, found %d' % (r, len(byrole.get(r, []))))
    nums = [p['number'] for p in parts]
    if len(set(nums)) != len(nums):
        raise Refuse('two partitions share a number')
    spans = sorted((p['first_lba'], p['first_lba'] + p['sectors'] - 1, p['number']) for p in parts)
    for a, b in zip(spans, spans[1:]):
        if b[0] <= a[1]:
            raise Refuse('partitions %d and %d overlap' % (a[2], b[2]))
    for lo, hi, n in spans:
        if lo < 34 or hi > layout['last_lba']:
            raise Refuse('partition %d runs outside the disk' % n)
    esp, root, iso = byrole['target-esp'][0], byrole['target-root'][0], byrole['iso'][0]
    if esp['type'].upper() != ESP:
        raise Refuse('the partition for Ubuntu\'s EFI files is not typed as an EFI partition')
    for p in (root, iso):
        if p['type'].upper() != LINUX:
            raise Refuse('partition %d (%s) must be typed Linux filesystem, so Windows '
                         'leaves it alone' % (p['number'], p['role']))
    for p in (esp, root, iso):
        if p.get('name') != NAMES[p['role']]:
            raise Refuse('partition %d must be named %s' % (p['number'], NAMES[p['role']]))
    if esp['sectors'] * ss < system['esp_bytes']:
        raise Refuse('the EFI partition for Ubuntu is too small')
    if root['sectors'] * ss < system['min_root_bytes']:
        raise Refuse('the partition for Ubuntu is smaller than %d GiB' % (system['min_root_bytes'] >> 30))
    if iso['sectors'] * ss < system['iso_bytes']:
        raise Refuse('the ISO does not fit its partition')
    return esp, root, iso


def storage_config(layout, esp, root):
    ss = layout['sector_size']
    cfg = [{'type': 'disk', 'id': 'disk0', 'path': '__OTHERSYS_DISK__', 'ptable': 'gpt',
            'preserve': True, 'grub_device': False}]
    for p in sorted(layout['partitions'], key=lambda p: p['number']):
        a = {'type': 'partition', 'id': 'part%d' % p['number'], 'device': 'disk0',
             'number': p['number'], 'offset': p['first_lba'] * ss, 'size': p['sectors'] * ss,
             'partition_type': p['type'].upper(), 'uuid': p['guid'].lower(), 'preserve': True}
        if p.get('name'):
            a['partition_name'] = p['name']
        if p['type'].upper() == ESP:
            a['flag'] = 'boot'          # verified by curtin before anything is written
        if p is esp:
            a['grub_device'] = True     # grub goes here, and only here
            a['wipe'] = 'superblock'
        if p is root:
            a['flag'] = 'linux'
            a['wipe'] = 'superblock'
        cfg.append(a)
    cfg += [
        {'type': 'format', 'id': 'fmt-esp', 'volume': 'part%d' % esp['number'],
         'fstype': 'fat32', 'preserve': False},
        {'type': 'format', 'id': 'fmt-root', 'volume': 'part%d' % root['number'],
         'fstype': 'ext4', 'preserve': False},
        {'type': 'mount', 'id': 'mnt-root', 'device': 'fmt-root', 'path': '/'},
        {'type': 'mount', 'id': 'mnt-esp', 'device': 'fmt-esp', 'path': '/boot/efi'},
    ]
    return cfg


def check_choices(c):
    if not re.match(r'^[a-z][a-z0-9-]{0,31}$', c['username']):
        raise Refuse('user name %r is not a Linux user name' % c['username'])
    if not re.match(r'^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$', c['hostname']):
        raise Refuse('computer name %r is not a host name' % c['hostname'])
    if not re.match(r'^[A-Za-z_]+(\.UTF-8)?$', c['locale']):
        raise Refuse('locale %r' % c['locale'])
    if not re.match(r'^[A-Za-z0-9_+/-]+$', c['timezone']):
        raise Refuse('time zone %r' % c['timezone'])
    if any(ch in c.get('realname', '') for ch in ':\n'):
        raise Refuse('the name has a character a user record cannot hold')


def crypt_sha512(password):
    r = subprocess.run(['openssl', 'passwd', '-6', '-stdin'], input=password,
                       capture_output=True, text=True, check=True)
    h = r.stdout.strip()
    if not h.startswith('$6$'):
        raise RuntimeError('openssl passwd did not give a SHA-512 crypt')
    return h


def grub_cfg(iso, system, test_args):
    return '''# AurOS: start Ubuntu's own installer from the partition AurOS wrote
# the verified ISO to. Loaded by Ubuntu's signed grub (from that ISO),
# which reads /boot/grub/grub.cfg on the partition it was started from
# ($root; this grub leaves $cmdpath empty). Anything unexpected returns
# to the firmware, which starts the next entry in BootOrder: Windows.
set timeout=0
set timeout_style=hidden
if ! regexp --set=1:aurdisk '^([^,]+),' "$root"; then
    echo "AurOS: cannot tell which disk this is. Starting Windows instead."
    sleep 10
    exit 1
fi
set auriso="($aurdisk,gpt{num})"
probe --set=aurisopart --part-uuid $auriso
probe --set=aurisouuid --fs-uuid $auriso
if [ "$aurisopart" != "{partuuid}" -o "$aurisouuid" != "{fsuuid}" ]; then
    echo "AurOS: the Ubuntu installer is not where AurOS put it. Starting Windows instead."
    sleep 10
    exit 1
fi
menuentry "Install Ubuntu" {{
    set gfxpayload=keep
    linux $auriso/casper/vmlinuz autoinstall ds=nocloud nopersistent live-media=/dev/disk/by-partuuid/{partuuid} {test} --- quiet splash
    initrd $auriso/casper/initrd
}}
'''.format(num=iso['number'], fsuuid=system['iso_fsuuid'], partuuid=iso['guid'].lower(),
           test=test_args).replace('  ---', ' ---')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--system', required=True)
    ap.add_argument('--layout', required=True)
    ap.add_argument('--boot', required=True)
    ap.add_argument('--choices', required=True)
    ap.add_argument('--efi-dir', required=True, help='bootx64.efi etc. copied out of the verified ISO')
    ap.add_argument('--out', required=True)
    ap.add_argument('--password-out')
    ap.add_argument('--test-hooks', help='TEST ONLY: a directory of hooks for the VM test')
    ap.add_argument('--test-skip-fixup', action='store_true',
                    help='TEST ONLY: leave the firmware as Ubuntu\'s installer leaves it')
    a = ap.parse_args()

    system = json.load(open(a.system))
    layout = json.load(open(a.layout))
    boot = json.load(open(a.boot))
    choices = json.load(open(a.choices))
    try:
        esp, root, iso = check_layout(layout, system)
        check_choices(choices)
    except Refuse as e:
        print('plan: REFUSED: %s' % e, file=sys.stderr)
        return 2

    password = choices.get('password') or '%s-%s-%02d' % (
        secrets.choice(WORDS), secrets.choice(WORDS), secrets.randbelow(100))
    test = {}
    if a.test_skip_fixup:
        test['skip_fixup'] = True

    out = a.out
    if os.path.exists(out):
        shutil.rmtree(out)
    lib = os.path.join(out, 'othersys')
    os.makedirs(os.path.join(lib, 'units'))
    os.makedirs(os.path.join(out, 'EFI', 'AurOS'))
    os.makedirs(os.path.join(out, 'boot', 'grub'))

    names = {'bootx64.efi': 'shimx64.efi', 'grubx64.efi': 'grubx64.efi', 'mmx64.efi': 'mmx64.efi'}
    for src, dst in names.items():
        shutil.copyfile(os.path.join(a.efi_dir, src), os.path.join(out, 'EFI', 'AurOS', dst))

    plan = {
        'version': 1, 'system': system['id'],
        'disk_guid': layout['disk_guid'].lower(), 'sector_size': layout['sector_size'],
        'partitions': [{k: p[k] for k in ('number', 'first_lba', 'sectors', 'type', 'guid', 'name', 'role')
                        if k in p} for p in layout['partitions']],
        'esp_partuuid': esp['guid'].lower(), 'root_partuuid': root['guid'].lower(),
        'iso_partuuid': iso['guid'].lower(), 'iso_number': iso['number'],
        'root_number': root['number'], 'esp_number': esp['number'],
        'iso_name': iso['name'], 'iso_fsuuid': system['iso_fsuuid'],
        'bootorder_before': boot['bootorder_before'], 'handoff_entry': boot.get('handoff_entry'),
        'handoff_desc': boot.get('handoff_desc', 'AurOS Installer'),
        'user': choices['username'], 'autologin': bool(choices.get('autologin', True)),
        'test': test,
    }
    with open(os.path.join(lib, 'plan.json'), 'w') as f:
        json.dump(plan, f, indent=1)
    shutil.copyfile(os.path.join(HERE, 'othersys.py'), os.path.join(lib, 'othersys.py'))
    shutil.copyfile(os.path.join(HERE, 'ask'), os.path.join(lib, 'ask'))
    for u in sorted(os.listdir(os.path.join(HERE, 'units'))):
        shutil.copyfile(os.path.join(HERE, 'units', u), os.path.join(lib, 'units', u))
    test_args = ''
    if a.test_hooks:
        shutil.copytree(a.test_hooks, os.path.join(lib, 'test'))
        test_args = 'console=ttyS0,115200n8 console=tty1'
    sums = []
    for d, _dirs, files in sorted(os.walk(lib)):
        for fn in sorted(files):
            p = os.path.join(d, fn)
            sums.append('%s  %s' % (sha256(p), os.path.relpath(p, lib)))
    with open(os.path.join(lib, 'SHA256SUMS'), 'w') as f:
        f.write('\n'.join(sums) + '\n')

    seed = '/run/othersys/seed'
    early = [
        'mkdir -p %s /run/othersys/lib && mount -o ro /dev/disk/by-partuuid/%s %s && '
        'cp -r %s/othersys/. /run/othersys/lib/ && umount %s' % (seed, esp['guid'].lower(), seed, seed, seed),
        'cd /run/othersys/lib && sha256sum --quiet -c SHA256SUMS',
        '/usr/bin/python3 /run/othersys/lib/othersys.py early',
        # from here until power-off: Windows stays first whatever the
        # installer writes, so a power cut starts Windows
        'systemd-run --unit=othersys-guard --collect /usr/bin/python3 '
        '/run/othersys/lib/othersys.py guard',
    ]
    late = ['/usr/bin/python3 /run/othersys/lib/othersys.py late']
    errors = ['/usr/bin/python3 /run/othersys/lib/othersys.py error || true']
    if a.test_hooks:
        early.insert(0, 'systemd-run --unit=othersys-serial -p StandardOutput=file:/dev/ttyS0 '
                        'journalctl -f -o short-monotonic --no-pager || true')
        late.append('/bin/sh /run/othersys/lib/test/hooks late /target')
        errors.append('/bin/sh /run/othersys/lib/test/hooks error || true')

    ai = {
        'version': 1,
        'locale': choices['locale'],
        'keyboard': {'layout': choices['keyboard']['layout'],
                     'variant': choices['keyboard'].get('variant', '')},
        'timezone': choices['timezone'],
        'identity': {'realname': choices.get('realname', ''), 'username': choices['username'],
                     'hostname': choices['hostname'], 'password': crypt_sha512(password)},
        'source': {'id': system['source_id'], 'search_drivers': False},
        'apt': {'fallback': 'offline-install', 'geoip': False},
        'codecs': {'install': False},
        'drivers': {'install': False},
        'oem': {'install': False},
        'storage': {'config': storage_config(layout, esp, root)},
        'early-commands': early,
        'late-commands': late,
        'shutdown': 'poweroff',
    }
    ai['error-commands'] = errors
    with open(os.path.join(out, 'user-data'), 'w') as f:
        f.write('#cloud-config\n' + json.dumps({'autoinstall': ai}, indent=1) + '\n')
    with open(os.path.join(out, 'meta-data'), 'w') as f:
        f.write('instance-id: auros-othersys-%s\n' % secrets.token_hex(4))
    with open(os.path.join(out, 'boot', 'grub', 'grub.cfg'), 'w') as f:
        f.write(grub_cfg(iso, system, test_args))
    if a.password_out:
        with open(a.password_out, 'w') as f:
            f.write(password + '\n')
    print(json.dumps({'seed': out, 'esp': esp['number'], 'root': root['number'],
                      'iso': iso['number'], 'user': choices['username']}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
