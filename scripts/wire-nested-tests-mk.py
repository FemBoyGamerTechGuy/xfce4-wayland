#!/usr/bin/env python3
"""wire-nested-tests-mk.py — idempotent Makefile patch (byte-precise:
never touches recipe TABs) adding the nested-X11 regression tools:

  build/tests/panelprobe  pixel probe: panel visibility + software-cursor
                          path verification (X11 + XTEST)
  build/tests/miniwm      minimal reparenting window manager that resizes
                          clients (X11) — reproduces a real host WM
  build/tests/fdtest2     minimal reproduction of the Xlib event-delivery
                          starvation (X11 + wayland-server)

All are gated on X11_ON like x11probe and added to `all`.
"""
import sys

MK = "Makefile"

PANELPROBE_LINK = (
    "# nested-X11 regression probe: panel pixels + cursor path (Xvfb)\n"
    "build/tests/panelprobe: $(OBJ)/tests/panelprobe.o | build/tests\n"
    "\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/panelprobe.o $(LDLIBS_X11) $(LDLIBS_XTST) -lm\n"
    "\n"
)
PANELPROBE_OBJ = (
    "$(OBJ)/tests/panelprobe.o: tests/panelprobe.c | $(OBJ)/tests\n"
    "\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_X11) $(CFLAGS_XTST) -c $< -o $@\n"
    "\n"
)
MINIWM_LINK = (
    "# minimal reparenting WM for the nested regression (Xvfb)\n"
    "build/tests/miniwm: $(OBJ)/tests/miniwm.o | build/tests\n"
    "\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/miniwm.o $(LDLIBS_X11) -lm\n"
    "\n"
)
MINIWM_OBJ = (
    "$(OBJ)/tests/miniwm.o: tests/miniwm.c | $(OBJ)/tests\n"
    "\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_X11) -c $< -o $@\n"
    "\n"
)
FDTEST2_LINK = (
    "# Xlib event-delivery starvation reproducer (diagnostic; see\n"
    "# xb_watchdog in src/libxw/xw-backend-x11.c)\n"
    "build/tests/fdtest2: $(OBJ)/tests/fdtest2.o | build/tests\n"
    "\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/tests/fdtest2.o $(LDLIBS_WLS) $(LDLIBS_X11) -lm\n"
    "\n"
)
FDTEST2_OBJ = (
    "$(OBJ)/tests/fdtest2.o: tests/fdtest2.c | $(OBJ)/tests\n"
    "\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(CFLAGS_WLS) $(CFLAGS_X11) -c $< -o $@\n"
    "\n"
)
ALL_HOOK = (
    "ifeq ($(X11_ON),y)\n"
    "all: build/tests/x11probe\n"
    "endif"
)
ALL_HOOK_NEW = (
    "ifeq ($(X11_ON),y)\n"
    "all: build/tests/x11probe build/tests/panelprobe build/tests/miniwm \\\n"
    "\tbuild/tests/fdtest2\n"
    "endif"
)


def main():
    with open(MK, "rb") as f:
        data = f.read().decode("utf-8")
    changed = False

    def insert_once(anchor, block, after=True):
        nonlocal data, changed
        if block in data:
            return
        if anchor not in data:
            print(f"wire-nested-tests: anchor not found: {anchor[:60]!r}")
            sys.exit(1)
        new = data.replace(anchor, anchor + block if after else block + anchor, 1)
        assert new != data
        data = new
        changed = True

    # link rules + object rules after the x11probe ones
    insert_once(
        "# XTEST probe: xtst dev files live in the optional sysroot (the",
        PANELPROBE_LINK + MINIWM_LINK + FDTEST2_LINK,
        after=False,
    )
    insert_once(
        "# ---------------------------------------------------------------- targets",
        PANELPROBE_OBJ + MINIWM_OBJ + FDTEST2_OBJ,
        after=False,
    )
    # all: hook
    if ALL_HOOK in data and ALL_HOOK_NEW not in data:
        data = data.replace(ALL_HOOK, ALL_HOOK_NEW, 1)
        changed = True
    elif ALL_HOOK_NEW not in data:
        print("wire-nested-tests: all-hook pattern not found")
        sys.exit(1)

    if changed:
        with open(MK, "wb") as f:
            f.write(data.encode("utf-8"))
        print("wire-nested-tests: Makefile patched")
    else:
        print("wire-nested-tests: already wired (no-op)")


if __name__ == "__main__":
    main()
