#!/usr/bin/python3
"""selftest — prove every othersys check and refusal fails when it should.

No VM, about a minute, runs anywhere with sgdisk, sfdisk, mkfs.vfat,
mtools, openssl and OVMF's variable store:

  * checks.py: each check passes on a good fixture and FAILS on a broken
    one (a flipped byte in Windows, a changed type GUID, a lost attribute
    bit, a deleted partition, Ubuntu first in BootOrder, Windows dropped
    from it, a queued MOK request, the ISO partition left behind, a
    MokManager-looking screen).
  * plan.py: refuses a layout that would let Ubuntu's installer delete or
    retype a Windows partition (a partition with no role, no type, an
    overlap, a wrong type or name), and its answer file lists every
    partition with preserve + type + GUID and formats only Ubuntu's two;
    grub.cfg carries nopersistent and live-media (without them casper
    would add a partition to her disk).
  * othersys.py against a stand-in efivarfs: the late fix-up undoes what
    curtin does (Ubuntu first), deletes only AurOS's own restart entry,
    arms BootNext, cancels a queued MOK request; the hold puts Windows
    back first if something promoted Ubuntu; "yes" puts Ubuntu first and
    keeps everything; "no" clears BootNext; nothing ever leaves BootOrder.
  * each of the above, sabotaged, is seen to fail.
"""
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools'))
import checks  # noqa: E402
import efivarstore  # noqa: E402

PLAN = os.path.join(REPO, 'src/othersys/ubuntu/plan.py')
SYSTEM = os.path.join(REPO, 'src/othersys/systems/ubuntu-26.04.1-desktop.json')
OVMF_VARS = '/usr/share/OVMF/OVMF_VARS_4M.ms.fd'
MiB = 1 << 20
fails = 0
count = 0


def expect(name, failures, should_fail):
    global fails, count
    count += 1
    got_fail = bool(failures) if isinstance(failures, list) else bool(failures)
    ok = got_fail == should_fail
    if not ok:
        fails += 1
    print('%s  %s%s' % ('ok  ' if ok else 'FAIL', name,
                        (' -> ' + '; '.join(failures)[:160]) if isinstance(failures, list) and failures else ''))


