# xfce4-wayland — top-level build
#
# Plain GNU make build (deliberate decision: keeps the dependency surface at
# gcc + make + pkg-config + wayland-scanner; see DEPENDENCIES.md).
#
# Targets:
#   all       — build libraries, binaries
#   tests     — build and run the automated test suite
#   check     — tests + process-level session checks
#   asan      — sanitizer regression pass
#   config    — print the build configuration summary
#   install   — install to $(prefix) (honors DESTDIR)
#   uninstall — remove installed files
#   clean     — remove build/
#   dist      — create release archive in dist/
#
# Knobs (make VAR=value, or config.local.mk):
#   XW_X11=auto|1|0        nested X11 backend (libX11)
#   XW_LIBINPUT=auto|1|0   real-input backend (libinput)
#   XW_DRM=auto|1|0        real DRM/KMS backend (libdrm + libudev)
#   XW_LIBSEAT=auto|1|0    external libseat seat provider (libseat;
#                          the built-in seatd-client and direct-VT
#                          providers need no library)
#   XW_PNG=auto|1|0        PNG icon decoding in the client library
#                          (libpng; without it icons fall back to XPM
#                          and procedural rendering)
#   PROFILE=release|debug|asan   build profile (switching needs
#                          `make clean`; the guard enforces it)
#   prefix=DIR             installation prefix ($HOME/.local works)
#   CC/AR/CFLAGS/LDFLAGS   standard toolchain overrides
#   XW_FONT=PATH           override the build-time font source
#                          (default: bundled assets/fonts asset)

# Optional local overrides (sysroot, CC, extra flags).
-include config.local.mk

# Optional feature knobs for the real-session components.
XW_X11      ?= auto
XW_LIBINPUT ?= auto
XW_DRM      ?= auto
XW_LIBSEAT  ?= auto

# Optional local sysroot holding wayland dev files (headers, scanner,
# protocol XML); never committed. If unset we auto-detect
# ./.toolchain/sysroot, then ../.toolchain/sysroot, then fall back to
# system-wide installs.
XW_SYSROOT ?= $(if $(wildcard $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/.toolchain/sysroot)),$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/.toolchain/sysroot),$(if $(wildcard $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../.toolchain/sysroot)),$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../.toolchain/sysroot),))

CC       ?= cc
AR       ?= ar
PYTHON   ?= python3

# Feature toggles: auto (default: enable when the dependency is
# found, degrade with a notice otherwise) | 1 (require, hard error)
# | 0 (never build). See BUILDING.md.
XW_X11      ?= auto
XW_LIBINPUT ?= auto

# Goals of this invocation (used by guard/check skip logic).
XW_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)

# The protocol rules below are generated via eval and would otherwise
# become the default goal; the default goal must be `all`.
.DEFAULT_GOAL := all

CSTD     := -std=c11

# Build profiles: release (default) | debug | asan. Switching the
# profile requires `make clean`; the guard below fails loudly
# instead of silently mixing plain and sanitized objects.
PROFILE  ?= release
ifeq ($(PROFILE),release)
PROFILE_CFLAGS  := -O2 -g
PROFILE_LDFLAGS :=
else ifeq ($(PROFILE),debug)
PROFILE_CFLAGS  := -O0 -g3 -DXW_DEBUG
PROFILE_LDFLAGS :=
else ifeq ($(PROFILE),asan)
PROFILE_CFLAGS  := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
PROFILE_LDFLAGS := -fsanitize=address,undefined
else
$(error unknown PROFILE '$(PROFILE)' (expected release, debug or asan))
endif
CFLAGS   ?= $(PROFILE_CFLAGS)
LDFLAGS  ?= $(PROFILE_LDFLAGS)

# Profile-switch guard: the build tree records its profile in
# build/.profile. Rebuilding with another PROFILE over a populated
# tree is a stale-object hazard (mixed sanitized/plain objects),
# so refuse and point at `make clean`. `clean` in the goals always
# resets the tree, so the guard steps aside for it.
ifneq ($(filter clean,$(XW_GOALS)),)
XW_PROFILE_GUARD := skip
endif
# `config` (informational only, compiles nothing) may inspect any tree
ifeq ($(XW_GOALS),config)
XW_PROFILE_GUARD := skip
endif
XW_TREE_PROFILE := $(shell cat build/.profile 2>/dev/null)
ifneq ($(XW_PROFILE_GUARD),skip)
ifneq ($(XW_TREE_PROFILE),$(PROFILE))
ifneq ($(wildcard build/obj),)
$(error build tree holds objects from PROFILE '$(XW_TREE_PROFILE)'; run `make clean` before building PROFILE '$(PROFILE)')
endif
endif
endif
WARN      = -Wall -Wextra -Werror -Wshadow -Wpointer-arith
DEFS      = -D_GNU_SOURCE
INCLUDES  = -Isrc -Isrc/libxw -Isrc/libxwcl -Itests/harness -Ibuild/gen
LDLIBS_M  = -lm

export PKG_CONFIG_PATH := $(if $(XW_SYSROOT),$(XW_SYSROOT)/usr/lib/x86_64-linux-gnu/pkgconfig:,$(PKG_CONFIG_PATH))
# Runtime library resolution for make-spawned test binaries
# (sysroot libinput pulls transitive deps from the sysroot).
# No effect without a local sysroot; env.sh does the same for
# interactive shells.
export LD_LIBRARY_PATH := $(if $(XW_SYSROOT),$(XW_SYSROOT)/usr/lib/x86_64-linux-gnu$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH)),$(LD_LIBRARY_PATH))

