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

### Session 5 addendum

- **CSD geometry offsets (M2 PART gap closed)**: set_window_geometry
  x/y now apply. render: blit_surface takes a source sub-rect origin
  (content composited 1:1 from (geo_x, geo_y); oversized buffers —
  shadow margins — composite only the declared rect, which keeps
  damage exact; the 1:1 path covers any buffer containing the source
  rect). input: surface-local coords account for the buffer-origin
  offset and the default interactive rect is the geometry rect, so
  clicks on CSD shadows fall through. Test: csd-geometry.
- **Session restart (re-exec) covered + fixed**: the re-exec rebuilt
  a minimal argv and silently dropped user flags (-n kept autostart
  off in the original session but not in the restarted one); it now
  re-execs with the original argv. New session 1c process checks:
  socket teardown + reappearance, same pid, fresh compositor,
  restarts=0, flags preserved, clean logout.

---
Task ID: artix-build-fix
Agent: chief-autonomous-developer
Task: Fix the clean-distro quick-start failure reported on Artix/XLibre:
genfont "no usable system TTF font found", dev-session.sh continuing
past a failed build, and an earlier "zsh: number expected".

Work Log:
- Investigated: reproduced the font failure exactly (mount-namespace
  hiding /usr/share/fonts; root cause = 3 hardcoded Debian-layout font
  paths in tools/genfont.py — Arch-family uses /usr/share/fonts/TTF/...).
  Confirmed the dev-session cascade (backgrounded launch of missing
  binaries, silent 5s socket timeout, confusing output, rc=127 late).
  Audited "zsh: number expected": zsh 5.9 contains exactly 4 such
  message variants, all from builtin option parsing (read -u/-t/-k
  style) — none used anywhere in the repo; could NOT be reproduced
  from any repo script under zsh (script / sourced / bin-sh modes).
  Most likely origin: the reporter's interactive zsh environment.
  Response: hardened everything anyway + real zsh test coverage.
- Bundled font: subset DejaVu Sans 2.37 to ASCII U+0020-007E with
  pyftsubset (759,720 -> 43,932 bytes; name table incl. embedded
  license, layout features, hinting preserved). Verified rendering
  bit-identical to the full font (metrics, advances, anti-aliased
  pixel strips at sizes 12/16/20/24) — scripts/verify-font-subset.py.
  License audited: Bitstream Vera + Arev + public-domain DejaVu
  changes; redistribution permitted with license text; retained name
  "DejaVu Sans" satisfies the rename condition. assets/fonts/ now
  carries the subset + LICENSE-DejaVuSans.txt + provenance README;
  THIRD-PARTY-LICENSES.md updated (font is now redistributed, was
  "not redistributed" before).
- genfont.py rewritten: default source = bundled asset (always
  present, deterministic, distro-agnostic); --font PATH override for
  packagers; NO system font search at all; precise multi-line
  diagnostics for missing asset / missing Pillow / bad font file;
  glyph-count sanity check. Output provenance recorded in the header.
- Makefile: dependency validation now checks python3 presence, Pillow
  importability and the bundled asset (actionable $(error) messages,
  skipped for clean/dist/config as before); XW_FONT knob (rule gains a
  real dependency on the actual font source); `make config` reports
  the font source. Patches via byte-precise scripts
  (scripts/wire-font-asset-mk.py) per the established Makefile lesson.
- dev-session.sh rewritten fail-fast: preflight existence checks for
  all 5 binaries (refuses with instructions before launching
  anything), early-crash detection with foreground-debug hints, socket
  timeout diagnostic instead of silent continue, correct exit codes,
  self-sources env.sh (the old script only worked when the parent
  shell had sourced it — sysroot runtime libs). Found + fixed a REAL
  zsh incompatibility in the process: zsh does not word-split
  unquoted expansions, so `for b in $NEEDED` broke the preflight under
  zsh (literal list now). bootstrap-sysroot.sh had the same class of
  bug (apt-get download $PKGS) — fixed with a literal list.
- make check self-containment: the Makefile now exports
  LD_LIBRARY_PATH when a sysroot is active (symmetric with its
  PKG_CONFIG_PATH export) — make-spawned test binaries resolve
  transitive sysroot deps (libinput -> libmtdev) without the parent
  shell having sourced env.sh. test-session.sh + test-build-regressions
  self-source env.sh likewise.
- scripts/test-build-regressions.sh (38 checks, wired into
  `make check`): R1 font generation (success, determinism, stripped
  env, 95 glyphs, precise missing-asset/--font diagnostics); R2 build
  failure propagation (broken xw-panel.c -> make fails, no binary for
  the failed target, quick-start after PARTIAL build refuses); R3 the
  Artix regression itself — clean build + full session run with every
  system font hidden via unprivileged mount namespace (skips honestly
  where userns is unavailable); R4 dev-session failure modes (unbuilt
  tree, early-crashing session manager, no leaked processes); R5 shell
  compatibility: sh/bash/zsh syntax checks, env.sh sourcing under zsh,
  dev-session refusal + FULL session run under zsh, and a grep guard
  for the zsh error family ("number expected", "unknown condition",
  "no matches found", "bad math"). zsh detection falls back to a
  rootless-extracted .toolchain/zsh-root (zsh 5.9 verified here).
- Docs: BUILDING.md (quick-start fail-fast notes, bundled-font
  requirement row, XW_FONT knob, troubleshooting entries for the new
  diagnostics incl. an honest zsh section), README.md quick-start +
  status counts (33 + 71 + 38), DEPENDENCIES.md (no font package
  needed), TESTING.md (new suite + counts), THIRD-PARTY-LICENSES.md
  (redistribution record).
- Audited C code for system()/popen: none. execvp with explicit argv
  for compositor/power; /bin/sh -c only for XDG autostart Exec strings
  (spec-required, XW_SHELL-overridable) — unchanged, documented.
- Full verification on the final tree, clean environment (unset
  LD_LIBRARY_PATH/PKG_CONFIG_PATH/XDG_RUNTIME_DIR): make check =
  33/33 in-process + 71/71 process-level + 38/38 build regressions,
  0 skipped; make asan = PASS (ASan+UBSan+LSan, all suites, release
  restored); dev-session.sh --logout full round trip rc=0; nested X11
  session under Xvfb: backend auto-selection (DISPLAY, no
  WAYLAND_DISPLAY -> x11), compositor --backend x11, panel
  autostarted, x11probe reads back rendered pixels, clean logout rc=0.
- Honest backend status (verified by running, not by claim): plain
  `make` + dev-session.sh = HEADLESS backend (real compositor+WM+
  panel over a real Wayland socket, pixman software rendering, no
  visible output surface). On X11/XLibre with $DISPLAY:
  `build/bin/xw-session --nested` = nested x11 backend — the whole
  desktop as a window inside the X session (verified under Xvfb via
  the standard X11 protocol XLibre also implements; XTEST input
  verified). Nested Wayland backend works inside a Wayland parent.
  Direct DRM/KMS hardware output: NOT implemented (ROADMAP Phase 4).

Stage Summary:
- The Artix quick-start failure class is eliminated: no system font is
  ever searched; the build is distro-agnostic and deterministic.
- dev-session.sh and `make check` are self-contained (no dependence on
  the parent shell having sourced env.sh) and fail fast with precise
  diagnostics; a failed or partial build can no longer cascade into
  half-started sessions.
- All entry scripts verified under zsh 5.9 (full session runs), bash,
  dash; the one real zsh hazard found (word-splitting) is fixed and
  regression-guarded; the reported "zsh: number expected" is provably
  not producible by repo scripts (documented in BUILDING.md
  troubleshooting with the reproduction hint `zsh -f`).
- New artifacts: assets/fonts/{DejaVuSans-ascii.ttf,LICENSE-
  DejaVuSans.txt,README.md}, scripts/{test-build-regressions.sh,
  verify-font-subset.py,wire-font-asset-mk.py,wire-check-target-mk.py,
  wire-sysroot-ldlib-mk.py}, .toolchain/zsh-root (gitignored, local).
