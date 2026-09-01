#!/usr/bin/env python3
"""Wire the nested + X11 backends into the Makefile.

Byte-precise (tab-safe) edits:
  1. X11 availability probe + conditional flags
  2. LIBXW_SRC excludes the X11 backend when libX11 is absent
  3. Generic libxw rule gains $(HAVE_X11) (xw-compositor.c gates the
     x11_create call behind XW_HAVE_X11_BACKEND)
  4. Per-file rules for xw-backend-nested.o (needs wayland-client cflags)
     and xw-backend-x11.o (needs X11 cflags)
  5. xw-compositor links libxwcl.a + wayland-client + X11
     (the nested backend is a client of the parent via libxwcl)
  6. run-tests links X11 too (libxw.a may pull x11.o via the dispatch)
"""
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    d = f.read()

def sub(old, new, count=1):
    global d
    assert d.count(old) == count, f"pattern not found (count={d.count(old)}): {old[:70]!r}"
    d = d.replace(old, new)

# --- 1+2: X11 probe + conditional source list -------------------------------
sub(
    b"CFLAGS_PIX   := $(shell pkg-config --cflags pixman-1)\nLDLIBS_PIX   := $(shell pkg-config --libs pixman-1)\n",
    b"CFLAGS_PIX   := $(shell pkg-config --cflags pixman-1)\n"
    b"LDLIBS_PIX   := $(shell pkg-config --libs pixman-1)\n"
    b"\n"
    b"# X11 nested backend: optional at build time (degrades gracefully\n"
    b"# to a runtime error when libX11 is absent)\n"
    b"X11_OK := $(shell pkg-config --exists x11 && echo yes)\n"
    b"ifeq ($(X11_OK),yes)\n"
    b"CFLAGS_X11 := $(shell pkg-config --cflags x11)\n"
    b"LDLIBS_X11 := $(shell pkg-config --libs x11)\n"
    b"HAVE_X11   := -DXW_HAVE_X11_BACKEND\n"
    b"else\n"
    b"CFLAGS_X11 :=\nLDLIBS_X11 :=\nHAVE_X11   :=\n"
    b"endif\n",
)

sub(
    b"LIBXW_SRC := $(wildcard src/libxw/*.c)\n",
    b"ifeq ($(X11_OK),yes)\n"
    b"LIBXW_SRC := $(wildcard src/libxw/*.c)\n"
    b"else\n"
    b"LIBXW_SRC := $(filter-out src/libxw/xw-backend-x11.c,$(wildcard src/libxw/*.c))\n"
    b"endif\n",
)

# --- 3: generic libxw rule gains HAVE_X11 ------------------------------------
sub(
    b"$(OBJ)/libxw/%.o: src/libxw/%.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
    b"\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@\n",
    b"$(OBJ)/libxw/%.o: src/libxw/%.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
    b"\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) -c $< -o $@\n"
    b"\n"
    b"# backend files with mixed includes: nested needs wayland-client\n"
    b"# (loop integration via client-core); x11 needs X11 headers\n"
    b"$(OBJ)/libxw/xw-backend-nested.o: src/libxw/xw-backend-nested.c $(LIBXW_DEPS) src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/libxw\n"
    b"\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_WLC) $(CFLAGS_XKB) $(CFLAGS_PIX) -c $< -o $@\n"
    b"\n"
    b"$(OBJ)/libxw/xw-backend-x11.o: src/libxw/xw-backend-x11.c $(LIBXW_DEPS) $(GEN_HEADERS) | $(OBJ)/libxw\n"
    b"\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLS) $(CFLAGS_X11) $(CFLAGS_XKB) $(CFLAGS_PIX) $(HAVE_X11) -c $< -o $@\n",
)

# --- 5: xw-compositor links libxwcl + client + X11 ---------------------------
sub(
    b"build/bin/xw-compositor: $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a | build/bin\n"
    b"\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n",
    b"build/bin/xw-compositor: $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a | build/bin\n"
    b"\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/compositor/xw-compositor.o build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n",
)

# --- 6: run-tests gains X11 ----------------------------------------------------
sub(
    b"\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_M)\n",
    b"\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/harness.o $(OBJ)/tests/client.o $(patsubst tests/suite/%.c,$(OBJ)/tests/%.o,$(TEST_SRC)) build/lib/libxw.a build/lib/libxwcl.a $(LDLIBS_WLS) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_WLC) $(LDLIBS_X11) $(LDLIBS_M)\n",
)

with open(path, "wb") as f:
    f.write(d)

print("Makefile patched: nested + x11 backends wired")
