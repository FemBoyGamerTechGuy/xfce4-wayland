#!/bin/sh
# bootstrap-sysroot.sh — rootless sysroot bootstrap for development.
#
# Downloads distribution packages with `apt-get download` (no root, no
# installation into the system) and extracts them into
# .toolchain/sysroot so that pkg-config, the compiler and the linker can
# see Wayland / xkbcommon / libinput development files without touching
# the host system.
#
# This is a *convenience for Debian-family containers* only. The normal
# way to get these files is to install your distribution's development
# packages (see BUILDING.md); this script exists for locked-down
# development containers where that is not possible.
#
# Usage:   sh scripts/bootstrap-sysroot.sh
# Result:  .toolchain/sysroot usable via `. scripts/env.sh`
set -eu

XW_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="$XW_ROOT/.toolchain"
DL="$TC/dl"
SYS="$TC/sysroot"

command -v apt-get >/dev/null 2>&1 || {
    echo "bootstrap: apt-get not found — this helper is Debian-family only." >&2
    echo "bootstrap: install the equivalent development packages with your" >&2
    echo "bootstrap: package manager instead (see BUILDING.md)." >&2
    exit 1
}

mkdir -p "$DL" "$SYS"
echo "bootstrap: downloading packages (no root, no system changes)"

# Literal package list: zsh does not word-split unquoted parameter
# expansions, so `apt-get download $PKGS` would pass one giant argument
# when this script runs under zsh.
(cd "$DL" && apt-get download \
    libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev \
    libinput-dev libinput10 libudev-dev libxtst-dev libxtst6 \
    libxi-dev libxi6 libevdev2 libwacom9 libmtdev1t64 libgudev-1.0-0)

echo "bootstrap: extracting into $SYS"
for deb in "$DL"/*.deb; do
    dpkg -x "$deb" "$SYS"
done

LIB="$SYS/usr/lib/x86_64-linux-gnu"
PC="$LIB/pkgconfig"

echo "bootstrap: rewriting pkg-config prefix paths"
for pc in "$PC"/*.pc; do
    sed -i "s|^prefix=.*|prefix=$SYS/usr|" "$pc"
    sed -i "s|^exec_prefix=.*|exec_prefix=\${prefix}|" "$pc"
    sed -i "s|^libdir=.*|libdir=$LIB|" "$pc"
    sed -i "s|^includedir=.*|includedir=$SYS/usr/include|" "$pc"
done

echo "bootstrap: linking dev .so symlinks to matching system runtime libs"
# wayland/xkbcommon/udev runtimes are present system-wide in this
# container with matching versions; link the dev SONAME to the system
# copy so binaries run without rpath gymnastics.
for soname in \
    libwayland-client.so.0 libwayland-server.so.0 libwayland-cursor.so.0 \
    libwayland-egl.so.0 libxkbcommon.so.0 libudev.so.1
do
    base="${soname%.so.*}"
    if [ -e "/lib/x86_64-linux-gnu/$soname" ]; then
        ln -sf "/lib/x86_64-linux-gnu/$soname" "$LIB/$base.so"
    fi
done

# libinput runtime is NOT installed system-wide here: keep the sysroot
# copy, link the dev name to it, and record the rpath in libinput.pc
# (upstream's Requires: libudev is preserved so -ludev resolves too).
# libmtdev + libgudev are libinput's/libwacom's own runtime deps: they
# must be extracted too, or the final link fails with "DSO missing"/
# undefined references (g_udev_*, mtdev_*) — apt-get download does not
# resolve dependency closures, so they are listed explicitly above.
if [ -e "$LIB/libinput.so.10" ]; then
    ln -sf libinput.so.10 "$LIB/libinput.so"
    sed -i "s|^Libs:.*|Libs: -L\${libdir} -Wl,-rpath,\${libdir} -linput|" "$PC/libinput.pc"
    grep -q "^Requires: libudev" "$PC/libinput.pc" ||
        sed -i "s|^Libs:|Requires: libudev\nLibs:|" "$PC/libinput.pc"
fi

echo "bootstrap: verifying wayland-scanner"
"$SYS/usr/bin/wayland-scanner" --version

echo "bootstrap: done — run: . scripts/env.sh"
