# Roadmap

Status legend: **DONE** (implemented + tested), **PART** (implemented,
gaps listed), **TODO**. Honest status only — no silent omissions. Each
done item carries automated coverage noted in [TESTING.md](TESTING.md).

## M0 — Foundations
- DONE repo layout, proprietary LICENSE, third-party audit, build system
  (plain make + wayland-scanner), dev scripts, local sysroot bootstrap.
- DONE vendored protocol XMLs (wlr layer-shell, foreign-toplevel,
  output-management) with provenance recorded.

## M1 — Compositor core (libxw)
- DONE `wl_display` server bootstrap, socket, globals registry.
- DONE headless backend: N outputs (WxH+scale), frame scheduling,
  deterministic test hooks, input injection API.
- DONE wl_shm buffer management (argb8888/xrgb8888), damage tracking.
- DONE pixman renderer: layered compositing, software cursor,
  screenshots-as-pixels API (used by tests).
- DONE single-pixel-buffer protocol (server).
- TODO linux-dmabuf import (blocked on: no dmabuf in headless test env;
  needs DRM backend first).
- TODO DRM/KMS backend; nested (windowed) backend; GL renderer path.
- TODO presentation-time feedback.

## M2 — Shell + window management
- DONE xdg-shell (toplevel + popup): configure/ack state machine,
  maximize/minimize/fullscreen/restores, interactive move/resize
  (keyboard + pointer), edge snapping, workspaces, click-to-focus,
  stacking layers, Alt+Tab MRU switcher, window rules (INI patterns),
  multi-output awareness.
- PART tiling: keyboard half-tiling + edge snap implemented; quarter
  snap via corner drag is TODO.
- TODO xdg-toplevel-drag, xdg-toplevel-icon/tag.
- TODO pointer-constraints + relative-pointer (games/remote).

## M3 — Input + shortcuts
- DONE xkbcommon keymap/state, modifier tracking, wl_keyboard events.
- DONE pointer: motion/button/axis dispatch, click-to-focus.
- DONE shortcut engine: INI config, exact-modifier matching, conflict
  detection, consume-don't-forward, XFCE default table, actions bus.
- TODO key repeat (configurable rate/delay).
- TODO touch gestures.
- TODO xkb per-seat runtime layout switching (switch action exists;
  layout list is single-entry today).

## M4 — Desktop integration protocols
- DONE layer-shell (server + client) — panel surfaces.
- DONE xdg-activation (launcher activation path).
- DONE wlr-foreign-toplevel-management (taskbar integration).
- DONE ext-workspace (panel workspace switching).
- TODO ext-idle-notify + ext-session-lock (screen lock; the exit dialog's
  Lock button currently reports "not implemented" honestly).
- TODO ext-image-copy-capture (screenshots tool; internal pixel API
  exists and is tested).
- TODO wlr-output-management (display settings UI).
- TODO primary-selection + ext-data-control (clipboard manager support;
  core wl_data_device clipboard works).

## M5 — Data transfer
- DONE wl_data_device: selection (copy/paste, text), basic drag & drop.
- TODO mime-type negotiation breadth (only UTF-8 accepted in v0),
  clipboard persistence across app exit, drag icons.

## M6 — Session
- DONE xw-session: runtime dir, autostart (XDG .desktop with OnlyShowIn
  filtering), supervision with restart policy, control socket protocol,
  clean shutdown ordering, restart-session.
- DONE power actions via loginctl CLI (logind/elogind), graceful error
  when unavailable.
- DONE graphical exit: xw-exit dialog (Log Out/Restart/Shutdown/Reboot/
  Suspend/Hibernate/Cancel) + Ctrl+Alt+Delete default binding + panel
  button + `xw-session-ctl` CLI.
- TODO session save/restore of open windows (xfce4-session parity),
  lock screen (see M4), screensaver, idle actions.
- TODO PAM integration for unlock (depends on session-lock work).

## M7 — Panel / desktop
- DONE xw-panel: layer-shell top bar; workspace switcher; taskbar with
  foreign-toplevel activate; application launcher menu from XDG
  .desktop dirs; clock; exit button; keyboard-invokable launcher.
- TODO panel plugins API (XFCE-style external plugins), panel position/
  size/multi-row config, notification area, desktop icons + wallpaper
  (layer-shell background client), notification daemon, volume/power
  indicators, accessibility (a11y) bridge, settings GUI.
- TODO application finder (xfce4-appfinder parity: Super+space style
  search).

## M8 — XWayland compatibility
- TODO optional XWayland startup for legacy X11 apps (never a foundation;
  core desktop does not use it).

## M9 — Hardening/quality
- DONE zero-warning builds (-Wall -Wextra -Werror), leak discipline in
  tests, regression test policy.
- TODO fuzz protocol parsing, valgrind/sanitizer CI job, performance
  benchmarks (damage efficiency), seat/permission security review with
  explicit threat model (see SECURITY.md).