- Suite totals: 33 in-process, 71 process-level, 38 build regressions,
  sanitizer-clean.

---
Task ID: artix-link-fix
Agent: chief-autonomous-developer
Task: Fix the post-font-fix build failure on Artix/XLibre:
`ld: build/lib/libxw.a(xw-input-libinput.o): undefined reference to
'udev_new@@LIBUDEV_183'` / `libudev.so.1: DSO missing from command
line` — as a dependency-graph/propagation problem, not a one-off
flag, per the user's 13 requirements.

Work Log:
- Root cause audit: src/libxw/xw-input-libinput.c calls udev_new()/
  udev_unref() DIRECTLY (it creates the udev seat context handed to
  libinput_udev_create_context). Only this file references libudev
  symbols; it lands inside libxw.a. The final link carried only
  `pkg-config --libs libinput` = -linput: upstream libinput.pc does
  not hand out -ludev (Requires.private), and modern ld defaults to
  --no-copy-dt-needed-entries, refusing symbols from indirect
  DT_NEEDED DSOs. The sandbox never caught it because the local
  sysroot's libinput.pc (Debian-family, patched by
  bootstrap-sysroot.sh) declares a PUBLIC `Requires: libudev`, which
  makes pkg-config emit -ludev transitively. Verified: nm shows
  udev_new/udev_unref undefined in xw-input-libinput.o; libinput.so
  exports only libinput_udev_* (its own API), not udev_*.
- Makefile fix (byte-precise scripts/wire-udev-link-mk.py, 9
  idempotent patches; NEVER the text editor — the Edit tool was
  caught converting all 77 recipe TABs to spaces mid-session, the
  exact hazard the worklog warned about; recovered via
  git checkout + re-running the wire script):
  1. The libinput feature now detects, validates and links BOTH
     pkg-config modules (libinput, libudev): XW_LIBINPUT=1 gives a
     precise error per missing module; auto enables only when BOTH
     resolve and the degrade notice names the missing one; 0 stays
     off.
  2. CFLAGS_LIBUDEV/LDLIBS_LIBUDEV defined; the
     xw-input-libinput.o compile rule adds the udev cflags
     (<libudev.h> is included directly) and its stale comment
     ("udev context comes through libinput itself") was corrected.
  3. Both links that consume libxw.a (xw-compositor, run-tests) put
     $(LDLIBS_LIBUDEV) directly after $(LDLIBS_LIBINPUT): after the
     archive that references the symbols, after -linput (static-safe
     order), before xkbcommon/pixman. Client binaries (libxwcl only)
     stay clean — no global flag anywhere.
  4. LIBINPUT_FOUND now probes with `--libs`, not `--exists`:
     pkgconf's --exists resolves Requires.private, so with only the
     libudev dev files missing, upstream libinput.pc makes --exists
     report LIBINPUT itself as missing — misdirecting the
     diagnostic. --libs parses exactly what the link consumes.
  5. New resolved-feature stamp guard (build/.features, mirrors the
     PROFILE guard): switching XW_X11/XW_LIBINPUT across a
     resolved-state change over a populated tree now fails loudly
     with "make clean" — without it, libxw.a silently kept a
     udev-using member while linking without -ludev (the same DSO
     failure class). auto<->1 with the same outcome never forces a
     clean.
- Regression coverage (test-build-regressions.sh R6, 20 new checks —
  suite 38 -> 58, all via `make check`):
  R6a builds a hostile pkg-config dir with UPSTREAM-shaped
  libinput.pc/libudev.pc (no -ludev handed out; PKG_CONFIG_LIBDIR
  makes it the only search root so system .pc files cannot mask it on
  real distros) with the full .pc closure (transitive Requires like
  wayland-server->libffi stay resolvable) and re-links the final
  executable — the literal Artix failure; asserts rc=0 AND -ludev
  present AND ordered after libxw.a/-linput on the link command.
  R6b: XW_LIBINPUT=1 without libudev.pc -> hard error naming libudev.
  R6c: auto without libudev.pc -> config reports the backend off with
  a libudev notice. R6d: =0 -> off. R6e: feature switching on a
  populated tree refuses with make-clean guidance. R6f: full
  XW_LIBINPUT=0 build from a clean copy — archive excludes
  xw-input-libinput.o, binary references no udev/libinput symbols.
  R6g: scripts/test-link-deps.sh — symbol-coverage audit: for every
  final executable it takes the exact link command from `make -n`,
  parses objects/archives/-l/-L, and fails if any undefined symbol of
  the inputs is not provided by a library/object on that same line
  (C runtime + sanitizer runtimes accounted). Catches the whole class
  for any future dependency, not just udev. Two audit subtleties
  fixed by testing: `nm -D` succeeds with EMPTY output on relocatable
  .o (the || fallback never fired), and glibc ifunc symbols print as
  lowercase 'i' (strcmp/memcpy) — both handled.
  Session 4b in test-session.sh (4 checks, suite 71 -> 75): starts
  the compositor with -I libinput — the udev-seat code path whose
  symbols broke the link — asserting it logs its mode or an honest
  refusal and exits with discipline (portable: udev-less machines
  pass via the honest-failure branch).
- scripts/quickstart-clean.sh: the literal documented quick-start
  (clone + env.sh + make + dev-session --logout) on a pristine git
  clone — the req-13 gate; verified rc=0 here.
- Docs: DEPENDENCIES.md libudev is now a first-class direct
  build/link dependency (row + prose + the addition-bar note); the
  full "why" (upstream pc shape, --no-copy-dt-needed) is recorded.
  BUILDING.md: libudev dev requirement row, XW_LIBINPUT knob
  semantics (both dev sets + make-clean rule), sysroot bootstrap
  note, libudev dev packages added to ALL distribution examples
  (systemd-libs on Arch/Artix family, libudev-dev on Debian/Ubuntu,
  systemd-devel on Fedora/openSUSE, libudev-devel on Void, udev-dev
  on Alpine), and troubleshooting entries for the new diagnostics
  plus "undefined reference/DSO missing from command line" (what it
  means, why the released build does not produce it, what to check
  after local hacks). TESTING.md/README.md counts updated.
- Verification on the final tree, stripped environment: clean
  rebuild from scratch rc=0 (link line inspected: libxw.a libxwcl.a
  ... -linput -ludev ... -lxkbcommon); make check = 33/33 + 75/75 +
  58/58, 0 skipped; make asan = PASS (ASan+UBSan+LSan incl. the new
  libinput-startup checks; release restored). Runtime: dev-session
  --logout round trip rc=0 (headless, panel autostarted); nested X11
  under Xvfb: backend x11, x11probe reads back rendered pixels, XTEST
  injects Ctrl+Alt+D, shortcut engine dispatches action 24, clean
  exit 0; `xw-compositor -I libinput` runs the REAL udev-seat path —
  "input: libinput udev mode, seat 'seat0'" (udev_new -> context ->
  assign_seat all executed), clean SIGTERM exit 0. No-root install
  smoke test to a $HOME-style prefix passes.

Stage Summary:
- The Artix link failure class is closed: libudev is an explicit,
  validated, correctly-ordered direct dependency of the libinput
  feature; the hostile-pc regression reproduces the user's exact
  environment shape (upstream libinput.pc) inside `make check`, so
  the sandbox can no longer mask this family of bugs.
- The systematic audit (test-link-deps.sh) + feature-stamp guard +
  LIBINPUT three-state semantics give the build system fail-fast
  coverage for the entire "missing/misordered linker dependency"
  class, not just this instance.
- Honest backend status (unchanged, verified by running): plain
  build + dev-session = HEADLESS backend (real compositor+WM+panel
  over a real Wayland socket, pixman software rendering); with
  $DISPLAY = nested X11 window (verified under Xvfb incl. XTEST
  input); the libinput real-input source starts in udev seat mode
  when asked; direct DRM/KMS scanout remains Phase 4 (not built).
