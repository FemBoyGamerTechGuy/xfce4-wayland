# WORKLOG

Append-only engineering log. Oldest entries first. This file is the
authoritative checkpoint trail for resuming autonomous work: the
"Next" section at the end always describes the immediate continuation
point.

---

## 2026-08-31 — session 1

- Inspected environment: Debian 13 (trixie), gcc/make/pkg-config/git,
  python3+Pillow, pixman/cairo present; **no** wayland dev packages, no
  root. Network available.
- Bootstrapped a rootless sysroot at `../.toolchain/sysroot` by
  `apt-get download` + `dpkg -x`: libwayland-dev 1.23.1, libxkbcommon-dev
  1.7.0, wayland-protocols 1.44. wayland-scanner verified working.
  (libwayland runtime .so already present system-wide — matching version.)
- Vendored wlr-protocol XMLs from the swaywm GitHub mirror after
  gitlab.freedesktop.org raw URLs proved bot-gated: layer-shell,
  foreign-toplevel-management, output-management (MIT; provenance in
  THIRD-PARTY-LICENSES.md).
- Decisions (see ARCHITECTURE.md for rationale):
  - plain GNU make build (not meson) — minimal dependency surface;
  - pixman software renderer, headless-first backend;
  - no GLib, no GTK, no D-Bus linkage; power via loginctl CLI;
  - clients are native Wayland clients on libxwcl with a build-time
    generated bitmap font (genfont.py, Pillow) — no runtime font stack;
  - session control via private unix line protocol (xw-session ctl
    socket), not a Wayland protocol (documented in ARCHITECTURE.md);
  - original code proprietary; no XFCE code incorporated (behavioral
    re-implementation only, verified as of this entry).
- Wrote repo skeleton: LICENSE, README, ARCHITECTURE, ROADMAP, BUILDING,
  DEPENDENCIES, THIRD-PARTY-LICENSES, scripts/env.sh, tools/genfont.py
  (95-glyph font table generated successfully), Makefile (protocol
  codegen via wayland-scanner).

### Next
Implement libxw core: internal headers (xw-internal.h), util (log,
region helpers), INI parser, server/bootstrap, headless backend, outputs
+ wl_output, wl_shm/buffers + surface commit machinery, pixman renderer,
then a minimal xw-compositor main that can start, accept a connection,
and shut down cleanly. Compile early.


## 2026-08-31 — session 2

- Fixed the build system (recipe `$$@`/`$$<` escaping, protocol path
  layout, GEN_PROTO_OBJ paths, consolidated mkdir rule, per-module
  protocol basenames). `make all` now builds libxw.a, libxwcl.a,
  xw-compositor and the test binary from clean in one pass.
- Patched the sysroot .pc files (prefix= → real path) and linked the
  sysroot dev .so symlinks to the system runtime copies.
- Implemented the missing libxw modules (session 1 had headers only):
  xw-seat.c (wl_seat/keyboard/pointer + xkb + focus + grab logic),
  xw-actions.c (actions bus + commands config), xw-xdg-shell.c
  (wm_base/xdg_surface/toplevel/popup + positioner math + role
  dispatch), xw-layer-shell.c, xw-foreign-toplevel.c,
  xw-ext-workspace.c, xw-activation.c, xw-data-device.c
  (selection + DnD), xw-shortcuts.c (parser + XFCE default table),
  and the xw-compositor binary main.
- Researched and verified the exact xfwm4 4.20 default shortcut table
  from docs.xfce.org (web fetch) — defaults are faithful; deviations
  documented in ROADMAP.md (window menu / xkill / taskmanager keys
  unbound in v0; no default tiling bindings, matching upstream).
- libxwcl client library (xwc/xwc-input/xwc-draw) with pumped sync
  so the in-process test harness can drive client+server without
  deadlocking; bitmap font fixed (per-glyph arrays) and used by the
  drawing helpers.
- Integration test harness (tests/harness) + 9-test core suite
  (lifecycle, window map, pixel-exact rendering, workspace switching
  incl. wrap-around, shortcut dispatch/suppression, show desktop,
  pointer click-to-focus). 9/9 pass.
