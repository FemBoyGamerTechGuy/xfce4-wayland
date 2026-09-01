#!/usr/bin/env python3
"""Wire feature toggles, dependency validation, build profiles and
installation targets into the Makefile.

Byte-precise (tab-safe) edits:
  1. header comment documents targets and knobs
  2. XW_X11 / XW_LIBINPUT feature toggles (auto | 1 | 0)
  3. PROFILE presets (release | debug | asan) + profile-switch guard
     (fails loudly instead of silently mixing sanitized/plain objects)
  4. required-dependency validation with actionable $(error) messages;
     skipped for goals that compile nothing (clean/dist)
  5. optional-dep probes for X11 and libinput with directive-grade
     "what you lose / how to enable / how to silence" notes
  6. libinput object rule + link flags on xw-compositor and run-tests
  7. install / uninstall / config targets (prefix, DESTDIR, docdir,
     wayland-sessions desktop file, example configs)
  8. dist tarball: reproducible sort order
"""
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    d = f.read()

def sub(old, new, count=1):
    global d
    assert d.count(old) == count, (
        f"pattern not found (count={d.count(old)}, want {count}): {old[:80]!r}")
    d = d.replace(old, new)

TAB = b"\t"

# ------------------------------------------------ 1: header comment
sub(
    b"# Targets:\n"
    b"#   all      \xe2\x80\x94 build libraries, binaries\n"
    b"#   tests    \xe2\x80\x94 build and run the automated test suite\n"
    b"#   clean    \xe2\x80\x94 remove build/\n"
    b"#   dist     \xe2\x80\x94 create release archive in dist/\n",
    b"# Targets:\n"
    b"#   all       \xe2\x80\x94 build libraries, binaries\n"
    b"#   tests     \xe2\x80\x94 build and run the automated test suite\n"
    b"#   check     \xe2\x80\x94 tests + process-level session checks\n"
    b"#   asan      \xe2\x80\x94 sanitizer regression pass\n"
    b"#   config    \xe2\x80\x94 print the build configuration summary\n"
    b"#   install   \xe2\x80\x94 install to $(prefix) (honors DESTDIR)\n"
    b"#   uninstall \xe2\x80\x94 remove installed files\n"
    b"#   clean     \xe2\x80\x94 remove build/\n"
    b"#   dist      \xe2\x80\x94 create release archive in dist/\n"
    b"#\n"
    b"# Knobs (make VAR=value, or config.local.mk):\n"
    b"#   XW_X11=auto|1|0        nested X11 backend (libX11)\n"
    b"#   XW_LIBINPUT=auto|1|0   real-input backend (libinput)\n"
    b"#   PROFILE=release|debug|asan   build profile (switching needs\n"
    b"#                          `make clean`; the guard enforces it)\n"
    b"#   prefix=DIR             installation prefix ($HOME/.local works)\n"
    b"#   CC/AR/CFLAGS/LDFLAGS   standard toolchain overrides\n",
)

# ------------------------------------------------ 2: options block
sub(
    b"CC       ?= cc\n"
    b"AR       ?= ar\n"
    b"PYTHON   ?= python3\n",
    b"CC       ?= cc\n"
    b"AR       ?= ar\n"
    b"PYTHON   ?= python3\n"
    b"\n"
    b"# Feature toggles: auto (default: enable when the dependency is\n"
    b"# found, degrade with a notice otherwise) | 1 (require, hard error)\n"
    b"# | 0 (never build). See BUILDING.md.\n"
    b"XW_X11      ?= auto\n"
    b"XW_LIBINPUT ?= auto\n"
    b"\n"
    b"# Goals of this invocation (used by guard/check skip logic).\n"
    b"XW_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)\n",
)

