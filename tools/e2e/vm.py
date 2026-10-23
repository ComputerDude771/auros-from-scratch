"""vm.py -- one QEMU machine for the end-to-end test, and the host around it.

    Recorder   numbered screenshots, a log, and how long each phase took
    Hub        the HTTP server QEMU's guest reaches as http://10.0.2.2:8000:
               the website and installer a person downloads, and the
               channel to the in-guest UI helper (guest/helper.ps1)
    Machine    QEMU (q35, OVMF with Secure Boot and Microsoft's keys, a
               TPM 2.0 from swtpm, an AHCI SSD, e1000e, std VGA, a USB
               tablet), driven over QMP: screendumps, keys, clicks

The machine's firmware variables live in a file that persists across
every reboot of the guest (BootNext is written there by the installer and
read by the firmware on the next start), and the guest's own reboots are
QEMU resets, so nothing is restarted from outside the machine.
"""
import contextlib
import hashlib
import http.server
import json
import os
import re
import socket
import subprocess
import tempfile
import threading
import time
import urllib.parse

import numpy as np
from PIL import Image


HERE = os.path.dirname(os.path.abspath(__file__))
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.ms.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.ms.fd"
ACCEL = os.environ.get("E2E_ACCEL", "kvm")      # tcg only to try this file out


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")[:60] or "shot"


