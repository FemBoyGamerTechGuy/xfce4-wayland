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
- DONE build hardening (Phase 2.5): feature toggles (XW_X11/XW_LIBINPUT
  = auto/1/0 with actionable diagnostics), PROFILE presets
  (release/debug/asan) with a stale-tree guard, required-dependency
  validation, install/uninstall with configurable prefix + DESTDIR,
  `make config` summary, wayland-sessions .desktop entry, example
  configs; zero-root verified end to end (BUILDING.md).

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
- DONE real-input source (Phase 3 groundwork): libinput-backed input
  module orthogonal to the output backends — udev seat mode (device
  discovery + hotplug, for real sessions) and path mode
  ($XW_INPUT_DEVICES, deterministic testing; also drives headless
  debugging from real keyboards). `-I/--input auto|libinput|none`;
  AUTO never touches system devices unless explicitly opted in.
  Translation pipeline (clamping, sub-pixel accumulation, abs→layout,
  v120 wheels) covered white-box at Level 1; the thin libinput_event
  decoder is Level 3 (TESTING.md).
- DONE seat/provider abstraction (Phase 4, `xw-session-seat.c`):
  libseat (optional external), a built-in seatd wire-protocol client
  (plain libc; cross-validated against upstream libseat in the test
  suite), and a direct-VT provider (KD_GRAPHICS + VT_PROCESS, devices
  via login ACLs or groups). Capability probing with honest combined
  diagnostics; `--seat-provider`/`$XW_SEAT_PROVIDER` override; session
  disable/ack/enable lifecycle uniform across providers.
- DONE DRM/KMS backend (Phase 4, `xw-backend-drm.c`): device
  discovery (no hardcoded card), connector/mode/CRTC enumeration via
  pure testable planning functions, preferred-mode modesets,
  double-buffered dumb scanout with page-flip event pacing (immediate
  fallback on flip-rejecting drivers, logged), udev hotplug
  (connector removal), DRM master drop/re-acquire across VT switches,
  full CRTC restoration on every exit path. Input devices open
  through the seat provider. Backend selection in xw-session
  (--backend=drm|x11|wayland|headless, TTY+KMS auto-detection,
  explicit drm never falls back). ioctl paths verified by the manual
  hardware checklist (TESTING.md); GL/EGL rendering is a later
  accelerator.
- TODO presentation-time feedback; atomic modesetting; live modeset
  of newly plugged monitors; hardware cursor planes.

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
- DONE keysym canonicalization: modifier-produced variants
  (Shift+Tab→ISO_Left_Tab, Alt+Print→Sys_Req, KP_Enter) match their
  physical key like keycode-based matchers do — without it
  Alt+Shift+Tab / Alt+Print bindings could never fire.
- DONE key repeat, protocol-correct: wl_keyboard.repeat_info
  (rate/delay, keyboard.conf + env overrides; 500ms/30Hz XFCE
  defaults) lets clients repeat themselves; the server repeats ONLY
  interactive keyboard move/resize keys; the X11 backend filters the
  X server's synthetic repeats so keys are never delivered twice.
- DONE full default-table coverage test (47/47 bindings dispatched
  through the real key pipeline, modifiers as real keys, keypad
  bindings under NumLock).
- TODO touch input.
- TODO xkb per-seat runtime layout switching.

## M4 — Desktop integration protocols
- DONE layer-shell (server): set_size/set_anchor/margins/exclusive
  zones → wm usable area, set_layer restacking, keyboard interactivity
  (exclusive + on-demand).
