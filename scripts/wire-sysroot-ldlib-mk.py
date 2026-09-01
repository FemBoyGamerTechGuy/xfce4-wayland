#!/usr/bin/env python3
"""Byte-precise Makefile patch: export the sysroot runtime library path.

`make tests`/`make check` spawn test binaries directly. When the build
used a local sysroot (locked-down containers), those binaries need
LD_LIBRARY_PATH pointing at the sysroot (a RUNPATH on the executable
does not cover transitive dependencies of libinput). env.sh did this
only for shells that sourced it — the accident-of-environment class
this project is eliminating. The Makefile now exports it itself,
symmetric to its PKG_CONFIG_PATH export; no effect without a sysroot.
"""

import sys

MK = "Makefile"

def main() -> int:
    with open(MK, "rb") as f:
        data = f.read()
    old = (b"export PKG_CONFIG_PATH := $(if $(XW_SYSROOT),"
           b"$(XW_SYSROOT)/usr/lib/x86_64-linux-gnu/pkgconfig:,$(PKG_CONFIG_PATH))\n")
    new = old + (b"# Runtime library resolution for make-spawned test binaries\n"
                 b"# (sysroot libinput pulls transitive deps from the sysroot).\n"
                 b"# No effect without a local sysroot; env.sh does the same for\n"
                 b"# interactive shells.\n"
                 b"export LD_LIBRARY_PATH := $(if $(XW_SYSROOT),"
                 b"$(XW_SYSROOT)/usr/lib/x86_64-linux-gnu"
                 b"$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH)),$(LD_LIBRARY_PATH))\n")
    if new in data:
        print("already applied")
        return 0
    if old not in data:
        print("PKG_CONFIG_PATH export anchor not found", file=sys.stderr)
        return 1
    data = data.replace(old, new, 1)
    with open(MK, "wb") as f:
        f.write(data)
    print("sysroot LD_LIBRARY_PATH export applied")
    return 0

if __name__ == "__main__":
    sys.exit(main())
