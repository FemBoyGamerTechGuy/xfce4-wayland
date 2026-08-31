# Building

## Requirements

- A C11 compiler (gcc or clang)
- GNU make
- pkg-config
- wayland-scanner, plus dev files (headers + `.pc`) for:
  - wayland-server, wayland-client >= 1.21
  - libxkbcommon >= 1.0
  - pixman-1 >= 0.42
- wayland-protocols >= 1.36 (we use xdg-activation, ext-workspace,
  single-pixel-buffer; 1.44 or newer recommended)
- python3 + Pillow **at build time** (font rasterization only)
- `xkeyboard-config` keymaps at runtime (usually already installed)

## Normal build (system-wide dev packages)

    make

Outputs land in `build/bin/`, libraries in `build/lib/`.

## Building without root (local sysroot)

If the system lacks the Wayland dev packages (common on locked-down
machines), fetch the distro packages without root and extract them:

    mkdir -p ../.toolchain/debs ../.toolchain/sysroot
    cd ../.toolchain/debs
    apt-get download libwayland-dev libwayland-bin libxkbcommon-dev \
                    wayland-protocols
    for d in *.deb; do dpkg -x "$d" ../sysroot; done

The Makefile auto-detects `../.toolchain/sysroot` (or `XW_SYSROOT=` /
`config.local.mk`). `scripts/env.sh` documents the same paths for
interactive shells.

## Targets

    make            # everything (libs, binaries, tests binary)
    make tests      # build + run the test suite
    make clean      # remove build/
    make dist       # source tarball into dist/

## Cross notes

The tree is self-contained C; cross-building is expected to work by
setting CC/AR and a pkg-config sysroot for the target.
