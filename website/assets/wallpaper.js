/* wallpaper.js — the controls on wallpaper.html. The drawing is wall.js. */
(function () {
  "use strict";
  var canvas = document.querySelector("[data-wp-canvas]");
  if (!canvas || !window.AurWall || !window.AUROS_THEMES) return;
  var T = window.AUROS_THEMES, A = window.AurWall;
  var $ = function (s) { return document.querySelector(s); };
  var status = $("[data-wp-status]");

  var state = { theme: "nocturne", seed: 0, p: A.paramsOf(T.nocturne) };

  /* sizes: this screen first, then the usual ones */
  var sizes = [];
  var dpr = window.devicePixelRatio || 1;
  var sw = Math.round(screen.width * dpr), sh = Math.round(screen.height * dpr);
  if (sw > 0 && sh > 0 && sw <= 7680 && sh <= 7680) sizes.push([sw, sh, "This screen (" + sw + "×" + sh + ")"]);
  [[1920, 1080, "1920×1080"], [2560, 1440, "2560×1440"], [3840, 2160, "3840×2160"], [1366, 768, "1366×768, a common old laptop"],
   [1024, 600, "1024×600, the smallest AurOS is built for"], [1080, 2340, "1080×2340, a phone"]].forEach(function (s) { sizes.push(s); });
  var sizeSel = $("[data-wp-size]");
  sizes.forEach(function (s, i) { var o = document.createElement("option"); o.value = String(i); o.textContent = s[2]; sizeSel.appendChild(o); });

  /* theme chips */
  var chips = $("[data-wp-themes]");
  T.order.forEach(function (id) {
    var t = T[id], b = document.createElement("button");
    b.type = "button"; b.className = "theme-chip"; b.setAttribute("data-id", id);
    var dots = document.createElement("span"); dots.className = "dots"; dots.setAttribute("aria-hidden", "true");
    [t.wall_c1, t.wall_c2, t.wall_c3].forEach(function (c) { var i = document.createElement("i"); i.style.background = c; dots.appendChild(i); });
    b.appendChild(dots); b.appendChild(document.createTextNode(t.name));
    b.addEventListener("click", function () { state.theme = id; state.p = A.paramsOf(t); syncForm(); draw(); });
    chips.appendChild(b);
  });

  var seedIn = $("[data-wp-seed]"), styleSel = $("[data-wp-style]");
  function syncForm() {
    Array.prototype.forEach.call(chips.children, function (b) { b.setAttribute("aria-pressed", b.getAttribute("data-id") === state.theme ? "true" : "false"); });
    seedIn.value = String(state.seed);
    styleSel.value = state.p.style;
    ["c1", "c2", "c3", "c4"].forEach(function (k) { $('[data-wp-c="' + k + '"]').value = state.p[k].toLowerCase(); });
    ["intensity", "grain", "vignette"].forEach(function (k) { $('[data-wp-n="' + k + '"]').value = String(state.p[k]); $('[data-wp-o="' + k + '"]').textContent = (+state.p[k]).toFixed(2); });
  }

  function snippet() {
    var p = state.p;
    return ["# wallpaper, from the AurOS wallpaper press" + (state.seed ? " (drawn here at seed " + state.seed + "; AurOS draws seed 0)" : ""),
            'wall_style="' + p.style + '"',
            'wall_c1="' + p.c1.toUpperCase() + '"  wall_c2="' + p.c2.toUpperCase() + '"',
            'wall_c3="' + p.c3.toUpperCase() + '"  wall_c4="' + p.c4.toUpperCase() + '"',
            'wall_intensity="' + (+p.intensity).toFixed(2) + '"  wall_grain="' + (+p.grain).toFixed(3) + '"  wall_vignette="' + (+p.vignette).toFixed(2) + '"'].join("\n");
  }
  function hash() {
    var p = state.p, base = A.paramsOf(T[state.theme]), parts = ["theme=" + state.theme, "seed=" + state.seed];
    ["style", "c1", "c2", "c3", "c4", "intensity", "grain", "vignette"].forEach(function (k) {
      if (String(p[k]).toLowerCase() !== String(base[k]).toLowerCase()) parts.push(k + "=" + encodeURIComponent(String(p[k]).replace("#", "")));
    });
    return "#" + parts.join("&");
  }

  var job = 0;
  function draw() {
    var my = ++job;
    var sz = sizes[+sizeSel.value || 0];
    var w = 960, h = Math.round(960 * sz[1] / sz[0]);
    if (h > 720) { h = 720; w = Math.round(720 * sz[0] / sz[1]); }
    status.textContent = "Drawing…";
    A.draw(canvas, w, h, state.p, state.seed).then(function () {
      if (my !== job) return;
      var t = T[state.theme];
      status.textContent = t.name + " · " + state.p.style + " · seed " + state.seed + (state.seed === 0 ? " (AurOS’s own)" : "");
      canvas.setAttribute("aria-label", "Wallpaper preview: " + t.name + ", " + state.p.style + " style, seed " + state.seed);
    });
    $("[data-wp-snippet]").textContent = snippet();
    var link = $("[data-wp-link]"); link.href = hash();
    try { history.replaceState(null, "", hash()); } catch (e) { /* file:// or sandbox: fine */ }
  }

  function roll() {
    try { var u = new Uint32Array(1); crypto.getRandomValues(u); state.seed = (u[0] % 999999) + 1; }
    catch (e) { state.seed = ((Math.random() * 999999) | 0) + 1; }
  }

  $("[data-wp-roll]").addEventListener("click", function () { roll(); syncForm(); draw(); });
  $("[data-wp-zero]").addEventListener("click", function () { state.seed = 0; syncForm(); draw(); });
  seedIn.addEventListener("change", function () { var v = Math.max(0, Math.min(999999, parseInt(seedIn.value, 10) || 0)); state.seed = v; syncForm(); draw(); });
  styleSel.addEventListener("change", function () { state.p.style = styleSel.value; draw(); });
  Array.prototype.forEach.call(document.querySelectorAll("[data-wp-c]"), function (inp) {
    inp.addEventListener("input", function () { state.p[inp.getAttribute("data-wp-c")] = inp.value.toUpperCase(); draw(); });
  });
  Array.prototype.forEach.call(document.querySelectorAll("[data-wp-n]"), function (inp) {
    var k = inp.getAttribute("data-wp-n"), timer = 0;
    inp.addEventListener("input", function () {
      state.p[k] = +inp.value; $('[data-wp-o="' + k + '"]').textContent = (+inp.value).toFixed(2);
      clearTimeout(timer); timer = setTimeout(draw, 120);
    });
  });
  sizeSel.addEventListener("change", draw);

  $("[data-wp-download]").addEventListener("click", function () {
    var sz = sizes[+sizeSel.value || 0], btn = this;
    btn.setAttribute("aria-disabled", "true");
    status.textContent = "Drawing " + sz[0] + "×" + sz[1] + "…";
    var big = document.createElement("canvas");
    A.draw(big, sz[0], sz[1], state.p, state.seed).then(function () {
      big.toBlob(function (blob) {
        btn.removeAttribute("aria-disabled");
        if (!blob) { status.textContent = "This browser could not make the file."; return; }
        var a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = "auros-" + state.theme + "-" + state.p.style + "-seed" + state.seed + "-" + sz[0] + "x" + sz[1] + ".png";
        document.body.appendChild(a); a.click(); a.remove();
        setTimeout(function () { URL.revokeObjectURL(a.href); }, 4000);
        status.textContent = "Saved " + a.download;
      }, "image/png");
    });
  });

  $("[data-wp-copy]").addEventListener("click", function () {
    var text = snippet(), btn = this;
    var done = function () { btn.textContent = "Copied"; setTimeout(function () { btn.textContent = "Copy these lines"; }, 1800); };
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, function () { select(); });
    else select();
    function select() {
      var r = document.createRange(); r.selectNodeContents($("[data-wp-snippet]"));
      var s = window.getSelection(); s.removeAllRanges(); s.addRange(r);
      btn.textContent = "Selected: press Ctrl+C";
    }
  });

  /* read a link like #theme=moss&seed=42 */
  (function fromHash() {
    var q = {};
    location.hash.replace(/^#/, "").split("&").forEach(function (kv) { var i = kv.indexOf("="); if (i > 0) q[kv.slice(0, i)] = decodeURIComponent(kv.slice(i + 1)); });
    if (q.theme && T[q.theme] && T.order.indexOf(q.theme) >= 0) { state.theme = q.theme; state.p = A.paramsOf(T[q.theme]); }
    if (q.seed) state.seed = Math.max(0, Math.min(999999, parseInt(q.seed, 10) || 0));
    if (q.style && A.styles.indexOf(q.style) >= 0) state.p.style = q.style;
    ["c1", "c2", "c3", "c4"].forEach(function (k) { if (q[k] && /^[0-9a-f]{6}$/i.test(q[k])) state.p[k] = "#" + q[k].toUpperCase(); });
    ["intensity", "grain", "vignette"].forEach(function (k) { if (q[k] != null && isFinite(+q[k])) state.p[k] = Math.max(0, Math.min(1, +q[k])); });
  })();

  syncForm();
  draw();
})();
