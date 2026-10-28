#!/usr/bin/python3
"""othersys — AurOS's part of an official Ubuntu installed beside Windows.

WHAT THIS IS

AurOS can install an official, unmodified Ubuntu beside Windows. AurOS
prepares the disk from Windows (the partitions, the verified ISO, a small
FAT partition with Ubuntu's own boot chain and the answer file) and arms
BootNext; Ubuntu's own installer, running unattended, formats and fills
only the partitions it is named. This program is everything AurOS does
on the Linux side, and it runs in three places:

  early   in the live installer, before anything is written: finds the
          disk by its GPT GUID, compares every partition with what
          Windows recorded, refuses on any difference, and tells the
          installer which device the disk is.
  late    in the live installer, after Ubuntu's installer has finished:
          undoes what Ubuntu's installer did to the firmware (it puts its
          own "ubuntu" entry FIRST in BootOrder, and nothing in autoinstall
          can stop it: curtin calls grub-multi-install with no arguments),
          removes the entry AurOS made for the restart, arms BootNext at
          Ubuntu, cancels any MOK enrolment queued during the install,
          and installs the rest of this program into the new system.
  boot    in the installed Ubuntu, every boot, before the desktop: the
          first time, removes the temporary ISO partition and gives its
          space to Ubuntu; until she has answered "does everything work?",
          keeps Windows first and re-arms BootNext at Ubuntu (the hold);
          and acts on an answer if one is waiting.
  answer  acts on the word the question wrote (yes / no / later).

THE RULES IT KEEPS (from docs/AURBRIDGE.md, unchanged for another OS)

  * BootOrder changes to "Ubuntu first" only after she says yes. Before
    that, Windows stays first and BootNext brings her back to Ubuntu.
  * Nothing is removed from BootOrder, ever. An entry this program cannot
    read is kept where it is. Only AurOS's own one-shot "AurOS Installer"
    entry is deleted, and only when it points at the partition AurOS made.
  * The machine's own EFI partition is never written here. Ubuntu gets an
    EFI partition of its own, made by AurOS in the freed space.
  * A partition is only ever deleted when it is the ISO partition AurOS
    made: same number, same unique GUID, same GPT name, and an ISO 9660
    volume with the UUID recorded from the verified download.

It is a single file with no imports outside the standard library, because
it runs both in the live installer and in the installed system and must
not depend on anything the installer may or may not have put there
(efibootmgr, for one, is not in the installed package list).
"""
import fcntl
import json
import os
import re
import struct
import subprocess
import sys
import time
import uuid

EFIVARS = os.environ.get('OTHERSYS_TEST_EFIVARS', '/sys/firmware/efi/efivars')
GLOBAL = '8be4df61-93ca-11d2-aa0d-00e098032b8c'
SHIM = '605dab50-e046-4300-abb6-3dd810dd8b23'
# Every variable through which shim's MokManager is asked to do something
# at the next start. Any one of them present means a blue firmware-looking
# screen that waits for a key and a password.
# AurOS's own vendor GUID, for the one variable that tells the Windows
# side what she answered (docs/OTHER-SYSTEMS.md, "Put Windows back"):
# AurOSOtherSysAnswer = b"answer=yes|no\nsystem=<id>\n", non-volatile,
# readable at runtime, so GetFirmwareEnvironmentVariable finds it.
AUROS_GUID = 'b89b3dc4-fc68-4749-a5e5-f3eb5f51d569'
ANSWER_VAR = 'AurOSOtherSysAnswer'
MOK_REQUESTS = ('MokNew', 'MokAuth', 'MokDel', 'MokDelAuth', 'MokXNew',
                'MokXAuth', 'MokXDel', 'MokXDelAuth', 'MokPW', 'MokSB',
                'MokDB', 'MokDBState', 'MokTML')
NV_BS_RT = 0x07

