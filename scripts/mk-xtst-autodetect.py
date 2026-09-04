#!/usr/bin/env python3
"""Make x11probe conditional on libXtst availability (the same
auto-detect pattern as every other optional feature), so `make all`
and the ASan profile build succeed on boxes without the XTest dev
files (kbddriver covers the key path there with plain libX11)."""
import sys

MK = "/home/z/my-project/Makefile"
T = b"\t"

with open(MK, "rb") as f:
    data = f.read()

if b"XTST_FOUND" in data:
    print("already present")
    sys.exit(0)

# 1) detection block, after the XTST path logic
anchor = (b"# XTEST probe: xtst dev files live in the optional sysroot (the\n"
          b"# runtime lib may be there too), so use explicit paths with rpath\n"
          b"ifeq ($(X11_ON)$(XW_SYSROOT),y$(XW_SYSROOT))\n")
assert anchor in data, "xtst anchor not found"
detect = (b"# x11probe needs the XTest dev headers; kbddriver (plain\n"
          b"# libX11) covers the same key-path regression where they are\n"
          b"# absent, so the build degrades instead of failing\n"
          b"XTST_FOUND := $(shell printf '%s' '\\#include <X11/extensions/XTest.h>\\nint main(void){return 0;}' | $(CC) -x c - $(CFLAGS_X11) -o /dev/null - >/dev/null 2>&1 && echo y)\n\n"
          )
data = data.replace(anchor, detect + anchor, 1)

# 2) guard the all: entry
old_all = b"all: build/tests/x11probe build/tests/panelprobe build/tests/miniwm \\\n" + T + b"build/tests/fdtest2 build/tests/x11client\n"
assert old_all in data, "all x11probe entry not found"
new_all = (b"ifeq ($(XTST_FOUND),y)\n"
           b"all: build/tests/x11probe build/tests/panelprobe build/tests/miniwm \\\n"
           + T + b"build/tests/fdtest2 build/tests/x11client\n"
           b"else\n"
           + T + b"$(info x11probe: XTest dev files not found - build/tests/kbddriver (libX11 only) covers the key matrix)\n"
           b"endif\n")
data = data.replace(old_all, new_all, 1)

with open(MK, "wb") as f:
    f.write(data)
print("Makefile updated")