- New artifacts: scripts/{wire-udev-link-mk.py,test-link-deps.sh,
  quickstart-clean.sh}; suite totals now 33 in-process + 75
  process-level + 58 build regressions, sanitizer-clean.

---
Task ID: nested-panel-invisible
Agent: chief-autonomous-developer
Task: Fix the nested-session panel invisibility: "the compositor window
appears and the mouse cursor works, but the xw-panel is completely
invisible" while the session reports the panel autostarted. Trace the
complete path, fix compositor or panel, add regression tests, verify
the full nested session end-to-end.

Work Log:
- Reproduction ladder (fresh Xvfb; no WM -> panel VISIBLE, so the
  report needed a real WM): wrote tests/miniwm.c (minimal reparenting
  WM that resizes clients like openbox/xfwm4) and tests/panelprobe.c
  (pixel probe for panel colors, background extent, right-edge
  coverage, and the software cursor at an XTEST warp point). Under the
  WM the panel appeared at STALE width and later crashed — leading to
  three independent root causes plus two more crashers found on the
  way:
  1. X event starvation (the resize never reached the compositor):
     minimal reproducer tests/fdtest2.c — with an XPutImage present
     (any large request) the X server defers flushing subsequent
     event batches of that connection server-side (verified: kernel
     socket queues 0/0 for seconds while Reparent/Configure/Map/Expose
     are pending), and Xlib's _XReply read-ahead drains the socket
     into its own queue on round trips, so a pure epoll on
     ConnectionNumber never fires. A delayed XSync round trip
     materialized all events (XPending=5). Fix: xb_watchdog in
     xw-backend-x11.c — 50ms timer doing one XSync (the reply forces
     the server to flush) + a shared XPending drain; the fd callback
     keeps instant delivery (same reasoning as GTK's X11 backend
     polling XPending instead of trusting select()).
  2. Layer surfaces were never reconfigured on output resize:
     xw_layer_reconfigure_output() (xw-layer-shell.c) relayouts +
     re-sends configure for anchored surfaces when the output
     geometry changes; xw_output_resize() calls it. The panel now
     learns the new width and recommits (spans the WM geometry).
  3. Client crash #1 (the panel SEGV, exit 139): libxwcl freed the
     wl_output listener state in out_done while the listener stayed
     attached — the second output announcement (the resize
     reannounce, newly reachable) called into freed memory. The state
     now lives for the connection's lifetime and w/h update on every
     done.
  4. Client crash #2 (buffer use-after-free window): pool_destroy
     destroyed wl_buffers that were still the surface's committed
     content (the server-side wl_shm_buffer dies with the resource;
     the renderer keeps reading it until the next commit). Fixed by
     pool retirement: the old pool is destroyed only after the
     replacement buffer is committed (guarded for allocation
     failure), and window/layer destroy now destroys surfaces before
     pools.
  5. xw-exit segfaulted (exit 139, caught by the new exit logging)
     on any output != 720px tall: draw() used a hardcoded 720 height
     bound against an output-sized layer buffer -> write past the shm
     mapping. Geometry now derives from the configure (dialog_rect);
     it only ever worked on the headless 1280x720 default.
  6. Session observability: xw-session reaped children with statuses
     discarded — a crashed panel was indistinguishable from a running
     one (the exact report). Autostart/spawned exits are now logged
     with name, pid, wait status, runtime, and a 127-specific
     Exec=-line hint. dev-session.sh's blind "panel autostarted" echo
     replaced by a real pgrep check after a settle delay.
- Tests: in-process layer-shell-resize-reconfigure (protocol suite,
  33 -> 34); session 5 rebuilt as the full nested regression (Xvfb +
  miniwm + real panel autostart + panelprobe: alive, no child exits,
  panel visible + spans resized width + software-cursor path + no
  crash lines + clean logout, 75 -> 84 process checks); new session 7
  (autostart exit logging incl. the 127 hint); panelprobe/miniwm/
  fdtest2 wired into the Makefile behind X11_ON via byte-precise
  scripts/wire-nested-tests-mk.py; new xw-demo client (the Makefile
  already expected it) as the canonical non-modal "normal app".
- Sandbox bootstrap fix: the sysroot needs libinput's recursive
  runtime deps (libmtdev1t64, libgudev-1.0-0) or the final link fails
  (DSO missing g_udev_*/mtdev_*); added to bootstrap-sysroot.sh's
  download list.
- Docs: ARCHITECTURE.md (X11 event-delivery watchdog rationale,
  wl_buffer lifetime contract, layer reconfigure-on-resize, output
  announcement not one-shot), BUILDING.md troubleshooting ("window
  appears but panel does not" -> read the exit log, 127 = Exec path),
  TESTING.md (new coverage + regression-policy entries), README
  counts (34 + 84 + 49-58).
- Final verification of the whole checklist on the rebuilt tree,
  Xvfb + reparenting WM, `build/bin/xw-session --nested` with panel
  autostart: panel + clock + workspace buttons + tasklist + exit
  button all present in pixels; panel spans the WM-resized 700x450
  window; XTEST warp shows the COMPOSITOR's software cursor (the X
  cursor of our window is invisible by construction -> the visible
  cursor provably went through the compositor input path); XTEST
  click on the exit button spawns xw-exit end-to-end; with xw-demo
  open the panel stays visible and the tasklist gains a task button
  (btn pixel count 2432 -> 6483); clean logout rc=0, no leftover
  processes.
- Full suites on the final tree: 34/34 in-process, 84/84
  process-level, 49/49 build regressions (+1 environmental skip: zsh
  absent in this sandbox; 58 with zsh installed), make asan = PASS
  (ASan+UBSan+LSan, release restored).

Stage Summary:
- The nested-session panel invisibility is closed end-to-end. It was
  not one bug: event starvation (server deferral + Xlib read-ahead),
  missing layer reconfiguration on resize, a client-side use-after-
  free on output reannouncement, a buffer-lifetime violation, a
  hardcoded dialog height, and silent child-exit supervision — any
  one of which could make the panel invisible or dead while the
  session claimed success.
- "Panel autostarted" is no longer a claim the system makes without
  evidence: the session logs child exits with status/runtime/hints,
  dev-session verifies the process, and the nested regression
  verifies actual pixels through a real reparenting WM.
- Honest backend status (unchanged, now pixel-verified under a WM):
  plain build + dev-session = HEADLESS; with $DISPLAY = nested X11
  window (panel, tasklist, dialog, cursor, interaction verified);
  nested Wayland works; direct DRM/KMS remains Phase 4.
- New artifacts: tests/{panelprobe,miniwm,fdtest2}.c,
  scripts/wire-nested-tests-mk.py, src/clients/xw-demo.c; suites:
  34 in-process + 84 process-level + 49-58 build regressions,
  sanitizer-clean.

---
Task ID: 2026-09-01-repush
Agent: main (Super Z)
Task: Configure GitHub PAT, push eb57104, re-verify tree from bare state

Work Log:
- Container came back bare (build/ and .toolchain/ gone, git repo
  intact, working tree clean after core.fileMode=false for the
  rwxr-xr-x mount noise).
- PAT configured at ~/.git-credentials (600, outside the repo, never
  committed); push eb57104 -> origin/main verified via ls-remote
  (remote tip == local HEAD).
- Rebuilt from zero: bootstrap-sysroot.sh (apt download, rootless,
  wayland 1.23.1), . scripts/env.sh, make — clean with X11 backend +
  libinput, -Werror, no warnings.
- Re-ran all suites on the fresh tree: 34/34 in-process, 84/84
  process-level (nested panel pixel regression included, under Xvfb),
  49 passed / 0 failed / 1 env-skip build regressions.

Stage Summary:
- The nested-session panel invisibility fix (eb57104) is pushed,
  reproducible from a bare container, and fully re-verified in this
  session. Repo state: clean at eb57104 on origin/main.

