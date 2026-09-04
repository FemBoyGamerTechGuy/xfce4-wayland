# Testing

The suite is deterministic, automated, and GPU-free: everything that
runs in CI or on a developer machine runs against the **headless
backend** with the pixman software renderer, or against a **nested
backend** inside a disposable Xvfb/parent session. Real hardware
(DRM/KMS, physical input devices) is a separate, explicitly-labeled
verification level that is never silently claimed.

## The three test levels

Development environments rarely provide DRM hardware or physical
input devices — and tests must never touch a developer's real session
or suspend a dev machine. The project therefore maintains three
strictly separated levels, and every coverage claim states which level
verified it:

```text
Level 1 — Unit / in-process integration (make tests)
    The compositor library runs inside the test binary; real Wayland
    clients connect over the socket; input arrives through the same
    seat/shortcut pipeline real input uses. Deterministic, fast,
    ASan-clean. Runs anywhere (no X, no devices, no root).

Level 2 — Process / nested integration (make check)
    Real binaries as real processes: xw-session supervising
    xw-compositor, clients as separate processes, the X11 backend
    under Xvfb (pixel round-trips + XTEST-synthesized input), the
    nested Wayland backend across two real compositor processes, the
    power backend against a fake loginctl. No root, no real devices,
    no real power state changes (a forced-unavailable environment
    keeps the suite safe on every host). Includes the build-system
    regression suite (see scripts/test-build-regressions.sh): clean
    builds on font-less systems (mount namespace), build-failure
    propagation, fail-fast dev-session behavior, and zsh/bash/dash
    execution of every entry script.

Level 3 — Real hardware / DRM-KMS (manual, documented procedure)
    Physical displays through the DRM/KMS backend and physical input
    devices through libinput. Requires hardware; nothing at this level
    is claimed as verified by Levels 1-2. Currently covers: real-input
    pipeline smoke runs via XW_INPUT_DEVICES on machines with readable
    evdev nodes. The DRM/KMS backend itself is not implemented yet
    (ROADMAP Phase 4), so Level 3 display coverage is pending by
    definition.
```

What Level 1 *cannot* cover is stated honestly: the thin
libinput_event decoder and the X server's autorepeat emission need
devices or uinput (root) — the pure logic around them
(translation handlers, repeat filtering, X11 filter) is tested at
Level 1, and the module refuses to start when its prerequisites are
absent rather than half-working.

## Layout

- `tests/harness/xwtest.h` — assert framework (`XWT_CHECK`,
  `XWT_ASSERT`, `XWT_WAIT`), test registration.
- `tests/harness/harness.c` — embeds the compositor in-process, drives
  both sides deterministically (server dispatch + client read) between
  input injections.
- `tests/harness/client.c` — shared helpers for the in-process test
  client (solid-color windows).
- `tests/suite/test_core.c` — core, WM, input/shortcut suite.
- `tests/suite/test_protocols.c` — desktop-integration protocol suite
  (layer-shell, popups, clipboard, foreign-toplevel, activation); raw
  Wayland objects are driven directly next to white-box assertions.
- `tests/suite/test_input.c` — **Level 1 input coverage**: key repeat
  (protocol values default + configured, client-side single delivery,
  server-side WM move repeat with paced real-time waits), the libinput
  translation pipeline (clamping, sub-pixel accumulation, abs→layout
  mapping, button/axis delivery) through the real handlers with
  path-mode contexts (no devices — deterministic everywhere).
- `tests/suite/test_session.c` — the graphical exit dialog as a real
  child process against the in-process compositor.
- `tests/suite/test_lock.c` — session-lock + idle-notify coverage
  (in-process white-box + libxwcl client, plus the real xw-lock binary
  as a child process: passphrase unlock flow, kill-while-locked
  security, --idle autolock; a raw wayland connection drives the
  strict protocol-error paths).
- `tests/suite/test_panel.c` — panel coverage (see below).
- `tests/suite/test_realclient.c` — real-toolkit regressions for the
  integration round: `wl_pointer.set_cursor` (the request whose NULL
  handler aborted the compositor — every real toolkit sends it),
  `wl_buffer.release` on replacement (rotating shm pools stall forever
  without it), the subcompositor lifecycle (sync/desync position
  semantics, stacking, both destroy orderings), and the slow-start
  contract (a client that idles between connect and first commit is
  never a failure).