- DONE xdg-activation (tokens, single-use, focus handover).
- DONE wlr-foreign-toplevel-management (handles, state, requests).
- DONE ext-workspace (group + workspace handles, activate switching).
- DONE ext-session-lock-v1 (server): full state machine (PENDING ->
  ACTIVE on a presented locked frame, unlock, denial, client death
  keeps the session locked with a takeover path for a new client,
  timeout fallback when the client never commits). Security gates are
  server-enforced: while locked ONLY lock surfaces render (opaque
  blank, pixel-verified) and receive input (shortcuts dead, windows
  silent); strict protocol errors (commit-before-ack, null buffer,
  dimensions mismatch, invalid serial, duplicate output, role,
  already-constructed, invalid destroy/unlock) all implemented and
  tested; output resize reconfigures lock surfaces to the exact new
  size.
- DONE ext-idle-notify-v1 (server, v2): per-notification event-loop
  timers over seat activity timestamps; idled/resumed with re-arm;
  get_input_idle_notification accepted (identical semantics here: the
  only activity source is input — no sensors exist).
- DONE xw-lock client: ext-session-lock + idle client support in
  libxwcl (xwc_lock/xwc_idle) and a real lock screen binary (prompt,
  masked input, constant-time passphrase compare, wrong-passphrase
  feedback, unlock + roundtrip; --idle SECONDS auto-lock via
  ext-idle-notify; killed lock clients leave the session locked by
  design). Ctrl+Alt+L (default shortcut) already spawns it via the
  pre-existing `lock` action.
  Gaps (honest): authentication is a local passphrase file
  (development-grade, NOT PAM — see ROADMAP backlog "PAM unlock");
  libxwcl tracks a single output, so xw-lock covers one output (the
  server handles any number); lock surfaces use the same shm-only
  buffer path as every other client.
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
- DONE xw-session-ctl: status/ping/power-status/logout/restart/
  shutdown/reboot/suspend/hibernate/exit-dialog/run commands. Runtime
  spawns (exit dialog, `run CMD`) are supervised session children:
  SIGTERM at shutdown, reaped by the SIGCHLD loop.
- DONE power backend (xw-power): loginctl liveness probe (works with
  systemd-logind and elogind; a loginctl without a running daemon
  reads as unavailable), kernel sleep-mode probing, capability report
  with human-readable reasons, execution via fork+execvp (no shell)
  with the backend's stderr captured into error replies. xw-session
  passes the user config dir to the compositor (INI config actually
  takes effect in sessions now).
- DONE xw-exit graphical exit dialog (layer-shell modal overlay,
  exclusive keyboard, arrows/Enter/Escape/per-button hotkeys, mouse
  hit-testing, ctl wiring for all six actions); unavailable power
  actions render dim with their reason and cannot be activated.
- PART Ctrl+Alt+Delete fires the exit-dialog action; the dialog
  appears only if `cmd_exit` resolves to the xw-exit binary in the
  compositor's actions.conf search path (default shipped config does).
- PART session restart path (re-exec) implemented but not covered by
  an automated test.
- TODO session save/restore, screen lock, idle actions, direct
  D-Bus Can* queries (currently probed via loginctl + /sys, see
  DEPENDENCIES.md "explicitly rejected" for the rationale).

## M7 — Panel / desktop
- DONE xw-panel v0: layer-shell top bar (exclusive zone, windows never
  render under it), workspace switcher (ext-workspace, click to
  switch), tasklist (wlr-foreign-toplevel: click = activate,
  middle/right click = close, active/minimized state), clock (HH:MM, display-only v0: a click is intentionally a no-op, no popup/calendar; see subprojects/panel/README.md),
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
- DONE applications menu v2 (M-panel-ux): a two-pane popup
  (categories | applications, the Whisker shape) over the XDG
  .desktop database — discovery with user-over-system shadowing and
  mtime-cached rescans, visibility rules (NoDisplay/Hidden/TryExec/
  OnlyShowIn/NotShowIn), localization (Name[lang]), XFCE-style
  categories, favorites from the config, live search (name, generic
  name, comment, id), wheel/arrow scrolling with a scrollbar, XDG
  icons, and spec-correct Exec parsing (quoting, field codes,
  Terminal=true wrapping with a per-terminal strategy) launched
  through the forked ctl-run wire.
