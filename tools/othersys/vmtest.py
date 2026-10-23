#!/usr/bin/python3
"""vmtest — the othersys proof (P1) in a VM, phase by phase.

    vmtest.py WORK OUT [--variant ntfs|bitlocker|nofixup] [--phases a,b,...]

WORK is what mkdisk.py made (disk.raw, vars.fd, layout.json, boot.json,
before.json, seed/othersys/plan.json). OUT gets shots/, serial-*.log,
results.md, results.json, timings.

The machine: q35 + SMM, OVMF_CODE_4M.ms.fd with Secure Boot enforcing and
Microsoft's keys, an NVMe SSD, no network (a Wi-Fi laptop's installer has
none). Phases:

  install       BootNext (planted as Windows would) -> Ubuntu's shim from
                the ISO -> grub -> casper -> autoinstall -> poweroff
  firstboot     the firmware's own choice: Ubuntu, from the BootNext the
                install armed; the desktop, signed in automatically
  windowsfirst  BootNext cleared: what BootOrder alone starts (Windows)
  confirm       she answered "yes" (written into the inbox offline); the
                boot unit acts on it: Ubuntu first, Windows kept
  afteryes      BootNext cleared: BootOrder alone now starts Ubuntu
  moknegative   a MOK request planted on a copy of the firmware: the
                blue-screen check must FAIL here, proving it can see one
  powercut      (alone) the plug pulled the moment the installer has put
                Ubuntu first: the next start must be Windows

Each phase records wall-clock time. Every check is recorded with what it
was expected to do; "must fail" checks prove the check can fail.
"""
import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools'))
import checks  # noqa: E402
import efivarstore  # noqa: E402

OVMF_CODE = '/usr/share/OVMF/OVMF_CODE_4M.ms.fd'
SHIM_GUID = '605dab50-e046-4300-abb6-3dd810dd8b23'


class Rec:
    def __init__(self, out):
        self.out = out
        os.makedirs(os.path.join(out, 'shots'), exist_ok=True)
        self.results = []
        self.timings = []
        self.logf = open(os.path.join(out, 'driver.log'), 'a')

    def log(self, msg):
        line = '%s %s' % (time.strftime('%H:%M:%S'), msg)
        print(line, flush=True)
        self.logf.write(line + '\n')
        self.logf.flush()

    def check(self, phase, name, failures, expect='pass'):
        ok = (not failures) if expect == 'pass' else bool(failures)
        self.results.append({'phase': phase, 'check': name, 'expect': expect,
                             'ok': ok, 'detail': failures})
        self.log('%s  [%s] %s%s%s' % ('ok  ' if ok else 'FAIL', phase, name,
                                     ' (must fail: it did)' if expect == 'fail' and ok else '',
                                     (': ' + '; '.join(failures)) if failures else ''))
        self.write()
        return ok

    def time(self, phase, what, seconds):
        self.timings.append({'phase': phase, 'what': what, 'seconds': round(seconds, 1)})
        self.log('time [%s] %s: %.0f s' % (phase, what, seconds))
        self.write()

    def write(self):
        json.dump({'results': self.results, 'timings': self.timings},
                  open(os.path.join(self.out, 'results.json'), 'w'), indent=1)
        lines = ['# othersys P1 results', '',
                 '| ok | phase | check | expected | detail |', '|---|---|---|---|---|']
        for r in self.results:
            lines.append('| %s | %s | %s | %s | %s |' % (
                'yes' if r['ok'] else '**NO**', r['phase'], r['check'], r['expect'],
                '; '.join(r['detail'])[:300].replace('|', '/')))
        lines += ['', '## Timings (wall clock)', '', '| phase | what | seconds |', '|---|---|---|']
        for t in self.timings:
            lines.append('| %s | %s | %s |' % (t['phase'], t['what'], t['seconds']))
        open(os.path.join(self.out, 'results.md'), 'w').write('\n'.join(lines) + '\n')


class QMP:
    def __init__(self, path):
        for _ in range(200):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError('QMP did not come up')
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QMP closed')
            r = json.loads(line)
            if 'return' in r or 'error' in r:
                return r


