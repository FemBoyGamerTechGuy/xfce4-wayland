# Third-party licenses and provenance audit

This repository contains original code under the proprietary
[LICENSE](LICENSE). That license covers **only** original work by this
project. Everything obtained from third parties keeps its own license and
attribution. Nothing here relicenses third-party code.

## 1. Vendored protocol definitions (protocols/)

| File | Origin | Upstream license | Derivative? |
|------|--------|------------------|-------------|
| `protocols/wlr-layer-shell-unstable-v1.xml` | https://gitlab.freedesktop.org/wlroots/wlr-protocols (fetched from the swaywm GitHub mirror; see `git log` for the commit) | MIT | No — protocol description only. All implementation code (both compositor and client side) is original. |
| `protocols/wlr-foreign-toplevel-management-unstable-v1.xml` | same | MIT | No — same reasoning. |
| `protocols/wlr-output-management-unstable-v1.xml` | same | MIT | No — same reasoning. Reserved for the display-settings work (see ROADMAP); not yet wired into the build. |

These XML files are machine-readable interface descriptions used as input
to `wayland-scanner`. Vendoring them does **not** incorporate wlroots code,
does not link against wlroots, and does not make this project a derivative
work of wlroots. The MIT copyright and permission notices inside each XML
file are preserved verbatim.

## 2. Build-time and linked libraries

| Component | Package | Upstream license | How used | Proprietary-compatible? |
|-----------|---------|------------------|-----------|--------------------------|
| Wayland protocol library | `libwayland-server`, `libwayland-client`, `libwayland-cursor` (1.23.1, Debian trixie) | MIT | Dynamically linked | Yes — MIT is permissive |
| XKB library | `libxkbcommon` (1.7.0) | MIT/X11 | Dynamically linked | Yes |
| Software raster library | `pixman` (0.44) | MIT | Dynamically linked | Yes |
| C runtime | glibc | LGPL-2.1+ | Dynamically linked | Yes — LGPL dynamic-linking exception |
| Protocol definitions | `wayland-protocols` (1.44) | MIT | Processed at build time by wayland-scanner into C glue | Yes |
| Font rasterization (build only) | Pillow (11.x), Python 3 | HPND (historical permission notice, BSD-like) / PSF | Build-time tool | Yes — build tools do not affect shipped code licensing |
| Glyph source (build only) | DejaVu Sans TTF | DejaVu license (free, permission-based, with attribution requirement) | Rasterized to bitmap glyph data at build time; the font file is **not** embedded or redistributed; the bitmap table is generated locally per build | Yes — DejaVu license permits use/modification; we keep attribution here |

**Attribution for DejaVu Sans** (as required by its license when derived
works are distributed):

    DejaVu fonts v2.37
    Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.
    Bitstream Vera is a trademark of Bitstream, Inc.
    DejaVu changes: Copyright (c) 2006-2023 DejaVu Project contributors
    (https://github.com/dejavu-fonts/dejavu-fonts). Permission is hereby
    granted, free of charge, to copy the fonts and use, modify, sell
    and/or redistribute them. See the full license text at
    https://github.com/dejavu-fonts/dejavu-fonts/blob/master/LICENSE.

If the build falls back to another font file (Noto Sans SC: SIL OFL 1.1;
Tinos: Apache 2.0), the generated file records which font was used, and the
corresponding notice applies; none of those licenses restrict embedding of
rendered glyph bitmaps.

## 3. Code copied or adapted from XFCE

**None.** As of this document's revision, this repository contains no code
copied, adapted, or derived from any XFCE component, and no XFCE
repositories are vendored in `subprojects/`. Behavioral compatibility with
XFCE was achieved by studying documented behavior and re-implementing it.
If a future milestone vendors or adapts an XFCE component into
`subprojects/`, that component's GPLv2 license stays fully intact and the
`subprojects/` boundary keeps it separated from proprietary original code;
the provenance table above will be extended first (before code lands).

## 4. Copyleft review for new code

New original code links only MIT/MIT-X11-licensed libraries dynamically,
plus LGPL glibc — the standard dynamic-linking exception applies. No GPL,
LGPL (beyond libc), or AGPL library is linked into any shipped binary.
`subprojects/` is the quarantine area for any future copyleft component.
