# Building

This guide is **distribution-agnostic**: the primary instructions work
on any Linux distribution with a C11 toolchain, GNU make and pkg-config.
Distribution package names are given as clearly-marked *examples* at the
end — never as the primary instructions — because package names differ
between distributions and releases. When in doubt, search your package
manager (see ["If your distribution is not listed"](#if-your-distribution-is-not-listed)).

## Contents

1. [Requirements](#requirements)
2. [Quick start](#quick-start)
3. [Build system knobs](#build-system-knobs)
4. [Build profiles](#build-profiles)
5. [Testing](#testing)
6. [Zero-root development](#zero-root-development)
7. [Installation](#installation)
8. [User-local installation (no root)](#user-local-installation-no-root)
9. [Wayland session integration](#wayland-session-integration)
10. [Sysroot bootstrap for locked-down containers](#sysroot-bootstrap-for-locked-down-containers)
11. [Installing dependencies by distribution (examples)](#installing-dependencies-by-distribution-examples)
12. [Troubleshooting](#troubleshooting)

---

## Requirements

The build system is plain GNU make (a deliberate decision: the build
dependency surface stays at `cc + make + pkg-config + wayland-scanner`,
see [DEPENDENCIES.md](DEPENDENCIES.md) for the full audit). It never
invokes a package manager and never requires root: see
[Zero-root development](#zero-root-development).

### Required (build cannot proceed without these)

| Requirement | Notes |
|---|---|
| C11 compiler | gcc >= 10 or clang >= 12; older compilers may work but are not tested |
| Linker | standard `ld` via the compiler driver |
| GNU make | >= 4.2 (`$(file ...)`, `!=` not required; 4.x tested) |
| pkg-config | (or `pkgconf`) locates libraries; checked with an actionable error if missing |
| wayland-scanner | ships with the wayland development package of every distribution; generates protocol glue from XML |
| libwayland dev files | **wayland-server** and **wayland-client**, >= 1.21 (runtime must match: `.so` versions 1.21+ handle all used protocols) |
| wayland-protocols | >= 1.36 (uses xdg-shell, xdg-activation, ext-workspace, single-pixel-buffer; 1.44+ recommended) |
| libxkbcommon dev | >= 1.0 (keymap compilation, modifiers, shortcut keysyms) |
| pixman-1 dev | >= 0.42 (software renderer: compositing, damage regions) |
| python3 + Pillow | **build time only** — rasterizes the bitmap font (`tools/genfont.py`). **No system font is needed**: the font ships in the repository (`assets/fonts/DejaVuSans-ascii.ttf`, a licensed subset of DejaVu Sans — see [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md)); the generated table is identical on every distribution |

### Recommended

| Requirement | Notes |
|---|---|
| libinput dev | >= 1.19 — the real-input backend (udev seat mode + path mode, v120 wheel handling). Without it the compositor still builds and runs (headless + nested input unaffected); see [feature toggles](#build-system-knobs) |
| libudev dev | >= 183 — required **together with** libinput dev: the input backend creates the udev seat context itself (it calls `udev_new()` and passes the context to `libinput_udev_create_context`), which makes libudev a direct compile + link dependency — pkg-config module `libudev` — not something libinput hands out transitively at link time |
| libX11 dev | the nested X11 backend (run the whole desktop inside an X11/XLibre session). Degrades gracefully when absent |
| Xvfb + libXtst + libXi dev | required to run the X11 process-level checks in `make check`; the suite skips nothing silently — the checks fail loudly without Xvfb |
| xkeyboard-config | **runtime only** — xkb keymaps (`evdev/pc105/us` defaults); virtually always installed already |

### Optional

| Requirement | Notes |
|---|---|
| libdrm dev | future DRM/KMS backend for direct hardware output (Phase 4; not yet wired into the build) |
| XWayland | optional compatibility layer for legacy X11 applications (Phase 8; not yet wired) |
| logind / elogind | **runtime only** — power actions (suspend/hibernate/shutdown/reboot) and device-seat management. Neither is required at build time |
| gdb, valgrind | development debugging |

### Category summary

```text
Required            : cc, make, pkg-config, wayland-scanner,
                      wayland-server/client, wayland-protocols,
                      xkbcommon, pixman, python3+Pillow (build-time)
Recommended         : libinput, libX11, Xvfb+Xtst+Xi (for check),
                      xkeyboard-config (runtime)
Optional            : libdrm (future), XWayland (future),
                      logind/elogind (runtime, power+seats)
Development/testing : sanitizers (clang or gcc with ASan/UBSan/LSan)
Runtime only        : xkeyboard-config, loginctl (or elogind's),
                      Xvfb (only for the X11 process checks)
```

---

## Quick start

```sh
git clone https://github.com/FemBoyGamerTechGuy/xfce4-wayland
cd xfce4-wayland
make                 # build everything (compositor, session, panel, tests)
make check           # in-process suite + process-level session checks + build regressions
./scripts/dev-session.sh --logout   # headless dev session, clean logout
```

Every step is fail-fast: `make` stops at the first missing dependency
with an actionable message, and `dev-session.sh` refuses to launch any
binary that was not successfully built (it checks before starting
anything — a failed build can never cascade into confusing launch
errors). The whole flow also runs clean under zsh, bash and dash.

Binaries land in `build/bin/` (`xw-compositor`, `xw-session`,
`xw-session-ctl`, `xw-exit`, `xw-panel`), libraries in `build/lib/`.

`make config` prints the resolved configuration: compiler, flags,
profile, sysroot, dependency versions, backends, install prefix.

To try the desktop **inside your current session** (the safe
development workflow):

```sh
build/bin/xw-session --nested    # WAYLAND_DISPLAY -> nested, DISPLAY -> x11
```

See [README.md](README.md#backends) for backend details.

---

## Build system knobs

All knobs work as `make VAR=value` (repeatable) or via a
`config.local.mk` file (gitignored; loaded automatically). Standard
toolchain variables (`CC`, `AR`, `CFLAGS`, `LDFLAGS`, `PKG_CONFIG_PATH`,
`DESTDIR`) are respected. No hardcoded `/usr`, `/opt` or library paths
exist anywhere in the build: everything resolves through pkg-config or
the configurable install prefix.

| Knob | Values | Effect |
|---|---|---|
| `XW_X11` | `auto` (default) / `1` / `0` | nested X11 backend. `auto` builds it when libX11 dev files are found and prints an actionable note otherwise; `1` requires it (hard error with instructions); `0` never builds it |
| `XW_LIBINPUT` | `auto` (default) / `1` / `0` | real-input backend (needs the libinput **and** libudev dev sets — see the requirement row above). `auto` builds it when both are found, printing an actionable note naming the missing one otherwise; `1` requires both (hard error naming whichever is missing); `0` never builds it. Switching the backend on/off over a populated build tree requires `make clean` (the build refuses to mix feature sets, exactly like PROFILE switching) |
| `PROFILE` | `release` (default) / `debug` / `asan` | compiler/linker preset, see [profiles](#build-profiles) |
| `prefix` | path (default `/usr/local`) | installation prefix |
| `DESTDIR` | path | staged install root (packagers) |
| `XW_SYSROOT` | path | local sysroot with dev files (auto-detects `.toolchain/sysroot`); usually set by [scripts/env.sh](#sysroot-bootstrap-for-locked-down-containers) |
| `XW_FONT` | path | override the build-time font source (default: the bundled `assets/fonts` asset; the override must exist, otherwise the build stops with an error) |
| `bindir`, `datadir`, `docdir`, `SESSIONS_DIR`, `sysconfdir` | paths | install layout overrides (derived from `prefix` by default) |

### Runtime environment variables (input/repeat)

| Variable | Effect |
|---|---|
| `XW_INPUT_DEVICES` | colon-separated evdev nodes for the input source's path mode (`/dev/input/event3:/dev/input/event5`); deterministic, no udev needed. Setting it is the explicit opt-in that lets AUTO mode touch real devices |
| `XW_REPEAT_DELAY_MS`, `XW_REPEAT_RATE_HZ` | override the key-repeat parameters for debugging/testing (defaults 500 ms / 30 Hz) |
| `XW_BACKEND`, `XW_COMPOSITOR` | used by `xw-session` (see `xw-session --help`) |

---

## Build profiles

`PROFILE=` selects a compiler/linker preset. The build tree records the
profile in `build/.profile`; **switching profiles over a populated tree
fails loudly** (`make clean` first) instead of silently mixing plain and
sanitized objects. `CFLAGS`/`LDFLAGS` on the command line override the
preset entirely.

| Profile | Flags | Use |
|---|---|---|
| `release` (default) | `-O2 -g` | production |
| `debug` | `-O0 -g3 -DXW_DEBUG` | development debugging |
| `asan` | `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` (+ link flags) | sanitizer pass — `make asan` drives the full regression (ASan + UBSan + LSan incl. child processes) |

### Recommended configurations by goal

**Minimal development build** — core compositor + tests, nothing optional:

```sh
make clean && make XW_X11=0 XW_LIBINPUT=0
make tests
```

**Nested development build** — everything needed for
`xw-session --nested` inside an existing X11 or Wayland session
(libX11 + libinput included):

```sh
make clean && make          # both optional deps are auto-enabled
build/bin/xw-session --nested    # inside your running Wayland/X11 session
```

**Native Wayland build** — for running real sessions (today: headless
with real input devices; DRM/KMS pending):

```sh
make clean && make XW_LIBINPUT=1
XW_INPUT_DEVICES=/dev/input/event3 build/bin/xw-compositor -B headless
```

**DRM/KMS build** — direct hardware output; the DRM backend is not yet
implemented (ROADMAP Phase 4), so this is currently identical to the
native build above. The build system reserves `libdrm` for it.

**Sanitizer build**:

```sh
make clean && make PROFILE=asan && make tests PROFILE=asan
# or the full regression (restores the release build afterwards):
make asan
```

---

## Testing

```sh
make tests      # in-process integration suite (compositor + real clients)
make check      # tests + process-level session checks (supervision,
                # autostart, Xvfb-driven X11 backend, nested sessions)
make asan       # full ASan/UBSan/LSan regression pass
XWT_FILTER=name ./build/tests/run-tests   # run a single test
```

Test levels, coverage inventory and the real-hardware strategy live in
[TESTING.md](TESTING.md).

---

## Zero-root development

**Nothing in the normal development loop needs root.** The build system
never invokes a package manager, never writes outside the source tree
(or `build/`), and the test suite runs entirely as your user.

### What works without root

- compilation and linking
- the full unit + integration suite (`make tests`)
- process-level session tests (`make check`, including the Xvfb-driven
  X11 backend checks — Xvfb does not need privileges)
- sanitizer builds (ASan, UBSan, LSan)
- the nested Wayland compositor (runs as a normal Wayland client)
- the nested X11/XLibre compositor (a normal X client)
- the panel, session manager, exit dialog, demo clients
- the headless compositor with real input devices via
  `XW_INPUT_DEVICES=/dev/input/event*` (readable nodes only; see below)
- software rendering, configuration tools, `make install
  prefix=$HOME/.local`

### What may require privileges (and how the project avoids them)

| Operation | Why it would need root | How xfce4-wayland handles it |
|---|---|---|
| DRM/KMS output | direct GPU/scanout access | via **logind/elogind**: a registered session gets the DRM device `master` fd without root; the DRM backend (Phase 4) uses this |
| VT (console) switching | VT ioctls | logind/elogind session activation (Phase 4) |
| Direct evdev access | `/dev/input/*` permissions | libinput's udev seat mode receives device fds from udev/logind; the explicit `XW_INPUT_DEVICES` path mode needs nodes readable by your user (many distros put the logged-in user in an `input`-capable group; that is a system decision, not ours) |
| Suspend/hibernate/shutdown/reboot | system-wide state | delegated to **loginctl** (logind *or* elogind — both ship a `loginctl`), which enforces polkit policy for the session user; the compositor itself never runs as root |
| Session/device management | seat ownership | logind/elogind session scope, started by the display manager |

The compositor and session manager are designed to run **as a normal
user inside a registered session**. Never solve a permission problem by
running the desktop as root.

---

## Installation

```sh
make install                # /usr/local (needs write access there)
make install prefix=/usr    # system prefix of your choice
make install DESTDIR=/pkg/stage   # packaging/staged installs
```

Layout (all configurable, see [knobs](#build-system-knobs)):

```text
$(prefix)/bin/                              xw-compositor, xw-session,
                                            xw-session-ctl, xw-exit, xw-panel
$(prefix)/share/wayland-sessions/           xfce4-wayland.desktop
$(prefix)/share/doc/xfce4-wayland/          *.md + LICENSE
$(prefix)/share/doc/xfce4-wayland/examples/ example INI configuration
```

`make uninstall` removes exactly those files (no directory
wildcards). The build output itself stays in `build/` and is never
installed implicitly.

There are no `/etc` writes: user configuration lives in
`$XDG_CONFIG_HOME/xfce4-wayland/` (created by `xw-session` on first
run); example files documenting every key the parsers actually read are
installed under `share/doc/xfce4-wayland/examples/`.

---

## User-local installation (no root)

```sh
make install prefix=$HOME/.local
export PATH="$HOME/.local/bin:$PATH"
```

Then:

- **binaries** are on `PATH` (`xw-session` finds its sibling
  `xw-compositor` automatically);
- the **Wayland session entry** is found by display managers that
  respect `XDG_DATA_DIRS`:
  `export XDG_DATA_DIRS="$HOME/.local/share:$XDG_DATA_DIRS"`;
- **runtime sockets** need `$XDG_RUNTIME_DIR` (any systemd/elogind
  system provides it; otherwise set it to a `chmod 700` tmpdir);
- run nested without touching your session:
  `xw-session --nested` (needs `$WAYLAND_DISPLAY` or `$DISPLAY`).

No `sudo`, `su` or `doas` appears anywhere in this workflow.

---

## Wayland session integration

`make install` ships a `wayland-sessions/xfce4-wayland.desktop` entry:

```text
[Desktop Entry]
Name=XFCE4-Wayland
Comment=This session starts the xfce4-wayland desktop environment
Exec=xw-session
Type=Application
DesktopNames=XFCE
```

### How a display manager discovers the session

Display managers (GDM, SDDM, LightDM, greetd + a selector, ly, ...) list
Wayland sessions by scanning the `wayland-sessions` subdirectory of
every `$XDG_DATA_DIRS` entry (typically `/usr/share` and
`$HOME/.local/share`). Selecting the entry makes the DM start `Exec=`
inside a **logind session** (`XDG_SESSION_TYPE=wayland`, seat and VT
assigned, environment prepared). `xw-session` then supervises the
compositor and desktop components.

### Honest status of each session shape (2026-09)

| Shape | Status |
|---|---|
| Nested inside an existing Wayland session | **works, tested** (`xw-session --nested` with `$WAYLAND_DISPLAY`) |
| Nested inside an existing X11/XLibre session | **works, tested** (Xvfb-verified incl. XTEST input) |
| Headless (no display output) + optional real input devices | **works** (development/CI shape) |
| Real TTY session owning the display via DRM/KMS | **not yet** — requires the DRM/KMS backend (ROADMAP Phase 4). The `.desktop` entry is shipped now so the session appears in DMs the moment that backend lands |

Do **not** assume the DM-listed entry gives you a hardware session
today; that is exactly what the roadmap tracks honestly.

### Testing the session without a display manager

- **Nested** (recommended): `xw-session --nested` from inside your
  running desktop. Everything is real except that output and input are
  forwarded by your current session.
- **Headless**: `build/bin/xw-session` (or `./scripts/dev-session.sh`)
  — full session lifecycle (autostart, panel, supervision, clean
  logout) without display hardware.
- **From a TTY**: possible but pointless today (nothing can own the
  display without the DRM backend). When Phase 4 lands, the procedure
  will be: log into the TTY, run `xw-session` (logind grants the DRM
  master), Ctrl+Alt+F-keys switch VTs.

### How nested differs from a real session

- Output: the compositor renders into a window of the parent session
  instead of scanout buffers (DRM later).
- Input: the parent forwards keyboard/pointer events as Wayland/X
  events; the libinput source is intentionally not started (AUTO mode
  never activates under nested backends).
- Session power: suspend/shutdown pass through the parent session's
  logind just the same (the CLI is identical).
- Environment: `xw-session` sets `XDG_RUNTIME_DIR` per session; nested
  sessions keep `$DISPLAY` set for future XWayland use.

### Exiting cleanly

- Keyboard: Ctrl+Alt+Delete opens the graphical exit dialog (also
  reachable via the panel's exit button).
- The dialog offers Log Out / Restart Session / Shut Down / Reboot /
  Suspend / Hibernate / Cancel, each disabled with a stated reason when
  unavailable (no logind, no kernel support, ...).
- Scripted: `xw-session-ctl logout` (clean client → compositor →
  socket teardown; verified by the process-level test suite).
- Nested: closing the compositor window equals logout (clean teardown,
  not a kill).

---

## Sysroot bootstrap for locked-down containers

If your machine already has the development packages installed through
its package manager, skip this section. If you are in a container or
locked-down environment **without root**, `scripts/bootstrap-sysroot.sh`
downloads the required *development* packages with `apt-get download`
(no installation, no system changes) and extracts them into
`.toolchain/sysroot`:

```sh
sh scripts/bootstrap-sysroot.sh    # Debian-family containers only
. scripts/env.sh                   # makes sysroot visible to the build
make && make check
```

The script:

- downloads wayland/xkbcommon/libinput/udev/Xtst/Xi dev packages plus
  the runtimes not present system-wide (libinput's dependencies:
  libevdev, libwacom, mtdev, libgudev). libudev is not just a libinput
  runtime dep here: the input backend links it directly, so its dev
  files (headers + `libudev.pc`) are required with the libinput feature,
- extracts them with `dpkg -x` into `.toolchain/sysroot` (gitignored,
  never committed),
- rewrites the `.pc` prefix paths, links dev SONAMEs to matching system
  runtimes, and records the sysroot rpath for libinput,
- exports `PKG_CONFIG_PATH`, `PATH`, `LD_LIBRARY_PATH` via
  `scripts/env.sh`.

This is a **convenience for Debian-family development containers**. The
normal path is installing your distribution's own packages. The
sysroot is auto-detected at `./.toolchain/sysroot`; `XW_SYSROOT=` or
`config.local.mk` can point elsewhere.

---

## Installing dependencies by distribution (examples)

The commands below are **examples, not instructions** — package names
and splits differ across distributions *and releases*. The canonical
way to find a package is your package manager's search (next section).
Example groups are minimal (build only) — add the test/optional
packages from the [requirements table](#requirements) as needed.

### Arch Linux / Artix Linux

```sh
sudo pacman -S base-devel wayland wayland-protocols libxkbcommon \
                 pixman libx11 libinput systemd-libs python-pillow
# for `make check`:
sudo pacman -S xorg-server-xvfb libxtst libxi
```

(`systemd-libs` ships `libudev.pc` on Arch and the Artix family; it
provides the udev library only — no systemd service. Verify with
`pkg-config --modversion libudev`.)

### Debian

```sh
sudo apt install build-essential pkg-config libwayland-dev \
                   wayland-protocols libxkbcommon-dev libpixman-1-dev \
                   libx11-dev libinput-dev libudev-dev python3-pil
# for `make check`:
sudo apt install xvfb libxtst-dev libxi-dev
```

(The exact `libpixman-1-dev`/`libwayland-dev`/`libudev-dev` names are
verified against Debian 13 "trixie"; the sysroot bootstrap in this
repo was built from them.)

### Ubuntu

Same package names as Debian (Ubuntu inherits them):

```sh
sudo apt install build-essential pkg-config libwayland-dev \
                   wayland-protocols libxkbcommon-dev libpixman-1-dev \
                   libx11-dev libinput-dev libudev-dev python3-pil
sudo apt install xvfb libxtst-dev libxi-dev   # for make check
```

### Fedora

```sh
sudo dnf install gcc make pkgconf wayland-devel wayland-protocols-devel \
                   libxkbcommon-devel pixman-devel libX11-devel \
                   libinput-devel systemd-devel python3-pillow
# for `make check`:
sudo dnf install xorg-x11-server-Xvfb libXtst-devel libXi-devel
```

(`systemd-devel` carries `libudev.pc`; on openSUSE the same package
name applies.)

### openSUSE

```sh
sudo zypper install gcc make pkg-config wayland-devel wayland-protocols \
                     libxkbcommon-devel pixman-devel libX11-devel \
                     libinput-devel systemd-devel python3-Pillow
# for `make check` (Xvfb lives in the xorg-x11-server package):
sudo zypper install xorg-x11-server libXtst-devel libXi-devel
```

### Void Linux

```sh
sudo xbps-install base-devel wayland-devel wayland-protocols \
                    libxkbcommon-devel pixman-devel libX11-devel \
                    libinput-devel libudev-devel python3-Pillow
# for `make check`:
sudo xbps-install xorgserver-xvfb libXtst-devel libXi-devel
```

### Alpine Linux

```sh
sudo apk add build-base pkgconf wayland-dev wayland-protocols \
                libxkbcommon-dev pixman-dev libx11-dev libinput-dev \
                udev-dev py3-pillow
# for `make check`:
sudo apk add xvfb libxtst-dev libxi-dev
```

(The `sudo` above is *the distribution's* tool for installing
software; the project itself never invokes any of them — see
[Zero-root development](#zero-root-development).)

### If your distribution is not listed

Find the packages with your package manager's search facility and map
them to the [requirements table](#requirements). The names almost
always follow the distribution's dev-package convention:

```text
Arch / Artix   : pacman -Ss wayland        (dev libs usually have no suffix)
Debian family  : apt search wayland        (development packages end in -dev)
Fedora family  : dnf search wayland        (development packages end in -devel)
openSUSE       : zypper search wayland     (development packages end in -devel)
Void           : xbps-query -Rs wayland    (development packages end in -devel)
Alpine         : apk search wayland        (development packages end in -dev)
Slackware      : slackpkg search wayland   (installs ship both headers and libs)
Gentoo         : emerge --search wayland   (headers ship with the library)
Nix/NixOS      : search in nixpkgs for e.g. "libinput", "wayland"
```

What you need, in package-manager-agnostic terms:

1. a C compiler, `make`, `pkg-config`;
2. the **development** files (headers + `.pc`) for `wayland-server`,
   `wayland-client`, `xkbcommon`, `pixman-1`;
3. the **wayland-scanner program** (usually in the wayland dev
   package, sometimes a separate `-bin`/`tools` package);
4. the **wayland-protocols data package** (XML files under
   `/usr/share/wayland-protocols`);
5. optional development files for `libinput` and `x11`;
6. `python3` with **Pillow** for the build-time font generator. No
   font package is required — the rasterized font is bundled in
   `assets/fonts/` (licensed subset of DejaVu Sans).

Verify a dependency is visible after installing: `pkg-config
--modversion wayland-server` (and similarly for the others) must print
a version. If a package installed but pkg-config cannot see it, set
`PKG_CONFIG_PATH` to the directory containing its `.pc` file.

---

## Troubleshooting

The build system's dependency checks print actionable messages that
name the missing component, what stops working without it, and where to
read more. They never call a package manager. Common cases:

**`required dependency check failed: wayland-server wayland-client ...`**
Core compositor libraries are missing. Install your distribution's
wayland development package, or point `PKG_CONFIG_PATH` at a sysroot
(see above). `pkg-config --modversion wayland-server` should print a
version when this is fixed.

**`wayland-scanner was not found`**
The protocol code generator is missing. It ships with the wayland
development package everywhere; with a sysroot make sure
`$XW_SYSROOT/usr/bin` is in `PATH` (`. scripts/env.sh` does this).

**`libinput: development files not found — the real-input backend will
not be built`**
Informational (AUTO mode). Install libinput development files to enable
real input, or `make XW_LIBINPUT=0` to silence the note.

**`libinput: the libudev development files (pkg-config module 'libudev')
were not found — ...`**
Informational (AUTO mode). The input backend creates its own udev
seat context, so libudev is a direct link dependency; libinput alone
is not enough. Install the package shipping `libudev.pc` (see the
distribution examples above) or `make XW_LIBINPUT=0` to silence the
note.

**`libinput backend requested (XW_LIBINPUT=1) but the libudev
development files were not found ...`**
`XW_LIBINPUT=1` was set but the module named in the message is
missing. Install it (the message names the pkg-config module and the
reason), or drop back to `auto`/`0`.

**`undefined reference to 'udev_new'` / `DSO missing from command
line` during linking**
An object references a symbol that lives in a library which is not
*explicitly* on the link command (modern ld refuses to resolve
symbols from indirect DT_NEEDED libraries). The released build does
not produce this: libudev is an explicit direct dependency of the
libinput feature, and `make check` (R6) re-links under an
upstream-shaped `libinput.pc` plus audits every final link command's
symbol coverage ([scripts/test-link-deps.sh]). If you see it after
local changes, add the missing library via `pkg-config --libs
<module>` to the affected link rule in the Makefile — never as a
global flag.

**`build tree holds objects for features '...'` (or no stamp)**
You switched `XW_X11`/`XW_LIBINPUT` across a resolved-state change
over a populated tree (or the tree predates feature tracking). `make
clean` once; the guard exists because archives would otherwise keep
stale members — the same DSO failure class as above.

**`build tree holds objects from PROFILE 'release'`**
You switched `PROFILE` over a populated tree. `make clean` first —
mixing sanitized and plain objects produces broken binaries.

**Tests fail with `cannot connect to display`**
`$XDG_RUNTIME_DIR` is missing or unwritable. `. scripts/env.sh` creates
a usable one; any systemd/elogind system provides it already.

**X11 process checks fail**
`make check` runs the x11 backend under Xvfb; install Xvfb (plus libXtst
and libXi development files — the sysroot bootstrap includes them).

**`the Pillow python module is required at build time ...`**
The build-time font rasterizer is missing its Pillow module. Install
your distribution's package (`python3-pil` / `python-pillow` /
`python3-pillow`) or run `python3 -m pip install --user pillow`
(no root needed).

**`the bundled font asset assets/fonts/DejaVuSans-ascii.ttf is missing`**
The checkout is damaged (the font ships in the repository and no
system font is ever searched). Restore it with
`git checkout -- assets/fonts` or re-extract the release archive.

**`dev-session: .../build/bin/... does not exist or is not executable`**
The build did not complete. `dev-session.sh` refuses to launch
binaries that were not built — run `make` and fix the underlying
build error first (this is deliberate: a failed build must never
cascade into half-started sessions).

**A message like `zsh: number expected` before anything else runs**
Not produced by this repository: all shipped scripts run clean under
zsh 5.9 (verified by `make check`'s shell-compatibility section:
syntax, sourcing and full session execution under zsh). A `number
expected` message from zsh comes from builtin option parsing
(`read -u`-style) in whatever shell function or plugin was active in
your interactive session — reproduce with `zsh -f` (clean mode) to
isolate it from your configuration, and file a report here if a repo
script still misbehaves in clean-mode zsh.
