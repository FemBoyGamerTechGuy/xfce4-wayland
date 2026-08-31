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
- `scripts/test-session.sh` — process-level session integration test
  (real `xw-session` + `xw-compositor` children, ctl socket, autostart
  filtering, clean logout).
- `scripts/run-asan.sh` — full sanitizer regression pass (ASan + UBSan
  + LeakSanitizer) including the process-level test; restores the
  release build afterwards.
- `tests/debug-layer.c` — scratch reproducer for layer-shell child
  processes (not part of the suite; built ad hoc when debugging).

## Running

    make tests        # builds and runs the in-process suite
    make check        # in-process suite + process-level session test
    make asan         # full sanitizer pass (rebuilds, tests, restores)

Filtering tests (triage):

    XWT_FILTER=popup build/tests/run-tests    # name substring
    XWT_PREINSTANCES=0 ...                    # debug-layer only

## What is covered today (16 in-process tests + 18 process checks)

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
- process-level: session manager supervises the compositor child, ctl
  protocol (ping/status/logout), honest power failure without logind,
  XDG autostart filtering (OnlyShowIn/NotShowIn/Hidden), clean logout
  (exit code 0, sockets removed, no leftover processes)

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
- activation token double-free / premature server-side destroy
  (`foreign-toplevel-activation`)
- NULL-source selection fabricated an empty offer to clients
  (`clipboard-selection`)
