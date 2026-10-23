"""vision.py -- what the end-to-end test can see on a screendump.

Nothing here knows about QEMU. Every function takes an RGB numpy array
(height x width x 3, uint8) as a screendump gives it, so each one can be
checked against a saved PNG on its own:

    python3 tools/e2e/vision.py SHOT.png          # buttons, boxes, words

The AurOS installer draws its own window (no controls, so no UI
Automation tree), which is why it is found by colour: the primary button
is the only large solid block of the accent on the page.
"""
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ACCENT = (0x7D, 0xD3, 0xC0)      # wizard.c C_ACCENT, also aurshell's accent


def load(path):
    return np.asarray(Image.open(path).convert("RGB"))


def crop(img, region):
    """region is (x, y, w, h) in screen pixels, or None for all of it."""
    if region is None:
        return img, 0, 0
    x, y, w, h = [int(v) for v in region]
    H, W = img.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(W, x + w), min(H, y + h)
    return img[y0:y1, x0:x1], x0, y0


def components(mask):
    """4-connected components of a boolean mask, by runs and union-find.

    Returns [(x0, y0, x1, y1, pixels)], x1/y1 exclusive. Written out
    rather than taken from scipy so the runner needs nothing extra."""
    h, w = mask.shape
    pad = np.zeros((h, w + 2), np.int8)
    pad[:, 1:-1] = mask
    d = np.diff(pad, axis=1)
    runs, parent = [], []

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    prev = []
    for y in range(h):
        starts = np.flatnonzero(d[y] == 1)
        ends = np.flatnonzero(d[y] == -1)
        cur = []
        k = 0
        for s, e in zip(starts.tolist(), ends.tolist()):
            i = len(runs)
            runs.append((y, s, e))
            parent.append(i)
            cur.append(i)
            # the row above's runs are disjoint and sorted, so the first
            # one that can touch us only ever moves right
            while k < len(prev) and runs[prev[k]][2] <= s:
                k += 1
            j = k
            while j < len(prev) and runs[prev[j]][1] < e:
                a, b = find(i), find(prev[j])
                if a != b:
                    parent[a] = b
                j += 1
        prev = cur
    boxes = {}
    for i, (y, s, e) in enumerate(runs):
        r = find(i)
        b = boxes.get(r)
        if b is None:
            boxes[r] = [s, y, e, y + 1, e - s]
        else:
            b[0] = min(b[0], s); b[1] = min(b[1], y)
            b[2] = max(b[2], e); b[3] = max(b[3], y + 1); b[4] += e - s
    return [tuple(b) for b in boxes.values()]


def accent_mask(img, rgb=ACCENT):
    """Pixels that are `rgb` drawn 72-100% opaque over a dark ground (the
    wizard's button is 92%, 100% under the mouse, 80% pressed). Each
    channel over its target gives about the same ratio, which is what
    tells the teal from the warm accent at any brightness."""
    a = img.astype(np.float32)
    q = a / np.array(rgb, np.float32)
    lo, hi = q.min(axis=2), q.max(axis=2)
    return (lo >= 0.70) & (hi <= 1.12) & (hi - lo <= 0.14)


def find_buttons(img, region=None, rgb=ACCENT):
    """Solid blocks of the accent the size of a button, largest first.

    [(x, y, w, h)] in screen coordinates. A checked box (22 px), a step
    dot, accent-coloured text and the window's backdrop ribbons are all
    excluded by size or by how solid they are."""
    sub, ox, oy = crop(img, region)
    out = []
    for x0, y0, x1, y1, n in components(accent_mask(sub, rgb)):
        w, h = x1 - x0, y1 - y0
        if w >= 80 and 30 <= h <= 80 and n >= 0.55 * w * h:
            out.append((x0 + ox, y0 + oy, w, h))
    out.sort(key=lambda b: -b[2] * b[3])
    return out


def find_checked_boxes(img, region=None):
    """Ticked check boxes: a solid 18-28 px square of the accent or the
    warm accent (the wizard's second box is tinted warm)."""
    sub, ox, oy = crop(img, region)
    out = []
    for rgb in (ACCENT, (0xF2, 0xB8, 0x80)):
        for x0, y0, x1, y1, n in components(accent_mask(sub, rgb)):
            w, h = x1 - x0, y1 - y0
            if 19 <= w <= 28 and 19 <= h <= 28 and abs(w - h) <= 3 and n >= 0.5 * w * h:
                out.append((x0 + ox, y0 + oy, w, h))
    return out


def find_unchecked_boxes(img, region=None):
    """Empty check boxes: an 18-28 px square outline, lighter than a dark,
    flat inside. Large glyphs have hollow outlines too, but their insides
    are the (light) text colour, and they are rarely square."""
    sub, ox, oy = crop(img, region)
    lum = sub.astype(np.int16).sum(axis=2) // 3
    sat = sub.max(axis=2).astype(np.int16) - sub.min(axis=2)
    ring = (lum >= 38) & (lum <= 150) & (sat <= 45)
    out = []
    for x0, y0, x1, y1, n in components(ring):
        w, h = x1 - x0, y1 - y0
        if not (17 <= w <= 30 and 17 <= h <= 30 and abs(w - h) <= 2):
            continue
        inner = lum[y0 + 4:y1 - 4, x0 + 4:x1 - 4]
        band = ring[y0:y1, x0:x1].copy()
        band[3:-3, 3:-3] = False
        if inner.size == 0 or inner.max() > 60 or inner.std() > 4:
            continue
        # the outline really is on the square's edge, all the way round
        edge = np.zeros_like(band)
        edge[:3, :] = edge[-3:, :] = edge[:, :3] = edge[:, -3:] = True
        if band[edge].mean() < 0.55:
            continue
        if ring[y0 + 4:y1 - 4, x0 + 4:x1 - 4].any():
            continue
        out.append((x0 + ox, y0 + oy, w, h))
    out.sort(key=lambda b: (b[1], b[0]))
    return out


