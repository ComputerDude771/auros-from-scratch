/* wall.js — AurOS's procedural wallpaper renderer, ported to JavaScript.
 *
 * A line-by-line port of src/common/wall.c: the same value noise, the
 * same hash constants, the same seven styles (letterpress, aurora,
 * mesh, waves, gradient, noise, solid) and the same post-processing
 * (vignette, then grain). C does this in 32-bit floats and JavaScript
 * in 64-bit ones, so the output is the same picture, not the same bytes.
 *
 * ONE ADDITION, and it is labelled as one: a seed. wall.c has none --
 * a theme renders byte-identical every time, on purpose, so builds are
 * reproducible. With seed 0 this port does exactly what wall.c does.
 * With any other seed it adds the seed to every noise seed and moves a
 * few of each style's fixed numbers (ribbon phases, blob positions, the
 * height of a rule) by a small amount, so each seed is its own picture
 * drawn by the same rules. AurOS itself does not do this.
 *
 * Works in a page (window.AurWall) and in a Web Worker (self.AurWall).
 */
(function (root) {
  "use strict";
  var TAU = 6.28318530717958647692;

  function clampf(v, a, b) { return v < a ? a : (v > b ? b : v); }
  function lerpf(a, b, t) { return a + (b - a) * t; }
  function smoothstep(e0, e1, x) {
    var t = clampf((x - e0) / (e1 - e0 + 1e-9), 0, 1);
    return t * t * (3 - 2 * t);
  }
  function unpack(hex) {
    var c = parseInt(String(hex).replace("#", ""), 16);
    return [((c >> 16) & 255) / 255, ((c >> 8) & 255) / 255, (c & 255) / 255];
  }
  function mixc(a, b, t) { return [lerpf(a[0], b[0], t), lerpf(a[1], b[1], t), lerpf(a[2], b[2], t)]; }
  function saturate(c, amt) {
    var l = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    return [clampf(l + (c[0] - l) * amt, 0, 1), clampf(l + (c[1] - l) * amt, 0, 1), clampf(l + (c[2] - l) * amt, 0, 1)];
  }

  /* deterministic value noise -- the constants are wall.c's */
  function hash1i(x, s) {
    var n = (Math.imul(x, 374761393) + Math.imul(s, 668265263)) >>> 0;
    n = Math.imul(n ^ (n >>> 13), 1274126177) >>> 0;
    return ((n ^ (n >>> 16)) & 0xFFFFFF) / 16777215;
  }
  function hash2i(x, y, s) {
    var n = (Math.imul(x, 73856093) ^ Math.imul(y, 19349663) ^ Math.imul(s, 83492791)) >>> 0;
    n = Math.imul(n ^ (n >>> 13), 1274126177) >>> 0;
    return ((n ^ (n >>> 16)) & 0xFFFFFF) / 16777215;
  }
  function noise1(x, s) {
    var i = Math.floor(x), f = x - i;
    f = f * f * (3 - 2 * f);
    return lerpf(hash1i(i, s), hash1i(i + 1, s), f);
  }
  function noise2(x, y, s) {
    var xi = Math.floor(x), yi = Math.floor(y);
    var fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    var a = hash2i(xi, yi, s), b = hash2i(xi + 1, yi, s);
    var c = hash2i(xi, yi + 1, s), d = hash2i(xi + 1, yi + 1, s);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
  }
  function fbm2(x, y, oct, s) {
    var v = 0, amp = 0.5, fr = 1;
    for (var i = 0; i < oct; i++) { v += amp * noise2(x * fr, y * fr, s + i * 31); fr *= 2.03; amp *= 0.5; }
    return v;
  }

  /* The seed's small structural nudges: a reproducible number in
   * [-1, 1] per (seed, slot). Always 0 when seed is 0. */
  function jit(seed, slot) {
    if (!seed) return 0;
    return hash1i(slot * 7919 + 13, seed) * 2 - 1;
  }

  /* fb is a Float32Array of w*h*3 */
  function get(fb, i) { return [fb[i], fb[i + 1], fb[i + 2]]; }
  function put(fb, i, c) { fb[i] = c[0]; fb[i + 1] = c[1]; fb[i + 2] = c[2]; }
  function blend(fb, i, c, t) {
    fb[i] = lerpf(fb[i], c[0], t); fb[i + 1] = lerpf(fb[i + 1], c[1], t); fb[i + 2] = lerpf(fb[i + 2], c[2], t);
  }

  /* ── aurora ─────────────────────────────────────────────────────── */
  function styleAurora(fb, w, h, c, S) {
    var horizon = h * 0.80, x, y, i;
    for (y = 0; y < h; y++) {
      var t = clampf(y / horizon, 0, 1);
      var base = mixc(c.c1, mixc(c.c1, c.c2, 0.62), Math.pow(t, 1.5));
      for (x = 0; x < w; x++) put(fb, (y * w + x) * 3, base);
    }
    for (y = 0; y < (horizon | 0); y++) {
      var fade = 1 - Math.pow(y / horizon, 2.0);
      for (x = 0; x < w; x++) {
        var r = hash2i(x, y, 991 + S);
        if (r > 0.99950) {
          var b = (r - 0.99950) / 0.00050;
          var ii = b * 0.85 * fade; i = (y * w + x) * 3;
          fb[i] = clampf(fb[i] + ii * 0.88, 0, 1);
          fb[i + 1] = clampf(fb[i + 1] + ii * 0.92, 0, 1);
          fb[i + 2] = clampf(fb[i + 2] + ii * 1.00, 0, 1);
        }
      }
    }
    var rib = [
      [0.36, 0.070, 1.7, 0.00, 0.030, 1.00],
      [0.45, 0.095, 1.1, 2.10, 0.055, 0.62],
      [0.29, 0.055, 2.6, 4.30, 0.020, 0.78],
      [0.52, 0.105, 0.8, 1.15, 0.080, 0.26],
      [0.40, 0.045, 3.4, 5.60, 0.014, 0.55]
    ];
    var cols = [
      saturate(c.c3, 1.45),
      saturate(mixc(c.c3, c.c4, 0.55), 1.40),
      saturate(c.c4, 1.45),
      saturate(mixc(c.c2, c.c3, 0.75), 1.30),
      saturate(mixc(c.c4, c.c3, 0.30), 1.40)
    ];
    for (var k = 0; k < 5; k++) {
      rib[k][0] += 0.05 * jit(S, 10 + k);
      rib[k][3] += 3.1 * jit(S, 20 + k);
    }
    for (var q = 0; q < 5; q++) {
      var R = rib[q], col = cols[q];
      for (x = 0; x < w; x++) {
        var tx = x / w;
        var cy = R[0] + R[1] * (0.60 * Math.sin(tx * TAU * R[2] + R[3])
                              + 0.28 * Math.sin(tx * TAU * R[2] * 2.31 + R[3] * 1.7)
                              + 0.12 * Math.sin(tx * TAU * R[2] * 4.07 + R[3] * 2.9));
        cy *= h;
        var th = R[4] * h * (0.62 + 0.38 * noise1(tx * 3.1 + q * 11.3, 17 + S));
        if (th < 1) th = 1;
        var env = smoothstep(0, 0.16, tx) * smoothstep(1, 0.84, tx);
        env *= 0.40 + 0.60 * noise1(tx * 2.4 + q * 5.7, 43 + S);
        var striae = 0.55 + 0.45 * noise1(tx * w * 0.020 + q * 7.1, 71 + S);
        var amt = R[5] * env * striae * c.intensity;
        if (amt <= 0.001) continue;
        var y0 = (cy - th * 4.0) | 0, y1 = (cy + th * 4.0) | 0;
        if (y0 < 0) y0 = 0;
        if (y1 >= h) y1 = h - 1;
        for (y = y0; y <= y1; y++) {
          var d = (y - cy) / th;
          var it = (Math.exp(-d * d) * 0.80 + Math.exp(-d * d * 0.10) * 0.20) * amt;
          if (d < 0) it *= 1 + 0.30 * (-d);
          i = (y * w + x) * 3;
          fb[i] = clampf(fb[i] + col[0] * it, 0, 1);
          fb[i + 1] = clampf(fb[i + 1] + col[1] * it, 0, 1);
          fb[i + 2] = clampf(fb[i + 2] + col[2] * it, 0, 1);
        }
      }
    }
    var farR = mixc(c.c1, [0, 0, 0], 0.30), nearR = mixc(c.c1, [0, 0, 0], 0.68);
    var rimC = saturate(c.c3, 1.2);
    for (x = 0; x < w; x++) {
      var t2 = x / w;
      var nf = fbm2(t2 * 2.6, 0.5, 5, 301 + S);
      var rf = 1 - Math.abs(2 * nf - 1);
      rf = Math.pow(clampf(rf, 0, 1), 0.75);
      var farY = h * 0.815 - h * 0.150 * rf;
      var nn = fbm2(t2 * 1.5 + 11, 2.5, 5, 401 + S);
      var rn = 1 - Math.abs(2 * nn - 1);
      rn = Math.pow(clampf(rn, 0, 1), 0.85);
      var nearY = h * 0.935 - h * 0.115 * rn;
      var sample = clampf(farY - 6, 0, h - 1) | 0;
      var above = get(fb, (sample * w + x) * 3);
      var glow = clampf((above[0] + above[1] + above[2]) / 1.4, 0, 1);
      for (y = farY | 0; y < h; y++) {
        if (y < 0) continue;
        var rim = smoothstep(farY + 3.0, farY - 0.5, y);
        put(fb, (y * w + x) * 3, mixc(farR, rimC, rim * 0.22 * glow));
      }
      for (y = nearY | 0; y < h; y++) {
        if (y < 0) continue;
        put(fb, (y * w + x) * 3, nearR);
      }
    }
  }

  /* ── mesh ───────────────────────────────────────────────────────── */
  function styleMesh(fb, w, h, c, S) {
    var pt = [
      [0.12, 0.16, 0.55], [0.86, 0.10, 0.48], [0.72, 0.78, 0.62],
      [0.22, 0.88, 0.52], [0.52, 0.42, 0.70], [0.95, 0.50, 0.40]
    ];
    for (var j = 0; j < 6; j++) { pt[j][0] += 0.09 * jit(S, 30 + j); pt[j][1] += 0.09 * jit(S, 40 + j); }
    var k = clampf(c.intensity, 0, 1);
    var col = [
      c.c1,
      mixc(c.c1, mixc(c.c2, c.c3, 0.35), 0.55 + 0.45 * k),
      mixc(c.c2, c.c3, k),
      c.c2,
      mixc(c.c1, c.c2, 0.5),
      mixc(c.c2, c.c4, k)
    ];
    pt[2][2] *= 0.55 + 0.45 * k;
    pt[5][2] *= 0.55 + 0.45 * k;
    for (var y = 0; y < h; y++) {
      var fy = y / h;
      for (var x = 0; x < w; x++) {
        var fx = x / w;
        var wx = fx + 0.10 * (fbm2(fx * 2.4, fy * 2.4, 4, 7 + S) - 0.5);
        var wy = fy + 0.10 * (fbm2(fx * 2.4 + 9, fy * 2.4, 4, 23 + S) - 0.5);
        var tw = 0, ar = 0, ag = 0, ab = 0;
        for (var i = 0; i < 6; i++) {
          var dx = wx - pt[i][0], dy = (wy - pt[i][1]) * 0.85;
          var wgt = pt[i][2] / ((dx * dx + dy * dy) * 6.5 + 0.045);
          ar += col[i][0] * wgt; ag += col[i][1] * wgt; ab += col[i][2] * wgt;
          tw += wgt;
        }
        var o = (y * w + x) * 3;
        fb[o] = ar / tw; fb[o + 1] = ag / tw; fb[o + 2] = ab / tw;
      }
    }
  }

  /* ── waves ──────────────────────────────────────────────────────── */
  function rnd(v) { return v < 0 ? -Math.round(-v) : Math.round(v); } /* C roundf: half away from zero */
  function styleWaves(fb, w, h, c, S) {
    var hor = h * (0.62 + 0.04 * jit(S, 50));
    var skyTop = c.c1, skyBot = mixc(c.c2, c.c3, 0.30);
    var x, y, t, o;
    for (y = 0; y < h; y++) {
      var v;
      if (y < hor) { t = y / hor; v = mixc(skyTop, skyBot, Math.pow(t, 1.6)); }
      else { t = (y - hor) / (h - hor); v = mixc(mixc(c.c1, c.c2, 0.55), c.c1, Math.pow(t, 0.6)); }
      for (x = 0; x < w; x++) put(fb, (y * w + x) * 3, v);
    }
    var sx = w * (0.5 + 0.18 * jit(S, 51)), sy = hor - h * 0.055, sr = h * 0.20;
    for (y = (sy - sr) | 0; y <= ((sy + sr) | 0); y++) {
      if (y < 0 || y >= h) continue;
      for (x = (sx - sr) | 0; x <= ((sx + sr) | 0); x++) {
        if (x < 0 || x >= w) continue;
        var dx = (x - sx) / sr, dy = (y - sy) / sr;
        var d = Math.sqrt(dx * dx + dy * dy);
        if (d > 1) continue;
        var g = clampf((y - (sy - sr)) / (2 * sr), 0, 1);
        var col = mixc(c.c4, c.c3, g);
        var band = (y - (sy - sr)) / (h * 0.030);
        var bf = band - Math.floor(band);
        var cut = smoothstep(0, 0.45, g) * 0.92;
        if (bf < cut * 0.75 && g > 0.34) continue;
        blend(fb, (y * w + x) * 3, col, smoothstep(1, 0.90, d));
      }
    }
    for (y = (hor - h * 0.05) | 0; y < ((hor + h * 0.05) | 0); y++) {
      if (y < 0 || y >= h) continue;
      var dd = Math.abs(y - hor) / (h * 0.05);
      var ii = (1 - dd) * 0.55 * c.intensity;
      for (x = 0; x < w; x++) blend(fb, (y * w + x) * 3, c.c3, ii);
    }
    var ZK = 4.0, XK = 7.0, gc = saturate(c.c3, 1.25);
    for (y = (hor | 0) + 1; y < h; y++) {
      t = (y - hor) / (h - hor);
      var z = 1 / (t + 0.035);
      var fade = smoothstep(0, 0.10, t) * (1 - t * 0.18);
      if (fade <= 0) continue;
      var dzdy = z * z * ZK / (h - hor);
      var zr = z * ZK;
      var drail = Math.abs(zr - rnd(zr));
      var rl = smoothstep(dzdy * 1.5, 0, drail) * fade * (1 - smoothstep(0.07, 0.19, dzdy));
      var dpdx = z * XK * (2 / w);
      for (x = 0; x < w; x++) {
        var fx = (x - w * 0.5) / (w * 0.5);
        var px = fx * z * XK;
        var dv = Math.abs(px - rnd(px));
        var vl = smoothstep(dpdx * 1.5, 0, dv) * fade * (1 - smoothstep(0.07, 0.19, dpdx));
        var it = clampf(rl + vl, 0, 1) * 0.95 * c.intensity;
        if (it <= 0.002) continue;
        blend(fb, (y * w + x) * 3, gc, it);
      }
    }
  }

  /* ── gradient ───────────────────────────────────────────────────── */
  function styleGradient(fb, w, h, c, S) {
    var ax = 0.22 + 0.12 * jit(S, 60), ay = 0.18 + 0.10 * jit(S, 61);
    var bx = 0.82 + 0.10 * jit(S, 62), by = 0.86 + 0.08 * jit(S, 63);
    for (var y = 0; y < h; y++) {
      var fy = y / h;
      for (var x = 0; x < w; x++) {
        var fx = x / w;
        var t = clampf(fx * 0.62 + fy * 0.38, 0, 1);
        var v = mixc(c.c1, c.c2, smoothstep(0, 1, t));
        var d1 = Math.hypot(fx - ax, (fy - ay) * 1.3);
        var d2 = Math.hypot(fx - bx, (fy - by) * 1.3);
        v = mixc(v, c.c3, Math.exp(-d1 * d1 * 5.5) * 0.42 * c.intensity);
        v = mixc(v, c.c4, Math.exp(-d2 * d2 * 6.5) * 0.32 * c.intensity);
        put(fb, (y * w + x) * 3, v);
      }
    }
  }

  /* ── noise ──────────────────────────────────────────────────────── */
  function styleNoise(fb, w, h, c, S) {
    for (var y = 0; y < h; y++) {
      for (var x = 0; x < w; x++) {
        var n = fbm2(x / w * 3.2, y / h * 3.2, 6, 5 + S);
        n = clampf((n - 0.25) / 0.5, 0, 1);
        var v = n < 0.5 ? mixc(c.c1, c.c2, n * 2) : mixc(c.c2, c.c3, (n - 0.5) * 2 * c.intensity);
        put(fb, (y * w + x) * 3, v);
      }
    }
  }

  /* ── letterpress ────────────────────────────────────────────────── */
  function hrule(fb, w, h, x0, x1, y, weight, col, a) {
    if (a <= 0 || weight <= 0) return;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    var y0 = y - weight * 0.5, y1 = y + weight * 0.5;
    var iy0 = Math.floor(y0), iy1 = Math.ceil(y1);
    if (iy0 < 0) iy0 = 0;
    if (iy1 > h) iy1 = h;
    for (var yy = iy0; yy < iy1; yy++) {
      var top = yy > y0 ? yy : y0, bot = (yy + 1) < y1 ? (yy + 1) : y1;
      var cov = bot - top;
      if (cov <= 0) continue;
      for (var xx = x0; xx < x1; xx++) blend(fb, (yy * w + xx) * 3, col, a * cov);
    }
  }
  function vrule(fb, w, h, x, y0, y1, weight, col, a) {
    if (a <= 0 || weight <= 0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    var x0 = x - weight * 0.5, x1 = x + weight * 0.5;
    var ix0 = Math.floor(x0), ix1 = Math.ceil(x1);
    if (ix0 < 0) ix0 = 0;
    if (ix1 > w) ix1 = w;
    for (var xx = ix0; xx < ix1; xx++) {
      var l = xx > x0 ? xx : x0, r = (xx + 1) < x1 ? (xx + 1) : x1;
      var cov = r - l;
      if (cov <= 0) continue;
      for (var yy = y0; yy < y1; yy++) blend(fb, (yy * w + xx) * 3, col, a * cov);
    }
  }
  function styleLetterpress(fb, w, h, c, S) {
    var k = clampf(c.intensity, 0, 1), x, y;
    for (y = 0; y < h; y++) {
      var fy = y / h;
      for (x = 0; x < w; x++) {
        var n = fbm2(x / w * 2.3, fy * 1.7, 2, 19 + S);
        put(fb, (y * w + x) * 3, mixc(c.c1, c.c2, clampf((n - 0.35) * 0.46, 0, 1)));
      }
    }
    var bx = (w * 0.28) | 0;
    for (y = 0; y < h; y++) for (x = 0; x < bx; x++) blend(fb, (y * w + x) * 3, c.c2, 0.62 * k);
    var step = (h / 16) | 0; if (step < 18) step = 18;
    for (y = step; y < h; y += step) hrule(fb, w, h, 0, w, y + 0.5, 1, c.c4, 0.05 * k);
    var yLong = h * (0.780 + 0.05 * jit(S, 70));
    var yShort = h * (0.140 + 0.04 * jit(S, 71));
    var xRight = (w * 0.952) | 0;
    hrule(fb, w, h, bx + ((w * 0.02) | 0), xRight, yLong, 1, c.c4, 0.55);
    hrule(fb, w, h, 0, bx, yShort, 2, c.c4, 0.45);
    vrule(fb, w, h, bx + 0.5, 0, h, 1, c.c4, 0.42);
    var side = (h * 0.026) | 0; if (side < 12) side = 12;
    var mx = xRight - side, my = (yLong | 0) - side - 7;
    for (y = my; y < my + side; y++) {
      if (y < 0 || y >= h) continue;
      for (x = mx; x < mx + side; x++) { if (x >= 0 && x < w) put(fb, (y * w + x) * 3, c.c3); }
    }
    var s2 = (side / 3) | 0; if (s2 < 4) s2 = 4;
    var m2x = mx - side - 10, m2y = (yLong | 0) - s2 - 7;
    for (y = m2y; y < m2y + s2; y++) {
      if (y < 0 || y >= h) continue;
      for (x = m2x; x < m2x + s2; x++) { if (x >= 0 && x < w) put(fb, (y * w + x) * 3, c.c4); }
    }
    return { ruleY: yLong / h, markX: mx / w, tintX: 0.28 };
  }

  function styleSolid(fb, w, h, c) {
    for (var i = 0; i < w * h; i++) put(fb, i * 3, c.c1);
  }

  function post(fb, w, h, c) {
    for (var y = 0; y < h; y++) {
      var fy = (y / h - 0.5) * 2;
      for (var x = 0; x < w; x++) {
        var fx = (x / w - 0.5) * 2, i = (y * w + x) * 3;
        if (c.vignette > 0) {
          var d = Math.sqrt(fx * fx * 0.78 + fy * fy);
          var v = 1 - c.vignette * smoothstep(0.35, 1.45, d);
          fb[i] *= v; fb[i + 1] *= v; fb[i + 2] *= v;
        }
        if (c.grain > 0) {
          var n = (hash2i(x, y, 1337) - 0.5) * c.grain;
          fb[i] += n; fb[i + 1] += n; fb[i + 2] += n;
        }
        fb[i] = clampf(fb[i], 0, 1); fb[i + 1] = clampf(fb[i + 1], 0, 1); fb[i + 2] = clampf(fb[i + 2], 0, 1);
      }
    }
  }

  var STYLES = { letterpress: styleLetterpress, aurora: styleAurora, mesh: styleMesh, waves: styleWaves,
                 gradient: styleGradient, noise: styleNoise, solid: styleSolid };

  /* params: { style, c1..c4 (hex), intensity, grain, vignette } -- the
   * wall_* keys of a theme, without the prefix. Returns RGBA bytes. */
  function render(w, h, p, seed) {
    seed = (seed | 0) >>> 0;
    var S = seed & 0x7FFFFFFF;
    var c = {
      c1: unpack(p.c1 || "#0B0E14"), c2: unpack(p.c2 || "#14303A"),
      c3: unpack(p.c3 || "#7DD3C0"), c4: unpack(p.c4 || "#A78BFA"),
      intensity: p.intensity == null ? 0.55 : +p.intensity,
      grain: p.grain == null ? 0.05 : +p.grain,
      vignette: p.vignette == null ? 0.45 : +p.vignette
    };
    var fb = new Float32Array(w * h * 3);
    var fn = STYLES[p.style] || styleAurora;
    var meta = fn(fb, w, h, c, S) || null;
    post(fb, w, h, c);
    var out = new Uint8ClampedArray(w * h * 4);
    for (var i = 0, j = 0; i < w * h; i++, j += 3) {
      out[i * 4] = (fb[j] * 255 + 0.5) | 0;
      out[i * 4 + 1] = (fb[j + 1] * 255 + 0.5) | 0;
      out[i * 4 + 2] = (fb[j + 2] * 255 + 0.5) | 0;
      out[i * 4 + 3] = 255;
    }
    return { pixels: out, meta: meta };
  }

  function paramsOf(theme) {
    return { style: theme.wall_style, c1: theme.wall_c1, c2: theme.wall_c2, c3: theme.wall_c3, c4: theme.wall_c4,
             intensity: theme.wall_intensity, grain: theme.wall_grain, vignette: theme.wall_vignette };
  }

  var api = { render: render, paramsOf: paramsOf, styles: Object.keys(STYLES) };

  /* In a page: draw into a canvas, in a worker when one can be made, so
   * a big render does not freeze the page. Falls back to drawing here. */
  if (typeof document !== "undefined") {
    var worker = null, jobs = {}, nextId = 1, triedWorker = false;
    function getWorker() {
      if (triedWorker) return worker;
      triedWorker = true;
      try {
        var src = document.querySelector('script[src$="wall.js"]');
        var url = src ? src.src.replace(/wall\.js(\?.*)?$/, "wall-worker.js") : "assets/wall-worker.js";
        worker = new Worker(url);
        worker.onmessage = function (e) {
          var j = jobs[e.data.id]; delete jobs[e.data.id];
          if (j) j.resolve({ pixels: new Uint8ClampedArray(e.data.buf), meta: e.data.meta });
        };
        worker.onerror = function () {
          worker = null;
          for (var id in jobs) { var j = jobs[id]; delete jobs[id]; j.resolve(render(j.w, j.h, j.p, j.seed)); }
        };
      } catch (err) { worker = null; }
      return worker;
    }
    api.renderAsync = function (w, h, p, seed) {
      return new Promise(function (resolve) {
        var wk = getWorker();
        if (!wk) { setTimeout(function () { resolve(render(w, h, p, seed)); }, 0); return; }
        var id = nextId++;
        jobs[id] = { resolve: resolve, w: w, h: h, p: p, seed: seed };
        wk.postMessage({ id: id, w: w, h: h, p: p, seed: seed });
      });
    };
    api.draw = function (canvas, w, h, p, seed) {
      canvas.width = w; canvas.height = h;
      return api.renderAsync(w, h, p, seed).then(function (r) {
        var ctx = canvas.getContext("2d");
        ctx.putImageData(new ImageData(r.pixels, w, h), 0, 0);
        return r.meta;
      });
    };
  }

  root.AurWall = api;
})(typeof self !== "undefined" ? self : this);
