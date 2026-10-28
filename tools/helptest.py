#!/usr/bin/env python3
"""helptest: the help pages on the computer, checked the way she meets them.

    python3 -I tools/helptest.py                 check rootfs/usr/share/auros/help
    python3 -I tools/helptest.py --dir <folder>  check another copy

The Help button opens these pages (rootfs/usr/share/auros/help/) in the
browser, on a computer that may not be online yet -- "Getting online" is
one of them. Fails if:

  - a page the index promises is missing, or the index misses one;
  - any link, stylesheet or #place inside the folder does not exist;
  - anything would be fetched from elsewhere (http:, https:, //, file:),
    or a page has a script;
  - a topic page has no way back to the index or does not say how to
    close it ("Close this", the band's button);
  - visible text uses a word docs/EASY.md rule 2 forbids (the list is
    read from EASY.md itself, so the rule and the check cannot drift);
  - the stylesheet sets any text under 18px, or body text under 22px;
  - any text colour pair, light or dark, is under 4.5:1.

Standard library only; no browser needed.
"""
import html
import html.parser
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

TOPICS = ["getting-online.html", "programs.html", "printing.html",
          "video-calls.html", "windows.html", "never-call.html"]

fails = []
passes = 0


def check(cond, what, why=""):
    global passes
    if cond:
        passes += 1
        print("ok   " + what)
    else:
        fails.append(what)
        print("FAIL " + what + (" -- " + why if why else ""))


class Page(html.parser.HTMLParser):
    """Links, ids, scripts and the words a person can see."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.refs, self.ids, self.text = [], set(), []
        self.scripts = 0
        self.skip = 0          # inside <script>/<style>/<title>

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag in ("script", "style", "title"):
            self.skip += 1
        if tag == "script":
            self.scripts += 1
        for k in ("href", "src"):
            if a.get(k) is not None:
                self.refs.append(a[k])
        if a.get("id"):
            self.ids.add(a["id"])
        for k, v in attrs:
            if k.startswith("on"):
                self.scripts += 1          # an onclick= is a script too

    def handle_endtag(self, tag):
        if tag in ("script", "style", "title") and self.skip:
            self.skip -= 1

    def handle_data(self, data):
        if not self.skip:
            self.text.append(data)


def forbidden_words():
    """docs/EASY.md rule 2's list: the italic run after "may not say"."""
    with open(os.path.join(REPO, "docs", "EASY.md"), encoding="utf-8") as f:
        easy = f.read()
    m = re.search(r"may not\s+say,[^*]*\*(.+?)\*", easy, re.S)
    if not m:
        return []
    words = [w.strip(" .\n") for w in m.group(1).replace("\n", " ").split(",")]
    return [w for w in words if w]


# ── colour ────────────────────────────────────────────────────────
def rgb(h):
    h = h.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


def lum(c):
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in rgb(c)]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def ratio(a, b):
    x, y = lum(a), lum(b)
    return (max(x, y) + 0.05) / (min(x, y) + 0.05)


def tokens(block):
    return dict(re.findall(r"--([a-z0-9-]+)\s*:\s*(#[0-9A-Fa-f]{3,6})", block))


