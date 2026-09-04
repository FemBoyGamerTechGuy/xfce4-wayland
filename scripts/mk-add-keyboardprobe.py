#!/usr/bin/env python3
"""Insert the keyboardprobe build target into the Makefile, byte-safe
(tabs preserved — the file's recipes are tab-indented)."""
import sys

ROOT = "/home/z/my-project"
MK = f"{ROOT}/Makefile"

with open(MK, "rb") as f:
    data = f.read()

T = b"\t"

if b"build/tests/keyboardprobe" in data:
    print("already present")
    sys.exit(0)

# 1) link + compile rules, inserted after the mockseatd link rule
anchor = (b"# mock seatd server (process-level seat-provider tests; "
          b"plain libc)\n"
          b"build/tests/mockseatd: $(OBJ)/tests/mockseatd.o | build/tests\n"
          + T + b"$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/mockseatd.o "
          b"$(LDLIBS_M)\n")
assert anchor in data, "mockseatd anchor not found"

probe_rules = (
    b"\n"
    b"# minimal raw Wayland keyboard probe (physical \"backspace types u\"\n"
    b"# instrument: records the exact wl_keyboard wire stream + "
    b"client-side\n"
    b"# decode). Runs against any compositor socket, container or NVIDIA "
    b"box.\n"
    b"build/tests/keyboardprobe: $(OBJ)/tests/keyboardprobe.o \\\n"
    + T + b"$(OBJ)/gen/xdg-shell-protocol.o | build/tests\n"
    + T + b"$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/keyboardprobe.o \\\n"
    + T + b"$(OBJ)/gen/xdg-shell-protocol.o $(LDLIBS_WLC) $(LDLIBS_XKB) "
    b"-lm\n"
    b"\n"
    b"$(OBJ)/tests/keyboardprobe.o: tests/keyboardprobe.c | "
    b"$(OBJ)/tests\n"
    + T + b"$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) "
    b"$(CFLAGS_XKB) -c $< -o $@\n"
)

data = data.replace(anchor, anchor + probe_rules, 1)

# 2) add to the `all:` target
old_all = b"all: build/tests/mockseatd\n"
assert old_all in data, "all: anchor not found"
data = data.replace(old_all, old_all[:-1] + b" build/tests/keyboardprobe\n", 1)

with open(MK, "wb") as f:
    f.write(data)
print("Makefile updated")
