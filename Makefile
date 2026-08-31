# xfce4-wayland — top-level build
#
# Plain GNU make build (deliberate decision: keeps the dependency surface at
# gcc + make + pkg-config + wayland-scanner; see DEPENDENCIES.md).
#
# Targets:
#   all      — build libraries, binaries
#   tests    — build and run the automated test suite
#   clean    — remove build/
#   dist     — create release archive in dist/

# Optional local overrides (sysroot, CC, extra flags).
-include config.local.mk

# Optional out-of-tree sysroot holding wayland dev files (headers, scanner,
# protocol XML). If unset we fall back to a sibling .toolchain/sysroot
# directory, then to system-wide installs.
XW_SYSROOT ?= $(if $(wildcard $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../.toolchain/sysroot)),$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../.toolchain/sysroot),)

CC       ?= cc
AR       ?= ar
PYTHON   ?= python3

CSTD     := -std=c11
CFLAGS   ?= -O2 -g
LDFLAGS  ?=
WARN      = -Wall -Wextra -Werror -Wshadow -Wpointer-arith
DEFS      = -D_GNU_SOURCE
INCLUDES  = -Isrc -Isrc/libxw -Isrc/libxwcl -Itests/harness -Ibuild/gen
LDLIBS_M  = -lm

export PKG_CONFIG_PATH := $(if $(XW_SYSROOT),$(XW_SYSROOT)/usr/lib/x86_64-linux-gnu/pkgconfig:,$(PKG_CONFIG_PATH))

WAYLAND_SCANNER := $(firstword $(wildcard $(XW_SYSROOT)/usr/bin/wayland-scanner) $(shell command -v wayland-scanner 2>/dev/null))
WP_DIR   := $(firstword $(wildcard $(XW_SYSROOT)/usr/share/wayland-protocols) /usr/share/wayland-protocols)

CFLAGS_WLS   := $(shell pkg-config --cflags wayland-server)
LDLIBS_WLS   := $(shell pkg-config --libs wayland-server)
CFLAGS_WLC   := $(shell pkg-config --cflags wayland-client)
LDLIBS_WLC   := $(shell pkg-config --libs wayland-client)
CFLAGS_XKB   := $(shell pkg-config --cflags xkbcommon)
LDLIBS_XKB   := $(shell pkg-config --libs xkbcommon)
CFLAGS_PIX   := $(shell pkg-config --cflags pixman-1)
LDLIBS_PIX   := $(shell pkg-config --libs pixman-1)

# ---------------------------------------------------------------- protocols
# (XML path, generated basename). wlr-* are vendored in-repo; the rest come
# from the wayland-protocols installation (each upstream protocol lives in
# its own directory, several with -vN suffixes on the XML file name).
PROT_PAIRS := \
	$(WP_DIR)/stable/xdg-shell/xdg-shell.xml|xdg-shell \
	$(WP_DIR)/staging/xdg-activation/xdg-activation-v1.xml|xdg-activation \
	$(WP_DIR)/staging/ext-workspace/ext-workspace-v1.xml|ext-workspace \
	$(WP_DIR)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml|single-pixel-buffer \
	protocols/wlr-layer-shell-unstable-v1.xml|wlr-layer-shell-unstable-v1 \
	protocols/wlr-foreign-toplevel-management-unstable-v1.xml|wlr-foreign-toplevel-management-unstable-v1

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

# Font bitmap generation (build-time; runtime has no font dependency)
GEN_HEADERS += $(GEN)/xw-font-data.h

$(GEN)/xw-font-data.h: tools/genfont.py | $(GEN)
	$(PYTHON) tools/genfont.py -o $@

# consolidated directory rule
DIRECTORIES := $(GEN) $(OBJ)/libxw $(OBJ)/libxwcl $(OBJ)/compositor \
	$(OBJ)/session $(OBJ)/clients $(OBJ)/tests $(OBJ)/gen build/lib \
	build/bin build/tests
$(DIRECTORIES):
	mkdir -p $@


