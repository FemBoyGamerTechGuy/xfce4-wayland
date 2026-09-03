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
  |  | shm buffers, |  |  nested wayland / nested x11       |     |
  |  | damage,      |  |  drm/kms (real TTY sessions)       |     |
  |  | outputs,     |  '------------------------------------'     |
  |  | renderer     |  .------------------------------------.     |
  |  | (pixman)     |  | seat providers (libseat /          |     |
  |  '--------------'  |  seatd client / direct VT)         |     |
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
`libxwcl`. The panel is its own subproject (`subprojects/panel/`); the
component map and dependency contract live in
[subprojects/README.md](subprojects/README.md).

## Component independence (the architecture invariant)

**xw-compositor is a general-purpose Wayland compositor/WM platform.**
The XFCE desktop is one client ecosystem on top of it — never a
compile-time dependency. The build enforces the direction:

- `make compositor` succeeds with `subprojects/` and `src/clients/`
  deleted: the binary references no panel, session or dialog.
- `make panel` compiles **zero** `libxw` objects: the panel links
  `libxwcl` + the tiny ctl client and talks to the compositor purely
  through protocols (`wl_seat`, `zwlr_layer_shell_v1`,
  `wlr-foreign-toplevel-management`, `ext-workspace`). The
  build-regression suite audits the link lines for exactly this.
- Cross-component *actions* (launch a terminal, open the exit dialog)
  go through the session manager's private ctl socket — a 0700
  unix-socket line protocol — never `system()`, never another
  component's internals.
- Where the compositor and a client both need the same fact (the list
  of common terminal emulators, for the launcher and the
  terminal shortcut), each component keeps its own copy: sharing the
  code would create the very compositor-to-client link the boundary
  forbids. Facts are duplicated; behavior is not.

Using the platform without the XFCE desktop:

- `build/bin/xw-compositor -B drm` — bare KMS session, nothing else
  (kiosk, testing, embedding).
- `xw-session --no-panel` — full session lifecycle, no bar.
- `$XW_PANEL_CMD=<your-client>` — replace the panel with any
  layer-shell client of your own; the session starts it instead.
- The nested Wayland/X11 backends run the compositor as a window in
  another desktop — the development loop for custom shells and
  alternative panels.

WM policy (stacking, tiling, workspaces) is compositor-side data and
configuration (`rules.conf`, actions/shortcuts conf), not client
business: replacing the panel never changes window management, and a
different WM policy is a `libxw` change, reachable without touching
any desktop component.

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
- **X11 event delivery (nested backend)**: polling only the X
  connection fd is NOT sufficient, for two independently reproducible
  reasons (see `xb_watchdog` in `src/libxw/xw-backend-x11.c` and
  `tests/fdtest2.c`): the X server defers flushing event batches of a
  connection that recently carried large requests (our XPutImage
  presents — the map-time structure events then sit server-side while
  the fd stays silent), and Xlib's `_XReply` read-ahead drains the
  socket into its own internal queue after any round trip. The
  backend therefore also runs a 50ms watchdog that performs one XSync
  round trip (the reply forces the server to flush the connection)
  and drains Xlib's queue; the fd callback keeps instant delivery
  when the server flushed on its own. This is the same reasoning
  behind GTK's X11 backend polling XPending from its event-loop check
  phase instead of trusting select().
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
  ext-workspace exports them to clients (panel); per-toplevel workspace
  membership rides on our companion protocol
  **xw-workspace-info-v1** (`protocols/xw-workspace-info-v1.xml`): it
  annotates wlr foreign-toplevel handles with `workspace(index)` events
  (−1 = sticky), which is exactly what a graphical pager needs — read
  the wire, never compositor memory.
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

Protocol contract notes learned the hard way (all regression-tested):

- wl_buffer lifetime: the compositor keeps rendering from the attached
  wl_buffer until the next commit swaps it, and the server-side
  wl_shm_buffer dies with the wl_buffer resource. Clients must not
  destroy a buffer that is still the surface's committed content —
  libxwcl "retires" the previous pool on resize and destroys it only
  after the replacement buffer has been committed.
- layer surfaces anchored to opposite edges derive their size from
  the output; when the output geometry changes the compositor sends a
  fresh configure so anchored clients recommit at the new size (the
  panel always spans the actual output).
- the wl_output announcement is not one-shot: the compositor
  re-announces on mode changes (nested resize), and client listener
  state must live as long as the proxy.

One process, one layer-shell surface: a top or bottom bar anchored
LEFT|RIGHT (compositor dictates the width) with an exclusive zone equal
to its height, so windows can never render under it. The v1 module
map (all in `subprojects/panel/`, archived as `libpanelcore.a` so the
test suite unit-tests the modules directly):

