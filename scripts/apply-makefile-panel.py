#!/usr/bin/env python3
"""Apply the subprojects/panel Makefile changes tab-safely.

The interactive Edit path normalized the file's recipe TABs to spaces,
breaking make ("missing separator"). This script performs the same four
edits with byte-exact strings so every untouched recipe keeps its TAB.
Run once after `git checkout -- Makefile`:

    python3 scripts/apply-makefile-panel.py
"""
import sys

MK = "Makefile"

edits = [
    # 1. DIRECTORIES: add build/obj/panel
    (
        "DIRECTORIES := $(GEN) $(OBJ)/libxw $(OBJ)/libxwcl $(OBJ)/compositor \\\n"
        "\t$(OBJ)/session $(OBJ)/clients $(OBJ)/tests $(OBJ)/gen build/lib \\\n"
        "\tbuild/bin build/tests\n",
        "DIRECTORIES := $(GEN) $(OBJ)/libxw $(OBJ)/libxwcl $(OBJ)/compositor \\\n"
        "\t$(OBJ)/session $(OBJ)/clients $(OBJ)/panel $(OBJ)/tests $(OBJ)/gen build/lib \\\n"
        "\tbuild/bin build/tests\n",
    ),
    # 2. panel rules (object from subprojects/panel, binary from client stack)
    (
        "# ---------------------------------------------------------------- clients\n"
        "\n"
        "build/bin/xw-panel: $(OBJ)/clients/xw-panel.o $(OBJ)/clients/xw-ctl.o build/lib/libxwcl.a | build/bin\n"
        "\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/clients/xw-panel.o $(OBJ)/clients/xw-ctl.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n",
        "# ---------------------------------------------------------------- clients\n"
        "\n"
        "# the panel lives in its own subproject (subprojects/panel) and builds\n"
        "# from the CLIENT stack only: libxwcl + xw-ctl. It never links or\n"
        "# compiles anything from libxw — `make panel` works without the\n"
        "# compositor and the compositor never references it.\n"
        "build/bin/xw-panel: $(OBJ)/panel/xw-panel.o $(OBJ)/clients/xw-ctl.o build/lib/libxwcl.a | build/bin\n"
        "\t$(CC) $(LDFLAGS) -o $@ $(OBJ)/panel/xw-panel.o $(OBJ)/clients/xw-ctl.o build/lib/libxwcl.a $(LDLIBS_WLC) $(LDLIBS_XKB) $(LDLIBS_PIX) $(LDLIBS_M)\n"
        "\n"
        "$(OBJ)/panel/%.o: subprojects/panel/%.c src/libxwcl/*.h $(GEN_HEADERS) | $(OBJ)/panel\n"
        "\t$(CC) $(CSTD) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(CFLAGS_WLC) $(CFLAGS_PIX) -c $< -o $@\n",
    ),
    # 3. CLIENT_BINS path for the moved source
    (
        "\t$(if $(wildcard src/clients/xw-panel.c),build/bin/xw-panel,)\n",
        "\t$(if $(wildcard subprojects/panel/xw-panel.c),build/bin/xw-panel,)\n",
    ),
    # 4. PHONY + component targets
    (
        ".PHONY: all tests check asan clean dist install uninstall config\n",
        ".PHONY: all tests check asan clean dist install uninstall config \\\n"
        "\tcompositor panel session clients\n",
    ),
    (
        "tests: all\n"
        "\tbuild/tests/run-tests\n",
        "tests: all\n"
        "\tbuild/tests/run-tests\n"
        "\n"
        "# ------------------------------------------------- component build targets\n"
        "# Each desktop component builds on its own; the compositor never needs\n"
        "# the panel (and vice versa). See subprojects/README.md for the map.\n"
        "compositor: build/bin/xw-compositor\n"
        "panel: build/bin/xw-panel\n"
        "session: $(SESSION_BINS)\n"
        "clients: build/bin/xw-demo build/bin/xw-exit build/bin/xw-lock\n",
    ),
]


def main() -> int:
    with open(MK, "r", encoding="utf-8") as f:
        text = f.read()
    for old, new in edits:
        if text.count(old) != 1:
            print(f"FAIL: expected exactly 1 match, got {text.count(old)} for:\n{old[:90]}...")
            return 1
        text = text.replace(old, new)
    with open(MK, "w", encoding="utf-8") as f:
        f.write(text)
    print("Makefile: 5 edits applied (tabs preserved)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
