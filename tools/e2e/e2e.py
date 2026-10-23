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
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vision                                                    # noqa: E402
from vm import (Failure, Hub, Machine, Recorder, OVMF_VARS,      # noqa: E402
                run, sha256)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
E = os.environ.get

# ── what to install, and from where ──────────────────────────────────
PUBLISHED_URL = ("https://github.com/ComputerDude771/auros-from-scratch/raw/"
                 "image-desktop/AurOS-Installer-test.exe")
INSTALLER_URL = E("INSTALLER_URL") or PUBLISHED_URL
INSTALLER_SHA256 = E("INSTALLER_SHA256", "")
# A file on the runner instead (an artifact from an earlier job): served
# from this host and the website's download links are pointed at it.
INSTALLER_PATH = E("INSTALLER_PATH", "")
INSTALLER_NAME = "AurOS-Installer-test.exe"
# The website a person starts from. Served from the runner; "none" gives
# a one-link page instead.
WEBSITE_DIR = E("WEBSITE_DIR") or os.path.join(REPO, "website")
WEBSITE_PAGE = E("WEBSITE_PAGE", "index.html")
# The download link's name, as UI Automation reads it from Edge.
DOWNLOAD_LINK = E("DOWNLOAD_LINK", r"(?i)download.*(installer|aurOS)")

# ── the store, after AurOS works (M3) ────────────────────────────────
STORE_NAME = E("STORE_NAME", "Get more programs")    # the app list's name for it
STORE_QUERY = E("STORE_QUERY", "VLC")                 # typed into its search
STORE_RESULT = E("STORE_RESULT", "VLC")               # the result to open
STORE_INSTALL = E("STORE_INSTALL", "Install")         # the button that installs
STORE_OPEN = E("STORE_OPEN", "Open|Launch")           # appears when installed
APP_FILES = E("APP_FILES", "usr/bin/vlc var/lib/flatpak/app/org.videolan.VLC")

HUB = "http://10.0.2.2:8000"
DEADLINE = time.time() + 60 * float(E("E2E_MINUTES", "300"))


def left():
    return DEADLINE - time.time()


# ════════════════════════════════════════════════════════════════════
#  M1: Windows
# ════════════════════════════════════════════════════════════════════

