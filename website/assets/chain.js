/* chain.js — the Secure Boot chain on how-it-works.html.
 * Facts from docs/AURBRIDGE.md, "Secure Boot stays on", and docs/SIGNING.md. */
(function () {
  "use strict";
  var chain = document.querySelector("[data-chain]");
  if (!chain) return;
  var detail = chain.querySelector("[data-chain-detail]");
  var links = Array.prototype.slice.call(chain.querySelectorAll("[data-link]"));
  var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  var D = [
    { t: "The PC’s firmware, with Secure Boot on",
      file: "built into the PC",
      signed: "Holds a list of keys it trusts. Almost every PC sold with Windows lists “Microsoft Corporation UEFI CA 2011”.",
      checks: "Before it runs anything from the drive, it checks that thing was signed by a key on its list. shim is.",
      note: "AurOS never asks for this list to be changed." },
    { t: "shim",
      file: "\\EFI\\AurOS\\shimx64.efi",
      signed: "Ubuntu’s shim, signed by Microsoft through the Microsoft Corporation UEFI CA 2011.",
      checks: "Carries Canonical’s certificate, and checks that GRUB was signed by Canonical before running it.",
      note: "It is the same shim Ubuntu itself uses. AurOS adds no key of its own." },
    { t: "GRUB",
      file: "\\EFI\\AurOS\\grubx64.efi",
      signed: "Canonical’s, checked by shim.",
      checks: "Reads the grub.cfg beside it in \\EFI\\AurOS, and checks the kernel’s signature before starting it.",
      note: "Loaded from \\EFI\\AurOS it reads the configuration beside itself first, even when a real Ubuntu keeps one in \\EFI\\ubuntu; that case is tested." },
    { t: "The Linux kernel",
      file: "Ubuntu 24.04’s kernel",
      signed: "Canonical’s, checked by GRUB.",
      checks: "With Secure Boot on it locks itself down: modules that are not signed are refused, and some ways into its memory are shut.",
      note: "The installer’s text screen prints “secure   Secure Boot on; kernel lockdown integrity” on its first screen." },
    { t: "aurshell, the desktop",
      file: "an ordinary program on AurOS’s partition",
      signed: "Not part of the signature chain. Secure Boot’s job ends once a signed kernel is running.",
      checks: "Paints the desktop straight to the screen through the kernel’s display interface (DRM/KMS): no X11, no Wayland compositor.",
      note: "Saying the desktop is “checked by Secure Boot” would be untrue, so this page does not." }
  ];

  var cur = 0, timer = 0;
  function show(i, fromUser) {
    cur = i;
    links.forEach(function (b, j) {
      b.setAttribute("aria-pressed", j === i ? "true" : "false");
      b.classList.toggle("passed", j < i);
    });
    var d = D[i];
    detail.innerHTML = "";
    var h = document.createElement("h3"); h.textContent = d.t; detail.appendChild(h);
    var f = document.createElement("p"); f.className = "mono small"; f.textContent = d.file; detail.appendChild(f);
    var dl = document.createElement("dl");
    [["Signed", d.signed], ["Does", d.checks]].forEach(function (r) {
      var dt = document.createElement("dt"); dt.textContent = r[0];
      var dd = document.createElement("dd"); dd.textContent = r[1];
      dl.appendChild(dt); dl.appendChild(dd);
    });
    detail.appendChild(dl);
    var n = document.createElement("p"); n.className = "small"; n.textContent = d.note; detail.appendChild(n);
    if (fromUser) stop();
  }
  function stop() { if (timer) { clearInterval(timer); timer = 0; } }
  links.forEach(function (b, i) {
    b.addEventListener("click", function () { show(i, true); });
    b.addEventListener("keydown", function (e) {
      if (e.key === "ArrowRight" || e.key === "ArrowDown") { e.preventDefault(); var n = Math.min(links.length - 1, i + 1); links[n].focus(); show(n, true); }
      if (e.key === "ArrowLeft" || e.key === "ArrowUp") { e.preventDefault(); var p = Math.max(0, i - 1); links[p].focus(); show(p, true); }
    });
  });
  show(0);
  /* walk the chain once by itself, the way a start-up does, unless the
     reader prefers less motion or starts pressing things */
  if (!reduce && "IntersectionObserver" in window) {
    var io = new IntersectionObserver(function (es) {
      if (!es[0].isIntersecting) return;
      io.disconnect();
      timer = setInterval(function () { if (cur >= D.length - 1) { stop(); return; } show(cur + 1); }, 2200);
    }, { threshold: 0.6 });
    io.observe(chain);
  }
})();