class VM:
    def __init__(self, rec, work, phase, vars_fd=None, ram=8192, cpus=4):
        self.rec, self.work, self.phase = rec, work, phase
        self.vars = vars_fd or os.path.join(work, 'vars.fd')
        self.serial = os.path.join(rec.out, 'serial-%s.log' % phase)
        self.n = 0
        kvm = os.access('/dev/kvm', os.W_OK)
        self.accel = 'kvm' if kvm else 'tcg'
        sockdir = tempfile.mkdtemp(prefix='os-', dir='/tmp')
        self.qsock = os.path.join(sockdir, 'qmp.sock')
        cmd = ['qemu-system-x86_64', '-name', 'othersys', '-nodefaults',
               '-machine', 'q35,smm=on,accel=%s' % self.accel,
               '-cpu', 'host' if kvm else 'max', '-smp', str(cpus), '-m', str(ram),
               '-global', 'driver=cfi.pflash01,property=secure,value=on',
               '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=%s' % OVMF_CODE,
               '-drive', 'if=pflash,format=raw,unit=1,file=%s' % self.vars,
               '-drive', 'id=d0,if=none,format=raw,cache=unsafe,discard=unmap,file=%s'
               % os.path.join(work, 'disk.raw'),
               '-device', 'nvme,drive=d0,serial=AUROSOS0001',
               '-device', 'VGA,edid=on,xres=1280,yres=800',
               '-device', 'qemu-xhci,id=xhci', '-device', 'usb-tablet,bus=xhci.0',
               '-device', 'usb-kbd,bus=xhci.0',
               '-nic', 'none', '-rtc', 'base=utc',
               '-qmp', 'unix:%s,server=on,wait=off' % self.qsock,
               '-serial', 'file:%s' % self.serial,
               '-display', 'none', '-no-reboot']
        rec.log('[%s] qemu (%s): %s' % (phase, self.accel, ' '.join(cmd)))
        self.qlog = open(os.path.join(rec.out, 'qemu-%s.log' % phase), 'w')
        self.t0 = time.time()
        self.proc = subprocess.Popen(cmd, stdout=self.qlog, stderr=subprocess.STDOUT)
        self.qmp = QMP(self.qsock)

    def alive(self):
        return self.proc.poll() is None

    def text(self):
        try:
            return open(self.serial, 'rb').read().decode('utf-8', 'replace')
        except OSError:
            return ''

    def shot(self, tag=''):
        if not self.alive():
            return None
        self.n += 1
        base = os.path.join(self.rec.out, 'shots', '%s-%03d-%04ds%s' % (
            self.phase, self.n, time.time() - self.t0, ('-' + tag) if tag else ''))
        try:
            self.qmp.cmd('screendump', filename=base + '.ppm')
            from PIL import Image
            Image.open(base + '.ppm').save(base + '.png')
            os.unlink(base + '.ppm')
            return base + '.png'
        except Exception as e:      # the machine may be switching off
            self.rec.log('[%s] screendump failed: %s' % (self.phase, e))
            return None

    def run(self, timeout, until=None, every=20, stop_on=()):
        """Wait for the machine to switch itself off (True), or `until`
        (a serial marker) then keep waiting for the exit; screenshots every
        `every` seconds. Returns (exited, seconds, marks{marker: t})."""
        marks, last = {}, 0
        end = time.time() + timeout
        while time.time() < end and self.alive():
            t = self.text()
            for m in ([until] if until else []) + list(stop_on):
                if m and m not in marks and m in t:
                    marks[m] = time.time() - self.t0
                    self.rec.log('[%s] serial: %s at %.0f s' % (self.phase, m, marks[m]))
            if any(m in marks for m in stop_on):
                break
            if time.time() - last >= every:
                self.shot()
                last = time.time()
            time.sleep(1)
        exited = not self.alive()
        if not exited:
            self.shot('end')
        return exited, time.time() - self.t0, marks

    def kill(self):
        if self.alive():
            try:
                self.qmp.cmd('quit')
            except Exception:
                pass
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


# ------------------------------------------------------------------ helpers

def vars_live(vfd):
    return efivarstore.read_store(vfd)


def clear_var(vfd, name, guid=efivarstore.GLOBAL):
    blob = bytearray(open(vfd, 'rb').read())
    n = efivarstore.retire(blob, name, guid)
    open(vfd, 'wb').write(bytes(blob))
    return n


