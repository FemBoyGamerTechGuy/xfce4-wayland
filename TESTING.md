# Testing

The suite is deterministic, automated, and GPU-free: everything runs
against the **headless backend** with the pixman software renderer.

## Layout

- `tests/harness/xwtest.h` — assert framework, test registration, child
  process orchestration, leak counters.
- `tests/harness/harness.c` — embeds the compositor in-process, drives
  the event loop between injections, collects results.
- `tests/harness/client.c` — scripted Wayland client (real
  libwayland-client) used from forked children, reporting protocol
  events back over a pipe.
- `tests/suite/*.c` — the suites (core, wm, input/shortcuts, clipboard,
  layer-shell/panel-integration, session).

## Running

    make tests        # builds and runs build/tests/run-tests
    build/tests/run-tests [suite-name ...]   # subset
    build/tests/run-tests -v                 # verbose per-check output

## What is covered today

- compositor bootstrap + clean shutdown; socket lifecycle
- output creation, geometry, scale; multi-output
- wl_shm buffer attach/damage/commit; frame callbacks
- xdg toplevel lifecycle: map/unmap, title/app_id, configure/ack flow
- maximize/minimize/fullscreen state machine incl. restore geometry
- interactive move/resize (pointer-injected and keyboard-injected)
- edge snapping + keyboard half-tiling
- workspaces: switching, window assignment, sticky windows, wrap-around
- focus: click-to-focus, focus-on-activation, Alt+Tab MRU order,
  wl_keyboard enter/leave observable by clients
- shortcut engine: exact-modifier matching, consume-vs-forward to the
  focused client, conflicts reported, config reload, every default
  binding's action fires
- window rules: match on app_id/title, applied at map
- clipboard: selection set/get across two clients; drag-drop basic flow
- layer-shell: panel surface mapping, exclusive zone affects usable area
- foreign-toplevel + ext-workspace export observable by a client
- session end-to-end: xw-session launches compositor + stub components,
  autostart filtering, LOGOUT/RESTART ctl protocol, clean child reaping,
  power actions against a fake loginctl (command-line verification)
- screenshots-as-pixels: window presence and stacking order verified by
  reading the rendered output buffer

## Regression policy

Every bug fixed during development gets a regression test named for it
(`test_regression_<id>_*`). A fix without a regression test is not
considered done. Bugs found so far are tracked in WORKLOG.md with their
tests.