# ------------------------------------------------ 3: profile block
sub(
    b"CSTD     := -std=c11\n"
    b"CFLAGS   ?= -O2 -g\n"
    b"LDFLAGS  ?=\n",
    b"CSTD     := -std=c11\n"
    b"\n"
    b"# Build profiles: release (default) | debug | asan. Switching the\n"
    b"# profile requires `make clean`; the guard below fails loudly\n"
    b"# instead of silently mixing plain and sanitized objects.\n"
    b"PROFILE  ?= release\n"
    b"ifeq ($(PROFILE),release)\n"
    b"PROFILE_CFLAGS  := -O2 -g\n"
    b"PROFILE_LDFLAGS :=\n"
    b"else ifeq ($(PROFILE),debug)\n"
    b"PROFILE_CFLAGS  := -O0 -g3 -DXW_DEBUG\n"
    b"PROFILE_LDFLAGS :=\n"
    b"else ifeq ($(PROFILE),asan)\n"
    b"PROFILE_CFLAGS  := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer\n"
    b"PROFILE_LDFLAGS := -fsanitize=address,undefined\n"
    b"else\n"
    b"$(error unknown PROFILE '$(PROFILE)' (expected release, debug or asan))\n"
    b"endif\n"
    b"CFLAGS   ?= $(PROFILE_CFLAGS)\n"
    b"LDFLAGS  ?= $(PROFILE_LDFLAGS)\n"
    b"\n"
    b"# Profile-switch guard: the build tree records its profile in\n"
    b"# build/.profile. Rebuilding with another PROFILE over a populated\n"
    b"# tree is a stale-object hazard (mixed sanitized/plain objects),\n"
    b"# so refuse and point at `make clean`. `clean` in the goals always\n"
    b"# resets the tree, so the guard steps aside for it.\n"
    b"ifneq ($(filter clean,$(XW_GOALS)),)\n"
    b"XW_PROFILE_GUARD := skip\n"
    b"endif\n"
    b"XW_TREE_PROFILE := $(shell cat build/.profile 2>/dev/null)\n"
    b"ifneq ($(XW_PROFILE_GUARD),skip)\n"
    b"ifneq ($(XW_TREE_PROFILE),$(PROFILE))\n"
    b"ifneq ($(wildcard build/obj),)\n"
    b"$(error build tree holds objects from PROFILE '$(XW_TREE_PROFILE)'; run `make clean` before building PROFILE '$(PROFILE)')\n"
    b"endif\n"
    b"endif\n"
    b"endif\n",
)

