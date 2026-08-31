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
- **Backend abstraction** (`struct xw_backend`): owns outputs and the
  input event source. The headless backend is complete and is what the
  test suite and CI use; DRM/KMS and nested backends are roadmap items.
  This is the same seam where GPU rendering would be introduced.
- **Buffer management**: wl_shm (and single-pixel-buffer) today; linux-dmabuf
  is a roadmap item.
- **Rendering**: pixman-based compositor with damage tracking, integer
  output scale, software cursor. Simple, correct, GPU-independent.
- **Input**: injected through the backend interface (headless backend
  exposes a test API; a libinput backend is a roadmap item), then
  processed by a real xkbcommon state machine before dispatch.

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
2. `xw_shortcuts` translates keycode → keysym (no mods) and matches a
   binding table parsed from `shortcuts.conf` (INI; format documented in
   `data/shortcuts.conf` and `docs/shortcuts.5.md`).
3. Matching is "modifier-exact": a binding for `Super+D` does not fire on
   `Super+Shift+D`; NumLock/CapsLock are masked per XKB conventions.
4. A consumed key is **not** delivered to the focused client.
5. Bindings dispatch onto the **actions bus** (`xw_actions`): a table of
   named actions (workspace-switch, window-close, app-launch, ...) bound
   to handlers installed by the shell. Every action is testable without
   a GUI.
6. Conflict detection runs at load time: duplicate bindings are reported,
   the first wins, and `xw-shortcuts check` (test API) lists them.
7. XFCE's default binding set (from xfwm4/xfce4-settings defaults) is the
   shipped default table.

## Session and power management

`xw-session` is intentionally not a Wayland component. It:

- creates the session runtime directory (`$XDG_RUNTIME_DIR/xw-session.<pid>/`);
- launches and supervises `xw-compositor`, `xw-panel`, and autostart
  entries (XDG autostart `.desktop` files, with `OnlyShowIn` filtered to
  `XFCE`/`xfce4-wayland`);
- exposes a **control socket** (`ctl.sock`) with a tiny line protocol
  (`LOGOUT`, `RESTART`, `SHUTDOWN`, `REBOOT`, `SUSPEND`, `HIBERNATE`,
  `LOCK`, `PING`), used by `xw-exit`, `xw-session-ctl`, the panel, and
  the test suite;
- on power actions, invokes `loginctl <action>` (works with both logind
  and elogind's loginctl); if absent, reports a clean error to the
  requester instead of failing silently.

No D-Bus linkage, no daemon requirement. This is deliberately
conservative: `systemd-logind`/`elogind` remain optional integrations,
probed at runtime.

The graphical exit path is `xw-exit` (a native Wayland dialog) wired to
`Ctrl+Alt+Delete` by default (XFCE parity) plus a panel button.

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
