#!/bin/sh
# Source this file to set up the build environment for xfce4-wayland.
#
# It makes a locally extracted sysroot (wayland dev files, protocol XML,
# wayland-scanner) visible to pkg-config and PATH without requiring root.
#
# The sysroot is optional and never committed: it is looked up at
# ./.toolchain/sysroot (gitignored) or ../.toolchain/sysroot. It contains
# third-party binaries/headers under their own licenses; the repository
# itself stays clean of prebuilt artifacts.

# ${BASH_SOURCE:-$0}: when sourced with '.', $0 is the shell, not this file
XW_ROOT="$(cd "$(dirname "${BASH_SOURCE:-$0}")/.." && pwd)"

# Optional local sysroot: looked up at ./.toolchain/sysroot (gitignored,
# never committed) and ../.toolchain/sysroot; otherwise system-wide dev
# files are used.
XW_SYSROOT=""
for candidate in "$XW_ROOT/.toolchain/sysroot" "$XW_ROOT/../.toolchain/sysroot"; do
    if [ -d "$candidate/usr" ]; then
        XW_SYSROOT="$candidate"
        break
    fi
done

if [ -n "$XW_SYSROOT" ]; then
    export XW_SYSROOT
    export PKG_CONFIG_PATH="$XW_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PATH="$XW_SYSROOT/usr/bin:$PATH"
    # runtime libs pulled from the sysroot (libinput and its dependencies
    # are not installed system-wide in locked-down containers)
    export LD_LIBRARY_PATH="$XW_SYSROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    echo "env: using sysroot $XW_SYSROOT"
else
    echo "env: no local sysroot found; relying on system-wide wayland dev files"
fi

# XDG_RUNTIME_DIR must exist for Wayland sockets.
# ${VAR:-} so that set -u callers can source this file safely.
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    XDG_RUNTIME_DIR="/tmp/xdg-runtime-$(id -u)"
    export XDG_RUNTIME_DIR
fi
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null