---
Task ID: 2026-09-01-session-lock
Agent: main (Super Z)
Task: ext-session-lock-v1 + ext-idle-notify-v1 (screen lock) — server,
client library, xw-lock binary, tests, docs

Work Log:
- Server: src/libxw/xw-session-lock.c (new) — full protocol state
  machine (PENDING/ACTIVE/RELEASED + owner-dead takeover), security
  gates wired into xw-render.c (locked render = opaque blank + lock
  surfaces + cursor ONLY, pixel-verified) and xw-seat.c (input to lock
  surfaces only; shortcuts/interactive/popups dead while locked),
  `locked`-event flush from the post-present hook (spec ordering),
  grace timer ($XW_LOCK_TIMEOUT_MS) for never-committing clients,
  output add/resize/remove hooks, strict protocol errors (all 9),
  zombie-lifetime rule (lock surface objects outlive the lock object
  per spec; wl_map id-order teardown focus clearing — UAF found by
  ASan in the child test, fixed).
- Server: src/libxw/xw-idle.c (new) — ext-idle-notify v2, per-note
  event-loop timers over per-seat activity timestamps (all input entry
  points call xw_idle_activity), idled/resumed + re-arm,
  timer_update(0)-DISARMS pitfall handled (1ms floor for already-
  elapsed deadlines — caught by the independence test).
- Client: libxwcl xwc-lock.c (xwc_lock/xwc_idle) reusing the shared
  pool machinery via thunks; registry binds the two new globals;
  xw-lock.c client: passphrase prompt, masked input, constant-time
  compare, wrong-pass feedback, unlock+roundtrip, --idle autolock,
  refuses to start without a passphrase file. Ctrl+Alt+L was already
  wired to the `lock` action -> cmd_lock "xw-lock".
- Tests: tests/suite/test_lock.c — 8 in-process (lifecycle, input
  gate, denial, client death + takeover, timeout, resize, idle,
  raw-protocol commit-before-first-ack) + 3 child-process tests with
  the real binary (unlock flow incl. wrong passphrase, SIGKILL while
  locked stays locked + takeover, --idle autolock). 34 -> 45 in-process.