```
xw-panel.c      core: metrics, regions, input routing, main loop
panel-apps.c    XDG .desktop database (discovery, filtering, categories,
                Exec parsing per the spec, terminal strategy, search)
panel-menu.c    the applications menu: two-pane popup (categories |
                applications), search, scroll, favorites
panel-clock.c   clock format engine + the calendar popup
panel-pager.c   the graphical workspace pager (miniature desktops)
panel-taskbar.c the window-button overflow list
panel-config.c  the panel.conf INI reader + defaults
panel-util.c    shared trace/ctl/time/metrics/text helpers
```

**Layout** is metric-driven, not pixel-hardcoded: the bar height
derives from the output's *logical* size (auto mode, 30 px at 720p
class up to 52 at 4K; `height=` overrides) so it stays proportional
when the compositor exposes a scale; fonts pick the 16 or 24 px
raster, icons and paddings derive from the height. The region order
is the XFCE one — `[Start|launchers] [taskbar…] [pager][clock][Exit]`
— with the taskbar consuming the flexible middle; overlap is
impossible by construction (one flat widget array laid out from both
ends). Region order and bar height are verified by pixel scans at
720p/1080p/1440p/4K in the test suite.

**Data sources** stay pure protocol: windows and titles via
wlr-foreign-toplevel (click active = minimize, click other = focus,
middle/right = close), workspaces via ext-workspace, per-window
workspace membership via xw-workspace-info-v1. Application metadata
comes from the freedesktop desktop-entry files; icons from the XDG
icon theme search (PNG through optional libpng, XPM in-house, SVG
skipped — a documented deviation) with procedural fallbacks so the
panel never depends on an icon theme being installed.

**Popups** (menu, calendar, overflow list) share one pattern: an
xdg_popup parented to the bar through `zwlr_layer_surface.get_popup`,
a positioner anchored to the button (upward-opening on bottom bars),
configure→draw→grab ordering, Escape/outside-press dismissal through
the seat grab, and a same-click reopen guard for the toggle.

`libxwcl` exposes the manager bindings (`xwc_tasklist`,
`xwc_wspaces`) with lazy proxy binding — binding a manager immediately
materializes `new_id` announcement proxies (workspace group + handles,
window handles), so clients that never create these objects never pay
for them (an eager version leaked 5 proxies per client, caught by
LSan). The panel main loop dispatches with a 1-second timeout ceiling:
the poll timeout exists precisely so timer-driven redraws (the clock)
can fire between server events. Launch actions go through the
session's ctl `run` wire (parsed argv, shell-quoted, forked) so no
click can ever block the dispatch loop.

## Seat providers and the real DRM/KMS session

The compositor core never opens a display or input device itself: the
DRM backend and the libinput source both acquire everything through
the seat/session abstraction in `src/libxw/xw-session-seat.c`.

```
  xw-compositor (-B drm)
    |
    |  xw_seat_session_open()  (capability probing, honest failures)
    v
  +--------------------+------------------+-----------------------+
  | libseat (optional) | seatd client     | direct VT session     |
  | wraps logind /     | wire protocol    | /dev/tty + KD_GRAPHICS|
  | elogind / seatd    | over unix socket | + VT_PROCESS switching |
  +--------------------+------------------+-----------------------+
    | open_device() returns fds (SCM_RIGHTS / ACL-granted opens)
    v
  DRM backend (xw-backend-drm.c)        libinput source
    card discovery, connectors,          udev discovery; every device
    modes, dumb-buffer scanout,           open goes through the seat
    page flips, hotplug, master           (open_restricted hook)
```

Design rules the module enforces:

* **No provider is hard-coded.** Selection order (AUTO): libseat, the
  built-in seatd client, the direct VT provider. Each failure is
  logged; total failure produces one combined diagnostic. An explicit
  `--seat-provider` never falls back.
* **The seatd client is plain libc.** The wire protocol (seatd 0.9:
  `OPEN_SEAT` handshake, `OPEN_DEVICE` with fds via `SCM_RIGHTS`,
  `DISABLE_SEAT` -> client ack -> `ENABLE_SEAT` for VT switches,
  error transport with errno values) is implemented directly, and the
  test suite cross-validates the mock server against *upstream
  libseat's own client* so both speak provably the same protocol.
* **The direct provider is the classic TTY path.** It takes over the
  controlling terminal (`KD_GRAPHICS`, `VT_PROCESS` with SIGUSR1/
  SIGUSR2 delivered through the event loop's signal sources, so the
  release handler runs in normal, async-signal-safe context), and
  opens devices with the permissions the login already granted —
  logind/elogind ACLs on the active session, or video/input groups
  on traditional setups. It never chmods anything and never asks for
  root.
* **Session switching is uniform.** Whatever the provider, the
  compositor sees `disable` (release scanout: drop DRM master,
  suspend libinput) followed by an ack (`xw_seat_session_ack_disable`)
  and later `enable` (re-acquire master, repaint everything). The
  DRM backend implements the two halves; the panel and clients are
  unaffected.