WAYLAND_SCANNER := $(firstword $(wildcard $(XW_SYSROOT)/usr/bin/wayland-scanner) $(shell command -v wayland-scanner 2>/dev/null))
WP_DIR   := $(firstword $(wildcard $(XW_SYSROOT)/usr/share/wayland-protocols) /usr/share/wayland-protocols)

CFLAGS_WLS   := $(shell pkg-config --cflags wayland-server 2>/dev/null)
LDLIBS_WLS   := $(shell pkg-config --libs wayland-server 2>/dev/null)
CFLAGS_WLC   := $(shell pkg-config --cflags wayland-client 2>/dev/null)
LDLIBS_WLC   := $(shell pkg-config --libs wayland-client 2>/dev/null)
CFLAGS_XKB   := $(shell pkg-config --cflags xkbcommon 2>/dev/null)
LDLIBS_XKB   := $(shell pkg-config --libs xkbcommon 2>/dev/null)
CFLAGS_PIX   := $(shell pkg-config --cflags pixman-1 2>/dev/null)
LDLIBS_PIX   := $(shell pkg-config --libs pixman-1 2>/dev/null)

# ------------------------------------------------ dependency validation
# Actionable failures instead of "dependency not found". The build
# system never invokes a package manager; the messages tell the
# user exactly what is missing and where to read more. Checks are
# skipped for goals that compile nothing (clean, dist, config).
ifeq ($(filter-out clean dist config,$(XW_GOALS)),)
XW_SKIP_DEPS := 1
endif

