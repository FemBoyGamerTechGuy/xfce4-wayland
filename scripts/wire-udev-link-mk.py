#!/usr/bin/env python3
"""Byte-precise Makefile patch: libudev as an explicit direct dependency
of the libinput real-input backend (fixes the Artix link failure):

  /usr/bin/ld: build/lib/libxw.a(xw-input-libinput.o): undefined
  reference to symbol 'udev_new@@LIBUDEV_183'
  /usr/bin/ld: /usr/lib/libudev.so.1: error adding symbols: DSO missing
  from command line

Root cause: xw-input-libinput.c calls udev_new()/udev_unref() directly
(the udev seat context is created by us, not by libinput). The final
link line carried only `pkg-config --libs libinput` = -linput; upstream
libinput.pc does not hand out -ludev, and modern ld defaults to
--no-copy-dt-needed-entries, so the indirect DT_NEEDED libudev.so.1
cannot satisfy the reference.

Patches:
  1. libinput feature detection requires BOTH pkg-config modules
     (libinput, libudev); XW_LIBINPUT=1 gives a precise error for each
     missing one, auto degrades with a notice naming the missing one,
     0 stays off.
  2. CFLAGS_LIBUDEV/LDLIBS_LIBUDEV are defined alongside the libinput
     flags.
  3. The xw-input-libinput.o compile rule adds $(CFLAGS_LIBUDEV)
     (<libudev.h> is included directly) and its comment stops claiming
     the udev context "comes through libinput".
  4. The two final links that consume libxw.a (xw-compositor,
     run-tests) gain $(LDLIBS_LIBUDEV) directly after
     $(LDLIBS_LIBINPUT) — after the archive that references the
     symbols, before xkbcommon/pixman (order-safe for shared and
     static libinput alike).
  5. `make config` reports both module versions.
  6. New resolved-feature stamp guard (build/.features): switching
     XW_X11/XW_LIBINPUT between different RESOLVED states over a
     populated tree now fails loudly (stale archive members are the
     same "DSO missing" class) instead of silently mixing objects;
     auto<->1 with the same resolved state never forces a clean.

Idempotent: re-running detects already-applied patches.
"""

import sys

MK = "Makefile"
T = "\t"


def enc(s: str) -> bytes:
    return s.encode("utf-8")