# ----------------------------------------------------------------- libxw
LIBXW_SRC := $(wildcard src/libxw/*.c)
LIBXW_OBJ := $(patsubst src/libxw/%.c,$(OBJ)/libxw/%.o,$(LIBXW_SRC))
LIBXW_DEPS := src/libxw/*.h src/*.h

GEN_PROTO_OBJ := $(GEN_PROTO_SRC:$(GEN)/%.c=$(OBJ)/gen/%.o)
build/lib/libxw.a: $(LIBXW_OBJ) $(GEN_PROTO_OBJ) | build/lib
	$(AR) rcs $@ $(LIBXW_OBJ) $(GEN_PROTO_OBJ)

$(OBJ)/libxw/%.o: src/libxw/%.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@

$(OBJ)/gen/%-protocol.o: $(GEN)/%-protocol.c | $(OBJ)/gen
	$(CC) $(CSTD) $(CFLAGS) -w $(INCLUDES) $(CFLAGS_WLS) -c $< -o $@


# --------------------------------------------------------------- libxwcl
LIBXWCL_SRC := $(wildcard src/libxwcl/*.c)
LIBXWCL_OBJ := $(patsubst src/libxwcl/%.c,$(OBJ)/libxwcl/%.o,$(LIBXWCL_SRC))

LIBXWCL_PROTO := $(OBJ)/gen/xdg-shell-protocol.o \
	$(OBJ)/gen/xdg-activation-protocol.o \
	$(OBJ)/gen/wlr-layer-shell-unstable-v1-protocol.o \
	$(OBJ)/gen/wlr-foreign-toplevel-management-unstable-v1-protocol.o \
	$(OBJ)/gen/ext-workspace-protocol.o \
	$(OBJ)/gen/single-pixel-buffer-protocol.o

build/lib/libxwcl.a: $(LIBXWCL_OBJ) $(LIBXWCL_PROTO) | build/lib
	$(AR) rcs $@ $(LIBXWCL_OBJ) $(LIBXWCL_PROTO)

$(OBJ)/libxwcl/%.o: src/libxwcl/%.c src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/libxwcl
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) -c $< -o $@

# ---------------------------------------------------------------- server bins
build/bin/xw-compositor: $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)

$(OBJ)/compositor/%.o: src/compositor/%.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/compositor
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@

build/bin/xw-session: $(OBJ)/session/xw-session.o | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/session/xw-session.o $(LDLIBS_M)

build/bin/xw-session-ctl: $(OBJ)/session/xw-session-ctl.o | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/session/xw-session-ctl.o $(LDLIBS_M)

$(OBJ)/session/%.o: src/session/%.c | $(OBJ)/session
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) -c $< -o $@

# ---------------------------------------------------------------- clients

build/bin/xw-panel: $(OBJ)/clients/xw-panel.o build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/clients/xw-panel.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)

build/bin/xw-exit: $(OBJ)/clients/xw-exit.o build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/clients/xw-exit.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)

build/bin/xw-demo: $(OBJ)/clients/xw-demo.o build/lib/libxwcl.a | build/bin
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/clients/xw-demo.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)

$(OBJ)/clients/%.o: src/clients/%.c src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/clients
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) -c $< -o $@

# ---------------------------------------------------------------- tests
TEST_SRC := $(wildcard tests/suite/*.c)
build/tests/run-tests: $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a | build/tests
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_M)

$(OBJ)/tests/%.o: tests/suite/%.c tests/harness/xwtest.h $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_WLC) $(CFLAGS_PIX) $(CFLAGS_XKB) -c $< -o $@

$(OBJ)/tests/harness.o: tests/harness/harness.c tests/harness/xwtest.h $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@

$(OBJ)/tests/client.o: tests/harness/client.c tests/harness/xwtest.h $(GEN_HEADERS) | $(OBJ)/tests
	$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) -c $< -o $@

# ---------------------------------------------------------------- targets
.PHONY: all tests check asan clean dist

all: build/bin/xw-compositor build/tests/run-tests
# session manager, exit dialog, panel, demo client: added in later
# milestones (their rules below activate when sources appear)
SESSION_BINS := $(if $(wildcard src/session/xw-session.c),build/bin/xw-session build/bin/xw-session-ctl,)
CLIENT_BINS := $(if $(wildcard src/clients/xw-demo.c),build/bin/xw-demo,) \
	$(if $(wildcard src/clients/xw-exit.c),build/bin/xw-exit,) \
	$(if $(wildcard src/clients/xw-panel.c),build/bin/xw-panel,)
all: build/bin/xw-compositor $(SESSION_BINS) $(CLIENT_BINS) build/tests/run-tests

tests: all
	build/tests/run-tests

check: tests
	sh scripts/test-session.sh

asan:
	sh scripts/run-asan.sh

clean:
	rm -rf build dist

dist: all
	mkdir -p dist && tar --exclude=build --exclude=dist --exclude=.git \
	        -czf dist/xfce4-wayland.tar.gz -C .. xfce4-wayland && \
	        echo "dist/xfce4-wayland.tar.gz created"
