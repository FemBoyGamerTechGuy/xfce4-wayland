# Bundled build-time font asset

`DejaVuSans-latin.ttf` is a **subset of DejaVu Sans 2.37** covering
U+0020..U+007E (printable ASCII), U+00A0..U+024F (Latin-1 Supplement,
Latin Extended-A/B) and U+2000..U+203F (general punctuation: dashes,
quotes, bullets, ellipsis) — the exact ranges `tools/genfont.py`
rasterizes into the client bitmap font.

Why it exists: the build must not depend on any system font file.
Distributions place fonts at different paths and names
(`/usr/share/fonts/truetype/…` on Debian/Ubuntu, `/usr/share/fonts/TTF/…`
on Arch/Artix, `/usr/share/fonts/…` elsewhere), so "find a system TTF"
is a distro-specific assumption. Bundling a 78552-byte subset makes the
build deterministic and distro-agnostic; no font package is a build
requirement.

Why Latin (not just ASCII): the panel renders real application names
from XDG `.desktop` files, which are UTF-8 and frequently carry accented
letters ("Éditeur de texte", "Größe", "Vim"). With the older
ASCII-only raster every non-ASCII letter rendered as an invisible blank
gap — application names appeared to be missing letters. The Latin
coverage plus the UTF-8-aware renderer (`src/libxwcl/xwc-draw.c`) fixes
that; codepoints outside the rasterized ranges (CJK, emoji) render a
visible hollow fallback box instead of silently vanishing.

The historical `DejaVuSans-ascii.ttf` (ASCII-only subset) is kept for
reference/packagers who need the minimal footprint; use
`make XW_FONT=assets/fonts/DejaVuSans-ascii.ttf` to rasterize it
instead (application names with accents will show fallback boxes).

Provenance and license:

- Upstream: DejaVu fonts, https://dejavu-fonts.github.io/ — version 2.37
  (the version string is preserved inside the file).
- License: Bitstream Vera license + Arev license + public-domain DejaVu
  changes; the full verbatim text is in `LICENSE-DejaVuSans.txt` (also
  embedded in the font's `name` table, from which this copy was extracted).
- This subset was produced with fontTools (`python3 -m fontTools.subset
  /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf --unicodes=…`), which
  preserves the upstream license and naming tables.