def set_next(vfd, num):
    efivarstore.plant(vfd, 'BootNext', efivarstore.GLOBAL, struct.pack('<H', int(num, 16)))


class RootFS:
    """The installed root partition, mounted from the image (machine off)."""
    def __init__(self, work, plan, rw=False):
        tab, parts = checks.gpt(os.path.join(work, 'disk.raw'))
        q = parts[plan['root_number']]
        ss = tab.get('sectorsize', 512)
        self.mnt = tempfile.mkdtemp(prefix='osroot-')
        self.loop = subprocess.run(
            ['sudo', 'losetup', '-f', '--show', '-o', str(q['start'] * ss),
             '--sizelimit', str(q['size'] * ss)] + ([] if rw else ['-r']) +
            [os.path.join(work, 'disk.raw')], check=True, capture_output=True,
            text=True).stdout.strip()
        subprocess.run(['sudo', 'mount', '-o', 'rw' if rw else 'ro,noload', self.loop, self.mnt],
                       check=True)

    def read(self, path):
        r = subprocess.run(['sudo', 'cat', os.path.join(self.mnt, path.lstrip('/'))],
                           capture_output=True)
        return r.stdout if r.returncode == 0 else None

    def write(self, path, data):
        subprocess.run(['sudo', 'tee', os.path.join(self.mnt, path.lstrip('/'))],
                       input=data, check=True, stdout=subprocess.DEVNULL)

    def copy_out(self, path, dest):
        subprocess.run(['sudo', 'cp', '-r', os.path.join(self.mnt, path.lstrip('/')), dest])
        subprocess.run(['sudo', 'chown', '-R', '%d:%d' % (os.getuid(), os.getgid()), dest])

    def close(self):
        subprocess.run(['sudo', 'umount', self.mnt])
        subprocess.run(['sudo', 'losetup', '-d', self.loop])
        os.rmdir(self.mnt)


def firmware_checks(rec, phase, work, plan, boot, expect_first, expect_next, after_install=True):
    v = vars_live(os.path.join(work, 'vars.fd'))
    rec.check(phase, 'no MOK enrolment queued (MokNew/MokAuth/... absent)', checks.check_no_mok(v))
    rec.check(phase, 'Ubuntu has one entry, on its own EFI partition',
              checks.check_ubuntu_entry(v, plan['esp_partuuid']))
    ub = checks.ubuntu_entry(v, plan['esp_partuuid'])
    ub = ub[0] if ub else None
    if after_install:
        rec.check(phase, 'the "AurOS Installer" restart entry is gone',
                  checks.check_entry_gone(v, boot['handoff_entry'], boot['handoff_desc']))
    if expect_first == 'windows':
        rec.check(phase, 'BootOrder starts Windows first', checks.check_windows_first(v, boot['windows_entry']))
    elif expect_first == 'ubuntu' and ub:
        rec.check(phase, 'BootOrder starts Ubuntu first, Windows kept',
                  checks.check_ubuntu_first_windows_kept(v, ub, boot['windows_entry']))
    if expect_next == 'ubuntu' and ub:
        rec.check(phase, 'BootNext is Ubuntu (the hold)', checks.check_bootnext(v, ub))
    rec.log('[%s] firmware: BootOrder %s BootNext %s' % (phase, ','.join(checks.order(v)), checks.bootnext(v)))
    return ub


def windows_checks(rec, phase, work, layout, before):
    after = checks.snapshot(os.path.join(work, 'disk.raw'), layout)
    json.dump(after, open(os.path.join(rec.out, 'snapshot-%s.json' % phase), 'w'), indent=1)
    rec.check(phase, 'Windows\' partitions byte-identical, GPT entries unchanged',
              checks.check_windows_intact(before, after))


def screens_not_mok(rec, phase):
    shots = sorted(f for f in os.listdir(os.path.join(rec.out, 'shots')) if f.startswith(phase + '-'))
    bad = []
    for f in shots:
        r = checks.check_not_mokmanager(os.path.join(rec.out, 'shots', f))
        if r:
            bad.append('%s: %s' % (f, '; '.join(r)))
    rec.check(phase, 'no MokManager (blue) screen in any of %d screenshots' % len(shots), bad)