ifeq ($(XW_SKIP_DEPS),)
ifeq ($(shell command -v pkg-config >/dev/null 2>&1 && echo y),)
$(error pkg-config is required to locate the wayland/xkbcommon/pixman development files but was not found. Install pkg-config first (see BUILDING.md, "Installing dependencies by distribution").)
endif
XW_PC_MISSING :=
ifeq ($(shell pkg-config --exists wayland-server 2>/dev/null && echo y),)
XW_PC_MISSING += wayland-server
endif
ifeq ($(shell pkg-config --exists wayland-client 2>/dev/null && echo y),)
XW_PC_MISSING += wayland-client
endif
ifeq ($(shell pkg-config --exists xkbcommon 2>/dev/null && echo y),)
XW_PC_MISSING += xkbcommon
endif
ifeq ($(shell pkg-config --exists pixman-1 2>/dev/null && echo y),)
XW_PC_MISSING += pixman-1
endif
ifneq ($(XW_PC_MISSING),)
$(error required dependency check failed: $(XW_PC_MISSING). These are the core compositor libraries; nothing can be built without them. Install your distribution's development packages (BUILDING.md, "Installing dependencies by distribution"), or point PKG_CONFIG_PATH at an unpacked sysroot (BUILDING.md, "Sysroot bootstrap for containers").)
endif
ifeq ($(WAYLAND_SCANNER),)
$(error wayland-scanner was not found. It generates the protocol glue from the XML definitions and is required to build. It ships with the wayland development package of every distribution; with a local sysroot, ensure its usr/bin is in PATH (see BUILDING.md).)
endif
ifeq ($(wildcard $(WP_DIR)),)
$(error the wayland-protocols data directory was not found (looked for $(WP_DIR)). The xdg-shell/activation/ext-workspace XMLs are required at build time. Install the wayland-protocols package of your distribution.)
endif
ifeq ($(shell command -v $(PYTHON) >/dev/null 2>&1 && echo y),)
$(error $(PYTHON) was not found. It runs the build-time generators (font rasterization, Makefile feature wiring). Install python3, or pass PYTHON=/path/to/python3.)
endif
ifeq ($(shell $(PYTHON) -c 'import PIL' 2>/dev/null && echo y),)
$(error the Pillow python module is required at build time to rasterize the bundled font into the client bitmap font. Install your distribution's package (python3-pil on Debian/Ubuntu, python-pillow on Arch/Artix, python3-pillow on Fedora/openSUSE; see BUILDING.md) or run: $(PYTHON) -m pip install --user pillow)
endif
ifneq ($(XW_FONT),)
ifeq ($(wildcard $(XW_FONT)),)
$(error XW_FONT is set to '$(XW_FONT)' but that file does not exist.)
endif
else
ifeq ($(wildcard assets/fonts/DejaVuSans-ascii.ttf),)
$(error the bundled font asset assets/fonts/DejaVuSans-ascii.ttf is missing. It is the default build-time font source (no system font is needed or searched). A missing asset means a damaged checkout; restore it with: git checkout -- assets/fonts)
endif
endif
endif # XW_SKIP_DEPS

# ------------------------------------------------ optional components
# X11 nested backend (libX11) — XW_X11 = auto|1|0.
X11_FOUND := $(shell pkg-config --exists x11 2>/dev/null && echo y)
ifeq ($(XW_X11),1)
  ifneq ($(X11_FOUND),y)
    $(error X11 nested backend requested (XW_X11=1) but the libX11 development files were not found. Install your distribution's X11 development package (BUILDING.md), or build with XW_X11=auto/0.)
  endif
  X11_ON := y
else ifeq ($(XW_X11),0)
  X11_ON :=
else
  X11_ON := $(X11_FOUND)
  ifneq ($(X11_FOUND),y)
    $(info x11: libX11 development files not found — the nested X11 backend will not be built.)
    $(info x11: install the libX11 development package (BUILDING.md) to enable it, or set XW_X11=0 to silence this note.)
  endif
endif

ifeq ($(X11_ON),y)
CFLAGS_X11 := $(shell pkg-config --cflags x11 2>/dev/null)
LDLIBS_X11 := $(shell pkg-config --libs x11 2>/dev/null)
HAVE_X11   := -DXW_HAVE_X11_BACKEND
else
CFLAGS_X11 :=
LDLIBS_X11 :=
HAVE_X11   :=
endif

# libinput real-input backend — XW_LIBINPUT = auto|1|0.
# The backend calls libudev DIRECTLY (udev_new creates the seat
# context handed to libinput_udev_create_context), so the feature
# needs TWO development sets: libinput and libudev. The udev_*
# symbols are referenced by xw-input-libinput.o inside libxw.a and
# must appear on the final link line explicitly: upstream
# libinput.pc does not hand out -ludev (pkg-config --libs libinput
# is just -linput; libudev only rides along as libinput's
# DT_NEEDED), and ld's default --no-copy-dt-needed-entries refuses
# to resolve symbols from indirect DSOs — the exact
# "DSO missing from command line" failure seen on Arch/Artix.
# probed with --libs, not --exists: pkgconf's --exists resolves
# Requires.private, and upstream libinput.pc declares Requires.private:
# libudev -- with only the libudev dev files missing, --exists would
# report LIBINPUT itself as missing and misdirect the diagnostic.
# --libs parses the file and its public Requires exactly like the
# link rule will consume it.
LIBINPUT_FOUND := $(shell pkg-config --libs libinput >/dev/null 2>&1 && echo y)
LIBUDEV_FOUND  := $(shell pkg-config --exists libudev 2>/dev/null && echo y)
LIBINPUT_VER   := $(shell pkg-config --modversion libinput 2>/dev/null)
LIBUDEV_VER    := $(shell pkg-config --modversion libudev 2>/dev/null)
ifeq ($(XW_LIBINPUT),1)
  ifneq ($(LIBINPUT_FOUND),y)
    $(error libinput backend requested (XW_LIBINPUT=1) but the libinput development files were not found. libinput drives real keyboards/mice for the native session; the core compositor, nested backends and all tests build and run without it. Install your distribution's libinput development package (BUILDING.md), or build with XW_LIBINPUT=auto/0.)
  endif
  ifneq ($(LIBUDEV_FOUND),y)
    $(error libinput backend requested (XW_LIBINPUT=1) but the libudev development files were not found (pkg-config module 'libudev'). The backend creates the udev context itself for device discovery (udev seat mode), which makes libudev a direct link dependency of libxw.a — not something libinput provides transitively at link time. Install your distribution's libudev development package (BUILDING.md), or build with XW_LIBINPUT=auto/0.)
  endif
  LIBINPUT_ON := y
else ifeq ($(XW_LIBINPUT),0)
  LIBINPUT_ON :=
else
  # auto: enabled only when BOTH direct dependencies resolve
  ifeq ($(LIBINPUT_FOUND)$(LIBUDEV_FOUND),yy)
    LIBINPUT_ON := y
  else
    LIBINPUT_ON :=
    ifneq ($(LIBINPUT_FOUND),y)
      $(info libinput: development files not found — the real-input backend will not be built (headless and nested input are unaffected).)
    else
      $(info libinput: the libudev development files (pkg-config module 'libudev') were not found — the real-input backend will not be built: it creates its own udev context for device discovery, which makes libudev a direct link dependency rather than something libinput provides transitively.)
    endif
    $(info libinput: install the missing development package(s) (BUILDING.md) to enable the backend, or set XW_LIBINPUT=0 to silence this note.)
  endif
endif

ifeq ($(LIBINPUT_ON),y)
CFLAGS_LIBINPUT := $(shell pkg-config --cflags libinput 2>/dev/null)
LDLIBS_LIBINPUT := $(shell pkg-config --libs libinput 2>/dev/null)
HAVE_LIBINPUT   := -DXW_HAVE_LIBINPUT
else
CFLAGS_LIBINPUT :=
LDLIBS_LIBINPUT :=
HAVE_LIBINPUT   :=
endif


# DRM/KMS backend — XW_DRM = auto|1|0. Needs libdrm AND libudev dev
# files: the backend includes <xf86drm.h>/<xf86drmMode.h> directly and
# runs its own udev hotplug monitor, so libudev is a direct link
# dependency (same reasoning as the libinput block above). Without it
# the compositor still builds and runs headless/nested; the DRM
# planning helpers stay compiled (they are libdrm-free) and their tests
# keep running.
DRM_FOUND      := $(shell pkg-config --libs libdrm >/dev/null 2>&1 && echo y)
LIBUDEV_FOUND2 := $(shell pkg-config --exists libudev 2>/dev/null && echo y)
ifeq ($(XW_DRM),1)
  ifneq ($(DRM_FOUND),y)
    $(error DRM backend requested (XW_DRM=1) but the libdrm development files were not found. The DRM backend drives physical display hardware for real TTY sessions; the compositor, nested backends and all tests build and run without it. Install your distribution's libdrm development package (BUILDING.md), or build with XW_DRM=auto/0.)
  endif
  ifneq ($(LIBUDEV_FOUND2),y)
    $(error DRM backend requested (XW_DRM=1) but the libudev development files were not found (pkg-config module 'libudev'). The DRM backend runs its own udev monitor for display hotplug, which makes libudev a direct link dependency. Install your distribution's libudev development package (BUILDING.md), or build with XW_DRM=auto/0.)
  endif
  DRM_ON := y
else ifeq ($(XW_DRM),0)
  DRM_ON :=
else
  ifeq ($(DRM_FOUND)$(LIBUDEV_FOUND2),yy)
    DRM_ON := y
  else
    DRM_ON :=
    $(info drm: libdrm/libudev development files not found — the DRM/KMS backend will not be built (headless, nested X11/Wayland and the seat logic are unaffected).)
    $(info drm: install the libdrm and libudev development packages (BUILDING.md) to enable real display hardware, or set XW_DRM=0 to silence this note.)
  endif
endif

ifeq ($(DRM_ON),y)
CFLAGS_DRM   := $(shell pkg-config --cflags libdrm 2>/dev/null)
LDLIBS_DRM   := $(shell pkg-config --libs libdrm 2>/dev/null)
HAVE_DRM     := -DXW_HAVE_DRM_BACKEND
else
CFLAGS_DRM   :=
LDLIBS_DRM   :=
HAVE_DRM     :=
endif

# External libseat seat provider — XW_LIBSEAT = auto|1|0. Optional by
# design: the compositor has its own seatd wire-protocol client and a
# direct-VT provider, so no functionality is lost without libseat —
# it adds the logind/elogind backends through libseat's own runtime
# selection where the system provides them.
LIBSEAT_FOUND := $(shell pkg-config --libs libseat >/dev/null 2>&1 && echo y)
LIBSEAT_VER   := $(shell pkg-config --modversion libseat 2>/dev/null)
ifeq ($(XW_LIBSEAT),1)
  ifneq ($(LIBSEAT_FOUND),y)
    $(error libseat provider requested (XW_LIBSEAT=1) but the libseat development files were not found. The seatd-client and direct-VT providers cover the same ground without it; install your distribution's libseat development package (BUILDING.md) if you want libseat's logind/elogind backends, or build with XW_LIBSEAT=auto/0.)
  endif
  LIBSEAT_ON := y
else ifeq ($(XW_LIBSEAT),0)
  LIBSEAT_ON :=
else
  LIBSEAT_ON := $(LIBSEAT_FOUND)
  ifneq ($(LIBSEAT_FOUND),y)
    $(warning ----------------------------------------------------------------)
    $(warning libseat: development files not found — the libseat seat provider is NOT built.)
    $(warning Real-TTY consequence: on systems managed by logind/elogind the)
    $(warning compositor will get DRM scanout but NO keyboard and NO mouse:)
    $(warning device fds are granted by the seat manager — never by running as)
    $(warning root, never by relaxing device permissions. The verbose runtime)
    $(warning log will say exactly: "libseat not compiled into this build".)
    $(warning Fix — install the libseat development files and rebuild:)
    $(warning   Debian/Ubuntu: apt install libseat-dev)
    $(warning   Arch/Artix:    pacman -S seatd)
    $(warning   Fedora:        dnf install libseat-devel)
    $(warning   Void:          xbps-install libseat-devel)
    $(warning   Alpine:        apk add libseat-dev)
    $(warning   Gentoo:        emerge gui-libs/libseat)
    $(warning Alternatives: enable the seatd service and join the 'seat' group,)
    $(warning or, on direct-VT-only machines, add this user to the 'input' group.)
    $(warning Details: BUILDING.md, "Seat and session management".)
    $(warning Set XW_LIBSEAT=0 to silence this warning when building on)
    $(warning purpose without libseat (headless/nested dev boxes).)
    $(warning ----------------------------------------------------------------)
  endif
endif

ifeq ($(LIBSEAT_ON),y)
CFLAGS_LIBSEAT := $(shell pkg-config --cflags libseat 2>/dev/null)
LDLIBS_LIBSEAT := $(shell pkg-config --libs libseat 2>/dev/null)
HAVE_LIBSEAT   := -DXW_HAVE_LIBSEAT
else
CFLAGS_LIBSEAT :=
LDLIBS_LIBSEAT :=
HAVE_LIBSEAT   :=
endif

# libpng icon decoding — XW_PNG = auto|1|0. Optional: libxwcl's icon
# pipeline (xwc-icon.c) uses it for PNG icons (hicolor themes are PNG
# almost everywhere). Without it the panel still renders XPM icons and
# degrades to text + procedural fallbacks — no crash, no missing panel.
PNG_FOUND := $(shell pkg-config --exists libpng 2>/dev/null && echo y)
PNG_VER   := $(shell pkg-config --modversion libpng 2>/dev/null)
ifeq ($(XW_PNG),1)
  ifneq ($(PNG_FOUND),y)
    $(error PNG icon decoding requested (XW_PNG=1) but the libpng development files were not found. Install your distribution's libpng development package (BUILDING.md), or build with XW_PNG=auto/0 — icons then fall back to XPM and procedural rendering.)
  endif
  PNG_ON := y
else ifeq ($(XW_PNG),0)
  PNG_ON :=
else
  PNG_ON := $(PNG_FOUND)
  ifneq ($(PNG_FOUND),y)
    $(info png: libpng development files not found — PNG icons will not render (XPM icons and procedural fallbacks still do).)
    $(info png: install the libpng development package (BUILDING.md) to enable PNG icons, or set XW_PNG=0 to silence this note.)
  endif
endif

ifeq ($(PNG_ON),y)
CFLAGS_PNG := $(shell pkg-config --cflags libpng 2>/dev/null)
LDLIBS_PNG := $(shell pkg-config --libs libpng 2>/dev/null)
HAVE_PNG    := -DXW_HAVE_PNG
else
CFLAGS_PNG :=
LDLIBS_PNG :=
HAVE_PNG    :=
endif

# libudev is a DIRECT link dependency when EITHER the libinput backend
# or the DRM backend is built (both create their own udev objects), so
# it resolves independently of which one pulled it in.
ifneq ($(LIBINPUT_ON)$(DRM_ON),)
CFLAGS_LIBUDEV := $(shell pkg-config --cflags libudev 2>/dev/null)
LDLIBS_LIBUDEV := $(shell pkg-config --libs libudev 2>/dev/null)
else
CFLAGS_LIBUDEV :=
LDLIBS_LIBUDEV :=
endif



# Resolved-feature stamp guard: like the PROFILE guard, this
# refuses to mix objects built with a different set of optional
# backends in the same tree. The stamp records the RESOLVED state
# (x11/libinput on/off), so switching between auto and 1 with the
# same outcome never forces a clean; switching a feature on/off
# does. Without the guard the archive would silently keep stale
# members (e.g. a udev-using xw-input-libinput.o left in libxw.a
# while linking without -ludev — the same "DSO missing from
# command line" failure class).
ifeq ($(XW_PROFILE_GUARD),skip)
XW_FEATURE_GUARD := skip
endif
XW_TREE_FEATURES := $(shell cat build/.features 2>/dev/null)
XW_FEATURES_NOW  := x11=$(if $(X11_ON),y,n) libinput=$(if $(LIBINPUT_ON),y,n) drm=$(if $(DRM_ON),y,n) libseat=$(if $(LIBSEAT_ON),y,n) png=$(if $(PNG_ON),y,n)
ifneq ($(XW_FEATURE_GUARD),skip)
ifneq ($(XW_TREE_FEATURES),$(XW_FEATURES_NOW))
ifneq ($(wildcard build/obj),)
ifeq ($(XW_TREE_FEATURES),)
$(error build tree holds objects but no build/.features stamp (built before feature tracking existed, or by a file-target-only build). Run `make clean` once so the tree records its feature set.)
else
$(error build tree holds objects for features '$(XW_TREE_FEATURES)' but the current configuration resolves to '$(XW_FEATURES_NOW)'. Switching XW_X11/XW_LIBINPUT/XW_DRM/XW_LIBSEAT/XW_PNG across a resolved-state change needs a clean tree: run `make clean` first.)
endif
endif
endif
endif

# ---------------------------------------------------------------- protocols
# (XML path, generated basename). wlr-* are vendored in-repo; the rest come
# from the wayland-protocols installation (each upstream protocol lives in
# its own directory, several with -vN suffixes on the XML file name).
PROT_PAIRS := \
	$(WP_DIR)/stable/xdg-shell/xdg-shell.xml|xdg-shell \
	$(WP_DIR)/staging/xdg-activation/xdg-activation-v1.xml|xdg-activation \
	$(WP_DIR)/staging/ext-workspace/ext-workspace-v1.xml|ext-workspace \
	$(WP_DIR)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml|single-pixel-buffer \
	$(WP_DIR)/staging/ext-session-lock/ext-session-lock-v1.xml|ext-session-lock \
	$(WP_DIR)/staging/ext-idle-notify/ext-idle-notify-v1.xml|ext-idle-notify \
	protocols/wlr-layer-shell-unstable-v1.xml|wlr-layer-shell-unstable-v1 \
	protocols/wlr-foreign-toplevel-management-unstable-v1.xml|wlr-foreign-toplevel-management-unstable-v1 \
	protocols/xw-workspace-info-v1.xml|xw-workspace-info-v1

GEN := build/gen
OBJ := build/obj

GEN_HEADERS :=
define PROT_RULE
GEN_HEADERS += $(GEN)/$(2).h $(GEN)/$(2)-protocol.h
$(GEN)/$(2).h: $(1) | $(GEN)
	$$(WAYLAND_SCANNER) client-header $$< $$@
$(GEN)/$(2)-protocol.h: $(1) | $(GEN)
	$$(WAYLAND_SCANNER) server-header $$< $$@
$(GEN)/$(2)-protocol.c: $(1) | $(GEN)
	$$(WAYLAND_SCANNER) private-code $$< $$@
GEN_PROTO_SRC += $(GEN)/$(2)-protocol.c
endef
$(foreach p,$(PROT_PAIRS),$(eval $(call PROT_RULE,$(word 1,$(subst |, ,$(p))),$(word 2,$(subst |, ,$(p))))))

# Font bitmap generation (build-time; runtime has no font dependency).
# Default source: the font bundled in assets/fonts/ -- distro-agnostic
# and deterministic (no system font is searched or required; see
# assets/fonts/README.md). XW_FONT=PATH overrides the source file for
# packagers who want to rasterize their own font (needs Pillow).
XW_FONT ?=
XW_FONT_DEP := $(if $(XW_FONT),$(XW_FONT),assets/fonts/DejaVuSans-latin.ttf)
GEN_HEADERS += $(GEN)/xw-font-data.h

$(GEN)/xw-font-data.h: tools/genfont.py $(XW_FONT_DEP) | $(GEN)
	$(PYTHON) tools/genfont.py -o $@ $(if $(XW_FONT),--font $(XW_FONT))

# consolidated directory rule
DIRECTORIES := $(GEN) $(OBJ)/libxw $(OBJ)/libxwcl $(OBJ)/compositor \
	$(OBJ)/session $(OBJ)/clients $(OBJ)/panel $(OBJ)/tests $(OBJ)/gen build/lib \
	build/bin build/tests
$(DIRECTORIES):
	mkdir -p $@


# ----------------------------------------------------------------- libxw
XW_EXCLUDE_SRC :=
ifneq ($(X11_ON),y)
XW_EXCLUDE_SRC += src/libxw/xw-backend-x11.c
endif
ifneq ($(LIBINPUT_ON),y)
XW_EXCLUDE_SRC += src/libxw/xw-input-libinput.c
endif
LIBXW_SRC := $(filter-out $(XW_EXCLUDE_SRC),$(wildcard src/libxw/*.c))
LIBXW_OBJ := $(patsubst src/libxw/%.c,$(OBJ)/libxw/%.o,$(LIBXW_SRC))
LIBXW_DEPS := src/libxw/*.h src/*.h

GEN_PROTO_OBJ := $(GEN_PROTO_SRC:$(GEN)/%.c=$(OBJ)/gen/%.o)
build/lib/libxw.a: $(LIBXW_OBJ) $(GEN_PROTO_OBJ) | build/lib
	$(AR) rcs $@ $(LIBXW_OBJ) $(GEN_PROTO_OBJ)

$(OBJ)/libxw/%.o: src/libxw/%.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) $(HAVE_LIBINPUT) $(HAVE_DRM) $(HAVE_LIBSEAT) -c $< -o $@

# backend files with mixed includes: nested needs wayland-client
# (loop integration via client-core); x11 needs X11 headers
$(OBJ)/libxw/xw-backend-nested.o: src/libxw/xw-backend-nested.c $(LIBXW_DEPS) src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/libxw
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_WLC) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@

$(OBJ)/libxw/xw-backend-x11.o: src/libxw/xw-backend-x11.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_X11) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) $(HAVE_LIBINPUT) -c $< -o $@

# DRM backend: the pure planning section needs no libdrm, so the file
# is ALWAYS compiled (its tests run everywhere); the KMS half is
# compiled only with libdrm + libudev present
$(OBJ)/libxw/xw-backend-drm.o: src/libxw/xw-backend-drm.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_DRM) $(CFLAGS_LIBUDEV) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_DRM) $(HAVE_LIBINPUT) -c $< -o $@

# seat providers: libseat is optional (the built-in seatd client and
# the direct-VT provider are plain libc); the file is always compiled
$(OBJ)/libxw/xw-session-seat.o: src/libxw/xw-session-seat.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_LIBSEAT) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_LIBSEAT) $(HAVE_DRM) -c $< -o $@


# libinput backend: needs libinput AND libudev headers — the
# backend creates the udev context itself (udev seat mode), so
# <libudev.h> is included directly and the udev_* symbols it
# pulls in must be linked explicitly (see the feature block
# above for why libinput.pc does not provide them).
$(OBJ)/libxw/xw-input-libinput.o: src/libxw/xw-input-libinput.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_LIBINPUT) $(CFLAGS_LIBUDEV) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_LIBINPUT) -c $< -o $@

$(OBJ)/gen/%-protocol.o: $(GEN)/%-protocol.c | $(OBJ)/gen
	$(CC) $(CSTD) $(CFLAGS) -w $(INCLUDES) $(CFLAGS_WLS) -c $< -o $@


# --------------------------------------------------------------- libxwcl
LIBXWCL_SRC := $(wildcard src/libxwcl/*.c)
LIBXWCL_OBJ := $(patsubst src/libxwcl/%.c,$(OBJ)/libxwcl/%.o,$(LIBXWCL_SRC))

LIBXWCL_PROTO := $(OBJ)/gen/xdg-shell-protocol.o \
	$(OBJ)/gen/xw-workspace-info-v1-protocol.o \
	$(OBJ)/gen/xdg-activation-protocol.o \
	$(OBJ)/gen/wlr-layer-shell-unstable-v1-protocol.o \
	$(OBJ)/gen/wlr-foreign-toplevel-management-unstable-v1-protocol.o \
	$(OBJ)/gen/ext-workspace-protocol.o \
	$(OBJ)/gen/single-pixel-buffer-protocol.o \
	$(OBJ)/gen/ext-session-lock-protocol.o \
	$(OBJ)/gen/ext-idle-notify-protocol.o

build/lib/libxwcl.a: $(LIBXWCL_OBJ) $(LIBXWCL_PROTO) | build/lib
	$(AR) rcs $@ $(LIBXWCL_OBJ) $(LIBXWCL_PROTO)

$(OBJ)/libxwcl/%.o: src/libxwcl/%.c src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/libxwcl
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) $(HAVE_PNG) $(CFLAGS_PNG) -c $< -o $@

# ---------------------------------------------------------------- server bins
build/bin/xw-compositor: $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_LIBUDEV) $(LDLIBS_DRM) $(LDLIBS_LIBSEAT) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_PNG) $(LDLIBS_M)

$(OBJ)/compositor/%.o: src/compositor/%.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/compositor
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@

build/bin/xw-session: $(OBJ)/session/xw-session.o $(OBJ)/session/xw-power.o | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/session/xw-session.o $(OBJ)/session/xw-power.o $(LDLIBS_M)

build/bin/xw-session-ctl: $(OBJ)/session/xw-session-ctl.o | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/session/xw-session-ctl.o $(LDLIBS_M)

$(OBJ)/session/%.o: src/session/%.c | $(OBJ)/session
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) -c $< -o $@

# ---------------------------------------------------------------- clients

# the panel lives in its own subproject (subprojects/panel) and builds
# from the CLIENT stack only: libxwcl + xw-ctl. It never links or
# compiles anything from libxw — `make panel` works without the
# compositor and the compositor never references it.
#
# panel modules (menu, clock, pager, taskbar, apps database, config)
# are archived into libpanelcore.a; xw-panel.c is the thin main. The
# test suite links the same archive, so the modules are unit-tested
# without spawning the binary.
PANEL_MAIN := subprojects/panel/xw-panel.c
PANEL_CORE_SRC := $(filter-out $(PANEL_MAIN),$(wildcard subprojects/panel/*.c))
PANEL_CORE_OBJ := $(PANEL_CORE_SRC:subprojects/panel/%.c=$(OBJ)/panel/%.o)

build/lib/libpanelcore.a: $(PANEL_CORE_OBJ) | build/lib
	$(AR) rcs $@ $(PANEL_CORE_OBJ)

build/bin/xw-panel: $(OBJ)/panel/xw-panel.o build/lib/libpanelcore.a $(OBJ)/clients/xw-ctl.o build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/panel/xw-panel.o build/lib/libpanelcore.a $(OBJ)/clients/xw-ctl.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_PNG) $(LDLIBS_M)

$(OBJ)/panel/%.o: subprojects/panel/%.c src/libxwcl/*.h subprojects/panel/*.h $(GEN_HEADERS) | $(OBJ)/panel
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) -c $< -o $@

build/bin/xw-exit: $(OBJ)/clients/xw-exit.o $(OBJ)/clients/xw-ctl.o $(OBJ)/session/xw-power.o build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/clients/xw-exit.o $(OBJ)/clients/xw-ctl.o $(OBJ)/session/xw-power.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_PNG) $(LDLIBS_M)

build/bin/xw-demo: $(OBJ)/clients/xw-demo.o build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/clients/xw-demo.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_PNG) $(LDLIBS_M)

build/bin/xw-lock: $(OBJ)/clients/xw-lock.o build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/clients/xw-lock.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_PNG) $(LDLIBS_M)

$(OBJ)/clients/%.o: src/clients/%.c src/clients/*.h src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/clients
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) -c $< -o $@

# ---------------------------------------------------------------- tests
TEST_SRC := $(wildcard tests/suite/*.c)
build/tests/run-tests: $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a build/lib/libpanelcore.a $(OBJ)/clients/xw-ctl.o | build/tests
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a build/lib/libpanelcore.a $(OBJ)/clients/xw-ctl.o $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_LIBINPUT) $(LDLIBS_LIBUDEV) $(LDLIBS_DRM) $(LDLIBS_LIBSEAT) $(LDLIBS_PNG) $(LDLIBS_M)

$(OBJ)/tests/%.o: tests/suite/%.c tests/harness/xwtest.h $(LIBXW_DEPS) src/libxwcl/*.h subprojects/panel/*.h $(GEN_HEADERS) | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) -Isubprojects/panel $(CFLAGS_WLS) $(CFLAGS_WLC) $(CFLAGS_PIX) $(CFLAGS_XKB) $(HAVE_X11) $(HAVE_LIBINPUT) $(HAVE_LIBSEAT) $(HAVE_DRM) -c $< -o $@

$(OBJ)/tests/harness.o: tests/harness/harness.c tests/harness/xwtest.h $(LIBXW_DEPS) src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@

$(OBJ)/tests/client.o: tests/harness/client.c tests/harness/xwtest.h src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) -c $< -o $@

# XTEST probe helper (process-level x11-backend test; needs Xvfb)
build/tests/x11probe: $(OBJ)/tests/x11probe.o | build/tests
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/x11probe.o $(LDLIBS_X11) $(LDLIBS_XTST) -lm

# nested-X11 regression probe: panel pixels + cursor path (Xvfb)
build/tests/panelprobe: $(OBJ)/tests/panelprobe.o | build/tests
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/panelprobe.o $(LDLIBS_X11) $(LDLIBS_XTST) -lm

# minimal reparenting WM for the nested regression (Xvfb)
build/tests/miniwm: $(OBJ)/tests/miniwm.o | build/tests
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/miniwm.o $(LDLIBS_X11) -lm

# Xlib event-delivery starvation reproducer (diagnostic; see
# xb_watchdog in src/libxw/xw-backend-x11.c)
build/tests/fdtest2: $(OBJ)/tests/fdtest2.o | build/tests
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/fdtest2.o $(LDLIBS_WLS) $(LDLIBS_X11) -lm

# mock seatd server (process-level seat-provider tests; plain libc)
build/tests/mockseatd: $(OBJ)/tests/mockseatd.o | build/tests
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/mockseatd.o $(LDLIBS_M)

# XTEST probe: xtst dev files live in the optional sysroot (the
# runtime lib may be there too), so use explicit paths with rpath
ifeq ($(X11_ON)$(XW_SYSROOT),y$(XW_SYSROOT))
CFLAGS_XTST := -I$(XW_SYSROOT)/usr/include
LDLIBS_XTST  := -lXtst -L$(XW_SYSROOT)/usr/lib/x86_64-linux-gnu -Wl,-rpath,$(XW_SYSROOT)/usr/lib/x86_64-linux-gnu
else
CFLAGS_XTST :=
LDLIBS_XTST  := -lXtst
endif

$(OBJ)/tests/x11probe.o: tests/x11probe.c | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_X11) $(CFLAGS_XTST) -c $< -o $@

$(OBJ)/tests/panelprobe.o: tests/panelprobe.c | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_X11) $(CFLAGS_XTST) -c $< -o $@

$(OBJ)/tests/miniwm.o: tests/miniwm.c | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_X11) -c $< -o $@

$(OBJ)/tests/mockseatd.o: tests/mockseatd.c | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) -c $< -o $@

	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_X11) -c $< -o $@

$(OBJ)/tests/fdtest2.o: tests/fdtest2.c | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_WLS) $(CFLAGS_X11) -c $< -o $@

# ---------------------------------------------------------------- targets
.PHONY: all tests check asan clean dist install uninstall config \
	compositor panel session clients

# session manager, exit dialog, panel, demo client
SESSION_BINS := $(if $(wildcard src/session/xw-session.c),build/bin/xw-session build/bin/xw-session-ctl,)
CLIENT_BINS := $(if $(wildcard src/clients/xw-demo.c),build/bin/xw-demo,) \
	$(if $(wildcard src/clients/xw-exit.c),build/bin/xw-exit,) \
	$(if $(wildcard src/clients/xw-lock.c),build/bin/xw-lock,) \
	$(if $(wildcard subprojects/panel/xw-panel.c),build/bin/xw-panel,)
all: build/.profile build/.features build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS) \
	build/tests/run-tests
ifeq ($(X11_ON),y)
all: build/tests/x11probe build/tests/panelprobe build/tests/miniwm \
	build/tests/fdtest2
endif
all: build/tests/mockseatd

# profile stamp consulted by the PROFILE guard near the top
build/.profile:
	@mkdir -p $(@D)
	@printf '%s\n' '$(PROFILE)' > $@

# resolved-feature stamp consulted by the feature guard above
build/.features:
	@mkdir -p $(@D)
	@printf 'x11=%s libinput=%s drm=%s libseat=%s png=%s\n' '$(if $(X11_ON),y,n)' '$(if $(LIBINPUT_ON),y,n)' '$(if $(DRM_ON),y,n)' '$(if $(LIBSEAT_ON),y,n)' '$(if $(PNG_ON),y,n)' > $@

tests: all
	build/tests/run-tests

# ------------------------------------------------- component build targets
# Each desktop component builds on its own; the compositor never needs
# the panel (and vice versa). See subprojects/README.md for the map.
compositor: build/.profile build/bin/xw-compositor
panel: build/.profile build/bin/xw-panel
session: build/.profile $(SESSION_BINS)
clients: build/.profile build/bin/xw-demo build/bin/xw-exit build/bin/xw-lock

check: tests
	sh scripts/test-session.sh
	sh scripts/test-build-regressions.sh

asan:
	sh scripts/run-asan.sh

clean:
	rm -rf build dist

dist: all
	mkdir -p dist && tar --sort=name --exclude=build --exclude=dist \
	        --exclude=.git --exclude=.toolchain \
	        -czf dist/xfce4-wayland.tar.gz -C .. xfce4-wayland && \
	        echo "dist/xfce4-wayland.tar.gz created"

# ------------------------------------------------------------- installation
# Standards-conscious, configurable layout. DESTDIR is honored for
# staged/packaged installs. A user-local install needs no root:
#   make install prefix=$HOME/.local
# (then extend PATH; see BUILDING.md "User-local installation".)
prefix        ?= /usr/local
exec_prefix   ?= $(prefix)
bindir        ?= $(exec_prefix)/bin
datarootdir   ?= $(prefix)/share
datadir       ?= $(datarootdir)
docdir        ?= $(datarootdir)/doc/xfce4-wayland
SESSIONS_DIR  ?= $(datadir)/wayland-sessions

INST_BINS := build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS)
INST_DOCS := README.md BUILDING.md ARCHITECTURE.md ROADMAP.md \
	TESTING.md DEPENDENCIES.md SECURITY.md \
	THIRD-PARTY-LICENSES.md TODO.md LICENSE

install: all
	install -d $(DESTDIR)$(bindir) $(DESTDIR)$(SESSIONS_DIR) \
	        $(DESTDIR)$(docdir) $(DESTDIR)$(docdir)/examples
	for b in $(INST_BINS); do install -m755 "$$b" $(DESTDIR)$(bindir) || exit 1; done
	install -m644 data/wayland-sessions/xfce4-wayland.desktop \
	        $(DESTDIR)$(SESSIONS_DIR)
	for f in $(INST_DOCS); do install -m644 "$$f" $(DESTDIR)$(docdir) || exit 1; done
	for f in data/examples/*.conf; do \
	        install -m644 "$$f" $(DESTDIR)$(docdir)/examples || exit 1; done
	@echo "installed to $(DESTDIR)$(prefix)"

uninstall:
	for b in $(notdir $(INST_BINS)); do rm -f $(DESTDIR)$(bindir)/$$b; done
	rm -f $(DESTDIR)$(SESSIONS_DIR)/xfce4-wayland.desktop
	rm -rf $(DESTDIR)$(docdir)

# configuration summary (no compilation)
config:
	@echo "xfce4-wayland build configuration"
	@echo "  CC             $(CC)"
	@echo "  CFLAGS         $(CFLAGS)"
	@echo "  LDFLAGS        $(LDFLAGS)"
	@echo "  profile        $(PROFILE)"
	@echo "  sysroot        $(if $(XW_SYSROOT),$(XW_SYSROOT),none (system))"
	@echo "  wayland        $(shell pkg-config --modversion wayland-server 2>/dev/null || echo missing)"
	@echo "  xkbcommon      $(shell pkg-config --modversion xkbcommon 2>/dev/null || echo missing)"
	@echo "  pixman         $(shell pkg-config --modversion pixman-1 2>/dev/null || echo missing)"
	@echo "  X11 backend    $(if $(X11_ON),yes (libX11 $(shell pkg-config --modversion x11 2>/dev/null)),no)"
	@echo "  libinput       $(if $(LIBINPUT_ON),yes (libinput $(LIBINPUT_VER) + libudev $(LIBUDEV_VER)),no)"
	@echo "  DRM backend    $(if $(DRM_ON),yes (libdrm $(shell pkg-config --modversion libdrm 2>/dev/null) + libudev $(LIBUDEV_VER)),no)"
	@echo "  libseat        $(if $(LIBSEAT_ON),yes (libseat $(LIBSEAT_VER)),no - built-in seatd client + direct VT provider always available)"
	@echo "  font source    $(if $(XW_FONT),$(XW_FONT) (XW_FONT override),bundled asset assets/fonts/DejaVuSans-ascii.ttf)"
	@echo "  install prefix $(prefix)"