- Bugs found and FIXED (not worked around):
  1. Compositor teardown double-freed output globals (display destroy
         then backend destroy) → strict teardown ordering + module fins.
  2. Client teardown UAF: wl_surface (created first) freed the window
         while the xdg_surface destructor still dereferenced it →
         orphan-back-pointer protocol between the two destructors;
         same pattern applied to layer-shell.
  3. wl_list member misuse: wm->stack was iterated with `link`
         instead of `stack_link` in 12 places → 16-byte-offset garbage
         windows (this produced the "invisible garbage" renders).
  4. xkb keymap compiled from empty RMLVO (raw non-evdev keycodes,
         no modifiers) → default rules=evdev model=pc105 layout=us.
  5. evdev+8 keycode translation missing (wayland/xkb keycodes are
         linux keycode + 8; injection API stays raw linux).
  6. Client drew on toplevel.configure before the pool existed →
         draw moved to xdg_surface.configure.
  7. Idle repaint source + signal sources + client proxies leaked at
         teardown → all now released; suite is ASAN/LSAN clean.
- Verified: 9/9 tests, zero ASAN errors, zero leaks (10312 bytes in
  120 allocations before the fixes).

### Next
M6 session: write xw-session (autostart + supervision + ctl socket),
xw-session-ctl, then xw-exit dialog (M7 entry) wiring the
XW_ACTION_EXIT_DIALOG path, followed by xw-panel v0. Extend the test
suite with layer-shell, popup, clipboard/DnD, foreign-toplevel and
activation coverage. Then git commit the milestone.


## 2026-08-31 — session 3

M6/M7 verification + hardening. The M6/M7 code from the interrupted
session existed but had never been proven; the two new exit-dialog
tests failed. Root-caused and fixed (not worked around) — every fix
has a regression test.

- Fixed `scripts/env.sh`: `$0` is the shell when the file is *sourced*
  → `${BASH_SOURCE:-$0}`; also made it `set -u`-safe
  (`${XDG_RUNTIME_DIR:-}`) so sanitizer scripts can source it.
