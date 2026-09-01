#!/usr/bin/env python3
"""One-time verification: the ASCII-subset font must rasterize pixel-identically
to the full DejaVu Sans for every glyph and metric that genfont.py consumes.

Run when the bundled asset is (re)created. Not part of the regular test suite:
the full font only exists on dev machines with dejavu installed."""

import sys
from PIL import ImageFont

FULL = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
SUB = "assets/fonts/DejaVuSans-ascii.ttf"

def glyphs(path, size):
    f = ImageFont.truetype(path, size)
    out = {}
    asc, desc = f.getmetrics()
    for cp in range(0x20, 0x7F):
        ch = chr(cp)
        bbox = f.getbbox(ch)
        out[cp] = (bbox, f.getlength(ch))
    return (asc, desc), out

fails = 0
for size in (12, 16, 20, 24):
    (fa, fd), fg = glyphs(FULL, size)
    (sa, sd), sg = glyphs(SUB, size)
    if (fa, fd) != (sa, sd):
        print(f"size {size}: metrics differ: full=({fa},{fd}) sub=({sa},{sd})")
        fails += 1
    for cp in fg:
        if fg[cp] != sg[cp]:
            print(f"size {size}: glyph {chr(cp)!r} differs: {fg[cp]} vs {sg[cp]}")
            fails += 1
# byte-level bitmap comparison at the default build size
from PIL import Image, ImageDraw
for path, tag in ((FULL, "full"), (SUB, "sub")):
    f = ImageFont.truetype(path, 16)
    img = Image.new("L", (640, 32), 0)
    d = ImageDraw.Draw(img)
    d.text((2, 2), "".join(chr(c) for c in range(0x20, 0x7F)), font=f, fill=255)
    img.save(f"/tmp/fontcmp-{tag}.png")
import hashlib
h = []
for tag in ("full", "sub"):
    with open(f"/tmp/fontcmp-{tag}.png", "rb") as fp:
        h.append(hashlib.sha256(fp.read()).hexdigest())
if h[0] != h[1]:
    print("rendered strip hash mismatch:", h)
    fails += 1

print(f"subset verification: {'PASS (metrics, advances and rendered pixels identical)' if fails == 0 else f'{fails} FAILURES'}")
sys.exit(1 if fails else 0)
