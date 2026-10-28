#!/usr/bin/env python3
"""e2e.py -- AurOS installed from real Windows, end to end, in a VM.

    e2e.py windows OUT VMDIR --iso WIN.iso --answer ANSWER.iso
        Windows Setup, unattended, to the desktop; then shut down. VMDIR
        then holds the installed machine (disk, firmware variables, TPM).
    e2e.py flow OUT GOLDEN WORK
        Start that machine (a copy-on-write layer over it in WORK), and do
        what a person does: open the website in Edge, download the
        installer, open it, answer Windows' warnings, go through the
        wizard, restart, watch AurOS install itself, say it works, then
        get a program from the store.
    e2e.py inspect OUT WORK
        After the flow: read the disk with the machine off (did the
        program really land on AurOS's disk?).

Everything a person would press is pressed through QEMU's keyboard and
USB tablet, and every press is preceded by a screenshot (shots/NNNN-click-*
marks where it went). The settings that change between builds are the
environment variables below; tools/e2e/README.md has the rest.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vision                                                    # noqa: E402
from vm import (Failure, Hub, Machine, Recorder, OVMF_VARS,      # noqa: E402
                run)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
E = os.environ.get

# ── what to install, and from where ──────────────────────────────────
# The default: the test build published on branch image-desktop of the
# repository this runs for (tools/repo.sh: AUROS_REPO, else
# GITHUB_REPOSITORY, else the origin remote). e2e.yml always sets it.
INSTALLER_URL = E("INSTALLER_URL") or (
    "https://github.com/"
    + subprocess.run(["sh", os.path.join(REPO, "tools", "repo.sh")], check=True,
                     stdout=subprocess.PIPE, text=True).stdout.strip()
    + "/raw/image-desktop/AurOS-Installer-test.exe")
INSTALLER_SHA256 = E("INSTALLER_SHA256", "")
# A file on the runner instead (an artifact from an earlier job): served
# from this host and the website's download links are pointed at it.
INSTALLER_PATH = E("INSTALLER_PATH", "")
# The file's name is whatever the link ends in (AurOS-Installer-test.exe,
# AurOS-Setup.exe...); Edge saves it under that name.
INSTALLER_NAME = os.path.basename(INSTALLER_PATH or INSTALLER_URL.split("?")[0])
INSTALLER_STEM = re.sub(r"\.exe$", "", INSTALLER_NAME, flags=re.I)
# The website a person starts from. Served from the runner; "none" gives
# a one-link page instead.
WEBSITE_DIR = E("WEBSITE_DIR") or os.path.join(REPO, "website")
WEBSITE_PAGE = E("WEBSITE_PAGE", "index.html")
# The download link's name, as UI Automation reads it from Edge: the
# website's "Get AurOS" button (an older page said "Download the ...").
DOWNLOAD_LINK = E("DOWNLOAD_LINK", r"(?i)^\s*get aurOS\b|download.*(installer|aurOS)")

# ── the store, after AurOS works (M3) ────────────────────────────────
STORE_NAME = E("STORE_NAME", "Get more programs")    # the app list's name for it
STORE_QUERY = E("STORE_QUERY", "VLC")                 # typed into its search
STORE_RESULT = E("STORE_RESULT", "VLC")               # the result to open
STORE_INSTALL = E("STORE_INSTALL", "Install")         # the button that installs
STORE_OPEN = E("STORE_OPEN", "Open|Launch|Press here to open")  # appears when installed
APP_FILES = E("APP_FILES", "usr/bin/vlc var/lib/flatpak/app/org.videolan.VLC")

# ── which PC (tools/e2e/README.md, "Variants") ──────────────────────
VARIANT = E("E2E_VARIANT", "base")
DISK_GB = int(E("E2E_DISK_GB", "128"))
DISK_THROTTLE = E("E2E_DISK_THROTTLE", "")     # e.g. bps=104857600,iops=120

# What the installer must say, and do, on each variant's PC (its stops
# and fixes: src/aurbridge/preflight.c, fix_win.c; selftest.py checks
# each page pattern still matches both titles of its stop). Without this a
# variant whose PC came out wrong (too much room left, encryption not
# on) would pass by testing the plain PC again.
#   page:     a regular expression some wizard page must show
#   fix:      "Fix these for me" was pressed
#   restarts: Windows restarted for the fix, before the install
VARIANT_EXPECT = {
    "smalldisk": {"page": r"enough free space|free space on drive", "fix": True, "restarts": 0},
    "bitlocker": {"page": r"is encrypted|encryption off", "fix": True, "restarts": 0},
    "pending":   {"page": r"waiting (to|for a) restart|finish an update", "fix": True, "restarts": 1},
}

HUB = "http://10.0.2.2:8000"
DEADLINE = time.time() + 60 * float(E("E2E_MINUTES", "300"))


def left():
    return DEADLINE - time.time()


# ════════════════════════════════════════════════════════════════════
#  M1: Windows
# ════════════════════════════════════════════════════════════════════

def pass_cd_prompt(m, timeout=180):
    """Windows' DVD says "Press any key to boot from CD or DVD" for about
    five seconds, then gives up and the firmware moves on. Press a key
    exactly then and never after: Setup starts seconds later with its
    Cancel button focused, and a stray Enter there asks whether to quit."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        img = m.screendump()
        lit = (img.max(axis=2) > 60).mean()
        if lit > 0.3:           # a full screen of colour: past the prompt
            return False
        if lit > 0.0005 and vision.find_text(vision.ocr(img), "Press any key"):
            m.save(img, "cd-prompt")
            m.key("ret", label="boot from the DVD")
            time.sleep(1.5)
            if not vision.find_text(vision.ocr(m.screendump()), "Press any key"):
                return True
            continue
        time.sleep(0.3)
    return False