- DONE clock + calendar (M-panel-clock): date+time through a
  locale-safe format engine (config: format, seconds); clicking the
  clock opens a one-month calendar popup — month navigation (arrows,
  wheel, Left/Right keys), today highlighted, Monday-first grid,
  Escape/outside dismissal.
- DONE graphical workspace pager (M-panel-pager): each workspace box
  renders a miniature desktop with window tiles (focused window
  accented, sticky windows dimmed on every box); the active workspace
  is outlined; membership arrives through the new
  xw-workspace-info-v1 protocol (see M6.5 below).
- DONE taskbar v2 (M-panel-taskbar): icons per window (app_id ->
  .desktop Icon -> themed icon -> letter tile), active/minimized
  distinction, XFCE minimize-on-active-click, squeeze to icon-only,
  and a "+N" overflow popup listing hidden windows.
- DONE panel layout engine (M-panel-layout): metric-driven bar
  (height auto from the output's logical size, 16/24 px font rasters,
  scale-aware icons), the XFCE region order [Start|launchers]
  [taskbar] [pager][clock][Exit], top/bottom position, panel.conf
  configuration with complete defaults.
- DONE panel hardening (M-panel-polish): direct application
  launching (posix_spawn of the spec-parsed Exec argv — no shell, no
  session relay, signal state reset, visible failure status line),
  UTF-8 text rendering with Latin coverage + safe ellipsized
  truncation (missing/clipped letters fixed), icon-theme resolution
  following Inherit= chains and the desktop's configured theme
  (GTK/XFCE), PNG decode actually wired into the build, missing-icon
  fallback glyph + one-shot miss logging; Ctrl+C verified to pass
  through to clients (never a desktop shortcut) while Ctrl+Alt+Del
  keeps opening the session action dialog.
- PART remaining limits: single output; fixed plugin order; no
  plugin API; SVG icons not renderable (no vector stack — PNG/XPM
  only, generic fallback glyph); calendar days display-only (no event
  list); menu does not cascade flyout submenus (two-pane instead);
  no recent-apps tracking.
- TODO panel plugin API, notification daemon, desktop icons,
  wallpaper, settings GUI, application finder, per-output panels.

## M6.5 — Desktop-integration protocols of our own
- DONE xw-workspace-info-v1 (M-panel-ux): a small read-only companion
  protocol annotating wlr foreign-toplevel handles with their
  workspace (events: workspace(index), done; -1 = sticky) — the panel
  pager's data source, keeping the panel a pure protocol client.
  Vendored in protocols/ (original implementation, MIT).

## M8 — XWayland compatibility
- DONE optional XWayland startup for legacy X11 apps (never a
  foundation; the core desktop does not use it): `xw-session` starts
  Xwayland rootless with `-displayfd` + a per-session authority
  cookie, supervises it (restarts, paired with its helper), exports
  `$DISPLAY`/`$XAUTHORITY` to session children. The compositor side is
  `xwayland_shell_v1` (Xwayland 24+ per-window association) mapping X11
  windows through the SAME WM/taskbar/workspace model as native ones.
  `xw-xwm` (session helper, raw X wire protocol — no Xlib/xcb) is the
  window manager Xwayland's rootless mode requires: COMPOSITE redirect,
  SubstructureRedirect on the root, geometry mirroring,
  WM_DELETE_WINDOW delivery via the private xw_window_control_v1
  protocol. Regressions: `scripts/test-xwayland.sh`,
  `scripts/test-realapps.sh`.