- `tests/suite/test_xwm.c` — the X11-first-class regressions: a full
  stack per test (in-process compositor + REAL Xwayland rootless +
  the real xw-xwm helper), driven by a controllable real X11 client
  (`build/tests/x11client`, Xlib) and the real xterm. Pins:
  mask-aware ConfigureRequest parsing (the 3x14 xterm), WM_NAME /
  WM_CLASS identity over window-control v2, PropertyNotify retitling,
  X input focus mirroring, WM_TAKE_FOCUS delivery (the SendEvent
  format-byte bug), override-redirect classification and invariants
  (X-owned geometry, no taskbar/focus, move refused), WM_DELETE vs
  destroy fallback, helper teardown, the real xterm's two-phase
  resize, and the extent-vs-interior border model (the compositor
  models interior + 2*border; the helper converts). Skips (counted,
  not silent) when .apps-root is not populated.
- `tests/suite/test_backends.c` — nested backend coverage: a real
  compositor (B, nested) running inside another real compositor (A,
  headless) in one process. Asserts topology, the present pipeline
  (B's framebuffer content visible in A's pixels), clients of B
  rendering through B into A, input routing (keys injected into A
  reach B's shortcut engine when B's window is focused — parent
  shortcuts must not shadow the child desktop), and the X11
  synthetic-repeat filter logic.
- `tests/x11probe.c` + `build/tests/x11probe` — X11-backend process
  probe: finds the compositor window in a live X server, reads back
  its pixels (XGetImage) and injects XTEST keyboard input.
- `scripts/test-session.sh` — **Level 2**: process-level session
  integration (real `xw-session` + `xw-compositor` children, ctl
  socket, autostart filtering, runtime spawns, panel autostart, clean
  logout, the power backend with a fake loginctl, the X11 backend
  under Xvfb with synthesized input, the libinput real-input source
  startup (udev-seat path or its honest logged refusal),
  `xw-session --nested`, and the nested Wayland backend across two
  real processes).
- `scripts/fetch-test-apps.sh` — downloads REAL client applications
  (foot, zenity/GTK4, xterm, xeyes, Xwayland, plus their recursive
  runtime dependencies) as .debs WITHOUT root and extracts them into
  the gitignored `.apps-root/` prefix. The integration tests below
  run real toolkits against the compositor this way — synthetic
  protocol clients alone missed the entire bug class this round.
  (Debian-family apt; on other distros install the same apps and point
  `$XW_XWAYLAND_CMD` at the Xwayland binary.)
- `scripts/test-xwayland.sh` — **Level 2**: the XWayland stack without
  the session: compositor + `Xwayland -rootless` + `xw-xwm` + a real
  X11 client (xeyes). Asserts every process stays alive, the X socket
  appears, and the X11 window is MANAGED as an ordinary compositor
  window (map log with app-id `xwayland`).
- `scripts/test-realapps.sh` — **Level 2**: full-session acceptance
  with real toolkits: `xw-session` brings up compositor + XWayland +
  xw-xwm + panel; two native GTK4 apps launch and stay; an X11 app
  launches through XWayland into the SAME window list; a deliberately
  slow-starting client (3s before its first window) is not treated as
  a failure; everything stops cleanly at logout. This is the
  in-container mirror of the physical NVIDIA acceptance checklist
  below.
- `scripts/audit-interfaces.py` — static audit for the NULL-request-
  handler bug class: cross-references every protocol XML (in-repo +
  sysroot wayland-protocols) with the C interface implementations and
  fails on any request whose listener is not initialized — a NULL
  handler makes libwayland abort the whole compositor when a client
  sends that request.