LIB = '/usr/lib/auros-othersys'
STATE_DIR = '/var/lib/auros-othersys'
INBOX = STATE_DIR + '/inbox'
LIVE_LIB = '/run/othersys/lib'
LOG_LIVE = '/run/othersys/log'
ANSWERS = ('yes', 'no', 'later')

FS_IOC_GETFLAGS = 0x80086601
FS_IOC_SETFLAGS = 0x40086602
FS_IMMUTABLE_FL = 0x10


class Refuse(Exception):
    """Something is not as Windows left it. Stop before writing."""


def log(msg):
    line = '%s othersys: %s' % (time.strftime('%Y-%m-%dT%H:%M:%S'), msg)
    print(line, flush=True)
    for path in (os.environ.get('OTHERSYS_LOG'),):
        if path:
            try:
                with open(path, 'a') as f:
                    f.write(line + '\n')
            except OSError:
                pass


# ---------------------------------------------------------------- efivarfs

def _var(name, guid):
    return os.path.join(EFIVARS, '%s-%s' % (name, guid))


def var_get(name, guid=GLOBAL):
    try:
        with open(_var(name, guid), 'rb') as f:
            raw = f.read()
    except FileNotFoundError:
        return None
    return raw[4:] if len(raw) >= 4 else None


def _mutable(path):
    """efivarfs makes existing variables immutable so that `rm -rf /`
    cannot brick a machine; writing or deleting one needs that flag off."""
    try:
        fd = os.open(path, os.O_RDONLY)
    except FileNotFoundError:
        return
    try:
        buf = bytearray(8)
        try:
            fcntl.ioctl(fd, FS_IOC_GETFLAGS, buf)
        except OSError:
            return              # not efivarfs (the unit tests' stand-in)
        flags = struct.unpack_from('<i', buf)[0]
        if flags & FS_IMMUTABLE_FL:
            struct.pack_into('<i', buf, 0, flags & ~FS_IMMUTABLE_FL)
            fcntl.ioctl(fd, FS_IOC_SETFLAGS, buf)
    finally:
        os.close(fd)


def var_set(name, data, guid=GLOBAL):
    path = _var(name, guid)
    _mutable(path)
    # one write: efivarfs takes attributes + data as a single variable set
    # (efivarfs replaces the variable on every write; the unit tests'
    # stand-in is a plain directory, which has to be truncated)
    trunc = os.O_TRUNC if 'OTHERSYS_TEST_EFIVARS' in os.environ else 0
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | trunc, 0o644)
    try:
        os.write(fd, struct.pack('<I', NV_BS_RT) + bytes(data))
    finally:
        os.close(fd)
    if var_get(name, guid) != bytes(data):
        raise RuntimeError('%s did not read back as written' % name)


def var_del(name, guid=GLOBAL):
    path = _var(name, guid)
    if not os.path.exists(path):
        return False
    _mutable(path)
    os.unlink(path)
    if os.path.exists(path):
        raise RuntimeError('%s is still there after deleting it' % name)
    return True


