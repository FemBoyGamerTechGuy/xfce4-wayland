#!/usr/bin/env python3
"""Byte-precise Makefile patch: wire test-build-regressions.sh into check."""

import sys

MK = "Makefile"

def main() -> int:
    with open(MK, "rb") as f:
        data = f.read()
    tab = b"\t"
    old = (b"check: tests\n"
           + tab + b"sh scripts/test-session.sh\n")
    new = (b"check: tests\n"
           + tab + b"sh scripts/test-session.sh\n"
           + tab + b"sh scripts/test-build-regressions.sh\n")
    if new in data:
        print("already applied")
        return 0
    if old not in data:
        print("check target anchor not found", file=sys.stderr)
        return 1
    data = data.replace(old, new, 1)
    # also document the target in the header comment
    hdr_old = b"#   check     - tests + process-level session checks\n"
    hdr_new = (b"#   check     - tests + process-level session checks +\n"
               b"#               build-system regression suite\n")
    if hdr_old in data and hdr_new not in data:
        data = data.replace(hdr_old, hdr_new, 1)
    with open(MK, "wb") as f:
        f.write(data)
    print("check target wired")
    return 0

if __name__ == "__main__":
    sys.exit(main())