- **SIGCHLD status theft (design bug)**: the compositor's reaper called
  `waitpid(-1)`, stealing exit statuses of children it never spawned
  (the test's exit-dialog child, the session manager's compositor, any
  embedder's children). Now the compositor tracks the pids it spawns
  (children[], `xw_compositor_track_child`) and reaps only those
  (`xw-compositor.c`, `xw-util.c`, `xw-internal.h`).
- **Test-side pacing bug**: the rendered-dialog test busy-spun without
  wall-clock time; a forked child needs real time to exec + connect +
  handshake. Added per-iteration sleep (test_session.c).
- Makefile dependency bug: `tests/suite/%.o` did not depend on the
  internal headers → stale objects compiled against the old struct
  layout after header edits (silent ABI skew). Also learned the hard
  way: the Edit tool space-mangles Makefile tabs — Makefile changes
  are now applied byte-precisely via python.
- **layer-shell `set_size` request was unimplemented** (opcode 0 —
  any panel-style client calling set_size crashed the server's
  dispatch; only the exit dialog worked because it never sends
  set_size). Implemented `ls_set_size` (+ pending-configure update)
  and `ls_set_layer` (v2 restacking with damage) — the full
  zwlr_layer_surface_v1 request set is now covered.
- **Corner gravities were wrong**: gravity_offset treated corner
  gravities as "centered" on the X axis; corners now combine axis
  semantics per the xdg-shell spec.
- **Popup anchor double-count**: popup_place added the anchor rect
  origin on top of the anchor point (which already includes it) —
  popups placed 2x the rect offset away.
- **Popup outside-click dismissal was missing** (menus never closed):
  a press outside the topmost popup now dismisses the popup chain,
  keeping popups under the cursor (xw-seat.c).
- **xdg-activation double-free + protocol footgun**: act_activate
  unlinked the token list AND destroyed the resource (destructor
  unlinked again → poisoned-pointer SEGV); also a server-side destroy
  turns the client's spec-sanctioned token destroy into a protocol
  error. Tokens are now invalidated in place (single-use preserved).
- **wl_list_for_each head-sentinel dereference** in
  xw_seat_pointer_button (press on a layer surface with no matching
  window → w = list-head cast → UAF found by ASAN). Rewritten with an
  explicit hit flag; audited the whole tree for the same idiom.
- **wm_base listener double-attach** in libxwcl: attached per window
  creation instead of once per connection.
- **NULL-source selection fabricated an empty offer** when clearing
  the clipboard; now selection(NULL) is sent honestly.
- New protocol suite (tests/suite/test_protocols.c, 5 tests): layer
  panel geometry + exclusive zone + set_layer, exclusive keyboard
  interactivity + focus release, popup positioning/dismissal,
  clipboard selection set/clear/owner, foreign-toplevel
  announce/title/activation + xdg-activation focus handover +
  single-use. Raw protocol objects (second registry) driven directly.
- libxwcl: `xwc_layer_set_layer`, `xwc_win_surface`,
  `xwc_win_xdg_surface` accessors.
- New process-level session test `scripts/test-session.sh` (18 checks:
  supervision, ctl protocol, honest power failure without logind,
  autostart OnlyShowIn/NotShowIn/Hidden filtering, clean logout) and
  `scripts/run-asan.sh` (full ASan/UBSan/LSan pass incl. child
  process). `make check` / `make asan` targets; `XWT_FILTER` test
  selection in the harness.
- Removed the leftover [kdbg] seat debug printf; -O1-only
  format-truncation warnings fixed properly (snprintf return checks).
- Verified: 16/16 in-process tests, 18/18 process checks, ASAN+LSAN
  clean (incl. the forked dialog child), dev-session.sh --logout exits
  rc=0 end-to-end. Docs (README/ROADMAP/TODO/TESTING/BUILDING)
  corrected to match reality (panel/demo marked planned, testing
  claims aligned with what actually runs).

### Next
M8 panel v0 (`src/clients/xw-panel.c`): layer-shell top bar with
workspace switcher (ext-workspace), tasklist (foreign-toplevel
handles: click-to-activate, close), clock, and an exit button that
runs the XW_ACTION_EXIT_DIALOG action. Reuse libxwcl + bitmap font.
After that: notification daemon skeleton + wallpaper layer client;
then back to the shortcut/theme settings GUI once the panel proves
the client-library surface.


## 2026-08-31 — session 4

M7 panel v0 (M8 in TODO numbering): implemented, tested, hardened.
The session started by triaging the interrupted tree: all 53 "modified"
files were spurious 100644→100755 mode flips from the container
snapshot machinery (zero content changes; no M8 work had landed).
Added scripts/fix-modes.sh (restores every tracked file's mode from
HEAD) and verified the M6/M7 baseline first (16/16, 18/18, tree clean).

- libxwcl: new xwc-tasklist.c — wlr-foreign-toplevel + ext-workspace
  client bindings for panels (xwc_tasklist/xwc_wspaces: announce,
  title/app_id/state, activate/close, workspace names/active/switch).
- xwc_dispatch was rewritten: the timeout argument was silently
  ignored (blocking dispatch). Standalone clients now use the
  prepare_read/poll/read-or-cancel dance — and must FLUSH first:
  wl_display_dispatch flushed implicitly, poll() does not, so the
  exit dialog silently stopped mapping its buffer until the flush
  was added (caught by exit-dialog-rendered). Embedded (pump) mode
  pumps + drains without ever sleeping.
- xwc_layer_create now sends set_size when EITHER dimension is fixed
  (a width-from-anchors bar previously could not specify height
  alone); xwc_win_closed() accessor added.
- xw-panel.c: one layer-shell surface, top bar, exclusive zone =
  height. Launcher (ctl run), workspace switcher (ext-workspace),
  tasklist (click activate / middle-right close), HH:MM clock,
  exit button (ctl exit-dialog). Bitmap font, no toolkit.
- xw-session: ctl `exit-dialog` (spawns the same command as the
  compositor's XW_ACTION_EXIT_DIALOG; $XW_EXIT_CMD override) and
  `run CMD` (session-scoped execution for the panel launcher) —
  both as supervised children (SIGTERM at shutdown, SIGCHLD reaped,
  bounded table). xw-session-ctl CLI updated; ctl wire factored into
  src/clients/xw-ctl.c shared by xw-exit and xw-panel.
- Makefile: xw-panel + xw-ctl wired in (byte-precise python patches;
  the Edit tool still tab-mangles make). Learned: a no-recipe extra
  prerequisite rule made $< resolve to the HEADER and gcc emitted a
  precompiled-header "object"; fixed by putting src/clients/*.h into
  the pattern rule. Also: test objects now depend on
  src/libxwcl/*.h — struct xwc is embedded in the harness, and a
  stale object with the old layout made the library memset overrun
  into socket_name ("cannot connect to display", 16/16 failures).
- **UBSan: wl_list_for_each empty-list sentinel (real bug)** — on an
  EMPTY list the iterator is left pointing at the list HEAD cast as
  an entry; the click-to-focus layer scan used the post-loop value,
  so pressing a TOP-layer surface (panel) with an empty OVERLAY read
  garbage wm memory as a bool (value 16). Same family as the session-3
  sentinel UAF, survived because nothing ever clicked a TOP layer.
  Rewritten with a separate found-iterator; regression: panel-clicks.
- **LSan: eager manager binding leaked 5 x 96 B per client (real
  bug)** — binding ext_workspace_manager_v1 at registry time makes
  the server immediately create 1 group + 4 workspace proxies via
  new_id events; clients that never use them (the exit dialog)
  leaked exactly those 5. Fixed with LAZY binding (registry records
  the global name; tasklist/wspaces bind on demand and own the
  proxies). tests/debug-readevents.c documents the hunt (disasm of
  the calloc call site + minimal reproducer).
- tests: test_panel.c (5 tests: tasklist-client, workspace-client,
  panel-maps, panel-clicks, panel-exit-button with a fake session
  manager accepting the ctl line); test-session.sh session 3
  (panel autostart + ctl run/exit-dialog + supervised teardown);
  paced (wall-clock) waits for forked-client conditions — the
  fast-spin XWT_WAIT races child processes under ASan (session-3
  lesson, rediscovered the hard way).
- dev-session.sh now autostarts the panel (isolated HOME) — full
  desktop demo, clean logout verified.
- Verified: 21/21 in-process, 28/28 process checks, full
  ASan+UBSan+LSan pass incl. both forked children, dev-session
  --logout rc=0. Docs updated (README/ROADMAP/TODO/TESTING/
  ARCHITECTURE; ROADMAP M7 marked DONE with PART gaps).

### Next
Notification daemon skeleton (M7 backlog) or wallpaper/desktop
layer client; then the settings GUI once more client surface is
proven. Session restart (re-exec) still needs an automated test.
Consider upstreaming the paced-wait helper into the harness (tests
currently re-implement it in test_panel.c).

## Session 4 — Phase 2: nested backends (real desktop inside a session)

**Goal**: move beyond headless — run the whole desktop as a window
inside the user's existing Wayland or X11/XLibre session (the safe
development workflow before DRM/KMS).

### Work
- **Backend refactor**: `struct xw_backend` gained an ops vtable
  (`present`, `destroy`); `xw_output_repaint` calls `present` after
  compositing. Output lifecycle factored out of headless into shared
  `xw_output_create/destroy` + new **`xw_output_resize`** (realloc
  backbuffers, re-announce geometry/mode/done, relayout). Injection
  API moved to xw-compositor.c (backend-independent).
- **nested Wayland backend** (`xw-backend-nested.c`): the compositor is
  a *client of the parent* via libxwcl (dogfooded); one output mirrors
  the parent window; present = memcpy into the SHM back buffer +
  commit; parent input forwarded verbatim (evdev keycodes, Wayland
  button codes); the parent socket is multiplexed on our own event
  loop. Test hook: `nested_pump` lets an in-process parent run during
  the blocking handshake.
- **nested X11 backend** (`xw-backend-x11.c`): top-level X window,
  XPutImage straight from the native pixman buffer (identical byte
  layout, zero conversion), X keycodes = evdev+8, buttons 1-3 →
  0x110/111/112, wheel 4/5 → axes, detectable auto-repeat via XKB,
  invisible X cursor (our software cursor is the visible one),
  ConfigureNotify → output resize, WM_DELETE_WINDOW → stop.
- **CLI**: `xw-compositor -B headless|nested|x11` + `-D parent`;
  `xw-session --nested` auto-selects (WAYLAND_DISPLAY → nested, else
  DISPLAY → x11, $XW_BACKEND overrides); nested sessions keep DISPLAY
  (XWayland future). `xw-session-ctl` gained `-S` (parity with
  xw-session; needed by tests and multi-session setups).
- **Multi-compositor correctness**: removed ALL file-static module
  state (layer-shell, ext-workspace, activation, data-device) into
  per-compositor fields — two compositors in one process previously
  leaked one and could free the other's state (UAF). Signals now armed
  at the top of `xw_compositor_create` (early TERM during backend
  handshakes killed the process with the default disposition before).

### Bugs found & fixed (the interesting ones)
- `wl_display_read_events` **decrements `reader_count`
  unconditionally** — calling it without a `prepare_read` intent
  corrupts the count to -1 and the next call blocks forever on the
  reader futex (decoded from the libwayland disassembly after a
  traced hang). The nested fd callback now uses the canonical
  prepare→read→dispatch→flush loop.
- libxwcl `xwc_sync` with a pump never drained its own side of the
  connection (pumps only drive the embedded server) — added
  `xwc_drain`; sync now completes against an in-process parent.
- Static callback table without `.ud` → NULL user data → crash in the
  first configure (found by ASan immediately).
- Output buffers leaked for every compositor after the backend
  refactor (the old headless destroy freed them; the new contract had
  no owner) — `xw_backend_destroy` now always destroys outputs.
- Makefile's *first* target was an eval-generated protocol rule: bare
  `make` silently did nothing since the first build (only
  `make tests/check` built). Fixed with `.DEFAULT_GOAL := all`.
- The 4-signals-armed-late race: compositors killed during creation
  exited 143 instead of 0 (reproduced 3/3 with immediate kill).

### Tests
- `tests/suite/test_backends.c` — compositor-inside-compositor
  (in-process): topology, present pipeline verified through PIXELS,
  clients of the nested desktop render through to the host, input
  routing with parent/child shortcut shadowing check.
- `scripts/test-session.sh` sessions 4-6: x11 backend under Xvfb
  (pixel round-trip via XGetImage + XTEST-injected Ctrl+Alt+D consumed
  by the shortcut engine), `xw-session --nested` end to end, and the
  nested Wayland backend across two real processes with a live panel.
- Verified: 22/22 in-process, 47/47 process checks, full
  ASan+UBSan+LSan pass. Docs updated across the board.

### Next
Phase 3: real input (libinput seat backend for DRM sessions);
meanwhile keyboard move/resize + shortcut gaps from the XFCE table,
notification daemon, XWayland detection for nested-X11 sessions.

## 2026-09-01 — session 5

Phase 2.5/3: build-system hardening, real input, protocol-correct key
repeat, the logind/elogind power backend, and a shortcut-engine parity
fix — each driven by tests that found real bugs.

- **Environment recovery**: the container was reset between sessions;
  the rootless sysroot had to be rebuilt. scripts/bootstrap-sysroot.sh
  now automates it (downloads dev packages + the runtimes libinput
  needs: libevdev/libwacom/mtdev/libgudev, rewrites .pc prefixes,
  rpaths libinput, links matching system SONAMEs) and
  scripts/env.sh exports LD_LIBRARY_PATH for the sysroot (RUNPATH is
  not transitive to libinput's own dependencies).
- **Build system**: XW_X11/XW_LIBINPUT toggles (auto/1/0) with the
  directive-grade diagnostics (what breaks, how to enable, how to
  silence); PROFILE presets (release/debug/asan) + a build/.profile
  stamp that REFUSES profile switches over a populated tree (stale
  sanitized/plain mixing was a real bug class here); required-dep
  validation with actionable $(error) messages; make config summary;
  install/uninstall (prefix/DESTDIR, wayland-sessions .desktop, example
  INI configs documenting every parser key — rules.conf examples were
  corrected against the real fnmatch semantics); dist tarball sorted.
  Verified zero-root end to end.
- **Real input (Phase 3)**: xw-input-libinput.c as an input SOURCE
  orthogonal to backends — udev seat mode + $XW_INPUT_DEVICES path
  mode; -I/--input auto|libinput|none; AUTO never grabs devices
  (tests + nested stay deterministic); translation in white-box
  handlers (clamping, sub-pixel acc, abs→layout, v120 wheels); libinput
  logging routed into xw_log. In-container libinput runtime deps
  fetched rootlessly into the sysroot.
- **Key repeat, protocol-correct**: wl_keyboard.repeat_info after the
  keymap (clients repeat — was missing entirely, so NO client had key
  repeat); server-side repeat only for interactive keyboard
  move/resize; X11 backend filters detectable-autorepeat presses of
  held keys (xw_x11_key_filter) so clients never double-repeat.
  Config: keyboard.conf [keyboard] + $XW_REPEAT_* env; 500ms/30Hz
  XFCE defaults.
- **Power backend**: xw-power.c shared by xw-session and xw-exit —
  loginctl liveness (run it, don't check PATH: the container HAS
  loginctl with no daemon and must read as unavailable),
  /sys/power/state probing ($XW_POWER_STATE_PATH override), reasons
  for every unavailable action, fork+execvp with fixed argv (no shell;
  replaces system()), stderr captured into error replies. ctl
  `power-status`; exit dialog greys unavailable actions with their
  reason and refuses activation; xw-session now passes the user config
  dir to the compositor (INI config finally effective in sessions).
  Process tests: session 1 forced-unavailable environment (the suite
  can never suspend a dev machine), session 1b fake-loginctl success
  path incl. captured-stderr failure.
- **Bugs found by the new tests and FIXED**:
  1. xwc_drain never flushed the client's outgoing request buffer —
     a request stuck in the socket buffer stalled the whole handshake
     (first seen as a missing keymap event). Fixed with an explicit
     wl_display_flush; regression: repeat-info tests.
  2. Releases of keys consumed by interactive move/resize leaked to
     clients as stray releases (same class as shortcut suppression);
     fixed via the consumed-keys bitmap; regression: wm-key-repeat.
  3. Shortcuts: Shift+Tab produces ISO_Left_Tab and Alt+Print produces
     Sys_Req — the keysym matcher compared literally, so
     <Alt><Shift>Tab (cycle back) and <Alt>Print (screenshot) could
     NEVER fire. canonical_keysym() on both binding and event sides
     (xfwm4 matches by keycode; a keysym matcher must canonicalize).
     Found by the new table-driven all-defaults test (38→47/47).
  4. Test harness had wrong evdev codes for F11/F12 (69/70 are
     NumLock/ScrollLock) — latent, nothing had injected F11/F12.
- **Tests**: 31→32 in-process (repeat-info, repeat-info-config,
  wm-key-repeat, client-no-double-repeat, input-lifecycle, input-auto-
  off, input-motion-pipeline, input-key-pipeline, x11-repeat-filter,
  shortcut-all-defaults), 47→61 process checks, TESTING.md rewritten
  around the explicit 3-level strategy (unit / nested-process /
  real-hardware) with honest statements of what each level cannot
  cover. Full ASan/UBSan/LSan pass. -O1-only format-truncation
  warnings fixed by enlarging buffers (worklog lesson from session 2
  held: fix, don't suppress).
- Docs: BUILDING.md rewritten as the distro-agnostic guide (requirements
  categories, knobs, profiles, zero-root section, DM/TTY session
  integration with honest status table, distro package EXAMPLES incl.
  a generic "not listed" procedure, troubleshooting keyed to the build
  system's own error messages); DEPENDENCIES.md is now the full matrix
  (why/min-version/license/mode/copyleft per entry, rejected list,
  addition bar); README/ARCHITECTURE/ROADMAP/TODO updated to match
  reality.

### Next
DRM/KMS backend (Phase 4 output half): libdrm device discovery,
connectors/CRTCs/planes, dumb-buffer scanout first, atomic modesetting
after; logind session takeover for DRM master without root; multi-
monitor + hotplug on top. Also queued: session restart (re-exec)
automated test, notification daemon skeleton, xdg geometry offsets
(CSD shadows).