- DONE X11 window identity and lifecycle in the one compositor model
  (window-control v2): WM_NAME/_NET_WM_NAME titles (with
  PropertyNotify retitling), WM_CLASS res_class app-ids, ICCCM
  WM_NORMAL_HINTS constraints, override-redirect popups (X-owned
  geometry, above managed windows, no taskbar/focus — classified at
  CreateNotify, not at first draw), granted client resizes reported
  back through set_geometry (extent space: interior + X border),
  WM_DELETE_WINDOW / WM_TAKE_FOCUS delivery with correct SendEvent
  format bytes, border-aware geometry mirroring both directions.
  Regressions: tests/suite/test_xwm.c (11 tests, real Xwayland +
  real helper + controllable X11 client + the real xterm).
- DONE EWMH fullscreen for X11 apps (window-control v3): both request
  paths (runtime _NET_WM_STATE client message, map-time property),
  the compositor's one fullscreen model (per-output rectangle via the
  window's output, saved restore geometry, taskbar state),
  _NET_WM_STATE property kept in sync, pre-map requests deferred to
  just after placement. The surface-space calibration (below) keeps
  the mirror stable for both Xwayland surface conventions.
  Regressions: `xwm-fullscreen` (suite) + scripts/test-realapps.sh
  (wmctrl fullscreens the real xeyes to the exact output rectangle,
  xprop verifies the synced state).
- DONE connection-loss triage in the helper: both sockets (X and the
  compositor) are watched, wl return codes checked; both gone =
  orderly stack teardown (exit 0), one gone = that peer's crash
  reported loudly, wl protocol errors identified as compositor bugs.
  Regression: scripts/test-xwm-loss.sh.
- DONE the canonical geometry round (2026-09-05, the
  physical-NVIDIA findings): ONE model rect per window now drives
  render, hit-test, pointer-event translation AND damage for both
  window families (the XWAYLAND branch in xw_surface_get_pos was
  missing — X11 surfaces hit-tested against a phantom rect at the
  layout origin while rendering used the model: the off-center
  grab/hit area, clicks on the wrong window, X11 clients fed
  global pointer coordinates, Xwayland starved of pointer events).
  Frame callbacks now reach MAPPED toplevel surfaces (the map/unmap
  funnel maintains surface->mapped; Xwayland 24.1 presents frame N+1
  only after frame N's callback — the white/invisible frozen-window
  symptom was exactly one presented frame). State geometry
  (fullscreen/maximized) is authoritative against stale-size
  commits, state windows map at their state rect, and grants are
  distinguished from commits (xw_geom_pending) so in-flight
  pre-grant commits cannot rewind a live resize. Mirror positions
  are identity across the compositor<->X boundary (X window x/y IS
  the extent origin; only sizes convert by 2*border — the old
  +bw/-bw on positions shifted every mirrored window 1px/round).
  Instrument: XW_GEOMETRY_TRACE=1 (per-window geometry line +
  hit-test picks + the global→surface-local translation).
  Regressions: xwm-geometry-truth (the X11 battery, real
  Xwayland/helper/Xlib client) + geometry-native (native twin +
  frame-callback liveness); both verified red with the fixes
  reverted. Physical NVIDIA acceptance: the checklist in
  TESTING.md (real-hand drags, menu interaction, continuous
  repaint of frame-clocked apps) — the headless battery pins every
  geometry invariant, the physical pass remains the final
  authority and has NOT yet been re-run by hand.
- PART the measured surface-space calibration: Xwayland sizes some
  windows' wl_surfaces to the X11 interior, others to the extent
  (interior + 2*border) — both measured live with the same binary
  (x11client/xterm: extent; Xaw xeyes: interior). The helper now
  measures the convention per window and converts in that space,
  which killed a 2px-per-round ratchet on drawn interior-convention
  windows. Override-redirect windows still push the extent convention
  (no mirror feedback to calibrate against; unambiguous for the
  border-0 popups every real Xaw menu actually is).
- TODO X11 clipboard bridge (wl_data_device <-> X selections).
- TODO EWMH workspace mirroring (_NET_CURRENT_DESKTOP sync both ways);
  X-side activation channel (_NET_ACTIVE_WINDOW requests currently
  logged as unimplemented).

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
