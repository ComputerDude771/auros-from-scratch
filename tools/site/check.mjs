#!/usr/bin/env node
// The website's front door, checked the way she meets it.
//
//   node tools/site/check.mjs                      check website/
//   node tools/site/check.mjs --dir <folder>       check another copy
//   node tools/site/check.mjs --shots <folder>     also save screenshots
//
// Stamps a copy with the repository it is published from (tools/site/
// stamp.sh, as pages.yml does), serves it under /<repository name>/ (as
// GitHub Pages does, so a link that starts with "/" breaks here too),
// drives Chromium with Playwright, and fails if:
//   - "Get AurOS" is not wholly in the first screen at 1366x768 and
//     390x844 (Windows, and with script off), or is shorter than 56px;
//   - its href is not the release address;
//   - any visible text is under 18px, or any text (buttons included) is
//     under 4.5:1 against its background, from the computed styles;
//   - any internal link, image, script, stylesheet or #anchor is missing;
//   - a Mac, iPhone, Android, Chromebook or Linux visit does not show
//     "Open this page on the Windows computer...", or a Windows-on-ARM or
//     32-bit Windows visit does not show "AurOS can't go on this
//     computer", each with an email link and a copy button;
//   - a page asks any server but this one for anything;
//   - the page scrolls sideways at 320px or at 200% zoom;
//   - advanced.html (the page the installer names when it cannot go on)
//     is missing, unlinked from the footer, or fails the same text floors;
//   - published unstamped on another repository's GitHub Pages, the
//     button does not find that repository's release (assets/door.js).
//
// Playwright: uses the one Node can find (a local install, the global
// one, or the folder named by PLAYWRIGHT_DIR).

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
import os from "node:os";
import { execSync, execFileSync } from "node:child_process";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..", "..");
// The repository the site is published from (AUROS_REPO, else
// GITHUB_REPOSITORY, else the origin remote): never written here.
const repoSh = (what) => execFileSync("sh", [path.join(REPO, "tools", "repo.sh"), what]).toString().trim();
const AUROS_REPO = repoSh("repo");
const RELEASE_URL = `https://github.com/${AUROS_REPO}/releases/latest/download/AurOS-Setup.exe`;
const PAGES_HOST = repoSh("host");                      // owner.github.io/name/
const BASE = PAGES_HOST.slice(PAGES_HOST.indexOf("/"));  // /name/, or / for owner.github.io

const argv = process.argv.slice(2);
const opt = (name) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : undefined; };
const SRC = path.resolve(opt("--dir") || path.join(REPO, "website"));
// What is served and read: the folder as the deploy publishes it.
const SITE = fs.mkdtempSync(path.join(os.tmpdir(), "auros-site-"));
process.stdout.write(execFileSync("sh", [path.join(REPO, "tools", "site", "stamp.sh"), SRC, SITE]));
process.on("exit", () => fs.rmSync(SITE, { recursive: true, force: true }));
const SHOTS = opt("--shots") ? path.resolve(opt("--shots")) : null;

function loadPlaywright() {
  const bases = [];
  if (process.env.PLAYWRIGHT_DIR) bases.push(path.join(process.env.PLAYWRIGHT_DIR, "node_modules"), process.env.PLAYWRIGHT_DIR);
  bases.push(process.cwd(), HERE);
  try { bases.push(execSync("npm root -g", { stdio: ["ignore", "pipe", "ignore"] }).toString().trim()); } catch {}
  for (const b of bases) {
    try { return createRequire(path.join(b, "noop.js"))("playwright"); } catch {}
    try { return createRequire(path.join(b, "node_modules", "noop.js"))("playwright"); } catch {}
  }
  console.error("check.mjs: Playwright not found. Install it (npm i playwright) or set PLAYWRIGHT_DIR.");
  process.exit(2);
}

// ── a static server, like GitHub Pages, under a subpath ───────────────
const TYPES = { ".html": "text/html; charset=utf-8", ".css": "text/css", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".svg": "image/svg+xml",
  ".webp": "image/webp", ".woff2": "font/woff2", ".woff": "font/woff", ".md": "text/markdown; charset=utf-8", ".txt": "text/plain; charset=utf-8", ".ico": "image/x-icon" };

