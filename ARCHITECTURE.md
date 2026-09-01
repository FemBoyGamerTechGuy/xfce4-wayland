# Architecture

## Bird's-eye view

```
                  Wayland protocol boundary
  .---------------------------------------------------------------.
  |                      xw-compositor (server)                   |
  |  .--------------.  .--------------.  .----------------------.  |
  |  | protocol     |  | window       |  | input                |  |
  |  | frontends    |  | management   |  |  seat, xkb state,    |  |
  |  | (xdg-shell,  |  |  workspaces, |  |  shortcut engine     |  |
  |  | layer-shell, |  |  stacking,   |  |  (actions bus)       |  |
  |  | activation,  |  |  focus,      |  |  pointer interaction |  |
  |  | toplevel-mgmt|  |  snap/tiling,|  |  drag & drop         |  |
  |  | data-device) |  |  rules)      |  |                      |  |
  |  '--------------'  '--------------'  '----------------------'  |
  |  .--------------.  .------------------------------------.     |
  |  | libxw core   |  | backends                           |     |
  |  | surfaces,    |  |  headless (tests, CI, dev)         |     |
  |  | shm buffers, |  |  (future: drm/kms, wayland-nested) |     |
  |  | damage,      |  '------------------------------------'     |
  |  | outputs,     |  .------------------------------------.     |
  |  | renderer     |  | renderer: pixman software          |     |
  |  | (pixman)     |  '------------------------------------'     |
  |  '--------------.                                        |     |
  '---------------------------------------------------------------'
          ^                                   ^
          | Wayland (layer-shell, toplevel    | Wayland (xdg-shell)
          | mgmt, ext-workspace)              |
   .---------------.                  .------------------.
   | xw-panel      |                  | apps / xw-exit / |
   | (desktop UI)  |                  | xw-demo          |
   '-------+-------'                  '------------------'
           | unix ctl socket (private, tiny, line protocol)
   .--------------------------------------------------.
   | xw-session: launch, supervise, autostart, exit, |
   | power actions (logind via loginctl CLI)         |
   '--------------------------------------------------'
```

The compositor is a library (`libxw`) plus a thin binary; the session
manager is a separate, library-free process that supervises everything.
Desktop components (panel, dialogs) are native Wayland clients built on
`libxwcl`.

## Why not wlroots

wlroots is a competent compositor building block, but it is explicitly
out of scope for this project (see the project specification). We
therefore own the following responsibilities directly:

- **Display/event integration**: `wl_display` + `wl_event_loop` from
  libwayland-server; our backend interface drives frame scheduling.
- **Backend abstraction** (`struct xw_backend` + ops vtable): backends
  hand composited frames to a display via the `present` op and pump
  input events into the seat. Three exist today: headless (tests, CI),
  **nested Wayland** (the whole desktop as a window in a parent
  compositor, implemented as a libxwcl client with its socket
  multiplexed on our own event loop) and **nested X11** (a top-level
  X window; XPutImage from the native pixman buffer, X keycodes are
  evdev+8). DRM/KMS is the roadmap item; this is the same seam where
  GPU rendering would be introduced.
- **Buffer management**: wl_shm (and single-pixel-buffer) today; linux-dmabuf
  is a roadmap item.
- **Rendering**: pixman-based compositor with damage tracking, integer
  output scale, software cursor. Simple, correct, GPU-independent.
- **Input**: injected through the backend interface (headless exposes a
  test API; nested backends forward real parent events: evdev keycodes
  and Wayland button codes verbatim; a libinput backend is a roadmap
  item), then processed by a real xkbcommon state machine before
  dispatch.

Owning these is more work, but it is the point of the exercise: an XFCE
experience on an architecture we fully understand and control.

## Window management model

`xw_wm` is the window manager. All toplevels (xdg-shell) and layer
surfaces live in one stacking tree per output group:

- **Windows** carry: geometry (server-side canonical), title/app_id,
  workspace id (or sticky), states (maximized/fullscreen/minimized),
  restore geometry, and an xdg-shell configure state machine (states are
  only applied on `ack_configure` — never assumed).
- **Workspaces** are virtual desktops with a count, names, and per-output
  visibility. Only windows on the active workspace are mapped/rendered.
  ext-workspace exports them to clients (panel).
