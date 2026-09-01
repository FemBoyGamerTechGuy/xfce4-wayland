#!/usr/bin/env python3
"""Byte-precise Makefile patch: bundled-font build wiring.

1. Dependency validation gains three immediate checks: python3 present,
   Pillow importable, bundled font asset present (all with actionable
   messages; skipped for clean/dist/config like the other checks).
2. The xw-font-data.h rule depends on the actual font source and honors
   XW_FONT (packager override; default = bundled asset).
3. `make config` reports the resolved font source.
4. The knobs header comment lists XW_FONT.

Idempotent: re-running detects already-applied patches.
"""

import sys

MK = "Makefile"

def main() -> int:
    with open(MK, "rb") as f:
        data = f.read()

    changed = False

    # ---- patch 1: dependency validation (python / Pillow / font asset) ----
    anchor = b"endif # XW_SKIP_DEPS\n"
    val_block = (
        b"ifeq ($(shell command -v $(PYTHON) >/dev/null 2>&1 && echo y),)\n"
        b"$(error $(PYTHON) was not found. It runs the build-time generators (font rasterization, Makefile feature wiring). Install python3, or pass PYTHON=/path/to/python3.)\n"
        b"endif\n"
        b"ifeq ($(shell $(PYTHON) -c 'import PIL' 2>/dev/null && echo y),)\n"
        b"$(error the Pillow python module is required at build time to rasterize the bundled font into the client bitmap font. Install your distribution's package (python3-pil on Debian/Ubuntu, python-pillow on Arch/Artix, python3-pillow on Fedora/openSUSE; see BUILDING.md) or run: $(PYTHON) -m pip install --user pillow)\n"
        b"endif\n"
        b"ifeq ($(wildcard assets/fonts/DejaVuSans-ascii.ttf)$(if $(XW_FONT),x,),)\n"
        b"$(error the bundled font asset assets/fonts/DejaVuSans-ascii.ttf is missing. It is the default build-time font source (no system font is needed or searched). A missing asset means a damaged checkout; restore it with: git checkout -- assets/fonts)\n"
        b"endif\n"
    )
    # only require the bundled asset when the packager did not override XW_FONT
    val_block = val_block.replace(
        b"ifeq ($(wildcard assets/fonts/DejaVuSans-ascii.ttf)$(if $(XW_FONT),x,),)",
        b"ifneq ($(XW_FONT),)\nifeq ($(wildcard $(XW_FONT)),)\n$(error XW_FONT is set to '$(XW_FONT)' but that file does not exist.)\nendif\nelse\nifeq ($(wildcard assets/fonts/DejaVuSans-ascii.ttf),)")
    val_block = val_block.replace(
        b"$(error the bundled font asset assets/fonts/DejaVuSans-ascii.ttf is missing. It is the default build-time font source (no system font is needed or searched). A missing asset means a damaged checkout; restore it with: git checkout -- assets/fonts)\nendif\n",
        b"$(error the bundled font asset assets/fonts/DejaVuSans-ascii.ttf is missing. It is the default build-time font source (no system font is needed or searched). A missing asset means a damaged checkout; restore it with: git checkout -- assets/fonts)\nendif\nendif\n")

    if val_block not in data:
        if anchor not in data:
            print("patch1: anchor not found", file=sys.stderr)
            return 1
        data = data.replace(anchor, val_block + anchor, 1)
        changed = True
        print("patch1: dependency validation (python/Pillow/font asset) applied")
    else:
        print("patch1: already applied")

    # ---- patch 2: font rule with real dependency + XW_FONT override ----
    tab = b"\t"
    old_rule = (
        b"# Font bitmap generation (build-time; runtime has no font dependency)\n"
        b"GEN_HEADERS += $(GEN)/xw-font-data.h\n"
        b"\n"
        b"$(GEN)/xw-font-data.h: tools/genfont.py | $(GEN)\n"
        + tab + b"$(PYTHON) tools/genfont.py -o $@\n"
    )
    new_rule = (
        b"# Font bitmap generation (build-time; runtime has no font dependency).\n"
        b"# Default source: the font bundled in assets/fonts/ -- distro-agnostic\n"
        b"# and deterministic (no system font is searched or required; see\n"
        b"# assets/fonts/README.md). XW_FONT=PATH overrides the source file for\n"
        b"# packagers who want to rasterize their own font (needs Pillow).\n"
        b"XW_FONT ?=\n"
        b"XW_FONT_DEP := $(if $(XW_FONT),$(XW_FONT),assets/fonts/DejaVuSans-ascii.ttf)\n"
        b"GEN_HEADERS += $(GEN)/xw-font-data.h\n"
        b"\n"
        b"$(GEN)/xw-font-data.h: tools/genfont.py $(XW_FONT_DEP) | $(GEN)\n"
        + tab + b"$(PYTHON) tools/genfont.py -o $@ $(if $(XW_FONT),--font $(XW_FONT))\n"
    )
    if new_rule not in data:
        if old_rule not in data:
            print("patch2: font rule anchor not found", file=sys.stderr)
            return 1
        data = data.replace(old_rule, new_rule, 1)
        changed = True
        print("patch2: font rule (real dependency + XW_FONT) applied")
    else:
        print("patch2: already applied")

    # ---- patch 3: config summary reports the font source ----
    cfg_anchor = b'\t@echo "  install prefix $(prefix)"\n'
    cfg_line = tab + b'@echo "  font source    $(if $(XW_FONT),$(XW_FONT) (XW_FONT override),bundled asset assets/fonts/DejaVuSans-ascii.ttf)"\n'
    if cfg_line not in data:
        if cfg_anchor not in data:
            print("patch3: config anchor not found", file=sys.stderr)
            return 1
        data = data.replace(cfg_anchor, cfg_line + cfg_anchor, 1)
        changed = True
        print("patch3: config summary font line applied")
    else:
        print("patch3: already applied")

    # ---- patch 4: knobs doc comment ----
    knob_old = b"#   CC/AR/CFLAGS/LDFLAGS   standard toolchain overrides\n"
    knob_new = (b"#   CC/AR/CFLAGS/LDFLAGS   standard toolchain overrides\n"
                b"#   XW_FONT=PATH           override the build-time font source\n"
                b"#                          (default: bundled assets/fonts asset)\n")
    if knob_new not in data:
        if knob_old not in data:
            print("patch4: knobs anchor not found", file=sys.stderr)
            return 1
        data = data.replace(knob_old, knob_new, 1)
        changed = True
        print("patch4: knobs comment applied")
    else:
        print("patch4: already applied")

    if changed:
        with open(MK, "wb") as f:
            f.write(data)
        print("Makefile patched")
    else:
        print("Makefile unchanged (all patches already present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
