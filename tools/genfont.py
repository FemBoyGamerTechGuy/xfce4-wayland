#!/usr/bin/env python3
"""Generate a compact 8-bit-alpha bitmap font as a C header.

The desktop clients (panel, exit dialog) must render text without pulling a
font stack (fontconfig/freetype/pango) into the runtime dependency set. This
build-time tool rasterizes the supported codepoint ranges into per-glyph
8-bit alpha bitmaps; the generated data is compiled into the client library.

Coverage (the panel renders real application names from .desktop files,
which are UTF-8 and frequently carry Latin accents):

  main table   U+0020..U+007E  printable ASCII
               U+00A0..U+024F  Latin-1 Supplement, Latin Extended-A/B
  punct table  U+2000..U+203F  dashes, quotes, bullets, ellipsis, primes

Two sizes are emitted: the base table (default 16 px) used by every client,
and a second larger table (default 24 px) for tall panels, menu rows and
calendar headers, where the base raster would look undersized. Tables are
direct-indexed by codepoint (O(1) lookup); uncovered slots carry NULL bit
pointers and render as the client's fallback advance.

Font source policy (distro-agnostic by design):

  1. Default: the font bundled in this repository, assets/fonts/
     DejaVuSans-latin.ttf (a subset of DejaVu Sans 2.37, see
     assets/fonts/README.md and THIRD-PARTY-LICENSES.md). It is always
     present in a clean checkout, so no system font package is ever a build
     requirement and the generated table is identical on every distribution.
  2. --font PATH: explicit override (a packager can rasterize their own
     TTF/OTF; Pillow must be able to open it). Fonts that lack the extended
     ranges still build — those slots simply render as fallback advances.

System fonts are deliberately NOT searched: font paths and names differ per
distribution (/usr/share/fonts/truetype/... vs /usr/share/fonts/TTF/...),
and relying on them made the build fail on systems where the documented
package list was in fact installed.

Usage: genfont.py -o OUTPUT [--font PATH] [--size PX] [--size2 PX]

Output: a C header defining xw_glyph_table/xw_glyph_table2 (main) and
xw_glyph_ptable/xw_glyph_ptable2 (punctuation) plus the XW_FONT_*
metrics/range macros.
"""

import argparse
import os
import sys

# tools/genfont.py -> repository root (works from any cwd, also in build copies)
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUNDLED_FONT = os.path.join(REPO_ROOT, "assets", "fonts", "DejaVuSans-latin.ttf")

# codepoint ranges rasterized into the two tables. (first, last) inclusive.
MAIN_FIRST, MAIN_LAST = 0x20, 0x24F
PUNCT_FIRST, PUNCT_LAST = 0x2000, 0x203F

# PIL is imported inside main() (deferred so --help works without Pillow);
# the module-level names are assigned there for rasterize().
Image = None
ImageDraw = None


