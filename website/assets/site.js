/* AurOS website — the script every page loads.
 *
 * Progressive enhancement only: with JavaScript off every page still
 * reads the same and every link still goes where it says. Nothing here
 * is loaded from anywhere else, nothing is measured and nothing is sent.
 * The one thing kept is your Nocturne/Sandstone choice, in this
 * browser's own storage, and only if you press the switch.
 */
(function () {
  "use strict";
  var doc = document.documentElement;
  var KEY = "auros-site-theme";

  /* ── theme: Nocturne (dark) or Sandstone (light) ─────────────────── */
  function systemLight() {
    return !!(window.matchMedia && window.matchMedia("(prefers-color-scheme: light)").matches);
  }
  function current() {
    var t = doc.getAttribute("data-theme");
    if (t === "light" || t === "dark") return t;
    return systemLight() ? "light" : "dark";
  }
  function labelToggles() {
    var t = current();
    Array.prototype.forEach.call(document.querySelectorAll("[data-theme-toggle]"), function (b) {
      var name = b.querySelector("[data-theme-name]");
      if (name) name.textContent = t === "light" ? "Sandstone" : "Nocturne";
      b.setAttribute("aria-label", "Colours: " + (t === "light" ? "Sandstone (light)" : "Nocturne (dark)") +
        ". Switch to " + (t === "light" ? "Nocturne (dark)" : "Sandstone (light)"));
    });
  }
  window.AurSite = { theme: current };
  document.addEventListener("click", function (e) {
    var b = e.target.closest && e.target.closest("[data-theme-toggle]");
    if (!b) return;
    var next = current() === "light" ? "dark" : "light";
    doc.setAttribute("data-theme", next);
    try { localStorage.setItem(KEY, next); } catch (err) { /* private window: fine */ }
    labelToggles();
    document.dispatchEvent(new CustomEvent("auros-theme", { detail: next }));
  });
  if (window.matchMedia) {
    var mq = window.matchMedia("(prefers-color-scheme: light)");
    var onSys = function () {
      if (!doc.getAttribute("data-theme")) { labelToggles(); document.dispatchEvent(new CustomEvent("auros-theme", { detail: current() })); }
    };
    if (mq.addEventListener) mq.addEventListener("change", onSys); else if (mq.addListener) mq.addListener(onSys);
  }
  labelToggles();

  /* ── the menu on narrow screens ──────────────────────────────────── */
  var menuBtn = document.querySelector("[data-menu]");
  var nav = document.getElementById("site-nav");
  if (menuBtn && nav) {
    menuBtn.addEventListener("click", function () {
      var open = nav.classList.toggle("open");
      menuBtn.setAttribute("aria-expanded", open ? "true" : "false");
    });
  }

  /* ── days until a date ───────────────────────────────────────────── */
  Array.prototype.forEach.call(document.querySelectorAll("[data-days-until]"), function (el) {
    var parts = el.getAttribute("data-days-until").split("-");
    var end = Date.UTC(+parts[0], +parts[1] - 1, +parts[2]);
    var now = new Date();
    var today = Date.UTC(now.getFullYear(), now.getMonth(), now.getDate());
    var days = Math.round((end - today) / 86400000);
    el.textContent = days > 0 ? days.toLocaleString("en-GB") : "0";
  });
  /* the strip of months under it */
  Array.prototype.forEach.call(document.querySelectorAll("[data-month-strip]"), function (el) {
    var a = el.getAttribute("data-month-strip").split(",");   /* from,to as YYYY-MM */
    var f = a[0].split("-"), t = a[1].split("-");
    var y = +f[0], m = +f[1] - 1, ty = +t[0], tm = +t[1] - 1;
    var now = new Date(), ny = now.getFullYear(), nm = now.getMonth();
    var names = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    el.innerHTML = "";
    while (y < ty || (y === ty && m <= tm)) {
      var s = document.createElement("span");
      if (y < ny || (y === ny && m < nm)) s.className = "past";
      else if (y === ny && m === nm) s.className = "now";
      s.title = names[m] + " " + y;
      el.appendChild(s);
      m++; if (m > 11) { m = 0; y++; }
    }
  });

  /* ── the press sheet on the home page ────────────────────────────── */
  var press = document.querySelector("canvas[data-press]");
  if (press && window.AurWall && window.AUROS_THEMES) {
    var seedOut = document.querySelector("[data-press-seed]");
    var keep = document.querySelector("[data-press-keep]");
    var again = document.querySelector("[data-press-again]");
    var seed = 0;
    var newSeed = function () {
      try { var u = new Uint32Array(1); crypto.getRandomValues(u); seed = (u[0] % 999999) + 1; }
      catch (err) { seed = ((Math.random() * 999999) | 0) + 1; }
    };
    var paint = function () {
      var light = current() === "light";
      var theme = window.AUROS_THEMES[light ? "sandstone" : "nocturne"];
      var r = press.getBoundingClientRect();
      var scale = Math.min(window.devicePixelRatio || 1, 1.5);
      var w = Math.max(320, Math.round(r.width * scale)), h = Math.max(240, Math.round(r.height * scale));
      press.setAttribute("aria-label", "A " + theme.name + " wallpaper drawn for this visit with seed " + seed + ".");
      if (seedOut) seedOut.textContent = String(seed);
      if (keep) keep.href = "wallpaper.html#theme=" + theme.id + "&seed=" + seed;
      return window.AurWall.draw(press, w, h, window.AurWall.paramsOf(theme), seed);
    };
    newSeed();
    paint();
    if (again) again.addEventListener("click", function () { newSeed(); paint(); });
    document.addEventListener("auros-theme", paint);
    var lastW = window.innerWidth, timer = 0;
    window.addEventListener("resize", function () {
      if (Math.abs(window.innerWidth - lastW) < 40) return;
      lastW = window.innerWidth;
      clearTimeout(timer); timer = setTimeout(paint, 250);
    });
  }

  /* ── the pre-download checklist gate ─────────────────────────────── */
  var list = document.querySelector("[data-checklist]");
  var gate = document.querySelector("[data-gate]");
  if (list && gate) {
    var boxes = Array.prototype.slice.call(list.querySelectorAll('input[type="checkbox"]'));
    var link = gate.querySelector("[data-gate-link]");
    var count = gate.querySelector("[data-gate-count]");
    var update = function () {
      var done = boxes.filter(function (b) { return b.checked; }).length;
      var all = done === boxes.length;
      if (count) {
        count.textContent = "";
        var b = document.createElement("b");
        b.textContent = done + " of " + boxes.length;
        count.appendChild(b);
        count.appendChild(document.createTextNode(all
          ? " ticked. The link below is live. Read TRY-IT on the way, it is short."
          : " ticked. The link stays greyed until every box is."));
      }
      /* The href is never removed: a link without one drops out of the
         accessibility tree. aria-disabled says the state, and the click
         handler holds the gate. With JavaScript off, it is just a link. */
      if (link) { if (all) link.removeAttribute("aria-disabled"); else link.setAttribute("aria-disabled", "true"); }
    };
    boxes.forEach(function (b) { b.addEventListener("change", update); });
    if (link) link.addEventListener("click", function (e) {
      if (link.getAttribute("aria-disabled") === "true") {
        e.preventDefault();
        var first = boxes.filter(function (b) { return !b.checked; })[0];
        if (first) first.focus();
      }
    });
    update();
  }
})();