# ---------------------------------------- 4+5: deps validation + probes
sub(
    b"# X11 nested backend: optional at build time (degrades gracefully\n"
    b"# to a runtime error when libX11 is absent)\n"
    b"X11_OK := $(shell pkg-config --exists x11 && echo yes)\n"
    b"ifeq ($(X11_OK),yes)\n"
    b"CFLAGS_X11 := $(shell pkg-config --cflags x11)\n"
    b"LDLIBS_X11 := $(shell pkg-config --libs x11)\n"
    b"HAVE_X11   := -DXW_HAVE_X11_BACKEND\n"
    b"else\n"
    b"CFLAGS_X11 :=\n"
    b"LDLIBS_X11 :=\n"
    b"HAVE_X11   :=\n"
    b"endif\n",
    b"# ------------------------------------------------ dependency validation\n"
    b"# Actionable failures instead of \"dependency not found\". The build\n"
    b"# system never invokes a package manager; the messages tell the\n"
    b"# user exactly what is missing and where to read more. Checks are\n"
    b"# skipped for goals that compile nothing (clean, dist, config).\n"
    b"ifeq ($(filter-out clean dist config,$(XW_GOALS)),)\n"
    b"XW_SKIP_DEPS := 1\n"
    b"endif\n"
    b"\n"
    b"ifeq ($(XW_SKIP_DEPS),)\n"
    b"ifeq ($(shell command -v pkg-config >/dev/null 2>&1 && echo y),)\n"
    b"$(error pkg-config is required to locate the wayland/xkbcommon/pixman development files but was not found. Install pkg-config first (see BUILDING.md, \"Installing dependencies by distribution\").)\n"
    b"endif\n"
    b"XW_PC_MISSING :=\n"
    b"ifeq ($(shell pkg-config --exists wayland-server 2>/dev/null && echo y),)\n"
    b"XW_PC_MISSING += wayland-server\n"
    b"endif\n"
    b"ifeq ($(shell pkg-config --exists wayland-client 2>/dev/null && echo y),)\n"
    b"XW_PC_MISSING += wayland-client\n"
    b"endif\n"
    b"ifeq ($(shell pkg-config --exists xkbcommon 2>/dev/null && echo y),)\n"
    b"XW_PC_MISSING += xkbcommon\n"
    b"endif\n"
    b"ifeq ($(shell pkg-config --exists pixman-1 2>/dev/null && echo y),)\n"
    b"XW_PC_MISSING += pixman-1\n"
    b"endif\n"
    b"ifneq ($(XW_PC_MISSING),)\n"
    b"$(error required dependency check failed: $(XW_PC_MISSING). These are the core compositor libraries; nothing can be built without them. Install your distribution's development packages (BUILDING.md, \"Installing dependencies by distribution\"), or point PKG_CONFIG_PATH at an unpacked sysroot (BUILDING.md, \"Sysroot bootstrap for containers\").)\n"
    b"endif\n"
    b"ifeq ($(WAYLAND_SCANNER),)\n"
    b"$(error wayland-scanner was not found. It generates the protocol glue from the XML definitions and is required to build. It ships with the wayland development package of every distribution; with a local sysroot, ensure its usr/bin is in PATH (see BUILDING.md).)\n"
    b"endif\n"
    b"ifeq ($(wildcard $(WP_DIR)),)\n"
    b"$(error the wayland-protocols data directory was not found (looked for $(WP_DIR)). The xdg-shell/activation/ext-workspace XMLs are required at build time. Install the wayland-protocols package of your distribution.)\n"
    b"endif\n"
    b"endif # XW_SKIP_DEPS\n"
    b"\n"
    b"# ------------------------------------------------ optional components\n"
    b"# X11 nested backend (libX11) \xe2\x80\x94 XW_X11 = auto|1|0.\n"
    b"X11_FOUND := $(shell pkg-config --exists x11 2>/dev/null && echo y)\n"
    b"ifeq ($(XW_X11),1)\n"
    b"  ifneq ($(X11_FOUND),y)\n"
    b"    $(error X11 nested backend requested (XW_X11=1) but the libX11 development files were not found. Install your distribution's X11 development package (BUILDING.md), or build with XW_X11=auto/0.)\n"
    b"  endif\n"
    b"  X11_ON := y\n"
    b"else ifeq ($(XW_X11),0)\n"
    b"  X11_ON :=\n"
    b"else\n"
    b"  X11_ON := $(X11_FOUND)\n"
    b"  ifneq ($(X11_FOUND),y)\n"
    b"    $(info x11: libX11 development files not found \xe2\x80\x94 the nested X11 backend will not be built.)\n"
    b"    $(info x11: install the libX11 development package (BUILDING.md) to enable it, or set XW_X11=0 to silence this note.)\n"
    b"  endif\n"
    b"endif\n"
    b"\n"
    b"ifeq ($(X11_ON),y)\n"
    b"CFLAGS_X11 := $(shell pkg-config --cflags x11)\n"
    b"LDLIBS_X11 := $(shell pkg-config --libs x11)\n"
    b"HAVE_X11   := -DXW_HAVE_X11_BACKEND\n"
    b"else\n"
    b"CFLAGS_X11 :=\n"
    b"LDLIBS_X11 :=\n"
    b"HAVE_X11   :=\n"
    b"endif\n"
    b"\n"
    b"# libinput real-input backend \xe2\x80\x94 XW_LIBINPUT = auto|1|0.\n"
    b"LIBINPUT_FOUND := $(shell pkg-config --exists libinput 2>/dev/null && echo y)\n"
    b"ifeq ($(XW_LIBINPUT),1)\n"
    b"  ifneq ($(LIBINPUT_FOUND),y)\n"
    b"    $(error libinput backend requested (XW_LIBINPUT=1) but the libinput development files were not found. libinput drives real keyboards/mice for the native session; the core compositor, nested backends and all tests build and run without it. Install your distribution's libinput development package (BUILDING.md), or build with XW_LIBINPUT=auto/0.)\n"
    b"  endif\n"
    b"  LIBINPUT_ON := y\n"
    b"else ifeq ($(XW_LIBINPUT),0)\n"
    b"  LIBINPUT_ON :=\n"
    b"else\n"
    b"  LIBINPUT_ON := $(LIBINPUT_FOUND)\n"
    b"  ifneq ($(LIBINPUT_FOUND),y)\n"
    b"    $(info libinput: development files not found \xe2\x80\x94 the real-input backend will not be built (headless and nested input are unaffected).)\n"
    b"    $(info libinput: install the libinput development package (BUILDING.md) to enable it, or set XW_LIBINPUT=0 to silence this note.)\n"
    b"  endif\n"
    b"endif\n"
    b"\n"
    b"ifeq ($(LIBINPUT_ON),y)\n"
    b"CFLAGS_LIBINPUT := $(shell pkg-config --cflags libinput)\n"
    b"LDLIBS_LIBINPUT := $(shell pkg-config --libs libinput)\n"
    b"HAVE_LIBINPUT   := -DXW_HAVE_LIBINPUT\n"
    b"else\n"
    b"CFLAGS_LIBINPUT :=\n"
    b"LDLIBS_LIBINPUT :=\n"
    b"HAVE_LIBINPUT   :=\n"
    b"endif\n",
)

