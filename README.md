# xfce4-wayland

A community-driven native Wayland implementation of XFCE4, preserving the
XFCE desktop experience while replacing X11-specific components with
modern Wayland-native implementations.

> **Unofficial project.** Not affiliated with or endorsed by the Xfce
> project.

A native Wayland desktop environment that reproduces the functionality,
workflow, configurability, and user experience of XFCE4 on a modern,
Wayland-native architecture.

**This is not** XFCE4 running through XWayland, and it is not a wrapper
around any existing compositor library. It is an original implementation:
our own compositor core, our own window manager, our own panel, session
manager, and settings persistence, speaking Wayland protocols directly.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the design, [ROADMAP.md](ROADMAP.md)
for status and what remains, and [BUILDING.md](BUILDING.md) to build it.

## Design pillars

1. **Wayland-native first.** Core components communicate through Wayland
   protocols. XWayland is an optional compatibility component only.
2. **No wlroots.** The compositor is implemented directly on
   `libwayland-server` with a small, auditable core (see
   [ARCHITECTURE.md](ARCHITECTURE.md), "Why not wlroots").
3. **Minimal, audited dependencies.** Original code carries a proprietary
   license; every third-party dependency is permissively licensed and
   documented in [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).
4. **XFCE workflow parity.** Panel, workspaces, window rules, keyboard
   shortcuts (a first-class subsystem), autostart, session exit dialogs —
   behaving the way XFCE users expect.
5. **A real exit path.** The session can always be ended graphically
   (Log Out / Restart / Shutdown / Reboot / Suspend) with clean teardown —
   no terminal escape hatch required.

## Component map

| Component | Binary | Role |
|-----------|--------|------|
| Compositor core + WM | `xw-compositor` | Display server, window manager, workspaces, input, shortcuts |
| Session manager | `xw-session` | Session lifecycle, autostart, supervision, session d-bus, power actions |
| Session control CLI | `xw-session-ctl` | Scriptable session commands (logout/restart/shutdown/...) |
| Exit dialog | `xw-exit` | Graphical session-exit dialog |
| Demo client | `xw-demo` | Minimal xdg-shell window (tests the desktop: tasklist, stacking, exclusive zone) |
| Panel | `xw-panel` | Layer-shell desktop bar (own subproject, `subprojects/panel/`): applications menu (XDG .desktop database, categories, search), taskbar with icons + overflow list, graphical workspace pager, clock with calendar popup, launchers, action button |
| Screen lock | `xw-lock` | ext-session-lock client: passphrase lock screen, auto-lock on idle (Ctrl+Alt+L) |

## Quick start (development)

```sh
make                    # build everything (no system font needed; the
                        # build-time font is bundled in assets/fonts/)
make compositor         # only the compositor (no panel/session/clients)
make panel              # only the panel — from the client stack alone
                        # (libxwcl; zero compositor objects compiled)
make session            # only the session manager
make clients            # exit dialog, lock, demo client
make check              # full suite: unit + process + build regressions
./scripts/dev-session.sh --logout   # headless dev session (refuses to run
                                    # unless the build succeeded)
```

`. scripts/env.sh` is optional — it only picks up a local
`.toolchain/sysroot` for locked-down containers; normal distributions
don't need it. Fail-fast everywhere: `make` stops at the first missing
dependency with an actionable message, `dev-session.sh` never launches
binaries that were not built, and all scripts run clean under zsh,
bash and dash.

```sh
make install prefix=$HOME/.local   # user-local install (no root)
```

See [BUILDING.md](BUILDING.md) for the full distribution-agnostic
build guide (requirements, profiles, zero-root workflow, distro
package examples, session integration).

## Backends

The compositor runs on four interchangeable backends:

| Backend | Use | Selection |
|---------|-----|-----------|
| headless | tests, CI, DRM-less development | default |
| nested | the whole desktop as a window inside an existing **Wayland** session | `xw-compositor -B nested` or `xw-session --nested` with `$WAYLAND_DISPLAY` |
| x11 | the whole desktop as a window inside an **X11/XLibre** session | `xw-compositor -B x11` or `xw-session --nested` with `$DISPLAY` |
| drm | the physical display through kernel KMS (real TTY sessions) | `xw-session --backend=drm`, or plain `xw-session` from a TTY with KMS hardware |

Nested mode is the development workflow inside an existing desktop —
it runs the real compositor, real WM, real panel and real clients,
safely inside your current session:

```sh
build/bin/xw-session --nested      # picks wayland or x11 automatically
```

DRM mode is the real thing: from a TTY login, the session acquires a
**seat** through whatever the machine provides — an elogind/logind
session (libseat pinned to its logind backend; elogind speaks the same
D-Bus API), the built-in seatd wire-protocol client (zero libraries),
or a direct VT takeover — then drives the
monitor (connector/mode enumeration, dumb-buffer scanout, page
flips) and reads real keyboards/mice through the same seat. A TTY
login has no session d-bus, so `xw-session` also starts one
dbus-daemon as a supervised child (reusing a live bus when there
already is one) — pipewire, wireplumber, xfsettingsd, polkit agents
and portals are bus-activated and quietly die without it. No
systemd, elogind, seatd or display-manager assumptions; no root
compositor, ever:

```sh
make && xw-session                # TTY + KMS -> real desktop
xw-session --backend=drm -V       # verbose seat/device diagnostics
```

The x11 backend is verified end to end under Xvfb with synthesized
(XTEST) keyboard input; the nested Wayland backend is verified both
in-process (a compositor hosting another compositor) and across
process boundaries.

## Status

Early-stage but functional: the compositor core, window manager,
shortcut engine (protocol-correct key repeat, full default-table
coverage), real-input backend (libinput: udev seat + explicit device
modes), session manager with honest logind/elogind power reporting,
graphical exit dialog (unavailable actions show their reason), the
desktop panel, and a real screen locker (ext-session-lock with
server-enforced blanking and input gating — a killed locker leaves the
session locked) exist and pass an automated suite (45 in-process
tests + 103 process-level checks + 50-59 build-system regression checks,
ASAN/UBSAN/LSAN-clean, incl. the forked panel and dialog children;
the build is verified on font-less systems and under zsh). Nested
Wayland and nested X11 backends run the desktop inside an existing
session (Phase 2 of the roadmap). The DRM/KMS backend for direct
hardware output is the current phase (input groundwork landed; scanout
pending). The [ROADMAP](ROADMAP.md) tracks remaining XFCE feature
parity honestly — incomplete features are listed, never silently
omitted.

## License

Original code: proprietary (see [LICENSE](LICENSE)).
Third-party material: its own licenses, see
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).
