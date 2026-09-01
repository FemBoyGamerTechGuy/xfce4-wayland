# Bundled build-time font asset

`DejaVuSans-ascii.ttf` is a **subset of DejaVu Sans 2.37** restricted to the
printable ASCII range U+0020..U+007E (the exact range `tools/genfont.py`
rasterizes into the client bitmap font).

Why it exists: the build must not depend on any system font file. Distributions
place fonts at different paths with different names (`/usr/share/fonts/truetype/…`
on Debian/Ubuntu, `/usr/share/fonts/TTF/…` on Arch/Artix, `/usr/share/fonts/…`
elsewhere), so "find a system TTF" is a distro-specific assumption. Bundling a
43932-byte subset makes the build deterministic and distro-agnostic; no font
package is a build requirement.

Provenance and license:

- Upstream: DejaVu fonts, https://dejavu-fonts.github.io/ — version 2.37
  (the version string is preserved inside the file).
- License: Bitstream Vera license + Arev license + public-domain DejaVu
  changes; the full verbatim text is in `LICENSE-DejaVuSans.txt` (also
  embedded in the font's `name` table, from which this copy was extracted).
- Modification from upstream: glyph subsetting only (pyftsubset, ASCII range,
  name table and layout features preserved). The rendering is bit-identical
  to the full font for every glyph genfont.py consumes (metrics, advances
  and anti-aliased pixels were compared programmatically when the subset was
  created — see `scripts/verify-font-subset.py`).
- Redistribution: permitted by the license as long as this license text
  accompanies the font. The name "DejaVu Sans" is retained (it contains
  neither "Bitstream" nor "Vera", satisfying the rename condition for
  modified derivatives).

See `THIRD-PARTY-LICENSES.md` (section 2) for the repository-wide provenance
record.
