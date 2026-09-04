#!/usr/bin/env python3
"""Wire build/tests/x11client into the `all` target (tab-safe).

The Edit tool normalizes tab indentation to spaces, which corrupts
Makefile recipe lines (make: "missing separator"). All Makefile
surgery goes through byte-exact scripts like this one.
"""
import sys

PATH = "Makefile"
data = open(PATH, "rb").read()

OLD = b"all: build/tests/x11probe build/tests/panelprobe build/tests/miniwm \\\n\tbuild/tests/fdtest2\nendif"
NEW = b"all: build/tests/x11probe build/tests/panelprobe build/tests/miniwm \\\n\tbuild/tests/fdtest2 build/tests/x11client\nendif"

if data.count(OLD) != 1:
    print(f"FAIL: anchor found {data.count(OLD)} times", file=sys.stderr)
    sys.exit(1)
data = data.replace(OLD, NEW)
open(PATH, "wb").write(data)
print("ok: x11client wired into all (X11_ON block), tabs preserved")
