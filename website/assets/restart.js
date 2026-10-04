/* restart.js — the install and the restore, one step at a time, with a
 * plug to pull.
 *
 * The order of the steps is src/aurstage/install.c (the install) and
 * src/aurstage/rescue.c (the restore). The words on the screen are the
 * ones those files print. The named instants are the fault_maybe()
 * names, and the "what the test found" lines are copied from
 * docs/results/powercut.txt, which tools/powercuttest.sh produced.
 * The disk drawing is to no scale: it shows which region holds what,
 * not how big anything is.
 */
(function () {
  "use strict";
  var rs = document.querySelector("[data-rs]");
  if (!rs) return;
  var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  function h(tag, attrs) {
    var el = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      var v = attrs[k];
      if (v == null || v === false) continue;
      if (k === "class") el.className = v;
      else if (k === "text") el.textContent = v;
      else if (k === "style") el.setAttribute("style", v);
      else if (k.slice(0, 2) === "on") el.addEventListener(k.slice(2), v);
      else el.setAttribute(k, v === true ? "" : v);
    }
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      if (c == null || c === false) continue;
      (Array.isArray(c) ? c : [c]).forEach(function (x) { if (x != null && x !== false) el.appendChild(typeof x === "string" ? document.createTextNode(x) : x); });
    }
    return el;
  }

  /* ── geometry, in thousandths of the disk (not to scale) ─────────── */
  var G = { esp: [0, 30], win: [30, 940], winSmall: [30, 500], files: [30, 300], img: [300, 345],
            root: [500, 840], saved: [840, 890], start: [890, 940], rec: [940, 1000] };

  /* ── what the test transcript says, per kind of instant ──────────── */
  var BACK = ["the table is valid again", "the three original partitions are back", "Windows is its full size again",
              "the EFI partition is byte-for-byte what it was", "the Windows FILESYSTEM is its full size again",
              "and the installer says so itself"];
  function testLines(name, kind) {
    var L = ["the machine stops dead at " + name];
    if (kind === "above") L.push("the disk is byte-for-byte untouched");
    if (kind === "capture") { L.push("the disk is byte-for-byte untouched"); L.push("a half-written saved copy is refused"); }
    if (kind === "below") {
      L.push("after the cut: every file in Windows is still exactly what it was");
      L.push("and Windows can be put back");
      BACK.forEach(function (b) { L.push("after " + name + ": " + b); });
    }
    if (kind === "restore") {
      L.push("running it again finishes the job");
      BACK.forEach(function (b) { L.push("after " + name + ": " + b); });
    }
    return L;
  }

  /* Keys that belong to one step only; everything else (the disk, the
   * firmware's list, the screen) carries on from the step before. */
  var ONCE = { id: 1, where: 1, title: 1, scr: 1, instant: 1, cut: 1, line: 1, danger: 1, key: 1, untested: 1 };

  /* ── the install ─────────────────────────────────────────────────── */
  function installSteps(stick) {
    var S = [];
    var base = { map: "old", gpt: [0, 0, 0], ntfs: 940, files: 1, img: 0, espFiles: 0, saved: 0, root: 0, start: 0,
                 order: "Windows Boot Manager", next: "\u2014", way: "not made yet", screen: "win" };
    function add(o) { var s = {}, k; for (k in base) if (!ONCE[k]) s[k] = base[k]; for (k in o) s[k] = o[k]; base = s; S.push(s); return s; }
    var winSays = "Windows starts as usual. The layout of the drive has not changed.";

    add({ id: "check", where: "Windows", title: "Check this PC", screen: "win",
          scr: ["Check this PC", "Reads the PC and changes nothing. If it finds something, \u201cFix these for me\u201d fixes it: Fast Startup, low space, BitLocker, a waiting restart."],
          cut: { kind: "fine", person: winSays } });
    add({ id: "download", where: "Windows", title: "Download AurOS", img: stick ? 0 : 1,
          scr: ["Start installing", "About 1.8 GB, in pieces, each one checked. It resumes if the connection drops." + (stick ? "" : " It lands in C:\\AurOS.")],
          cut: { kind: "fine", person: winSays + " Pressing Start installing again downloads only what is missing." } });
    add({ id: "arm", where: "Windows", title: "Get the restart ready", espFiles: 1, next: "the AurOS installer",
          scr: ["Restart now", "Its start-up files go under \\EFI\\AurOS, and the PC is told to start the installer once. Nothing on the drive is resized or rewritten before the restart."],
          cut: { kind: "fine", person: "The one-time entry is set, so the next start goes to the installer\u2019s text screen, which checks everything again before it changes anything." } });
    add({ id: "restart", where: "The restart", title: "The one restart", screen: "black", next: "\u2014",
          scr: [], cut: { kind: "fine", person: "The firmware has used up the one-time entry. Switched on again, Windows starts." } });

    add({ id: "capture", where: "After the restart", title: "Save how this PC starts Windows", screen: "text", way: stick ? "being written to the stick" : "in memory",
          scr: ["Saving this computer's Windows startup, so it can be put back."],
          instant: stick ? "capture-mid" : null,
          cut: stick ? { kind: "capture", person: "Nothing has been written to the PC\u2019s disk. The half-written copy on the stick is refused rather than half believed." }
                     : { kind: "fine", person: "The copy was only in memory, and nothing has been written to the disk. Windows starts as before." } });
    add({ id: "gate", where: "After the restart", title: "Everything checked", way: stick ? "on the stick" : "in memory",
          scr: ["saved    read back and checked", "Everything AurOS can check has been checked."], instant: "gate",
          cut: { kind: "above", person: "Nothing has been written to the disk. Windows starts as before." } });

    add({ id: "line", where: "After the restart", title: "Below this line the disk changes", line: true,
          scr: ["Making room on the Windows drive. This is the only part", "that cannot be undone. Do not turn the computer off."], instant: "shrink-begin",
          cut: { kind: "above", person: "The shrink had not started. Nothing has been written to the disk. Windows starts as before." } });
    add({ id: "shrink", where: "After the restart", title: "Making room (the shrink)", ntfs: 500, danger: true,
          scr: ["aurstage: ................"],
          cut: { kind: "danger", person: "This is the one window the design cannot make safe. The tool that resizes Windows\u2019 filesystem (ntfsresize) only promises to survive an interruption when it is growing one, not shrinking it. A shrink cut here is treated as a damaged drive: Windows\u2019 own disk check is the way back, and in the worst case files are lost. So the installer refuses to start on battery, reads the whole region it will move first, and this is the only screen that says not to switch off." } });
    add({ id: "shrink-end", where: "After the restart", title: "Windows is smaller",
          scr: ["windows  resized"], instant: "shrink-end",
          cut: { kind: "below", person: "The table still says Windows is its old size, and a partition bigger than its filesystem starts normally. Windows starts, after checking the drive (a resize marks it to be checked)." } });
    if (!stick) add({ id: "mirror", where: "After the restart", title: "Keep the way back on this PC first", saved: 1, way: "on the disk (not in the table yet)",
          scr: ["Keeping the way back on this computer, before anything else is written.", "saved    the way back is on this computer, read back and checked"], instant: "mirror-end", untested: true,
          cut: { kind: "named", person: "The copy is on the disk, in space the table does not mention yet. Windows starts, smaller, with every file in it." } });
    add({ id: "write-mid", where: "After the restart", title: "Copying AurOS (a quarter through)", root: 0.25,
          scr: ["Copying AurOS onto this computer.", "aurstage: ....."], instant: "write-mid",
          cut: { kind: "below", person: "AurOS is half written into space the table does not mention, and its first megabyte is written last, so nothing half-written there can be taken for a filesystem. Windows starts, smaller." } });
    add({ id: "write-end", where: "After the restart", title: "AurOS written, read back, checked", root: 1,
          scr: ["aurstage: ...................."], instant: "write-end",
          cut: { kind: "below", person: "AurOS is on the disk, but the table does not say so yet. The PC still starts Windows first. Windows starts, smaller." } });
    add({ id: "boot-mid", where: "After the restart", title: "Making the PC able to start AurOS", start: 0.5,
          scr: ["Making this computer able to start AurOS."], instant: "boot-mid",
          cut: { kind: "below", person: "Half of AurOS\u2019s start-up partition is written into unlisted space. The PC\u2019s own EFI partition is untouched. Windows starts." } });
    add({ id: "boot-end", where: "After the restart", title: "AurOS\u2019s start-up written", start: 1,
          scr: ["aurstage: ...................."], instant: "boot-end",
          cut: { kind: "below", person: "Everything AurOS needs is on the disk; none of it is in the table. Windows starts." } });
    add({ id: "probe", where: "After the restart", title: "Check this PC works under AurOS",
          scr: ["Checking that this computer works under AurOS.", "checks   \u2026 drivers loaded; wireless works, the screen brightness works, sound works"],
          cut: { kind: "fine", person: "No instant is named here; the disk is as it was at the last one. Windows starts, smaller. If AurOS had found no way to get online, it would have stopped here, before the table changes." } });
    add({ id: "commit-array", where: "After the restart", title: "New layout: the entry list", gpt: [1, 0, 0],
          scr: ["Writing the new layout."], instant: "commit-array",
          cut: { kind: "below", person: "The new list of partitions is written, but the sector that points to it is not, so its checksum no longer matches and every reader falls back to the spare copy at the end of the disk, which still describes the old layout exactly. Windows starts." } });
    add({ id: "commit-sector", where: "After the restart", title: "The one sector (LBA 1)", gpt: [1, 1, 0], map: "new", ntfs: 500,
          scr: [], instant: "commit-sector", key: true,
          cut: { kind: "below", person: "This single 512-byte write is the instant the disk becomes the new shape. Before it, the old layout; after it, the new one; there is no in-between to land in. Windows starts, smaller, with AurOS beside it." } });
    add({ id: "commit-backup", where: "After the restart", title: "The spare copy catches up", gpt: [1, 1, 1],
          scr: [], instant: "commit-backup",
          cut: { kind: "below", person: "Both copies of the table describe the new layout. Windows is still first in the start-up list. Windows starts." } });
    add({ id: "entry", where: "After the restart", title: "AurOS joins the start-up menu, once", next: "AurOS",
          scr: ["startup  AurOS is in this computer's start-up menu (entry 0003); Windows is still what it starts by default"], instant: "boot-entry",
          cut: { kind: "below", person: "AurOS is in the menu and asked for once, but Windows is still the default. Switched on, it tries AurOS once; if AurOS cannot start, the firmware falls back to Windows by itself." } });
    if (stick) add({ id: "mirror2", where: "After the restart", title: "A second copy of the way back, on the PC", saved: 1, way: "on the stick and on the disk",
          scr: ["Keeping a copy of the way back on this computer too.", "saved    a second copy is on this computer"],
          cut: { kind: "fine", person: "AurOS is installed and in the menu. The copy on the stick is the one that matters if this one is cut short." } });
    add({ id: "settle", where: "After the restart", title: "AurOS fills the space it was given",
          scr: ["Making AurOS fill the space it was given."], instant: "settle-end",
          cut: { kind: "below", person: "AurOS is installed. Switched on, the one-time entry starts AurOS; Windows is still in the menu and still the default." } });
    add({ id: "handover", where: "After the restart", title: "AurOS starts, in the same boot", screen: "auros", next: "AurOS (re-armed every start until you answer)",
          scr: ["aurstage-report v1 verdict=installed record=done", "AurOS is installed. Starting it now."],
          cut: { kind: "fine", person: "Switched on again, AurOS starts and asks its question again; it re-arms the one-time start each time until you answer. Windows is in the menu throughout." } });
    add({ id: "ask", where: "AurOS", title: "\u201cAurOS is on this computer\u201d", screen: "auros",
          scr: ["AurOS is on this computer", "Have a look around. Nothing has been decided yet."],
          cut: { kind: "fine", person: "Nothing has been decided, so nothing was lost. Switched on again, AurOS asks again." } });
    add({ id: "yes", where: "AurOS", title: "\u201cYes, it all works\u201d", screen: "auros", order: "AurOS, then Windows Boot Manager", next: "\u2014",
          scr: ["AurOS starts from now on", "Windows is still here, and still on the menu when you switch on."],
          cut: { kind: "fine", person: "AurOS is what the PC starts now. Windows is in the menu, and Settings \u2192 Put Windows back is there for as long as you want it." } });
    return S;
  }

  /* ── the restore ─────────────────────────────────────────────────── */
  function restoreSteps(stick) {
    var S = [];
    var base = { map: "new", gpt: [1, 1, 1], ntfs: 500, files: 1, img: stick ? 0 : 1, espFiles: 1, saved: 1, root: 1, start: 1,
                 order: "AurOS, then Windows Boot Manager", next: "\u2014", way: stick ? "on the stick and on the disk" : "on the disk", screen: "auros" };
    function add(o) { var s = {}, k; for (k in base) if (!ONCE[k]) s[k] = base[k]; for (k in o) s[k] = o[k]; base = s; S.push(s); return s; }
    var rerun = "In the test, starting the restore again finished the job: every step of it can safely be done twice. How a person, rather than a test, starts it again on a machine cut here has not been tried on a real PC.";
    add({ id: "press", where: "AurOS", title: "Settings \u2192 Put Windows back, twice", screen: "auros",
          scr: ["Put Windows back", "Remove AurOS and put Windows back. The computer restarts. Windows gets all of its drive back."],
          cut: { kind: "fine", person: "Nothing has changed yet. AurOS starts again." } });
    add({ id: "restart", where: "The restart", title: "Restart into the restore", screen: "black", next: "the restore, once",
          scr: [], cut: { kind: "fine", person: "The restore has not touched anything yet." } });
    add({ id: "check", where: "The restore", title: "Check the saved copy", screen: "text", next: "\u2014",
          scr: ["checking the saved copy of this computer's startup", "the saved copy is complete and undamaged"],
          cut: { kind: "fine", person: "Only read so far. The disk is still the AurOS shape." } });
    add({ id: "restore-array", where: "The restore", title: "The old entry list goes back", gpt: [0, 1, 1],
          scr: ["putting this computer's original layout back"], instant: "restore-array",
          cut: { kind: "restore", person: "The entry list no longer matches its header, so every reader falls back to the spare copy, which still describes the disk exactly as it is this second. " + rerun } });
    add({ id: "restore-sector", where: "The restore", title: "The one sector, back", gpt: [0, 0, 1], map: "old",
          scr: [], instant: "restore-sector", key: true,
          cut: { kind: "restore", person: "This one write is the instant the PC\u2019s layout becomes the one it had before AurOS. " + rerun } });
    add({ id: "restore-backup", where: "The restore", title: "The spare copy catches up", gpt: [0, 0, 0],
          scr: ["the original layout is back"], instant: "restore-backup",
          cut: { kind: "restore", person: "Both copies of the table are the original again; AurOS\u2019s areas are no longer listed. " + rerun } });
    add({ id: "restore-esp-mid", where: "The restore", title: "Windows\u2019 start-up files go back", espFiles: 0,
          scr: ["putting the Windows startup files back"], instant: "restore-esp-mid",
          cut: { kind: "restore", person: "The PC\u2019s EFI partition is half put back from the saved copy. " + rerun } });
    add({ id: "restore-grow", where: "The restore", title: "Windows grows back to full size",
          scr: ["the Windows startup files are back", "making drive 3 its full size again"], instant: "restore-grow",
          cut: { kind: "restore", person: "The table is the original; the filesystem inside it is about to be made its full size again. " + rerun } });
    add({ id: "off", where: "The restore", title: "Windows is back. It switches itself off.", order: "Windows Boot Manager", ntfs: 940, root: 0, start: 0, saved: 0, way: "no longer needed",
          scr: ["drive 3 is its full size again", "Windows is back exactly as it was. Restart the computer.", "aurstage-report v1 verdict=restored record=done", "Windows is back. This computer will switch itself off;", "switch it on again and Windows starts."],
          cut: { kind: "fine", person: "It was switching itself off anyway. Switched on, Windows starts, its full size." } });
    return S;
  }

  /* ── state ───────────────────────────────────────────────────────── */
  var journey = "install", stick = false, steps = installSteps(false), at = 0, playing = 0, cutOpen = false;
  var range = rs.querySelector("[data-range]");
  var list = rs.querySelector("[data-steps]");
  var screen = rs.querySelector("[data-screen]");
  var mapEl = rs.querySelector("[data-map]"), groundEl = rs.querySelector("[data-ground]");
  var facts = rs.querySelector("[data-facts]");
  var cutEl = document.querySelector("[data-cut]");
  var plug = rs.querySelector("[data-plug]");

  function rebuild() {
    steps = journey === "install" ? installSteps(stick) : restoreSteps(stick);
    at = Math.min(at, steps.length - 1);
    range.max = String(steps.length - 1);
    list.innerHTML = "";
    var lastWhere = "";
    steps.forEach(function (s, i) {
      var li = h("li", { class: (s.line ? "line " : "") + (s.danger ? "danger " : "") + (s.key ? "key " : "") },
        s.where !== lastWhere ? h("span", { class: "rs-where" }, s.where) : null,
        h("button", { type: "button", "data-i": i, onclick: function () { stop(); setAt(i); } },
          h("span", { class: "rs-t" }, s.title),
          s.instant ? h("span", { class: "rs-inst" + (s.untested ? " untested" : "") }, s.instant) : null));
      lastWhere = s.where;
      list.appendChild(li);
    });
    setAt(at);
  }

  function seg(cls, r, label, extra) {
    var el = h("div", { class: "seg-p " + cls, style: "left:" + (r[0] / 10) + "%;width:" + ((r[1] - r[0]) / 10) + "%;" + (extra || "") }, label ? h("span", null, label) : null);
    return el;
  }
  function drawDisk(s) {
    /* the map */
    mapEl.innerHTML = "";
    mapEl.appendChild(seg("m esp", G.esp, "EFI"));
    if (s.map === "old") mapEl.appendChild(seg("m win", G.win, "Windows (C:)"));
    else {
      mapEl.appendChild(seg("m win", G.winSmall, "Windows (C:)"));
      mapEl.appendChild(seg("m auros", G.root, "AurOS"));
      mapEl.appendChild(seg("m saved", G.saved, ""));
      mapEl.appendChild(seg("m start", G.start, ""));
    }
    mapEl.appendChild(seg("m rec", G.rec, "Rec."));
    var g = h("div", { class: "gpt", title: "The three writes that change the table" },
      ["entries", "LBA 1", "spare"].map(function (n, i) { return h("span", { class: s.gpt[i] ? "new" : "" }, n); }));
    mapEl.appendChild(g);
    mapEl.setAttribute("aria-label", "Partition table: " + (s.map === "old" ? "EFI, Windows at full size, recovery" : "EFI, Windows made smaller, AurOS, the way back, AurOS start-up, recovery"));

    /* the ground */
    groundEl.innerHTML = "";
    groundEl.appendChild(seg("g esp" + (s.espFiles ? " marked" : ""), G.esp, ""));
    groundEl.appendChild(seg("g ntfs", [G.win[0], s.ntfs], ""));
    groundEl.appendChild(seg("g files", G.files, "your files"));
    if (s.img) groundEl.appendChild(seg("g img", G.img, ""));
    if (s.root) groundEl.appendChild(seg("g auros", [G.root[0], G.root[0] + (G.root[1] - G.root[0]) * s.root], s.root >= 1 ? "AurOS" : ""));
    if (s.saved) groundEl.appendChild(seg("g saved", G.saved, ""));
    if (s.start) groundEl.appendChild(seg("g start", [G.start[0], G.start[0] + (G.start[1] - G.start[0]) * s.start], ""));
    groundEl.appendChild(seg("g rec", G.rec, ""));
    groundEl.setAttribute("aria-label", "Disk contents: Windows filesystem " + (s.ntfs < 900 ? "made smaller" : "at full size") +
      (s.root ? ", AurOS " + (s.root >= 1 ? "written" : "partly written") : "") + (s.saved ? ", the way back written" : ""));

    facts.innerHTML = "";
    [["Switching on starts", s.order], ["Next start only", s.next], ["The way back is", s.way]].forEach(function (f) {
      facts.appendChild(h("div", null, h("dt", null, f[0]), h("dd", null, f[1])));
    });
  }

  var logLines = [];
  function drawScreen(s) {
    screen.className = "rs-screen " + s.screen;
    screen.innerHTML = "";
    if (s.screen === "win") {
      screen.appendChild(h("div", { class: "scr-win" },
        h("div", { class: "scr-win-bar" }, h("span", null, "\u25C6 AurOS installer")),
        h("div", { class: "scr-win-body" }, h("p", { class: "scr-h" }, s.scr[0]), h("p", null, s.scr[1]))));
    } else if (s.screen === "text") {
      /* the text screen accumulates, as a real console does */
      var lines = [];
      for (var i = 0; i <= at; i++) if (steps[i].screen === "text") lines = lines.concat(steps[i].scr);
      var box = h("div", { class: "scr-text" }, lines.slice(-9).map(function (l, j, arr) {
        return h("div", { class: j >= arr.length - s.scr.length ? "fresh" : "" }, l);
      }));
      screen.appendChild(box);
      if (s.danger) screen.appendChild(h("div", { class: "scr-danger" }, "Do not turn the computer off."));
    } else if (s.screen === "auros") {
      screen.appendChild(h("div", { class: "scr-auros" },
        h("p", { class: "scr-h" }, s.scr[0] || ""), h("hr"), s.scr.slice(1).map(function (l) { return h("p", null, l); })));
    } else {
      screen.appendChild(h("div", { class: "scr-black" }, h("span", null, "restarting\u2026")));
    }
    if (s.instant) screen.appendChild(h("div", { class: "scr-instant" }, "named instant: ", h("b", null, s.instant)));
  }

  function setAt(i) {
    at = Math.max(0, Math.min(steps.length - 1, i));
    var s = steps[at];
    range.value = String(at);
    range.setAttribute("aria-valuetext", (at + 1) + " of " + steps.length + ": " + s.title);
    Array.prototype.forEach.call(list.querySelectorAll("button"), function (b) {
      var on = +b.getAttribute("data-i") === at;
      b.setAttribute("aria-current", on ? "step" : "false");
      if (on && b.scrollIntoView && !reduce && document.activeElement !== b) {
        var lr = list.getBoundingClientRect(), br = b.getBoundingClientRect();
        if (br.left < lr.left || br.right > lr.right) list.scrollLeft += (br.left - lr.left) - lr.width / 3;
      }
    });
    drawDisk(s);
    drawScreen(s);
    if (cutOpen) closeCut();
  }

  /* ── the plug ────────────────────────────────────────────────────── */
  function closeCut() { cutOpen = false; cutEl.hidden = true; rs.classList.remove("off"); plug.textContent = "Pull the plug"; }
  function pull() {
    stop();
    if (cutOpen) { closeCut(); plug.focus(); return; }
    var s = steps[at], c = s.cut;
    cutOpen = true;
    rs.classList.add("off");
    plug.textContent = "Plug it back in";
    cutEl.innerHTML = "";
    var head = c.kind === "danger" ? "The one bad row" : s.instant ? "The power went at " : "The power went during ";
    cutEl.appendChild(h("p", { class: "kicker plain" }, "Power cut \u00b7 step " + (at + 1) + " of " + steps.length));
    cutEl.appendChild(h("h3", { class: "cut-h" }, c.kind === "danger" ? head : [head, s.instant ? h("span", { class: "mono" }, s.instant) : "\u201c" + s.title + "\u201d"]));
    cutEl.appendChild(h("p", { class: "cut-person" }, c.person));
    var ev = h("div", { class: "cut-evidence" });
    if (c.kind === "above" || c.kind === "below" || c.kind === "capture" || c.kind === "restore") {
      ev.appendChild(h("p", { class: "note-title" }, "What the test found at this instant"));
      var pre = h("div", { class: "cut-log", role: "list" });
      ev.appendChild(pre);
      var lines = testLines(s.instant, c.kind), k = 0;
      (function next() {
        if (!cutOpen) return;
        if (k < lines.length) {
          pre.appendChild(h("div", { role: "listitem" }, h("span", null, lines[k]), h("b", null, "ok")));
          k++; setTimeout(next, reduce ? 0 : 110);
        }
      })();
      ev.appendChild(h("p", { class: "small" }, "From docs/results/powercut.txt. Run on a virtual machine with real UEFI firmware, Secure Boot on" +
        (journey === "install" ? ", and an AurOS memory stick holding the way back." : ", restoring from the copy on an AurOS memory stick.")));
    } else if (c.kind === "named") {
      ev.appendChild(h("p", { class: "small" }, "This instant is named in the code (" + s.instant + "), but the power-cut run does not stop here: it runs with a memory stick, and this step only exists without one. What is written above follows from the design; it is not a test result."));
    } else if (c.kind === "danger") {
      ev.appendChild(h("p", { class: "small" }, "No named instant sits inside the shrink, and none could: there is nothing a test could prove there except that it can go wrong. The two instants on either side of it, shrink-begin and shrink-end, are tested."));
    } else {
      ev.appendChild(h("p", { class: "small" }, "No instant is named at this step. What is written above follows from the design (docs/AURBRIDGE.md, \u201cPower loss, step by step\u201d), not from a power-cut test."));
    }
    cutEl.appendChild(ev);
    cutEl.appendChild(h("p", { class: "btn-row" },
      h("button", { class: "btn btn-sm", type: "button", onclick: function () { closeCut(); plug.focus(); } }, "Plug it back in"),
      at < steps.length - 1 ? h("button", { class: "btn btn-sm", type: "button", onclick: function () { closeCut(); setAt(at + 1); } }, "Carry on to the next step") : null));
    cutEl.hidden = false;
    cutEl.focus({ preventScroll: true });
    if (cutEl.scrollIntoView) cutEl.scrollIntoView({ block: "nearest", behavior: reduce ? "auto" : "smooth" });
  }
  plug.addEventListener("click", pull);

  /* ── playing ─────────────────────────────────────────────────────── */
  var playBtn = rs.querySelector("[data-play]");
  function stop() { if (playing) { clearInterval(playing); playing = 0; playBtn.textContent = "Play"; playBtn.setAttribute("aria-pressed", "false"); } }
  playBtn.addEventListener("click", function () {
    if (playing) { stop(); return; }
    if (at >= steps.length - 1) setAt(0);
    playBtn.textContent = "Pause"; playBtn.setAttribute("aria-pressed", "true");
    playing = setInterval(function () { if (at >= steps.length - 1) { stop(); return; } setAt(at + 1); }, 1900);
  });
  rs.querySelector("[data-prev]").addEventListener("click", function () { stop(); setAt(at - 1); });
  rs.querySelector("[data-next]").addEventListener("click", function () { stop(); setAt(at + 1); });
  range.addEventListener("input", function () { stop(); setAt(+range.value); });

  Array.prototype.forEach.call(document.querySelectorAll("[data-journey]"), function (b) {
    b.addEventListener("click", function () {
      journey = b.getAttribute("data-journey");
      Array.prototype.forEach.call(document.querySelectorAll("[data-journey]"), function (o) { o.setAttribute("aria-pressed", o === b ? "true" : "false"); });
      stop(); closeCut(); at = 0; rebuild();
    });
  });
  var stickBox = document.querySelector("[data-stick]");
  if (stickBox) stickBox.addEventListener("change", function () {
    stick = stickBox.checked;
    var id = steps[at].id;
    stop(); closeCut(); rebuild();
    for (var i = 0; i < steps.length; i++) if (steps[i].id === id) { setAt(i); break; }
  });

  /* ── the eighteen instants, as buttons ───────────────────────────── */
  var inst = document.querySelector("[data-instants]");
  if (inst) {
    var I = [["install", ["gate", "capture-mid", "shrink-begin", "shrink-end", "write-mid", "write-end", "boot-mid", "boot-end",
                          "commit-array", "commit-sector", "commit-backup", "boot-entry", "settle-end"]],
             ["restore", ["restore-array", "restore-sector", "restore-backup", "restore-esp-mid", "restore-grow"]]];
    I.forEach(function (grp) {
      var row = h("div", { class: "inst-row" }, h("span", { class: "inst-lab" }, grp[0] === "install" ? "During the install" : "During the restore"));
      grp[1].forEach(function (name) {
        row.appendChild(h("button", { type: "button", class: "inst", onclick: function () {
          journey = grp[0];
          Array.prototype.forEach.call(document.querySelectorAll("[data-journey]"), function (o) { o.setAttribute("aria-pressed", o.getAttribute("data-journey") === journey ? "true" : "false"); });
          /* capture-mid is only an instant with a stick, and the test ran with one */
          stick = true; if (stickBox) stickBox.checked = true;
          stop(); closeCut(); rebuild();
          for (var i = 0; i < steps.length; i++) if (steps[i].instant === name) { setAt(i); break; }
          rs.scrollIntoView({ behavior: reduce ? "auto" : "smooth", block: "start" });
          setTimeout(pull, reduce ? 0 : 450);
        } }, name));
      });
      inst.appendChild(row);
    });
  }

  rebuild();
})();