def rasterize(font, tag, out, first, last, need_min):
    """Emit glyph data for `font` into `out` (list of C lines).

    Returns the glyph_table rows (one per codepoint in the range; missing
    glyphs get NULL bit pointers) and the (line_h, ascent) metrics.
    """
    glyphs = {}
    ascent, descent = font.getmetrics()
    for cp in range(first, last + 1):
        ch = chr(cp)
        try:
            bbox = font.getbbox(ch)
        except Exception:
            continue
        if bbox is None:
            continue
        w = max(1, bbox[2] - bbox[0])
        h = max(1, bbox[3] - bbox[1])
        img = Image.new("L", (w + 2, h + 2), 0)
        d = ImageDraw.Draw(img)
        d.text((-bbox[0] + 1, -bbox[1] + 1), ch, font=font, fill=255)
        glyphs[cp] = (w + 2, h + 2, img)

    if len(glyphs) < need_min:
        print("error: font provides only %d of %d codepoints in "
              "U+%04X..U+%04X — unusable"
              % (len(glyphs), last - first + 1, first, last), file=sys.stderr)
        sys.exit(1)

    line_h = ascent + descent
    glyph_table = []
    for cp in range(first, last + 1):
        if cp not in glyphs:
            glyph_table.append("    {0, 0, 0, 0, 0, NULL}, /* U+%04x */" % cp)
            continue
        w, h, img = glyphs[cp]
        data = list(img.getdata())
        out.append("/* U+%04x %r */" % (cp, chr(cp)))
        out.append("static const uint8_t xw_bits%s_%d[] = {" % (tag, cp))
        out.append("    " + ", ".join(str(b) for b in data) + ",")
        out.append("};")
        bbox = font.getbbox(chr(cp))
        # Draw origin: pen at (0, ascent); image holds glyph starting at (xoff, ascent + yoff)
        xoff = bbox[0] - 1
        yoff = bbox[1] - 1 - ascent
        adv = font.getlength(chr(cp))
        glyph_table.append("    {%d, %d, %d, %d, %d, xw_bits%s_%d},"
                           % (w, h, xoff, yoff, int(adv + 0.5), tag, cp))

    return glyph_table, line_h, ascent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--font",
                    help="explicit font file to rasterize (default: the "
                         "font bundled in this repository)")
    ap.add_argument("--size", type=int, default=16,
                    help="base rasterization size in px (default 16)")
    ap.add_argument("--size2", type=int, default=24,
                    help="second (larger) rasterization size in px "
                         "(default 24; 0 disables the second table)")
    args = ap.parse_args()

    try:
        global Image, ImageDraw
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("error: the Pillow python module is required at build time to\n"
              "       rasterize the bundled font into the client bitmap font.\n"
              "       Install your distribution's package (python3-pil on\n"
              "       Debian/Ubuntu, python-pillow on Arch/Artix,\n"
              "       python3-pillow on Fedora/openSUSE; see BUILDING.md), or\n"
              "       run: python3 -m pip install --user pillow",
              file=sys.stderr)
        return 1

    font_path = args.font
    if font_path:
        if not os.path.exists(font_path):
            print("error: --font '%s' does not exist" % font_path,
                  file=sys.stderr)
            return 1
        source_note = os.path.basename(font_path) + " (explicit --font)"
    else:
        font_path = BUNDLED_FONT
        if not os.path.exists(font_path):
            print("error: the bundled font asset is missing:\n"
                  "       expected: %s\n"
                  "       It ships in the repository and is the default\n"
                  "       build-time font source (no system font is used).\n"
                  "       A missing asset means a damaged checkout — restore it\n"
                  "       with: git checkout -- assets/fonts\n"
                  "       or re-extract the release archive." % font_path,
                  file=sys.stderr)
            return 1
        source_note = os.path.basename(font_path) + " (bundled asset, see assets/fonts/README.md)"

    try:
        font = ImageFont.truetype(font_path, args.size)
    except Exception as e:
        print("error: could not open '%s' as a font: %s\n"
              "       (Pillow/%s)" % (font_path, e,
                                    getattr(Image, "__version__", "?")),
              file=sys.stderr)
        return 1

    out = ["/* Generated by tools/genfont.py — DO NOT EDIT.",
           " * Rasterized at build time from: " + source_note,
           " * The bundled asset is a subset of DejaVu Sans 2.37 — license",
           " * and provenance: assets/fonts/LICENSE-DejaVuSans.txt and",
           " * THIRD-PARTY-LICENSES.md. Only bitmap data is compiled in; the",
           " * font file itself is used at build time only.",
           " */",
           "#ifndef XW_FONT_DATA_H",
           "#define XW_FONT_DATA_H",
           "",
           "#define XW_FONT_FIRST 0x20",
           "#define XW_FONT_LAST 0x24f",
           "#define XW_FONT_P_FIRST 0x2000",
           "#define XW_FONT_P_LAST 0x203f",
           "",
           "struct xw_glyph {",
           "    uint8_t w, h;",
           "    int8_t xoff, yoff; /* draw offset from pen origin */",
           "    uint8_t adv;       /* advance to next character */",
           "    const uint8_t *bits; /* w*h alpha bytes */",
           "};",
           "",
           "/* direct codepoint-indexed tables: index = cp - FIRST. Slots the",
           " * font does not cover have bits == NULL (fallback advance). */",
           ""]

    # ASCII + Latin ranges must be substantially covered; the punctuation
    # range is optional (a font without dashes/quotes still builds).
    main_need = (0x24F - 0x20 + 1) // 2
    punct_need = 8

    table, line_h, ascent = rasterize(font, "", out, MAIN_FIRST, MAIN_LAST,
                                      main_need)
    ptable, _, _ = rasterize(font, "_p", out, PUNCT_FIRST, PUNCT_LAST,
                             punct_need)
    out.append("")
    out.append("#define XW_FONT_LINE_H %d" % line_h)
    out.append("#define XW_FONT_ASCENT %d" % ascent)
    out.append("")
    out.append("static const struct xw_glyph xw_glyph_table[] = {")
    out.extend(table)
    out.append("};")
    out.append("static const struct xw_glyph xw_glyph_ptable[] = {")
    out.extend(ptable)
    out.append("};")
    out.append("")

    n2 = len(table)
    if args.size2 and args.size2 > 0:
        try:
            font2 = ImageFont.truetype(font_path, args.size2)
        except Exception as e:
            print("error: could not rasterize size %d from '%s': %s"
                  % (args.size2, font_path, e), file=sys.stderr)
            return 1
        table2, line_h2, ascent2 = rasterize(font2, "2", out, MAIN_FIRST,
                                             MAIN_LAST, main_need)
        ptable2, _, _ = rasterize(font2, "2p", out, PUNCT_FIRST, PUNCT_LAST,
                                  punct_need)
        out.append("")
        out.append("#define XW_FONT2_LINE_H %d" % line_h2)
        out.append("#define XW_FONT2_ASCENT %d" % ascent2)
        out.append("")
        out.append("static const struct xw_glyph xw_glyph_table2[] = {")
        out.extend(table2)
        out.append("};")
        out.append("static const struct xw_glyph xw_glyph_ptable2[] = {")
        out.extend(ptable2)
        out.append("};")
        n2 = len(table2)

    out.append("")
    out.append("#endif")

    outdir = os.path.dirname(os.path.abspath(args.output))
    if outdir and not os.path.isdir(outdir):
        os.makedirs(outdir, exist_ok=True)
    with open(args.output, "w") as f:
        f.write("\n".join(out) + "\n")
    print("genfont: %d+%d glyphs (line height %d, size %d%s) source: %s -> %s"
          % (len(table), len(ptable), line_h, args.size,
             ", second table line height %d (size %d, %d+%d glyphs)"
             % (line_h2, args.size2, n2, len(ptable2)) if args.size2 else "",
             os.path.basename(font_path), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