- `scripts/test-build-regressions.sh` — **Level 2 (build system)**:
  regression suite for the distro-agnostic build: font generation from
  the bundled asset (determinism, stripped environment, precise
  failure diagnostics), `make` failure propagation on broken sources,
  the quick-start flow after a partial/failed build (dev-session must
  refuse), a **clean build with every system font hidden** inside an
  unprivileged mount namespace (the exact "clean distro, documented
  deps, still fails" class), shell compatibility (syntax + sourcing +
  full session runs under zsh, bash and dash), and **linker
  dependency propagation (R6)**: the final link is re-executed under
  an upstream-shaped `libinput.pc` (which hands out no `-ludev`, the
  Arch/Artix condition), `-ludev` ordering is asserted on the link
  command, `XW_LIBINPUT=1/auto/0` semantics are exercised against
  missing libudev dev files, feature switching over a populated tree
  must refuse, and a `XW_LIBINPUT=0` build is verified to exclude the
  backend from the archive and binary. Complemented by
  `scripts/test-link-deps.sh` — a symbol-coverage audit that parses
  every final executable's actual link command and fails if any
  undefined symbol of its objects/archives is not provided by a
  library on that command line (the general "DSO missing from command
  line" catcher).
- `scripts/run-asan.sh` — full sanitizer regression pass (ASan + UBSan
  + LeakSanitizer) including the process-level test; restores the
  release build afterwards.

## Running

    make tests        # Level 1: builds and runs the in-process suite
    make check        # Level 1 + Level 2 (session test + build regressions)
    make asan         # full sanitizer pass (rebuilds, tests, restores)

Filtering tests (triage):

    XWT_FILTER=popup build/tests/run-tests    # name substring
    XWT_PREINSTANCES=0 ...                    # debug-layer only

### Level 3 procedure (real input devices, no DRM needed)

On a machine where you may read your input devices (many distros grant
the logged-in user read access, or you are in an `input`-capable
group):

```sh
ls /dev/input/event*                 # pick a keyboard/mouse node
XW_INPUT_DEVICES=/dev/input/event3 \
    build/bin/xw-compositor -B headless
# in another terminal: connect a client to the printed socket and
# type/move — real events flow through the full pipeline
```

This exercises the udev-free path mode (device open, event decode,
translation). The udev seat mode (discovery + hotplug) additionally
requires a running udev instance; run
`build/bin/xw-compositor -I libinput` in such an environment. Record
what was verified (device, kernel, distro) in WORKLOG.md — that is
what keeps "verified at Level 3" honest.

## What is covered today (122 Level-1 tests + 144 Level-2 checks + 50-59 build-regression checks)

The nested-session regression (session 5 below) is the reason several
of these numbers exist: an invisible panel that looked like a working
one.

- compositor bootstrap + clean shutdown; socket lifecycle
- output creation, geometry, scale; multi-output
- wl_shm buffer attach/damage/commit; pixel-exact rendering assertions
- xdg toplevel lifecycle: map, title/app_id, configure/ack flow
- workspaces: switching, wrap-around, visibility
- shortcut engine: default table dispatch, consume-vs-forward
  suppression, show-desktop
- **session lock (ext-session-lock-v1): lock lifecycle with the
  `locked`-event-after-presented-frame ordering, unlock, second-lock
  denial (finished), client death while locked keeps the session
  locked (pixel-scanned: no window content anywhere) with takeover by
  a new client, grace-timer flush when the client never commits,
  output-resize reconfigure to the exact new size, and the
  commit-before-first-ack protocol error via a raw (non-libxwcl)
  connection — a PENDING offender's death must release the gate**
- **session lock input gate: while locked, keys and buttons are
  delivered to the lock surface ONLY — the window's callbacks stay
  silent (asserted both directions), shortcut interactivity is dead**
- **idle-notify (ext-idle-notify-v1): idled after the timeout with no
  input, resumed immediately on input, re-arm and second idled,
  independent timeouts; timers need paced real-time waits (the same
  pattern as key repeat)**
- **xw-lock (the real binary as a child process, Level 1 harness):
  wrong passphrase keeps the lock, correct passphrase unlocks and
  exits 0, SIGKILL while locked leaves the session locked and a
  second xw-lock takes over, --idle auto-lock engages only after the
  idle timeout (continuous input suppresses it)**
- focus: click-to-focus + activation, pointer hit-testing
- **key repeat: repeat_info delivery (default + configured values),
  exactly one press per keypress to clients (no server-side double
  repeat), server-side repeat of interactive move/resize keys, repeat
  stops on release**
- **libinput pipeline (Level 1, path mode): lifecycle with zero
  devices, AUTO never touches devices, relative motion clamped to the
  layout, sub-pixel accumulation, absolute motion mapped to the
  layout, linux button codes verbatim, wheel units preserved**
- **X11 backend: synthetic autorepeat presses filtered (pure logic
  test; real X autorepeat is Level 2-adjacent via Xvfb typing)**
- layer-shell: panel geometry, exclusive zone shrinking the usable
  area, set_size/set_layer requests, exclusive keyboard interactivity
  and focus release on teardown, overlay rendering
- xdg popups: positioner math (anchor rect + anchor + corner gravity),
  parent-relative configure, mapping, outside-click dismissal with
  popup_done delivery
- clipboard: wl_data_device selection set/clear, owner tracking,
  NULL-source offers not fabricated
- wlr-foreign-toplevel: existing-window announcement on bind, title
  change events, new-window announcement, handle activation focusing
  the window
- xdg-activation: token issuance, focus handover, single-use policy
  (replayed tokens rejected)
- session exit: the exit dialog maps a modal overlay (pixel-verified),
  takes keyboard, Escape cancels with exit code 0; unavailable power
  actions render dim with reasons and cannot be activated
- panel (in-process, client library): tasklist announce/title/state
  tracking, activate focuses (+ un-minimizes), close reaches the
  window as an xdg close event, closed tasks disappear; workspace
  names/active mirror the wm, activate switches workspaces
- panel (real binary): bar maps on the top layer, renders, reserves
  its exclusive zone (windows placed below), workspace-switcher
  clicks switch workspaces end-to-end, exit button sends the ctl
  `exit-dialog` line (fake session manager accepts it), panel survives
  the action
- **panel launch matrix (this round): 14 realistic .desktop fixtures
  (quoted/escaped Exec arguments, field codes, Terminal=true through a
  stub terminal, unicode names, long names, every icon flavor,
  nonexistent executable, malformed Exec) each launched end-to-end
  from the menu — the real child process must create its marker file,
  the menu must close, and `waitpid` must prove the panel survived;
  refused launches keep the menu open and show the red status line;
  an 8-round rapid relaunch burst spawns 8 processes back-to-back.
  Companion unit suites: apps-spawn (posix_spawn contract: marker,
  PATH lookup, visible failures), ctrlc-passthrough (Ctrl+C fires no
  compositor action and the client receives the key with its modifier
  state; Ctrl+Alt+Delete still opens the exit dialog and is consumed),
  client-font-utf8 (accented Latin/euro/ellipsis glyphs rasterize; a
  CJK codepoint draws the fallback box; cut sequences never draw
  garbage), panel-text-fit (UTF-8-boundary truncation with a real
  ellipsis), theme-inherit / theme-gtk / theme-xfce / ext-strip
  (Inherit= chains, gtk-3.0/xfconf theme discovery,
  extension-stripped icon names)**
- **panel interaction lifecycle (panel-interact round): pointer
  enter/leave transitions on the real forked bar with hover-fill
  rendering through the full enter+motion+redraw path (checked clear
  of the software cursor), the panel dying under pointer focus clears
  focus and subsequent motion survives (the ptr_focus use-after-free
  class — ASan-verified), a late-created wl_pointer gets exactly one
  enter replay, the launcher resolves $XW_TERMINAL and delivers the
  ctl `run <terminal>` line, the v0 clock is display-only (click =
  no action, no crash), a layer surface created before any output is
  held unconfigured and adopted+mapped+interactive when one appears,
  and a bare compositor (no panel at all) still serves windows with
  pointer focus and clicks — the component-independence guarantee**
- **layer reconfigure on output resize: an anchored bar learns the new
  output size via a fresh configure, recommits, spans the new extent,
  and the server-side geometry follows (the nested-invisibility root
  cause class; `layer-shell-resize-reconfigure`)**
- **nested X11 session under a real reparenting WM (session 5):
  Xvfb + `tests/miniwm` (reparents + resizes the compositor window),
  the panel autostarts as a real layer-shell client, survives the
  resize race, spans the WM-granted geometry, the session logs any
  child exit, `tests/panelprobe` verifies panel pixels + full-extent
  background + the compositor's SOFTWARE cursor at an XTEST warp point
  (the X cursor of the window is invisible, so any visible cursor went
  through the compositor input path), and logout is clean**
- **autostart child exits are reported with status + duration, and a
  127 exit gets an Exec=-line hint (session 7): a dead panel can never
  again masquerade as a working one**
- process-level (Level 2): session manager supervises the compositor
  child, ctl protocol (ping/status/power-status/logout/run/
  exit-dialog), honest power failure without logind, **power backend
  success path with a fake loginctl (capability report, suspend
  round-trip, failure carrying the backend's stderr message)**, XDG
  autostart filtering (OnlyShowIn/NotShowIn/Hidden), panel autostart,
  runtime spawns are supervised (killed + reaped at logout), clean
  logout (exit code 0, sockets removed, no leftover processes)

Not yet covered (honest gaps): drag-and-drop flows, popup grabs,
multi-seat, session restart (re-exec), power actions against a *real*
logind (no logind exists in the build container — the fake-loginctl
coverage is Level 2 by definition), libinput's udev seat mode and the
libinput_event decoder (Level 3), touch/gestures/tablets (unimplemented).


## Seat providers and the DRM backend

Level 1 (`tests/suite/test_seat.c`, `tests/suite/test_drm.c`):

- the built-in seatd client speaks the real wire protocol against a
  forked **mock seatd server**: handshake + seat name, device open
  with the fd arriving through `SCM_RIGHTS`, device close, session
  switch, the DISABLE -> ack -> ENABLE lifecycle (delivered as
  background traffic behind a blocking request, exactly like a real
  VT switch), server-side errors, protocol garbage, and a seat
  manager that hangs up at connect (SIGPIPE-safety: the client writes
  with `MSG_NOSIGNAL`)
- **cross-validation**: the same mock server also serves *upstream
  libseat's* seatd backend (`LIBSEAT_BACKEND=seatd`) — open, device,
  switch, close — proving the mock (and with it every
  built-in-client assertion above) implements the protocol the real
  library expects
- the direct-VT provider refuses a non-VT `/dev/tty` (containers,
  CI); on real TTYs the test skips instead of taking the console over
- AUTO selection with every provider deliberately broken fails (no
  silent root fallback), matching the combined diagnostic
- DRM planning logic without hardware: preferred/largest/highest-
  refresh mode selection, connector naming (HDMI-A-1, DP-3, eDP-1...),
  CRTC assignment (firmware pairing reuse, possible-mask fallback,
  exhaustion)

Level 2 (`scripts/test-session.sh`, session 8) — the backend selection
matrix and the honest failure taxonomy:

- explicit `--backend=drm` without KMS: exit 1, the compositor's
  reason visible, no restart loop, never a fallback
- AUTO on a TTY without KMS hardware: headless with an *explained*
  downgrade; AUTO with KMS picks DRM (hardware-only)
- bogus backend/seat-provider names rejected
- **the compositor acquires a seat through the real protocol**:
  `tests/mockseatd` (a standalone minimal seatd server) accepts the
  compositor's connection, the compositor logs the seat from the mock
  (`seat-mock`), then fails honestly at the DRM stage (no `/dev/dri`
  in CI) — the full seat path runs, only the hardware part cannot
- the direct provider's no-VT diagnostic and exit code


### VT lifecycle and signal behavior (the "never trap the user" contract)

The properties that keep a real session recoverable are regression
tested on three levels:

- **the ack that un-blocks the VT switch** (`seat-seatd-disable-
  lifecycle`, `seat-seatd-disable-autoack`): a disable event that
  arrives as background traffic behind a blocking request is
  delivered synchronously; acknowledging completes the provider's
  disable/ack/enable dance; and — the trap regression — a disable
  with *no registered consumer* acknowledges by itself, so the seat
  daemon's VT handoff can never wait forever
- **compositor-side VT switch keys** (`seat-vt-switch-keys`): with a
  seat session present, Ctrl+Alt+F2 is consumed (a focused client
  never receives the F2 press; the Ctrl/Alt modifier keys themselves
  pass through — they are ordinary keys while held) and the switch
  request round-trips to the seat provider; plain F2 is delivered to
  clients like any key
- **session signal lifecycle** (`scripts/test-session.sh`, session
  10): SIGINT (the Ctrl+C path) and SIGHUP (controlling terminal
  gone) end the session cleanly — exit 0, control socket removed, no
  orphaned children; a compositor that dies by signal is restarted
  exactly the bounded number of times, each death reported as
  `killed by signal N`, the emergency console-restore attempt is
  visible in the log (a documented no-op on non-VT containers), and
  the session exits nonzero instead of looping forever
- **the inherited signal mask** (`panel-menu-compositor-shutdown` and
  the compositor spawner): processes forked+execed from a signalfd
  parent get the mask reset — the panel exits cleanly on SIGTERM with
  a menu open (it used to ignore every signal silently because
  SIGTERM was blocked across exec)

What is hardware-only (documented, not automatable honestly):
Ctrl+Alt+F1..F12 actually flipping the console, switching back, and
termios restoration on a real VT. The manual checklist below covers
those on a machine with a VT.

### Manual hardware checklist (not automatable honestly)

CI has no `/dev/dri`, no monitors, no VTs — and the DRM ioctl paths
must not be faked (a fake DRM backend would be a lie with green
tests). Verify on real hardware, in this order:

1. `make clean && make` as your normal user (zero root).
2. From a TTY (Ctrl+Alt+F3): `xw-session --backend=drm --verbose` —
   expect the seat provider, device, connector, mode and socket lines
   in the log; the monitor switches to the desktop; panel, clock,
   workspaces, tasklist, exit button visible.
3. Keyboard, mouse, wheel, touchpad, key repeat, workspace shortcuts.
4. Ctrl+Alt+F2 away and back: screen restores to the TTY, session
   re-acquires DRM master and repaints.
5. Ctrl+C: clean exit, the TTY is usable text again (CRTC restored,
   master dropped).
6. Repeated with: seatd (and the built-in client), libseat over
   logind, libseat over elogind where available, and `--seat-provider=direct`
   from the TTY login. Any provider you could not test, say so.

### Manual panel checklist (application launching, this round)

On the real DRM session, after the generic checks above:

1. Panel visible; Start menu opens; application names render without
   missing letters (check entries with accents — Éditeur, Größe —
   and a long name that must end with an ellipsis, not a cut glyph).
2. Icons render for common apps AND for apps whose icons only exist
   in the configured (GTK/XFCE) icon theme — e.g. a ChatGPT-style
   AppImage/AUR entry: it resolves through the same generic theme
   path, no special-casing. An icon that only exists as SVG renders
   the generic app-grid glyph (documented deviation).
3. Launch a terminal, a graphical application, an app with
   spaces/quotes in its Exec, one with field codes, one with
   Terminal=true: xw-panel stays alive (check
   `pgrep -x xw-panel`), the menu closes after each success.
4. Launch an app whose executable is missing: the panel survives, a
   red status line appears next to Start, the menu stays open.
5. Rapid Start clicks + repeated launches: no crash, no freeze.
6. Ctrl+C inside the launched terminal interrupts programs in that
   terminal normally (it is NOT a desktop shortcut; the compositor
   never eats it). Ctrl+Alt+Del still opens the session action
   dialog (logout/reboot/shutdown); Escape dismisses it; the panel
   keeps working afterward.
7. Clock, calendar, pager, taskbar, Exit button still function.

### Manual XWayland checklist (X11 applications, this round)

Prerequisites: Xwayland installed (any distro package; the session
finds it on PATH or via `$XW_XWAYLAND_CMD`), and the compositor from
this build (xwayland_shell_v1 in `wayland-info` / the globals dump).

1. After login, the session log shows the XWayland block:
   executable, pid, `display :N`, `socket /tmp/.X11-unix/XN`, ready
   yes, alive yes, wm helper pid. `xw-session-ctl status` reports
   `xwayland=:N pid=... alive=yes`.
2. Launch an X11 application from the panel (e.g. an .desktop entry
   with an X11 app): its window appears as a normal compositor window
   — same focus-on-click, same taskbar button, same Alt+Tab entry as
   native Wayland apps.
3. The X11 window moves by title-bar drag, resizes from its edges,
   and maximizes/unmaximizes (the compositor mirrors its geometry
   into X11 so input lands correctly — click INSIDE the window at
   its edges to verify).
4. Switch workspaces with the X11 window on the other one and back:
   it hides and reappears; its taskbar button tracks the workspace.
5. Close the X11 window from its own button AND from the taskbar
   button: the app exits (WM_DELETE_WINDOW is delivered), the window
   and its button disappear, native apps and the compositor are
   unaffected.
6. Native Wayland apps keep working after X11 apps were launched
   and closed (mix both orders).
7. `xw-session-ctl xwayland` prints the live diagnostic block.
8. A slow-starting X11 app (first big Java/Electron apps qualify)
   maps late without the session logging errors or restarting
   anything.

Known v1 gaps (documented, not hidden): X11 windows carry the
app-id `xwayland` and a generic title (the compositor deliberately
speaks no X11 protocol — names would need an X property reader in
xw-xwm); fullscreen X11 windows track the compositor geometry, not
per-output modes.

### Manual real-client checklist (native Wayland applications)

With `.apps-root` fetched (`scripts/fetch-test-apps.sh`) or foot/
any GTK4 app installed on the machine:

1. Launch foot (or any native terminal): it stays open, its window
   renders, the cursor changes shape over it (client cursor images —
   `set_cursor` — are honored now).
2. Type into foot for 30+ seconds: no freeze (buffer rotation without
   `wl_buffer.release` used to stall clients after a few frames).
3. Launch a second native app: both remain visible and responsive.
4. The panel's taskbar lists both; clicking buttons switches focus
   correctly between them.

### Physical NVIDIA acceptance (the full sequence)

On the real DRM session, in order: panel appears; launch a small
native Wayland app — it stays; launch a second — both stay; switch
workspace and back — both alive and correctly managed; launch an X11
app through XWayland — it stays, appears in the same task list, its
workspace switches, closing it works; native apps still work
afterward. Then the deliberately slow app (xw-demo --delay-ms 5000
from a terminal): the session must never log a launch error or
restart the compositor while it has not mapped yet.

## Regression policy

Every bug fixed during development gets a test that fails without the
fix. Bugs found so far are tracked in WORKLOG.md with their tests.
Highlights (each verifiable by reverting the fix):

- SIGCHLD reaping stole the exit dialog child's status from the test
  (`exit-dialog-cancel`)
- layer-shell `set_size` request unimplemented — server abort on
  opcode 0 (`layer-shell-panel`)
- corner gravities computed as "centered" on one axis
  (`popup-positioning`)
- `wl_list_for_each` head-sentinel dereference when pressing on a
  layer surface (found by ASAN; `layer-shell-focus`)
- wl_list_for_each leaves the iterator at the head sentinel on EMPTY
  lists — pressing a TOP-layer surface (panel) with an empty OVERLAY
  layer read garbage wm memory (found by UBSan, value 16 in a bool;
  `panel-clicks`)
- eager binding of ext-workspace/foreign-toplevel managers created
  new_id announcement proxies that non-panel clients leaked (LSan,
  5 x 96 bytes in the exit-dialog child; fixed with lazy binding;
  `tests/debug-readevents.c` documents the hunt)
- xwc_dispatch ignored its timeout argument and (after the poll()
  rewrite) stopped flushing requests that wl_display_dispatch had
  flushed implicitly — the exit dialog never mapped its buffer
  (`exit-dialog-rendered`)
- X server defers flushing event batches of a connection that just
  carried a large request (our XPutImage presents): the map-time
  structure events (Reparent/Configure/Map/Expose) never reached the
  compositor, so the nested output never learned the WM's resize;
  Xlib's `_XReply` read-ahead additionally drains the socket into its
  own queue where a pure fd-poll never looks. Fixed with the x11
  backend event watchdog (one XSync round trip + XPending drain per
  50ms; reproduced deterministically by `tests/fdtest2.c`, verified
  end-to-end by session 5 under `tests/miniwm`)
- anchored layer surfaces were never reconfigured when the output
  resized — the panel kept stale geometry forever (nested regression
  + `layer-shell-resize-reconfigure`)
- libxwcl freed the wl_output listener state on the first `done`
  event while the listener stayed attached: the second output
  announcement (mode change / resize) called into freed memory and
  the panel segfaulted (session 5, ASan)
- libxwcl destroyed old wl_buffers at configure time while the
  compositor still rendered from them (the server-side wl_shm_buffer
  dies with the resource) — pools are now retired and destroyed only
  after the replacement buffer is committed (session 5)
- xw-exit drew with a hardcoded 720px height: on any other output the
  draw ran past the shm mapping and the dialog crashed (exit status
  139 in the session log; geometry now derives from the configure)
- the session manager discarded autostart/spawned child exit statuses
  — a crashed panel was indistinguishable from a running one (now
  logged with status, runtime and an Exec hint; session 7)
- **xwc_drain never flushed the client's outgoing request buffer — a
  request stuck in the socket buffer silently stalled the whole
  handshake (first seen as a missing wl_keyboard keymap event in the
  input tests; fixed with an explicit flush; `repeat-info`)**
- **releases of keys consumed by interactive keyboard move/resize
  leaked to clients as stray release events (`wm-key-repeat`)**
- activation token double-free / premature server-side destroy
  (`foreign-toplevel-activation`)
- NULL-source selection fabricated an empty offer to clients
  (`clipboard-selection`)
- **the seatd client wrote to a dead seat-manager socket with plain
  `write()`: the compositor died of SIGPIPE the moment a seatd daemon
  vanished mid-handshake (writes now use `MSG_NOSIGNAL`; the
  close-early mock test reproduces it)**
- **raw-protocol lock tests never destroyed their wl_registry/
  bind proxies: a latent client-side leak that stayed invisible until
  heavier tests recycled the stack slots LSan uses for reachability
  (destroy-before-disconnect in the tests)**
- **xwc_lock_destroy skipped the ext_session_lock_v1 destroy on the
  `finished` (denied) path and left held-lock proxies allocated after
  disconnect — the client library now destroys dead locks and frees
  held ones without a request (server still sees the connection die;
  spec-verified) (`session-lock-*`)**
- **wl_pointer.set_cursor had a NULL request handler: libwayland
  ABORTS the compositor on any request dispatched to a NULL listener
  ("listener function for opcode 0 of wl_pointer is NULL"), and every
  real toolkit sends set_cursor the moment its window takes focus —
  the "window visible for a fraction of a second, then the compositor
  restarts" physical-session bug. Reproduced in-container with the
  extracted real Xwayland before the fix; regressed by
  `pointer-set-cursor`. The same audit
  (scripts/audit-interfaces.py) now guards the whole interface table**
- **wl_buffer.release was never sent: clients rotating 2+ shm buffers
  (foot, GTK, XWayland) treat release as reuse permission and stop
  committing after their pool is exhausted — applications froze after
  a few frames with no error anywhere. Regressed by
  `buffer-release-rotation`**
- **wl_subcompositor was absent entirely: foot refuses to start
  ("no sub compositor"), GTK/Qt/Chromium use subsurfaces for menus
  and overlays. Regressed by `subsurface-lifecycle`**
- **wl_touch was never created as a resource: a client creating and
  releasing it hit a fatal invalid-object error (now a resource with
  a release handler; touch stays unadvertised)**
- **Xwayland 24+ maps X11 windows only through xwayland_shell_v1 and
  only in rootless mode with a COMPOSITE-redirecting X window manager
  present: without all three, X clients connect and render but no
  window ever reaches the compositor (regressed by
  scripts/test-xwayland.sh + scripts/test-realapps.sh)**
- **the XWM helper's SendEvent set the "generated" bit on the FORMAT
  byte (32|0x80 = 160): invalid ClientMessage format, BadValue on
  every delivery — WM_DELETE_WINDOW never reached supporting clients
  and WM_TAKE_FOCUS apps (the GTK input model) never learned they had
  focus (regressed by `xwm-close-delete` / `xwm-focus-protocol`)**
- **the override-redirect byte in CreateNotify was read from offset 24
  (padding; it sits at 22 — ConfigureNotify's is at 26, its layout
  differs): popups/menus/tooltips were never classified; additionally
  classification waited for a buffer commit, leaving undrawn popups
  as ghosts in the managed list (regressed by
  `xwm-override-redirect`)**
- **the helper never selected PropertyChangeMask on managed client
  windows: terminals retitled themselves and the taskbar never heard
  (regressed by `xwm-title-change`)**
- **the geometry mirror confused the wl_surface EXTENT (interior +
  X border) with the ConfigureWindow INTERIOR: each mirror round grew
  the window by its own border — a ratchet for every drawn client —
  and granted resizes never reached the compositor at all for
  undrawn ones (no set_geometry channel); both directions now
  convert through the border width (regressed by
  `xwm-configure-mask`; the real-xterm resize is covered by
  `xwm-xterm-real`)**
- **the XWayland test scaffold leaked the compositor's signalfd
  blocked-signal mask (HUP/INT/TERM/CHLD survive fork AND exec) into
  every spawned child: kill(SIGTERM) silently did nothing and the
  suite wedged in wait4(); children now restore default signal state,
  and xterm requires an absolute argv[0] (regressed by the suite
  completing at all — test_xwm.c's child_signal_defaults)**
- **a clean rebuild silently skipped the whole XWayland suite: the
  x11client probe was not wired into `make all`, and a skip counted
  as a pass in the summary — the ASan round had 10 invisible skips
  inside a green "121/121" (the probe is built with `all`, and the
  summary now reports "(N skipped)")**
