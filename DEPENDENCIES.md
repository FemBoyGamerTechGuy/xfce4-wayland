# Dependency policy and audit

Every dependency in this project exists for a documented reason. The
philosophy: a small number of permissively licensed, widely deployed,
focused libraries — no desktop-ecosystem frameworks, no D-Bus requirement,
no wlroots, no systemd requirement.

## Runtime dependencies of shipped code

| Library | License | Version | Why |
|---------|---------|---------|-----|
| libwayland-server | MIT | >= 1.21 | The Wayland display server protocol library. Irreplaceable: implements the wire protocol, object model, event loop. We use `wl_display`/`wl_event_loop`/`wl_resource` directly. |
| libwayland-client | MIT | >= 1.21 | Same, client side, for panel/exit-dialog/test clients. |
| libxkbcommon | MIT/X11 | >= 1.0 | XKB keymap and keyboard state. Required for correct modifier/keysym handling (XKB is the Wayland keyboard standard). Reads keymap data from `xkeyboard-config` at runtime. |
| pixman | MIT | >= 0.42 | Software compositing (ARGB blending, regions, damage). Chosen over an EGL/GLES pipeline so the compositor renders correctly on systems without GPU acceleration; a GPU path can be added later behind the same internal API. |
| libc (glibc/musl) | LGPL-2.1+ (dynamic) | — | C runtime; dynamic linking, LGPL-compliant. |
| python3 + Pillow (build time only) | PSF / HPND | — | Rasterizes the bitmap font into generated C data at build time. **Not a runtime dependency.** |

**Explicitly rejected:**

- **wlroots** — architectural constraint (project spec). We implement the
  backend/compositor/protocol plumbing ourselves.
- **GLib / GIO / GObject** — convenient but drags a large abstraction layer
  into every component. We use plain C with `wl_event_loop` and `poll()`.
- **D-Bus** — not required by the core. Power actions shell out to the
  `loginctl` CLI when logind/elogind is present (no daemon linkage). See
  ARCHITECTURE.md, "Session and power management".
- **GTK / Qt** — the panel and dialogs are native Wayland clients built on
  libxwcl; this keeps the dependency tree flat and licensing clean. A
  GTK-based settings GUI remains a roadmap option.
- **cairo/pango/fontconfig/freetype (runtime)** — replaced by the
  build-time bitmap font generator (`tools/genfont.py`) for panel/dialog
  text. Revisit when proper text shaping becomes necessary.

## Protocol definitions

| Protocol | Source | License | Server side | Client side |
|----------|--------|---------|-------------|-------------|
| wayland core (xdg data-device, wl_seat, wl_shm...) | libwayland | MIT | provided by libwayland | provided by libwayland |
| xdg-shell | wayland-protocols (stable) | MIT | yes | yes |
| xdg-activation | wayland-protocols (staging) | MIT | yes | panel |
| ext-workspace | wayland-protocols (staging) | MIT | yes | panel |
| single-pixel-buffer | wayland-protocols (staging) | MIT | yes | clients |
| wlr-layer-shell-unstable-v1 | wlr-protocols (vendored XML) | MIT | yes | panel |
| wlr-foreign-toplevel-management-unstable-v1 | wlr-protocols (vendored XML) | MIT | yes | panel |

wlr-protocol XML files are protocol *descriptions*, not code: both sides are
implemented in this repository. Vendoring them introduces no wlroots
code linkage. `ext-workspace` and `xdg-activation` are staging protocols —
chosen deliberately because they are the standardized direction; our
compatibility surface is small and tracked in the ROADMAP.

## Optional runtime integrations (not linked, discovered at runtime)

- **loginctl / eloginctl (logind or elogind)** — used for
  Shutdown/Reboot/Suspend/Hibernate and session seat handling. Absent
  systems degrade gracefully (buttons report the limitation).
- **XWayland** — future optional component for legacy X11 clients

### Runtime/build, optional

| Dependency | License | Needed for |
|------------|---------|------------|
| libX11 | X11 (MIT-style) | the nested X11 backend (runs the desktop inside an X11/XLibre session). Absent at build time: the backend is compiled out and `-B x11` reports a clear error; everything else still builds. |
| libXtst + libXext | X11 (MIT-style) | test-only: the Xvfb x11-backend check injects XTEST keyboard input. Fetched rootless into `.toolchain/sysroot` when the system package is missing. |
  (ROADMAP). Never a foundation of the desktop.
