/* AurOS website — the only script on the site.
 *
 * It does one thing: the pre-download checklist gate. It is pure
 * progressive enhancement. With JavaScript off, every checkbox still
 * works and the download link is still a plain <a href> — we never trap
 * a reader behind a script. Nothing here is loaded from anywhere else,
 * nothing is measured, nothing is stored, nothing is sent.
 */
(function () {
  "use strict";

  var list = document.querySelector("[data-checklist]");
  var gate = document.querySelector("[data-gate]");
  if (!list || !gate) return;

  var boxes = Array.prototype.slice.call(
    list.querySelectorAll('input[type="checkbox"]')
  );
  var link = gate.querySelector("[data-gate-link]");
  var count = gate.querySelector("[data-gate-count]");
  if (!boxes.length || !link) return;

  function update() {
    var done = boxes.filter(function (b) { return b.checked; }).length;
    var all = done === boxes.length;

    if (count) {
      count.innerHTML = "";
      var b = document.createElement("b");
      b.textContent = done + " of " + boxes.length;
      count.appendChild(b);
      count.appendChild(
        document.createTextNode(
          all
            ? " confirmed. Read the safety page before you run it."
            : " confirmed. Work through the rest before you download."
        )
      );
    }

    /* The href is never removed: a link without one drops out of the
       accessibility tree. aria-disabled announces the state, and the
       click handler below is what actually holds the gate. */
    if (all) link.removeAttribute("aria-disabled");
    else link.setAttribute("aria-disabled", "true");
  }

  boxes.forEach(function (b) { b.addEventListener("change", update); });

  link.addEventListener("click", function (e) {
    if (link.getAttribute("aria-disabled") === "true") {
      e.preventDefault();
      var first = boxes.filter(function (b) { return !b.checked; })[0];
      if (first) first.focus();
    }
  });

  update();
})();
