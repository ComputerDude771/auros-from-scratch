#!/usr/bin/env python3
"""mkseeds.py -- small, real seed fonts for fuzz_font.

A whole system font is hundreds of kilobytes, and a fuzzer mutating
that spends nearly all its time copying bytes that never reach a
parser. Subsetting a real font down to the glyphs fuzz_font draws keeps
every structure that matters -- cmap 4 and 12, composite glyphs, kern
and GPOS pair kerning (formats 1 and 2), CFF with subroutines and
hintmask, a .ttc -- in a few kilobytes.

Needs fontTools (pip install fonttools). The generated seeds are
committed in tools/fuzz/corpus/font, so this only runs when they are
regenerated:

    PYTHONPATH=<fonttools> python3 tools/fuzz/mkseeds.py
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.ttLib.ttCollection import TTCollection

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "corpus", "font")

TEXT = "Ta Wo P. AV fiÄéñœЖяЙ"

SOURCES = [
    ("dejavu-sans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", {}),
    ("liberation-sans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
     {"legacy_kern": True}),
    ("freesans.ttf", "/usr/share/fonts/truetype/freefont/FreeSans.ttf", {"legacy_kern": True}),
    ("inter.otf", "/usr/share/fonts/opentype/inter/Inter-Regular.otf", {}),
    ("inter-bold.otf", "/usr/share/fonts/opentype/inter/Inter-Bold.otf", {"desubroutinize": False}),
]


def make(name, src, opts):
    if not os.path.exists(src):
        print("skip (missing)", src)
        return None
    o = subset.Options()
    o.layout_features = ["kern"]
    o.notdef_outline = True
    o.hinting = False
    o.name_IDs = []
    o.legacy_kern = opts.get("legacy_kern", False)
    o.desubroutinize = opts.get("desubroutinize", False)
    f = TTFont(src)
    s = subset.Subsetter(o)
    s.populate(text=TEXT, unicodes=[0xF001, 0x1F600])
    s.subset(f)
    path = os.path.join(OUT, name)
    f.save(path)
    print("%-22s %6d bytes" % (name, os.path.getsize(path)))
    return path


def main():
    os.makedirs(OUT, exist_ok=True)
    made = [p for p in (make(*s) for s in SOURCES) if p]
    # A collection of two faces: font_load() takes the first.
    ttfs = [p for p in made if p.endswith(".ttf")][:2]
    if len(ttfs) == 2:
        c = TTCollection()
        c.fonts = [TTFont(p) for p in ttfs]
        path = os.path.join(OUT, "pair.ttc")
        c.save(path)
        print("%-22s %6d bytes" % ("pair.ttc", os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