def sh(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def load_othersys():
    spec = importlib.util.spec_from_file_location('othersys', os.path.join(REPO, 'src/othersys/ubuntu/othersys.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# ------------------------------------------------------------ fixtures

def small_disk(d):
    """A 96 MiB disk with the Windows 11 shape and AurOS's three partitions."""
    disk = os.path.join(d, 'disk.raw')
    with open(disk, 'wb') as f:
        f.truncate(96 * MiB)
    g = {n: str(uuid.uuid4()) for n in range(1, 8)}
    spec = [(1, 2048, 8 * MiB, 'EF00', 'EFI system partition'),
            (2, None, 2 * MiB, '0C01', 'Microsoft reserved partition'),
            (3, None, 30 * MiB, '0700', 'Basic data partition'),
            (5, None, 8 * MiB, 'EF00', 'AUROS-OS-ESP'),
            (6, None, 20 * MiB, '8300', 'AUROS-OS-ROOT'),
            (7, None, 8 * MiB, '8300', 'AUROS-OS-ISO')]
    cmd = ['sgdisk', '-o']
    at = 2048
    for n, start, size, t, name in spec:
        start = start or at
        end = start + size // 512 - 1
        cmd += ['-n', '%d:%d:%d' % (n, start, end), '-t', '%d:%s' % (n, t), '-u', '%d:%s' % (n, g[n]),
                '-c', '%d:%s' % (n, name)]
        at = end + 1
    rec_start = (96 * MiB // 512 - 34 - 10 * MiB // 512) // 2048 * 2048
    cmd += ['-n', '4:%d:%d' % (rec_start, rec_start + 10 * MiB // 512 - 1), '-t', '4:2700',
            '-u', '4:%s' % g[4], '-A', '4:set:0', '-A', '4:set:63', disk]
    sh(*cmd)
    with open(disk, 'r+b') as f:
        for n in (1, 2, 3, 4):
            pass
    tab, parts = checks.gpt(disk)
    with open(disk, 'r+b') as f:
        for n in (1, 2, 3, 4):
            f.seek(parts[n]['start'] * 512)
            f.write(os.urandom(256 * 1024))
    roles = {1: 'keep', 2: 'keep', 3: 'keep', 4: 'keep', 5: 'target-esp', 6: 'target-root', 7: 'iso'}
    layout = {'disk_guid': tab['id'].lower(), 'sector_size': 512, 'last_lba': tab['lastlba'],
              'partitions': [{'number': n, 'first_lba': q['start'], 'sectors': q['size'],
                              'type': q['type'], 'guid': q['uuid'].lower(), 'name': q.get('name', ''),
                              'role': roles[n]} for n, q in sorted(parts.items())]}
    return disk, layout


def test_checks(d):
    print('== checks.py')
    disk, layout = small_disk(d)
    before = checks.snapshot(disk, layout)
    expect('Windows intact: unchanged disk passes', checks.check_windows_intact(before, checks.snapshot(disk, layout)), False)
    tab, parts = checks.gpt(disk)
    # a byte inside C:
    work = os.path.join(d, 'flip.raw')
    shutil.copyfile(disk, work)
    with open(work, 'r+b') as f:
        f.seek(parts[3]['start'] * 512 + 4096)
        b = f.read(1)
        f.seek(-1, 1)
        f.write(bytes([b[0] ^ 1]))
    expect('Windows intact: one flipped bit in C: fails', checks.check_windows_intact(before, checks.snapshot(work, layout)), True)
    shutil.copyfile(disk, work)
    with open(work, 'r+b') as f:
        f.seek(parts[2]['start'] * 512 + 1000 * 1024)
        f.write(b'\x01')
    expect('Windows intact: a byte in the MSR fails', checks.check_windows_intact(before, checks.snapshot(work, layout)), True)
    shutil.copyfile(disk, work)
    sh('sgdisk', '-t', '3:8300', work)
    expect('Windows intact: C: retyped (curtin without partition_type) fails',
           checks.check_windows_intact(before, checks.snapshot(work, layout)), True)
    shutil.copyfile(disk, work)
    sh('sgdisk', '-A', '4:clear:63', work)
    expect('Windows intact: Recovery lost its no-drive-letter bit fails',
           checks.check_windows_intact(before, checks.snapshot(work, layout)), True)
    shutil.copyfile(disk, work)
    sh('sgdisk', '-d', '2', work)
    expect('Windows intact: MSR deleted (an unlisted partition) fails',
           checks.check_windows_intact(before, checks.snapshot(work, layout)), True)
    shutil.copyfile(disk, work)
    sh('sgdisk', '-c', '1:EFI', work)
    expect('Windows intact: ESP renamed fails', checks.check_windows_intact(before, checks.snapshot(work, layout)), True)
    plan = {'iso_partuuid': [p for p in layout['partitions'] if p['role'] == 'iso'][0]['guid'], 'root_number': 6}
    expect('ISO gone: the ISO partition still there fails', checks.check_iso_gone(disk, plan), True)
    shutil.copyfile(disk, work)
    sh('sgdisk', '-d', '7', work)
    expect('ISO gone: deleted passes', checks.check_iso_gone(work, plan), False)
    expect('root grew: unchanged root fails', checks.check_root_grew(disk, plan, parts[6]['size']), True)

    # firmware
    v = os.path.join(d, 'vars.fd')
    shutil.copyfile(OVMF_VARS, v)
    ev = [sys.executable, '-I', os.path.join(REPO, 'tools/efivarstore.py'), v]
    sh(*ev, 'plant-hd', 'Boot0002', 'Windows Boot Manager', '\\EFI\\Microsoft\\Boot\\bootmgfw.efi', disk, '1')
    sh(*ev, 'plant-hd', 'Boot0004', 'ubuntu', '\\EFI\\ubuntu\\shimx64.efi', disk, '5')
    esp = [p for p in layout['partitions'] if p['role'] == 'target-esp'][0]['guid']
    sh(*ev, 'set-order', '0002', '0000', '0001', '0004')
    vs = efivarstore.read_store(v)
    expect('Windows first: 0002 first passes', checks.check_windows_first(vs, '0002'), False)
    expect('Ubuntu entry: found on its own ESP', checks.check_ubuntu_entry(vs, esp), False)
    expect('Ubuntu entry: on the wrong partition fails', checks.check_ubuntu_entry(vs, str(uuid.uuid4())), True)
    sh(*ev, 'set-order', '0004', '0002', '0000', '0001')
    vs = efivarstore.read_store(v)
    expect('Windows first: Ubuntu first (what curtin leaves) fails', checks.check_windows_first(vs, '0002'), True)
    expect('Ubuntu first, Windows kept: passes', checks.check_ubuntu_first_windows_kept(vs, '0004', '0002'), False)
    sh(*ev, 'set-order', '0004', '0000', '0001')
    vs = efivarstore.read_store(v)
    expect('Ubuntu first, Windows kept: Windows dropped fails',
           checks.check_ubuntu_first_windows_kept(vs, '0004', '0002'), True)
    expect('BootNext: absent fails', checks.check_bootnext(vs, '0004'), True)
    sh(*ev, 'set-next', '0004')
    expect('BootNext: 0004 passes', checks.check_bootnext(efivarstore.read_store(v), '0004'), False)
    expect('no MOK: clean store passes', checks.check_no_mok(efivarstore.read_store(v)), False)
    efivarstore.plant(v, 'MokNew', checks.SHIM, os.urandom(40))
    expect('no MOK: a queued MokNew fails', checks.check_no_mok(efivarstore.read_store(v)), True)
    sh(*ev, 'plant-hd', 'Boot0003', 'AurOS Installer', '\\EFI\\AurOS\\shimx64.efi', disk, '5')
    expect('restart entry gone: still there fails',
           checks.check_entry_gone(efivarstore.read_store(v), '0003', 'AurOS Installer'), True)
    expect('marker: absent fails', checks.check_marker('nothing here', 'OTHERSYS-TEST SESSION-UP'), True)

    # the screen: a picture that looks like MokManager's first page
    try:
        from PIL import Image, ImageDraw, ImageFont
        im = Image.new('RGB', (1024, 768), (0, 0, 170))
        dr = ImageDraw.Draw(im)
        try:
            font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 28)
        except OSError:
            font = ImageFont.load_default()
        dr.text((200, 300), 'Perform MOK management', fill=(255, 255, 255), font=font)
        p = os.path.join(d, 'mok.png')
        im.save(p)
        expect('not MokManager: a MokManager-looking screen fails', checks.check_not_mokmanager(p), True)
        im = Image.new('RGB', (1024, 768), (40, 40, 40))
        ImageDraw.Draw(im).text((200, 300), 'Activities  Files  Firefox', fill=(255, 255, 255), font=font)
        p = os.path.join(d, 'desk.png')
        im.save(p)
        expect('not MokManager: a desktop passes', checks.check_not_mokmanager(p), False)
    except ImportError:
        print('skip  screen checks (no PIL)')
    return disk, layout


# --------------------------------------------------------------- plan

def run_plan(d, layout, extra=()):
    lp = os.path.join(d, 'layout.json')
    json.dump(layout, open(lp, 'w'))
    bp = os.path.join(d, 'boot.json')
    json.dump({'bootorder_before': ['0002', '0000', '0001'], 'handoff_entry': '0003',
               'handoff_desc': 'AurOS Installer'}, open(bp, 'w'))
    cp = os.path.join(d, 'choices.json')
    json.dump({'locale': 'en_US.UTF-8', 'keyboard': {'layout': 'us'}, 'timezone': 'Europe/Amsterdam',
               'username': 'grandma', 'realname': 'Grandma', 'hostname': 'grandmas-pc'}, open(cp, 'w'))
    efi = os.path.join(d, 'efi')
    os.makedirs(efi, exist_ok=True)
    for f in ('bootx64.efi', 'grubx64.efi', 'mmx64.efi'):
        open(os.path.join(efi, f), 'wb').write(b'MZ stand-in')
    sysp = os.path.join(d, 'system.json')
    s = json.load(open(SYSTEM))
    s.update({'min_root_bytes': 16 * MiB, 'esp_bytes': 8 * MiB, 'iso_bytes': 4 * MiB})
    json.dump(s, open(sysp, 'w'))
    r = subprocess.run([sys.executable, '-I', PLAN, '--system', sysp, '--layout', lp, '--boot', bp,
                        '--choices', cp, '--efi-dir', efi, '--out', os.path.join(d, 'seed')] + list(extra),
                       capture_output=True, text=True)
    return r


def lint_seed(seed, layout):
    """The answer file's fence, read back from what plan.py wrote."""
    bad = []
    ai = json.loads(open(os.path.join(seed, 'user-data')).read().split('\n', 1)[1])['autoinstall']
    cfg = ai['storage']['config']
    parts = {a['number']: a for a in cfg if a['type'] == 'partition'}
    for p in layout['partitions']:
        a = parts.get(p['number'])
        if a is None:
            bad.append('partition %d is not in the answer file (curtin would delete it)' % p['number'])
            continue
        if not a.get('preserve'):
            bad.append('partition %d is not preserved' % p['number'])
        if (a.get('partition_type') or '').upper() != p['type'].upper():
            bad.append('partition %d has no/wrong partition_type' % p['number'])
        if (a.get('uuid') or '').lower() != p['guid'].lower():
            bad.append('partition %d has no/wrong uuid' % p['number'])
        if a.get('offset') != p['first_lba'] * 512 or a.get('size') != p['sectors'] * 512:
            bad.append('partition %d offset/size differ' % p['number'])
    fmts = [a for a in cfg if a['type'] == 'format']
    targets = {'part%d' % p['number'] for p in layout['partitions'] if p['role'] in ('target-esp', 'target-root')}
    for f in fmts:
        if f['volume'] not in targets:
            bad.append('%s would be formatted' % f['volume'])
    if len(fmts) != 2:
        bad.append('%d partitions formatted, expected 2' % len(fmts))
    grub = open(os.path.join(seed, 'boot/grub/grub.cfg')).read()
    for word in ('nopersistent', 'live-media=/dev/disk/by-partuuid/', 'autoinstall', 'ds=nocloud'):
        if word not in grub:
            bad.append('grub.cfg lacks %s' % word)
    if not any('othersys.py guard' in c for c in ai.get('early-commands', [])):
        bad.append('early-commands do not start the guard (a power cut would start half an Ubuntu)')
    if not any('othersys.py error' in c for c in ai.get('error-commands', [])):
        bad.append('error-commands do not put Windows first again')
    if not ai.get('identity', {}).get('password', '').startswith('$6$'):
        bad.append('the password is not hashed')
    return bad, ai


def test_plan(d, layout):
    print('== plan.py')
    r = run_plan(d, layout)
    expect('plan: a good layout is accepted', [] if r.returncode == 0 else [r.stderr.strip()], False)
    if r.returncode:
        return
    seed = os.path.join(d, 'seed')
    bad, ai = lint_seed(seed, layout)
    expect('plan: the answer file keeps the fence', bad, False)
    # sabotage the written answer file, and the lint must see it
    ud = os.path.join(seed, 'user-data')
    good = open(ud).read()
    doc = json.loads(good.split('\n', 1)[1])
    doc['autoinstall']['storage']['config'] = [a for a in doc['autoinstall']['storage']['config']
                                               if not (a['type'] == 'partition' and a['number'] == 2)]
    open(ud, 'w').write('#cloud-config\n' + json.dumps(doc))
    expect('lint: an answer file without the MSR fails', lint_seed(seed, layout)[0], True)
    doc = json.loads(good.split('\n', 1)[1])
    for a in doc['autoinstall']['storage']['config']:
        if a.get('number') == 3:
            del a['partition_type']
    open(ud, 'w').write('#cloud-config\n' + json.dumps(doc))
    expect('lint: an answer file without C:\'s type fails', lint_seed(seed, layout)[0], True)
    doc = json.loads(good.split('\n', 1)[1])
    doc['autoinstall']['storage']['config'].append({'type': 'format', 'id': 'x', 'volume': 'part3', 'fstype': 'ext4'})
    open(ud, 'w').write('#cloud-config\n' + json.dumps(doc))
    expect('lint: an answer file formatting C: fails', lint_seed(seed, layout)[0], True)
    doc = json.loads(good.split('\n', 1)[1])
    doc['autoinstall']['early-commands'] = [c for c in doc['autoinstall']['early-commands'] if 'guard' not in c]
    open(ud, 'w').write('#cloud-config\n' + json.dumps(doc))
    expect('lint: an answer file without the guard fails', lint_seed(seed, layout)[0], True)
    doc = json.loads(good.split('\n', 1)[1])
    doc['autoinstall'].pop('error-commands')
    open(ud, 'w').write('#cloud-config\n' + json.dumps(doc))
    expect('lint: an answer file without the error handler fails', lint_seed(seed, layout)[0], True)
    open(ud, 'w').write(good)
    g = os.path.join(seed, 'boot/grub/grub.cfg')
    gt = open(g).read()
    open(g, 'w').write(gt.replace(' nopersistent', ''))
    expect('lint: grub.cfg without nopersistent fails', lint_seed(seed, layout)[0], True)
    open(g, 'w').write(gt)
    sums = open(os.path.join(seed, 'othersys/SHA256SUMS')).read()
    expect('plan: SHA256SUMS covers the helper', [] if 'othersys.py' in sums and 'plan.json' in sums else ['missing'], False)

    def refused(name, mutate):
        lay = json.loads(json.dumps(layout))
        mutate(lay)
        r = run_plan(d, lay)
        expect('plan refuses: ' + name, ['exit %d %s' % (r.returncode, r.stderr.strip())]
               if r.returncode == 2 else [], True)
        # and prove the refusal is a refusal (exit 2), not a crash
        if r.returncode not in (0, 2):
            print('      (crashed instead: %s)' % r.stderr.strip()[-200:])

    refused('a partition with no role', lambda l: l['partitions'][1].pop('role'))
    refused('a partition with no type GUID', lambda l: l['partitions'][2].__setitem__('type', ''))
    refused('a Windows partition overlapping Ubuntu\'s', lambda l: [p.__setitem__('sectors', p['sectors'] + 4096)
                                                                for p in l['partitions'] if p['number'] == 3])
    refused('the ISO partition typed Basic data (Windows would mount it)',
            lambda l: [p.__setitem__('type', 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')
                       for p in l['partitions'] if p['role'] == 'iso'])
    refused('two root partitions', lambda l: [p.__setitem__('role', 'target-root')
                                              for p in l['partitions'] if p['role'] == 'iso'])
    refused('Ubuntu\'s partition not named by AurOS', lambda l: [p.__setitem__('name', 'Basic data partition')
                                                                 for p in l['partitions'] if p['role'] == 'target-root'])
    # and the unsabotaged layout still passes (the mutations above were copies)
    r = run_plan(d, layout)
    expect('plan: the good layout is still accepted', [] if r.returncode == 0 else [r.stderr], False)


# ----------------------------------------------------------- othersys.py

class FakeEfivars:
    GLOBAL = '8be4df61-93ca-11d2-aa0d-00e098032b8c'

    def __init__(self, d):
        self.d = os.path.join(d, 'efivars')
        if os.path.exists(self.d):
            shutil.rmtree(self.d)
        os.makedirs(self.d)

    def put(self, name, data, guid=GLOBAL):
        open(os.path.join(self.d, '%s-%s' % (name, guid)), 'wb').write(struct.pack('<I', 7) + data)

    def get(self, name, guid=GLOBAL):
        try:
            return open(os.path.join(self.d, '%s-%s' % (name, guid)), 'rb').read()[4:]
        except FileNotFoundError:
            return None

    def order(self):
        d = self.get('BootOrder')
        return ['%04X' % x for x in struct.unpack('<%dH' % (len(d) // 2), d)] if d else []


def hd_option(desc, path, partuuid, number=5):
    return efivarstore.make_hd_load_option(desc, path, number, 2048, 4096, uuid.UUID(partuuid).bytes_le)


def test_helper(d):
    print('== othersys.py (stand-in efivarfs)')
    fv = FakeEfivars(d)
    os.environ['OTHERSYS_TEST_EFIVARS'] = fv.d
    m = load_othersys()
    esp = str(uuid.uuid4())
    win_esp = str(uuid.uuid4())
    plan = {'system': 'x', 'esp_partuuid': esp, 'bootorder_before': ['0002', '0000', '0001'],
            'handoff_entry': '0003', 'handoff_desc': 'AurOS Installer', 'test': {}}

    def machine_after_curtin():
        fv.__init__(d)
        fv.put('Boot0000', efivarstore.make_load_option('UiApp', '\\x'))
        fv.put('Boot0001', efivarstore.make_load_option('EFI Shell', '\\y'))
        fv.put('Boot0002', hd_option('Windows Boot Manager', '\\EFI\\Microsoft\\Boot\\bootmgfw.efi', win_esp, 1))
        fv.put('Boot0003', hd_option('AurOS Installer', '\\EFI\\AurOS\\shimx64.efi', esp))
        fv.put('Boot0004', hd_option('ubuntu', '\\EFI\\ubuntu\\shimx64.efi', esp))
        fv.put('Boot0005', b'\x01\x00\x00\x00garbage')     # unreadable: must be kept
        # what grub-install leaves: ubuntu first, everything else after
        fv.put('BootOrder', struct.pack('<6H', 4, 2, 0, 1, 5, 3))

    machine_after_curtin()
    state = {}
    m.firmware_fixup(plan, state)
    o = fv.order()
    expect('late: Windows first again', [] if o[0] == '0002' else ['order %s' % o], False)
    expect('late: nothing but the restart entry left BootOrder',
           [] if set(o) == {'0002', '0000', '0001', '0004', '0005'} else ['order %s' % o], False)
    expect('late: the restart entry is deleted', [] if fv.get('Boot0003') is None else ['still there'], False)
    expect('late: BootNext = Ubuntu', [] if fv.get('BootNext') == b'\x04\x00' else [repr(fv.get('BootNext'))], False)
    expect('late: Ubuntu right after what Windows recorded', [] if o[:4] == ['0002', '0000', '0001', '0004'] else [str(o)], False)
    # sabotage: the test switch that skips the fix-up leaves Ubuntu first, and the order check sees it
    machine_after_curtin()
    m.firmware_fixup(dict(plan, test={'skip_fixup': True}), {})
    expect('late, fix-up skipped: Ubuntu stays first (check would fail)', [] if fv.order()[0] == '0002' else ['ubuntu first'], True)
    # someone else's entry with the restart entry's number is not deleted
    machine_after_curtin()
    fv.put('Boot0003', hd_option('Some vendor tool', '\\EFI\\vendor\\tool.efi', win_esp, 1))
    m.firmware_fixup(plan, {})
    expect('late: a foreign Boot0003 is left alone', [] if fv.get('Boot0003') else ['deleted'], False)
    # MOK
    fv.put('MokNew', b'x' * 40, m.SHIM)
    fv.put('MokAuth', b'y' * 32, m.SHIM)
    st = {}
    m.cancel_mok(st)
    expect('late: queued MOK requests cancelled', [] if not m.mok_pending() else m.mok_pending(), False)
    expect('late: and recorded', [] if st['mok_pending_at_install'] == ['MokNew', 'MokAuth'] else [str(st)], False)
    # the guard, during the install: the moment curtin puts Ubuntu first
    machine_after_curtin()
    new = m.windows_first_again(plan, 'guard')
    o = fv.order()
    expect('guard: Windows first the moment the installer puts Ubuntu first',
           [] if o and o[0] == '0002' else [str(o)], False)
    expect('guard: nothing left BootOrder', [] if set(o) == {'0002', '0000', '0001', '0004', '0005', '0003'} else [str(o)], False)
    expect('guard: no BootNext armed (a power cut now starts Windows)', [] if fv.get('BootNext') is None else ['armed'], False)
    expect('guard: Windows already first: no write', [] if m.windows_first_again(plan, 'guard') is None else ['wrote'], False)
    m.firmware_fixup(plan, {})
    o = fv.order()
    expect('guard then late: Windows first, BootNext Ubuntu, restart entry gone',
           [] if o[0] == '0002' and fv.get('BootNext') == b'\x04\x00' and fv.get('Boot0003') is None else [str(o)], False)
    machine_after_curtin()
    m.cmd_guard(dict(plan, test={'skip_fixup': True}))
    expect('guard, TEST skip_fixup: does nothing (so nofixup still proves the check)',
           [] if fv.order()[0] == '0004' else [str(fv.order())], False)
    # the error handler: after curtin's write, and after late armed BootNext
    for when, prep in (('after the installer\'s firmware write', lambda: None),
                       ('after late armed BootNext', lambda: m.firmware_fixup(plan, {}))):
        machine_after_curtin()
        prep()
        m.cmd_error(dict(plan, system='ubuntu-26.04.1-desktop'))
        o = fv.order()
        a = fv.get(m.ANSWER_VAR, m.AUROS_GUID)
        expect('error %s: Windows first, no BootNext, "failed" recorded' % when,
               [] if o[0] == '0002' and fv.get('BootNext') is None and a and a.startswith(b'answer=failed\n')
               else ['order %s BootNext %r answer %r' % (o, fv.get('BootNext'), a)], False)
    # the hold
    machine_after_curtin()
    m.firmware_fixup(plan, {})
    fv.put('BootOrder', struct.pack('<5H', 4, 2, 0, 1, 5))     # an update re-promoted Ubuntu
    m.var_del('BootNext')
    m.hold(plan, {'ubuntu_entry': '0004'})
    o = fv.order()
    expect('hold: Windows first again after an update promoted Ubuntu', [] if o[0] == '0002' else [str(o)], False)
    expect('hold: BootNext re-armed', [] if fv.get('BootNext') == b'\x04\x00' else ['not armed'], False)
    expect('hold: nothing removed', [] if set(o) == {'0002', '0000', '0001', '0004', '0005'} else [str(o)], False)
    # yes
    st = {'ubuntu_entry': '0004'}
    m.confirm(st)
    o = fv.order()
    expect('yes: Ubuntu first', [] if o[0] == '0004' else [str(o)], False)
    expect('yes: Windows and everything else kept', [] if set(o) == {'0002', '0000', '0001', '0004', '0005'} else [str(o)], False)
    a = fv.get(m.ANSWER_VAR, m.AUROS_GUID)
    expect('yes: recorded for Windows', [] if a and a.startswith(b'answer=yes\n') else [repr(a)], False)
    # no
    st = {'ubuntu_entry': '0004'}
    m.decline(st)
    expect('no: BootNext cleared', [] if fv.get('BootNext') is None else ['still armed'], False)
    a = fv.get(m.ANSWER_VAR, m.AUROS_GUID)
    expect('no: recorded for Windows', [] if a and a.startswith(b'answer=no\n') else [repr(a)], False)
    # the vmtest check reads the same thing from a firmware store, and must
    # fail when it is missing or says the other answer
    v = os.path.join(d, 'vars-answer.fd')
    shutil.copyfile(OVMF_VARS, v)
    expect('answer check: absent fails', checks.check_answer_recorded(efivarstore.read_store(v), 'yes'), True)
    efivarstore.plant(v, m.ANSWER_VAR, m.AUROS_GUID, a)
    expect('answer check: "no" read as "no" passes', checks.check_answer_recorded(efivarstore.read_store(v), 'no'), False)
    expect('answer check: "no" when "yes" expected fails',
           checks.check_answer_recorded(efivarstore.read_store(v), 'yes'), True)
    # answers: only the three words
    os.makedirs(os.path.join(d, 'inbox'), exist_ok=True)
    m.INBOX = os.path.join(d, 'inbox')
    machine_after_curtin()
    m.firmware_fixup(plan, {})
    for word, first in (('rm -rf /', '0002'), ('YES\n', '0004')):
        open(os.path.join(m.INBOX, 'answer'), 'w').write(word)
        st = {'ubuntu_entry': '0004'}
        m.take_answer(st)
        expect('answer %r -> %s first' % (word.strip(), first), [] if fv.order()[0] == first else [str(fv.order())], False)
    # restored_order never drops a number, for any order it is given
    import itertools
    bad = []
    for cur in itertools.permutations([0, 1, 2, 4, 5]):
        new = m.restored_order([2, 0, 1], list(cur), 4)
        if set(new) != set(cur) or new[0] != 2:
            bad.append('%s -> %s' % (cur, new))
    expect('restored_order: Windows first, nothing dropped, all 120 orders', bad, False)
    expect('restored_order: sabotaged (drops the last) is caught',
           [] if set(m.restored_order([2], [4, 2, 9], 4)[:-1]) == {4, 2, 9} else ['dropped'], True)
    # compare_layout
    lay = {'disk_guid': 'aa', 'sector_size': 512,
           'partitions': [{'number': 1, 'first_lba': 2048, 'sectors': 100, 'type': 'T', 'guid': 'g1'}]}
    good = {'label': 'gpt', 'id': 'AA', 'sectorsize': 512,
            'partitions': [{'node': '/dev/x1', 'start': 2048, 'size': 100, 'type': 't', 'uuid': 'G1'}]}
    expect('early: the recorded layout matches', m.compare_layout(lay, good), False)
    moved = json.loads(json.dumps(good))
    moved['partitions'][0]['start'] = 4096
    expect('early: a moved partition is refused', m.compare_layout(lay, moved), True)
    extra = json.loads(json.dumps(good))
    extra['partitions'].append({'node': '/dev/x2', 'start': 9000, 'size': 10, 'type': 't', 'uuid': 'g2'})
    expect('early: a partition Windows did not see is refused', m.compare_layout(lay, extra), True)
    other = dict(good, id='BB')
    expect('early: another disk is refused', m.compare_layout(lay, other), True)
    # autologin, for each login screen a system can have
    for system, dm, paths in (
            ('ubuntu-26.04.1-desktop', 'gdm3', ['etc/gdm3/custom.conf']),
            ('xubuntu-26.04.1-desktop', 'lightdm', ['etc/lightdm/lightdm.conf.d/50-auros-othersys.conf']),
            ('ubuntu-budgie-26.04.1-desktop', 'sddm', ['etc/sddm.conf.d/50-auros-othersys.conf'])):
        t = os.path.join(d, 'target-' + dm)
        shutil.rmtree(t, ignore_errors=True)
        if dm == 'gdm3':
            os.makedirs(os.path.join(t, 'etc/gdm3'))
            open(os.path.join(t, 'etc/gdm3/custom.conf'), 'w').write('[daemon]\n#AutomaticLoginEnable = true\n')
        elif dm == 'lightdm':
            os.makedirs(os.path.join(t, 'etc/lightdm'))
        else:
            os.makedirs(os.path.join(t, 'usr/bin'))
            open(os.path.join(t, 'usr/bin/sddm'), 'w').close()
            os.makedirs(os.path.join(t, 'usr/share/wayland-sessions'))
            for n in ('budgie-desktop', 'aaa-other'):
                open(os.path.join(t, 'usr/share/wayland-sessions/%s.desktop' % n), 'w').close()
        got = m.autologin(t, {'user': 'grandma', 'system': system})
        text = ''.join(open(os.path.join(t, p)).read() for p in paths if os.path.exists(os.path.join(t, p)))
        bad = [] if 'grandma' in text and got else ['configured %s: %r' % (got, text)]
        if dm == 'gdm3' and 'AutomaticLoginEnable=true' not in text:
            bad.append('gdm: not enabled')
        if dm == 'sddm' and 'Session=budgie-desktop' not in text:
            bad.append('sddm: wrong session in %r' % text)
        expect('autologin with %s (%s)' % (dm, system.split('-2')[0]), bad, False)
    expect('autologin: no login screen is reported, not guessed',
           [] if m.autologin(os.path.join(d, 'nothing'), {'user': 'grandma', 'system': 'x'}) == [] else ['guessed'], False)
    del os.environ['OTHERSYS_TEST_EFIVARS']


def test_verify(d):
    print('== verify.py (the publisher\'s signature)')
    spec = importlib.util.spec_from_file_location('verify', os.path.join(REPO, 'src/othersys/verify.py'))
    v = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(v)
    system = json.load(open(SYSTEM))
    key = os.path.join(REPO, 'src/othersys', system['signing_key'])
    fx = os.path.join(HERE, 'fixtures')
    sums, sig = os.path.join(fx, 'ubuntu-26.04-SHA256SUMS'), os.path.join(fx, 'ubuntu-26.04-SHA256SUMS.gpg')
    expect('verify: Canonical\'s signed SHA256SUMS passes', v.check_sums(system, sums, sig, key), False)
    bad = os.path.join(d, 'sums')
    open(bad, 'w').write(open(sums).read().replace('601e30fb', '601e30fc'))
    expect('verify: one changed hex digit fails', v.check_sums(system, bad, sig, key), True)
    expect('verify: a different pinned key fails',
           v.check_sums(dict(system, signing_fingerprint='0' * 40), sums, sig, key), True)
    expect('verify: a different pinned ISO hash fails',
           v.check_sums(dict(system, iso_sha256='0' * 64), sums, sig, key), True)
    iso = os.path.join(d, 'fake.iso')
    open(iso, 'wb').write(b'not an iso')
    expect('verify: a file that is not the ISO fails', v.check_iso(system, iso), True)


def main():
    d = tempfile.mkdtemp(prefix='othersys-selftest-')
    try:
        disk, layout = test_checks(d)
        test_plan(d, layout)
        test_helper(d)
        test_verify(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('\n%d checked, %d not as expected' % (count, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