def cmd_windows(a):
    rec = Recorder(a.out)
    hub = Hub(rec)
    os.makedirs(a.vm, exist_ok=True)
    disk, vars_fd, tpm = (os.path.join(a.vm, n) for n in ("win.qcow2", "vars.fd", "tpm"))
    run(["qemu-img", "create", "-f", "qcow2", disk, "128G"], rec)
    shutil.copyfile(OVMF_VARS, vars_fd)
    m = Machine(rec, a.vm, disk, vars_fd, tpm, cdroms=[a.iso, a.answer])
    ok = False
    try:
        with rec.phase("windows: firmware to Setup"):
            m.start()
            m.auto_shots(2)
            # The DVD's "Press any key to boot from CD or DVD" waits a
            # few seconds and then gives up on it; keep pressing until
            # Setup is loading.
            for _ in range(45):
                m.key("ret")
                time.sleep(1)
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
        self.m = Machine(self.rec, a.work, self.disk, self.vars, self.tpm)

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
        # Whatever the pages link to as the installer, link to this one.
        pat = re.compile(r'href="[^"]*' + re.escape(INSTALLER_NAME) + '"')
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
                            f"toggle={e.get('toggle')}\t{e.get('window')!r}\n")
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
    def pick(els, name, types=None, exact=False):
        rx = re.compile(name if not exact else "^" + re.escape(name) + "$", re.I)
        for e in els:
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
            time.sleep(45)
            self.m.shot("windows-desktop")
            self.guest("Get-Disk | Format-List Number,FriendlyName,Size; "
                       "Get-Volume -DriveLetter C | Format-List SizeRemaining; "
                       "(Get-ItemProperty 'HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power').HiberbootEnabled")
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
        with self.rec.phase("edge: tick the boxes and press Download"):
            # The download is at the foot of the page: End, as a person
            # scrolls there. The boxes gate the link (website/assets).
            self.m.key("end", label="scroll to the download")
            time.sleep(4)
            for attempt in range(8):
                els = self.elements("msedge", ["CheckBox", "Hyperlink", "Button"])
                boxes = [e for e in els if e.get("type") == "CheckBox" and e.get("toggle") == "Off"]
                if boxes:
                    self.click_el(boxes[0], "website tick: " + (boxes[0].get("name") or "")[:40])
                    time.sleep(1.5)
                    continue
                link = self.pick(els, DOWNLOAD_LINK, ("Hyperlink", "Button"))
                if link:
                    self.click_el(link, "website download link")
                    time.sleep(5)
                    return
                self.m.scroll(640, 400, 5)
                time.sleep(2)
            raise Failure("no download link on the page (ui-elements.log has what Edge showed)")

    def keep_download(self):
        """Edge's download warnings, answered the way a person who wants
        the file does; returns once the file is downloaded."""
        with self.rec.phase("edge: download, and keep it"):
            dl = os.path.join("$env:USERPROFILE", "Downloads", INSTALLER_NAME)
            deadline = time.time() + 15 * 60
            pressed = []
            while time.time() < deadline:
                els = self.elements("msedge", ["Button", "MenuItem", "Hyperlink", "ListItem",
                                               "Text", "SplitButton"])
                names = " | ".join((e.get("name") or "")[:80] for e in els)
                for want, types in (("^Keep anyway$", None), ("^Show more$", ("Button",)),
                                    ("^Keep$", None)):
                    e = self.pick(els, want, types)
                    if e and pressed.count(want) < 4:
                        self.click_el(e, "edge " + want.strip("^$"))
                        pressed.append(want)
                        time.sleep(3)
                        break
                else:
                    done = self.guest(f"$f='{dl}'; if (Test-Path $f) {{ (Get-FileHash $f).Hash }} "
                                      f"else {{ 'missing' }}; "
                                      f"Get-ChildItem \"$env:USERPROFILE\\Downloads\" | Format-Table Name,Length")
                    h = done.split()[0].lower() if done.strip() else ""
                    if re.fullmatch(r"[0-9a-f]{64}", h):
                        self.log(f"downloaded {INSTALLER_NAME}: SHA-256 {h}")
                        if INSTALLER_SHA256 and h != INSTALLER_SHA256.lower():
                            raise Failure(f"the downloaded installer is {h}, expected {INSTALLER_SHA256}")
                        self.m.shot("downloaded")
                        return
                    warn = re.search(r"(isn.t commonly downloaded|can.t be downloaded securely|"
                                     r"might be dangerous|was blocked|unsafe|verify)", names, re.I)
                    more = self.pick(els, r"^(More actions|See more|More options)", ("Button", "SplitButton", "MenuItem"))
                    if warn and more and pressed.count("more") < 4:
                        self.click_el(more, "edge download item: more actions")
                        pressed.append("more")
                        time.sleep(3)
                        continue
                    time.sleep(5)
            raise Failure("the download never finished (ui-elements.log has Edge's buttons)")

    # -- 3. open it: SmartScreen, then UAC --
    def open_installer(self):
        with self.rec.phase("windows: open the installer (SmartScreen, UAC)"):
            els = self.elements("msedge", ["Button", "Hyperlink", "ListItem", "MenuItem"])
            op = self.pick(els, r"^Open file$") or self.pick(els, INSTALLER_NAME.replace(".", r"\."))
            if op:
                self.click_el(op, "edge open file")
            else:
                # The flyout closed: open Downloads (Ctrl+J) and use it there.
                self.m.key("ctrl", "j", label="Edge downloads")
                time.sleep(4)
                els = self.elements("msedge", ["Button", "Hyperlink", "ListItem"])
                op = self.pick(els, r"^Open file$") or self.pick(els, INSTALLER_NAME.replace(".", r"\."))
                if not op:
                    raise Failure("Edge offers no way to open the download")
                self.click_el(op, "edge open file")
            deadline = time.time() + 6 * 60
            uac_tries = 0
            while time.time() < deadline:
                time.sleep(4)
                ws = self.windows()
                if any((w.get("name") or "") == "Install AurOS" for w in ws):
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
    def wizard_rect(self):
        for w in self.windows():
            if (w.get("name") or "") == "Install AurOS" and w.get("rect"):
                return w["rect"]
        return None

    def drive_wizard(self):
        """Press the teal primary button whenever it is enabled; when it
        is not, do what the page asks (tick its boxes, type the word it
        names) or wait. Stops after pressing "Restart now"."""
        clicks = {}
        typed = False
        idle_since = time.time()
        last_label = None
        rect = self.wizard_rect()
        phase_cm = self.rec.phase("wizard: welcome to the check")
        phase_cm.__enter__()
        try:
            while left() > 0:
                if not self.m.alive():
                    raise Failure("the machine switched off during the wizard")
                if rect is None or time.time() - idle_since > 120:
                    rect = self.wizard_rect() or rect
                if rect is None:
                    w = self.windows()
                    raise Failure("the installer's window is gone; windows: " +
                                  json.dumps([(x.get('name'), x.get('proc')) for x in w]))
                img = self.m.screendump()
                btns = vision.find_buttons(img, rect)
                if btns:
                    b = max(btns, key=lambda b: (b[1] + b[3], b[0] + b[2]))
                    label = vision.text_of(vision.ocr(img, b, psm=7)).strip() or "?"
                    if re.search(r"close", label, re.I) and not re.search(r"restart", label, re.I):
                        self.m.save(img, "wizard-ended-with-close")
                        words = vision.ocr(img, rect)
                        raise Failure("the installer stopped without offering a restart: " +
                                      vision.text_of(words)[:800])
                    n = clicks.get(label, 0)
                    if label == last_label and n >= 6:
                        self.m.save(img, "wizard-stuck")
                        raise Failure(f"pressed '{label}' six times and the page did not move on")
                    if re.search(r"restart", label, re.I):
                        phase_cm.__exit__(None, None, None)
                        self.m.click(*vision.centre(b), label="wizard " + label)
                        self.restart_clicked = time.time()
                        return
                    self.m.click(*vision.centre(b), label="wizard " + label)
                    clicks[label] = n + 1
                    last_label = label
                    idle_since = time.time()
                    # name the phase after the page the press leads to
                    if re.search(r"start(ing)? install|erase", label, re.I):
                        phase_cm.__exit__(None, None, None)
                        phase_cm = self.rec.phase("wizard: download and prepare (to Restart now)")
                        phase_cm.__enter__()
                    elif re.search(r"get started", label, re.I):
                        pass
                    time.sleep(3)
                    continue
                boxes = vision.find_unchecked_boxes(img, rect)
                if boxes:
                    self.m.click(*vision.centre(boxes[0]), label="wizard tick box")
                    time.sleep(1.5)
                    after = vision.find_unchecked_boxes(self.m.screendump(), rect)
                    if len(after) >= len(boxes):
                        self.log("the box did not tick")
                    idle_since = time.time()
                    continue
                words = vision.ocr(img, rect)
                text = vision.text_of(words)
                mt = re.search(r"\bType ([A-Z]{3,})\b", text)
                if mt and not typed:
                    word = mt.group(1)
                    self.m.key("end", label=f"scroll to where {word} is typed")
                    time.sleep(1.5)
                    img = self.m.screendump()
                    ph = vision.find_text(vision.ocr(img, rect), word, last=True)
                    if ph:
                        self.m.click(*vision.centre(ph), label=f"wizard: the box for {word}")
                    self.m.type(word, label="typed confirmation")
                    typed = True
                    time.sleep(1.5)
                    self.m.shot("wizard-typed")
                    idle_since = time.time()
                    continue
                if time.time() - idle_since > 90 and not re.search(r"Setting up|Ready to restart", text):
                    # nothing to press, nothing to tick: perhaps below the fold
                    self.m.key("end", label="scroll down: nothing to do on screen")
                    idle_since = time.time()
                time.sleep(5)
            raise Failure("ran out of time in the wizard")
        except BaseException:
            phase_cm.__exit__(*sys.exc_info())
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
            b, _ = self.ocr_find(STORE_NAME)
            if not b:
                # the Home card / app list: Super brings it, as on any desktop
                self.m.key("meta_l", label="open the app list")
                time.sleep(3)
                b, _ = self.ocr_find(STORE_NAME)
            if not b:
                raise Failure(f"no '{STORE_NAME}' on screen")
            self.m.click(*vision.centre(b), label="open " + STORE_NAME)
            p, _, _ = self.wait_text(["Search", "Explore", "Installed", "Updates"], 300, every=10)
            if not p:
                raise Failure("the store did not open")
            self.m.shot("store-open")
        with self.rec.phase("aurOS: find and install " + STORE_QUERY):
            b, _ = self.ocr_find("Search")
            if b:
                self.m.click(*vision.centre(b), label="store search")
            else:
                self.m.key("ctrl", "f", label="store search (Ctrl+F)")
            time.sleep(2)
            self.m.type(STORE_QUERY, label="search")
            self.m.key("ret")
            time.sleep(10)
            p, b, _ = self.wait_text([STORE_RESULT], 300, every=10)
            if not p:
                raise Failure(f"no '{STORE_RESULT}' in the search results")
            self.m.click(*vision.centre(b), label="open result " + STORE_RESULT)
            time.sleep(8)
            p, b, _ = self.wait_text([STORE_INSTALL], 300, every=10)
            if not p:
                raise Failure(f"no '{STORE_INSTALL}' button")
            self.m.click(*vision.centre(b), label="install")
            # an authentication prompt would show here; that is a finding
            done = None
            deadline = time.time() + 45 * 60
            while time.time() < deadline and not done:
                for o in STORE_OPEN.split("|"):
                    p, b, words = self.wait_text([o], 30, every=15,
                                                 fail=["Authentication", "Failed to install"])
                    if p:
                        done = (o, b)
                        break
            if not done:
                raise Failure("the install did not finish in 45 minutes")
            self.m.shot("store-installed")
        with self.rec.phase("aurOS: open " + STORE_QUERY):
            self.m.click(*vision.centre(done[1]), label="open the installed program")
            time.sleep(20)
            self.m.shot("program-open")

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
    """Read AurOS's root file system with the machine off, through
    qemu-nbd, read-only. Proves the program is on the disk, not just on
    the screen."""
    rec = Recorder(a.out)
    disk = os.path.join(a.work, "win.qcow2")
    out = []
    dev = "/dev/nbd0"
    run("sudo modprobe nbd max_part=16", rec, check=False)
    run(["sudo", "qemu-nbd", "--read-only", "--connect", dev, disk], rec)
    try:
        time.sleep(2)
        out.append(run(["sudo", "sgdisk", "-p", dev], rec, check=False).stdout)
        out.append(run(["lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,PARTLABEL", dev], rec, check=False).stdout)
        found = {}
        mnt = os.path.join(a.work, "mnt")
        os.makedirs(mnt, exist_ok=True)
        parts = sorted(p for p in os.listdir("/dev") if p.startswith("nbd0p"))
        for p in parts:
            fs = run(["sudo", "blkid", "-o", "value", "-s", "TYPE", "/dev/" + p], rec, check=False).stdout.strip()
            if fs not in ("ext4", "btrfs", "xfs"):
                continue
            if run(["sudo", "mount", "-o", "ro,noload" if fs == "ext4" else "ro", "/dev/" + p, mnt],
                   rec, check=False).returncode != 0:
                continue
            try:
                ls = run(["sudo", "ls", mnt], rec, check=False).stdout.split()
                out.append(f"{p} ({fs}): {' '.join(ls)}")
                for rel in APP_FILES.split():
                    for base in ("", "@/", "@rootfs/"):
                        path = os.path.join(mnt, base, rel)
                        r = run(["sudo", "ls", "-ld", path], rec, check=False)
                        if r.returncode == 0:
                            found[rel] = f"{p}: {r.stdout.strip()}"
                r = run(["sudo", "sh", "-c", f"tail -n 40 {mnt}/var/log/apt/history.log 2>/dev/null; "
                         f"ls {mnt}/var/lib/flatpak/app 2>/dev/null"], rec, check=False)
                out.append(r.stdout)
            finally:
                run(["sudo", "umount", mnt], rec, check=False)
        out.append("found: " + json.dumps(found, indent=1))
        with open(os.path.join(a.out, "disk-inspection.txt"), "w") as fh:
            fh.write("\n".join(out))
        rec.log("\n".join(out)[-3000:])
        return 0 if found else 1
    finally:
        run(["sudo", "qemu-nbd", "--disconnect", dev], rec, check=False)


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