def cmd_windows(a):
    rec = Recorder(a.out)
    hub = Hub(rec)
    os.makedirs(a.vm, exist_ok=True)
    disk, vars_fd, tpm = (os.path.join(a.vm, n) for n in ("win.qcow2", "vars.fd", "tpm"))
    run(["qemu-img", "create", "-f", "qcow2", disk, f"{DISK_GB}G"], rec)
    shutil.copyfile(OVMF_VARS, vars_fd)
    m = Machine(rec, a.vm, disk, vars_fd, tpm, cdroms=[a.iso, a.answer])
    ok = False
    try:
        with rec.phase("windows: firmware to Setup"):
            m.start()
            m.auto_shots(2)
            if not pass_cd_prompt(m):
                raise Failure("the DVD's 'Press any key' prompt never showed")
            m.shot("setup-loading")
        m.auto_shots(30)
        with rec.phase("windows: Setup, to the first logon"):
            facts = hub.wait_event("firstlogon", timeout=min(left(), 120 * 60), alive=m.alive)
            if facts is None:
                raise Failure("Windows did not reach its first logon")
        with open(os.path.join(a.out, "windows-facts.json"), "w") as f:
            f.write(facts)
        for k, v in json.loads(facts).items():
            rec.log(f"windows {k}: {str(v).strip()}")
        with rec.phase("windows: settle on the desktop"):
            hub.wait_event("helper-up", 600, alive=m.alive)
            time.sleep(90)
            m.shot("windows-desktop")
            w = hub.ask("windows")
            rec.log("windows on screen: " + json.dumps(
                [(x["name"], x["proc"]) for x in (w or {}).get("windows", [])]))
        with rec.phase("windows: shut down (power button)"):
            m.stop("powerdown", 900)
        ok = True
    except Failure as e:
        rec.log(f"FAILED: {e}")
        if m.alive():
            m.shot("failure")
    finally:
        m.stop("quit", 5)
        rec.write_timings()
    return 0 if ok else 1


# ════════════════════════════════════════════════════════════════════
#  M2: from the website to "does this work?"
# ════════════════════════════════════════════════════════════════════