def main():
    args = sys.argv[1:]
    d = os.path.join(REPO, "rootfs", "usr", "share", "auros", "help")
    if "--dir" in args:
        d = os.path.abspath(args[args.index("--dir") + 1])
    print("checking", os.path.relpath(d, REPO) if d.startswith(REPO) else d)

    # 1. the pages
    pages = sorted(f for f in os.listdir(d) if f.endswith(".html")) if os.path.isdir(d) else []
    check("index.html" in pages, "index.html exists")
    for t in TOPICS:
        check(t in pages, f"{t} exists")
    check(os.path.isfile(os.path.join(d, "help.css")), "help.css exists")

    parsed = {}
    for p in pages:
        pg = Page()
        with open(os.path.join(d, p), encoding="utf-8") as f:
            pg.feed(f.read())
        parsed[p] = pg

    # 2. links inside the folder, and 3. nothing from elsewhere
    missing, outside = [], []
    for p, pg in parsed.items():
        for r in pg.refs:
            if re.match(r"^(https?:|//|file:|ftp:|data:)", r, re.I):
                outside.append(f"{r} on {p}")
                continue
            if re.match(r"^(mailto:|tel:|javascript:)", r, re.I):
                outside.append(f"{r} on {p}")
                continue
            target, _, frag = r.partition("#")
            target = target or p
            if "/" in target or target.startswith(".."):
                outside.append(f"{r} on {p} (leaves the folder)")
                continue
            if not os.path.isfile(os.path.join(d, target)):
                missing.append(f"{r} on {p}")
            elif frag and target.endswith(".html") and frag not in parsed.get(target, Page()).ids:
                missing.append(f"{r} on {p} (no such place)")
    check(not missing, f"every link and stylesheet exists ({sum(len(x.refs) for x in parsed.values())} checked)",
          "; ".join(missing[:8]))
    check(not outside, "nothing is fetched from anywhere else, and no link leaves the folder",
          "; ".join(outside[:8]))
    css_path = os.path.join(d, "help.css")
    css = open(css_path, encoding="utf-8").read() if os.path.isfile(css_path) else ""
    css_out = re.findall(r"url\(\s*['\"]?([^)'\"]+)", css) + re.findall(r"@import\s+['\"]?([^'\";]+)", css)
    check(not css_out, "the stylesheet loads nothing", ", ".join(css_out))
    scripted = [p for p, pg in parsed.items() if pg.scripts]
    check(not scripted, "no page has a script", ", ".join(scripted))

    # 4. the index lists every topic; every topic leads back and says how to close it
    idx = parsed.get("index.html")
    if idx:
        listed = {r.partition("#")[0] for r in idx.refs}
        check(all(t in listed for t in TOPICS), "the index links every topic",
              ", ".join(t for t in TOPICS if t not in listed))
    for t in TOPICS:
        pg = parsed.get(t)
        if not pg:
            continue
        check("index.html" in pg.refs, f"{t}: a way back to all the help")
        words = " ".join(pg.text)
        check("Close this" in words, f"{t}: says how to close it (Close this)")

    # 5. her words (docs/EASY.md rule 2)
    banned = forbidden_words()
    check(len(banned) >= 20, f"read docs/EASY.md's list of words she does not use ({len(banned)})")
    hits = []
    for p, pg in parsed.items():
        text = html.unescape(" ".join(pg.text))
        for w in banned:
            if re.search(r"(?<![\w-])" + re.escape(w) + r"(?![\w-])", text, re.I):
                hits.append(f'"{w}" on {p}')
    check(not hits, "no word from docs/EASY.md rule 2 on any page", "; ".join(hits[:10]))

    # 6. sizes
    sizes = [(float(v), u) for v, u in re.findall(r"font-size\s*:\s*([\d.]+)(px|em|rem)", css)]
    sizes += [(float(v), "px") for v in re.findall(r"font\s*:[^;]*?\b([\d.]+)px", css)]
    small = [f"{v}{u}" for v, u in sizes if (u == "px" and v < 18) or (u != "px" and v < 0.75)]
    check(not small, "no text under 18px in the stylesheet", ", ".join(small))
    body = re.search(r"body\s*\{[^}]*font-size\s*:\s*([\d.]+)px", css)
    check(bool(body) and float(body.group(1)) >= 22, "body text 22px or larger",
          body.group(1) + "px" if body else "no body font-size")
    inline = [p for p, pg in parsed.items()
              if re.search(r"style\s*=", open(os.path.join(d, p), encoding="utf-8").read())]
    check(not inline, "no page overrides the stylesheet inline", ", ".join(inline))

    # 7. colour, light and dark
    light = tokens(css.split("@media")[0])
    dark_m = re.search(r"prefers-color-scheme:\s*dark\)\s*\{(.+?)\}\s*\}", css, re.S)
    schemes = [("light", light)]
    if dark_m:
        schemes.append(("dark", {**light, **tokens(dark_m.group(1))}))
    check(len(schemes) == 2, "a dark variant exists")
    # (text, background) pairs actually used by help.css
    pairs = [("ink", "bg"), ("ink-2", "bg"), ("link", "bg"), ("ink", "soft"),
             ("link", "soft"), ("ink", "card"), ("link", "card"),
             ("bg", "go"), ("ink", "warn-bg"), ("link", "warn-bg")]
    for name, t in schemes:
        low = []
        for fg, bg in pairs:
            if fg not in t or bg not in t:
                low.append(f"--{fg}/--{bg} undefined")
                continue
            r = ratio(t[fg], t[bg])
            if r < 4.5:
                low.append(f"--{fg} on --{bg} {r:.2f}:1")
        check(not low, f"{name}: every text colour 4.5:1 or better", "; ".join(low))

    print(f"\n{passes} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
