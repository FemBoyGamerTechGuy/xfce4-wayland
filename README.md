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
| Session manager | `xw-session` | Session lifecycle, autostart, supervision, power actions |
| Session control CLI | `xw-session-ctl` | Scriptable session commands (logout/restart/shutdown/...) |
| Exit dialog | `xw-exit` | Graphical session-exit dialog |
| Panel | `xw-panel` | Layer-shell desktop bar: workspace switcher, tasklist, launcher, clock, exit button |
| Demo client | `xw-demo` | *(planned)* Test/demo toplevel client for development |

## Quick start (development)

```sh
. scripts/env.sh        # pick up local sysroot (no root needed)
make                    # build everything
make check              # in-process suite + process-level session tests
make asan               # full sanitizer regression pass
./scripts/dev-session.sh --logout   # headless dev session, clean logout
```

## Backends

The compositor runs on three interchangeable backends:

| Backend | Use | Selection |
|---------|-----|-----------|
| headless | tests, CI, DRM-less development | default |
| nested | the whole desktop as a window inside an existing **Wayland** session | `xw-compositor -B nested` or `xw-session --nested` with `$WAYLAND_DISPLAY` |
| x11 | the whole desktop as a window inside an **X11/XLibre** session | `xw-compositor -B x11` or `xw-session --nested` with `$DISPLAY` |

Nested mode is the intended development workflow before DRM/KMS is
stable — it runs the real compositor, real WM, real panel and real
clients, safely inside your current desktop:

```sh
build/bin/xw-session --nested      # picks wayland or x11 automatically
```

The x11 backend is verified end to end under Xvfb with synthesized
(XTEST) keyboard input; the nested Wayland backend is verified both
in-process (a compositor hosting another compositor) and across
process boundaries.

## Status

Early-stage but functional: the compositor core, window manager,
shortcut engine, session manager, graphical exit dialog, and the
desktop panel exist and pass an automated suite (22 in-process tests +
47 process-level checks, ASAN/UBSAN/LSAN-clean, incl. the forked panel
and dialog children). Nested Wayland and nested X11 backends run the
desktop inside an existing session (Phase 2 of the roadmap). The [ROADMAP](ROADMAP.md) tracks remaining XFCE
feature parity honestly — incomplete features are listed, never
silently omitted.

## License

Original code: proprietary (see [LICENSE](LICENSE)).
Third-party material: its own licenses, see
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).