def main() -> int:
    with open(MK, "rb") as f:
        data = f.read()

    changed = False

    def apply(name: str, old: bytes, new: bytes) -> None:
        nonlocal changed, data
        if new in data:
            print(f"{name}: already applied")
            return
        if old not in data:
            print(f"{name}: anchor not found", file=sys.stderr)
            sys.exit(1)
        if data.count(old) != 1:
            print(f"{name}: anchor is not unique", file=sys.stderr)
            sys.exit(1)
        data = data.replace(old, new, 1)
        changed = True
        print(f"{name}: applied")

    # ---- patch 1: feature detection (libinput AND libudev) ----
    old_detect = enc(
        "# libinput real-input backend — XW_LIBINPUT = auto|1|0.\n"
        "LIBINPUT_FOUND := $(shell pkg-config --exists libinput 2>/dev/null && echo y)\n"
        "ifeq ($(XW_LIBINPUT),1)\n"
        "  ifneq ($(LIBINPUT_FOUND),y)\n"
        "    $(error libinput backend requested (XW_LIBINPUT=1) but the libinput development files were not found. libinput drives real keyboards/mice for the native session; the core compositor, nested backends and all tests build and run without it. Install your distribution's libinput development package (BUILDING.md), or build with XW_LIBINPUT=auto/0.)\n"
        "  endif\n"
        "  LIBINPUT_ON := y\n"
        "else ifeq ($(XW_LIBINPUT),0)\n"
        "  LIBINPUT_ON :=\n"
        "else\n"
        "  LIBINPUT_ON := $(LIBINPUT_FOUND)\n"
        "  ifneq ($(LIBINPUT_FOUND),y)\n"
        "    $(info libinput: development files not found — the real-input backend will not be built (headless and nested input are unaffected).)\n"
        "    $(info libinput: install the libinput development package (BUILDING.md) to enable it, or set XW_LIBINPUT=0 to silence this note.)\n"
        "  endif\n"
        "endif\n"
    )
    new_detect = enc(
        "# libinput real-input backend — XW_LIBINPUT = auto|1|0.\n"
        "# The backend calls libudev DIRECTLY (udev_new creates the seat\n"
        "# context handed to libinput_udev_create_context), so the feature\n"
        "# needs TWO development sets: libinput and libudev. The udev_*\n"
        "# symbols are referenced by xw-input-libinput.o inside libxw.a and\n"
        "# must appear on the final link line explicitly: upstream\n"
        "# libinput.pc does not hand out -ludev (pkg-config --libs libinput\n"
        "# is just -linput; libudev only rides along as libinput's\n"
        "# DT_NEEDED), and ld's default --no-copy-dt-needed-entries refuses\n"
        "# to resolve symbols from indirect DSOs — the exact\n"
        "# \"DSO missing from command line\" failure seen on Arch/Artix.\n"
        "# probed with --libs, not --exists: pkgconf's --exists resolves\n"
        "# Requires.private, and upstream libinput.pc declares Requires.private:\n"
        "# libudev -- with only the libudev dev files missing, --exists would\n"
        "# report LIBINPUT itself as missing and misdirect the diagnostic.\n"
        "# --libs parses the file and its public Requires exactly like the\n"
        "# link rule will consume it.\n"
        "LIBINPUT_FOUND := $(shell pkg-config --libs libinput >/dev/null 2>&1 && echo y)\n"
        "LIBUDEV_FOUND  := $(shell pkg-config --exists libudev 2>/dev/null && echo y)\n"
        "LIBINPUT_VER   := $(shell pkg-config --modversion libinput 2>/dev/null)\n"
        "LIBUDEV_VER    := $(shell pkg-config --modversion libudev 2>/dev/null)\n"
        "ifeq ($(XW_LIBINPUT),1)\n"
        "  ifneq ($(LIBINPUT_FOUND),y)\n"
        "    $(error libinput backend requested (XW_LIBINPUT=1) but the libinput development files were not found. libinput drives real keyboards/mice for the native session; the core compositor, nested backends and all tests build and run without it. Install your distribution's libinput development package (BUILDING.md), or build with XW_LIBINPUT=auto/0.)\n"
        "  endif\n"
        "  ifneq ($(LIBUDEV_FOUND),y)\n"
        "    $(error libinput backend requested (XW_LIBINPUT=1) but the libudev development files were not found (pkg-config module 'libudev'). The backend creates the udev context itself for device discovery (udev seat mode), which makes libudev a direct link dependency of libxw.a — not something libinput provides transitively at link time. Install your distribution's libudev development package (BUILDING.md), or build with XW_LIBINPUT=auto/0.)\n"
        "  endif\n"
        "  LIBINPUT_ON := y\n"
        "else ifeq ($(XW_LIBINPUT),0)\n"
        "  LIBINPUT_ON :=\n"
        "else\n"
        "  # auto: enabled only when BOTH direct dependencies resolve\n"
        "  ifeq ($(LIBINPUT_FOUND)$(LIBUDEV_FOUND),yy)\n"
        "    LIBINPUT_ON := y\n"
        "  else\n"
        "    LIBINPUT_ON :=\n"
        "    ifneq ($(LIBINPUT_FOUND),y)\n"
        "      $(info libinput: development files not found — the real-input backend will not be built (headless and nested input are unaffected).)\n"
        "    else\n"
        "      $(info libinput: the libudev development files (pkg-config module 'libudev') were not found — the real-input backend will not be built: it creates its own udev context for device discovery, which makes libudev a direct link dependency rather than something libinput provides transitively.)\n"
        "    endif\n"
        "    $(info libinput: install the missing development package(s) (BUILDING.md) to enable the backend, or set XW_LIBINPUT=0 to silence this note.)\n"
        "  endif\n"
        "endif\n"
    )
    apply("patch1: libinput+libudev detection", old_detect, new_detect)

    # ---- patch 2: flags block gains libudev cflags/libs ----
    old_flags = enc(
        "ifeq ($(LIBINPUT_ON),y)\n"
        "CFLAGS_LIBINPUT := $(shell pkg-config --cflags libinput 2>/dev/null)\n"
        "LDLIBS_LIBINPUT := $(shell pkg-config --libs libinput 2>/dev/null)\n"
        "HAVE_LIBINPUT   := -DXW_HAVE_LIBINPUT\n"
        "else\n"
        "CFLAGS_LIBINPUT :=\n"
        "LDLIBS_LIBINPUT :=\n"
        "HAVE_LIBINPUT   :=\n"
        "endif\n"
    )
    new_flags = enc(
        "ifeq ($(LIBINPUT_ON),y)\n"
        "CFLAGS_LIBINPUT := $(shell pkg-config --cflags libinput 2>/dev/null)\n"
        "LDLIBS_LIBINPUT := $(shell pkg-config --libs libinput 2>/dev/null)\n"
        "CFLAGS_LIBUDEV  := $(shell pkg-config --cflags libudev 2>/dev/null)\n"
        "LDLIBS_LIBUDEV  := $(shell pkg-config --libs libudev 2>/dev/null)\n"
        "HAVE_LIBINPUT   := -DXW_HAVE_LIBINPUT\n"
        "else\n"
        "CFLAGS_LIBINPUT :=\n"
        "LDLIBS_LIBINPUT :=\n"
        "CFLAGS_LIBUDEV  :=\n"
        "LDLIBS_LIBUDEV  :=\n"
        "HAVE_LIBINPUT   :=\n"
        "endif\n"
    )
    apply("patch2: libudev flags", old_flags, new_flags)

    # ---- patch 3: resolved-feature stamp guard ----
    anchor_protocols = enc("# ---------------------------------------------------------------- protocols\n")
    guard = enc(
        "\n"
        "# Resolved-feature stamp guard: like the PROFILE guard, this\n"
        "# refuses to mix objects built with a different set of optional\n"
        "# backends in the same tree. The stamp records the RESOLVED state\n"
        "# (x11/libinput on/off), so switching between auto and 1 with the\n"
        "# same outcome never forces a clean; switching a feature on/off\n"
        "# does. Without the guard the archive would silently keep stale\n"
        "# members (e.g. a udev-using xw-input-libinput.o left in libxw.a\n"
        "# while linking without -ludev — the same \"DSO missing from\n"
        "# command line\" failure class).\n"
        "ifeq ($(XW_PROFILE_GUARD),skip)\n"
        "XW_FEATURE_GUARD := skip\n"
        "endif\n"
        "XW_TREE_FEATURES := $(shell cat build/.features 2>/dev/null)\n"
        "XW_FEATURES_NOW  := x11=$(if $(X11_ON),y,n) libinput=$(if $(LIBINPUT_ON),y,n)\n"
        "ifneq ($(XW_FEATURE_GUARD),skip)\n"
        "ifneq ($(XW_TREE_FEATURES),$(XW_FEATURES_NOW))\n"
        "ifneq ($(wildcard build/obj),)\n"
        "ifeq ($(XW_TREE_FEATURES),)\n"
        "$(error build tree holds objects but no build/.features stamp (built before feature tracking existed, or by a file-target-only build). Run `make clean` once so the tree records its feature set.)\n"
        "else\n"
        "$(error build tree holds objects for features '$(XW_TREE_FEATURES)' but the current configuration resolves to '$(XW_FEATURES_NOW)'. Switching XW_X11/XW_LIBINPUT across a resolved-state change needs a clean tree: run `make clean` first.)\n"
        "endif\n"
        "endif\n"
        "endif\n"
        "endif\n"
        "\n"
    )
    if guard in data:
        print("patch3: feature guard already applied")
    else:
        if anchor_protocols not in data:
            print("patch3: anchor not found", file=sys.stderr)
            sys.exit(1)
        data = data.replace(anchor_protocols, guard + anchor_protocols, 1)
        changed = True
        print("patch3: feature guard applied")

    # ---- patch 4: xw-input-libinput.o compile rule ----
    old_rule = enc(
        "# libinput backend: needs libinput headers (udev context comes\n"
        "# through libinput itself)\n"
        "$(OBJ)/libxw/xw-input-libinput.o: src/libxw/xw-input-libinput.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
        + T + "$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_LIBINPUT) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_LIBINPUT) -c $< -o $@\n"
    )
    new_rule = enc(
        "# libinput backend: needs libinput AND libudev headers — the\n"
        "# backend creates the udev context itself (udev seat mode), so\n"
        "# <libudev.h> is included directly and the udev_* symbols it\n"
        "# pulls in must be linked explicitly (see the feature block\n"
        "# above for why libinput.pc does not provide them).\n"
        "$(OBJ)/libxw/xw-input-libinput.o: src/libxw/xw-input-libinput.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
        + T + "$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_LIBINPUT) $(CFLAGS_LIBUDEV) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_LIBINPUT) -c $< -o $@\n"
    )
    apply("patch4: libinput compile rule", old_rule, new_rule)

    # ---- patch 5: xw-compositor link line ----
    old_link = enc(
        T + "$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n"
    )
    new_link = enc(
        T + "$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_LIBUDEV) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n"
    )
    apply("patch5: xw-compositor link", old_link, new_link)

    # ---- patch 6: run-tests link line ----
    old_tlink = enc(
        T + "$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_M)\n"
    )
    new_tlink = enc(
        T + "$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_LIBUDEV) $(LDLIBS_M)\n"
    )
    apply("patch6: run-tests link", old_tlink, new_tlink)

    # ---- patch 7: config summary line ----
    old_cfg = enc(
        T + '@echo "  libinput       $(if $(LIBINPUT_ON),yes ($(shell pkg-config --modversion libinput 2>/dev/null)),no)"\n'
    )
    new_cfg = enc(
        T + '@echo "  libinput       $(if $(LIBINPUT_ON),yes (libinput $(LIBINPUT_VER) + libudev $(LIBUDEV_VER)),no)"\n'
    )
    apply("patch7: config summary", old_cfg, new_cfg)

    # ---- patch 8: build/.features stamp rule + `all` prerequisite ----
    old_stamp = enc(
        "# profile stamp consulted by the PROFILE guard near the top\n"
        "build/.profile:\n"
        + T + "@mkdir -p $(@D)\n"
        + T + "@printf '%s\\n' '$(PROFILE)' > $@\n"
    )
    new_stamp = enc(
        "# profile stamp consulted by the PROFILE guard near the top\n"
        "build/.profile:\n"
        + T + "@mkdir -p $(@D)\n"
        + T + "@printf '%s\\n' '$(PROFILE)' > $@\n"
        "\n"
        "# resolved-feature stamp consulted by the feature guard above\n"
        "build/.features:\n"
        + T + "@mkdir -p $(@D)\n"
        + T + "@printf 'x11=%s libinput=%s\\n' '$(if $(X11_ON),y,n)' '$(if $(LIBINPUT_ON),y,n)' > $@\n"
    )
    apply("patch8: features stamp rule", old_stamp, new_stamp)

    old_all = enc(
        "all: build/.profile build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS) \\\n"
        + T + "build/tests/run-tests\n"
    )
    new_all = enc(
        "all: build/.profile build/.features build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS) \\\n"
        + T + "build/tests/run-tests\n"
    )
    apply("patch9: all depends on stamp", old_all, new_all)

    if changed:
        with open(MK, "wb") as f:
            f.write(data)
        print("Makefile patched")
    else:
        print("Makefile unchanged (all patches already present)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
