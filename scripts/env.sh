#!/bin/sh
# Source this file to set up the build environment for xfce4-wayland.
#
# It makes the locally extracted sysroot (wayland dev files, protocol XML,
# wayland-scanner) visible to pkg-config and PATH without requiring root.
#
# The sysroot lives outside the repository (../.toolchain/sysroot) because it
# contains third-party binaries/headers under their own licenses; the
# repository itself stays clean of prebuilt artifacts.

XW_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XW_SYSROOT="$(cd "$XW_ROOT/.." 2>/dev/null && pwd)/.toolchain/sysroot"

if [ -d "$XW_SYSROOT/usr" ]; then
    export XW_SYSROOT
    export PKG_CONFIG_PATH="$XW_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PATH="$XW_SYSROOT/usr/bin:$PATH"
    echo "env: using sysroot $XW_SYSROOT"
else
    echo "env: no local sysroot found; relying on system-wide wayland dev files"
fi

# XDG_RUNTIME_DIR must exist for Wayland sockets.
if [ -z "$XDG_RUNTIME_DIR" ]; then
    XDG_RUNTIME_DIR="/tmp/xdg-runtime-$(id -u)"
    export XDG_RUNTIME_DIR
fi
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null