# ---------------------------------------- 6: conditional source lists
sub(
    b"ifeq ($(X11_OK),yes)\n"
    b"LIBXW_SRC := $(wildcard src/libxw/*.c)\n"
    b"else\n"
    b"LIBXW_SRC := $(filter-out src/libxw/xw-backend-x11.c,$(wildcard src/libxw/*.c))\n"
    b"endif\n",
    b"XW_EXCLUDE_SRC :=\n"
    b"ifneq ($(X11_ON),y)\n"
    b"XW_EXCLUDE_SRC += src/libxw/xw-backend-x11.c\n"
    b"endif\n"
    b"ifneq ($(LIBINPUT_ON),y)\n"
    b"XW_EXCLUDE_SRC += src/libxw/xw-input-libinput.c\n"
    b"endif\n"
    b"LIBXW_SRC := $(filter-out $(XW_EXCLUDE_SRC),$(wildcard src/libxw/*.c))\n",
)

# object rules: HAVE_LIBINPUT next to HAVE_X11 (generic rule + x11 rule;
# the two recipe lines differ in CFLAGS_X11/CFLAGS_PIX order)
sub(
    b"$(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) -c $< -o $@\n",
    b"$(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) $(HAVE_LIBINPUT) -c $< -o $@\n",
)
sub(
    b"$(CFLAGS_WLS) $(CFLAGS_X11) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) -c $< -o $@\n",
    b"$(CFLAGS_WLS) $(CFLAGS_X11) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) $(HAVE_LIBINPUT) -c $< -o $@\n",
)

# libinput module rule after the x11 module rule
sub(
    b"$(OBJ)/libxw/xw-backend-x11.o: src/libxw/xw-backend-x11.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
    + TAB + b"$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_X11) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) $(HAVE_LIBINPUT) -c $< -o $@\n",
    b"$(OBJ)/libxw/xw-backend-x11.o: src/libxw/xw-backend-x11.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
    + TAB + b"$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_X11) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) $(HAVE_LIBINPUT) -c $< -o $@\n"
    b"\n"
    b"# libinput backend: needs libinput headers (udev context comes\n"
    b"# through libinput itself)\n"
    b"$(OBJ)/libxw/xw-input-libinput.o: src/libxw/xw-input-libinput.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
    + TAB + b"$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_LIBINPUT) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_LIBINPUT) -c $< -o $@\n",
)