function serve() {
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, "http://x");
    let p = decodeURIComponent(u.pathname);
    if (!p.startsWith(BASE)) { res.writeHead(404); return res.end("not under " + BASE); }
    p = p.slice(BASE.length);
    if (p === "" || p.endsWith("/")) p += "index.html";
    const file = path.resolve(SITE, p);
    if (!file.startsWith(SITE + path.sep)) { res.writeHead(403); return res.end(); }
    fs.stat(file, (err, st) => {
      if (err || !st.isFile()) { res.writeHead(404); return res.end("404"); }
      res.writeHead(200, { "content-type": TYPES[path.extname(file).toLowerCase()] || "application/octet-stream" });
      fs.createReadStream(file).pipe(res);
    });
  });
  return new Promise((ok) => server.listen(0, "127.0.0.1", () => ok(server)));
}

// ── results ───────────────────────────────────────────────────────────
const failures = [];
let passes = 0;
const fail = (what) => { failures.push(what); console.log("FAIL " + what); };
const pass = (what) => { passes++; console.log("ok   " + what); };
const check = (cond, what, why) => (cond ? pass(what) : fail(what + (why ? " -- " + why : "")));

// ── who visits ────────────────────────────────────────────────────────
const UA = {
  winEdge: "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 Edg/130.0.0.0",
  winChrome: "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
  winFirefox: "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:131.0) Gecko/20100101 Firefox/131.0",
  winFirefoxWow64: "Mozilla/5.0 (Windows NT 10.0; WOW64; rv:115.0) Gecko/20100101 Firefox/115.0",
  win32Firefox: "Mozilla/5.0 (Windows NT 10.0; rv:115.0) Gecko/20100101 Firefox/115.0",
  macSafari: "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15",
  macChrome: "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
  chromeOS: "Mozilla/5.0 (X11; CrOS x86_64 14541.0.0) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
  linux: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
};
// navigator.userAgentData as each real browser has it (null: none, as in
// Safari and Firefox). A UA override alone would leave Chromium's Linux one.
const UAD = {
  winX64: { platform: "Windows", architecture: "x86", bitness: "64", mobile: false },
  winArm: { platform: "Windows", architecture: "arm", bitness: "64", mobile: false },
  mac: { platform: "macOS", architecture: "arm", bitness: "64", mobile: false },
  chromeOS: { platform: "Chrome OS", architecture: "x86", bitness: "64", mobile: false },
  android: { platform: "Android", architecture: "", bitness: "", mobile: true },
  linux: { platform: "Linux", architecture: "x86", bitness: "64", mobile: false },
};
function uadScript(mock) {
  if (mock === null) {
    Object.defineProperty(Navigator.prototype, "userAgentData", { configurable: true, get: () => undefined });
    return;
  }
  const data = { brands: [], mobile: mock.mobile, platform: mock.platform,
    getHighEntropyValues: async () => ({ platform: mock.platform, architecture: mock.architecture, bitness: mock.bitness, mobile: mock.mobile }),
    toJSON() { return { brands: [], mobile: mock.mobile, platform: mock.platform }; } };
  Object.defineProperty(Navigator.prototype, "userAgentData", { configurable: true, get: () => data });
}