# ------------------------------------------------------------------- phases

def phase_install(rec, work, plan, boot, layout, before, variant):
    vm = VM(rec, work, 'install')
    exited, secs, marks = vm.run(
        timeout=80 * 60, until='OTHERSYS-TEST LATE-DONE',
        stop_on=('OTHERSYS-TEST INSTALL-ERROR',))
    t = vm.text()
    for m, label in (('BdsDxe: starting Boot0003', 'firmware hands over to the ISO\'s shim'),
                     ('Linux version', 'kernel started'),
                     ('othersys: early:', 'early-commands: disk matched'),
                     ('othersys: late:', 'late-commands started'),
                     ('OTHERSYS-TEST LATE-DONE', 'install finished')):
        i = t.find(m)
        rec.log('[install] marker %-40s %s' % (label, 'seen' if i >= 0 else 'NOT seen'))
    if 'OTHERSYS-TEST INSTALL-ERROR' in marks:
        time.sleep(60)
        vm.shot('error')
    vm.kill()
    rec.time('install', 'BootNext to power off (whole unattended install)', secs)
    rec.check('install', 'the firmware started the ISO\'s shim from the BootNext entry',
              checks.check_marker(t, 'BdsDxe: starting Boot0003'))
    rec.check('install', 'the early check matched the disk Windows recorded',
              checks.check_marker(t, 'othersys: early: disk'))
    rec.check('install', 'the installer finished and switched off',
              checks.check_marker(t, 'OTHERSYS-TEST LATE-DONE') + ([] if exited else ['still running at the deadline']))
    windows_checks(rec, 'install', work, layout, before)
    if variant == 'nofixup':
        v = vars_live(os.path.join(work, 'vars.fd'))
        rec.check('install', 'WITHOUT the fixup, Ubuntu\'s installer leaves Ubuntu first '
                  '(so the Windows-first check must fail)',
                  checks.check_windows_first(v, boot['windows_entry']), expect='fail')
        rec.log('[install] firmware as the installer left it: BootOrder %s BootNext %s'
                % (','.join(checks.order(v)), checks.bootnext(v)))
        return False
    ub = firmware_checks(rec, 'install', work, plan, boot, 'windows', 'ubuntu')
    return ub is not None and exited


def phase_boot(rec, work, plan, name, timeout=20 * 60):
    vm = VM(rec, work, name)
    exited, secs, marks = vm.run(timeout=timeout, every=15,
                                 stop_on=('OTHERSYS-TEST SESSION-UP', 'WINDOWS-STAND-IN-BOOTED'))
    if 'OTHERSYS-TEST SESSION-UP' in marks:
        # the session is up; give the question its 8 seconds and the shell
        # its first frame, then keep the picture
        while vm.alive() and time.time() - vm.t0 < marks['OTHERSYS-TEST SESSION-UP'] + 22:
            time.sleep(1)
        vm.shot('desktop')
        exited, secs2, _m = vm.run(timeout=120)
        secs = time.time() - vm.t0
    elif 'WINDOWS-STAND-IN-BOOTED' in marks:
        time.sleep(2)
        vm.shot('windows')
        vm.run(timeout=60)
        secs = time.time() - vm.t0
    t = vm.text()
    vm.kill()
    return t, marks, secs