def fmt_dur(s):
    s = int(s)
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}" if s >= 3600 else f"{s // 60}:{s % 60:02d}"


class Failure(Exception):
    """The run cannot go on; the message says where and why."""


# ── the record ───────────────────────────────────────────────────────

class Recorder:
    def __init__(self, out):
        self.out = out
        self.shots = os.path.join(out, "shots")
        os.makedirs(self.shots, exist_ok=True)
        self.t0 = time.time()
        self.lock = threading.Lock()
        existing = [f for f in os.listdir(self.shots) if f[:4].isdigit()]
        self.n = max([int(f[:4]) for f in existing], default=0)
        self.phases = []
        self.current = "start"
        self.logf = open(os.path.join(out, "driver.log"), "a", buffering=1)
        self.timings_path = os.path.join(out, "timings.md")

    def log(self, msg):
        line = f"[{time.strftime('%H:%M:%S')} +{fmt_dur(time.time() - self.t0)}] {msg}"
        with self.lock:
            print(line, flush=True)
            self.logf.write(line + "\n")

    def shot_path(self, label):
        with self.lock:
            self.n += 1
            return os.path.join(self.shots, f"{self.n:04d}-{slug(label)}.png")

    @contextlib.contextmanager
    def phase(self, name):
        start = time.time()
        prev, self.current = self.current, name
        self.log(f"=== {name}")
        rec = {"name": name, "start": start, "end": None, "result": "running"}
        self.phases.append(rec)
        try:
            yield rec
            rec["result"] = "ok"
        except BaseException as e:
            rec["result"] = f"FAILED: {e}"
            raise
        finally:
            rec["end"] = time.time()
            self.current = prev
            self.log(f"=== {name}: {rec['result']} after {fmt_dur(rec['end'] - start)}")
            self.write_timings()

    def write_timings(self):
        rows = ["| phase | started | took | result |", "|---|---|---|---|"]
        for p in self.phases:
            took = fmt_dur((p["end"] or time.time()) - p["start"])
            rows.append(f"| {p['name']} | +{fmt_dur(p['start'] - self.t0)} | {took} | "
                        f"{p['result'].replace('|', '/')} |")
        with open(self.timings_path, "w") as f:
            f.write("Wall-clock time of each phase (this run's record).\n\n")
            f.write("\n".join(rows) + "\n")


# ── the host's web server, and the channel to the guest's helper ─────

class Hub:
    def __init__(self, rec, www=None, port=8000):
        self.rec = rec
        self.www = www or os.path.join(rec.out, "www-empty")
        os.makedirs(self.www, exist_ok=True)
        self.cond = threading.Condition()
        self.events = []          # (time, name, body)
        self.cmds = []            # questions waiting for the helper
        self.results = {}
        self.next_id = 1
        self.helper_seen = 0.0
        hub = self
        evlog = open(os.path.join(rec.out, "guest-events.log"), "a", buffering=1)

        class H(http.server.SimpleHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def __init__(self, *a, **k):
                super().__init__(*a, directory=hub.www, **k)

            def log_message(self, fmt, *args):
                if not self.path.startswith("/e2e/cmd"):
                    evlog.write(f"{time.strftime('%H:%M:%S')} http {fmt % args}\n")

            def _send(self, code, body=b"", ctype="application/json"):
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if body:
                    self.wfile.write(body)

            def do_GET(self):
                u = urllib.parse.urlparse(self.path)
                if u.path == "/e2e/helper.ps1":
                    with open(os.path.join(HERE, "guest", "helper.ps1"), "rb") as f:
                        return self._send(200, f.read(), "text/plain; charset=utf-8")
                if u.path == "/e2e/cmd":
                    hub.helper_seen = time.time()
                    deadline = time.time() + 20
                    with hub.cond:
                        while not hub.cmds and time.time() < deadline:
                            hub.cond.wait(deadline - time.time())
                        c = hub.cmds.pop(0) if hub.cmds else None
                    if c is None:
                        return self._send(204)
                    return self._send(200, json.dumps(c).encode())
                if u.path.startswith("/e2e/"):
                    return self._send(404)
                return super().do_GET()

            def do_POST(self):
                u = urllib.parse.urlparse(self.path)
                q = urllib.parse.parse_qs(u.query)
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                text = body.decode("utf-8", "replace")
                if u.path == "/e2e/event":
                    name = q.get("name", ["?"])[0]
                    evlog.write(f"{time.strftime('%H:%M:%S')} event {name}\n{text}\n")
                    hub.rec.log(f"guest event: {name}")
                    with hub.cond:
                        hub.events.append((time.time(), name, text))
                        hub.cond.notify_all()
                    return self._send(204)
                if u.path == "/e2e/result":
                    try:
                        res = json.loads(text)
                    except ValueError:
                        res = {"ok": False, "error": "unreadable result", "raw": text[:500]}
                    with hub.cond:
                        hub.results[int(q.get("id", ["0"])[0])] = res
                        hub.cond.notify_all()
                    return self._send(204)
                return self._send(404)

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        rec.log(f"hub: serving {www or '(nothing)'} on 127.0.0.1:{port} (10.0.2.2:{port} in the guest)")

    def event(self, name, since=0.0):
        with self.cond:
            for t, n, body in self.events:
                if n == name and t >= since:
                    return body
        return None

    def wait_event(self, name, timeout, since=0.0, alive=None):
        deadline = time.time() + timeout
        while time.time() < deadline:
            b = self.event(name, since)
            if b is not None:
                return b
            if alive and not alive():
                raise Failure(f"the machine stopped while waiting for '{name}'")
            with self.cond:
                self.cond.wait(5)
        return None

    def helper_alive(self):
        return time.time() - self.helper_seen < 60

    def ask(self, op, timeout=90, **kw):
        """Ask the in-guest helper; None if it does not answer in time."""
        with self.cond:
            cid = self.next_id
            self.next_id += 1
            self.cmds.append(dict(kw, op=op, id=cid))
            self.cond.notify_all()
        deadline = time.time() + timeout
        with self.cond:
            while cid not in self.results and time.time() < deadline:
                self.cond.wait(deadline - time.time())
            res = self.results.pop(cid, None)
            if res is None:     # nobody took it: do not leave it for later
                self.cmds = [c for c in self.cmds if c["id"] != cid]
        return res


# ── QMP ──────────────────────────────────────────────────────────────

class QMP:
    def __init__(self, path, rec, timeout=60, alive=None):
        self.rec = rec
        deadline = time.time() + timeout
        while True:
            self.sock = socket.socket(socket.AF_UNIX)
            try:
                self.sock.connect(path)
                break
            except OSError:
                self.sock.close()
                if time.time() > deadline or (alive and not alive()):
                    raise Failure(f"QMP socket {path} never answered")
                time.sleep(0.5)
        self.f = self.sock.makefile("rwb", buffering=0)
        self.lock = threading.Lock()
        self.events = []
        self._read()                                 # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        line = self.f.readline()
        if not line:
            raise Failure("QMP connection closed (QEMU exited)")
        return json.loads(line)

    def cmd(self, name, **args):
        with self.lock:
            msg = {"execute": name}
            if args:
                msg["arguments"] = args
            self.f.write((json.dumps(msg) + "\n").encode())
            while True:
                r = self._read()
                if "event" in r:
                    self.events.append((time.time(), r["event"], r.get("data")))
                    self.rec.log(f"qmp event: {r['event']} {json.dumps(r.get('data') or {})}")
                    continue
                if "error" in r:
                    raise Failure(f"QMP {name}: {r['error'].get('desc')}")
                return r.get("return")


# US layout: character -> (qcode, shifted)
_KEYS = {" ": ("spc", 0), "\n": ("ret", 0), "\t": ("tab", 0), "-": ("minus", 0),
         "_": ("minus", 1), "=": ("equal", 0), "+": ("equal", 1), ".": ("dot", 0),
         ",": ("comma", 0), "/": ("slash", 0), "?": ("slash", 1), ":": ("semicolon", 1),
         ";": ("semicolon", 0), "'": ("apostrophe", 0), '"': ("apostrophe", 1),
         "\\": ("backslash", 0), "!": ("1", 1), "@": ("2", 1), "#": ("3", 1),
         "&": ("7", 1), "(": ("9", 1), ")": ("0", 1), "*": ("8", 1), "%": ("5", 1)}


class Machine:
    def __init__(self, rec, vmdir, disk, vars_fd, tpm_dir, cdroms=(), ram=6144,
                 cpus=4, xres=1280, yres=800, name="auros-e2e", throttle=""):
        self.rec, self.dir = rec, vmdir
        self.disk, self.vars, self.tpm_dir = disk, vars_fd, tpm_dir
        self.cdroms, self.ram, self.cpus = list(cdroms), ram, cpus
        self.xres, self.yres, self.name = xres, yres, name
        # "bps=104857600,iops=120": the drive limited to a hard disk's
        # speed (QEMU's block throttling), for the wall clock on one
        self.throttle = "".join(f",throttling.{k}-total={v}" for k, v in
                                (kv.split("=") for kv in throttle.split(",") if kv))
        self.qmp = None
        self.proc = self.swtpm = None
        self.size = (xres, yres)
        self.last = None              # the last screendump, as an array
        self._auto = None
        self._auto_every = 30
        self._last_saved = None
        self._last_saved_t = 0
        self.pointer = (xres // 2, yres // 2)
        self._last_input = time.time()
        self.hands = threading.RLock()      # one gesture at a time

    # -- life cycle --
    def start(self):
        os.makedirs(self.tpm_dir, exist_ok=True)
        # sockets in a short path: a Unix socket's name is at most 107 bytes
        sockdir = tempfile.mkdtemp(prefix="e2e-", dir="/tmp")
        tsock = os.path.join(sockdir, "swtpm.sock")
        qsock = os.path.join(sockdir, "qmp.sock")
        self.swtpm = subprocess.Popen(
            ["swtpm", "socket", "--tpm2", "--tpmstate", f"dir={self.tpm_dir}",
             "--ctrl", f"type=unixio,path={tsock}", "--terminate",
             "--log", f"file={os.path.join(self.rec.out, 'swtpm.log')},level=1"])
        for _ in range(100):
            if os.path.exists(tsock):
                break
            time.sleep(0.1)
        cmd = [
            "qemu-system-x86_64", "-name", self.name, "-nodefaults",
            "-machine", f"q35,smm=on,accel={ACCEL}",
            # S4 exists, as on a real PC, so Windows' Fast Startup is on by
            # its own default (hibernation needs it). S3 is irrelevant here.
            "-global", "ICH9-LPC.disable_s3=1", "-global", "ICH9-LPC.disable_s4=0",
            # No VMX/SVM: Windows must not start its own hypervisor (VBS)
            # three levels deep. The hv_* hints only make it faster.
            "-cpu", "host,-vmx,-svm,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time,"
                    "hv_vpindex,hv_synic,hv_stimer" if ACCEL == "kvm" else "max",
            "-smp", str(self.cpus), "-m", str(self.ram),
            "-global", "driver=cfi.pflash01,property=secure,value=on",
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={self.vars}",
            "-chardev", f"socket,id=chrtpm,path={tsock}",
            "-tpmdev", "emulator,id=tpm0,chardev=chrtpm", "-device", "tpm-tis,tpmdev=tpm0",
            # q35's own AHCI controller; the disk says it is an SSD
            "-drive", f"id=disk0,if=none,file={self.disk},format=qcow2,cache=unsafe,"
                      f"discard=unmap,detect-zeroes=unmap{self.throttle}",
            "-device", "ide-hd,drive=disk0,bus=ide.0,rotation_rate=1,bootindex=1,"
                       "serial=E2ESSD0001,model=E2E SATA SSD",
            "-netdev", "user,id=n0",
            "-device", "e1000e,netdev=n0,mac=52:54:00:e2:e0:01,romfile=",
            "-device", f"VGA,edid=on,xres={self.xres},yres={self.yres}",
            "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet,bus=xhci.0",
            "-rtc", "base=utc,driftfix=slew",
            "-qmp", f"unix:{qsock},server=on,wait=off",
            "-serial", f"file:{os.path.join(self.rec.out, 'serial.log')}",
            "-display", "none",
        ]
        for i, iso in enumerate(self.cdroms):
            cmd += ["-drive", f"id=cd{i},if=none,media=cdrom,readonly=on,file={iso}",
                    "-device", f"ide-cd,drive=cd{i},bus=ide.{i + 1}" +
                    (",bootindex=2" if i == 0 else "")]
        self.rec.log("qemu: " + " ".join(cmd))
        self.qlog = open(os.path.join(self.rec.out, "qemu.log"), "a")
        self.proc = subprocess.Popen(cmd, stdout=self.qlog, stderr=subprocess.STDOUT)
        try:
            self.qmp = QMP(qsock, self.rec, alive=self.alive)
        except Failure:
            self.qlog.flush()
            with open(self.qlog.name) as f:
                raise Failure("QEMU did not start: " + f.read()[-1500:])
        self.rec.log(f"qemu: running, pid {self.proc.pid}")

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def wait_exit(self, timeout):
        try:
            self.proc.wait(timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    def stop(self, how="powerdown", timeout=600):
        """Shut down: ACPI power button (what Windows does on a press of
        it is its own setting), then QMP quit, then kill."""
        self.auto_shots(None)
        if not self.alive():
            return
        if how == "powerdown":
            try:
                self.qmp.cmd("system_powerdown")
            except Failure:
                pass
            if self.wait_exit(timeout):
                self.rec.log("qemu: the guest switched itself off")
                return
            self.rec.log("qemu: still running after the power button; quitting")
        try:
            self.qmp.cmd("quit")
        except Failure:
            pass
        if not self.wait_exit(30):
            self.proc.kill()
            self.proc.wait()

    # -- eyes --
    def screendump(self):
        # one file per thread: the periodic shots run beside the driver
        p = os.path.join(self.dir, f"screen-{threading.get_ident()}.ppm")
        self.qmp.cmd("screendump", filename=p)
        img = np.asarray(Image.open(p).convert("RGB"))
        self.last = img
        self.size = (img.shape[1], img.shape[0])
        return img

    def save(self, img, label):
        path = self.rec.shot_path(label)
        Image.fromarray(img).save(path, optimize=False, compress_level=6)
        self._last_saved, self._last_saved_t = img, time.time()
        return path

    def shot(self, label):
        """A screenshot kept as evidence, named by what it shows."""
        img = self.screendump()
        path = self.save(img, label)
        self.rec.log(f"shot {os.path.basename(path)}")
        return img

    def auto_shots(self, every):
        """Keep a screenshot every `every` seconds whenever the screen has
        changed (and at least every five minutes); None stops it."""
        self._auto_every = every
        if every and not self._auto:
            self._auto = threading.Thread(target=self._auto_loop, daemon=True)
            self._auto.start()

    def _auto_loop(self):
        while self._auto_every and self.alive():
            try:
                # Someone is at this PC: a pointer that has not moved for two
                # minutes moves by one pixel and back, so Windows' display
                # timeout never blanks what the test is looking at.
                if time.time() - self._last_input > 120:
                    with self.hands:
                        x, y = self.pointer
                        self.move(x + 1, y)
                        self.move(x, y)
                img = self.screendump()
                prev = self._last_saved
                changed = (prev is None or prev.shape != img.shape or
                           np.abs(prev[::4, ::4].astype(np.int16) -
                                  img[::4, ::4]).mean() > 0.6)
                if changed or time.time() - self._last_saved_t > 300:
                    self.save(img, "auto-" + self.rec.current)
            except Exception as e:      # a missed frame is not a failure
                self.rec.log(f"auto shot: {e}")
            time.sleep(self._auto_every or 1)
        self._auto = None

    # -- hands --
    def _abs(self, x, y):
        w, h = self.size
        return (int(round(x * 32767 / max(1, w - 1))), int(round(y * 32767 / max(1, h - 1))))

    def move(self, x, y):
        self.pointer = (x, y)
        self._last_input = time.time()
        ax, ay = self._abs(x, y)
        self.qmp.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}])

    def click(self, x, y, button="left", double=False, label=None):
        """Move the tablet there, press and release. With a label, a
        screenshot is kept first, with a marker where the click goes."""
        if label:
            img = self.screendump()
            mark = img.copy()
            x0, y0 = int(x), int(y)
            for d in range(-9, 10):
                for yy, xx in ((y0 + d, x0), (y0, x0 + d)):
                    if 0 <= yy < mark.shape[0] and 0 <= xx < mark.shape[1]:
                        mark[yy, xx] = (255, 0, 255)
            path = self.save(mark, "click-" + label)
            self.rec.log(f"click ({x},{y}) {label}  [{os.path.basename(path)}]")
        with self.hands:
            self.move(x, y)
            time.sleep(0.15)
            for _ in range(2 if double else 1):
                self.qmp.cmd("input-send-event", events=[
                    {"type": "btn", "data": {"down": True, "button": button}}])
                time.sleep(0.08)
                self.qmp.cmd("input-send-event", events=[
                    {"type": "btn", "data": {"down": False, "button": button}}])
                time.sleep(0.12)

    def scroll(self, x, y, clicks):
        """Mouse wheel: positive scrolls down."""
        self.move(x, y)
        btn = "wheel-down" if clicks > 0 else "wheel-up"
        for _ in range(abs(clicks)):
            self.qmp.cmd("input-send-event", events=[
                {"type": "btn", "data": {"down": True, "button": btn}}])
            self.qmp.cmd("input-send-event", events=[
                {"type": "btn", "data": {"down": False, "button": btn}}])
            time.sleep(0.05)

    def key(self, *keys, label=None):
        """Press keys together: key("alt", "y"), key("ret")."""
        if label:
            self.shot("key-" + label)
            self.rec.log(f"key {'+'.join(keys)}  ({label})")
        self._last_input = time.time()
        self.qmp.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys],
                     **{"hold-time": 90})
        time.sleep(0.12)

    def type(self, text, label=None):
        if label:
            self.rec.log(f"type {text!r}  ({label})")
        for ch in text:
            if ch.isalpha() and ch.isascii():
                keys = ["shift", ch.lower()] if ch.isupper() else [ch]
            elif ch.isdigit():
                keys = [ch]
            else:
                q, sh = _KEYS[ch]
                keys = ["shift", q] if sh else [q]
            self.qmp.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys])
            time.sleep(0.06)


# ── files ────────────────────────────────────────────────────────────

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(cmd, rec=None, check=True, **kw):
    if rec:
        rec.log("$ " + (cmd if isinstance(cmd, str) else " ".join(cmd)))
    r = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, **kw)
    if rec and (r.stdout.strip() or r.stderr.strip()):
        rec.log((r.stdout + r.stderr).strip()[-4000:])
    if check and r.returncode != 0:
        raise Failure(f"command failed ({r.returncode}): {cmd}")
    return r