// ── measuring inside the page ─────────────────────────────────────────
// Every visible text, with its computed size and its contrast against the
// colour actually behind it (backgrounds composed up the tree).
function measureText() {
  const parse = (c) => {
    const m = c.match(/rgba?\(([^)]+)\)/);
    if (!m) return null;
    const v = m[1].split(/[ ,/]+/).filter(Boolean).map(Number);
    return { r: v[0], g: v[1], b: v[2], a: v.length > 3 ? v[3] : 1 };
  };
  const lin = (x) => { x /= 255; return x <= 0.04045 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4); };
  const L = (c) => 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
  const over = (top, under) => ({ r: top.r * top.a + under.r * (1 - top.a), g: top.g * top.a + under.g * (1 - top.a), b: top.b * top.a + under.b * (1 - top.a), a: 1 });
  const ratio = (a, b) => { const x = L(a), y = L(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
  function background(el) {
    const layers = [];
    for (let e = el; e; e = e.parentElement) {
      const cs = getComputedStyle(e);
      if (cs.backgroundImage && cs.backgroundImage !== "none") return null; // a picture behind: can't tell
      const c = parse(cs.backgroundColor);
      if (c && c.a > 0) { layers.push(c); if (c.a >= 1) break; }
    }
    let bg = { r: 255, g: 255, b: 255, a: 1 };
    for (let i = layers.length - 1; i >= 0; i--) bg = over(layers[i], bg);
    return bg;
  }
  const out = [];
  const seen = new Set();
  const w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  while (w.nextNode()) {
    const t = w.currentNode;
    if (!t.textContent.trim()) continue;
    const el = t.parentElement;
    if (!el || seen.has(el) || el.closest("script,style,noscript")) continue;
    seen.add(el);
    const r = document.createRange(); r.selectNodeContents(t);
    const box = r.getBoundingClientRect();
    if (box.width <= 1 || box.height <= 1) continue;          // not drawn (or screen-reader only)
    let hidden = false, op = 1;
    for (let e = el; e; e = e.parentElement) {
      const cs = getComputedStyle(e);
      if (cs.visibility === "hidden" || cs.display === "none") { hidden = true; break; }
      op *= parseFloat(cs.opacity);
    }
    if (hidden || op === 0) continue;
    if (el.closest(".skip") && box.bottom < 0) continue;      // the skip link, until it has focus (checked separately)
    const cs = getComputedStyle(el);
    const fg = parse(cs.color), bg = background(el);
    const cr = bg && fg ? ratio(over(fg, bg), bg) : null;
    out.push({ text: t.textContent.trim().slice(0, 60), tag: el.tagName.toLowerCase() + (el.id ? "#" + el.id : "") + (el.className && typeof el.className === "string" ? "." + el.className.trim().split(/\s+/).join(".") : ""),
      size: parseFloat(cs.fontSize), contrast: cr });
  }
  return out;
}

function rectOf(sel) {
  const el = document.querySelector(sel);
  if (!el) return null;
  const r = el.getBoundingClientRect();
  const cs = getComputedStyle(el);
  return { top: r.top, left: r.left, bottom: r.bottom, right: r.right, width: r.width, height: r.height,
    shown: r.width > 0 && r.height > 0 && cs.visibility !== "hidden", vw: innerWidth, vh: innerHeight,
    href: el.getAttribute && el.getAttribute("href"), text: (el.innerText || "").trim() };
}
const inFold = (r) => r && r.shown && r.top >= 0 && r.left >= 0 && r.bottom <= r.vh && r.right <= r.vw;

// ── the run ───────────────────────────────────────────────────────────
const server = await serve();
const ORIGIN = `http://127.0.0.1:${server.address().port}`;
const HOME = ORIGIN + BASE;
const { chromium, devices } = loadPlaywright();
const browser = await chromium.launch();
console.log(`checking ${path.relative(REPO, SRC) || SRC} at ${HOME}`);

const external = new Map();   // url -> page it was asked for on
const broken = new Map();     // url -> status
async function visit({ name, url = HOME, ua, uad, viewport = { width: 1366, height: 768 }, js = true, device }) {
  const ctxOpts = device ? { ...devices[device], deviceScaleFactor: 1 } : { viewport, userAgent: ua };
  if (!js) ctxOpts.javaScriptEnabled = false;
  const ctx = await browser.newContext(ctxOpts);
  if (uad !== undefined && js) await ctx.addInitScript(uadScript, uad);
  const page = await ctx.newPage();
  page.on("request", (rq) => {
    const u = rq.url();
    if (!u.startsWith(ORIGIN) && !/^(data|blob|about):/.test(u)) external.set(u, `${name} ${page.url()}`);
  });
  page.on("response", (rs) => { if (rs.url().startsWith(ORIGIN) && rs.status() >= 400) broken.set(rs.url(), rs.status()); });
  await page.goto(url, { waitUntil: "load" });
  await page.waitForTimeout(250); // let door.js finish its one async question
  return { ctx, page };
}

function textFindings(name, items) {
  const small = items.filter((i) => i.size < 18);
  const low = items.filter((i) => i.contrast !== null && i.contrast < 4.5);
  check(small.length === 0, `${name}: all text 18px or larger (${items.length} pieces)`,
    small.slice(0, 6).map((i) => `${i.size}px "${i.text}" <${i.tag}>`).join("; "));
  check(low.length === 0, `${name}: all text 4.5:1 or better`,
    low.slice(0, 6).map((i) => `${i.contrast.toFixed(2)}:1 "${i.text}" <${i.tag}>`).join("; "));
  const min = items.reduce((m, i) => (i.contrast !== null && i.contrast < m.c ? { c: i.contrast, t: i.text } : m), { c: 99, t: "" });
  if (items.length) console.log(`     lowest contrast ${min.c.toFixed(2)}:1 ("${min.t}")`);
}

async function shot(page, file, full = false) {
  if (!SHOTS) return;
  fs.mkdirSync(SHOTS, { recursive: true });
  if (full) { // let lazy pictures load, as they do for someone scrolling
    await page.evaluate(async () => { for (let y = 0; y < document.body.scrollHeight; y += 500) { scrollTo(0, y); await new Promise((r) => setTimeout(r, 60)); } scrollTo(0, 0); });
    await page.waitForTimeout(500);
  }
  await page.screenshot({ path: path.join(SHOTS, file), fullPage: full });
  console.log(`     saved ${path.relative(REPO, path.join(SHOTS, file))}`);
}

// 1. Windows, at the two sizes, with and without script
for (const v of [
  { name: "Windows Edge 1366x768", ua: UA.winEdge, uad: UAD.winX64, viewport: { width: 1366, height: 768 }, shots: ["index-desktop-fold.png", "index-desktop-full.png"] },
  { name: "Windows Chrome 390x844", ua: UA.winChrome, uad: UAD.winX64, viewport: { width: 390, height: 844 }, shots: ["index-narrow-windows-fold.png"] },
  { name: "Windows Firefox 1366x768 (can't tell the chip)", ua: UA.winFirefox, uad: null, viewport: { width: 1366, height: 768 } },
  { name: "32-bit Firefox on 64-bit Windows", ua: UA.winFirefoxWow64, uad: null, viewport: { width: 1366, height: 768 } },
  { name: "no script 1366x768", ua: UA.linux, js: false, viewport: { width: 1366, height: 768 } },
  { name: "no script 390x844", ua: UA.linux, js: false, viewport: { width: 390, height: 844 } },
]) {
  const { ctx, page } = await visit(v);
  const r = await page.evaluate(rectOf, "#get");
  check(inFold(r), `${v.name}: "Get AurOS" wholly on the first screen`, r ? `at ${Math.round(r.top)}-${Math.round(r.bottom)} of ${r.vh}, ${Math.round(r.left)}-${Math.round(r.right)} of ${r.vw}${r.shown ? "" : ", hidden"}` : "no #get");
  check(r && r.href === RELEASE_URL, `${v.name}: it links to the release`, r && r.href);
  check(r && /get aurOS/i.test(r.text), `${v.name}: it says "Get AurOS"`, r && JSON.stringify(r.text));
  check(r && r.height >= 56, `${v.name}: it is at least 56px tall`, r && `${Math.round(r.height)}px`);
  const el = await page.evaluate(rectOf, "#elsewhere");
  check(!el || !el.shown, `${v.name}: no "open it on another computer" message`);
  if (v.js !== false) {
    const again = await page.evaluate(() => [...document.querySelectorAll("[data-download]")].map((a) => a.href));
    check(again.every((h) => h === RELEASE_URL), `${v.name}: every other download link is the release too`, again.join(" "));
  }
  if (v.name.startsWith("Windows Edge") || v.name === "no script 390x844") textFindings(v.name, await page.evaluate(measureText));
  for (const [i, f] of (v.shots || []).entries()) await shot(page, f, i === 1);
  await ctx.close();
}

// 2. Not a Windows PC AurOS can go on: the kind message instead
for (const v of [
  { name: "Windows on ARM (Edge)", ua: UA.winEdge, uad: UAD.winArm, want: /can.t go on this computer/i, shots: ["index-arm.png"] },
  { name: "32-bit Windows (Firefox, no chip hints)", ua: UA.win32Firefox, uad: null, want: /can.t go on this computer/i },
  { name: "32-bit Windows (Edge's hints)", ua: UA.winEdge, uad: { platform: "Windows", architecture: "x86", bitness: "32", mobile: false }, want: /can.t go on this computer/i },
  { name: "Mac (Safari)", ua: UA.macSafari, uad: null, want: /open this page on the Windows computer/i, shots: ["index-mac.png"] },
  { name: "Mac (Chrome)", ua: UA.macChrome, uad: UAD.mac, want: /open this page on the Windows computer/i },
  { name: "iPhone", device: "iPhone 13", uad: null, want: /open this page on the Windows computer/i, shots: ["index-phone-fold.png"] },
  { name: "Android phone", device: "Pixel 7", uad: UAD.android, want: /open this page on the Windows computer/i },
  { name: "Chromebook", ua: UA.chromeOS, uad: UAD.chromeOS, want: /open this page on the Windows computer/i },
  { name: "Linux", ua: UA.linux, uad: UAD.linux, want: /open this page on the Windows computer/i },
]) {
  const { ctx, page } = await visit(v);
  const m = await page.evaluate(rectOf, "#elsewhere");
  const title = await page.evaluate(() => [...document.querySelectorAll("#elsewhere h2")].filter((h) => h.getClientRects().length).map((h) => h.innerText).join(" | "));
  check(m && m.shown && v.want.test(title), `${v.name}: says "${title || "nothing"}"`);
  const head = await page.evaluate(rectOf, "#elsewhere h2:not([hidden])");
  const titleTop = await page.evaluate(() => { const h = [...document.querySelectorAll("#elsewhere h2")].find((x) => x.getClientRects().length); return h ? h.getBoundingClientRect().top : null; });
  check(titleTop !== null && titleTop >= 0 && titleTop < (m ? m.vh : 0) - 60, `${v.name}: the message starts on the first screen`, `at ${titleTop}`);
  const mail = await page.evaluate(() => { const a = document.getElementById("mail-link"); return a && a.getClientRects().length ? a.href : ""; });
  const pageUrl = await page.evaluate(() => location.href.split("#")[0]);
  check(mail.startsWith("mailto:") && decodeURIComponent(mail).includes(pageUrl), `${v.name}: "Email me the link" carries this page's address`, mail);
  const copy = await page.evaluate(() => { const b = document.getElementById("copy-link"); return !!(b && b.tagName === "BUTTON" && b.getClientRects().length); });
  check(copy, `${v.name}: a "Copy the link" button`);
  const g = await page.evaluate(rectOf, "#get");
  check(!g || !g.shown, `${v.name}: the big download button is not offered`);
  if (v.name === "iPhone" || v.name.startsWith("Windows on ARM")) textFindings(v.name, await page.evaluate(measureText));
  for (const f of v.shots || []) await shot(page, f);
  await ctx.close();
}

// 3. Zoom and small screens: nothing scrolls sideways
for (const v of [
  { name: "200% zoom of 1366x768 (683x384)", viewport: { width: 683, height: 384 } },
  { name: "320px wide", viewport: { width: 320, height: 640 } },
]) {
  const { ctx, page } = await visit({ ...v, ua: UA.winEdge, uad: UAD.winX64 });
  const o = await page.evaluate(() => ({ sw: document.documentElement.scrollWidth, cw: document.documentElement.clientWidth }));
  check(o.sw <= o.cw + 1, `${v.name}: no sideways scrolling`, `${o.sw}px of content in ${o.cw}px`);
  const r = await page.evaluate(rectOf, "#get");
  check(r && r.shown && r.right <= r.vw && r.left >= 0, `${v.name}: "Get AurOS" fits across the screen`);
  await ctx.close();
}

// 3b. The page the installer names when it cannot go on
//     (src/aurbridge/fix_win.c ADVANCED): it exists, the front door's
//     footer links it, and its text passes the same size and contrast
//     floors -- the person reading it may be the one who could not read
//     the installer either.
{
  const adv = path.join(SITE, "advanced.html");
  check(fs.existsSync(adv), "advanced.html: the page the installer names exists");
  const front = fs.readFileSync(path.join(SITE, "index.html"), "utf8");
  check(/<footer[\s\S]*href="advanced\.html"[\s\S]*<\/footer>/.test(front), "advanced.html: linked from the front door's footer");
  if (fs.existsSync(adv)) {
    for (const v of [
      { name: "advanced.html 1366x768", viewport: { width: 1366, height: 768 } },
      { name: "advanced.html 390x844", viewport: { width: 390, height: 844 } },
    ]) {
      const { ctx, page } = await visit({ ...v, url: HOME + "advanced.html", ua: UA.winEdge, uad: UAD.winX64 });
      textFindings(v.name, await page.evaluate(measureText));
      const ids = await page.evaluate(() => ["bios", "rst"].filter((id) => document.getElementById(id)));
      check(ids.length === 2, `${v.name}: has the BIOS and the Intel storage sections`, ids.join(","));
      await ctx.close();
    }
    const { ctx, page } = await visit({ name: "advanced.html 320px", url: HOME + "advanced.html", ua: UA.winEdge, uad: UAD.winX64, viewport: { width: 320, height: 640 } });
    const o = await page.evaluate(() => ({ sw: document.documentElement.scrollWidth, cw: document.documentElement.clientWidth }));
    check(o.sw <= o.cw + 1, "advanced.html 320px wide: no sideways scrolling", `${o.sw}px of content in ${o.cw}px`);
    await ctx.close();
  }
}

// 4. Keyboard: the first Tab reaches the skip link, which is then readable
{
  const { ctx, page } = await visit({ name: "keyboard", ua: UA.winEdge, uad: UAD.winX64 });
  await page.keyboard.press("Tab");
  const f = await page.evaluate(() => { const a = document.activeElement; const r = a.getBoundingClientRect(); const cs = getComputedStyle(a);
    return { text: a.innerText, top: r.top, size: parseFloat(cs.fontSize), outline: cs.outlineStyle !== "none" || a.classList.contains("skip") }; });
  check(f.top >= 0 && f.size >= 18 && /download/i.test(f.text), `keyboard: Tab first shows "${f.text}"`);
  await page.keyboard.press("Tab"); await page.keyboard.press("Tab"); await page.keyboard.press("Tab");
  const ring = await page.evaluate(() => { const cs = getComputedStyle(document.activeElement); return `${cs.outlineStyle} ${cs.outlineWidth}`; });
  check(/solid (3|4|5|6)px/.test(ring), `keyboard: focus is drawn with a thick ring`, ring);
  await ctx.close();
}

// 5. Every page of the site: links, pictures, scripts, anchors; and no
//    request to any other server
{
  // every page in the folder (old addresses included), and whatever they link to
  const seed = ["index.html", ...fs.readdirSync(SITE).filter((f) => f.endsWith(".html") && f !== "index.html").sort()];
  const pages = new Set(seed);
  const queue = [...seed];
  const targets = new Map(); // "page.html#frag" or "file" -> where it is linked from
  const ids = new Map();
  const isStub = (html) => /http-equiv="refresh"/i.test(html);
  while (queue.length) {
    const p = queue.shift();
    const file = path.join(SITE, p);
    if (!fs.existsSync(file)) continue;
    const html = fs.readFileSync(file, "utf8");
    ids.set(p, new Set([...html.matchAll(/\b(?:id|name)\s*=\s*"([^"]+)"/g)].map((m) => m[1])));
    const refs = [...html.matchAll(/\b(?:href|src)\s*=\s*"([^"]*)"/g)].map((m) => m[1])
      .concat([...html.matchAll(/\bcontent\s*=\s*"[^"]*?url=([^"]+)"/gi)].map((m) => m[1].trim()));
    for (const raw of refs) {
      if (!raw || /^(mailto:|tel:|javascript:|data:|https?:|\/\/)/i.test(raw) || /^[\d.]+;?$/.test(raw) || raw.startsWith("width=")) continue;
      if (/[\s<>{}$]/.test(raw)) continue; // not a link (meta content, template text)
      const u = new URL(raw, ORIGIN + BASE + p);
      if (!u.pathname.startsWith(BASE)) { targets.set(raw + "  (leaves the site's folder)", p); continue; }
      const rel = u.pathname.slice(BASE.length) || "index.html";
      const key = (rel.endsWith("/") ? rel + "index.html" : rel) + (u.hash || "");
      if (!targets.has(key)) targets.set(key, p);
      const target = key.split("#")[0];
      if (target.endsWith(".html") && !pages.has(target)) { pages.add(target); queue.push(target); }
    }
  }
  const missing = [];
  for (const [key, from] of targets) {
    if (key.includes("(leaves")) { missing.push(`${key} on ${from}`); continue; }
    const [file, frag] = key.split("#");
    if (!fs.existsSync(path.join(SITE, file))) { missing.push(`${key} on ${from}`); continue; }
    if (frag && file.endsWith(".html") && !(ids.get(file) || new Set()).has(decodeURIComponent(frag))) missing.push(`${key} (no such place) on ${from}`);
  }
  check(missing.length === 0, `links: every internal link, picture, script and anchor exists (${targets.size} checked on ${pages.size} pages)`, missing.slice(0, 10).join("; "));

  for (const p of [...pages].sort()) {
    const html = fs.readFileSync(path.join(SITE, p), "utf8");
    if (isStub(html)) continue;
    const { ctx, page } = await visit({ name: p, url: HOME + p, ua: UA.winEdge, uad: UAD.winX64 });
    await page.waitForTimeout(400);
    await ctx.close();
  }
  check(broken.size === 0, `requests: nothing the pages load is missing`, [...broken].map(([u, s]) => `${s} ${u.replace(ORIGIN, "")}`).slice(0, 10).join("; "));
  check(external.size === 0, `requests: no page asks another server for anything`, [...external].map(([u, w]) => `${u} (${w})`).slice(0, 10).join("; "));
}

// 6. Published as it is in the tree (no stamp) on another repository's
//    GitHub Pages: the button works the release out from the page's own
//    address (assets/door.js), for a project site and for owner.github.io.
for (const [at, want] of [
  ["https://someone-else.github.io/their-copy/", "https://github.com/someone-else/their-copy/releases/latest/download/AurOS-Setup.exe"],
  ["https://someone-else.github.io/", "https://github.com/someone-else/someone-else.github.io/releases/latest/download/AurOS-Setup.exe"],
]) {
  const ctx = await browser.newContext({ viewport: { width: 1366, height: 768 }, userAgent: UA.winEdge });
  await ctx.addInitScript(uadScript, UAD.winX64);
  await ctx.route(() => true, (route) => {
    const u = route.request().url();
    if (!u.startsWith(at)) return route.abort();
    let p = decodeURIComponent(new URL(u).pathname.slice(new URL(at).pathname.length));
    if (p === "" || p.endsWith("/")) p += "index.html";
    const file = path.resolve(SRC, p);
    if (!file.startsWith(SRC + path.sep) || !fs.existsSync(file)) return route.fulfill({ status: 404, body: "404" });
    return route.fulfill({ path: file, contentType: TYPES[path.extname(file).toLowerCase()] || "application/octet-stream" });
  });
  const page = await ctx.newPage();
  await page.goto(at, { waitUntil: "load" });
  await page.waitForTimeout(250);
  const links = await page.evaluate(() => [document.getElementById("get"), ...document.querySelectorAll("[data-download]")].map((a) => a && a.href));
  check(links.length > 0 && links.every((h) => h === want), `unstamped at ${at}: every download link is that repository's release`, links.join(" "));
  await ctx.close();
}

await browser.close();
server.close();
console.log(`\n${passes} passed, ${failures.length} failed`);
process.exit(failures.length ? 1 : 0);