- Client NULL-global ordering bug found + fixed (configure fires
  during xwc_lock_create before g_lock is assigned — draw must use
  the callback's lock parameter). Child-test race found + fixed
  (waiting on the engaged gate races the child's handshake; wait for
  the locked event via the new white-box xw_session_lock_locked()).
- Full verification: 45/45 in-process, 84/84 process-level, 49/49
  build regressions (+1 env skip), ASan/UBSan/LSan clean (44/44 under
  sanitizers pre-release-restore, exit-time leak of the child display
  is the wl_display finalization, benign).
- Docs: ROADMAP (M4 entries + honest gaps: file-based auth not PAM,
  single-output client lib), ARCHITECTURE (security model + lifetimes
  + idle subtleties), TESTING (45 + new coverage entries), README
  (component table + counts), BUILDING (XW_LOCK_* env vars), TODO.

Stage Summary:
- The session can now be locked (Ctrl+Alt+L or xw-lock [--idle N]) and
  unlocked only with the passphrase; killing the locker keeps the
  session locked; a second locker takes over. Security properties are
  server-enforced and pixel-verified.
- Remaining honest gaps: PAM unlock backend, multi-output lock client.

---
Task ID: 2026-09-02-phase4
Agent: main (Super Z)
Task: Phase 4 — real TTY/DRM/KMS sessions: seat-provider abstraction
(libseat + built-in seatd client + direct VT), DRM backend, backend
selection, session environment, tests, docs

Work Log:
- Rebuilt the environment from a bare container: bootstrap-sysroot.sh
  extended with libdrm-dev/libdrm2, libseat-dev/libseat1, seatd
  (Debian trixie ships libseat 0.9.1 — real library available for
  cross-validation); baseline build + full suite green before changes.
- Studied the seatd 0.9.1 wire protocol from upstream source (protocol.h
  + libseat's seatd backend): OPEN_SEAT handshake, OPEN_DEVICE with
  SCM_RIGHTS fds, DISABLE_SEAT -> client ack -> ENABLE_SEAT lifecycle,
  error transport as errno values, CLOSE_SEAT.
- src/libxw/xw-session-seat.c (new, ~1100 lines): the seat/session
  abstraction. Three providers behind one vtable: (1) external libseat
  (build-time optional XW_LIBSEAT, wraps logind/elogind/seatd); (2)
  built-in seatd wire-protocol client — plain libc, blocking
  request/response with background-event queueing (order-preserving),
  MSG_NOSIGNAL writes (a dead seat manager must surface as EPIPE, not
  SIGPIPE); (3) direct-VT provider — /dev/tty takeover with
  KD_GRAPHICS + VT_PROCESS, SIGUSR1/SIGUSR2 through the event loop's
  signal sources, VT_RELDISP acking, full termios/KD/VT-mode
  restoration. AUTO probes libseat -> seatd -> direct with per-failure
  DEBUG logs and one honest combined ERROR diagnostic; explicit
  providers never fall back.
- src/libxw/xw-backend-drm.c (new, ~950 lines): the DRM/KMS backend.
  DRM-independent planning section (xw_drm_pick_mode: preferred ->
  largest -> highest refresh; connector naming; xw_drm_plan_crtc:
  firmware-pairing reuse then possible-mask fallback) tested without
  hardware. KMS section: /dev/dri/card* enumeration (no hardcoded
  card, prefers cards with connected monitors), drmSetMaster with
  driver-named diagnostics, per-connector outputs at the preferred
  mode, two dumb buffers per output (XRGB8888 = our a8r8g8b8 layout)
  with page-flip event pacing (parked frames flip on vblank),
  logged fallback to immediate updates on flip-rejecting drivers,
  udev hotplug (connector removal tears outputs down honestly; new
  connectors logged as next-start), session disable = drop master +
  suspend input, enable = re-master + full damage, destroy = restore
  every saved CRTC (including previously-disabled ones) + drop master
  + release the device through the seat.
- Compositor integration: XW_BACKEND_DRM + XW_SEAT_PROVIDER enums in
  xw.h; xw_compositor_create opens the seat BEFORE the DRM backend,
  destroys it after (backend teardown releases devices through it);
  input AUTO mode now opts in for DRM (real session needs real
  input). Compositor binary: -B drm, -P/--seat-provider, -t/--seat,
  $XW_SEAT_PROVIDER override.
- xw-input-libinput.c: open_restricted/close_restricted now route
  through the seat provider (fd -> device-id map for close), plus
  libinput_suspend/resume for the session lifecycle.
- xw-session.c: --backend=drm|x11|wayland|headless (+ -B), --verbose
  (compositor at INFO instead of -q), backend resolution logic
  (explicit drm NEVER falls back and never enters the restart loop;
  AUTO: TTY+KMS -> drm, TTY no-KMS -> headless with an explained
  downgrade, graphical parent -> headless unless -N), real-session
  environment for children (XDG_SESSION_TYPE=wayland,
  XDG_CURRENT_DESKTOP=XFCE, XDG_SESSION_DESKTOP=xfce, no DISPLAY
  leak), duplicate-service detection for autostart (scans
  /proc cmdline basenames; duplicates are skipped with a note, not
  treated as crashes — re-login/restart flows).
- Makefile: XW_DRM (libdrm+libudev) and XW_LIBSEAT knobs with the
  same auto/1/0 diagnostics pattern; feature stamp extended
  (x11/libinput/drm/libseat); libudev decoupled from libinput (it is
  a direct dep of EITHER libinput or DRM — caught by the
  XW_LIBINPUT=0 clean-build regression); xw-backend-drm.c and
  xw-session-seat.c always compiled (planning helpers + seat are
  libdrm-free); make config shows the new features.
- Tests: tests/suite/test_seat.c — a forked mock seatd server speaking
  the real protocol; handshake, SCM_RIGHTS device opens, close,
  switch, the DISABLE->ack->ENABLE dance delivered as background
  traffic behind a blocking request, server errors, garbage,
  hangup-at-connect, direct no-VT refusal, AUTO-all-broken refusal,
  and a cross-check where UPSTREAM libseat (LIBSEAT_BACKEND=seatd)
  opens a seat against our mock — proving protocol compatibility both
  ways. tests/suite/test_drm.c — planning logic (8 tests).
  tests/mockseatd.c + scripts/test-session.sh session 8 (19 checks):
  backend selection matrix, honest failure taxonomy, and the
  compositor acquiring a seat through the real protocol end-to-end
  (mock listening, compositor logs seat-mock, then fails at the
  DRM stage because CI has no /dev/dri — and says exactly that).
- Two latent leaks exposed by the new coverage (heavier tests recycle
  the stack slots LSan uses for reachability): raw-protocol lock
  tests never destroyed their registry/bind proxies (fixed in the
  tests), and xwc_lock_destroy skipped the lock destroy on the
  `finished` (denied) path and left held-lock proxies allocated after
  disconnect (fixed in libxwcl: dead locks are destroyed, held locks
  are freed with a request-less wl_proxy_destroy — the server still
  sees the connection die; spec-verified by the lock suite).
- Build-system surgery note: the conversational edit tool mangled
  Makefile recipe tabs into spaces (make: "missing separator"); the
  Makefile changes were re-applied through scripted edits with exact
  byte control and verified parseable before each build.
- Docs: DEPENDENCIES.md (libdrm/libseat/seatd rows, "Seat and session
  management" concept table + provider description), BUILDING.md
  ("Real TTY session" section: launch, provider table, selection
  rules, per-init seatd setup examples, honest hardware-dependent
  status; distro package lists gained libdrm/seatd/libseat with
  honesty notes where packaging is uncertain; XW_DRM/XW_LIBSEAT knobs;
  seat/DRM troubleshooting entries), ARCHITECTURE.md (seat provider
  diagram + design rules; DRM backend behavior), TESTING.md (new
  coverage, the manual hardware checklist, three regression-policy
  entries), ROADMAP.md/TODO.md (Phase 4 done except physical hardware
  verification), README.md (four backends, real-session quick start,
  updated counts).
- Full verification on the final tree: 63/63 in-process (45 -> 63),
  103/103 process-level (84 -> 103), 50/50 build regressions
  (49 -> 50 + 1 env skip), make asan = PASS (ASan+UBSan+LSan clean,
  release restored), zero warnings under -Werror.

Stage Summary:
- The compositor is now a real TTY desktop candidate: seat acquisition
  through three interchangeable providers (no systemd/elogind/seatd/
  display-manager assumptions, no root), DRM/KMS scanout with page
  flips, real input through the seat, honest backend selection with
  fatal explicit-DRM semantics, and a real-session environment for
  children.
- Everything testable without hardware is tested — including the
  seatd wire protocol against upstream libseat as an oracle — and the
  ioctl paths are documented as the manual hardware checklist
  (TESTING.md) instead of being faked.
- Honest gaps: physical DRM verification (needs a real machine;
  checklist written), live modeset of newly plugged monitors,
  atomic modesetting, hardware cursor planes, GL/EGL rendering.

---
Task ID: 2026-09-02-phase4-push
Agent: main (Super Z)
Task: push the Phase 4 commits

Work Log:
- Push attempted: `git push origin main` fails — the container came
  back bare and ~/.git-credentials (configured in the previous
  session with the user's PAT) was wiped with it. The credential
  helper is still `store`; only the token is missing.
- The four Phase 4 commits are complete and verified locally at HEAD
  (22b4331): 461d43c seat+drm, a405b26 session backend selection,
  4bbda32 tests, 22b4331 docs. Full suite green on the committed
  tree: 63/63 in-process, 103/103 process-level, 50/50 build
  regressions, `make asan` PASS.

Stage Summary:
- Push is blocked on credentials only. Re-provision the PAT (never
  echo it, never commit it) with:
    git config --global credential.helper store
    printf 'https://<TOKEN>@github.com\n' > ~/.git-credentials
    chmod 600 ~/.git-credentials
    git push origin main
- Until then the work is safe locally on main ahead of origin by 4
  commits.

---
Task ID: 2026-09-02-phase4-push-complete
Agent: main (Super Z)
Task: re-provision the PAT, push Phase 4, restore and re-verify the
build environment

Work Log:
- Container had come back bare again: ~/.git-credentials wiped AND
  .toolchain gone (build could not have run). Re-provisioned the
  credential store from the user's re-sent PAT (0600, outside the
  repo, never echoed into any committed file).
- `git push origin main`: 007f4ed..08c6367 — all five Phase 4
  commits (seat+drm, session backend selection, tests, docs,
  worklog) are on GitHub; local main == origin/main, tree clean.
- Re-bootstrapped the rootless sysroot with
  scripts/bootstrap-sysroot.sh (wayland 1.23.1, xkbcommon, libinput,
  libudev, libdrm, libseat, seatd; wayland-scanner verified).
- Full re-verification of the SAME tree at HEAD in the fresh
  environment: `make clean && make` — all four features
  (XW_HAVE_X11_BACKEND / XW_HAVE_LIBINPUT / XW_HAVE_LIBSEAT /
  XW_HAVE_DRM_BACKEND), zero warnings under -Werror;
  `make check` 63/63 in-process; process-level 103/103;
  build regressions 50/50 (+1 skip); `make asan` PASS
  (ASan+UBSan+LSan clean, release restored).

Stage Summary:
- Phase 4 is pushed and independently re-verified after a full
  environment loss. No code changes were needed — the committed tree
  rebuilt green from a bare container, which is itself the
  bootstrap-sysroot.sh disaster-recovery path working as designed.
- Remaining gaps are unchanged and hardware-gated: physical DRM
  verification (manual checklist in TESTING.md), live modeset of
  newly plugged monitors, atomic modesetting, hardware cursor
  planes, GL/EGL rendering.

---
Task ID: 2026-09-02-input-path-instrumentation
Agent: main (Super Z)
Task: Instrument the physical input path end to end per the real-TTY
bug contract (cursor visible, mouse dead), fix the first confirmed
defects found during the audit, and make the panel chain independently
traceable.

Work Log:
- Recovered an unverified auto-checkpoint commit (UUID message) that
  contained the planned instrumentation; found it did not build
  (missing <errno.h> in test_input.c) — fixed, rebuilt clean.
- Audited the full input wiring: compositor creates the seat session
  BEFORE xw_input_libinput_create (open_restricted can go through the
  provider); the libinput fd IS wired via wl_event_loop_add_fd;
  on_libinput_fd -> libinput_dispatch -> drain loop verified.
- CONFIRMED DEFECT #1: drain_libinput() never called
  libinput_event_destroy() — the libinput contract's destroy step was
  missing; every event object leaked in real sessions (CI drives the
  translation handlers directly, so tests never saw it). Fixed.
- CONFIRMED DEFECT #2 (robustness): xwc_layer_create dereferenced a
  NULL zwlr layer_shell global when the panel connected to a
  compositor without layer-shell (wrong WAYLAND_DISPLAY) — now a
  clean error.
- Added per the contract: structured input-acquisition failure report
  (provider/seat/session state/backend/node counts/last error/
  legitimate fixes — never root/chmod); device-open logs with fd +
  seat device id proving the compositor consumes the seat-granted fd;
  three-level pointer trace + KEY/BUTTON/AXIS event logs; seat
  environment report (libseat/seatd socket/logind/elogind/dbus);
  libseat's own backend logs surfaced.
- Panel track: XW_PANEL_TRACE chain trace (auto-on with
  xw-session --verbose), compositor-side layer map log with stored
  namespace, panel launched as a first-class session component
  (--no-panel / XW_PANEL_CMD=none).
- Verified in-container: headless+panel smoke run shows the complete
  chain in order; missing-device run shows the acquisition report.
- Full battery: 64/64 in-process (+1 new acquisition-report test),
  103/103 process-level, 50/50 build regressions, make asan PASS.
- BUILDING.md: per-provider input-permission table + diagnostic
  contract (the most likely real-machine fix: `input` group for
  direct VT sessions — /dev/input/event* is root:input 0660 on bare
  TTY logins).

Stage Summary:
- Commit f5c7324 "input: full physical-input path instrumentation +
  event-destroy fix" (amended from the broken checkpoint).
- The code path is now fully observable: a single real-TTY run of
  `xw-session --backend=drm --verbose` will pinpoint the exact step
  where physical input dies (seat open? device open? events? motion?
  damage?) and the panel's exact broken step.
- Awaiting the user's hardware run: most probable cause given
  symptoms (DRM scanout OK + input EACCES + no seat daemon on a bare
  TTY) is direct-provider EACCES on /dev/input/event* — the report
  will name it with the legitimate fix (usermod -aG input + re-login,
  or run seatd and join the seat group).

---
Task ID: 2026-09-02-elogind-first-provider-chain
Agent: main (Super Z)
Task: User report — mouse still dead on a clean-boot TTY login (XFCE
never running); MORE permission denied after logging out of an XFCE
Xlibre session; requested seat provider order elogind first, then
seatd.

Work Log:
- Container had been reset again (sysroot + credentials wiped);
  re-ran scripts/bootstrap-sysroot.sh (proven disaster-recovery path).
- Verified the sysroot's Debian libseat1 ships the logind backend:
  DT_NEEDED libsystemd.so.0, org.freedesktop.login1 D-Bus strings,
  XDG_SESSION_ID/TakeControl/GetSession — elogind implements this
  exact API, so the path is real, not theoretical.
- Downloaded upstream libseat source and confirmed the smoking gun:
  libseat's internal backend order is seatd FIRST, logind SECOND —
  the opposite of the requested preference. Also found the supported
  override: $LIBSEAT_BACKEND forces one named backend.
- Implemented the requested chain in xw_seat_session_open AUTO:
  elogind (libseat pinned to logind via LIBSEAT_BACKEND, save/
  restored) -> seatd socket (built-in client) -> direct VT ->
  last-resort libseat unpinned. The last-resort step exists because
  the old chain's libseat attempt could fall to its builtin backend
  (degenerate VT-less seat from the user's own perms) — that is very
  likely how the user's DRM scanout worked while input stayed EACCES
  (video group without input group). Preserved at the tail, after the
  requested order, so the input acquisition report explains reality.
- New XW_SEAT_PROVIDER_ELOGIND + -P elogind/logind aliases, forced
  semantics with a diagnostic naming the three usual causes
  (unregistered login/PAM, inactive session, unreachable d-bus).
- Diagnostics: seat environment report now prints $XDG_SESSION_ID
  and its meaning; input acquisition failure states whether libseat
  is compiled into the build (silent compile-out is a build problem
  the log must name).
- Fixed a regression found by test-session session 8: the first
  version of the chain skipped the libseat catch-all and the
  compositor failed at seat acquisition before DRM enumeration
  ("no DRM subsystem" never printed). The last-resort step restores
  the old container behavior under the new order.
- Full battery: 64/64 in-process, 50/50 build regressions,
  108/108 process-level (5 new elogind checks), ASan+UBSan+LSan PASS.
- Docs: BUILDING.md provider table + order rationale + the
  "more permission denied after graphical logout" explanation
  (elogind session-active semantics); README seat paragraph.

Stage Summary:
- Commit 7caebac on main. elogind is now tried first and pinned
  deterministically; every decision is visible under --verbose.
- The user's symptom set (DRM works, input dead, worse after logout)
  points to either an unregistered elogind session (PAM) or a build
  without libseat — both are now self-identifying in the logs.
  Next hardware run with --verbose will name the exact cause.

---
Task ID: 3b1d2fa round
Agent: main
Task: user reported "mouse still doesn't move" on the real TTY; uploaded xw-tty.log

Work Log:
- Read the uploaded log: it matches HEAD (7caebac) built WITHOUT
  libseat. The chain worked exactly as designed: elogind detected
  (session registered, XDG_SESSION_ID=1, active), but the elogind step
  runs through libseat -> skipped; seatd socket absent; direct VT ->
  EACCES on all 18 /dev/input nodes; DRM fine (uaccess covers /dev/dri,
  not keyboards/mice). Machine configured correctly, build missing one
  package. Root cause CLOSED, not a code bug.
- Restored the bare container build env (bootstrap-sysroot.sh,
  rootless; libseat 0.9.1) and re-verified HEAD: 64/64, 108/108,
  50/50 — the "install libseat + rebuild" path is proven end to end.
- Second finding in the same log: no session d-bus (TTY logins have
  none) -> libdbus X11 autolaunch failures, pipewire/RTKit degraded,
  wireplumber skipping components, xfsettingsd exit 1.
- xw-session: start_session_dbus() — one dbus-daemon as a supervised
  child on $XDG_RUNTIME_DIR/bus before the compositor, address
  exported to all children; live bus reused (env address or standard
  path, never killed), stale exported address replaced loudly,
  fail-open (never blocks the session), teardown stops only the
  daemon we started, also on the compositor-failure path (first
  draft leaked an orphan there — caught by manual repro);
  dbus-update-activation-environment best-effort so dbus-activated
  services see WAYLAND_DISPLAY; XW_SESSION_DBUS=0 opts out.
- Makefile: missing-libseat $(info) promoted to a boxed $(warning)
  with the real-TTY consequence + per-distro install table. Applied
  via script after the tool-based edit expanded recipe TABs to spaces
  and broke the Makefile (recovered via git checkout; tab count
  verified unchanged by the script).
- BUILDING.md: new section "The most common real-TTY failure: libseat
  missing at build time" (log signature, line-by-line why, fix table,
  make clean note), Troubleshooting entry, XW_SEAT_PROVIDER row now
  lists elogind/logind aliases (was stale), XW_SESSION_DBUS runtime
  row. README: session manager responsibilities + seat paragraph.
- Tests: test-session.sh session 9 (20 checks) — start, socket, real
  dbus-send round trip, autostart children inherit the exact address,
  clean logout, no orphan, foreign bus reused/never killed, stale
  address replaced, fresh bus at the standard path. First draft
  failed "reused-bus session exits cleanly": the logout ran after the
  "reusing" log line but BEFORE the ctl socket existed (output was
  discarded -> silent failure). Fixed by gating every logout on
  "session ready". Harness: 128/128, ASan re-ran the process suite
  128/128. build regressions 50/50, in-process 64/64.

Stage Summary:
- Commit 3b1d2fa on main. For the user's machine the fix is: install
  the distro libseat dev package, make clean && make, rerun with
  --verbose — the elogind provider then takes the session and
  logind/elogind grants every input fd (no root, no groups, no chmod).
  The second visible failure cluster (pipewire/xfsettingsd/polkit)
  is fixed in code by the session d-bus.
- The "Edit tool expands Makefile TABs" failure mode is recorded here
  for future rounds: edit Makefiles via scripted string replacement.

---
Task ID: e354474 round (2026-09-02)
Agent: main
Task: user hit "error: target not found: libseat" (pacman) trying to
install the fix the previous round prescribed

Work Log:
- Cause: our own instructions named a nonexistent Arch package. On
  Arch/Artix there is no 'libseat' split: the seatd package ships the
  daemon AND the library (libseat.so, libseat.pc, seat.h). The user
  followed BUILDING.md's fix table and the Makefile warning box
  verbatim; pacman was right to refuse.
- Makefile warning box: 'Arch/Artix: pacman -S libseat' -> 'pacman -S
  seatd' via scripted replacement; tab-recipe count verified
  unchanged (99 = 99); make -j parses the whole tree ("Nothing to be
  done", exit 0).
- BUILDING.md: Arch/Artix row now 'sudo pacman -S seatd' + note (no
  libseat package; seatd ships libseat; verify via pkg-config
  --modversion libseat); Alpine row aligned with the quickstart
  ('apk add seatd seatd-dev' - the two sections disagreed); Arch
  quickstart comment fixed (libseat does not come "from the AUR").
- No code paths touched; no test rerun needed beyond the parse check.

Stage Summary:
- Commit e354474 (fix) + this worklog commit on main, now 4 ahead of
  origin/main: push still blocked (credential store wiped).
- User-facing sequence on Arch: sudo pacman -S seatd; pkg-config
  --modversion libseat (expect 0.9.x); make clean && make (feature
  guard requires the clean when XW_LIBSEAT flips on); cat
  build/.features must show libseat=y; rerun xw-session
  --backend=drm --verbose from the TTY login (no sudo). Expected:
  "seat: elogind/logind detected ... trying libseat's logind backend
  first" then "/dev/input/eventN opened through seat provider ... (fd
  N)" with keyboard + pointer counts > 0 - the frozen-mouse root
  cause (build without libseat) is then fully closed.

---
Task ID: push round (2026-09-02)
Agent: main
Task: user supplied a fine-grained GitHub PAT in chat; push the
commits stranded locally (push blocked since the credential store
was wiped)

Work Log:
- Token handled with zero persistence: command-scoped XW_PUSH_TOKEN
  env var + GIT_ASKPASS scripts/git-askpass-env.sh (helper contains
  no secret, echoes the env var); git run with `-c credential.helper=`
  because a repo/global `credential.helper store` IS configured and
  would otherwise auto-save the token to ~/.git-credentials. Token
  never written to any file, log or worklog entry.
- Pushed 01f4b4b..bc997d2 (3b1d2fa session d-bus, d376786 + bc997d2
  worklogs, e354474 Arch package-name fix) plus this round's
  checkpoint commit (askpass helper + this entry).
- Recommended the user rotate/revoke the PAT after confirming the
  push (it transited a plaintext chat).

Stage Summary:
- origin/main restored to full sync; nothing stranded locally.
- Real-machine sequence to close the frozen-mouse issue for good:
  git pull; sudo pacman -S seatd; pkg-config --modversion libseat
  (expect 0.9.x); make clean && make; cat build/.features ->
  libseat=y; relaunch xw-session --backend=drm --verbose from the
  TTY login (no sudo) and expect "opened through seat provider" per
  input device.

---
Task ID: d68d18f round (2026-09-02)
Agent: main
Task: user reported the libseat fix "did not work"; asked for a way
to log everything into one file to hand back instead of running
commands one by one

Work Log:
- Interpreted the user's command notes: `cat build/.features` failed
  because they typed a space (build/ .features); `xw-session` without
  ./build/bin/ prefix is expected (make does not install to PATH;
  make install targets ~/.local).
- "Did not work" is ambiguous (rebuild maybe never linked libseat vs
  new runtime failure) — resolved by making the machine tell us.
- Wrote scripts/xw-tty-capture.sh: POSIX sh, zero-persistence capture
  of every diagnostic in one file (repo root, *.log is gitignored):
  [0] meta/git, [1] pkg-config versions + pacman -Qi seatd + ldd of
  both binaries with an explicit "libseat IS/IS NOT linked" CHECK
  line, [2] build/.features with libseat=y CHECK, [3] env +
  loginctl show-session + /dev/dri + /dev/input + seatd socket +
  session bus, [4] live window: bare compositor (-B drm -v) under
  `timeout`, exit code echoed into the log (124 = normal timeout),
  [5] live window: full xw-session --backend=drm --verbose, [6]
  self-digest grep. Every command + output + exit code recorded.
- Safety: skips live windows as root / without /dev/dri / without
  the binaries; timeout SIGTERM path verified against the
  compositor's clean SIGTERM+VT-restore handlers; trap prints the
  log path on Ctrl-C.
- Self-tested in the container (non-TTY: windows skipped, all check
  sections produced a well-formed log; libseat CHECK line verified
  against the sysroot build).

Stage Summary:
- Commit d68d18f on main, pushed with the user's PAT (askpass env
  helper, no persistence).
- User sequence: git pull; ./scripts/xw-tty-capture.sh; move the
  mouse in both 12s windows; send back the printed .log file. The
  CHECK lines + live windows decide the next fix deterministically.

---
Task ID: d7562f6 round (2026-09-02)
Agent: main
Task: analyze the uploaded xw-tty-capture log ("did not work" round)
and produce the auto-fix

Work Log:
- Read upload/xw-tty-capture-20260902-162919.log — verdict
  unambiguous: seatd 0.9.3-1 installed but the package carries NO
  libseat files (pkg-config: not found; build/.features: libseat=n;
  ldd: no libseat linked). pacman -Qi shows degenerate metadata
  (description "None", 0.00 B, epoch install date) — not a healthy
  repo package, likely hand-built with the files missing. Same
  root cause as before, one layer deeper: the elogind path stays
  compiled out, direct VT gets EACCES on all 18 /dev/input nodes.
- The capture log also ends mid-window-1 (no WINDOW1_EXIT_CODE, no
  window 2, no digest): the user Ctrl-C-ed; harmless — window 1 had
  already proven everything. Environment otherwise healthy: elogind
  session 1 active on seat0/vt1, 18 input nodes, /dev/dri/card1
  carries the uaccess ACL.
- Wrote scripts/xw-seat-fix.sh (POSIX sh, same one-file-log style):
  route 0 already-linked no-op; route 1 pkg-config sees dev files ->
  make clean && make tee'd into the log + verify .features/ldd;
  route 2 sudo pacman -S seatd reinstall (interactive, terminal
  visible; before/after Qi + Ql + file checks logged) then rebuild
  if the files appeared; route 3 fallback seatd-daemon route
  (gpasswd to 'seat' + service enable per detected init:
  runit/openrc/dinit/systemd, seatd-<init> split-package hint,
  manual nohup start for today) plus the documented input-group
  one-liner; relogin banner; DONE banner with the log path.
- Privileged steps are always printed before running and asked y/N
  first (auto-skipped when stdin is not a terminal). The script
  never chmods devices and never runs the compositor as root.
- Self-tested both branches: A) in-repo (already linked -> route 0
  clean exit 0); B) scratch tree (no pacman/seatd/build -> full
  route instructions, exit 0). Cleaned all artifacts.

Stage Summary:
- Commit d7562f6 pushed to origin/main via the PAT askpass helper.
- User sequence: git pull; ./scripts/xw-seat-fix.sh (answer y at the
  sudo/reinstall prompts). SUCCESS banner -> no relogin needed
  (elogind session already active); route 3 -> one logout/login so
  the group applies. Then ./scripts/xw-tty-capture.sh and send BOTH
  log files back.
- If the repo reinstall still yields no libseat.pc, route 3 makes
  the machine work via the built-in seatd client anyway — that path
  needs no library and is fully covered by the tests.

---
Task ID: repair-round (2026-09-02)
Agent: main
Task: user sent the xw-seat-fix and xw-tty-capture logs; mouse still
frozen; "i recompiled it to make sure ... all i did see its do its
just reinstall seatd"

Work Log:
- Read both logs — the machine's seatd 0.9.3-1 install is corrupted
  at BOTH levels: files (/usr/lib/libseat.so.1, libseat.pc and the
  /usr/bin/seatd BINARY are zero bytes — `seatd -h` silently
  succeeding as an "empty shell script" proves it) and pacman
  database (pacman -Ql lists nothing, -Qi shows all None/0.00 B).
  That also explains why route 3b failed: the runit service was
  enabled fine, but there was no working binary to start (no
  /run/seatd.sock). The repos themselves are healthy (pacman -Si
  shows full entries in Artix world + Arch extra).
- The previous round's `sudo pacman -S seatd` exited 1 and its
  output was NOT captured — xw-seat-fix.sh's sudo_run() failed to
  redirect into the log (bug, recorded; that script is NOT being
  edited this round because the user has local modifications in it
  that would block their git pull).
- User also never relogged (session still 1, seat group not yet
  active in id -nG) — irrelevant for the libseat path, only for the
  group fallbacks.
- Wrote scripts/xw-seat-repair.sh (NEW file, one-file-log style):
  [1] df -h/-i + du of the pacman cache, HARD STOP at >= 99% full
  (a full / is the classic producer of exactly this 0-byte-files +
  empty-DB corruption, and pacman refuses installs on it); [2]
  before-evidence incl. wc -c of the binary; [3] repair: pacman
  -Rdd --noconfirm (empty file list => nothing deleted from disk),
  fallback rm -rf of the corrupt DB dir, manual rm of the 0-byte
  leftovers (+ /etc/runit/sv/seatd), fresh `pacman -S --noconfirm`
  with a -Syy --overwrite retry — ALL sudo output captured into the
  log this time (sudo password still prompts on the tty; pacman
  needs no prompt with --noconfirm); [4] make clean && make tee'd +
  linked CHECK; [5] two 12s live windows (move-the-mouse prompts)
  only when linked + seatd socket wait (informational); [6]
  input-group fallback + relogin banner if all else failed.
- Self-tested: TEST A plain (route 0 no-op, exit 0), TEST B with
  PKG_CONFIG_PATH into the sysroot after make clean (full rebuild
  ran, MAKE_EXIT_CODE=0, ldd shows libseat.so.1, SUCCESS CHECK,
  windows skipped without /dev/dri, digest, exit 0). Fixed two
  stray-paren typos found by sh -n on the way.

Stage Summary:
- Commit (new script only, deliberately not touching the two scripts
  the user has locally modified, so their git pull stays clean).
- User sequence: git pull && ./scripts/xw-seat-repair.sh — answer y
  at the two prompts (purge+reinstall, then windows), move the mouse
  in the live windows, send the xw-seat-repair-*.log back. If the
  reinstall still fails, its pacman error text is IN the log now,
  and the input-group fallback fires (one gpasswd + one relogin).

---
Task ID: question-round (2026-09-02)
Agent: main
Task: user asked how other Wayland compositors actually acquire the
keyboard/mouse ("have you looked at their code to see why it works
for them?"); seatd repair still pending on their machine.

Work Log:
- Verified c6ff91a (xw-seat-repair.sh) IS on origin/main (0 commits
  ahead) — the user's `git pull` delivers it. sh -n re-checked.
- Pulled real upstream sources for exact quotes (gitlab raw is
  bot-gated with an HTTP-200 challenge page; used the swaywm GitHub
  mirror for wlroots and the kennylevinsen mirror for seatd; added
  an anti-bot-HTML rejection guard to the fetch helper):
  wlroots backend/libinput/backend.c (libinput_interface wiring:
  open_restricted -> wlr_session_open_file), backend/session/
  session.c (libseat_open_seat + libseat_open_device in
  wlr_session_open_file), seatd libseat/libseat.c (backend order
  seatd, logind, builtin; $LIBSEAT_BACKEND pinning),
  libseat/backend/logind.c (TakeControl; TakeDevice "uu" call with
  the fd arriving in the "hb" reply), seatd/client.c (SO_PEERCRED
  identity; handle_open_device -> dup -> connection_put_fd),
  common/connection.c (SCM_RIGHTS cmsg), seatd/seat.c
  (CLIENT_ACTIVE gate, path sanitizing, open() as root).
  Saved under tool-results/refs/ (gitignored).
- Confirmed this repo's own design mirrors wlroots line-for-line:
  xw-input-libinput.c open_restricted/close_restricted +
  libinput_udev_create_context; xw-session-seat.c
  libseat_open_seat/libseat_open_device — plus extras wlroots does
  not have (built-in seatd wire client, direct VT provider).
- Mapped every dead provider route to the corrupted seatd package:
  route 1 (elogind via libseat) dead = libseat not linked (0-byte
  .pc -> build/.features libseat=n); route 2 (built-in seatd
  client) dead = the seatd BINARY itself is 0 bytes, so the enabled
  runit service starts nothing and /run/seatd.sock never appears;
  route 3 (direct) alive for DRM (elogind udev ACL / video perms)
  but EACCES on /dev/input BY DESIGN (input never gets ACLs —
  broker-only). That is the exact "picture renders, mouse frozen"
  state in the user's logs.
- No source changes this round (analysis + answer only). This
  worklog entry committed locally, NOT pushed (no token this
  session — origin already has everything the user needs).

Stage Summary:
- Answer delivered with side-by-side upstream quotes (sway ->
  wlroots -> libseat -> elogind D-Bus / seatd SCM_RIGHTS).
- Continuation point UNCHANGED: user runs `git pull &&
  ./scripts/xw-seat-repair.sh`, answers y at the two prompts, moves
  the mouse in the live windows, sends xw-seat-repair-*.log back.
  If pacman still refuses, its error text is now captured in the
  log; the input-group fallback + relogin is the no-package escape.

---
Task ID: conflict-round (2026-09-02)
Agent: main
Task: user pasted their own `sudo pacman -Syu seatd` output instead
of running the repair script ("after i updated i didnt run the
commands you told me").

Work Log:
- Read upload/Pasted Content_1788363220828.txt (173 lines, Romanian
  pacman output): the 120-package -Syu transaction aborted at the
  file-conflict check — "eroare: eșec la efectuarea tranzacției
  (fișiere în conflict)" — listing ALL TEN seatd files as already
  existing on disk: /usr/bin/seatd, /usr/bin/seatd-launch,
  /usr/include/libseat.h, /usr/lib/libseat.so, libseat.so.1,
  pkgconfig/libseat.pc, /usr/lib/sysusers.d/seatd.conf,
  /usr/share/licenses/seatd/LICENSE, both man pages. Pacman's
  conflict check runs pre-commit for the WHOLE transaction, so
  nothing was installed — seatd stayed broken AND the 119-package
  system update did not happen.
- This CONFIRMS (no longer infers) the invisible exit-1 root cause
  from xw-seat-fix route 2: the corrupted DB entry owns zero files,
  so the zero-byte on-disk leftovers count as UNOWNED, and pacman
  refuses to overwrite unowned files. It was never a download,
  keyring or mirror problem. For the same reason a plain reinstall
  can NEVER fix this — the orphans must be deleted first.
- New facts from the transcript: the header is /usr/include/
  libseat.h (the repair + capture scripts said seat.h — wrong);
  the repo package ships NO /etc/runit/sv/seatd, so that directory
  on the user's disk is a leftover too (kept only if intact).
- Patched scripts/xw-seat-repair.sh (commit a977985, local — no
  token this session to push): the rm list is now the complete
  10-file set, the runit service dir is removed only when its run
  file is zero-byte, [2] evidence gains wc -c of the run file, and
  the header comment documents the confirmed root cause. The user's
  on-disk copy is c6ff91a (old 6-file rm list) — its first
  pacman -S will still hit the leftover conflict, but the
  --overwrite 'usr/*' retry covers it (pacman's fnmatch does not
  use FNM_PATHNAME, so the glob crosses '/').
- Reply sent: manual 3-command repair (pacman -Rdd seatd; rm the
  ten orphans; pacman -S seatd) THEN ./scripts/xw-seat-repair.sh,
  which will see pkg-config OK, skip the repair routes, rebuild and
  open the two live windows; afterwards redo sudo pacman -Syu (the
  aborted system update). seatd -h printing usage is a quick
  sanity signal — the old 0-byte binary ran as an empty script and
  printed nothing.

Stage Summary:
- Root cause proven by the user's own pacman output; repair recipe
  is now deterministic (purge DB entry + delete all ten orphans +
  fresh install).
- Continuation point: user runs the 3 commands + the repair script,
  moves the mouse in the two live windows, and sends back the
  xw-seat-repair-*.log (or the pacman output if the manual install
  still refuses).
