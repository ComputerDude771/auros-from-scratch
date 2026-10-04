/* desktop.js — a working model of the AurOS desktop.
 *
 * What is real here: the colours (assets/themes.js, copied from
 * themes/*.theme), the wallpapers (assets/wall.js, a port of
 * src/common/wall.c, drawn at seed 0 exactly as AurOS draws them), the
 * layout ("Everything in a row", shells/rail.shell, which the desktop
 * image ships with), the app list and its words (shellcommon.c), the
 * first-boot panel and its words (welcome.c) and the Settings rows and
 * the two-press "Put Windows back" rule (settings.c).
 *
 * What is not: everything inside the apps. They are stand-ins.
 */
(function () {
  "use strict";
  var root = document.querySelector("[data-demo]");
  if (!root || !window.AUROS_THEMES) return;
  var THEMES = window.AUROS_THEMES;
  var W = 1024, H = 600;
  var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ── tiny DOM helper ─────────────────────────────────────────────── */
  function h(tag, attrs) {
    var el = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      var v = attrs[k];
      if (v == null || v === false) continue;
      if (k === "class") el.className = v;
      else if (k === "text") el.textContent = v;
      else if (k === "html") el.innerHTML = v;
      else if (k.slice(0, 2) === "on") el.addEventListener(k.slice(2), v);
      else el.setAttribute(k, v === true ? "" : v);
    }
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      if (c == null || c === false) continue;
      if (Array.isArray(c)) c.forEach(function (x) { if (x != null && x !== false) el.appendChild(typeof x === "string" ? document.createTextNode(x) : x); });
      else el.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
    }
    return el;
  }

  /* ── icons: one ink, one stroke ──────────────────────────────────── */
  var ICON = {
    web: '<circle cx="12" cy="12" r="9"/><ellipse cx="12" cy="12" rx="4" ry="9"/><path d="M3 12h18"/>',
    mail: '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M3.5 7l8.5 6 8.5-6"/>',
    photo: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="8.5" cy="9.5" r="1.8"/><path d="M21 16l-5.5-5L5 20"/>',
    files: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
    write: '<rect x="5" y="3" width="14" height="18" rx="2"/><path d="M8.5 8h7M8.5 12h7M8.5 16h4"/>',
    music: '<path d="M9 18V5l11-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="17" cy="16" r="3"/>',
    calc: '<rect x="5" y="3" width="14" height="18" rx="2"/><rect x="8" y="6" width="8" height="4" rx="1"/><path d="M8.5 14h.01M12 14h.01M15.5 14h.01M8.5 17.5h.01M12 17.5h.01M15.5 17.5h.01"/>',
    set: '<path d="M4 7h16M4 17h16"/><circle cx="9" cy="7" r="2.4"/><circle cx="15" cy="17" r="2.4"/>',
    help: '<circle cx="12" cy="12" r="9"/><path d="M9.6 9.4a2.5 2.5 0 1 1 3.4 2.4c-.6.3-1 .8-1 1.5v.5M12 17h.01"/>',
    close: '<path d="M6 6l12 12M18 6L6 18"/>',
    back: '<path d="M15 5l-7 7 7 7"/>'
  };
  function icon(name, size) {
    var s = size || 24;
    var span = h("span", { class: "d-icon", "aria-hidden": "true" });
    span.innerHTML = '<svg viewBox="0 0 24 24" width="' + s + '" height="' + s + '" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">' + ICON[name] + "</svg>";
    return span;
  }

  /* src/aurshell/shellcommon.c, shell_seed_apps(), word for word */
  var APPS = [
    { id: "web", name: "Internet", hint: "Browse the web" },
    { id: "mail", name: "Email", hint: "Read your messages" },
    { id: "photo", name: "Photos", hint: "Pictures and videos" },
    { id: "files", name: "My Files", hint: "Documents you saved" },
    { id: "write", name: "Writing", hint: "Letters and notes" },
    { id: "music", name: "Music", hint: "Songs and radio" },
    { id: "calc", name: "Calculator", hint: "Do sums" },
    { id: "set", name: "Settings", hint: "Change how this works" },
    { id: "help", name: "Help", hint: "Show me how" }
  ];
  function app(id) { for (var i = 0; i < APPS.length; i++) if (APPS[i].id === id) return APPS[i]; return null; }

  /* ── state ───────────────────────────────────────────────────────── */
  var S;
  function fresh(themeId) {
    return {
      theme: themeId || (S && S.theme) || "nocturne",
      pages: ["home"],          /* the rail: home, then each app opened */
      cur: 0,
      homeSel: 0,
      welcome: "ask",           /* ask | working | confirmed | importing | imported | declined | null */
      imported: false,
      bright: 100, volume: 60, words: 100,
      setPage: null,            /* null = the list of rows */
      armedAt: 0,               /* when Remove was first pressed */
      armMsg: "",
      restoring: false,
      toast: "",
      writing: "",
      calc: { shown: "0", acc: null, op: null, fresh: true }
    };
  }
  S = fresh("nocturne");

  /* ── the theme ───────────────────────────────────────────────────── */
  var wallCanvas = h("canvas", { class: "d-wall", width: W, height: H, "aria-hidden": "true" });
  var wallCache = {};
  function applyTheme() {
    var t = THEMES[S.theme];
    var map = { bg: t.bg, "bg-alt": t.bg_alt, surface: t.surface, "surface-hi": t.surface_hi, overlay: t.overlay,
                muted: t.muted, subtle: t.subtle, fg: t.fg, "fg-hi": t.fg_hi, accent: t.accent, "accent-alt": t.accent_alt,
                ok: t.ok, warn: t.warn, err: t.err, info: t.info };
    for (var k in map) root.style.setProperty("--d-" + k, map[k]);
    root.style.setProperty("--d-radius", t.radius + "px");
    root.style.setProperty("--d-radius-sm", t.radius_sm + "px");
    root.style.setProperty("--d-gap", t.gap + "px");
    root.style.setProperty("--d-bar", t.bar_height + "px");
    root.style.setProperty("--d-anim", (reduce ? 0 : t.animation_ms) + "ms");
    root.setAttribute("data-variant", t.variant);
    var ctx = wallCanvas.getContext("2d");
    if (wallCache[t.id]) { ctx.putImageData(wallCache[t.id], 0, 0); }
    else if (window.AurWall) {
      window.AurWall.renderAsync(W, H, window.AurWall.paramsOf(t), 0).then(function (r) {
        var img = new ImageData(r.pixels, W, H);
        wallCache[t.id] = img;
        if (S.theme === t.id) ctx.putImageData(img, 0, 0);
      });
    }
    paintThemeFile(t);
    Array.prototype.forEach.call(document.querySelectorAll("[data-theme-chips] button"), function (b) {
      b.setAttribute("aria-pressed", b.getAttribute("data-id") === t.id ? "true" : "false");
    });
  }

  function paintThemeFile(t) {
    var pre = document.querySelector("[data-theme-file]");
    if (!pre) return;
    var lines = [
      '# ' + t.name + ' — ' + t.description,
      t.id === "nocturne" ? "" : 'inherit="nocturne"',
      'theme_variant="' + t.variant + '"',
      '',
      'bg="' + t.bg + '"          surface="' + t.surface + '"',
      'fg="' + t.fg + '"          subtle="' + t.subtle + '"',
      'overlay="' + t.overlay + '"     # the hairlines are the structure',
      'accent="' + t.accent + '"      # the one signal: where you are',
      '',
      'radius="' + t.radius + '"  gap="' + t.gap + '"  bar_height="' + t.bar_height + '"',
      '',
      'wall_style="' + t.wall_style + '"',
      'wall_c1="' + t.wall_c1 + '"  wall_c2="' + t.wall_c2 + '"',
      'wall_c3="' + t.wall_c3 + '"  wall_c4="' + t.wall_c4 + '"',
      'wall_intensity="' + t.wall_intensity.toFixed(2) + '"  wall_grain="' + t.wall_grain.toFixed(2) + '"  wall_vignette="' + t.wall_vignette.toFixed(2) + '"'
    ].filter(function (l, i) { return !(i === 1 && l === ""); });
    pre.textContent = lines.join("\n");
    var tt = document.querySelector("[data-theme-title]"); if (tt) tt.textContent = t.name;
    var tp = document.querySelector("[data-theme-path]"); if (tp) tp.textContent = "themes/" + t.id + ".theme";
  }

  /* ── the bar ─────────────────────────────────────────────────────── */
  var clockEl = h("span", { class: "d-clock" });
  var dateEl = h("span", { class: "d-date" });
  function tick() {
    var d = new Date();
    var days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    var mon = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    dateEl.textContent = days[d.getDay()] + " " + d.getDate() + " " + mon[d.getMonth()];
    clockEl.textContent = ("0" + d.getHours()).slice(-2) + ":" + ("0" + d.getMinutes()).slice(-2);
  }
  tick(); setInterval(tick, 15000);

  function bar() {
    return h("div", { class: "d-bar" },
      h("span", { class: "d-brand" }, h("span", { class: "d-dot", "aria-hidden": "true" }), "AurOS"),
      h("span", { class: "d-bar-right" },
        h("span", { class: "d-net", title: "Connected" }, h("span", { class: "sr" }, "Connected to the network")),
        h("span", { class: "d-bat" }, "84%"),
        dateEl, clockEl));
  }

  /* ── the rail ────────────────────────────────────────────────────── */
  function go(i) {
    if (i < 0 || i >= S.pages.length) return;
    S.cur = i; render(true);
  }
  function openApp(id) {
    var at = S.pages.indexOf(id);
    if (at < 0) { S.pages.push(id); at = S.pages.length - 1; }
    if (id === "set") { S.setPage = null; }
    S.cur = at; render(true);
  }
  function closeApp(id) {
    var at = S.pages.indexOf(id);
    if (at <= 0) return;
    S.pages.splice(at, 1);
    if (S.cur >= at) S.cur = Math.max(0, S.cur - 1);
    render(true);
  }

  function cardHead(a) {
    return h("div", { class: "d-card-head" },
      icon(a.id, 20), h("span", { class: "d-card-title" }, a.name),
      h("button", { class: "d-close", type: "button", "aria-label": "Close " + a.name, onclick: function () { closeApp(a.id); } }, icon("close", 16)));
  }

  function homeCard() {
    var grid = h("div", { class: "d-grid" });
    APPS.forEach(function (a, i) {
      grid.appendChild(h("button", { class: "d-tile", type: "button", "data-app": a.id, onclick: function () { openApp(a.id); } },
        icon(a.id, 34), h("span", { class: "d-tile-name" }, a.name), h("span", { class: "d-tile-hint" }, a.hint)));
    });
    return h("div", { class: "d-home" },
      h("h2", { class: "d-h" }, "What would you like to do?"),
      h("p", { class: "d-sub" }, "Pick one. You can always come back here."),
      grid);
  }

  /* ── the apps ────────────────────────────────────────────────────── */
  function placeholder(a, lines) {
    return h("div", { class: "d-center" }, icon(a.id, 46), h("p", { class: "d-big" }, a.name),
      lines.map(function (l) { return h("p", { class: "d-quiet" }, l); }));
  }

  function filesBody() {
    var folders = [["Documents", 214], ["Pictures", 1863], ["Music", 412], ["Videos", 37], ["Downloads", 96], ["Desktop", 18]];
    if (!S.imported) {
      return h("div", { class: "d-pad" },
        h("h3", { class: "d-h3" }, "Nothing has been brought across yet"),
        h("p", null, "Your documents, pictures and settings are still on the Windows part of the disk. Bringing them across only reads that part; it never writes to it."),
        h("button", { class: "d-btn d-primary", type: "button", onclick: function () { startImport(true); } }, "Bring my files across"));
    }
    return h("div", { class: "d-pad d-files" },
      h("div", { class: "d-folders" }, folders.map(function (f) {
        return h("div", { class: "d-folder" }, icon("files", 30), h("span", { class: "d-folder-name" }, f[0]),
          h("span", { class: "d-folder-n" }, f[1] + " items, from Windows"));
      })),
      h("div", { class: "d-report" },
        h("h3", { class: "d-h3" }, "What could not come across"),
        h("ul", null,
          h("li", null, h("b", null, "Wi-Fi passwords. "), "Your networks came across; each one asks for its password once."),
          h("li", null, h("b", null, "Passwords saved in Chrome or Edge. "), "Not carried across yet."),
          h("li", null, h("b", null, "OneDrive files that were only online. "), "They were never on this disk."),
          h("li", null, h("b", null, "Windows programs. "), "They do not run here."))),
      h("p", { class: "d-quiet d-note" }, "Sample folders: the demo has none of your files. The list of what cannot come across is the real one."));
  }

  function mailBody() {
    var msgs = [["Ana", "Photos from Sunday", "Here are the ones from the garden, the light was lovely…"],
                ["The library", "Your books are due on Friday", "You can renew them online or at the desk."],
                ["Sam", "Is the old computer still going?", "Mum says you changed what it runs. Does the printer still work?"]];
    return h("div", { class: "d-pad" },
      h("ul", { class: "d-mail" }, msgs.map(function (m, i) {
        return h("li", { class: i === 0 ? "unread" : "" }, h("span", { class: "d-from" }, m[0]), h("span", { class: "d-subj" }, m[1]), h("span", { class: "d-snip" }, m[2]));
      })),
      h("p", { class: "d-quiet d-note" }, "Pretend messages. The model has no email in it."));
  }

  var thumbsDrawn = false;
  function photoBody() {
    var wrap = h("div", { class: "d-thumbs" });
    var specs = THEMES.order.concat(["nocturne", "moss"]);
    specs.forEach(function (id, i) {
      var c = h("canvas", { class: "d-thumb", width: 192, height: 112, "aria-label": THEMES[id].name + " wallpaper, seed " + (i + 3) });
      wrap.appendChild(c);
      if (window.AurWall) {
        var p = window.AurWall.paramsOf(THEMES[id]);
        if (i >= 6) p.style = i === 6 ? "aurora" : "noise";
        window.AurWall.draw(c, 192, 112, p, i + 3);
      }
    });
    return h("div", { class: "d-pad" }, wrap,
      h("p", { class: "d-quiet d-note" }, "Drawn by the wallpaper code a moment ago, standing in for your photographs."));
  }

  function writeBody() {
    var ta = h("textarea", { class: "d-ta", "aria-label": "A letter", spellcheck: "true" });
    ta.value = S.writing || "Dear Sam,\n\nThe old computer is still going. ";
    var count = h("span", { class: "d-quiet" });
    function upd() { S.writing = ta.value; var n = (ta.value.trim().match(/\S+/g) || []).length; count.textContent = n + (n === 1 ? " word" : " words") + ". Nothing here is saved."; }
    ta.addEventListener("input", upd); upd();
    return h("div", { class: "d-pad d-writing" }, ta, count);
  }

  var audio = null;
  function playChord() {
    try {
      audio = audio || new (window.AudioContext || window.webkitAudioContext)();
      var now = audio.currentTime;
      [261.63, 329.63, 392.0, 523.25].forEach(function (f, i) {
        var o = audio.createOscillator(), g = audio.createGain();
        o.type = "sine"; o.frequency.value = f;
        g.gain.setValueAtTime(0, now + i * 0.12);
        g.gain.linearRampToValueAtTime(0.08 * (S.volume / 100), now + i * 0.12 + 0.02);
        g.gain.exponentialRampToValueAtTime(0.0001, now + i * 0.12 + 1.2);
        o.connect(g); g.connect(audio.destination); o.start(now + i * 0.12); o.stop(now + i * 0.12 + 1.3);
      });
    } catch (e) { /* no sound: fine */ }
  }
  function musicBody() {
    var a = app("music");
    return h("div", { class: "d-center" }, icon("music", 46), h("p", { class: "d-big" }, a.name),
      h("p", { class: "d-quiet" }, "No songs in the model. It can play one chord, at the volume Settings says."),
      h("button", { class: "d-btn d-primary", type: "button", onclick: playChord }, "Play a chord"));
  }

  function calcBody() {
    var C = S.calc;
    var disp = h("output", { class: "d-calc-out", "aria-live": "polite" }, C.shown);
    function show(v) { C.shown = v; disp.textContent = v; }
    function num(v) {
      var n = Math.round(v * 1e10) / 1e10;
      if (!isFinite(n)) return "Cannot do that";
      return String(n);
    }
    function apply() {
      var b = parseFloat(C.shown); if (isNaN(b)) b = 0;
      if (C.op && C.acc != null) {
        var a = C.acc, r = C.op === "+" ? a + b : C.op === "−" ? a - b : C.op === "×" ? a * b : a / b;
        show(num(r)); C.acc = parseFloat(C.shown);
      } else C.acc = b;
    }
    function press(k) {
      if (/^[0-9]$/.test(k)) { show(C.fresh || C.shown === "0" ? k : (C.shown.length < 14 ? C.shown + k : C.shown)); C.fresh = false; }
      else if (k === ".") { if (C.fresh) { show("0."); C.fresh = false; } else if (C.shown.indexOf(".") < 0) show(C.shown + "."); }
      else if (k === "C") { C.acc = null; C.op = null; C.fresh = true; show("0"); }
      else if (k === "=") { apply(); C.op = null; C.acc = null; C.fresh = true; }
      else { if (!C.fresh) apply(); else if (C.acc == null) C.acc = parseFloat(C.shown) || 0; C.op = k; C.fresh = true; }
    }
    var keys = ["7", "8", "9", "÷", "4", "5", "6", "×", "1", "2", "3", "−", "C", "0", ".", "+", "="];
    var pad = h("div", { class: "d-calc-keys" }, keys.map(function (k) {
      return h("button", { type: "button", class: "d-key" + (k === "=" ? " eq" : "") + (/[÷×−+]/.test(k) ? " op" : ""),
                           "aria-label": k === "C" ? "Clear" : k === "−" ? "minus" : k === "×" ? "times" : k === "÷" ? "divided by" : k === "=" ? "equals" : null,
                           onclick: function () { press(k); } }, k);
    }));
    return h("div", { class: "d-calc" }, disp, pad);
  }

  function helpBody() {
    return h("div", { class: "d-pad" },
      h("h3", { class: "d-h3" }, "Show me how"),
      h("ul", { class: "d-help" },
        h("li", null, h("b", null, "Moving between things. "), "Slide left and right, or use the arrow keys. Nothing is ever hidden: everything open is in the row."),
        h("li", null, h("b", null, "Getting back here. "), "Press Escape, or the × at the top of what you opened."),
        h("li", null, h("b", null, "Starting Windows. "), "Windows is still on this computer. When the computer switches on, AurOS’s start-up menu lists it."),
        h("li", null, h("b", null, "Removing AurOS. "), "Settings, then Put Windows back.")),
      h("p", { class: "d-quiet d-note" }, "Written for this model; AurOS’s own Help is not this text."));
  }

  /* ── Settings (src/aurshell/settings.c) ──────────────────────────── */
  var ROWS = [
    { id: "screen", name: "How bright the screen is", slider: "bright" },
    { id: "sound", name: "How loud the sound is", slider: "volume" },
    { id: "words", name: "How big the words are", slider: "words" },
    { id: "battery", name: "Battery", value: "84%, plugged in" },
    { id: "time", name: "The time and date" },
    { id: "shell", name: "How this computer works", page: true },
    { id: "look", name: "How it looks", page: true },
    { id: "windows", name: "Put Windows back", page: true }
  ];
  var SHELLS = [
    ["rail", "Everything in a row", "Nothing is ever hidden. Slide left and right."],
    ["tiles", "One thing at a time", "A page of big buttons. Press one, it fills the screen."],
    ["taskbar", "The familiar one", "A bar along the bottom with everything you have open."],
    ["dock", "Favourites along the edge", "Your regular programs always in the same place."],
    ["workbench", "Panes and keyboard", "Windows tile automatically. Fast once learned."]
  ];

  function setBody() {
    if (S.setPage === "look") return setLook();
    if (S.setPage === "shell") return setShell();
    if (S.setPage === "windows") return setWindows();
    var list = h("div", { class: "d-rows" });
    ROWS.forEach(function (r) {
      var right;
      if (r.slider) {
        var lab = r.name;
        var inp = h("input", { type: "range", min: r.slider === "words" ? 80 : 10, max: r.slider === "words" ? 140 : 100, step: r.slider === "words" ? 10 : 5, value: S[r.slider], "aria-label": lab });
        inp.addEventListener("input", function () { S[r.slider] = +inp.value; applyLive(); });
        right = h("span", { class: "d-row-ctl" }, inp);
        list.appendChild(h("div", { class: "d-row" }, h("span", { class: "d-row-name" }, r.name), right));
      } else if (r.page) {
        list.appendChild(h("button", { type: "button", class: "d-row d-row-btn", onclick: function () { S.setPage = r.id; S.armedAt = 0; S.armMsg = ""; render(); focusFirst(); } },
          h("span", { class: "d-row-name" }, r.name), h("span", { class: "d-row-val" }, r.id === "look" ? THEMES[S.theme].name : r.id === "shell" ? "Everything in a row" : "", " ›")));
      } else {
        var v = r.id === "time" ? (dateEl.textContent + ", " + clockEl.textContent) : r.value;
        list.appendChild(h("div", { class: "d-row" }, h("span", { class: "d-row-name" }, r.name), h("span", { class: "d-row-val" }, v)));
      }
    });
    return h("div", { class: "d-pad" }, list);
  }
  function subHead(title) {
    return h("div", { class: "d-subhead" },
      h("button", { type: "button", class: "d-back", onclick: function () { S.setPage = null; S.armedAt = 0; S.armMsg = ""; render(); focusFirst(); } }, icon("back", 16), "Go back"),
      h("h3", { class: "d-h3" }, title));
  }
  function setLook() {
    var list = h("div", { class: "d-choices" });
    THEMES.order.forEach(function (id) {
      var t = THEMES[id];
      if (!THEMES.inSettings[id]) return;
      list.appendChild(h("button", { type: "button", class: "d-choice", "aria-pressed": S.theme === id ? "true" : "false",
          onclick: function () { S.theme = id; applyTheme(); render(); focusFirst(); } },
        h("span", { class: "d-sw" }, [t.bg, t.surface, t.accent, t.fg].map(function (c) { return h("i", { style: "background:" + c }); })),
        h("span", { class: "d-choice-t" }, t.name), h("span", { class: "d-choice-d" }, t.description)));
    });
    return h("div", { class: "d-pad" }, subHead("How it looks"), list,
      h("p", { class: "d-quiet d-note" }, "Ember and Slate are on the disk too, and one command away: aurora set ember."));
  }
  function setShell() {
    var list = h("div", { class: "d-choices" });
    SHELLS.forEach(function (s) {
      list.appendChild(h("button", { type: "button", class: "d-choice", "aria-pressed": s[0] === "rail" ? "true" : "false",
        onclick: function () { if (s[0] !== "rail") toast("Only “Everything in a row” is built into this model."); } },
        h("span", { class: "d-choice-t" }, s[1]), h("span", { class: "d-choice-d" }, s[2])));
    });
    return h("div", { class: "d-pad" }, subHead("How this computer works"), list);
  }
  function setWindows() {
    var removeLabel = S.armedAt ? "Remove AurOS and put Windows back — press again to be sure" : "Remove AurOS and put Windows back";
    return h("div", { class: "d-pad" }, subHead("Put Windows back"),
      h("div", { class: "d-choices" },
        h("button", { type: "button", class: "d-choice", onclick: function () { S.armedAt = 0; S.armMsg = ""; S.setPage = null; render(); focusFirst(); } },
          h("span", { class: "d-choice-t" }, "Keep AurOS"), h("span", { class: "d-choice-d" }, "Nothing changes")),
        h("button", { type: "button", class: "d-choice d-danger" + (S.armedAt ? " armed" : ""), "data-remove": "1", onclick: pressRemove },
          h("span", { class: "d-choice-t" }, removeLabel),
          h("span", { class: "d-choice-d" }, "The computer restarts. Windows gets all of its drive back."))),
      S.armMsg ? h("p", { class: "d-warnline", role: "status" }, S.armMsg) : null,
      h("p", { class: "d-quiet d-note" }, "Everything saved inside AurOS goes with it. Copy what you want to keep onto a memory stick first."));
  }
  function pressRemove() {
    var now = Date.now();
    if (!S.armedAt) {
      S.armedAt = now;
      S.armMsg = "This removes AurOS and everything saved in it. Press the same button again to go ahead, or Keep AurOS.";
      render(); refocus("[data-remove]");
      return;
    }
    if (now - S.armedAt < 1500) {
      S.armMsg = "Too quick: the second press has to come at least a second and a half after the first, so a double-click cannot do this.";
      render(); refocus("[data-remove]");
      return;
    }
    restoreSequence();
  }

  /* ── the restore, as text on a black screen ──────────────────────── */
  function restoreSequence() {
    S.restoring = true; render();
    var scr = root.querySelector(".d-restore-log");
    /* The lines the real restore prints (src/aurstage/rescue.c and
     * install.c), in order, for a machine where it all goes well. */
    var lines = [
      "[ In AurOS the computer restarts here, into the restore. ]",
      "checking the saved copy of this computer's startup",
      "the saved copy is complete and undamaged",
      "putting this computer's original layout back",
      "the original layout is back",
      "putting the Windows startup files back",
      "the Windows startup files are back",
      "making drive 3 its full size again",
      "drive 3 is its full size again",
      "Windows is back exactly as it was. Restart the computer.",
      "aurstage-report v1 verdict=restored record=done",
      "Windows is back. This computer will switch itself off;",
      "switch it on again and Windows starts."
    ];
    var i = 0;
    (function next() {
      if (!S.restoring || !scr) return;
      if (i < lines.length) { scr.appendChild(h("div", null, lines[i++])); setTimeout(next, reduce ? 0 : 520); }
      else { var b = root.querySelector("[data-restore-done]"); if (b) { b.hidden = false; b.focus(); } }
    })();
  }

  /* ── the first-boot question (src/aurshell/welcome.c) ────────────── */
  var WELCOME = {
    ask: { head: "AurOS is on this computer", sub: "Have a look around. Nothing has been decided yet.",
           lines: ["Right now, switching this computer on still starts Windows.", "That stays true until you press Yes.", "This will ask again next time, so there is no hurry."],
           acts: [["Yes, it all works", "yes"], ["Let me look first", "later"], ["No, go back to Windows", "no"]] },
    working: { head: "One moment", sub: "Writing this down.", lines: [], acts: [] },
    confirmed: { head: "AurOS starts from now on", sub: "Windows is still here, and still on the menu when you switch on.",
                 lines: ["Switching this computer on will start AurOS.", "Your documents, pictures and settings are still on the Windows part of the disk."],
                 acts: [["Bring my files across", "import"], ["Not now", "skip"]] },
    importing: { head: "Bringing your files across", sub: "Your Windows drive is only being read, never written to.", lines: [], acts: [] },
    imported: { head: "Your files are here", sub: "Open Files to see them.",
                lines: ["Some things could not be brought across.", "Anything that could not come across is written down in the report."],
                acts: [["Close", "close"]] },
    declined: { head: "Windows will start next time", sub: "Turn this computer off and on again to go back.",
                lines: ["AurOS is still on this disk. Nothing has been deleted.", "You can start it again from the menu when you switch on."],
                acts: [["Close", "close"]] }
  };
  function answer(what) {
    if (what === "later") { S.welcome = null; render(); toast("AurOS will ask again next time."); focusRail(); return; }
    if (what === "yes" || what === "no") {
      S.welcome = "working"; render();
      setTimeout(function () { S.welcome = what === "yes" ? "confirmed" : "declined"; render(); focusWelcome(); }, reduce ? 50 : 700);
      return;
    }
    if (what === "import") { startImport(false); return; }
    if (what === "skip" || what === "close") { S.welcome = null; render(); focusRail(); }
  }
  function startImport(fromFiles) {
    S.welcome = "importing"; render(); focusWelcome();
    var log = root.querySelector(".d-import-log");
    var steps = ["Documents", "Pictures", "Music", "Videos", "Downloads", "Desktop", "Wi-Fi networks (not their passwords)", "Time zone and keyboard"];
    var i = 0;
    (function next() {
      if (S.welcome !== "importing") return;
      if (i < steps.length) { if (log) log.appendChild(h("div", null, "✓ " + steps[i])); i++; setTimeout(next, reduce ? 0 : 260); }
      else { S.imported = true; S.welcome = "imported"; render(); focusWelcome(); }
    })();
  }
  function welcomePanel() {
    var wv = WELCOME[S.welcome];
    if (!wv) return null;
    var body = h("div", { class: "d-wbody" }, wv.lines.map(function (l) { return h("p", null, l); }));
    if (S.welcome === "importing") body.appendChild(h("div", { class: "d-import-log", "aria-live": "polite" }));
    return h("div", { class: "d-welcome", role: "dialog", "aria-modal": "true", "aria-labelledby": "d-wh" },
      h("div", { class: "d-wcard" },
        h("h2", { class: "d-wh", id: "d-wh" }, wv.head),
        h("hr", { class: "d-wrule" }),
        h("p", { class: "d-wsub" }, wv.sub),
        body,
        h("div", { class: "d-wacts" }, wv.acts.map(function (a, i) {
          return h("button", { type: "button", class: "d-btn" + (i === 0 ? " d-primary" : ""), "data-answer": a[1], onclick: function () { answer(a[1]); } }, a[0]);
        }))));
  }

  /* ── toast ───────────────────────────────────────────────────────── */
  var toastTimer = 0;
  function toast(msg) {
    S.toast = msg; render();
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { S.toast = ""; var t = root.querySelector(".d-toast"); if (t) t.remove(); }, 3600);
  }

  /* ── live settings ───────────────────────────────────────────────── */
  function applyLive() {
    var dim = root.querySelector(".d-dim");
    if (dim) dim.style.opacity = String((100 - S.bright) / 100 * 0.75);
    root.style.setProperty("--d-text", (S.words / 100).toFixed(2));
  }

  /* ── render ──────────────────────────────────────────────────────── */
  var CARD_W = 640, CARD_GAP = 56;
  function render(animate) {
    var focused = document.activeElement && root.contains(document.activeElement);
    root.innerHTML = "";
    root.appendChild(wallCanvas);
    root.appendChild(bar());

    var track = h("div", { class: "d-track" });
    S.pages.forEach(function (id, i) {
      var card;
      if (id === "home") card = h("section", { class: "d-card d-home-card", "aria-label": "Home" }, homeCard());
      else {
        var a = app(id), body;
        if (id === "files") body = filesBody();
        else if (id === "mail") body = mailBody();
        else if (id === "photo") body = photoBody();
        else if (id === "write") body = writeBody();
        else if (id === "music") body = musicBody();
        else if (id === "calc") body = calcBody();
        else if (id === "set") body = setBody();
        else if (id === "help") body = helpBody();
        else body = placeholder(a, ["auros.example", "The model has no web inside it."]);
        card = h("section", { class: "d-card", "aria-label": a.name }, cardHead(a), h("div", { class: "d-card-body" }, body));
      }
      if (i === S.cur) card.classList.add("cur");
      else { card.setAttribute("inert", ""); card.setAttribute("aria-hidden", "true"); card.addEventListener("click", function () { go(i); }); }
      card.style.left = (i * (CARD_W + CARD_GAP)) + "px";
      track.appendChild(card);
    });
    var x = (W - CARD_W) / 2 - S.cur * (CARD_W + CARD_GAP);
    track.style.transform = "translateX(" + x + "px)";
    if (!animate) track.style.transition = "none";
    root.appendChild(track);

    var dots = h("div", { class: "d-dots", role: "group", "aria-label": "Pages" });
    S.pages.forEach(function (id, i) {
      dots.appendChild(h("button", { type: "button", class: "d-pdot" + (i === S.cur ? " on" : ""), "aria-label": (id === "home" ? "Home" : app(id).name) + (i === S.cur ? ", showing" : ""), onclick: function () { go(i); } }));
    });
    root.appendChild(dots);

    if (S.toast) root.appendChild(h("div", { class: "d-toast", role: "status" }, S.toast));
    root.appendChild(h("div", { class: "d-dim", "aria-hidden": "true" }));

    var wp = welcomePanel();
    if (wp) { track.setAttribute("inert", ""); dots.setAttribute("inert", ""); root.appendChild(wp); }

    if (S.restoring) {
      root.appendChild(h("div", { class: "d-restore", role: "dialog", "aria-modal": "true", "aria-label": "Putting Windows back" },
        h("div", { class: "d-restore-log", "aria-live": "polite" }),
        h("p", { class: "d-restore-note" }, "This is the model. Nothing was restored, because nothing was installed."),
        h("button", { type: "button", class: "d-btn", "data-restore-done": "1", hidden: true, onclick: function () { S = fresh(S.theme); applyTheme(); render(); focusWelcome(); } }, "Start the model over")));
      track.setAttribute("inert", "");
    }
    applyLive();
    if (focused && !root.contains(document.activeElement)) root.focus({ preventScroll: true });
  }

  function focusFirst() {
    var c = root.querySelector(".d-card.cur");
    var b = c && c.querySelector("button, input, textarea");
    if (b) b.focus({ preventScroll: true });
  }
  function refocus(sel) { var b = root.querySelector(sel); if (b) b.focus({ preventScroll: true }); }
  function focusWelcome() { var b = root.querySelector(".d-welcome button"); if (b) b.focus({ preventScroll: true }); }
  function focusRail() { var t = root.querySelector(".d-card.cur .d-tile, .d-card.cur button"); if (t) t.focus({ preventScroll: true }); }

  /* ── keys ────────────────────────────────────────────────────────── */
  root.addEventListener("keydown", function (e) {
    var tag = (e.target.tagName || "").toLowerCase();
    var typing = tag === "textarea" || (tag === "input" && e.target.type !== "range");
    if (e.key === "Escape") {
      /* As in welcome.c: Escape is "Let me look first", never an answer. */
      if (S.welcome === "ask") { e.preventDefault(); answer("later"); return; }
      if (S.welcome || S.restoring) return;
      var id = S.pages[S.cur];
      if (id === "set" && S.setPage) { S.setPage = null; S.armedAt = 0; S.armMsg = ""; render(); focusFirst(); }
      else if (S.cur > 0) { S.cur = 0; render(true); focusRail(); }
      e.preventDefault();
      return;
    }
    if (typing || S.welcome || S.restoring) return;
    if (tag === "input" && e.target.type === "range") return;
    if (e.key === "ArrowRight" || e.key === "ArrowLeft") {
      var onHome = S.pages[S.cur] === "home" && e.target.classList && e.target.classList.contains("d-tile");
      if (onHome) {
        /* inside the home grid the arrows move between tiles first */
        var tiles = Array.prototype.slice.call(root.querySelectorAll(".d-card.cur .d-tile"));
        var ix = tiles.indexOf(e.target) + (e.key === "ArrowRight" ? 1 : -1);
        if (ix >= 0 && ix < tiles.length) { tiles[ix].focus(); e.preventDefault(); return; }
      }
      var n = S.cur + (e.key === "ArrowRight" ? 1 : -1);
      if (n >= 0 && n < S.pages.length) { go(n); focusFirst(); e.preventDefault(); }
    } else if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      if (e.target.classList && e.target.classList.contains("d-tile")) {
        var tl = Array.prototype.slice.call(root.querySelectorAll(".d-card.cur .d-tile"));
        var j = tl.indexOf(e.target) + (e.key === "ArrowDown" ? 3 : -3);
        if (j >= 0 && j < tl.length) { tl[j].focus(); e.preventDefault(); }
      }
    }
  });
  /* swipe on touch */
  var tx0 = null;
  root.addEventListener("touchstart", function (e) { if (e.touches.length === 1) tx0 = e.touches[0].clientX; }, { passive: true });
  root.addEventListener("touchend", function (e) {
    if (tx0 == null || S.welcome || S.restoring) return;
    var dx = e.changedTouches[0].clientX - tx0; tx0 = null;
    if (Math.abs(dx) > 40) go(S.cur + (dx < 0 ? 1 : -1));
  });

  /* ── the frame: scale 1024x600 into whatever room there is ───────── */
  var frame = document.querySelector("[data-demo-frame]");
  var scroller = document.querySelector("[data-demo-scroll]");
  var zoom = "fit";
  function fit() {
    if (!frame) return;
    var full = document.fullscreenElement === frame;
    var avail = frame.clientWidth;
    var s;
    if (full) s = Math.min(window.innerWidth / W, window.innerHeight / H);
    else s = zoom === "actual" ? 1 : Math.min(1.25, avail / W);
    root.style.transform = "scale(" + s + ")";
    scroller.style.width = full ? (W * s) + "px" : "";
    scroller.style.height = (H * s) + "px";
    scroller.querySelector(".demo").style.marginLeft = "0";
    scroller.classList.toggle("actual", zoom === "actual" && !full);
    var inner = scroller.firstElementChild;
    if (inner) { inner.style.width = W + "px"; inner.style.height = H + "px"; }
    scroller.style.setProperty("--w", (W * s) + "px");
  }
  window.addEventListener("resize", fit);
  document.addEventListener("fullscreenchange", fit);
  Array.prototype.forEach.call(document.querySelectorAll("[data-zoom]"), function (b) {
    b.addEventListener("click", function () {
      zoom = b.getAttribute("data-zoom");
      Array.prototype.forEach.call(document.querySelectorAll("[data-zoom]"), function (o) { o.setAttribute("aria-pressed", o === b ? "true" : "false"); });
      fit();
    });
  });
  var fullBtn = document.querySelector("[data-demo-full]");
  if (fullBtn) {
    if (!frame.requestFullscreen) fullBtn.hidden = true;
    fullBtn.addEventListener("click", function () {
      if (document.fullscreenElement) document.exitFullscreen();
      else frame.requestFullscreen().then(function () { root.focus({ preventScroll: true }); }).catch(function () {});
    });
  }
  var again = document.querySelector("[data-demo-restart]");
  if (again) again.addEventListener("click", function () { S = fresh(S.theme); applyTheme(); render(); focusWelcome(); });

  /* ── theme chips outside the screen ──────────────────────────────── */
  var chips = document.querySelector("[data-theme-chips]");
  if (chips) THEMES.order.forEach(function (id) {
    var t = THEMES[id];
    chips.appendChild(h("button", { type: "button", class: "theme-chip", "data-id": id, "aria-pressed": "false",
        onclick: function () { S.theme = id; applyTheme(); render(); } },
      h("span", { class: "dots", "aria-hidden": "true" }, [t.bg, t.surface, t.accent].map(function (c) { return h("i", { style: "background:" + c }); })),
      t.name));
  });

  applyTheme();
  render();
  fit();
})();