# link lines gain LDLIBS_LIBINPUT (xw-compositor + run-tests)
sub(
    b"$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n",
    b"$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n",
)
sub(
    b"$(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_M)\n",
    b"$(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_M)\n",
)

# XTST probe condition rename
sub(
    b"ifeq ($(X11_OK)$(XW_SYSROOT),yes$(XW_SYSROOT))\n",
    b"ifeq ($(X11_ON)$(XW_SYSROOT),y$(XW_SYSROOT))\n",
)

# ---------------------------------------- 7: targets region + stamp rule
# (continuation lines inside CLIENT_BINS are TAB-indented in the file)
sub(
    b".PHONY: all tests check asan clean dist\n"
    b"\n"
    b"all: build/bin/xw-compositor build/tests/run-tests\n"
    b"# session manager, exit dialog, panel, demo client: added in later\n"
    b"# milestones (their rules below activate when sources appear)\n"
    b"SESSION_BINS := $(if $(wildcard src/session/xw-session.c),build/bin/xw-session build/bin/xw-session-ctl,)\n"
    b"CLIENT_BINS := $(if $(wildcard src/clients/xw-demo.c),build/bin/xw-demo,) \\\n"
    + TAB + b"$(if $(wildcard src/clients/xw-exit.c),build/bin/xw-exit,) \\\n"
    + TAB + b"$(if $(wildcard src/clients/xw-panel.c),build/bin/xw-panel,)\n"
    b"all: build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS) build/tests/run-tests\n"
    b"ifeq ($(X11_OK),yes)\n"
    b"all: build/tests/x11probe\n"
    b"endif\n",
    b".PHONY: all tests check asan clean dist install uninstall config\n"
    b"\n"
    b"# session manager, exit dialog, panel, demo client\n"
    b"SESSION_BINS := $(if $(wildcard src/session/xw-session.c),build/bin/xw-session build/bin/xw-session-ctl,)\n"
    b"CLIENT_BINS := $(if $(wildcard src/clients/xw-demo.c),build/bin/xw-demo,) \\\n"
    + TAB + b"$(if $(wildcard src/clients/xw-exit.c),build/bin/xw-exit,) \\\n"
    + TAB + b"$(if $(wildcard src/clients/xw-panel.c),build/bin/xw-panel,)\n"
    b"all: build/.profile build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS) \\\n"
    + TAB + b"build/tests/run-tests\n"
    b"ifeq ($(X11_ON),y)\n"
    b"all: build/tests/x11probe\n"
    b"endif\n"
    b"\n"
    b"# profile stamp consulted by the PROFILE guard near the top\n"
    b"build/.profile:\n"
    + TAB + b"@mkdir -p $(@D)\n"
    + TAB + b"@printf '%s\\n' '$(PROFILE)' > $@\n",
)

