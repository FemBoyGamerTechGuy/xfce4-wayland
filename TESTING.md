# Testing

The suite is deterministic, automated, and GPU-free: everything runs
against the **headless backend** with the pixman software renderer.

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
- `tests/suite/test_session.c` — the graphical exit dialog as a real
  child process against the in-process compositor.
- `tests/suite/test_panel.c` — panel coverage (see above).
- `tests/suite/test_backends.c` — nested backend coverage: a real
  compositor (B, nested) running inside another real compositor (A,
  headless) in one process. Asserts topology, the present pipeline
  (B's framebuffer content visible in A's pixels), clients of B
  rendering through B into A, and input routing (keys injected into A
  reach B's shortcut engine when B's window is focused — and parent
  shortcuts must not shadow the child desktop).
- `tests/x11probe.c` + `build/tests/x11probe` — X11-backend process
  probe: finds the compositor window in a live X server, reads back
  its pixels (XGetImage) and injects XTEST keyboard input.
- `scripts/test-session.sh` — process-level session integration test
  (real `xw-session` + `xw-compositor` children, ctl socket, autostart
  filtering, runtime spawns, panel autostart, clean logout, the X11
  backend under Xvfb with synthesized input, `xw-session --nested`,
  and the nested Wayland backend across two real processes).
- `scripts/run-asan.sh` — full sanitizer regression pass (ASan + UBSan
  + LeakSanitizer) including the process-level test; restores the
  release build afterwards.

## Running

    make tests        # builds and runs the in-process suite
    make check        # in-process suite + process-level session test
    make asan         # full sanitizer pass (rebuilds, tests, restores)

Filtering tests (triage):

    XWT_FILTER=popup build/tests/run-tests    # name substring
    XWT_PREINSTANCES=0 ...                    # debug-layer only

## What is covered today (22 in-process tests + 47 process checks)

- compositor bootstrap + clean shutdown; socket lifecycle
- output creation, geometry, scale; multi-output
- wl_shm buffer attach/damage/commit; pixel-exact rendering assertions
- xdg toplevel lifecycle: map, title/app_id, configure/ack flow
- workspaces: switching, wrap-around, visibility
- shortcut engine: default table dispatch, consume-vs-forward
  suppression, show-desktop
- focus: click-to-focus + activation, pointer hit-testing
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
  takes keyboard, Escape cancels with exit code 0
- panel (in-process, client library): tasklist announce/title/state
  tracking, activate focuses (+ un-minimizes), close reaches the
  window as an xdg close event, closed tasks disappear; workspace
  names/active mirror the wm, activate switches workspaces
- panel (real binary): bar maps on the top layer, renders, reserves
  its exclusive zone (windows placed below), workspace-switcher
  clicks switch workspaces end-to-end, exit button sends the ctl
  `exit-dialog` line (fake session manager accepts it), panel survives
  the action
- process-level: session manager supervises the compositor child, ctl
  protocol (ping/status/logout/run/exit-dialog), honest power failure
  without logind, XDG autostart filtering (OnlyShowIn/NotShowIn/Hidden),
  panel autostart, runtime spawns are supervised (killed + reaped at
  logout), clean logout (exit code 0, sockets removed, no leftover
  processes)

Not yet covered (honest gaps): drag-and-drop flows, popup grabs,
key repeat, multi-seat, D-Bus-free restart path, power actions against
a real logind (no logind exists in the build container).

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
- activation token double-free / premature server-side destroy
  (`foreign-toplevel-activation`)
- NULL-source selection fabricated an empty offer to clients
  (`clipboard-selection`)