def u16list(data):
    if not data:
        return []
    return list(struct.unpack('<%dH' % (len(data) // 2), data[:len(data) // 2 * 2]))


def boot_order():
    return u16list(var_get('BootOrder'))


def set_boot_order(nums):
    var_set('BootOrder', b''.join(struct.pack('<H', n) for n in nums))


def boot_next():
    d = var_get('BootNext')
    return struct.unpack('<H', d[:2])[0] if d and len(d) >= 2 else None


def set_boot_next(num):
    var_set('BootNext', struct.pack('<H', num))


def load_option(data):
    """EFI_LOAD_OPTION → dict(description, partuuid, path), or None."""
    if not data or len(data) < 6:
        return None
    attrs, dp_len = struct.unpack_from('<IH', data, 0)
    i, desc = 6, []
    while i + 1 < len(data):
        c = struct.unpack_from('<H', data, i)[0]
        i += 2
        if c == 0:
            break
        desc.append(chr(c))
    dp = data[i:i + dp_len]
    out = {'attributes': attrs, 'description': ''.join(desc),
           'partuuid': None, 'path': None}
    j = 0
    while j + 4 <= len(dp):
        t, st = dp[j], dp[j + 1]
        ln = struct.unpack_from('<H', dp, j + 2)[0]
        if ln < 4 or j + ln > len(dp):
            return None
        if t == 0x7F:
            break
        if t == 0x04 and st == 0x01 and ln == 42 and dp[j + 41] == 0x02:
            out['partuuid'] = str(uuid.UUID(bytes_le=bytes(dp[j + 24:j + 40])))
        if t == 0x04 and st == 0x04:
            out['path'] = dp[j + 4:j + ln].decode('utf-16-le', 'replace').rstrip('\x00')
        j += ln
    return out


def boot_entries():
    """{number: load option dict or None (unreadable)} for every Boot####."""
    out = {}
    try:
        names = os.listdir(EFIVARS)
    except FileNotFoundError:
        return out
    for n in names:
        m = re.match(r'^Boot([0-9A-Fa-f]{4})-' + GLOBAL + '$', n)
        if m:
            out[int(m.group(1), 16)] = load_option(var_get('Boot' + m.group(1)))
    return out


def find_entry(entries, partuuid, path_suffix):
    hits = [n for n, lo in entries.items()
            if lo and lo['partuuid'] == partuuid and lo['path']
            and lo['path'].replace('/', '\\').lower().endswith(path_suffix.lower())]
    return sorted(hits)


def restored_order(recorded, current, ours):
    """Windows-first order: what Windows recorded (the entries still in the
    firmware), then Ubuntu, then anything else that appeared since, in its
    current order. Nothing that is in BootOrder now is left out."""
    present = set(current) | set(recorded)
    out = [n for n in recorded if n in present and n != ours]
    out.append(ours)
    out += [n for n in current if n not in out]
    return out


def promoted_order(current, ours):
    return [ours] + [n for n in current if n != ours]


def mok_pending():
    return [n for n in MOK_REQUESTS if var_get(n, SHIM) is not None]


# ------------------------------------------------------------------- disks

def run(cmd, **kw):
    log('run: ' + ' '.join(cmd))
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def sfdisk_json(disk):
    return json.loads(run(['sfdisk', '--json', disk]))['partitiontable']


def disk_by_guid(guid):
    out = json.loads(run(['lsblk', '-J', '-d', '-o', 'PATH,PTUUID,TYPE']))
    hits = [d['path'] for d in out['blockdevices']
            if d.get('type') == 'disk' and (d.get('ptuuid') or '').lower() == guid.lower()]
    if len(hits) != 1:
        raise Refuse('the disk Windows prepared (GPT %s) was found %d times' % (guid, len(hits)))
    return hits[0]


def part_node(disk, number):
    return '%s%s%d' % (disk, 'p' if disk[-1].isdigit() else '', number)


def compare_layout(plan, table):
    """Every partition Windows recorded, exactly; and nothing more."""
    probs = []
    if table.get('label') != 'gpt':
        probs.append('the disk is not GPT')
    if (table.get('id') or '').lower() != plan['disk_guid'].lower():
        probs.append('disk GUID %s, expected %s' % (table.get('id'), plan['disk_guid']))
    ss = table.get('sectorsize', 512)
    if ss != plan['sector_size']:
        probs.append('sector size %s, expected %s' % (ss, plan['sector_size']))
    live = {}
    for p in table.get('partitions', []):
        m = re.search(r'(\d+)$', p['node'])
        live[int(m.group(1))] = p
    want = {p['number']: p for p in plan['partitions']}
    for n in sorted(set(live) | set(want)):
        if n not in live:
            probs.append('partition %d is missing' % n)
            continue
        if n not in want:
            probs.append('partition %d was not there when Windows looked' % n)
            continue
        lp, wp = live[n], want[n]
        if lp['start'] != wp['first_lba'] or lp['size'] != wp['sectors']:
            probs.append('partition %d moved: %s+%s, expected %s+%s'
                         % (n, lp['start'], lp['size'], wp['first_lba'], wp['sectors']))
        if lp['type'].lower() != wp['type'].lower():
            probs.append('partition %d type %s, expected %s' % (n, lp['type'], wp['type']))
        if lp.get('uuid', '').lower() != wp['guid'].lower():
            probs.append('partition %d GUID %s, expected %s' % (n, lp.get('uuid'), wp['guid']))
    return probs


# ------------------------------------------------------------------- early

def cmd_early(plan):
    disk = disk_by_guid(plan['disk_guid'])
    probs = compare_layout(plan, sfdisk_json(disk))
    if probs:
        for p in probs:
            log('REFUSE: ' + p)
        raise Refuse('the disk is not as Windows left it; nothing was written')
    iso = part_node(disk, plan['iso_number'])
    src = run(['findmnt', '-n', '-o', 'SOURCE', '/cdrom']).strip()
    if os.path.realpath(src) != os.path.realpath(iso):
        raise Refuse('the installer is running from %s, not from %s' % (src, iso))
    if plan['test'].get('fail_early'):
        raise Refuse('test: refusing on purpose')
    path = '/autoinstall.yaml'
    text = open(path).read()
    if '__OTHERSYS_DISK__' not in text:
        raise Refuse('%s has no disk placeholder' % path)
    with open(path, 'w') as f:
        f.write(text.replace('__OTHERSYS_DISK__', disk))
    log('early: disk %s matches what Windows recorded (%d partitions); '
        'installer runs from %s' % (disk, len(plan['partitions']), iso))


# -------------------------------------------------------------------- late

def write(path, text, mode=0o644, uid=None, gid=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)
    os.chmod(path, mode)
    if uid is not None:
        os.chown(path, uid, gid)


def target_user(target, name):
    for line in open(os.path.join(target, 'etc/passwd')):
        f = line.split(':')
        if f[0] == name:
            return int(f[2]), int(f[3]), f[5]
    raise RuntimeError('user %s is not in the new system' % name)


def firmware_fixup(plan, state):
    """What Ubuntu's installer did to the firmware, put back."""
    entries = boot_entries()
    ours = find_entry(entries, plan['esp_partuuid'], '\\EFI\\ubuntu\\shimx64.efi')
    if len(ours) != 1:
        raise RuntimeError('expected one "ubuntu" entry on the new EFI partition, found %s' % ours)
    ours = ours[0]
    before = boot_order()
    log('late: Ubuntu made Boot%04X; BootOrder is now %s' % (ours, fmt(before)))
    state['ubuntu_entry'] = '%04X' % ours
    state['bootorder_after_installer'] = fmt(before)
    if plan['test'].get('skip_fixup'):
        log('late: TEST: leaving the firmware as the installer left it')
        return
    # AurOS's own one-shot entry for the restart: it pointed at a partition
    # Ubuntu has since formatted. Only ours, only by its partition.
    gone = None
    h = plan.get('handoff_entry')
    if h is not None:
        hn = int(h, 16)
        lo = entries.get(hn)
        if lo and lo['partuuid'] == plan['esp_partuuid'] and lo['description'] == plan['handoff_desc']:
            var_del('Boot%04X' % hn)
            gone = hn
            log('late: removed the restart entry Boot%04X' % hn)
        elif lo:
            log('late: Boot%04X is not the restart entry any more; left alone' % hn)
    # the entry deleted above is the only number that leaves the order
    cur = [n for n in boot_order() if n != gone]
    recorded = [int(x, 16) for x in plan['bootorder_before']]
    new = restored_order(recorded, cur, ours)
    set_boot_order(new)
    set_boot_next(ours)
    log('late: BootOrder %s (Windows first), BootNext %04X' % (fmt(new), ours))
    state['bootorder_after_fixup'] = fmt(new)


def fmt(nums):
    return ','.join('%04X' % n for n in nums)


def cancel_mok(state):
    pending = mok_pending()
    state['mok_pending_at_install'] = pending
    for n in pending:
        var_del(n, SHIM)
    left = mok_pending()
    if left:
        raise RuntimeError('a MOK request is still queued: %s' % left)
    log('late: MOK requests %s' % ('cancelled: %s' % pending if pending else 'none queued'))


def session_name(target, system):
    """The desktop session SDDM must start: the one named like the system
    (budgie-desktop for ubuntu-budgie-...), else the first there is."""
    found = []
    for d in ('usr/share/wayland-sessions', 'usr/share/xsessions'):
        try:
            found += sorted(f[:-8] for f in os.listdir(os.path.join(target, d)) if f.endswith('.desktop'))
        except OSError:
            pass
    words = set(system.split('-'))
    for name in found:
        if words & set(name.split('-')):
            return name
    return found[0] if found else None


def autologin(target, plan):
    """Sign her in by herself, whichever login screen the system has:
    GDM (Ubuntu), LightDM (Xubuntu, Ubuntu Cinnamon), SDDM (Ubuntu
    Budgie). Returns what was configured."""
    user, done = plan['user'], []
    if os.path.isdir(os.path.join(target, 'etc/gdm3')):
        conf = os.path.join(target, 'etc/gdm3/custom.conf')
        text = open(conf).read() if os.path.exists(conf) else '[daemon]\n'
        text = re.sub(r'(?m)^#?\s*AutomaticLoginEnable\s*=.*\n', '', text)
        text = re.sub(r'(?m)^#?\s*AutomaticLogin\s*=.*\n', '', text)
        if '[daemon]' not in text:
            text = '[daemon]\n' + text
        text = text.replace('[daemon]\n', '[daemon]\nAutomaticLoginEnable=true\n'
                            'AutomaticLogin=%s\n' % user, 1)
        write(conf, text)
        done.append('gdm3')
    if os.path.isdir(os.path.join(target, 'etc/lightdm')):
        write(os.path.join(target, 'etc/lightdm/lightdm.conf.d/50-auros-othersys.conf'),
              '[Seat:*]\nautologin-user=%s\nautologin-user-timeout=0\n' % user)
        done.append('lightdm')
    if os.path.exists(os.path.join(target, 'usr/bin/sddm')):
        sess = session_name(target, plan.get('system', ''))
        write(os.path.join(target, 'etc/sddm.conf.d/50-auros-othersys.conf'),
              '[Autologin]\nUser=%s\n%s' % (user, 'Session=%s\n' % sess if sess else ''))
        done.append('sddm (%s)' % sess)
    if not done:
        log('late: no login screen found that AurOS knows; she will be asked for her password')
    return done


def install_helper(plan, target, state):
    lib = os.path.join(target, LIB.lstrip('/'))
    os.makedirs(lib, exist_ok=True)
    for f in ('othersys.py', 'ask', 'plan.json'):
        data = open(os.path.join(LIVE_LIB, f), 'rb').read()
        with open(os.path.join(lib, f), 'wb') as g:
            g.write(data)
        os.chmod(os.path.join(lib, f), 0o755 if f != 'plan.json' else 0o644)
    units = os.path.join(LIVE_LIB, 'units')
    sysd = os.path.join(target, 'etc/systemd/system')
    for u in sorted(os.listdir(units)):
        data = open(os.path.join(units, u)).read()
        if u.endswith('.desktop'):
            write(os.path.join(target, 'etc/xdg/autostart', u), data)
            continue
        write(os.path.join(sysd, u), data)
        if u.endswith('.service') and 'WantedBy=multi-user.target' in data:
            wants = os.path.join(sysd, 'multi-user.target.wants')
            os.makedirs(wants, exist_ok=True)
            link = os.path.join(wants, u)
            if not os.path.lexists(link):
                os.symlink('/etc/systemd/system/' + u, link)
        if u.endswith('.path'):
            wants = os.path.join(sysd, 'paths.target.wants')
            os.makedirs(wants, exist_ok=True)
            link = os.path.join(wants, u)
            if not os.path.lexists(link):
                os.symlink('/etc/systemd/system/' + u, link)
    uid, gid, home = target_user(target, plan['user'])
    st = os.path.join(target, STATE_DIR.lstrip('/'))
    os.makedirs(st, exist_ok=True)
    os.chmod(st, 0o755)
    inbox = os.path.join(target, INBOX.lstrip('/'))
    os.makedirs(inbox, exist_ok=True)
    os.chown(inbox, uid, gid)
    os.chmod(inbox, 0o700)
    # later shim/grub updates must not re-promote Ubuntu before she says yes
    subprocess.run(['chroot', target, 'debconf-set-selections'], check=True,
                   input='grub-efi-amd64 grub2/update_nvram boolean false\n', text=True)
    if plan.get('autologin'):
        state['autologin'] = autologin(target, plan)
    # the "Welcome to Ubuntu" tour asks about Ubuntu Pro and telemetry;
    # the defaults it would keep are what she gets without it
    gis = os.path.join(target, home.lstrip('/'), '.config/gnome-initial-setup-done')
    write(gis, 'yes', uid=uid, gid=gid)
    os.chown(os.path.dirname(gis), uid, gid)
    state['installed_at'] = int(time.time())
    write(os.path.join(st, 'state.json'), json.dumps(state, indent=1) + '\n')
    log('late: helper installed into the new system (user %s, autologin %s)'
        % (plan['user'], state.get('autologin')))


def windows_first_again(plan, why):
    """If something new was put ahead of what Windows recorded (what
    Ubuntu's installer does with "ubuntu"), put Windows' order back in
    front. Nothing leaves BootOrder. Returns the new order, or None."""
    cur = boot_order()
    recorded = [int(x, 16) for x in plan['bootorder_before']]
    if not cur or cur[0] in recorded:
        return None
    new = restored_order(recorded, cur, cur[0])
    set_boot_order(new)
    log('%s: Boot%04X was put ahead of Windows; BootOrder %s again' % (why, cur[0], fmt(new)))
    return new


GUARD_MAX_WRITES = 20


def cmd_guard(plan):
    """Live session, from the early check until the machine switches off:
    the moment the installer puts Ubuntu first, put Windows back first.
    Without this, a power cut between the installer's firmware write and
    the late fix-up would start a half-installed Ubuntu; with it, Windows.
    The late fix-up then arms BootNext as before."""
    if plan['test'].get('skip_fixup'):
        log('guard: TEST: not guarding')
        return
    log('guard: watching BootOrder')
    writes = 0
    while writes < GUARD_MAX_WRITES:
        try:
            if windows_first_again(plan, 'guard'):
                writes += 1
        except Exception as e:      # keep watching; late fixes it anyway
            log('guard: %s' % e)
        time.sleep(1)
    log('guard: %d rewrites; something keeps changing BootOrder; stopped' % writes)


def cmd_error(plan):
    """The install failed. Whatever the installer did to the firmware, the
    next start is Windows, and Windows can learn what happened."""
    try:
        windows_first_again(plan, 'error')
    except Exception as e:
        log('error: could not put Windows first: %s' % e)
    if boot_next() is not None:
        var_del('BootNext')
        log('error: BootNext cleared')
    record_answer({'system': plan['system'], 'answer': 'failed'})
    log('error: the next start is Windows; BootOrder %s' % fmt(boot_order()))


def cmd_late(plan, target='/target'):
    state = {'system': plan['system']}
    firmware_fixup(plan, state)
    cancel_mok(state)
    install_helper(plan, target, state)


# -------------------------------------------------------------------- boot

def load_state():
    try:
        return json.load(open(os.path.join(STATE_DIR, 'state.json')))
    except (OSError, ValueError):
        return {}


def save_state(state):
    tmp = os.path.join(STATE_DIR, 'state.json.new')
    with open(tmp, 'w') as f:
        f.write(json.dumps(state, indent=1) + '\n')
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, os.path.join(STATE_DIR, 'state.json'))


def iso_fs_uuid(node):
    with open(node, 'rb') as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
    if pvd[1:6] != b'CD001':
        return None
    d = pvd[830:846].decode('ascii', 'replace')
    return '%s-%s-%s-%s-%s-%s-%s' % (d[0:4], d[4:6], d[6:8], d[8:10], d[10:12], d[12:14], d[14:16])


def remove_iso(plan, state):
    """The ISO partition, once the system it installed has started."""
    if state.get('iso_removed'):
        return
    disk = disk_by_guid(plan['disk_guid'])
    n = plan['iso_number']
    table = sfdisk_json(disk)
    live = {int(re.search(r'(\d+)$', p['node']).group(1)): p for p in table['partitions']}
    p = live.get(n)
    if p is None:
        state['iso_removed'] = 'already gone'
        return
    node = part_node(disk, n)
    why = []
    if p.get('uuid', '').lower() != plan['iso_partuuid'].lower():
        why.append('its GUID is %s' % p.get('uuid'))
    if p.get('name') != plan['iso_name']:
        why.append('its name is %r' % p.get('name'))
    fsu = iso_fs_uuid(node)
    if fsu != plan['iso_fsuuid']:
        why.append('it holds %s, not the Ubuntu ISO' % fsu)
    if why:
        log('boot: partition %d is not the ISO AurOS wrote (%s); left alone' % (n, '; '.join(why)))
        state['iso_removed'] = 'refused: ' + '; '.join(why)
        return
    run(['sfdisk', '--no-reread', '--no-tell-kernel', '--wipe-partitions', 'never',
         '--delete', disk, str(n)])
    subprocess.run(['partx', '-d', '--nr', str(n), disk], capture_output=True)
    state['iso_removed'] = int(time.time())
    log('boot: removed the temporary ISO partition %s' % node)
    grow_root(plan, disk, state)


def grow_root(plan, disk, state):
    """Give the ISO's space to Ubuntu if it lies right after the root."""
    rn = plan['root_number']
    table = sfdisk_json(disk)
    parts = sorted(table['partitions'], key=lambda p: p['start'])
    root = [p for p in parts if p['node'] == part_node(disk, rn)]
    if not root:
        return
    root = root[0]
    end = root['start'] + root['size']
    nxt = [p['start'] for p in parts if p['start'] >= end]
    limit = min(nxt) if nxt else table['lastlba'] + 1
    if limit - end < 2048:
        state['root_grown'] = 'no room'
        return
    run(['sfdisk', '--no-reread', '--no-tell-kernel', '--wipe-partitions', 'never',
         '-N', str(rn), disk], input=',+\n')
    run(['partx', '-u', '--nr', str(rn), disk])
    run(['resize2fs', part_node(disk, rn)])
    state['root_grown'] = '%d sectors' % (limit - end)
    log('boot: Ubuntu now also has the ISO\'s %d MiB' % ((limit - end) * table.get('sectorsize', 512) >> 20))


def hold(plan, state):
    """Until she answers: Windows first, and back to Ubuntu next time."""
    ours = int(state['ubuntu_entry'], 16)
    entries = boot_entries()
    if ours not in entries:
        log('boot: Boot%04X is gone from the firmware; nothing to hold' % ours)
        return
    cur = boot_order()
    recorded = [int(x, 16) for x in plan['bootorder_before']]
    win = [n for n in recorded if n in entries and n in cur]
    ahead = bool(win) and ours in cur and cur.index(ours) < cur.index(win[0])
    if ahead or ours not in cur:
        new = restored_order(recorded, cur, ours)
        set_boot_order(new)
        log('boot: %s; BootOrder %s again' % (
            'something put Ubuntu ahead of Windows' if ahead else 'Ubuntu was not in BootOrder',
            fmt(new)))
    if boot_next() != ours:
        set_boot_next(ours)
    log('boot: holding: BootOrder %s, BootNext %04X' % (fmt(boot_order()), ours))


def record_answer(state):
    """Leave her answer where the Windows side can read it. Never fatal:
    the firmware already does what she said; this only lets Windows tidy
    up (after "no": give C: its space back) without asking her again."""
    data = ('answer=%s\nsystem=%s\n' % (state['answer'], state.get('system', ''))).encode('ascii')
    try:
        var_set(ANSWER_VAR, data, AUROS_GUID)
        state['answer_recorded'] = True
        log('answer: recorded for Windows (%s)' % ANSWER_VAR)
    except Exception as e:
        state['answer_recorded'] = 'failed: %s' % e
        log('answer: could not record it for Windows: %s' % e)


def confirm(state):
    ours = int(state['ubuntu_entry'], 16)
    new = promoted_order(boot_order(), ours)
    set_boot_order(new)
    state['answer'] = 'yes'
    state['answered_at'] = int(time.time())
    log('answer: yes: BootOrder %s (Ubuntu first, everything else kept)' % fmt(new))
    record_answer(state)


def decline(state):
    # the one-shot first, so no start after this reaches Ubuntu by itself
    var_del('BootNext')
    state['answer'] = 'no'
    state['answered_at'] = int(time.time())
    log('answer: no: BootNext cleared; BootOrder %s' % fmt(boot_order()))
    record_answer(state)


def take_answer(state):
    path = os.path.join(INBOX, 'answer')
    try:
        word = open(path, 'rb').read(32).decode('ascii', 'replace').strip().lower()
    except OSError:
        return
    try:
        os.unlink(path)
    except OSError:
        pass
    if word not in ANSWERS:
        log('answer: %r is not an answer; ignored' % word)
        return
    if state.get('answer') in ('yes', 'no'):
        log('answer: already answered %s; %r ignored' % (state['answer'], word))
        return
    if word == 'yes':
        confirm(state)
    elif word == 'no':
        decline(state)
    else:
        log('answer: later')


def cmd_boot(plan):
    state = load_state()
    if not state:
        log('boot: no state; this system was not installed by AurOS')
        return
    try:
        remove_iso(plan, state)
    except Exception as e:     # never stop the boot over the ISO partition
        log('boot: could not remove the ISO partition: %s' % e)
        state['iso_removed'] = 'failed: %s' % e
    take_answer(state)
    if state.get('answer') not in ('yes', 'no'):
        hold(plan, state)
    elif state.get('answer_recorded') is not True:
        record_answer(state)
    pending = mok_pending()
    if pending:
        log('boot: WARNING: a MOK request is queued: %s' % pending)
    state['boots'] = state.get('boots', 0) + 1
    save_state(state)


def cmd_answer(plan):
    state = load_state()
    take_answer(state)
    save_state(state)


def cmd_status(plan):
    state = load_state()
    print(json.dumps({'state': state, 'bootorder': fmt(boot_order()),
                      'bootnext': '%04X' % boot_next() if boot_next() is not None else None,
                      'mok_pending': mok_pending()}, indent=1))


def main(argv):
    what = argv[1] if len(argv) > 1 else 'status'
    live = what in ('early', 'late', 'guard', 'error')
    here = LIVE_LIB if live else LIB
    plan = json.load(open(os.path.join(here, 'plan.json')))
    plan.setdefault('test', {})
    if live:
        os.environ.setdefault('OTHERSYS_LOG', LOG_LIVE)
    try:
        {'early': cmd_early, 'late': cmd_late, 'guard': cmd_guard, 'error': cmd_error,
         'boot': cmd_boot, 'answer': cmd_answer, 'status': cmd_status}[what](plan)
    except Refuse as e:
        log('REFUSED: %s' % e)
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