# ── OCR ──────────────────────────────────────────────────────────────

def ocr(img, region=None, scale=2, psm=3):
    """Words on the screen: [{'text', 'x', 'y', 'w', 'h', 'conf', 'line'}],
    in screen coordinates. Light text on a dark ground is inverted first;
    tesseract reads dark on light far better."""
    sub, ox, oy = crop(img, region)
    if sub.size == 0:
        return []
    # Text as dark ink on white, whatever the screen: on a dark ground the
    # brightest channel (teal or violet text is then as bright as white),
    # on a light one the darkest. Then everything a little away from the
    # ground is pushed to full ink, which is what makes the muted grey
    # hints (#55606E on #0B0E14) readable at all.
    dark = sub.mean() < 110
    m = (sub.max(axis=2) if dark else sub.min(axis=2)).astype(np.float32)
    bg = float(np.median(m))
    ink = (m - bg) if dark else (bg - m)
    m = 255 - np.clip((ink - 8) * 3, 0, 255)
    g = Image.fromarray(m.astype(np.uint8))
    if scale != 1:
        g = g.resize((g.width * scale, g.height * scale), Image.LANCZOS)
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "o.png")
        g.save(p)
        try:
            r = subprocess.run(["tesseract", p, "stdout", "--psm", str(psm), "tsv"],
                               capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired):
            return []
    words = []
    for ln in r.stdout.splitlines()[1:]:
        f = ln.split("\t")
        if len(f) < 12 or f[0] != "5":
            continue
        t = f[11].strip()
        if not t:
            continue
        try:
            conf = float(f[10])
        except ValueError:
            conf = -1
        words.append({
            "text": t,
            "x": int(f[6]) // scale + ox, "y": int(f[7]) // scale + oy,
            "w": max(1, int(f[8]) // scale), "h": max(1, int(f[9]) // scale),
            "conf": conf, "line": (int(f[2]), int(f[3]), int(f[4])),
        })
    return words


def _norm(s, case=False):
    return re.sub(r"[^A-Za-z0-9]", "", s if case else s.lower())


def text_of(words):
    return " ".join(w["text"] for w in words)


def find_text(words, phrase, last=False, case=False):
    """The box (x, y, w, h) of `phrase` among OCR words, or None.

    Words are compared without case or punctuation, so "Yes," finds
    "Yes"; a phrase must be consecutive words on one text line. With
    last=True the lowest match on screen wins (a placeholder below the
    sentence that names it). case=True compares letters' case too."""
    want = [_norm(p, case) for p in phrase.split() if _norm(p, case)]
    if not want:
        return None
    hits = []
    for i in range(len(words)):
        j, k = i, 0
        while k < len(want) and j < len(words):
            got = _norm(words[j]["text"], case)
            if not got:
                j += 1
                continue
            if got != want[k] and not (k == len(want) - 1 and got.startswith(want[k])):
                break
            if j > i and abs((words[j]["y"] + words[j]["h"] / 2) -
                             (words[i]["y"] + words[i]["h"] / 2)) > words[i]["h"]:
                break
            j += 1
            k += 1
        if k == len(want):
            ws = words[i:j]
            x0 = min(w["x"] for w in ws); y0 = min(w["y"] for w in ws)
            x1 = max(w["x"] + w["w"] for w in ws); y1 = max(w["y"] + w["h"] for w in ws)
            hits.append((x0, y0, x1 - x0, y1 - y0))
    if not hits:
        return None
    hits.sort(key=lambda b: (b[1], b[0]))
    return hits[-1] if last else hits[0]


def find_lone_word(words, word):
    """A word that is a line on its own, or with one mark beside it (what
    an input box shows: its placeholder, or what was typed and a tick),
    the lowest on screen; None if none."""
    lines = {}
    for w in words:
        lines.setdefault(w["line"], []).append(w)
    hits = [x for ws in lines.values() if len(ws) <= 2 for x in ws
            if _norm(x["text"], True) == _norm(word, True)]
    if not hits:
        return None
    w = max(hits, key=lambda w: w["y"])
    return (w["x"], w["y"], w["w"], w["h"])


def centre(box):
    x, y, w, h = box
    return x + w // 2, y + h // 2


if __name__ == "__main__":
    for p in sys.argv[1:]:
        im = load(p)
        print(p, im.shape)
        print("  buttons  ", find_buttons(im))
        print("  checked  ", find_checked_boxes(im))
        print("  unchecked", find_unchecked_boxes(im))
        ws = ocr(im)
        print("  words    ", text_of(ws)[:400])
