/* check.js — what this browser will say about the computer it runs on.
 * Everything is read here and shown here. Nothing is stored or sent. */
(function () {
  "use strict";
  var out = document.querySelector("[data-readout]");
  if (!out) return;
  var nav = navigator, ua = nav.userAgent || "";

  function row(label, value, verdict, note) {
    var li = document.createElement("li");
    var chip = document.createElement("span");
    chip.className = "chip " + ({ good: "ok", bad: "no", maybe: "warn", info: "sim" }[verdict] || "");
    chip.textContent = { good: "fine", bad: "no", maybe: "see note", info: "for information" }[verdict] || "unknown";
    var body = document.createElement("div");
    var p1 = document.createElement("p"); p1.className = "what";
    p1.appendChild(document.createTextNode(label + ": "));
    var b = document.createElement("b"); b.textContent = value; p1.appendChild(b);
    var p2 = document.createElement("p"); p2.className = "how"; p2.textContent = note;
    body.appendChild(p1); body.appendChild(p2);
    li.appendChild(chip); li.appendChild(body);
    return li;
  }

  function finish(hi) {
    out.innerHTML = "";
    var plat = (nav.userAgentData && nav.userAgentData.platform) || "";
    var isWin = /Windows/i.test(plat) || /Windows NT/i.test(ua);
    var mobile = (nav.userAgentData && nav.userAgentData.mobile) || /Android|iPhone|iPad|Mobile/i.test(ua);

    /* the operating system */
    if (isWin) {
      var v = hi && hi.platformVersion ? parseInt(hi.platformVersion.split(".")[0], 10) : NaN;
      var name = isNaN(v) ? "Windows (this browser does not say whether 10 or 11)" : v >= 13 ? "Windows 11" : v >= 1 ? "Windows 10" : "Windows older than 10";
      var ok = isNaN(v) || v >= 1;
      out.appendChild(row("Operating system", name, ok ? "good" : "bad",
        ok ? "The installer runs on Windows 10 and 11." : "The installer needs Windows 10 or 11."));
    } else {
      out.appendChild(row("Operating system", mobile ? "a phone or tablet" : (plat || "not Windows"), "maybe",
        "The installer runs on Windows 10 or 11. Open this page on the PC you are thinking about to see what it says there."));
    }

    /* the processor */
    var arch = hi && hi.architecture ? hi.architecture : "", bits = hi && hi.bitness ? hi.bitness : "";
    if (arch) {
      if (/arm/i.test(arch)) out.appendChild(row("Processor", "ARM", "bad", "AurOS is built for 64-bit Intel and AMD processors. A Windows-on-ARM PC cannot run it."));
      else out.appendChild(row("Processor", "Intel or AMD" + (bits ? ", " + bits + "-bit" : ""), bits === "32" ? "bad" : "good",
        bits === "32" ? "AurOS needs a 64-bit processor." : "AurOS is built for 64-bit Intel and AMD processors."));
    } else if (isWin) {
      var w64 = /Win64|x64|WOW64/i.test(ua), arm = /ARM/i.test(ua);
      out.appendChild(row("Processor", arm ? "ARM" : w64 ? "64-bit (from the browser’s name for itself)" : "not said", arm ? "bad" : w64 ? "good" : "unknown",
        "AurOS is built for 64-bit Intel and AMD processors. This browser does not say more precisely; most Windows PCs are Intel or AMD."));
    } else {
      out.appendChild(row("Processor", "not this PC", "unknown", "Open this page on the Windows PC to see."));
    }

    /* threads */
    if (nav.hardwareConcurrency) out.appendChild(row("Processor threads", String(nav.hardwareConcurrency), "info",
      "How many things the processor can do at once. AurOS has no published minimum."));

    /* memory */
    if (nav.deviceMemory) out.appendChild(row("Memory", (nav.deviceMemory >= 8 ? "8 GB or more" : "about " + nav.deviceMemory + " GB"), "info",
      "Browsers round this, and stop counting at 8 GB on purpose. AurOS has no published minimum yet."));
    else out.appendChild(row("Memory", "not said by this browser", "unknown", "Only some browsers say. Settings → System → About shows it."));

    /* the screen */
    var dpr = window.devicePixelRatio || 1;
    var sw = Math.round(screen.width * dpr), sh = Math.round(screen.height * dpr);
    var big = Math.max(sw, sh) >= 1024 && Math.min(sw, sh) >= 600;
    out.appendChild(row("Screen", sw + " × " + sh, big ? "good" : "maybe",
      big ? "AurOS’s desktop is designed to work down to 1024 × 600." : "AurOS’s desktop is designed for screens of at least 1024 × 600."));

    /* what it can never see */
    out.appendChild(row("Disk, firmware, Secure Boot, encryption", "invisible to web pages", "unknown",
      "No web page can see these, and none should. The installer’s Check this PC reads them, and changes nothing."));
  }

  if (nav.userAgentData && nav.userAgentData.getHighEntropyValues) {
    nav.userAgentData.getHighEntropyValues(["platformVersion", "architecture", "bitness"]).then(finish, function () { finish(null); });
  } else finish(null);
})();
