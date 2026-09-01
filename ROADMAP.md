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
- DONE nested backends (Phase 2): Wayland (a client of the parent via
  libxwcl) and X11 (libX11 + XPutImage present path, XTEST-verified
  input). `xw-compositor -B nested|x11`, `xw-session --nested`.
- TODO DRM/KMS backend (Phase 4) for physical displays; libinput seat
  backend for real devices; GL renderer path.
- TODO presentation-time feedback.

## M2 — Shell + window management
- DONE xdg-shell server (toplevel): configure/ack state machine,
  initial map cycle, maximize/minimize/fullscreen with restores,
  size hints accepted.
- DONE xdg-shell popups: positioner math (anchor/gravity incl. corner
  combinations/offset, flip and slide constraint adjustment), seat grab,
  dismissal, reposition, outside-click dismissal of the popup chain
  (xfwm4 menu behavior).
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
- DONE layer-shell (server): set_size/set_anchor/margins/exclusive
  zones → wm usable area, set_layer restacking, keyboard interactivity
  (exclusive + on-demand).
- DONE xdg-activation (tokens, single-use, focus handover).
- DONE wlr-foreign-toplevel-management (handles, state, requests).
- DONE ext-workspace (group + workspace handles, activate switching).
- TODO ext-idle-notify + ext-session-lock (screen lock).
- TODO ext-image-copy-capture (screenshot protocol; the internal pixel
  API exists and is tested).
- TODO wlr-output-management (display settings UI).
- TODO primary-selection + ext-data-control.

## M5 — Data transfer
- DONE wl_data_device manager: selection with cross-client offers
  (NULL-source clears honestly — no fabricated empty offers), receive
  forwarding, basic drag & drop (enter/motion/drop/cancel).
- TODO action negotiation (v3 set_actions), clipboard persistence,
  drag icons, primary selection.

## M6 — Session
- DONE xw-session: runtime dir, autostart (XDG .desktop parsing with
  OnlyShowIn/NotShowIn/Hidden + user-overrides-system), compositor
  supervision with bounded restarts, control socket line protocol,
  clean shutdown ordering (clients → compositor → sockets).
- DONE xw-session-ctl: status/ping/logout/restart/shutdown/reboot/
  suspend/hibernate/exit-dialog/run commands. Runtime spawns (exit
  dialog, `run CMD`) are supervised session children: SIGTERM at
  shutdown, reaped by the SIGCHLD loop.
- DONE power actions via loginctl CLI; fails honestly when logind/
  elogind is unavailable (asserted by the process-level test).
- DONE xw-exit graphical exit dialog (layer-shell modal overlay,
  exclusive keyboard, arrows/Enter/Escape/per-button hotkeys, mouse
  hit-testing, ctl wiring for all six actions).
- PART Ctrl+Alt+Delete fires the exit-dialog action; the dialog
  appears only if `cmd_exit` resolves to the xw-exit binary in the
  compositor's actions.conf search path (default shipped config does).
- PART session restart path (re-exec) implemented but not covered by
  an automated test.
- TODO session save/restore, screen lock, idle actions.

## M7 — Panel / desktop
- DONE xw-panel v0: layer-shell top bar (exclusive zone, windows never
  render under it), workspace switcher (ext-workspace, click to
  switch), tasklist (wlr-foreign-toplevel: click = activate,
  middle/right click = close, active/minimized state), clock (HH:MM,
  per-minute redraw), launcher button (terminal via ctl `run`), exit
  button (session exit dialog via ctl `exit-dialog` — the same
  XW_ACTION_EXIT_DIALOG behavior as Ctrl+Alt+Del). One process, one
  surface, libxwcl + bitmap font, no toolkit.
- DONE libxwcl tasklist/workspace bindings (xwc_tasklist,
  xwc_wspaces) with lazy manager binding (eager binding leaked
  new_id announcement proxies in clients that never used them —
  LSan-caught); xwc_dispatch gained a real poll timeout (panel
  clock ticks) and explicit flushing (wl_display_dispatch flushed
  implicitly; poll() does not).
- PART launcher v0 is a single terminal button (no menu, no
  .desktop parsing, no icons — text labels only); single output;
  fixed plugin order; no plugin API.
- TODO panel plugin API, notification daemon, desktop icons,
  wallpaper, settings GUI, application finder.

## M8 — XWayland compatibility
- TODO optional XWayland startup for legacy X11 apps (never a
  foundation; the core desktop does not use it).

## M9 — Hardening/quality
- DONE zero-warning builds (-Wall -Wextra -Werror, also under -O1),
  ASAN/UBSAN/LSAN-clean suite + process tests (`make asan`),
  scoped child reaping (compositor only reaps what it spawned),
  regression tests for every fixed bug (see TESTING.md).
- TODO fuzz protocol parsing, sanitizer CI job, performance
  benchmarks, security review (see SECURITY.md).

## Client library (libxwcl)
- DONE connection + global binding (pumped sync for in-process
  tests), shm double-buffered toplevel windows, layer-shell surfaces
  (incl. set_layer), client-side xkb, input callbacks, bitmap-font
  drawing helpers, surface/xdg_surface/toplevel accessors for
  activation and popup parents.
- TODO popup windows helper, activation token helper, data-device
  (clipboard) helper for clients.
