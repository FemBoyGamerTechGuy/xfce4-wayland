#!/bin/sh
# quickstart-clean.sh — verify the DOCUMENTED quick-start on a pristine
# clone of this tree (the "can a clean checkout build and run" gate).
#
#   . scripts/env.sh && make && ./scripts/dev-session.sh --logout
#
# The clone lives in a temp dir; a local .toolchain sysroot (if the
# source tree has one) is exposed to it the same way it appears next
# to a developer checkout. Nothing outside /tmp is touched; no root.
#
# Usage: scripts/quickstart-clean.sh [clone-dir]
set -u
SRC="$(cd "$(dirname "$0")/.." && pwd)"
CLONE="${1:-/tmp/xw-quickstart-clean}"

rm -rf "$CLONE"
git clone -q "$SRC" "$CLONE" || exit 2

if [ -d "$SRC/.toolchain/sysroot" ]; then
    mkdir -p "$CLONE/.toolchain"
    [ -e "$CLONE/.toolchain/sysroot" ] || \
        ln -s "$SRC/.toolchain/sysroot" "$CLONE/.toolchain/sysroot"
fi

cd "$CLONE" || exit 2
. ./scripts/env.sh || exit 2
make -s all || { echo "quickstart-clean: BUILD FAILED"; exit 1; }
./scripts/dev-session.sh --logout
