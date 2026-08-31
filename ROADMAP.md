# Roadmap

Status legend: **DONE** (implemented + tested), **PART** (implemented,
gaps listed), **TODO**. Honest status only — no silent omissions. Each
done item carries automated coverage noted in [TESTING.md](TESTING.md).

## M0 — Foundations
- DONE repo layout, proprietary LICENSE, third-party audit, build system
  (plain make + wayland-scanner), dev scripts, local sysroot bootstrap.
- DONE vendored protocol XMLs (wlr layer-shell, foreign-toplevel,
  output-management) with provenance recorded.
- DONE build-time bitmap font (DejaVu rasterized by tools/genfont.py;
  no runtime font dependency).

## M1 — Compositor core (libxw)
- DONE `wl_display` server bootstrap, socket, globals registry.
- DONE headless backend: N outputs (WxH+scale), damage-driven repaint,
  deterministic test hooks, input injection API.
- DONE wl_shm buffer management (argb8888/xrgb8888), damage tracking.
- DONE pixman renderer: layered compositing, software cursor,
  output-pixels introspection API (used by tests).
- DONE single-pixel-buffer protocol (server).
- DONE clean teardown: no double-frees, no leaks under ASAN in the
  integration suite (client teardown ordering fixed and verified).
- TODO linux-dmabuf import (blocked on: no dmabuf in headless test env;
  needs DRM backend first).
- TODO DRM/KMS backend; nested (windowed) backend; GL renderer path.
- TODO presentation-time feedback.

## M2 — Shell + window management
- DONE xdg-shell server (toplevel): configure/ack state machine,
  initial map cycle, maximize/minimize/fullscreen with restores,
  size hints accepted.
- DONE xdg-shell popups: positioner math (anchor/gravity/offset, flip
  and slide constraint adjustment), seat grab, dismissal, reposition.
- DONE window manager: workspaces (count/names via workspaces.conf),
  stacking, click-to-focus + raise, focus fallback on unmap/minimize,
  cascade placement, MRU cycle (Alt+Tab), show-desktop toggle,
  interactive move/resize (pointer + keyboard, Esc cancel),
  edge snapping with preview, tiling (halves), window rules
  (rules.conf patterns at map time), sticky windows.
- PART xdg-shell: geometry offsets (set_window_geometry w/h applied,
  x/y offsets of CSD shadows not yet) — visual gap for shadowed CSD
  clients only.
- TODO xdg-toplevel-drag, xdg-toplevel-icon/tag, xdg-decoration
  negotiation (SSD titles).
- TODO pointer-constraints + relative-pointer (games/remote).

## M3 — Input + shortcuts
- DONE wl_seat/wl_keyboard/wl_pointer, xkbcommon keymap
  (evdev/pc105/us defaults + RMLVO config), shared memfd keymap,
  modifiers, enter/leave, pointer axis.
- DONE shortcut engine: XFCE binding syntax parser, exact-modifier
  matching, conflict detection, consume-don't-forward release
  suppression, actions bus with test hook.
- DONE xfwm4 4.20 default table (verified against the official
  docs.xfce.org keyboard-shortcuts page) + xfce4-settings command
  defaults; shortcuts.conf overrides.
- TODO key repeat (configurable rate/delay).
- TODO touch input.
- TODO xkb per-seat runtime layout switching.

## M4 — Desktop integration protocols
- DONE layer-shell (server): anchoring, margins, exclusive zones →
  wm usable area, keyboard interactivity (exclusive + on-demand).
- DONE xdg-activation (tokens, single-use, focus handover).
- DONE wlr-foreign-toplevel-management (handles, state, requests).
- DONE ext-workspace (group + workspace handles, activate switching).
- TODO ext-idle-notify + ext-session-lock (screen lock).
- TODO ext-image-copy-capture (screenshot protocol; the internal pixel
  API exists and is tested).
- TODO wlr-output-management (display settings UI).
- TODO primary-selection + ext-data-control.

## M5 — Data transfer
- DONE wl_data_device manager: selection with cross-client offers,
  receive forwarding, basic drag & drop (enter/motion/drop/cancel).
- TODO action negotiation (v3 set_actions), clipboard persistence,
  drag icons, primary selection.

## M6 — Session
- TODO xw-session: runtime dir, autostart (XDG .desktop parsing),
  supervision, control socket, clean shutdown ordering.
- TODO power actions via loginctl CLI (logind/elogind).
- TODO graphical exit dialog (xw-exit: Log Out/Restart/Shutdown/
  Reboot/Suspend/Hibernate) + Ctrl+Alt+Delete binding (the action
  and default binding exist; the dialog client is not written yet).
- TODO session save/restore, screen lock, idle actions.

## M7 — Panel / desktop
- TODO xw-panel: layer-shell bar, workspace switcher, tasklist
  (foreign-toplevel), launcher, clock, exit button.
- TODO panel plugin API, notification daemon, desktop icons,
  wallpaper, settings GUI, application finder.

## M8 — XWayland compatibility
- TODO optional XWayland startup for legacy X11 apps (never a
  foundation; the core desktop does not use it).

## M9 — Hardening/quality
- DONE zero-warning builds (-Wall -Wextra -Werror), ASAN-clean
  integration suite (no leaks, no UB) for core flows.
- TODO fuzz protocol parsing, sanitizer CI job, performance
  benchmarks, security review (see SECURITY.md).

## Client library (libxwcl)
- DONE connection + global binding (pumped sync for in-process
  tests), shm double-buffered toplevel windows, layer-shell surfaces,
  client-side xkb, input callbacks, bitmap-font drawing helpers.
- TODO popup windows helper, activation token helper, data-device
  (clipboard) helper for clients.