- **Focus** follows the XFCE default: click-to-focus, focus-on-map
  (activatable), with Alt+Tab / Alt+Esc MRU cycling. The keyboard focus
  is per-seat and drives wl_keyboard enter/leave.
- **Stacking**: three layers — background/bottom (layer-shell), normal
  toplevels, top/overlay (layer-shell) — each stacked in interactive
  order (top of interaction stack = most recently used within the layer).
- **Snapping/tiling**: edge snapping during interactive move (XFCE
  behavior): dragging to a screen edge previews a half/quarter tile;
  keyboard bindings tile directly (Super+Left/Right/Up/Down).
- **Window rules** (`rules.conf`): match on app_id/title with shell
  patterns; actions: workspace, maximize, fullscreen, focus/steal,
  no-focus. Applied at map time.

## Keyboard shortcuts (first-class subsystem)

This subsystem is a project priority. Design:

1. The seat's xkbcommon state tracks modifiers precisely; key events
   arrive as (keycode, press/release) from the backend.
2. `xw_shortcuts` translates keycode → keysym (with live modifiers)
   and matches a binding table parsed from `shortcuts.conf` (INI;
   format documented in `data/examples/shortcuts.conf`). Keysyms are
   **canonicalized** on both sides: modifier-produced variants
   (Shift+Tab → ISO_Left_Tab, Alt+Print → Sys_Req, KP_Enter) map back
   to their physical key so bindings behave like xfwm4's
   keycode-based matcher.
3. Matching is "modifier-exact": a binding for `Super+D` does not fire on
   `Super+Shift+D`; NumLock/CapsLock are masked per XKB conventions.
4. A consumed key is **not** delivered to the focused client (its
   release is suppressed too — the same bitmap covers shortcut and
   interactive move/resize consumption).
5. Bindings dispatch onto the **actions bus** (`xw_actions`): a table of
   named actions (workspace-switch, window-close, app-launch, ...) bound
   to handlers installed by the shell. Every action is testable without
   a GUI.
6. Conflict detection runs at load time: duplicate bindings are reported,
   the first wins, and diagnostics are logged with the offending
   binding strings.
7. XFCE's default binding set (from xfwm4/xfce4-settings defaults) is the
   shipped default table; a table-driven test presses **every** default
   binding through the real key pipeline.
8. Key repeat follows the Wayland protocol: the seat advertises
   rate/delay via `wl_keyboard.repeat_info` and clients repeat
   themselves; only interactive keyboard move/resize keys are
   repeated server-side (held arrows keep moving windows). Backends
   that receive synthetic repeats (X11 autorepeat) filter them.

## Session and power management

`xw-session` is intentionally not a Wayland component. It:

- creates the session runtime directory (`$XDG_RUNTIME_DIR/xw-session.<pid>/`);
- launches and supervises `xw-compositor` (passing the user config
  directory `$XDG_CONFIG_HOME/xfce4-wayland`, created on first run —
  user INI configuration takes effect inside sessions), `xw-panel`,
  and autostart entries (XDG autostart `.desktop` files, with
  `OnlyShowIn` filtered to `XFCE`/`xfce4-wayland`);
- exposes a **control socket** (`ctl.sock`) with a tiny line protocol
  (`status`, `ping`, `power-status`, `logout`, `restart`, `shutdown`,
  `reboot`, `suspend`, `hibernate`, `exit-dialog`, `run CMD`), used by
  `xw-exit`, `xw-session-ctl`, the panel, and the test suite;
- spawns runtime children on request (`exit-dialog` runs the same
  command as the compositor's XW_ACTION_EXIT_DIALOG — $XW_EXIT_CMD
  overrides, default `xw-exit`; `run` executes a command through
  `/bin/sh -c`) and supervises them like autostart: SIGTERM at
  shutdown, reaped by the SIGCHLD loop (bounded table);
- on power actions, delegates to the shared **power backend**
  (`xw-power`): fork + execvp of `loginctl <action>` with a fixed
  argv (no shell, no injection surface), stderr captured into the
  error reply. Works with both logind and elogind.