def phase_powercut(rec, work, plan, boot, layout, before):
    """Pull the plug the moment the installer has put Ubuntu first (the
    guard's log line says it has, and has put Windows back): the next
    start must be Windows, with Windows untouched."""
    vm = VM(rec, work, 'powercut')
    exited, secs, marks = vm.run(timeout=60 * 60, every=30,
                                 stop_on=('othersys: guard: Boot', 'OTHERSYS-TEST LATE-DONE',
                                          'OTHERSYS-TEST INSTALL-ERROR'))
    vm.shot('cut')
    vm.proc.kill()          # SIGKILL: no shutdown, no flush in the guest
    vm.proc.wait()
    t = vm.text()
    rec.time('powercut', 'power on to the cut', secs)
    rec.check('powercut', 'the installer put Ubuntu first and the guard put Windows back',
              checks.check_marker(t, 'othersys: guard: Boot'))
    rec.check('powercut', 'the cut came before the install finished',
              [] if 'OTHERSYS-TEST LATE-DONE' not in t else ['the install had already finished'])
    v = vars_live(os.path.join(work, 'vars.fd'))
    rec.log('[powercut] firmware at the cut: BootOrder %s BootNext %s' % (','.join(checks.order(v)), checks.bootnext(v)))
    rec.check('powercut', 'at the cut: BootOrder starts Windows first',
              checks.check_windows_first(v, boot['windows_entry']))
    rec.check('powercut', 'at the cut: no BootNext',
              [] if checks.bootnext(v) is None else ['BootNext %s' % checks.bootnext(v)])
    windows_checks(rec, 'powercut', work, layout, before)
    t, marks, secs = phase_boot(rec, work, plan, 'afterpowercut', timeout=6 * 60)
    rec.time('afterpowercut', 'power on to Windows Boot Manager', marks.get('WINDOWS-STAND-IN-BOOTED', secs))
    rec.check('afterpowercut', 'the next start is Windows',
              checks.check_marker(t, 'WINDOWS-STAND-IN-BOOTED'))
    rec.check('afterpowercut', 'and not the half-installed Ubuntu',
              [] if 'Linux version' not in t else ['a Linux kernel started'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work')
    ap.add_argument('out')
    ap.add_argument('--variant', default='ntfs')
    ap.add_argument('--phases', default='install,firstboot,windowsfirst,confirm,afteryes,moknegative')
    a = ap.parse_args()
    work, out = os.path.abspath(a.work), os.path.abspath(a.out)
    rec = Rec(out)
    plan = json.load(open(os.path.join(work, 'seed/othersys/plan.json')))
    boot = json.load(open(os.path.join(work, 'boot.json')))
    layout = json.load(open(os.path.join(work, 'layout.json')))
    before = json.load(open(os.path.join(work, 'before.json')))
    phases = a.phases.split(',')
    rec.log('variant %s, phases %s' % (a.variant, phases))
    try:
        run_phases(rec, work, out, plan, boot, layout, before, phases, a.variant)
    except Exception as e:
        import traceback
        rec.log(traceback.format_exc())
        rec.check('harness', 'the harness ran to the end', ['%s: %s' % (type(e).__name__, e)])
    return finish(rec, work, plan)


def run_phases(rec, work, out, plan, boot, layout, before, phases, variant):
    root_before = [p for p in layout['partitions'] if p['number'] == plan['root_number']][0]['sectors']
    ub = None
    if 'smoke' in phases:
        vm = VM(rec, work, 'smoke', ram=4096, cpus=2)
        exited, secs, marks = vm.run(timeout=int(os.environ.get('SMOKE_SECONDS', '900')), every=30,
                                     until='casper', stop_on=('Running /scripts/casper-bottom',
                                                              'Can\'t find', 'Kernel panic'))
        vm.shot('end')
        vm.kill()
        rec.log('[smoke] %s' % marks)
        return
    if 'powercut' in phases:
        return phase_powercut(rec, work, plan, boot, layout, before)
    if True:
        if 'install' in phases:
            if not phase_install(rec, work, plan, boot, layout, before, variant):
                rec.log('install did not leave a machine to go on with; stopping')
                return
            v = vars_live(os.path.join(work, 'vars.fd'))
            ub = checks.ubuntu_entry(v, plan['esp_partuuid'])[0]
            r = RootFS(work, plan)
            r.copy_out('/var/log/installer', os.path.join(out, 'installer-logs'))
            st = r.read('/var/lib/auros-othersys/state.json')
            open(os.path.join(out, 'state-after-install.json'), 'wb').write(st or b'')
            r.close()
            rec.check('install', 'the helper recorded the install in the new system',
                      [] if st else ['/var/lib/auros-othersys/state.json is missing'])
        v = vars_live(os.path.join(work, 'vars.fd'))
        hits = checks.ubuntu_entry(v, plan['esp_partuuid'])
        ub = hits[0] if hits else ub

        if 'firstboot' in phases:
            t, marks, secs = phase_boot(rec, work, plan, 'firstboot')
            rec.time('firstboot', 'power on to the desktop session (signed in by itself)',
                     marks.get('OTHERSYS-TEST SESSION-UP', secs))
            rec.check('firstboot', 'Ubuntu started from its own disk and signed her in',
                      checks.check_marker(t, 'OTHERSYS-TEST SESSION-UP'))
            screens_not_mok(rec, 'firstboot')
            windows_checks(rec, 'firstboot', work, layout, before)
            rec.check('firstboot', 'the temporary ISO partition is gone',
                      checks.check_iso_gone(os.path.join(work, 'disk.raw'), plan))
            rec.check('firstboot', 'Ubuntu got the ISO\'s space',
                      checks.check_root_grew(os.path.join(work, 'disk.raw'), plan, root_before))
            firmware_checks(rec, 'firstboot', work, plan, boot, 'windows', 'ubuntu')

        if 'windowsfirst' in phases:
            clear_var(os.path.join(work, 'vars.fd'), 'BootNext')
            t, marks, secs = phase_boot(rec, work, plan, 'windowsfirst', timeout=6 * 60)
            rec.time('windowsfirst', 'power on to Windows Boot Manager', marks.get('WINDOWS-STAND-IN-BOOTED', secs))
            rec.check('windowsfirst', 'without BootNext, the firmware starts Windows (BootOrder)',
                      checks.check_marker(t, 'WINDOWS-STAND-IN-BOOTED'))
            rec.check('windowsfirst', 'and Ubuntu did not start',
                      [] if 'OTHERSYS-TEST BOOT ' not in t else ['Ubuntu started'])

        if 'confirm' in phases and ub:
            r = RootFS(work, plan, rw=True)
            r.write('/var/lib/auros-othersys/inbox/answer', b'yes\n')
            r.close()
            set_next(os.path.join(work, 'vars.fd'), ub)
            t, marks, secs = phase_boot(rec, work, plan, 'confirm')
            rec.time('confirm', 'power on to the desktop session', marks.get('OTHERSYS-TEST SESSION-UP', secs))
            rec.check('confirm', 'Ubuntu started and took the answer',
                      checks.check_marker(t, 'answer: yes'))
            firmware_checks(rec, 'confirm', work, plan, boot, 'ubuntu', None)
            rec.check('confirm', 'her answer is in the firmware for Windows to read',
                      checks.check_answer_recorded(vars_live(os.path.join(work, 'vars.fd')), 'yes'))
            windows_checks(rec, 'confirm', work, layout, before)
            screens_not_mok(rec, 'confirm')

        if 'afteryes' in phases and ub:
            clear_var(os.path.join(work, 'vars.fd'), 'BootNext')
            t, marks, secs = phase_boot(rec, work, plan, 'afteryes')
            rec.time('afteryes', 'power on to the desktop session', marks.get('OTHERSYS-TEST SESSION-UP', secs))
            rec.check('afteryes', 'without BootNext, the firmware now starts Ubuntu',
                      checks.check_marker(t, 'OTHERSYS-TEST SESSION-UP'))
            screens_not_mok(rec, 'afteryes')
            windows_checks(rec, 'afteryes', work, layout, before)

        if 'moknegative' in phases and ub:
            neg = os.path.join(work, 'vars-mok.fd')
            shutil.copyfile(os.path.join(work, 'vars.fd'), neg)
            efivarstore.plant(neg, 'MokNew', SHIM_GUID, os.urandom(64))
            efivarstore.plant(neg, 'MokAuth', SHIM_GUID, os.urandom(32))
            rec.check('moknegative', 'the firmware check sees a planted MOK request (must fail)',
                      checks.check_no_mok(vars_live(neg)), expect='fail')
            vm = VM(rec, work, 'moknegative', vars_fd=neg)
            vm.run(timeout=45, every=5)
            vm.kill()
            shots = sorted(f for f in os.listdir(os.path.join(out, 'shots')) if f.startswith('moknegative-'))
            bad = []
            for f in shots:
                bad += checks.check_not_mokmanager(os.path.join(out, 'shots', f))
            rec.check('moknegative', 'the screen check sees MokManager\'s blue screen (must fail)',
                      bad, expect='fail')


def finish(rec, work, plan):
    rec.write()
    bad = [r for r in rec.results if not r['ok']]
    rec.log('%d checks, %d not as expected' % (len(rec.results), len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