The DRM backend itself follows the same pattern as the nested
backends (one `xw_output` per connector, present() from the repaint
path), with KMS specifics: `/dev/dri/card*` enumeration (no hardcoded
card, prefers a card with connected monitors), CRTC planning that
reuses the firmware's encoder/CRTC pairing, the connector's preferred
mode, two dumb buffers per output with page-flip event pacing (a
frame arriving while a flip is in flight is parked and flips on
vblank), udev hotplug monitoring (connector removal tears the output
down honestly; live modesets of newly plugged monitors are a roadmap
item, logged as such), full CRTC restoration on every exit path —
normal, signal, and initialization failure — and a fallback to
immediate buffer updates on drivers that reject page flips (logged,
not silent). Pure planning logic (mode choice, connector naming,
CRTC assignment) lives in DRM-independent functions covered by the
regular test suite; the ioctl paths are the manual hardware
checklist's job (TESTING.md).

## Session lock (ext-session-lock) and idle (ext-idle-notify)

The lock is a security boundary, so every guarantee is enforced by the
compositor and never delegated to the client:

- **Render gate.** While a lock is engaged (from the `lock()` request —
  PENDING counts — until `unlock_and_destroy`, or indefinitely after
  the lock client dies), `xw_render_output()` draws ONLY an opaque
  blank plus the lock surfaces plus the software cursor. No window,
  layer, popup or snap-preview pixel can leak; the regression suite
  scans every output pixel for window content while locked, not just
  spot-checks. The input gate matches: `surface_at()` hit-tests only
  lock surfaces, keyboard focus is pinned to them, and the shortcut
  engine / interactive move/resize / popup dismissal are skipped — a
  shortcut could otherwise spawn a shell and bypass the lock.
- **`locked` ordering.** The spec forbids sending `locked` before a
  locked frame has been presented. The event is flushed from the
  post-present hook (`xw_session_lock_after_present`, called at the end
  of every repaint cycle) once every output is covered by a committed
  lock surface — or when the grace timer expires ($XW_LOCK_TIMEOUT_MS,
  default 1000 ms) with the blank frame as the locked frame. Obscuring
  content from `lock()` onward (before the event) is strictly safer
  than the reverse and allowed.
- **Client death.** A lock client that dies while locked leaves the
  session locked: the gate stays, all outputs blank, and a NEW client
  may `lock()` and take over (the documented recovery path; the
  takeover is a normal PENDING lock). A client that dies while PENDING
  (never received `locked`) releases the gate — nothing was locked.
- **Object lifetimes.** Per protocol, lock surface objects outlive the
  lock object ("existing objects created through this interface remain
  valid"): the lock struct turns into a zombie (res == NULL) that its
  last lock surface frees. This matters during client-death teardown:
  libwayland destroys resources in wl_map id order, so the wl_surface
  (lower id) dies before ext_session_lock_v1 — the seat focus must be
  dropped by the surface destructor, not the lock destructor, or the
  later refocus dereferences freed memory (found by ASan, fixed by
  clearing kb/ptr focus in the unmap path).
- **Strictness.** All protocol errors are implemented: invalid_destroy,
  invalid_unlock, role, duplicate_output, already_constructed,
  commit_before_first_ack, null_buffer, dimensions_mismatch and
  invalid_serial (the acked configure's own dimensions are enforced on
  every commit, tracked through a small serial->size history ring so
  acking an older-but-unacked configure is honored exactly).

idle-notify keeps a last-activity timestamp per seat (updated by every
input entry point, injected input included) and one event-loop timer
per notification armed for the remaining window. Two subtleties:
`wl_event_source_timer_update(src, 0)` DISARMS a timer rather than
firing it — notifications whose deadline already elapsed (created after
the seat went idle) must use a 1 ms floor; and `get_input_idle_notification`
(v2) is accepted with semantics identical to `get_idle_notification`
because this compositor has no non-input activity source (no sensors).

The client side is `libxwcl`'s `xwc_lock`/`xwc_idle` plus the
`xw-lock` binary: a lock screen drawn with the shared bitmap font, a
passphrase prompt with masked input and constant-time comparison,
wrong-passphrase feedback, unlock via `unlock_and_destroy` followed by
a `wl_display` roundtrip (the protocol's note about exiting right after
unlocking), and `--idle SECONDS` auto-lock driven by an idle
notification. Killing the lock client is deliberately NOT an unlock —
the session stays locked; only the passphrase (or session termination)
unlocks. Authentication is v0-honest: a local passphrase file
($XW_LOCK_PASSPHRASE_FILE or ~/.config/xfce4-wayland/lock-pass); PAM is
roadmapped. xw-lock refuses to start without a passphrase file — a
lock that can never be unlocked is worse than no lock.

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
