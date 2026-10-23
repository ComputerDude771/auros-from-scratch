/* AurOS front door (index.html). Everything here is optional: without
 * script the page shows the Windows path, the button and every browser's
 * steps, which is the right default. Nothing here sends anything.
 *
 * The download address lives in ONE place: the href of #get in
 * index.html (on GitHub Pages, worked out from the page's own address,
 * below). This file copies it to the other download links.
 */
(function () {
  "use strict";

  /* The installer is not code-signed. If that ever changes, true hides
   * the steps for Windows' unknown-file warnings (.unsigned-only) and
   * shows the signed wording (.signed-only). */
  var SIGNED = false;
  /* What Windows would then show as "Verified publisher". */
  var PUBLISHER = "AurOS";

  var root = document.documentElement;
  var $ = function (id) { return document.getElementById(id); };
  var get = $("get");
  var DOWNLOAD_URL = get.getAttribute("href");

  /* On GitHub Pages the page's own address says where the releases are:
   * https://<owner>.github.io/<name>/ is github.com/<owner>/<name>, and
   * the repository <owner>.github.io is the site at the top. Anywhere else
   * (a custom domain, a test server) the href above stands: the deploy
   * writes the repository it publishes from into it (tools/site/stamp.sh). */
  var gh = /^([a-z0-9-]+)\.github\.io$/i.exec(location.hostname);
  if (gh && location.protocol === "https:") {
    var first = location.pathname.split("/")[1] || "";
    var name = (first && !/\.html?$/i.test(first)) ? first : gh[1] + ".github.io";
    if (/^[A-Za-z0-9._-]+$/.test(name)) {
      DOWNLOAD_URL = "https://github.com/" + gh[1] + "/" + name + "/releases/latest/download/AurOS-Setup.exe";
      get.setAttribute("href", DOWNLOAD_URL);
    }
  }

  if (SIGNED) root.classList.add("signed");
  Array.prototype.forEach.call(document.querySelectorAll(".publisher"), function (el) { el.textContent = PUBLISHER; });
  Array.prototype.forEach.call(document.querySelectorAll("[data-download]"), function (a) { a.href = DOWNLOAD_URL; });

  var ua = navigator.userAgent || "";

  /* ── which browser, so step 1 shows only hers ──────────────────── */
  var browser = /Edg(e|A|iOS)?\//.test(ua) ? "edge"
              : /Firefox\/|FxiOS\//.test(ua) ? "firefox"
              : (/Chrome\/|CriOS\//.test(ua) && !/OPR\/|Opera|Vivaldi|YaBrowser|SamsungBrowser/.test(ua)) ? "chrome"
              : "";
  if (browser) {
    root.classList.add("browser-known", "b-" + browser);
    $("all-browsers-row").hidden = false;
    $("all-browsers").addEventListener("click", function () {
      root.classList.remove("browser-known");
      $("all-browsers-row").hidden = true;
    });
  }

  /* ── which computer ───────────────────────────────────────────────
   * Sure answers only. Anything unsure keeps the button. */
  function osKind() {
    if (/iPhone|iPad|iPod|Android/i.test(ua)) return "phone";
    if (/Macintosh|Mac OS X/.test(ua)) return (navigator.maxTouchPoints > 1) ? "phone" : "mac"; /* iPads say Macintosh */
    if (/CrOS/.test(ua)) return "chromeos";
    if (/Windows/.test(ua)) return "windows";
    if (/Linux|X11/.test(ua)) return "linux";
    return "";
  }

  function pageAddress() {
    var u = location.href.split("#")[0].split("?")[0];
    return u;
  }

  function showElsewhere(kind) {
    root.classList.add("is-elsewhere", "k-" + kind);
    $("getbox").hidden = true;
    $("elsewhere").hidden = false;
    var url = pageAddress();
    $("page-address").textContent = url.replace(/^https?:\/\//, "").replace(/\/index\.html$/, "/");
    $("mail-link").href = "mailto:?subject=" + encodeURIComponent("AurOS, for my Windows computer") +
      "&body=" + encodeURIComponent("Open this on the Windows computer you want to change:\n\n" + url + "\n");
  }

  function copied(msg) { $("copied").textContent = msg; }
  $("copy-link").addEventListener("click", function () {
    var url = pageAddress();
    var ok = function () { copied("Copied. Now paste it into an email or a message to yourself."); };
    var fallback = function () {
      var t = document.createElement("textarea");
      t.value = url; t.setAttribute("readonly", ""); t.style.position = "fixed"; t.style.opacity = "0";
      document.body.appendChild(t); t.select();
      var done = false;
      try { done = document.execCommand("copy"); } catch (e) { done = false; }
      document.body.removeChild(t);
      if (done) ok(); else copied("Your browser wouldn't copy it. The address is written just below.");
    };
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(url).then(ok, fallback);
    else fallback();
  });

  var kind = osKind();
  if (kind && kind !== "windows") {
    showElsewhere(kind);
  } else if (kind === "windows") {
    if (/Windows NT (5|6)\./.test(ua)) $("old-win").hidden = false;
    var uad = navigator.userAgentData;
    var hints = uad && typeof uad.getHighEntropyValues === "function";
    /* Without Chrome's hints (Firefox), the old UA string still tells
     * 64-bit Windows apart: a 64-bit browser says Win64/x64, a 32-bit
     * one on 64-bit Windows says WOW64, and on 32-bit Windows it says
     * neither. Chrome and Edge always say "Win64; x64", so for them the
     * hints below decide. */
    if (!hints) {
      if (/ARM64|aarch64/i.test(ua)) showElsewhere("arm");
      else if (!/Win64|WOW64|x64|x86_64|amd64/i.test(ua)) showElsewhere("win32");
    }
    /* Chrome and Edge say what the processor is. "arm" means the
     * computer is ARM even when this browser runs as x64 under
     * emulation. */
    if (hints) {
      uad.getHighEntropyValues(["architecture", "bitness"]).then(function (v) {
        if (v && v.architecture === "arm") showElsewhere("arm");
        else if (v && v.bitness === "32") showElsewhere("win32");
      }, function () { /* unsure: keep the button */ });
    }
  }
})();
