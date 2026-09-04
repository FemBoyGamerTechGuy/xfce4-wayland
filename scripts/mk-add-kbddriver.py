#!/usr/bin/env python3
"""Insert the kbddriver build target into the Makefile, byte-safe."""
import sys

MK = "/home/z/my-project/Makefile"
T = b"\t"

with open(MK, "rb") as f:
    data = f.read()

if b"build/tests/kbddriver" in data:
    print("already present")
    sys.exit(0)

anchor = (b"$(OBJ)/tests/keyboardprobe.o: tests/keyboardprobe.c | "
          b"$(OBJ)/tests\n"
          + T + b"$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) "
          b"$(CFLAGS_XKB) -c $< -o $@\n")
assert anchor in data, "keyboardprobe anchor not found"

rules = (
    b"\n"
    b"# X-side key driver for the physical-kbd regression (XSendEvent;\n"
    b"# libX11 only so it builds wherever the X11 backend does)\n"
    b"build/tests/kbddriver: $(OBJ)/tests/kbddriver.o | build/tests\n"
    + T + b"$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/kbddriver.o "
    b"$(LDLIBS_X11) -lm\n"
    b"\n"
    b"$(OBJ)/tests/kbddriver.o: tests/kbddriver.c | $(OBJ)/tests\n"
    + T + b"$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_X11) "
    b"-c $< -o $@\n"
)

data = data.replace(anchor, anchor + rules, 1)

old_all = b"all: build/tests/mockseatd build/tests/keyboardprobe\n"
assert old_all in data, "all: anchor not found"
data = data.replace(
    old_all, old_all[:-1] + b" build/tests/kbddriver\n", 1)

with open(MK, "wb") as f:
    f.write(data)
print("Makefile updated")
