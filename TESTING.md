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
- `tests/suite/test_panel.c` — panel coverage (see below).
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

## What is covered today (34 Level-1 tests + 84 Level-2 checks + 49-58 build-regression checks)

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