# ---------------------------------------- 8: install targets, dist tweak
sub(
    b"dist: all\n"
    + TAB + b"mkdir -p dist && tar --exclude=build --exclude=dist --exclude=.git \\\n"
    + TAB + b"        -czf dist/xfce4-wayland.tar.gz -C .. xfce4-wayland && \\\n"
    + TAB + b"        echo \"dist/xfce4-wayland.tar.gz created\"\n",
    b"dist: all\n"
    + TAB + b"mkdir -p dist && tar --sort=name --exclude=build --exclude=dist \\\n"
    + TAB + b"        --exclude=.git --exclude=.toolchain \\\n"
    + TAB + b"        -czf dist/xfce4-wayland.tar.gz -C .. xfce4-wayland && \\\n"
    + TAB + b"        echo \"dist/xfce4-wayland.tar.gz created\"\n"
    b"\n"
    b"# ------------------------------------------------------------- installation\n"
    b"# Standards-conscious, configurable layout. DESTDIR is honored for\n"
    b"# staged/packaged installs. A user-local install needs no root:\n"
    b"#   make install prefix=$HOME/.local\n"
    b"# (then extend PATH; see BUILDING.md \"User-local installation\".)\n"
    b"prefix        ?= /usr/local\n"
    b"exec_prefix   ?= $(prefix)\n"
    b"bindir        ?= $(exec_prefix)/bin\n"
    b"datarootdir   ?= $(prefix)/share\n"
    b"datadir       ?= $(datarootdir)\n"
    b"docdir        ?= $(datarootdir)/doc/xfce4-wayland\n"
    b"SESSIONS_DIR  ?= $(datadir)/wayland-sessions\n"
    b"\n"
    b"INST_BINS := build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS)\n"
    b"INST_DOCS := README.md BUILDING.md ARCHITECTURE.md ROADMAP.md \\\n"
    b"        TESTING.md DEPENDENCIES.md SECURITY.md \\\n"
    b"        THIRD-PARTY-LICENSES.md TODO.md LICENSE\n"
    b"\n"
    b"install: all\n"
    + TAB + b"install -d $(DESTDIR)$(bindir) $(DESTDIR)$(SESSIONS_DIR) \\\n"
    b"                $(DESTDIR)$(docdir) $(DESTDIR)$(docdir)/examples\n"
    + TAB + b"for b in $(INST_BINS); do install -m755 \"$$b\" $(DESTDIR)$(bindir) || exit 1; done\n"
    + TAB + b"install -m644 data/wayland-sessions/xfce4-wayland.desktop \\\n"
    b"                $(DESTDIR)$(SESSIONS_DIR)\n"
    + TAB + b"for f in $(INST_DOCS); do install -m644 \"$$f\" $(DESTDIR)$(docdir) || exit 1; done\n"
    + TAB + b"for f in data/examples/*.conf; do \\\n"
    b"                install -m644 \"$$f\" $(DESTDIR)$(docdir)/examples || exit 1; done\n"
    + TAB + b"@echo \"installed to $(DESTDIR)$(prefix)\"\n"
    b"\n"
    b"uninstall:\n"
    + TAB + b"for b in $(notdir $(INST_BINS)); do rm -f $(DESTDIR)$(bindir)/$$b; done\n"
    + TAB + b"rm -f $(DESTDIR)$(SESSIONS_DIR)/xfce4-wayland.desktop\n"
    + TAB + b"rm -rf $(DESTDIR)$(docdir)\n"
    b"\n"
    b"# configuration summary (no compilation)\n"
    b"config:\n"
    + TAB + b"@echo \"xfce4-wayland build configuration\"\n"
    + TAB + b"@echo \"  CC             $(CC)\"\n"
    + TAB + b"@echo \"  CFLAGS         $(CFLAGS)\"\n"
    + TAB + b"@echo \"  LDFLAGS        $(LDFLAGS)\"\n"
    + TAB + b"@echo \"  profile        $(PROFILE)\"\n"
    + TAB + b"@echo \"  sysroot        $(if $(XW_SYSROOT),$(XW_SYSROOT),none (system))\"\n"
    + TAB + b"@echo \"  wayland        $(shell pkg-config --modversion wayland-server 2>/dev/null || echo missing)\"\n"
    + TAB + b"@echo \"  xkbcommon      $(shell pkg-config --modversion xkbcommon 2>/dev/null || echo missing)\"\n"
    + TAB + b"@echo \"  pixman         $(shell pkg-config --modversion pixman-1 2>/dev/null || echo missing)\"\n"
    + TAB + b"@echo \"  X11 backend    $(if $(X11_ON),yes (libX11 $(shell pkg-config --modversion x11 2>/dev/null)),no)\"\n"
    + TAB + b"@echo \"  libinput       $(if $(LIBINPUT_ON),yes ($(shell pkg-config --modversion libinput 2>/dev/null)),no)\"\n"
    + TAB + b"@echo \"  install prefix $(prefix)\"\n",
)

with open(path, "wb") as f:
    f.write(d)

print("patched", path, len(d), "bytes")
