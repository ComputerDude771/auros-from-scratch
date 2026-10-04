/* AurOS: the page is the drive.
 *
 * Everything that moves on index.html: the read/write head in the margin,
 * the partition bar and table (which are the menu), the age stepper, the
 * stickers, and the one restart -- played here with the real installer's
 * own sentences, interruptible at any moment by the power button, and
 * undoable with "Put Windows back", which proves itself with a SHA-256 of
 * Windows' section before and after.
 *
 * No libraries, no network, no storage.
 */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var reduce = window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ── the disk ───────────────────────────────────────────────────── */
  // 256 GB, 512-byte sectors. The layout a typical Windows PC ships with:
  // EFI, MSR, C:, and the maker's recovery partition at the very end.
  var TOTAL = 500118192;            // sectors
  var LAST_USABLE = TOTAL - 35;     // 500118157 (33 for the backup table + header)
  var GiB = 2097152, MiB = 2048;    // sectors
  var REC_FIRST = 498020352, REC_LAST = REC_FIRST + GiB - 1;
  var WIN_FIRST = 239616, WIN_LAST_FULL = REC_FIRST - 1;
  var AUR_SIZE = 28 * GiB, ABOOT_SIZE = 512 * MiB;
  var WIN_LAST_SMALL = WIN_LAST_FULL - AUR_SIZE - ABOOT_SIZE;
  var AUR_FIRST = WIN_LAST_SMALL + 1, AUR_LAST = AUR_FIRST + AUR_SIZE - 1;
  var ABOOT_FIRST = AUR_LAST + 1, ABOOT_LAST = ABOOT_FIRST + ABOOT_SIZE - 1;
  var WIN_USED = 109.6 * GiB;       // what Windows is actually using

  var PARTS = {
    efi:   { n: 1, name: "EFI system partition", type: "EFI System", first: 2048, last: 206847, ink: "--p-efi", href: "#p-efi", short: "EFI", say: "How I start. Signed files only." },
    msr:   { n: 2, name: "Microsoft reserved", type: "MSR", first: 206848, last: 239615, ink: "--p-msr", href: null, short: "MSR", say: "Windows keeps this empty on purpose. So do I." },
    win:   { n: 3, name: "Windows (C:)", type: "Basic data, NTFS", first: WIN_FIRST, last: WIN_LAST_FULL, ink: "--p-win", href: "#p-win", short: "Windows (C:)", say: "Everything you have on me." },
    rec:   { n: 4, name: "Recovery", type: "Windows RE", first: REC_FIRST, last: REC_LAST, ink: "--p-rec", href: "#p-rec", short: "Recovery", say: "My maker's factory reset. Never touched." },
    aur:   { n: 5, name: "AUROS-ROOT", type: "Linux, ext4", first: AUR_FIRST, last: AUR_LAST, ink: "--p-aur", href: "#p-aur", short: "AurOS", say: "AurOS itself." },
    aboot: { n: 6, name: "AUROS-BOOT", type: "Basic data, FAT32", first: ABOOT_FIRST, last: ABOOT_LAST, ink: "--p-aboot", href: "#p-back", short: "way back", say: "The saved copy of how Windows started." }
  };
  var installed = false;

  function fmtLBA(n) { return Math.round(n).toLocaleString("en-US"); }
  function fmtSize(sectors) {
    var b = sectors * 512;
    if (b >= 1024 * 1024 * 1024) return (b / 1073741824).toFixed(1) + " GB";
    return Math.round(b / 1048576) + " MB";
  }
  function setWinLast(last) { PARTS.win.last = last; }

  /* ── the bar: my partitions on a squashed scale ─────────────────── */
  var bar = $("bar");
  var ORDER = ["efi", "msr", "win", "aur", "aboot", "rec"];
  var segs = {};
  // A real 100 MB partition beside a 237 GB one is a hairline; size^0.38
  // keeps every partition visible while Windows still has most of me.
  function weight(sectors) { return sectors <= 0 ? 0 : Math.pow(sectors / MiB, 0.38); }
  function buildBar() {
    ORDER.forEach(function (k) {
      var p = PARTS[k];
      var s = document.createElement("a");
      s.className = "seg " + k;
      if (p.href) s.href = p.href; else s.setAttribute("aria-hidden", "true");
      s.setAttribute("aria-label", p.name);
      s.innerHTML = '<span class="lbl"><b>' + p.short + '</b><br><span class="sz"></span></span>';
      bar.appendChild(s);
      segs[k] = s;
    });
    var scan = document.createElement("i");
    scan.className = "scan";
    bar.appendChild(scan);
    segs.scan = scan;
  }
  // sizes: {k: sectors}; written: 0..1 of AurOS copied; unwritten: true while the gap is empty
  function drawBar(sizes, opts) {
    opts = opts || {};
    var total = 0, w = {};
    ORDER.forEach(function (k) { w[k] = weight(sizes[k] || 0); total += w[k]; });
    ORDER.forEach(function (k) {
      var s = segs[k];
      s.style.flexGrow = w[k].toFixed(3);
      s.style.flexBasis = "0px";
      s.classList.toggle("zero", !sizes[k]);
      s.classList.toggle("thin", w[k] / total < 0.07);
      s.querySelector(".sz").textContent = sizes[k] ? fmtSize(sizes[k]) : "";
    });
    var winSize = sizes.win || 1;
    segs.win.style.setProperty("--used", Math.min(96, WIN_USED / winSize * 100).toFixed(1) + "%");
    segs.aur.classList.toggle("filling", !!opts.filling);
    segs.aboot.classList.toggle("filling", !!opts.bootEmpty);
    segs.aur.style.setProperty("--written", ((opts.written == null ? 1 : opts.written) * 100).toFixed(1) + "%");
  }
  function sizesFor(winLast, withAurOS) {
    return {
      efi: PARTS.efi.last - PARTS.efi.first + 1,
      msr: PARTS.msr.last - PARTS.msr.first + 1,
      win: winLast - WIN_FIRST + 1,
      aur: withAurOS ? AUR_SIZE : 0,
      aboot: withAurOS ? ABOOT_SIZE : 0,
      rec: REC_LAST - REC_FIRST + 1
    };
  }
  // where on the bar (0..1) the head is, for an LBA inside a partition
  function barX(k, frac) {
    var r = bar.getBoundingClientRect(), s = segs[k].getBoundingClientRect();
    if (!r.width) return 0;
    return ((s.left - r.left) + s.width * frac) / r.width;
  }

  /* ── the table at LBA 1: the menu ───────────────────────────────── */
  function drawTable(newRows) {
    var rows = $("gpt-rows");
    var keys = installed ? ["efi", "msr", "win", "rec", "aur", "aboot"] : ["efi", "msr", "win", "rec"];
    rows.innerHTML = "";
    keys.forEach(function (k) {
      var p = PARTS[k];
      var tr = document.createElement("tr");
      if (p.href) {
        tr.className = "part";
        tr.tabIndex = 0;
        tr.setAttribute("role", "link");
        tr.addEventListener("click", function () { location.hash = p.href; });
        tr.addEventListener("keydown", function (e) { if (e.key === "Enter") location.hash = p.href; });
      }
      if (newRows && (k === "aur" || k === "aboot")) tr.classList.add("new");
      tr.innerHTML =
        "<td>" + p.n + "</td>" +
        '<td><span class="swatch" style="background:var(' + p.ink + ')"></span>' + p.name + "</td>" +
        "<td>" + p.type + "</td>" +
        '<td class="num">' + fmtLBA(p.first) + "</td>" +
        '<td class="num">' + fmtLBA(p.last) + "</td>" +
        '<td class="num">' + fmtSize(p.last - p.first + 1) + "</td>" +
        '<td class="say">' + p.say + "</td>";
      rows.appendChild(tr);
    });
    // each section's tag carries its sectors
    setRange("p-efi", PARTS.efi);
    setRange("p-win", PARTS.win);
    setRange("p-rec", PARTS.rec);
    var aurTags = document.querySelectorAll("#p-aur .range");
    if (aurTags[0]) aurTags[0].textContent = "LBA " + fmtLBA(AUR_FIRST) + " – " + fmtLBA(ABOOT_LAST) + " · " + fmtSize(AUR_SIZE + ABOOT_SIZE);
    if (aurTags[1]) aurTags[1].textContent = "LBA " + fmtLBA(AUR_FIRST) + " – " + fmtLBA(AUR_LAST) + " · " + fmtSize(AUR_SIZE);
    setRange("p-back", PARTS.aboot);
  }
  function setRange(id, p) {
    var el = document.querySelector("#" + id + " .range");
    if (el) el.textContent = "LBA " + fmtLBA(p.first) + " – " + fmtLBA(p.last) + " · " + fmtSize(p.last - p.first + 1);
  }

  /* ── the head: where on my drive you are reading ────────────────── */
  var headEl = $("head"), hLBA = $("head-lba"), hPart = $("head-part"), mPart = $("hm-part"), mLBA = $("hm-lba");
  var zones = [];
  function collectZones() {
    zones = [];
    var gpt = document.querySelector(".gpt");
    zones.push({ el: document.querySelector(".hero"), fn: function (f, top) {
      // LBA 0 until the table, then LBA 1-33
      var g = gpt.getBoundingClientRect();
      var line = window.innerHeight * 0.4;
      if (g.top > line) return { lba: 0, label: "protective MBR" };
      var ff = Math.max(0, Math.min(1, (line - g.top) / Math.max(1, g.height)));
      return { lba: 1 + Math.round(ff * 32), label: "primary GPT" };
    } });
    document.querySelectorAll("[data-part], [data-label]").forEach(function (el) {
      zones.push({ el: el, part: el.getAttribute("data-part"), label: el.getAttribute("data-label") });
    });
  }
  function headFor(z, f) {
    if (z.fn) return z.fn(f);
    if (z.label) {
      if (z.el.id === "backup") return { lba: TOTAL - 1, label: z.label };
      return { lba: null, label: z.label };
    }
    var k = z.part, p;
    if (!installed && (k === "aur" || k === "aboot")) {
      // before the restart, this stretch is still the empty end of C:
      p = k === "aur" ? { first: AUR_FIRST, last: AUR_LAST } : { first: ABOOT_FIRST, last: ABOOT_LAST };
      return { lba: p.first + f * (p.last - p.first), label: "inside C:, unused" };
    }
    p = PARTS[k];
    return { lba: p.first + f * (p.last - p.first), label: "partition " + p.n + " · " + p.name };
  }
  var lastHead = "", headTag = headEl.querySelector("i");
  // a short name for the narrow margin
  function shortLabel(l) {
    return l.replace(/^partition (\d) · /, "p$1 · ").replace(/^drive firmware · /, "firmware · ").replace(/ · last LBA$/, "");
  }
  function moveHead() {
    var line = window.innerHeight * 0.4, z = null, f = 0;
    for (var i = 0; i < zones.length; i++) {
      var r = zones[i].el.getBoundingClientRect();
      if (r.top <= line && r.bottom > line) { z = zones[i]; f = (line - r.top) / Math.max(1, r.height); break; }
    }
    if (!z) z = zones[0];
    var h = headFor(z, Math.max(0, Math.min(1, f)));
    var num = h.lba == null ? "parked" : fmtLBA(h.lba);
    var key = num + h.label;
    if (key === lastHead) return;
    lastHead = key;
    headTag.textContent = h.lba == null ? "head" : "LBA";
    hLBA.textContent = num;
    hPart.textContent = shortLabel(h.label);
    mLBA.textContent = h.lba == null ? "head parked" : "LBA " + num;
    mPart.textContent = h.label;
    // a real head travels to the sector; when it reads the drive's own
    // firmware it parks on its ramp, off the platter
    headEl.classList.toggle("parked", h.lba == null);
    headEl.style.top = (h.lba == null ? 3 : vhOf(h.lba)).toFixed(2) + "vh";
  }
  function vhOf(lba) { return 6 + 88 * (lba / TOTAL); }

  /* the margin also carries my partition map, to the same linear scale */
  var rmap = document.createElement("div");
  rmap.className = "rmap";
  document.querySelector(".ruler").appendChild(rmap);
  function drawRuler() {
    rmap.innerHTML = "";
    var keys = installed ? ["efi", "msr", "win", "aur", "aboot", "rec"] : ["efi", "msr", "win", "rec"];
    keys.forEach(function (k) {
      var p = PARTS[k], b = document.createElement("i");
      b.style.top = vhOf(p.first).toFixed(3) + "vh";
      b.style.height = "max(2px, " + (88 * (p.last - p.first + 1) / TOTAL).toFixed(3) + "vh)";
      b.style.background = "var(" + p.ink + ")";
      b.title = p.name;
      rmap.appendChild(b);
    });
    ["0", fmtLBA(TOTAL - 1)].forEach(function (t, j) {
      var e = document.createElement("span");
      e.textContent = t;
      e.style.top = (j ? 94.6 : 4.2) + "vh";
      rmap.appendChild(e);
    });
  }
  var ticking = false;
  function onScroll() {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(function () { ticking = false; moveHead(); });
  }

  /* ── who is talking: my age ─────────────────────────────────────── */
  var age = 11;
  function sayAge() {
    $("age").textContent = age;
    var bought = 2026 - age, why;
    if (age >= 14) {
      why = "I was bought around " + bought + ". Honest answer: I may be too old for AurOS too. PCs from before about 2012 often start the old way (BIOS), and the installer turns those away before it changes anything.";
    } else if (age >= 8) {
      why = "Not because anything in me broke. Windows 11 won’t install on me: my processor, from around " + bought + ", is older than its list allows.";
    } else {
      why = "I was bought around " + bought + ". I could probably run Windows 11, if whoever uses me wants it. AurOS is for the PCs that can’t, but it runs on me just the same.";
    }
    $("why").textContent = why;
    $("age-down").disabled = age <= 6;
    $("age-up").disabled = age >= 18;
  }
  $("age-down").addEventListener("click", function () { if (age > 6) { age--; sayAge(); } });
  $("age-up").addEventListener("click", function () { if (age < 18) { age++; sayAge(); } });

  function countdown() {
    var now = new Date(), end = new Date(2027, 9, 12);
    var d = Math.ceil((end - now) / 86400000);
    $("days").textContent = d > 1 ? d.toLocaleString("en-US") + " days" : d === 1 ? "1 day" : "no days";
  }

  /* ── grain on my case ───────────────────────────────────────────── */
  function grain() {
    try {
      var c = document.createElement("canvas");
      c.width = c.height = 140;
      var x = c.getContext("2d"), img = x.createImageData(140, 140);
      for (var i = 0; i < img.data.length; i += 4) {
        var v = Math.random() * 255 | 0;
        img.data[i] = img.data[i + 1] = img.data[i + 2] = v;
        img.data[i + 3] = 40;
      }
      x.putImageData(img, 0, 0);
      $("drive").style.setProperty("--grain", "url(" + c.toDataURL() + ")");
    } catch (e) { /* plain case it is */ }
  }

  /* ── stickers: peel them, move them ─────────────────────────────── */
  var says = $("drive-says"), saysDefault = says.innerHTML, peeled = 0;
  function quip(html, ms) {
    says.innerHTML = html;
    clearTimeout(quip.t);
    quip.t = setTimeout(function () { says.innerHTML = currentSays(); }, ms || 3800);
  }
  var PEEL = function () { return [
    "Hey. Those were on straight.",
    "That one held my warranty together. Probably.",
    "Fine. Rearrange me.",
    "Nobody has touched my case in " + age + " years.",
    "You know they’re just stickers. The tests are real, though: scroll down."
  ]; };
  document.querySelectorAll(".sticker").forEach(function (s) {
    var x = 0, y = 0, sx = 0, sy = 0, moving = false;
    s.addEventListener("pointerdown", function (e) {
      moving = true; sx = e.clientX - x; sy = e.clientY - y;
      s.setPointerCapture(e.pointerId);
      s.style.zIndex = 9;
      s.style.transform = "rotate(calc(var(--rot) * -0.4)) scale(1.06)";
    });
    s.addEventListener("pointermove", function (e) {
      if (!moving) return;
      x = e.clientX - sx; y = e.clientY - sy;
      s.style.translate = x + "px " + y + "px";
    });
    function drop() {
      if (!moving) return;
      moving = false;
      s.style.transform = "";
      if (Math.abs(x) + Math.abs(y) > 24 && !s.dataset.peeled) {
        s.dataset.peeled = "1";
        quip(PEEL()[Math.min(peeled++, 4)]);
      }
    }
    s.addEventListener("pointerup", drop);
    s.addEventListener("pointercancel", drop);
  });

  /* ── what's yours: the browser tells me a little ────────────────── */
  function yours() {
    var files = $("files"), out = [];
    function card(title, val) {
      var d = document.createElement("div");
      d.className = "file yours";
      d.innerHTML = '<i class="ic"></i><b></b><span></span>';
      d.querySelector("b").textContent = title;
      d.querySelector("span").textContent = val;
      files.appendChild(d);
    }
    try {
      var lang = navigator.language;
      if (lang) { card("Your language", lang); out.push("language"); }
      var tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
      if (tz) { card("Your time zone", tz.replace(/_/g, " ")); out.push("time zone"); }
      var dpr = window.devicePixelRatio || 1;
      card("Your display scale", Math.round(dpr * 100) + "%");
      out.push("display scale");
    } catch (e) { /* nothing to show */ }
    if (out.length) $("yours-note").textContent =
      "The dashed ones came from your browser, just now. On a real PC, Ferry reads the same things from Windows: " + out.join(", ") + ", plus the wallpaper and keyboard layout.";
  }

  /* ── what I say under the drive ─────────────────────────────────── */
  function currentSays() {
    if (!installed) return saysDefault;
    if (answer === "yes") return "<b>AurOS starts from now on.</b> Windows is still in its own partition, smaller, every file untouched. &ldquo;Put Windows back&rdquo; is further down.";
    if (answer === "no") return "<b>Windows starts next time.</b> AurOS is still on my drive and nothing has been deleted. &ldquo;Put Windows back&rdquo; gives Windows its space back.";
    return "<b>AurOS is on me now.</b> Two new partitions, in space Windows wasn’t using. Switching me on still starts Windows until someone answers Yes.";
  }

  /* ── the one restart ────────────────────────────────────────────── */
  var stage = $("stage"), stTitle = $("st-title"), stPara = $("st-para"), stBar = $("st-bar"),
      stPct = $("st-pct"), stSteps = $("st-steps"), stWarn = $("st-warn"), stTail = $("st-tail");
  var STEPS = ["Checking this computer", "Saving the way back to Windows", "Making room on the Windows drive",
               "Copying AurOS", "Setting up how it starts", "Checking AurOS works here", "Finishing"];
  // [para, share of the time, tail lines, power-cut instants in order]
  var PHASES = [
    { para: "Getting ready. Nothing on this computer has been changed yet.", w: 10,
      tail: ["secure   Secure Boot on; kernel lockdown integrity", "record   no memory stick: an interrupted install will start again from the beginning",
             "drive    healthy: no reallocated or pending sectors", "power    on mains", "image    desktop, 6.0 GiB", "checking the copy of AurOS on the Windows drive",
             "measuring how small the Windows drive can get", "reading every sector of the space that would be reclaimed", "Everything AurOS can check has been checked."],
      cuts: ["gate"] },
    { para: "Saving this computer’s Windows startup, so it can be put back.", w: 7,
      tail: ["saved    34 MB, read back and checked"], cuts: ["capture-mid"] },
    { para: "Making room on the Windows drive. This is the only part that cannot be undone. Do not turn the computer off.", w: 14,
      tail: ["windows  checking the Windows drive before it changes", "windows  moving nothing: the space at the end is unused", "windows  the Windows drive is now " + ((WIN_LAST_SMALL - WIN_FIRST + 1) / GiB).toFixed(1) + " GiB",
             "saved    the way back is on this computer, read back and checked"], cuts: ["shrink-begin", "shrink-end"] },
    { para: "Copying AurOS onto this computer.", w: 34,
      tail: ["copy     writing AUROS-ROOT", "copy     checking every block against the image", "copy     6.0 GiB written and read back"], cuts: ["write-mid", "write-end"] },
    { para: "Making this computer able to start AurOS.", w: 11,
      tail: ["boot     \\EFI\\AurOS\\shimx64.efi (signed by Microsoft)", "boot     \\EFI\\AurOS\\grubx64.efi (signed by Canonical)", "boot     start-up menu: AurOS, Windows, Put Windows back"], cuts: ["boot-mid", "boot-end"] },
    { para: "Checking that this computer works under AurOS.", w: 10,
      tail: ["checks   screen, keyboard, network, storage: all found"], cuts: [] },
    { para: "Writing the new layout.", w: 14,
      tail: ["layout   partition array", "layout   primary table", "layout   backup table", "layout   start-up entry added; Windows stays first", "handing over to the system on AUROS-ROOT, in this same boot"],
      cuts: ["commit-array", "commit-sector", "commit-backup", "boot-entry", "settle-end"] }
  ];
  var RESTORE = [
    { para: "Checking the copy of how Windows started that was saved before AurOS went on.", w: 30, tail: ["saved    34 MB, read back and checked against its fingerprint"], cuts: [] },
    { para: "Putting Windows and its drive back exactly as they were.", w: 55,
      tail: ["layout   partition array", "layout   primary table", "layout   backup table", "efi      \\EFI\\AurOS removed", "windows  the Windows drive is its full size again"],
      cuts: ["restore-array", "restore-sector", "restore-backup", "restore-esp-mid", "restore-grow"] },
    { para: "Windows is back. This computer will switch itself off; switch it on again and Windows starts.", w: 15, tail: [], cuts: [] }
  ];

  var run = null;         // the running restart, if any
  var speed = 1;
  var answer = null;
  var savedWin = null;    // Windows' section, saved before anything changes

  function lockScroll() {
    var any = ["stage", "dark", "after", "ask"].some(function (id) { return $(id).classList.contains("on"); });
    document.body.style.overflow = any ? "hidden" : "";
  }

  function sha256(text) {
    if (!(window.crypto && crypto.subtle)) return Promise.resolve(null);
    return crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)).then(function (buf) {
      return Array.prototype.map.call(new Uint8Array(buf), function (b) { return b.toString(16).padStart(2, "0"); }).join("");
    });
  }
  function winText() { return $("p-win").innerHTML; }

  function openStage(title, labels) {
    stage.classList.remove("stopped");
    stTitle.textContent = title;
    stSteps.innerHTML = "";
    labels.forEach(function (l) { var li = document.createElement("li"); li.textContent = l; stSteps.appendChild(li); });
    stWarn.classList.remove("on");
    stTail.innerHTML = "";
    stBar.style.width = "0%";
    stPct.innerHTML = "&nbsp;";
    $("st-fast").textContent = "Faster";
    speed = 1;
    stage.classList.add("on");
    lockScroll();
    $("power").classList.add("live");
    $("power-tip").textContent = "Pull my plug now. Go on.";
    $("st-plug").focus({ preventScroll: true });
  }
  function closeStage() {
    stage.classList.remove("on");
    lockScroll();
    $("power").classList.remove("live");
    $("power-tip").textContent = "My power button. Go on.";
  }
  function tail(line) {
    var d = document.createElement("div");
    d.textContent = line;
    stTail.appendChild(d);
    while (stTail.children.length > 5) stTail.removeChild(stTail.firstChild);
  }

  // Plays a list of phases. onTick(phaseIndex, fractionInPhase) lets the
  // caller move the bar; resolves when done, or never if the plug is pulled.
  function play(kind, phases, riskyFrom, onTick, onDone) {
    var totalW = phases.reduce(function (a, p) { return a + p.w; }, 0);
    var DURATION = reduce ? 9000 : 48000;   // ms, at speed 1
    var t = 0, last = performance.now(), pi = -1, shown = 0;
    bar.classList.add("running");
    run = { kind: kind, phase: 0, frac: 0, phases: phases, stopped: false };
    function frame(now) {
      if (!run || run.stopped) return;
      t += (now - last) * speed;
      last = now;
      var acc = 0, i = 0, f = 0, x = Math.min(1, t / DURATION) * totalW;
      for (i = 0; i < phases.length; i++) {
        if (x < acc + phases[i].w || i === phases.length - 1) { f = Math.min(1, (x - acc) / phases[i].w); break; }
        acc += phases[i].w;
      }
      if (i !== pi) {
        pi = i; shown = 0;
        stPara.textContent = phases[i].para;
        Array.prototype.forEach.call(stSteps.children, function (li, j) {
          li.className = j < i ? "done" : j === i ? "now" : "";
        });
        if (riskyFrom != null && i >= riskyFrom) stWarn.classList.add("on");
        if (i >= 0) tail("");
      }
      var want = Math.ceil(f * phases[i].tail.length);
      while (shown < want) tail(phases[i].tail[shown++]);
      var pct = Math.min(100, Math.floor(x / totalW * 100));
      stBar.style.width = pct + "%";
      stPct.textContent = pct + "%";
      run.phase = i; run.frac = f;
      if (onTick) onTick(i, f);
      if (t >= DURATION) {
        Array.prototype.forEach.call(stSteps.children, function (li) { li.className = "done"; });
        var r = run; run = null;
        bar.classList.remove("running");
        setTimeout(function () { onDone(r); }, reduce ? 300 : 1100);
        return;
      }
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(function (now) { last = now; frame(now); });
  }

  // the bar follows the install
  function installTick(i, f) {
    var shrink = 0, written = 0, scanAt = null;
    if (i === 0) scanAt = barX("win", 0.6 + 0.4 * f);                       // reading the end of C:
    if (i === 1) scanAt = barX("efi", f);                                    // saving the way back
    if (i >= 2) shrink = i === 2 ? f : 1;
    if (i === 2) scanAt = barX("win", 1);
    if (i >= 3) written = i === 3 ? f : 1;
    if (i === 3) scanAt = barX("aur", f);
    if (i === 4) scanAt = barX("aboot", f);
    if (i === 5) scanAt = barX("aur", 0.5);
    if (i === 6) scanAt = f < 0.6 ? 0.01 : 0.995;                            // the table, then its backup
    var winLast = Math.round(WIN_LAST_FULL - shrink * (AUR_SIZE + ABOOT_SIZE));
    var s = sizesFor(winLast, false);
    s.aur = Math.round(shrink * AUR_SIZE);
    s.aboot = Math.round(shrink * ABOOT_SIZE);
    drawBar(s, { filling: true, written: written, bootEmpty: i < 4 });
    bar.classList.toggle("busy", scanAt != null);
    if (scanAt != null) segs.scan.style.left = (scanAt * 100).toFixed(2) + "%";
  }

  $("restart").addEventListener("click", function () {
    if (installed) { location.hash = "#p-back"; return; }
    sha256(winText()).then(function (h) {
      savedWin = winText();
      $("h-before").textContent = h || "(this browser cannot compute SHA-256)";
      $("h-after").textContent = "—";
      $("h-verdict").textContent = "—";
      $("h-verdict").className = "";
      openStage("Installing AurOS", STEPS);
      play("install", PHASES, 2, installTick, finishInstall);
    });
  });
  $("st-fast").addEventListener("click", function () {
    speed = speed === 1 ? 5 : 1;
    this.textContent = speed === 1 ? "Faster" : "Normal speed";
  });

  function finishInstall() {
    closeStage();
    bar.classList.remove("busy");
    installed = true;
    answer = null;
    setWinLast(WIN_LAST_SMALL);
    document.body.classList.add("installed");
    drawBar(sizesFor(WIN_LAST_SMALL, true), { written: 1 });
    drawTable(true);
    drawRuler();
    bar.classList.remove("lba1-flash"); void bar.offsetWidth; bar.classList.add("lba1-flash");
    document.querySelectorAll("#files .file:not(.yours)").forEach(function (f) { f.classList.add("carried"); });
    $("restart").textContent = "Put Windows back";
    says.innerHTML = currentSays();
    $("putback-say").textContent = "";
    $("ask").classList.add("on");
    lockScroll();
    var first = document.querySelector("#ask [data-answer='later']");
    if (first) first.focus({ preventScroll: true });
    collectZones(); lastHead = ""; moveHead();
  }

  document.querySelectorAll("#ask [data-answer]").forEach(function (b) {
    b.addEventListener("click", function () {
      var a = b.getAttribute("data-answer");
      answer = a === "later" ? null : a;
      $("ask").classList.remove("on");
      lockScroll();
      says.innerHTML = currentSays();
      if (a === "later") quip("Take your time. I’ll ask again next time I start.", 4200);
      $("drive").scrollIntoView({ behavior: reduce ? "auto" : "smooth", block: "center" });
    });
  });

  /* ── put Windows back ───────────────────────────────────────────── */
  var armed = 0;
  $("putback").addEventListener("click", function () {
    var now = Date.now();
    if (!armed || now - armed > 8000) {
      armed = now;
      this.textContent = "Yes, put Windows back";
      $("putback-say").textContent = "AurOS and its files go. Windows gets all of its drive back. Press again to go ahead.";
      return;
    }
    if (now - armed < 1500) return;   // the real one won't take a double-click as a yes either
    armed = 0;
    this.textContent = "Put Windows back";
    $("putback-say").textContent = "";
    openStage("Putting Windows back", ["Checking the saved copy", "Putting Windows back", "Done"]);
    play("restore", RESTORE, 1, restoreTick, finishRestore);
  });
  function restoreTick(i, f) {
    var grow = i === 0 ? 0 : i === 1 ? f : 1;
    var s = sizesFor(Math.round(WIN_LAST_SMALL + grow * (AUR_SIZE + ABOOT_SIZE)), false);
    s.aur = Math.round((1 - grow) * AUR_SIZE);
    s.aboot = Math.round((1 - grow) * ABOOT_SIZE);
    drawBar(s, { written: 1 });
    bar.classList.toggle("busy", i < 2);
    segs.scan.style.left = ((i === 0 ? barX("aboot", f) : barX("win", 1)) * 100).toFixed(2) + "%";
  }
  function restoreWindows() {
    installed = false;
    answer = null;
    setWinLast(WIN_LAST_FULL);
    document.body.classList.remove("installed");
    if (savedWin != null) $("p-win").innerHTML = savedWin;
    drawBar(sizesFor(WIN_LAST_FULL, false), { written: 0 });
    drawTable(false);
    drawRuler();
    bar.classList.remove("busy");
    $("restart").textContent = "Restart me into AurOS";
    says.innerHTML = saysDefault;
    collectZones(); lastHead = ""; moveHead();
    return sha256(winText()).then(function (h) {
      var before = $("h-before").textContent;
      $("h-after").textContent = h || "(this browser cannot compute SHA-256)";
      var same = h && h === before;
      $("h-verdict").textContent = same ? "identical: Windows is exactly as it was" : "different";
      $("h-verdict").className = same ? "same" : "diff";
      return same;
    });
  }
  function finishRestore() {
    closeStage();
    restoreWindows().then(function () {
      $("p-back").scrollIntoView({ behavior: reduce ? "auto" : "smooth", block: "start" });
      quip("I’m back to how I was. Check the fingerprints below.", 6000);
    });
  }

  /* ── my power button ────────────────────────────────────────────── */
  // What the test found at each instant, in docs/results/powercut.txt:
  // "the power goes at the worst possible instant, does Windows come back?"
  var UNTOUCHED = ["gate", "capture-mid", "shrink-begin"];
  function outcome(cut, kind) {
    var from = "docs/results/powercut.txt · ── " + cut + " · tested with a memory stick (v3); 138 of 138 checks pass";
    if (kind === "restore") return {
      k: "Power cut during “Put Windows back”, at " + cut,
      title: "Running it again finishes the job.",
      text: "This is the worse half, and the tests treat it that way: switched back on, Put Windows back runs again from where it was and Windows comes back exactly as it was.",
      from: from, then: "putback" };
    if (UNTOUCHED.indexOf(cut) >= 0) return {
      k: "Power cut at " + cut,
      title: "My drive is byte-for-byte what it was.",
      text: "Nothing had been changed yet" + (cut === "capture-mid" ? ", and the half-written saved copy is refused rather than trusted" : "") + ". Switched back on, Windows starts as usual. Try again whenever you like.",
      from: from, then: "reset" };
    return {
      k: "Power cut at " + cut,
      title: "Every file in Windows is still exactly what it was.",
      text: "And Windows can be put back: the test does it after every cut, then checks the table, all three original partitions, Windows’ full size and the EFI partition, byte for byte. Here it puts Windows back now, and the fingerprint below proves it.",
      from: from, then: "reset" };
  }
  var pending = null;
  $("power").addEventListener("click", powerCut);
  $("st-plug").addEventListener("click", powerCut);
  function powerCut() {
    var dark = $("dark");
    if (dark.classList.contains("on")) { powerOn(); return; }
    if (run) {
      var r = run, ph = r.phases[r.phase];
      run.stopped = true; run = null;
      bar.classList.remove("running");
      var cut;
      if (ph.cuts.length) cut = ph.cuts[Math.min(ph.cuts.length - 1, Math.floor(r.frac * ph.cuts.length))];
      else cut = r.kind === "install" ? "boot-end" : "restore-array";
      pending = outcome(cut, r.kind);
      closeStage();
    } else {
      pending = null;
    }
    $("ow").textContent = pending ? "Ow." : "Off.";
    dark.classList.add("on");
    lockScroll();
    $("power").classList.add("live");
    $("power-tip").textContent = "Switch me back on.";
    $("power").focus({ preventScroll: true });
  }
  function powerOn() {
    $("dark").classList.remove("on");
    $("power").classList.remove("live");
    $("power-tip").textContent = "My power button. Go on.";
    if (!pending) {
      lockScroll();
      quip(installed ? "Still here. Windows first, until someone says Yes." : "Still here. That’s all an off switch should do.", 3600);
      return;
    }
    var o = pending; pending = null;
    $("af-k").textContent = o.k;
    $("af-title").textContent = o.title;
    $("af-text").textContent = o.text;
    $("af-from").textContent = o.from;
    $("after").classList.add("on");
    $("af-go").focus({ preventScroll: true });
    $("af-go").onclick = function () {
      $("after").classList.remove("on");
      lockScroll();
      restoreWindows().then(function () {
        if ($("h-before").textContent.charAt(0) !== "—")
          quip("Back to how I was. The fingerprints are in partition 6’s section.", 5200);
      });
    };
  }
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape" && $("after").classList.contains("on")) $("af-go").click();
  });

  /* ── the download, only for someone with a spare PC ─────────────── */
  var dl = $("dl"), boxes = [$("c1"), $("c2"), $("c3")];
  function gate() {
    var ok = boxes.every(function (b) { return b.checked; });
    if (ok) { dl.removeAttribute("aria-disabled"); dl.tabIndex = 0; }
    else { dl.setAttribute("aria-disabled", "true"); dl.tabIndex = -1; }
  }
  boxes.forEach(function (b) { b.addEventListener("change", gate); });
  dl.addEventListener("click", function (e) {
    if (dl.getAttribute("aria-disabled") === "true") { e.preventDefault(); boxes.filter(function (b) { return !b.checked; })[0].focus(); }
  });

  /* ── start ──────────────────────────────────────────────────────── */
  buildBar();
  drawBar(sizesFor(WIN_LAST_FULL, false), { written: 0 });
  drawTable(false);
  drawRuler();
  sayAge();
  countdown();
  grain();
  yours();
  gate();
  collectZones();
  moveHead();
  window.addEventListener("scroll", onScroll, { passive: true });
  window.addEventListener("resize", function () { lastHead = ""; onScroll(); });
})();