class Flow:
    def __init__(self, a):
        self.a = a
        self.rec = Recorder(a.out)
        self.www = self.prepare_website()
        self.hub = Hub(self.rec, self.www)
        os.makedirs(a.work, exist_ok=True)
        self.disk = os.path.join(a.work, "win.qcow2")
        self.vars = os.path.join(a.work, "vars.fd")
        self.tpm = os.path.join(a.work, "tpm")
        if not os.path.exists(self.disk):
            run(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2", "-b",
                 os.path.join(a.golden, "win.qcow2"), self.disk], self.rec)
            shutil.copyfile(os.path.join(a.golden, "vars.fd"), self.vars)
            shutil.copytree(os.path.join(a.golden, "tpm"), self.tpm)
        self.m = Machine(self.rec, a.work, self.disk, self.vars, self.tpm,
                         throttle=DISK_THROTTLE)
        self.rec.log(f"variant {VARIANT}: disk {DISK_GB} GB, throttle '{DISK_THROTTLE or 'none'}'")
        self.wizard_pages = []      # the text of each page a button was pressed on
        self.fix_presses = 0
        self.fix_restarts = 0

    def check_variant(self):
        """After the wizard: the variant's PC got the stop it was made
        for, and the installer's own fix cleared it."""
        want = VARIANT_EXPECT.get(VARIANT)
        if not want:
            return
        seen = [t for t in self.wizard_pages if re.search(want["page"], t, re.I)]
        self.log(f"variant {VARIANT}: pages matching /{want['page']}/: {len(seen)}, "
                 f"'Fix these for me' pressed {self.fix_presses}, restarts for a fix {self.fix_restarts}")
        if not seen:
            raise Failure(f"variant {VARIANT}: no wizard page said /{want['page']}/, so this PC was "
                          "not the one the variant claims (or the installer did not notice); pages: " +
                          " || ".join(t[:200] for t in self.wizard_pages))
        if want["fix"] and not self.fix_presses:
            raise Failure(f"variant {VARIANT}: the installer went on without its fix being pressed")
        if self.fix_restarts < want["restarts"]:
            raise Failure(f"variant {VARIANT}: the fix should have restarted Windows, and it did not")

    # -- the website a person starts from --
    def prepare_website(self):
        www = os.path.join(self.a.out, "www")
        shutil.rmtree(www, ignore_errors=True)
        if WEBSITE_DIR != "none" and os.path.isdir(WEBSITE_DIR):
            shutil.copytree(WEBSITE_DIR, www)
        else:
            os.makedirs(www)
            with open(os.path.join(www, WEBSITE_PAGE), "w") as f:
                f.write(f'<!doctype html><title>AurOS</title><h1>AurOS</h1>'
                        f'<p><a href="{INSTALLER_URL}">Download the AurOS installer</a></p>')
        target = INSTALLER_URL
        if INSTALLER_PATH:
            shutil.copyfile(INSTALLER_PATH, os.path.join(www, INSTALLER_NAME))
            target = f"{HUB}/{INSTALLER_NAME}"
        # Whatever the pages link to as the installer (any AurOS .exe),
        # link to this one.
        pat = re.compile(r'href="[^"]*/AurOS[^"/]*\.exe"')
        n = 0
        for dp, _, fs in os.walk(www):
            for fn in fs:
                if fn.endswith(".html"):
                    p = os.path.join(dp, fn)
                    s = open(p, encoding="utf-8").read()
                    s2, k = pat.subn(f'href="{target}"', s)
                    if k:
                        open(p, "w", encoding="utf-8").write(s2)
                        n += k
        self_log = os.path.join(self.a.out, "website.txt")
        with open(self_log, "w") as f:
            f.write(f"website from {WEBSITE_DIR}; {n} installer link(s) -> {target}\n")
        return www

    # -- small helpers --
    def log(self, msg):
        self.rec.log(msg)

    def elements(self, match, types, max=400, quiet=False):
        r = self.hub.ask("elements", match=match, types=types, max=max, timeout=120)
        if not r or not r.get("ok"):
            self.log(f"helper: no elements ({(r or {}).get('error', 'no answer')})")
            return []
        els = r.get("elements") or []
        if isinstance(els, dict):
            els = [els]
        if not quiet:
            with open(os.path.join(self.a.out, "ui-elements.log"), "a") as f:
                f.write(f"--- {time.strftime('%H:%M:%S')} {match} {types} ({r.get('ms')} ms)\n")
                for e in els:
                    f.write(f"{e.get('type')}\t{e.get('name')!r}\t{e.get('rect')}\t"
                            f"toggle={e.get('toggle')}\t{'page' if self.is_page(e) else 'ui'}\t"
                            f"{e.get('window')!r}\n")
        return els

    def windows(self):
        r = self.hub.ask("windows", timeout=60)
        ws = (r or {}).get("windows") or []
        return [ws] if isinstance(ws, dict) else ws

    def guest(self, script, timeout=120):
        r = self.hub.ask("run", script=script, timeout=timeout)
        out = (r or {}).get("output") or (r or {}).get("error") or "(no answer)"
        with open(os.path.join(self.a.out, "guest-diagnostics.log"), "a") as f:
            f.write(f"--- {time.strftime('%H:%M:%S')} {script}\n{out}\n")
        return out

    def click_el(self, e, why):
        x, y, w, h = e["rect"]
        self.m.click(x + w // 2, y + h // 2, label=why)

    @staticmethod
    def is_page(e):
        """Part of a website: inside a web document that fills most of a
        window. Edge's downloads flyout and menus are small documents."""
        return (e.get("doc") or 0) >= 250000

    @staticmethod
    def pick(els, name, types=None, page=None):
        """The first enabled element whose name matches; page=True only
        a web page's own, page=False only the browser's or Windows'."""
        rx = re.compile(name, re.I)
        for e in els:
            if page is not None and Flow.is_page(e) != page:
                continue
            if (types is None or e.get("type") in types) and rx.search(e.get("name") or ""):
                if e.get("enabled", True):
                    return e
        return None

    def ocr_find(self, phrase, img=None, region=None, last=False):
        img = self.m.screendump() if img is None else img
        words = vision.ocr(img, region)
        return vision.find_text(words, phrase, last=last), words

    def wait_text(self, phrases, timeout, every=10, fail=()):
        """OCR the screen until one of `phrases` shows; returns (phrase,
        box, words). A phrase in `fail` showing is a Failure."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.m.alive():
                raise Failure("the machine switched off")
            img = self.m.screendump()
            words = vision.ocr(img)
            for p in fail:
                if vision.find_text(words, p):
                    self.m.save(img, "failure-" + p)
                    raise Failure(f"the screen says '{p}': {vision.text_of(words)[:600]}")
            for p in phrases:
                b = vision.find_text(words, p)
                if b:
                    return p, b, words
            time.sleep(every)
        return None, None, None

    # -- 1. Windows, as the person finds it --
    def boot_windows(self):
        with self.rec.phase("windows: power on to the desktop"):
            t = time.time()
            self.m.start()
            self.m.auto_shots(10)
            if self.hub.wait_event("helper-up", 25 * 60, since=t, alive=self.m.alive) is None:
                raise Failure("Windows did not reach the desktop (the helper never started)")
            if VARIANT == "pending":
                # the feature that leaves a restart pending is enabled at logon
                r = self.hub.wait_event("pending-ready", 15 * 60, since=t, alive=self.m.alive)
                self.log(f"pending restart prepared: {r}")
            time.sleep(45)
            self.m.shot("windows-desktop")
            # What Windows says about itself, read as the signed-in user
            # (no elevation): the state the installer is about to meet.
            facts = self.guest(
                "$o = Get-CimInstance Win32_OperatingSystem; \"$($o.Caption) $($o.Version)\"; "
                "'DisplayVersion: ' + (Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion').DisplayVersion; "
                "'SecureBoot (UEFISecureBootEnabled): ' + (Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State').UEFISecureBootEnabled; "
                "'Fast Startup (HiberbootEnabled): ' + (Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power').HiberbootEnabled; "
                "'BitLocker on C: (shell, 1=on 2=off 3=encrypting 6=on/suspended): ' + "
                "(New-Object -ComObject Shell.Application).NameSpace('C:').Self.ExtendedProperty('System.Volume.BitLockerProtection'); "
                "'SmartScreen: ' + (Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer' -ErrorAction SilentlyContinue).SmartScreenEnabled; "
                "'Smart App Control (0 off, 1 on, 2 evaluation): ' + (Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\CI\\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState; "
                "Get-Volume -DriveLetter C | Format-List FileSystem,Size,SizeRemaining; "
                "Get-CimInstance Win32_DiskPartition | Format-Table Index,Type,Size,StartingOffset -AutoSize; "
                "Get-CimInstance Win32_VideoController | Format-List Name,CurrentHorizontalResolution,CurrentVerticalResolution")
            self.log("windows, as the installer will find it:\n" + facts.strip())
            with open(os.path.join(self.a.out, "windows-facts.txt"), "w") as fh:
                fh.write(facts)
        self.m.auto_shots(30)

    # -- 2. Edge: the website, its download link, Edge's own warnings --
    def open_website(self):
        url = f"{HUB}/{WEBSITE_PAGE}"
        with self.rec.phase("edge: open the website"):
            self.m.key("meta_l", "r", label="Run dialog")
            time.sleep(3)
            self.m.type(f"msedge {url}", label="open Edge on the website")
            self.m.key("ret")
            deadline = time.time() + 180
            while time.time() < deadline:
                ws = [w for w in self.windows() if w.get("proc") == "msedge" and w.get("name")]
                if any("AurOS" in (w.get("name") or "") for w in ws):
                    break
                time.sleep(5)
            else:
                raise Failure("Edge did not show the website: windows " +
                              json.dumps([(w.get('name'), w.get('proc')) for w in self.windows()]))
            time.sleep(8)
            self.m.key("meta_l", "up", label="maximise Edge")
            time.sleep(3)
            self.m.shot("website")

    def click_download(self):
        with self.rec.phase("edge: press the website's download button"):
            # The button as a person finds it: on screen, or further down
            # (End, then the wheel). Any boxes that gate it get ticked.
            for attempt in range(8):
                els = [e for e in self.elements("msedge", ["CheckBox", "Hyperlink", "Button"])
                       if self.is_page(e)]
                boxes = [e for e in els if e.get("type") == "CheckBox" and e.get("toggle") == "Off"]
                if boxes:
                    self.click_el(boxes[0], "website tick: " + (boxes[0].get("name") or "")[:40])
                    time.sleep(1.5)
                    continue
                link = self.pick(els, DOWNLOAD_LINK, ("Hyperlink", "Button"))
                if link:
                    self.click_el(link, "website: " + (link.get("name") or "download")[:40])
                    time.sleep(5)
                    return
                if attempt == 0:
                    self.m.key("end", label="look further down the page")
                else:
                    self.m.scroll(640, 400, 5)
                time.sleep(3)
            raise Failure("no download link on the page (ui-elements.log has what Edge showed)")

    def keep_download(self):
        """Edge's download warnings, answered the way a person who wants
        the file does; returns once the file is downloaded."""
        with self.rec.phase("edge: download, and keep it"):
            deadline = time.time() + 8 * 60
            pressed = []
            seen = None
            while time.time() < deadline:
                els = [e for e in self.elements("msedge", ["Button", "MenuItem", "Hyperlink",
                                                           "ListItem", "Text", "SplitButton"])
                       if not self.is_page(e)]  # never the page's own words
                names = " | ".join((e.get("name") or "")[:120] for e in els)
                if names != seen:
                    self.log("edge shows: " + names[:900])
                    seen = names
                for want, types in (("^Keep anyway$", ("Button", "MenuItem")),
                                    ("^Show more", ("Button", "SplitButton")),
                                    ("^Keep$", ("Button", "MenuItem"))):
                    e = self.pick(els, want, types)
                    if e and pressed.count(want) < 4:
                        self.click_el(e, "edge " + want.strip("^$"))
                        pressed.append(want)
                        time.sleep(3)
                        break
                else:
                    # the newest finished .exe in Downloads (Edge may add " (1)")
                    done = self.guest("$f = Get-ChildItem \"$env:USERPROFILE\\Downloads\" -Filter *.exe | "
                                      "Sort-Object LastWriteTime -Descending | Select-Object -First 1; "
                                      "if ($f) { (Get-FileHash $f.FullName).Hash; $f.Name } else { 'missing' }; "
                                      "Get-ChildItem \"$env:USERPROFILE\\Downloads\" | Format-Table Name,Length")
                    h = done.split()[0].lower() if done.strip() else ""
                    if re.fullmatch(r"[0-9a-f]{64}", h):
                        self.log(f"downloaded {done.split()[1] if len(done.split()) > 1 else '?'}: SHA-256 {h}")
                        if INSTALLER_SHA256 and h != INSTALLER_SHA256.lower():
                            raise Failure(f"the downloaded installer is {h}, expected {INSTALLER_SHA256}")
                        self.m.shot("downloaded")
                        return
                    warn = re.search(r"(isn.t commonly downloaded|can.t be downloaded securely|"
                                     r"might be dangerous|download blocked|was blocked|unsafe|"
                                     r"couldn.t verify)", names, re.I)
                    more = self.pick(els, r"^(More actions|See more|More options)", ("Button", "SplitButton", "MenuItem"))
                    if warn and more and pressed.count("more") < 4:
                        self.click_el(more, "edge download item: more actions")
                        pressed.append("more")
                        time.sleep(3)
                        continue
                    if warn and not more:
                        # Edge shows an item's buttons when the pointer is on it
                        item = next((e for e in els if e.get("type") in ("ListItem", "Text", "Button") and
                                     re.search(warn.group(1), e.get("name") or "", re.I)), None)
                        if item and pressed.count("hover") < 6:
                            x, y, w, h = item["rect"]
                            self.m.move(x + w // 2, y + h // 2)
                            pressed.append("hover")
                            time.sleep(2)
                            continue
                    if not warn and pressed.count("flyout") < 3:
                        # the flyout went away: the toolbar's Downloads button
                        tb = self.pick(els, r"^Downloads", ("Button", "SplitButton"))
                        if tb:
                            self.click_el(tb, "edge Downloads button")
                            pressed.append("flyout")
                            time.sleep(3)
                            continue
                    time.sleep(5)
            raise Failure("the download never finished (ui-elements.log has Edge's buttons)")

    # -- 3. open it: SmartScreen, then UAC --
    def open_installer(self):
        with self.rec.phase("windows: open the installer (SmartScreen, UAC)"):
            els = self.elements("msedge", ["Button", "Hyperlink", "ListItem", "MenuItem"])
            op = (self.pick(els, r"^Open file$", page=False) or
                  self.pick(els, re.escape(INSTALLER_STEM), page=False))
            if op:
                self.click_el(op, "edge open file")
            else:
                # The flyout closed: open Downloads (Ctrl+J) and use it there.
                self.m.key("ctrl", "j", label="Edge downloads")
                time.sleep(4)
                els = self.elements("msedge", ["Button", "Hyperlink", "ListItem"])
                # edge://downloads is Edge's own page, so a page is fine here
                op = (self.pick(els, r"^Open file$") or
                      self.pick(els, re.escape(INSTALLER_STEM), ("Hyperlink", "Button")))
                if not op:
                    raise Failure("Edge offers no way to open the download")
                self.click_el(op, "edge open file")
            deadline = time.time() + 6 * 60
            uac_tries = 0
            while time.time() < deadline:
                time.sleep(4)
                ws = self.windows()
                if self.wizard_rect(ws):
                    self.log("the installer's window is open")
                    return
                ss = [w for w in ws if re.search(r"smartscreen", (w.get("proc") or "") + (w.get("name") or ""), re.I)
                      or "Windows protected your PC" in (w.get("name") or "")]
                if ss:
                    els = self.elements("smartscreen|Windows protected|SmartScreen",
                                        ["Button", "Hyperlink", "Text"])
                    e = self.pick(els, r"^Run anyway$") or self.pick(els, r"^More info$")
                    if e:
                        self.click_el(e, "smartscreen " + e["name"])
                        continue
                img = self.m.screendump()
                words = vision.ocr(img)
                if vision.find_text(words, "allow this app to make changes"):
                    # The secure desktop: UI Automation cannot see it, and
                    # neither can anything else on the normal desktop. The
                    # keyboard reaches it: Alt+Y is "Yes".
                    uac_tries += 1
                    self.m.save(img, "uac")
                    if uac_tries <= 2:
                        self.m.key("alt", "y", label="UAC yes (Alt+Y)")
                    else:
                        yes = vision.find_text(words, "Yes", last=True)
                        if yes:
                            self.m.click(*vision.centre(yes), label="UAC Yes")
                        else:
                            self.m.key("left")
                            self.m.key("ret", label="UAC yes (Left, Enter)")
                    continue
                if vision.find_text(words, "Windows protected your PC"):
                    b = vision.find_text(words, "Run anyway") or vision.find_text(words, "More info")
                    if b:
                        self.m.click(*vision.centre(b), label="smartscreen (by OCR)")
                        continue
            raise Failure("the installer's window never opened")

    # -- 4. the wizard --
    def wizard_rect(self, ws=None):
        """The installer's window: its process is the downloaded file, or
        its title names AurOS outside Edge and Explorer."""
        for w in (self.windows() if ws is None else ws):
            proc, name = w.get("proc") or "", w.get("name") or ""
            if not w.get("rect") or w["rect"][2] < 200:
                continue
            if proc.lower().startswith(INSTALLER_STEM.lower()[:10]) or (
                    re.search(r"AurOS", name) and proc.lower() not in
                    ("msedge", "explorer", "searchhost", "shellexperiencehost")):
                return w["rect"]
        return None

    def drive_wizard(self):
        """Press the teal primary button whenever it is enabled; when it
        is not, do what the page asks (tick its boxes, type the word it
        names) or wait. Each press starts a new timed phase named after
        it. Returns right after pressing "Restart now"."""
        clicks = {}
        type_tries = 0
        idle_since = time.time()
        last_label = None
        installing = False          # pressed the button that starts the install
        seen_events = len(self.m.qmp.events)
        last_text, text_since = "", time.time()
        rect = self.wizard_rect()
        stage = [self.rec.phase("wizard: first page")]
        stage[0].__enter__()

        def next_stage(name):
            stage[0].__exit__(None, None, None)
            stage[0] = self.rec.phase(name)
            stage[0].__enter__()

        try:
            while left() > 0:
                if not self.m.alive():
                    raise Failure("the machine switched off during the wizard")
                resets = [e for e in self.m.qmp.events[seen_events:] if e[1] == "RESET"]
                seen_events = len(self.m.qmp.events)
                if resets and installing:
                    # It restarted by itself (the countdown after "Restart
                    # now"), which is what a person who walked away sees.
                    self.log("the machine restarted by itself after the install was prepared")
                    self.restart_clicked = resets[0][0]
                    stage[0].__exit__(None, None, None)
                    return
                if resets:
                    self.log("Windows restarted during the wizard (a fix that needs one?)")
                    self.fix_restarts += 1
                    rect = None
                if rect is None:
                    # Gone: closed, crashed, or Windows is restarting for
                    # a fix and the installer will reopen after sign-in.
                    self.m.shot("wizard-window-gone")
                    gone = time.time()
                    while rect is None and time.time() - gone < 15 * 60:
                        time.sleep(10)
                        rect = self.wizard_rect()
                    if rect is None:
                        w = self.windows()
                        raise Failure("the installer's window went away and did not come back; "
                                      "windows: " + json.dumps([(x.get('name'), x.get('proc')) for x in w]))
                    self.log("the installer's window is back")
                    continue
                img = self.m.screendump()
                btns = vision.find_buttons(img, rect)
                if btns:
                    b = max(btns, key=lambda b: (b[1] + b[3], b[0] + b[2]))
                    label = vision.text_of(vision.ocr(img, b, psm=7)).strip() or "?"
                    if re.search(r"close", label, re.I) and not re.search(r"restart", label, re.I):
                        self.m.save(img, "wizard-ended-with-close")
                        raise Failure("the installer stopped without offering a restart: " +
                                      vision.text_of(vision.ocr(img, rect))[:900])
                    n = clicks.get(label, 0)
                    if not self.wizard_pages or n == 0 or label != last_label:
                        page = vision.text_of(vision.ocr(img, rect))
                        self.wizard_pages.append(page)
                        self.log(f"wizard page before '{label}': {page[:400]}")
                    if re.search(r"fix these", label, re.I):
                        self.fix_presses += 1
                    if label == last_label and n >= 6:
                        self.m.save(img, "wizard-stuck")
                        raise Failure(f"pressed '{label}' six times and the page did not move on: " +
                                      vision.text_of(vision.ocr(img, rect))[:900])
                    self.m.click(*vision.centre(b), label="wizard " + label)
                    if re.search(r"install|erase", label, re.I):
                        installing = True
                    if re.search(r"restart", label, re.I):
                        self.restart_clicked = time.time()
                        stage[0].__exit__(None, None, None)
                        return
                    clicks[label] = n + 1
                    last_label = label
                    idle_since = time.time()
                    next_stage(f"wizard: after '{label}'")
                    time.sleep(3)
                    continue
                boxes = vision.find_unchecked_boxes(img, rect)
                if boxes:
                    self.m.click(*vision.centre(boxes[0]), label="wizard tick box")
                    time.sleep(1.5)
                    if len(vision.find_unchecked_boxes(self.m.screendump(), rect)) >= len(boxes):
                        self.log("the box did not tick")
                    idle_since = time.time()
                    continue
                words = vision.ocr(img, rect)
                text = vision.text_of(words)
                mt = re.search(r"\bType ([A-Z]{3,})\b", text)
                if mt and type_tries < 3 and time.time() - idle_since > (0 if type_tries == 0 else 8):
                    word = mt.group(1)
                    type_tries += 1
                    self.m.key("end", label=f"scroll to where {word} is typed")
                    time.sleep(1.5)
                    img = self.m.screendump()
                    # the input shows the word as its placeholder, in capitals,
                    # below the sentence asking for it (the button says "agree")
                    ph = vision.find_lone_word(vision.ocr(img, rect), word)
                    if ph:
                        self.m.click(*vision.centre(ph), label=f"wizard: the box for {word}")
                    for _ in range(len(word) + 4):
                        self.m.key("backspace")
                    self.m.type(word, label="typed confirmation")
                    time.sleep(1.5)
                    self.m.shot("wizard-typed")
                    idle_since = time.time()
                    continue
                # Busy: words the old wizard shows while it works, or any
                # number on the page moving (percent, MB, a countdown).
                if text != last_text:
                    if re.sub(r"\d", "", text) == re.sub(r"\d", "", last_text):
                        text_since = time.time()     # only numbers moved
                    last_text = text
                busy = (re.search(r"Setting up AurOS|Checking this PC|We only read", text)
                        or time.time() - text_since < 45)
                if not busy and time.time() - idle_since > 60:
                    # Nothing to press, tick or type that can be seen: the
                    # page may be below the fold, or want Enter (the wizard's
                    # Enter is its primary action).
                    nudge = clicks.get("#nudge", 0)
                    clicks["#nudge"] = nudge + 1
                    if nudge % 2 == 0:
                        self.m.key("ret", label="nothing to click: Enter")
                    else:
                        self.m.key("end", label="nothing to click: scroll to the end")
                    idle_since = time.time()
                if time.time() - idle_since > 300 and int(time.time() - idle_since) % 300 < 15:
                    self.log("wizard waiting: " + text[-300:])
                time.sleep(15 if busy else 5)
            raise Failure("ran out of time in the wizard")
        except BaseException:
            stage[0].__exit__(*sys.exc_info())
            raise

    # -- 5. the restart, the staging screen, AurOS --
    def watch_install(self):
        with self.rec.phase("restart: Windows shuts down, firmware starts the installer"):
            self.m.auto_shots(5)
            p, _, words = self.wait_text(["Installing AurOS", "Getting ready", "AurOS is installed",
                                          "Yes, it all works"], 20 * 60, every=5,
                                         fail=["AurOS was not installed"])
            if not p:
                raise Failure("no installer screen within 20 minutes of Restart now")
            self.log(f"first AurOS screen: {p}")
            back = self.hub.event("helper-up", since=self.restart_clicked)
            if back is not None:
                raise Failure("Windows came back instead of the installer")
        self.m.auto_shots(10)
        with self.rec.phase("staging: make room, copy AurOS, check, start it"):
            p, _, words = self.wait_text(["Yes, it all works", "Does it work", "does this work"],
                                         min(left(), 150 * 60), every=15,
                                         fail=["AurOS was not installed"])
            if not p:
                raise Failure("AurOS did not ask whether it works")
            if self.hub.event("helper-up", since=self.restart_clicked) is not None:
                raise Failure("Windows started again during the install")
        self.m.auto_shots(30)

    def answer_yes(self):
        with self.rec.phase("aurOS: say it works"):
            time.sleep(5)
            b, words = self.ocr_find("Yes, it all works")
            if not b:
                raise Failure("the Yes button went away")
            self.m.click(*vision.centre(b), label="AurOS: Yes, it all works")
            time.sleep(15)
            self.m.shot("after-yes")
            words = vision.ocr(self.m.screendump())
            self.log("after yes: " + vision.text_of(words)[:600])

    # -- 6. M3: a program from the store --
    def store(self):
        with self.rec.phase("aurOS: open the store"):
            # Panels left over from the first start (bring my files, close)
            for dismiss in ("Not now", "Close"):
                b, _ = self.ocr_find(dismiss)
                if b:
                    self.m.click(*vision.centre(b), label="dismiss " + dismiss)
                    time.sleep(3)
            # Where a person would look: the screen as it is, the Home
            # card (Home key in the rail layout), the AurOS button at the
            # top left, the Super key.
            b, words = self.ocr_find(STORE_NAME)
            tries = [("key", "home"), ("text", "AurOS"), ("key", "meta_l"), ("key", "esc")]
            while not b and tries:
                kind, what = tries.pop(0)
                if kind == "key":
                    self.m.key(what, label=f"look for {STORE_NAME}: {what}")
                else:
                    t = vision.find_text(words, what)
                    if not t:
                        continue
                    self.m.click(*vision.centre(t), label=f"look for {STORE_NAME}: {what}")
                time.sleep(3)
                b, words = self.ocr_find(STORE_NAME)
            if not b:
                self.m.shot("no-store")
                raise Failure(f"no '{STORE_NAME}' anywhere a person would look: " +
                              vision.text_of(words)[:600])
            self.m.click(*vision.centre(b), label="open " + STORE_NAME)
            p, _, _ = self.wait_text(["Search", "Explore", "Installed", "Updates"], 300, every=10)
            if not p:
                raise Failure("the store did not open")
            self.m.shot("store-open")
        with self.rec.phase("aurOS: find and install " + STORE_QUERY):
            found = None
            for attempt in range(4):
                b, _ = self.ocr_find("Search")
                if b:
                    self.m.click(*vision.centre(b), label="store search")
                else:
                    self.m.key("ctrl", "f", label="store search (Ctrl+F)")
                time.sleep(2)
                self.m.key("ctrl", "a")
                self.m.type(STORE_QUERY, label="search")
                self.m.key("ret")
                # the catalogue can take a minute or two after first going online
                p, found, _ = self.wait_text([STORE_RESULT], 120, every=10)
                if p:
                    break
                self.m.shot("store-no-result-yet")
            if not found:
                raise Failure(f"no '{STORE_RESULT}' in the search results after four searches")
            self.m.click(*vision.centre(found), label="open result " + STORE_RESULT)
            # "Install" exactly: OCR would happily match the "Installed" tab
            deadline = time.time() + 300
            btn = None
            while time.time() < deadline and not btn:
                time.sleep(8)
                btn = vision.find_lone_word(vision.ocr(self.m.screendump()), STORE_INSTALL)
            if not btn:
                self.m.shot("store-no-install")
                raise Failure(f"no '{STORE_INSTALL}' button")
            self.m.click(*vision.centre(btn), label="install")
            done = None
            deadline = time.time() + 45 * 60
            while time.time() < deadline and not done:
                time.sleep(15)
                img = self.m.screendump()
                words = vision.ocr(img)
                for bad in ("Authentication", "Failed to install", "Unable to install"):
                    if vision.find_text(words, bad):
                        self.m.save(img, "store-" + bad)
                        raise Failure(f"the store says '{bad}': {vision.text_of(words)[:500]}")
                for o in STORE_OPEN.split("|"):
                    box = (vision.find_lone_word(words, o) if " " not in o
                           else vision.find_text(words, o))
                    if box:
                        done = (o, box)
                        break
            if not done:
                raise Failure("the install did not finish in 45 minutes")
            self.m.shot("store-installed")
        with self.rec.phase("aurOS: open " + STORE_QUERY):
            self.m.click(*vision.centre(done[1]), label="open the installed program: " + done[0])
            time.sleep(25)
            img = self.m.shot("program-open")
            self.log("on screen: " + vision.text_of(vision.ocr(img))[:500])

    def shutdown(self):
        with self.rec.phase("aurOS: shut down"):
            self.m.stop("powerdown", 300)


def cmd_flow(a):
    f = Flow(a)
    rc = 1
    try:
        f.boot_windows()
        f.open_website()
        f.click_download()
        f.keep_download()
        f.open_installer()
        f.drive_wizard()
        f.check_variant()
        f.watch_install()
        f.answer_yes()
        if E("E2E_STORE", "1") == "1":
            f.store()
        rc = 0
    except Failure as e:
        f.log(f"FAILED: {e}")
        with open(os.path.join(a.out, "FAILED.txt"), "w") as fh:
            fh.write(str(e) + "\n")
        if f.m.alive():
            f.m.shot("failure")
            if f.hub.helper_alive():
                f.guest("Get-ChildItem C:\\AurOS -ErrorAction SilentlyContinue | Format-Table Name,Length; "
                        "Get-Process | Sort-Object CPU -Descending | Select-Object -First 15 Name,Id,CPU | Format-Table")
    finally:
        try:
            if f.m.alive():
                f.shutdown()
        except Failure as e:
            f.log(f"shutdown: {e}")
        f.m.stop("quit", 5)
        f.rec.write_timings()
    return rc


# ════════════════════════════════════════════════════════════════════
#  After the run: what is on the disk
# ════════════════════════════════════════════════════════════════════

def cmd_inspect(a):
    """Read AurOS's root file system with the machine off, read-only.
    Proves the program is on the disk, not just on the screen."""
    rec = Recorder(a.out)
    disk = os.path.join(a.work, "win.qcow2")
    out, found = [], {}
    attached = []
    with rec.phase("inspect: the disk, machine off"):
        dev = "/dev/nbd0"
        run("sudo modprobe nbd max_part=16", rec, check=False)
        if run(["sudo", "qemu-nbd", "--read-only", "--connect", dev, disk], rec,
               check=False).returncode == 0:
            attached.append(("nbd", dev))
            time.sleep(3)
            run(["sudo", "partprobe", dev], rec, check=False)
        else:
            # no nbd in this kernel: a sparse raw copy on a loop device
            raw = os.path.join(a.work, "disk.raw")
            run(["qemu-img", "convert", "-O", "raw", disk, raw], rec)
            dev = run(["sudo", "losetup", "--find", "--show", "--read-only", "-P", raw], rec).stdout.strip()
            attached.append(("loop", dev))
        try:
            out.append(run(["sudo", "sgdisk", "-p", dev], rec, check=False).stdout)
            out.append(run(["lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,PARTLABEL", dev], rec, check=False).stdout)
            mnt = os.path.join(a.work, "mnt")
            os.makedirs(mnt, exist_ok=True)
            base = os.path.basename(dev)
            parts = sorted(p for p in os.listdir("/dev") if p.startswith(base + "p"))
            for p in parts:
                fs = run(["sudo", "blkid", "-o", "value", "-s", "TYPE", "/dev/" + p], rec,
                         check=False).stdout.strip()
                if fs not in ("ext4", "btrfs", "xfs"):
                    continue
                if run(["sudo", "mount", "-o", "ro,noload" if fs == "ext4" else "ro", "/dev/" + p, mnt],
                       rec, check=False).returncode != 0:
                    continue
                try:
                    out.append(f"{p} ({fs}): " + run(["sudo", "ls", mnt], rec, check=False).stdout.replace("\n", " "))
                    for rel in APP_FILES.split():
                        r = run(["sudo", "ls", "-ld", os.path.join(mnt, rel)], rec, check=False)
                        if r.returncode == 0:
                            found[rel] = f"{p}: {r.stdout.strip()}"
                    out.append(run(["sudo", "sh", "-c",
                                    f"tail -n 30 {mnt}/var/log/apt/history.log 2>/dev/null; "
                                    f"ls -l {mnt}/var/lib/flatpak/app 2>/dev/null; "
                                    f"cat {mnt}/etc/os-release 2>/dev/null | head -3"],
                                   rec, check=False).stdout)
                finally:
                    run(["sudo", "umount", mnt], rec, check=False)
            out.append("found: " + json.dumps(found, indent=1))
            with open(os.path.join(a.out, "disk-inspection.txt"), "w") as fh:
                fh.write("\n".join(out))
            if not found:
                raise Failure(f"none of {APP_FILES} is on any Linux partition")
        finally:
            for kind, d in attached:
                if kind == "nbd":
                    run(["sudo", "qemu-nbd", "--disconnect", d], rec, check=False)
                else:
                    run(["sudo", "losetup", "-d", d], rec, check=False)
            rec.write_timings()
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("windows")
    w.add_argument("out"); w.add_argument("vm")
    w.add_argument("--iso", required=True); w.add_argument("--answer", required=True)
    fl = sub.add_parser("flow")
    fl.add_argument("out"); fl.add_argument("golden"); fl.add_argument("work")
    i = sub.add_parser("inspect")
    i.add_argument("out"); i.add_argument("work")
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    return {"windows": cmd_windows, "flow": cmd_flow, "inspect": cmd_inspect}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