The power backend also **probes capabilities** with honest reasons:
loginctl liveness (a loginctl binary without a running daemon reads
as unavailable — probed by running it), kernel sleep modes from
`/sys/power/state`. `power-status` reports the full capability line;
the exit dialog renders unavailable actions dim with their reason
("Suspend unavailable: no working logind/elogind (loginctl)") and
refuses to activate them.

No D-Bus linkage, no daemon requirement. This is deliberately
conservative: `systemd-logind`/`elogind` remain optional integrations,
probed at runtime. A direct D-Bus `Can*` query is a documented roadmap
refinement, not a requirement (see DEPENDENCIES.md).

The graphical exit path is `xw-exit` (a native Wayland dialog) wired to
`Ctrl+Alt+Delete` by default (XFCE parity) plus a panel button.

## Real input (libinput source)

Input is a **source**, deliberately orthogonal to the output
backends: `xw-input-libinput` turns device events into the same seat
calls the injection API uses (one pipeline for real, nested and
synthetic input — that is what makes the test coverage meaningful).

- **udev seat mode** (real sessions): libinput owns discovery and
  hotplug via udev; requires a running udev instance.
- **path mode** (`$XW_INPUT_DEVICES=/dev/input/event3:...`):
  explicit devices, deterministic, no udev — the input half of a
  headless debugging session.
- The compositor's `-I/--input auto|libinput|none`: AUTO (default)
  never touches system devices unless the device list is explicit,
  and nested/X11 backends always inherit input from the parent
  session instead — tests stay deterministic on any machine.
- Translation (clamping to the output layout, sub-pixel accumulation,
  absolute→layout mapping, v120 high-res wheel handling) lives in
  small handlers that the test suite drives directly; the thin
  libinput_event decoder is the only hardware-only code (Level 3,
  see TESTING.md). Pointer acceleration comes from libinput itself.
- Key repeat is never done by libinput or the module: clients repeat
  via repeat_info; the seat repeats interactive-move keys (see the
  shortcuts section above).

## The panel (xw-panel)

One process, one layer-shell surface: a top bar anchored LEFT|RIGHT
(compositor dictates the width) with a fixed height and an exclusive
zone of the same value, so windows can never render under it. The
plugins of v0, left to right: a terminal launcher (ctl `run`), the
workspace switcher (ext-workspace handles, click to switch), the
tasklist (wlr-foreign-toplevel handles: click activates, middle/right
click closes), then right-aligned clock (HH:MM, redrawn when the minute
flips) and exit button (ctl `exit-dialog`). Layout is recomputed on
every tasklist/workspace change and on configure; input is plain
hit-testing against the button table. `libxwcl` exposes the two manager
bindings (`xwc_tasklist`, `xwc_wspaces`) with lazy proxy binding —
binding a manager immediately materializes `new_id` announcement
proxies (workspace group + handles, window handles), so clients that
never create these objects never pay for them (an eager version leaked
5 proxies per client, caught by LSan). The panel main loop dispatches
with a 1-second timeout ceiling: the poll timeout exists precisely so
timer-driven redraws (the clock) can fire between server events.

## Surfaces, buffers, damage

wl_shm buffers are mapped server-side and wrapped in pixman images.
`wl_surface.commit` applies pending state atomically (attach/damage/
opaque region). Damage accumulates into a pixman region per surface and
per output; `xw_output_repaint()` recomposites dirty regions, flushes
frame callbacks, and updates the backbuffer. The renderer is order:
background color, background/bottom layers, toplevels (bottom → top of
the visible stack), top/overlay layers, then the software cursor.

## Configuration persistence (xfconf-like)

XFCE semantics, X11-free implementation: INI files under
`$XCFG_CONFIG_HOME/xfce4-wayland/` (channel-per-file). v0 channels:
`xw-compositor.conf`, `shortcuts.conf`, `rules.conf`, `session.ini`,
`workspaces.conf`. A settings daemon broadcasting changes over a
standard protocol is a roadmap item; today components reload on SIGHUP
or restart, and `xw-session restart` applies configuration changes.

## Headless-first development and testing

The headless backend + software renderer means the whole desktop runs
and is testable without any GPU or seat hardware. The test suite embeds
the compositor in-process, connects real clients over a unix socket,
injects input through the backend test API, and asserts on both the
internal state (window manager) and observable output (rendered pixels,
protocol events clients received). See [TESTING.md](TESTING.md).
