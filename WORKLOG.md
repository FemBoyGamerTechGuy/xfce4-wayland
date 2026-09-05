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

---
Task ID: success-round (2026-09-02)
Agent: main
Task: user sent the xw-tty-capture-20260902-184653 log after
running the manual repair commands — verify the fix end to end.

Work Log:
- Read upload/Pasted Content_1788364132864.txt (490 lines).
  The repair WORKED and the whole stack is now healthy:
  [1] pkg-config libseat 0.9.3 exit 0; pacman -Qi seatd fully
  populated (Desc, 110.79 KiB, install date TODAY 18:39:42,
  signature-validated — the manual 3-command recipe from the
  conflict-round reply); ldd shows libseat.so.1 resolved.
  [2] build/.features = x11=y libinput=y drm=y libseat=y; binaries
  rebuilt 18:43 (fresh). CHECK lines all positive.
  [4] bare compositor window: Seat opened with backend 'logind',
  session active, DRM card1 via libseat, HDMI-A-1 1920x1080@144,
  ALL 18 /dev/input nodes opened THROUGH the seat provider
  (fds 22-39), 8 devices acquired (7 keyboard, 3 pointer incl.
  "Gaming Mouse"), clean SIGTERM exit. No EACCES anywhere.
  [5] full session window: session dbus up, compositor ready,
  xw-panel mapped as a 1920x28 layer-shell bar, pipewire/
  wireplumber/xfce4-power-manager/polkit/xfsettingsd started,
  session ready, ordered teardown at timeout. Compositor itself
  logged ZERO errors.
- Remaining log noise is all unrelated/pre-existing: pipewire
  RTKit ServiceUnknown (no rtkit daemon on Artix; graceful
  fallback), xfce4-power-manager wanting wlr-output-power-
  management (roadmap), xfsettingsd wanting wlr-output-management
  (roadmap), libcamera SPA plugin notice. None affect input.
- The seatd daemon is not running (/run/seatd.sock absent) and
  does not need to be — the logind/elogind route through libseat
  is the active provider; the built-in seatd wire client remains
  a dormant backup route.
- User's tree: HEAD c6ff91a with local modifications to both xw
  scripts (their edits, untouched). The a977985/4eaae95 commits
  (repair-script hardening + worklogs) remain local-only in this
  workspace — nothing on the user's machine needs them; origin
  already has everything they use.

Stage Summary:
- THE FROZEN-MOUSE SAGA IS CLOSED: corrupted seatd package ->
  zero-byte libseat -> no broker -> EACCES on /dev/input was the
  entire cause; deterministic repair applied; input acquisition
  verified working through libseat's logind backend.
- Next steps if the user continues: roadmap items surfaced by
  the session log (wlr-output-power-management for DPMS,
  wlr-output-management for display settings), plus general
  hardening/testing. The 119-package system update they aborted
  with the failed -Syu should still be run (sudo pacman -Syu).

---
Task ID: bisect-round (2026-09-02)
Agent: main
Task: user reports the cursor STILL does not move despite the
seat fix being verified healthy — new bug, downstream of device
acquisition.

Work Log:
- Read the success-round capture log again with fresh eyes: with
  -v == XW_LOG_DEBUG (confirmed in src/compositor/xw-compositor.c
  line 183 and xw-util.c level logic — debug lines DO print at
  -v), BOTH 12s live windows logged ZERO input events of any
  kind: not one "libinput: POINTER_MOTION/KEY/BUTTON" line, no
  "xw-input: pointer motion", no "compositor: cursor position
  updated". Devices are open, 8 acquired, but no events flow.
- Traced the ENTIRE static chain and exonerated every layer:
  xw-input-libinput.c (wl_event_loop_add_fd on libinput fd ->
  on_libinput_fd -> libinput_dispatch -> drain_libinput -> all
  event cases handled incl. POINTER_MOTION rel+abs);
  xw-compositor.c inject_* -> xw-seat.c pointer_motion (cursor
  update + client delivery + damage_cursor at old+new); damage
  -> xw_output_damage_rect -> xw_schedule_repaint ->
  wl_event_loop_add_idle -> on_repaint_idle repaints damaged
  outputs; xw-render.c draw_cursor (software cursor, 12x17
  arrow); xw-backend-drm.c present path (flip + parked_frame +
  drmHandleEvent via db_on_drm_readable registered in the same
  loop); xw-session-seat.c DOES dispatch libseat's own fd
  (ls_on_readable via wl_event_loop_add_fd + initial
  libseat_dispatch(0)); libseat's logind backend dups the D-Bus
  fd (fcntl F_DUPFD_CLOEXEC, fetched upstream source verified).
  The event loop demonstrably dispatches (xw-panel round-trip
  mid-window in the last capture proves it).
- Conclusion: static analysis cannot find it — runtime bisect
  needed. The decisive split: kernel/libinput half (sudo
  libinput debug-events — root, opens /dev/input directly, NO
  compositor, NO libseat) vs compositor half (bare -v window
  through the seat broker), with event COUNTS per half and an
  automatic verdict matrix:
  kernel>0 & comp=0 -> seat-brokered fds inert (code fix: open
    input nodes on a fallback path / investigate paused devices)
  comp>0 -> input fine, RENDER/presentation bug (present path)
  kernel=0 -> hardware/driver (below the compositor entirely)
  keys>0 & motion=0 -> mouse-device-specific
- Wrote scripts/xw-input-bisect.sh (commit 838f7c0, local):
  one-file-log style, [1] kernel half (sudo y/N, 8s
  debug-events, auto list-devices + dmesg dump when zero),
  [2] bare compositor 12s window with loud WIGGLE banners,
  [3] xw-session window, [4] verdict with KTESTED/CTESTED flags
  so skipped halves cannot fake a verdict. Counting slices the
  log between section markers; patterns 'libinput: KEY ' /
  'libinput: BUTTON' cannot match device-add lines (unit-tested
  with a synthetic window: motion=4 keys=1 buttons=1, and
  "Gaming Mouse Keyboard" NOT counted as a key). Container dry
  run passes (guards fire, honest "incomplete data" verdict,
  exit 0).
- Fixed two stray-paren typos and one say/sd typo found by
  sh -n + dry run on the way (Edit tool round-trips verified).
- Cannot push 838f7c0 (no token this session) — the user gets
  the EQUIVALENT MANUAL COMMANDS in the chat instead (3 runs +
  4 grep counts), which is what the reply delivers.

Stage Summary:
- New working hypothesis set, in priority order: (a) the
  libseat/logind-brokered device fds are inert (paused state or
  broker quirk) — no events reach the compositor; (b) the user
  did not move the mouse during the capture windows (bisect
  banners + counts make this testable); (c) hardware/driver
  produces no evdev events at all; (d) events arrive but
  render/flip never updates the screen (would contradict the
  zero-debug-lines evidence unless (b)).
- Continuation point: user runs the 3 manual bisect commands,
  moves the mouse in every window, and pastes both logs (or the
  4 counts) back; the verdict matrix picks the next fix.

---
Task ID: nvidia-bisect-round (2026-09-03)
Agent: main
Task: user's manual bisect attempt: kernel half failed (libinput CLI
missing on their machine), compositor-half greps all zero; new fact
from the user: "probably forgot to say that I use a NVIDIA GPU".

Work Log:
- Read the user's paste: both halves INCONCLUSIVE, not negative.
  kernel-side: "timeout: libinput: no such file or directory" — the
  libinput CLI binary does not exist on their box while the library
  IS linked and working (compositor runs) — same corruption family
  as the old seatd package; `pacman -Qk libinput` / `sudo pacman -S
  libinput` restores it. comp-side: their run had no -v, and every
  per-event line is XW_LOG_DEBUG, filtered at the default INFO level
  — "compositor motion: 0" proves nothing either way.
- Re-traced the static chain with the NVIDIA fact in hand: the input
  wiring (fd -> dispatch -> handlers) is correct; the cursor is a
  SOFTWARE cursor (xw-render.c draw_cursor from seat->cursor_x — no
  hardware cursor plane, so the classic WLR_NO_HARDWARE_CURSORS
  problem does NOT apply here); damage -> idle repaint -> present
  all wired.
- Found the display-side NVIDIA landmine: db_present() parks every
  frame while waiting_flip is set, and waiting_flip is cleared ONLY
  by the vblank event in page_flip_complete. The NVIDIA proprietary
  DRM driver accepts drmModePageFlip() on the legacy path but never
  delivers the DRM_EVENT_PAGE_FLIP vblank event — waiting_flip then
  sticks forever, every later frame parks, and the display freezes
  on the last flipped frame for the whole session while frame
  callbacks keep flowing. This is a second, INDEPENDENT way to get
  "picture renders, cursor frozen", and it must be fixed regardless
  of what the input bisect says.
- xw-backend-drm.c changes: (1) flip watchdog — recurring 100ms
  timer (no extra wakeups beyond the loop's own 100ms dispatch
  timeout); a flip the driver ACCEPTED that is older than
  XW_FLIP_TIMEOUT_MS (300ms) trips it: a WARN naming the NVIDIA
  legacy path, the no_flip fallback engages, drmModeSetCrtc pins
  scanout to bos[current], and the newest frame is copied in — the
  display un-freezes on the spot; (2) kernel driver identification
  via drmGetVersion logged at startup ("nvidia" gets an explicit
  flip-event warning); (3) presentation counters + a 2s INFO stats
  line for the first 30s (presents/flips/vblank-events/parked/
  no-flip/in-flight/watchdog); (4) db_destroy removes the two new
  timer sources.
- xw-input-libinput.c changes: the input half of the instrumentation
  — per-type event counters (motion/abs/key/button/axis), one-time
  INFO "first pointer/key event" markers, and a 2s INFO stats line
  for the first 30s including live cursor coordinates. After 30s the
  stats timer removes itself (no long-term log spam). Everything is
  visible at the DEFAULT log level — that is the whole point.
- scripts/xw-input-bisect.sh: the kernel half no longer dies when
  the libinput CLI is missing — it falls back to picking the
  pointer's event node from /proc/bus/input/devices and counting
  raw 24-byte evdev records via `sudo timeout 8 dd | wc -c` (no
  package needed); the dmesg dump still fires on zero. sh -n clean.
- Rebuilt the dev sysroot in the fresh workspace, full make with
  -Werror clean, 64/64 tests pass, new log strings verified present
  in the binary, headless smoke run fine.
- No push token this session (env scan empty) — 6 commits sit
  locally, user HEAD is c6ff91a. Deliverables written to download/:
  a functional patch (git diff origin/main..main limited to src/ and
  the bisect script) plus full copies of the 3 changed/new files as
  a copy-over fallback for the case where git apply hits their local
  script edits.

Stage Summary:
- Both independent frozen-cursor causes are now covered: (a) the
  input half is measurable at default log level (stats + first-event
  markers + raw kernel fallback in the bisect); (b) the NVIDIA
  legacy page-flip event hole is FIXED by the watchdog — if that was
  the cause, the cursor starts moving the moment the new build runs.
- User sequence: apply the patch (or copy the 3 files), make, sudo
  pacman -S libinput (restore the CLI), then ./scripts/xw-input-
  bisect.sh with the mouse moving in every window. The [4] verdict +
  the new stats lines decide the next step: kernel=0 -> hardware/
  driver; kernel>0 & comp motion=0 -> seat-brokered fds inert (the
  next code fix is a fallback open path); comp motion>0 & screen
  still frozen -> flip path (the watchdog WARN + drm stats will show
  it, and the watchdog already self-heals it).

---
Task ID: push-round (2026-09-03)
Agent: main
Task: user supplied a GitHub PAT (pasted in chat) — push the 7
locally stranded commits (c6ff91a..e469b98) to origin/main.

Work Log:
- Token handled per the one-shot askpass design
  (scripts/git-askpass-env.sh): lives only in XW_PUSH_TOKEN env
  of the single git invocation, `credential.helper=` disabled
  for that call, push output scrubbed — the secret was NOT
  written to .git/config, ~/.git-credentials, or any repo file.
- Push succeeded: c6ff91a..e469b98 main -> main — the hardened
  seat repair, the bisect script, the worklog rounds, and the
  nvidia-bisect instrumentation round. Branch in sync,
  `git status` clean.
- Post-push QA: `make` is a no-op (build current for e469b98;
  -Werror clean and 64/64 tests passed at this commit in the
  previous session).
- The download/ patch + file-copy fallback from last session is
  now SUPERSEDED: `git pull` is the primary path.
  `git pull --rebase --autostash` absorbs any local script edits
  on the user's clone.
- Advised the user to rotate/revoke the PAT once the saga is
  done (a token pasted into a chat should be treated as
  exposed).

Stage Summary:
- origin/main now carries BOTH candidate fixes for the frozen
  cursor: (a) the input half — default-log-level event counters,
  first-event markers, 2s stats with live cursor coords, and a
  raw-evdev kernel fallback in the bisect script; (b) the render
  half — the NVIDIA legacy page-flip watchdog. If (b) was the
  cause, the cursor simply starts moving on the first run of
  the new build, no diagnosis needed.
- User sequence on the Artix box: git pull --rebase --autostash
  origin main && make -j$(nproc); optional `sudo pacman -S
  libinput` to restore the CLI (bisect works without it); then
  ./scripts/xw-input-bisect.sh from a TTY, mouse moving in every
  window — the [4] verdict picks the next move.

---
Task ID: panel-interact-round (2026-09-03)
Agent: main
Task: the DRM session renders and the cursor moves (NVIDIA flip fix
held) but the panel is completely non-interactive — trace the whole
click chain, fix the first broken boundaries, reorganize the desktop
components as independent subprojects, test, document.

Work Log:
- Fresh workspace: bootstrap-sysroot + full build + 64/64 green at
  b69fd42 before touching anything.
- Traced the complete chain statically, layer by layer:
  libinput drain -> inject_* -> seat motion/button -> surface_at ->
  set_ptr_focus (enter/leave/serials/frames) -> panel client binding
  (seat bound with listener attached at bind time; layer + callbacks
  registered in the client owner map) -> btn_at widget dispatch ->
  ctl actions. Every layer is correct; the existing panel-clicks test
  (real forked binary, real socket) already proves the chain in the
  harness. The breakage decomposed into eight real holes instead:
- (1) UAF-class: xw_surface_resource_destroyed never cleared
  seat->ptr_focus/drag refs — any surface dying under the cursor left
  a dangling focus that poisoned later focus decisions ("panel
  visible, cursor moves, nothing reacts" decay). xw_seat_forget_surface
  now runs from the surface destroy path.
- (2) seat_get_pointer never replayed enter to a late wl_pointer
  (keyboard did since forever — asymmetric). Fixed + tested with a
  raw late pointer.
- (3) focus never re-evaluated when surfaces map/unmap under a
  stationary cursor (cursor starts at 0,0 = inside a top panel).
  xw_seat_repointer on layer/window map+unmap.
- (4) layer-shell get_layer_surface silently DROPPED the request with
  no outputs (unbound object id = protocol kill for the client).
  Surfaces are now held unconfigured and adopted by
  xw_layer_output_added; xw_layer_output_removed re-anchors or closes.
- (5) layer stacking was tail-inserted with inverted topmost
  semantics (oldest layer surface stayed above newer ones forever).
  Head-insert, render/hit-test consistent, comments now true.
- (6) launcher dead on Arch: 'x-terminal-emulator' Debian-ism -> 127.
  Terminal fallback list in the panel (client) AND the compositor
  (action) — deliberately duplicated, documented.
- (7) exit button dead in a build tree: session + compositor spawned
  'xw-exit' by PATH; build/bin is not on PATH. Sibling-of-binary
  resolution in exit_dialog_command + xw_actions_init.
- (8) boundary instrumentation: focus/enter/leave/button lines with
  surface identity (role + namespace/title), first-enter INFO marker;
  panel XW_PANEL_TRACE prints the interaction chain end to end.
- subprojects/panel: git mv + README + dependency contract
  (subprojects/README.md); Makefile targets compositor/panel/session/
  clients, all profile-stamped. make panel compiles ZERO libxw
  objects (verified from a clean tree); make compositor builds alone.
  The interactive Edit tool normalized Makefile recipe TABs to spaces
  and broke make — recovered via scripts/apply-makefile-panel.py
  (persisted; the Makefile is tab-sensitive, never re-indent it).
- 6 new tests (focus lifecycle + hover + UAF survival, late-pointer
  enter replay, launcher ctl line, clock v0 no-op, layer-before-
  outputs, compositor-without-panel). Debugging them surfaced:
  xwt_pump_server looped FOREVER on a dead client connection (a
  protocol error used to wedge the whole suite — fixed with
  client_dead); the connect roundtrip can return with binds still
  queued server-side (the layer test now pumps before destroying the
  output — WAYLAND_DEBUG traced the exact wire race); the hover check
  must look clear of the software cursor (it sits exactly on the
  pointer); UBSan caught a NULL-focus deref in my own test 51 (raw
  xwc_win_create never commits -> never maps; fixed with a solid-
  configure helper).
- Final: 70/70 release, 70/70 ASan+UBSan+LSan (zero reports), session
  process tests 128/128, build regressions 50/50 (1 skipped), DRM
  mode/planning tests all green, NVIDIA watchdog untouched.

Stage Summary:
- The pointer protocol chain was already correct end-to-end (the
  harness proves it with the real binary); the panel's dead buttons
  decompose into the ptr_focus UAF + protocol-correctness holes (all
  fixed, ASan-proven) and two ACTION-side failures that make the
  launcher/exit buttons dead on the user's real box regardless of
  input (terminal fallback + sibling resolution, both fixed).
- The clock is intentionally display-only in v0 (documented in
  ROADMAP + subprojects/panel/README + its test asserts the no-op).
- Remaining hardware-only verification: one DRM session run on the
  user's NVIDIA box — the instrumentation now answers every boundary
  from a default-level log (first pointer event, first enter
  delivered, focus transitions with surface identity, panel button
  hits and activated actions).

---

## 2026-09-03 — panel-menu round: the VT trap, Ctrl+C, and the Start menu

Task: fix "Start does nothing / repeated Start crashes the panel",
"Ctrl+Alt+F1..F12 dead once the compositor runs", "Ctrl+C no longer
stops the session", "the session traps the user on the TTY" — and
make Start an applications menu.

- VT trap root cause: `xw_seat_session_ack_disable()` was implemented,
  documented and unit tested but NEVER CALLED by production code. On
  a switch away the providers dropped DRM master and returned without
  acknowledging — the kernel sat parked in VT_RELDISP wait (direct),
  seatd held its VT handoff, libseat never got
  `libseat_disable_seat()`. Fix: the DRM backend's disable callback
  acks synchronously after releasing resources, and a consumer-less
  disable auto-acks. Compositor-side Ctrl+Alt+F1..F12 handling added
  (weston pattern, before shortcuts and the lock gate).
- Ctrl+C: two real holes found and fixed. (1) xw-session had default
  SIGHUP disposition — a terminal gone killed it instantly, leaving
  the compositor orphaned on a graphics console; now SIGHUP =
  clean shutdown, plus an emergency console restore
  (KD_TEXT/VT_AUTO, logged) after compositor signal deaths. (2) The
  compositor blocks SIGINT/TERM/HUP/CHLD for signalfd — and the mask
  survives fork+exec, so EVERY process it spawned (terminals from the
  shortcuts, ctl-launched apps) inherited it and ignored Ctrl+C;
  xw_spawn_command, the panel and the async ctl helper now reset the
  mask before exec. Found by reading /proc/<pid>/status SigBlk of a
  leaked test panel.
- Start menu: the old behavior was a blocking ctl round trip inside
  the Wayland button handler (500ms session poll cycle => frozen
  panel per click; queued serial freezes under rapid clicking) plus a
  terminal fallback that was dead on the user's box. Now: xdg_popup
  parented to the bar layer, XDG .desktop discovery, ctl-run
  launching (fire-and-forget fork), Escape/outside-press dismissal,
  idempotent Start toggle (same-click suppression window), Enter
  launches the hovered item, terminal fallback when no .desktop apps
  exist. libxwcl gained xwc_popup_* (no blocking sync — a sync inside
  an event handler wedges the client on signals; libwayland retries
  EINTR internally).
- Compositor popup-grab semantics fixed: the grab now moves POINTER
  focus to the popup (item clicks used to be delivered to the
  pre-grab surface); outside-press dismissal under a grab does a raw
  input-region hit test (surface_at() short-circuits to the grab
  surface, so outside presses could never be told apart); leave is
  sent on every surface change (same-client bar->menu transitions
  used to send a second enter without a leave).
- Diagnostics: menu lifecycle lines unconditionally on stderr (start
  clicked / opening+count / already open / created+geometry /
  closing+why / item selected+command); VT release/ack/switch chain
  readable from one default-level log.
- Tests: 75/75 in-process (menu matrix, start-repeated, ack auto-ack,
  VT switch keys, mask regression), 144/144 process-level (session 10:
  SIGINT/SIGHUP clean exit, crash loop, no leaked children), 50/50
  build regressions, ASan+UBSan+LSan clean, debug+release profiles
  green. NVIDIA flip watchdog untouched.
- Honest limits: could not reproduce a hard segfault for "repeated
  Start crashes xw-panel" — the reproducible defects behind the
  report were the blocking-ctl freeze class, the popup-grab
  misrouting, and the inherited signal mask (all fixed + regression
  tested); an ASan run of the repeated-click matrix stays clean.
  Hardware-only: real VT switching, termios restore on a real VT.

Next: hardware verification on the user's NVIDIA box (one DRM
session: Ctrl+Alt+F2 away + back, Ctrl+C from the launching TTY,
Start menu open/launch, repeated Start clicking), then continue the
XFCE parity list (ROADMAP: icons, categories, favorites, clock
popup).

---

## 2026-09-03 — panel polish round: launching, icons, text, and a teardown use-after-free

The user report: applications missing icons, menu names with missing
letters, and xw-panel crashing when launching an application; plus the
explicit constraints — Ctrl+C must NOT be a desktop shortcut, Ctrl+Alt+Del
must keep opening the session action dialog.

**Reproduction first.** Built `panel-launch-matrix`: 14 realistic
.desktop fixtures (quoted/escaped Exec, field codes, Terminal=true,
unicode names, every icon flavor, nonexistent executable, malformed
Exec) launched end-to-end through the real menu, with waitpid-based
panel death detection. The client-side launch path turned out robust
(19 launches, zero crashes) — the in-container reproduction could not
reproduce the user's exact crash (no real DRM/apps here), so the fix
strategy was to eliminate the entire fragile launch class instead
(see below). Along the way the reproduction DID flush out a real
allocator bug (below) and several test-infrastructure defects.

**Root causes found and fixed:**

1. **Missing letters**: the bundled font raster covered only ASCII
   0x20..0x7E and the blitter iterated BYTES — every accented letter
   in a .desktop Name rendered as an invisible blank gap, and the
   label fitters cut raw bytes, splitting multibyte characters. New
   `DejaVuSans-latin.ttf` asset (Latin-1 + Latin Extended-A/B +
   punctuation incl. U+2026), codepoint-indexed tables, UTF-8 decoder
   in the renderer, visible tofu box for uncovered codepoints,
   `panel_text_fit` (UTF-8-boundary truncation + real ellipsis) used
   everywhere. (fba5e11)
2. **The launch path**: replaced the fork-inside-dispatch +
   session-ctl-relay + `/bin/sh -c` chain with direct `posix_spawn` of
   the spec-parsed Exec argv (panel-launch.c): SETSIGDEF/SETSIGMASK,
   stdio on /dev/null, executable sanity checked, visible failures
   (red status line + diagnostics), menu lifecycle hardened (data
   copied first, closed only after success). Terminal=true hosting
   now passes the application argv directly for -x/--/positional
   terminal styles. (d8e5b64)
3. **Missing icons**: (a) the libxwcl compile rule never passed
   -DXW_HAVE_PNG — PNG decoding was compiled out entirely even with
   libpng present (the .features stamp said png=y; the decode test
   silently skipped); (b) the active theme was never discovered from
   GTK/XFCE settings; (c) Inherit= chains were not followed; (d)
   Icon= values with an extension never matched. All fixed; misses
   log once and render a generic app-grid glyph. The audit also
   caught that xw-compositor needed the PNG libs on its link line.
   (e799ded, 1102436)
4. **A latent teardown use-after-free**: wl_seat bindings had no
   resource destructor; wl_display_destroy_clients (which runs before
   seat teardown) freed them, and xw_seat_destroy's cleanup walk
   wl_list_remove'd the freed links — a heap-corrupting write that
   surfaced as rare glibc "double free or corruption" aborts once the
   font tables shifted the heap layout. Found via a deterministic
   MALLOC_PERTURB_ reproduction + LD_PRELOAD backtracer (no gdb in
   the container); bisected to the first font commit purely by
   layout. Destructors added; the destroy walk removed. (1432868)
5. **%k field code** implemented (the .desktop file's location).
   (2ca72d4)

**Keyboard contract**: verified no Ctrl+C desktop shortcut exists
anywhere (defaults, examples, code); new `ctrlc-passthrough` test
proves Ctrl+C fires no compositor action and the focused client
receives the key with its modifier state, while Ctrl+Alt+Delete still
dispatches the exit-dialog action and is consumed. (d54039e)

**Validation**: 108/108 in-process (release, twice under
MALLOC_PERTURB_, full ASan/UBSan/LSan round), 144/144 process-level
session tests, 51/51 build regressions, 8/8 link audits.

Next: hardware verification on the user's NVIDIA box per the new
manual panel checklist in TESTING.md (names without missing letters,
ChatGPT-style icon resolution, terminal+graphical launches, failed
launch behavior, Ctrl+C terminal semantics, Ctrl+Alt+Del), then the
backlog (notification daemon, output management, PAM unlock).

---

## 2026-09-03 — the real-client round: two physical-session root causes (native window death, XWayland absence)

The user's physical NVIDIA testing refined the symptom picture into
TWO distinct bugs — native Wayland windows appearing for a fraction of
a second before the compositor "recovered" (restart), and X11
applications not launching at all through XWayland — with the explicit
instruction not to merge them into one generic launcher failure and
not to mask the native bug with recovery. The container could not
reproduce the compositor death with synthetic clients (the previous
round's launch matrix passed 14/14); the missing ingredient was REAL
toolkits. `scripts/fetch-test-apps.sh` downloads foot, zenity/GTK4,
xterm, xeyes, Xwayland (+ recursive deps, no root needed) into the
gitignored `.apps-root/` prefix — and running the real Xwayland
against the compositor reproduced the abort in seconds.

**Bug A root cause (native windows die → session restarts the
compositor): `wl_pointer.set_cursor` had a NULL request handler.**
libwayland-server ABORTS the whole process on any request dispatched
to a NULL listener ("listener function for opcode 0 of wl_pointer is
NULL"), and every real toolkit calls set_cursor the moment its window
takes pointer focus — hence "visible for a fraction of a second".
Reproduced in-container by Xwayland's own first set_cursor before any
fix existed; the session manager's restart was the "recovery". The
fix implements the request (cursor surface + hotspot, hide, roled-
surface rejection), renders client cursor images through the
software-cursor path (default arrow fallback), and forgets dying
cursor surfaces. Two sibling defects from the same audit
(`scripts/audit-interfaces.py`, now standing):
- **`wl_buffer.release` was never sent.** Clients rotating 2+ shm
  buffers (foot, GTK, XWayland) treat release as reuse permission and
  stop committing once their pool is outstanding — apps froze after a
  few frames with zero errors anywhere. A surface now holds its
  committed buffer (destroy listener) and releases it at replacement.
- **`wl_subcompositor` was absent** — foot refuses to start ("no sub
  compositor"); GTK/Qt/Chromium use subsurfaces. Full implementation:
  positions, sync (spec default: state at parent commit)/desync,
  place_above/below, tree rendering around the parent blit, pointer
  hit-test drilling, both destroy orderings. wl_touch also became a
  real resource with a release handler (a client releasing it was a
  fatal invalid-object error before).
No masking: the compositor binary gained fatal-signal diagnostics
(signal, fault address, state dump, backtrace, re-raised so the wait
status stays honestly WIFSIGNALED), and every client/window lifecycle
stage logs pid/surface/serial/geometry (client connected, surface
created, window MAPPED/UNMAPPED with app-id+title+geometry+output+
workspace, focus transitions).

**Bug B root cause (X11 apps don't launch): nothing spoke to Xwayland
24+ — three missing pieces.** (1) Xwayland maps X11 windows only in
ROOTLESS mode (default is rootful since 24.1) and only through the
xwayland_shell_v1 protocol (the xdg fallback was removed); (2) the
session never started Xwayland at all; (3) rootless mode requires an
X window manager that COMPOSITE-redirects the root's subwindows —
without one, X clients connect and render but no wl_surface is ever
created (the exact "no crash, no window" symptom). All three built:
`xwayland_shell_v1` in the compositor (windows flow through the SAME
xw_wm manage/map/focus/stack/workspace/taskbar path as native ones —
no parallel X11 window-management model); `xw-session` starts Xwayland
`-rootless -displayfd` with a per-session MIT-MAGIC-COOKIE-1 authority
file, exports $DISPLAY/$XAUTHORITY (also through
dbus-update-activation-environment), supervises both with bounded
restarts, and prints the requested diagnostic block (executable, pid,
display, socket, ready, alive, wm pid; live via `xw-session-ctl
xwayland`); and `xw-xwm` — the X window manager as a separate session
process speaking the RAW X11 wire protocol over the unix socket (no
Xlib, no xcb, libc only, same constraint as the session manager):
WM_S0 selection, Composite-RedirectSubwindows, SubstructureRedirect,
Map/ConfigureRequest handling, WL_SURFACE_SERIAL correlation, geometry
mirroring into X11 (Xwayland computes X input coords as window-pos +
surface-local, so mirroring keeps clicks landing correctly), and
WM_DELETE_WINDOW delivery. The compositor→helper channel is the new
private `xw_window_control_v1` protocol (geometry + close events keyed
by serial). Slow-start safety is pinned: no timeout exists anywhere —
`xw-demo --delay-ms N` deliberately idles between connect and first
commit, and the session must stay quiet.

Hunting notes for the record: Xwayland's client-side debug
(`WAYLAND_DEBUG=1`) showed the bind choice; its source (fetched as
tarball) gave the three conditions above; the extension name is
case-sensitive ("Composite", not "COMPOSITE" — the query was the last
invisible failure); and the X setup reply's length field is 16-bit at
offset 6 (reading 32 bits at offset 4 asks for 4096 bytes the server
never sends — the handshake hang).

**Validation**: 112/112 in-process (release, twice under
MALLOC_PERTURB_, full ASan/UBSan/LSan round — including the
pre-existing panel-taskbar flake now confirmed stable), 144/144
process-level session tests, 51/51 build regressions, XWayland stack
test PASS, full-session acceptance 21/21 (`scripts/test-realapps.sh`:
two native GTK4 apps + one X11 app + slow starter, all alive, mixed
workspace switching, clean logout, zero surviving processes). foot
(the app that exposed the subcompositor gap) maps and stays.

Next: physical NVIDIA acceptance per the new TESTING.md checklists
(XWayland + real-client + the full sequence), then the backlog (X11
titles via xw-xwm property reads, notification daemon, output
management, PAM unlock).

---

## 2026-09-04 — the X11-first-class round: six real root causes in the XWM/helper path

Continuation of the real-client integration work (the previous entry
landed the three Xwayland 24 requirements; this round made X11 windows
actually behave like windows). The round started from a hard-walled
suite: 112 pre-existing tests passing, 9 new xwm tests failing or
wedging the whole run. Every failure reproduced with REAL Xwayland +
the real helper + a controllable real X11 client (tests/x11client.c)
or the real xterm, and every fix is pinned by a regression test.

**Root causes, in the order found:**

1. **The suite wedge — a signal-mask leak, not a deadlock.** The
   in-process compositor blocks SIGHUP/INT/TERM/CHLD for its
   signalfd sources (`wl_event_loop_add_signal` requires the signal
   blocked); a blocked mask survives fork AND exec, and the test
   scaffolding spawned Xwayland/xw-xwm/x11client/xterm with raw
   fork+exec. Test [121] sat in wait4() forever: kill(pid, SIGTERM)
   silently did nothing because xterm was born SIGTERM-immune, and
   `timeout` could not kill run-tests either. The product's
   xw_spawn_command has restored default signal state since the
   launch-path round — the test spawner now does the same
   (child_signal_defaults in test_xwm.c). Same class: xterm requires
   an ABSOLUTE argv[0] ("No absolute path found for shell" — it
   treats argv[0] as the program it re-execs); a bare "xterm" never
   maps a window.
2. **Every SendEvent died with BadValue(0xa0).** The WM_DELETE_WINDOW
   and WM_TAKE_FOCUS ClientMessages were built with the format byte
   32|0x80. The 0x80 "generated by SendEvent" bit belongs on the event
   CODE byte of delivered events and is set by the SERVER; in the
   request's event blob it turns the format into 160 — invalid — and
   the delivery never happened. Taskbar close never reached supporting
   clients (the close-delete test failed at exactly this), and
   GTK-model (WM_TAKE_FOCUS) apps could never learn they had focus.
   The real-xterm check with xprop confirmed xterm itself is the
   passive model (WM_DELETE only), so the take-focus path needed its
   own regression (xwm-focus-protocol).
3. **Override-redirect windows were never classified.** The OR byte
   in CreateNotify sits at offset 22; the parser read 24 (padding).
   ConfigureNotify's OR byte is at 26 (its layout differs: it carries
   an above-sibling window) — read from 25. Popups, menus and
   tooltips would have entered the managed flow. Additionally,
   classification must not wait for a buffer commit: a blank popup
   (the probe never draws) leaves the window a ghost in wm->windows,
   visible to taskbar/Alt+Tab iteration — new xw_wm_or_reclassify
   moves it to or_windows at set_override_redirect time.
4. **WM_NAME changes were invisible.** The helper never selected
   PropertyChangeMask on managed client windows (X event masks are
   per-client per-window; the WM's selection cannot clobber the
   client's own). Terminals retitle themselves constantly; the
   taskbar title never updated. One ChangeWindowAttributes at
   MapRequest fixes it.
5. **The border ratchet — extent vs interior.** The compositor models
   the wl_surface EXTENT (Xwayland sizes the surface to the X11
   window INCLUDING its border; empirical: a 200x150+border1 window
   commits a 202x152 buffer), while ConfigureWindow speaks INTERIOR.
   The v0 mirror pushed extent numbers as interior dimensions: the X
   window grew by its border, the bigger extent came back as a new
   surface commit, the compositor mirrored again — 2px per round of
   runaway growth for any client that draws (xterm). The mirror now
   converts (interior = extent + border, size = extent - 2*border),
   in every direction: compositor->X, pending-geometry replay, OR
   pushes, and the new X->compositor channel.
6. **Granted resizes never reached the compositor model.** A
   client-initiated resize granted on the X side is X truth, but
   Xwayland only pushes a new surface buffer on damage — a client
   that resizes without drawing left the compositor's geometry stale
   (taskbar/snap wrong). New window-control v2 request set_geometry:
   the helper reports the granted geometry in extent space right
   after the grant (protocol, pending-identity stash for the
   association race, deliberately no echo-back).

**Hardening that fell out:** die() at the poll loop no longer reports
a stale EAGAIN from wl_display_flush as "connection lost" — it prints
the actual poll revents and which side closed; a raw wire dump
(XWM_WIRE=1, every event as hex) settles event-layout questions
against Xwayland's bytes instead of misremembered spec offsets (it
caught #3 immediately); test logs are per-test (xwm-<socket>.log —
the old per-display paths collided and truncated earlier tests'
evidence); XWT_CHECK flushes (failures were hiding in the stdio
buffer); the suite summary counts skips explicitly — a skip is not a
pass, and the probe binary is wired into `all` so a clean rebuild can
no longer silently skip the XWayland tests (the ASan round had
exactly that hole: 10 silent skips inside a "121/121"); the x11client
exits when its window is destroyed externally, the way a real
application does.

An intermittent xw-xwm exit ("X server connection lost") appeared
twice during the round: POLLIN|POLLHUP on the X socket — Xwayland
closing the helper's connection at stack teardown, downstream of a
different failure (the test abort destroying the in-process
compositor kills Xwayland's wl connection). Not reproducible in
6+9 clean runs once the primary bugs were fixed; the honest death
message stays, and the session supervisor restarts the helper with
bounded restarts regardless.

**Real-client verification** (scripts/repro-x11-state.sh): xeyes
maps through the whole path — X side: 144x94 window, WM_STATE Normal,
WM_CLASS "XEyes", listed by wmctrl (_NET_CLIENT_LIST/ACTIVE window);
compositor side: app 'XEyes', title 'xeyes', extent 150x100+565+206
vs X interior +566+207 (border-1 conversion, clicks align by
construction: X root = interior - border + surface-local). xeyes'
WM_HINTS input=false is honored (no forced SetInputFocus). The real
xterm: passive input model (WM_DELETE only, xprop-verified), app_id
'XTerm', font-metric resize (10x17 -> 484x316) granted without
ratcheting.

**Validation**: 122/122 in-process (release, twice under
MALLOC_PERTURB_, full ASan/UBSan/LSan round — this time with zero
skips), 144/144 process-level session tests, 51/0/1 build
regressions, XWayland stack PASS, full-session acceptance 23/23
(zenity GTK4 x2 + xeyes + slow starter, clean logout).

Next: per-output X mode tracking for fullscreen X11 apps, X11
clipboard bridge, notification daemon (backlog order unchanged).

---

## 2026-09-04 — the loss-triage + EWMH fullscreen round

Push retry round (new credentials): a5a01e8 pushed clean; the UUID
auto-snapshot on top (report-generation session scripts) was dropped
per the untracked-session-artifact policy.

Two root causes fixed, one of them found BY the round's own real-client
verification:

1. **The helper never watched the wl socket.** A compositor death
   while Xwayland lived left xw-xwm spinning at 100% CPU on a
   POLLHUP-only fd (no POLLIN → no dispatch → poll never blocks) until
   Xwayland noticed its own dead connection; the failure surfaced
   misattributed as "X server connection lost". This was the
   "intermittent helper exit at stack teardown" from the previous
   round. Now: both fds checked, every wl return code checked
   (flush/dispatch/dispatch_pending), and loss is TRIAGED before
   dying — both peers gone is an orderly teardown (info, exit 0),
   the compositor alone dying while X lives is reported as exactly
   that, and a wl protocol error names the interface+opcode as a
   compositor bug. scripts/test-xwm-loss.sh reproduces it
   deterministically: SIGSTOP Xwayland (pure wl-side loss), kill the
   compositor; the pre-fix code hangs the check, the fixed code
   exits in <5s with the right diagnosis.

2. **EWMH fullscreen was entirely absent** (ROADMAP M8's "per-output
   X mode tracking"): X11 clients that fullscreen (runtime
   _NET_WM_STATE message — what GTK/Qt/SDL send — or the map-time
   property games use) got nothing, and the atoms were not even
   advertised. window-control v3 adds set_fullscreen; the helper
   forwards both request paths and keeps the _NET_WM_STATE property
   in sync (preserving foreign atoms); the compositor — the single
   state authority — runs the SAME fullscreen logic xdg-shell gets
   (saved restore geometry, the window's output rectangle =
   per-output tracking, foreign-toplevel/taskbar state) and mirrors
   the resulting geometry through the existing event; pre-map
   requests are deferred to just after placement (pending-identity
   pattern); a fullscreen/maximized window's state geometry now wins
   over the first buffer size at map. Verified with the REAL path:
   wmctrl fullscreens the real xeyes to exactly 1280x720+0+0, xprop
   sees the synced property, remove restores (test-realapps.sh);
   both EWMH paths pinned in the suite (xwm-fullscreen, x11client
   grew fullscreen/unfullscreen/state/draw commands + a map-time
   flag).

3. **The surface-space ratchet** (found by #2's verification): while
   watching the real xeyes full screen, the geometry mirror shrank
   it 2px per round (150→148→...→0) forever. Root cause: Xwayland
   sizes some windows' wl_surfaces to the X11 extent (interior +
   2*border — x11client/xterm style, measured 240x140 border 1 →
   buffer 242x142) and others to the interior alone (Xaw xeyes,
   200x100 border 1 → buffer 200x100) — SAME Xwayland binary, both
   conventions live. The helper's fixed extent conversion resized
   already-correct interior-space windows on every mirror; each
   echo round (ConfigureWindow → damage → new buffer → role_commit
   → mirror) ate 2*border. The fix MEASURES the convention per
   window (first mirror vs the X truth; re-measured when the
   current mode's prediction stops matching, so client self-resizes
   can't wedge it) and converts in the measured space — mirror,
   pending-apply, and the set_geometry push. xeyes now gets exactly
   one mirror and stays at its size; the pre-fix probe-only tests
   never saw this because x11client never draws.

Also fixed en route: the test-realapps app_id check raced the
identity-vs-first-commit ordering (now accepts either log shape);
my own debug scripts' SIGKILLed sessions leaked compositor/panel
processes that broke test-session's pgrep cleanliness checks
(cleaned; the lesson: kill session children via the session, not
SIGKILL).

Known remaining, honestly scoped: X11 clipboard bridge
(wl_data_device <-> X selections — needs the helper as a
data-device client; INCR for large transfers), EWMH workspace
mirroring (_NET_CURRENT_DESKTOP sync), the X-side activation
channel, OR-window surface convention unmeasured (extent assumed;
border-0 popups are unambiguous).

**Validation**: 123/123 in-process (11 xwm tests), 144/144 session,
51/0/1 build regressions, XWayland stack PASS, xwm-loss PASS,
full-session acceptance 27/27 (incl. the wmctrl/xeyes/xprop
fullscreen block), zero-warning -Werror builds.

Next: X11 clipboard bridge, EWMH workspace mirroring, activation
channel (backlog order).

## Session 2026-09-05 — the central geometry round (physical-NVIDIA findings)

**Commission**: physical NVIDIA testing showed the automated suite
green while the desktop was wrong: decorations/grabs offset
off-center, fullscreen with a one-sided gap, granted resizes not
landing, white/invisible X11 windows, Mirage rendering but
unusable. Directive: audit EVERY coordinate space, instrument,
find the single source of truth, no per-app special cases, no
papering over.

**The audit** (all spaces A-K mapped first, then one instrument —
`XW_GEOMETRY_TRACE=1`, `xw_wm_trace_geometry()` prints model /
surface-pos / buffer / output / usable / state / restore / seat in
one line; `xw_wm_trace_pick()` prints what the hit-test resolves;
the seat logs the global→surface-local translation; the helper
already logs every mirror). Reproduced EVERY physical symptom
headlessly with a real Xwayland + real helper + real Xlib client
(the new `xwm-geometry-truth` battery, 10 failures on the unfixed
tree — hit-test 0/5, phantom input rect at (0,0), no pointer
events reaching X11 clients, no pixels, fullscreen gap, resize
revert).

**Root causes — one canonical model, five concrete breaks:**

1. `xw_surface_get_pos()` had NO branch for
   XW_SURFACE_ROLE_XWAYLAND: every X11 surface reported position
   (0,0)+buffer-size while rendering used the window model. Render
   said one rect, hit-test/pointer-translation/damage used another
   — the grab/hit offset, the phantom clickable rect at the
   top-left, X11 clients receiving GLOBAL pointer coordinates
   (every widget hit-test off by the window position = "the app
   is non-functional"), and the compositor starving Xwayland of
   pointer events entirely (no wl_pointer enter → no X pointer
   motion → nothing interactive). Fix: the XWAYLAND branch returns
   the model rect, same as xdg — one rect, four consumers
   (render, hit-test, pointer translation, damage).

2. `deliver_frame_callbacks()` skips surfaces with
   `s->mapped == false` — and NOTHING ever set `s->mapped` for
   toplevel roles (xdg AND xwayland; only cursors/subsurfaces/
   lock surfaces had it). Xwayland 24.1 presents its NEXT frame
   only after the previous wl_callback fires
   (xwl_screen_post_damage skips windows waiting on a frame
   callback): every X11 window presented exactly ONE frame (the
   initial background — white for apps with a background pixel,
   empty/transparent = "invisible") and froze. This also explains
   "shows its UI but is non-functional" for frame-clocked native
   clients (GTK frame clock). Fix: the window-map/unmap funnel
   (`xw_wm_window_map` / `xw_wm_or_map` / `xw_wm_window_unmap`)
   now maintains surface->mapped. Verified against Xwayland
   24.1.6 source in .apps-root (the whole present path:
   ensure_surface_for_window requires manual redirect — the
   helper's CompositeRedirectSubwindows(root, Manual) is correct
   and required — damage_report → damage_window_list →
   blockhandler → swap_pixmap copies the window backing into the
   shm buffer → attach; all machinery intact, only the callback
   was missing).

3. State geometry (fullscreen/maximized) was clobbered by
   stale-size commits: both role-commit paths (xdg
   toplevel_apply_commit, xwl role_commit) adopted the committed
   buffer size unconditionally — a pre-resize commit rewound the
   model to the old size, then the mirror pushed the old size
   back to X, reverting the client's own resize: the fullscreen
   "gap" and the unreliable granted resizes. Fix: while
   state-held, the granted rect is authoritative; mismatched
   commits re-assert (configure / notify_geometry) instead of
   adopting. State windows now also map AT their state rect
   (window_map sets usable/output before configure/mirror).

4. Granted-vs-committed race (found by the full-suite run order):
   an in-flight pre-grant commit (e.g. the unfullscreen
   resize-cleared backing) landed AFTER the interactive grant and
   rewound the model. Fix: `xw_geom_pending` — notify_geometry
   marks the grant in flight; commits carrying a different size
   while pending are stale pre-grant state; set_geometry (the X
   truth echo) clears the flag. "Compositor granted 360x240" is
   now provably distinct from "client committed 360x240".

5. The helper's position math was 1px-per-round off: X reports
   window x/y as the OUTER origin (border INSIDE it); the extent
   rect (== the compositor model == the surface) is anchored
   exactly there. surf_to_pos/interior_pos_to_surf added +bw /
   -bw to POSITIONS (only SIZES convert by 2*bw). Every mirror
   round shifted X11 windows 1*bw off the model. Fix: positions
   are identity across the mirror; OR pushes dropped the -bw.

**The tests** (both proven red on the unfixed tree — the
revert-check re-disabled FIX B and both went red again):
- `xwm-geometry-truth` (X11): model == X extent (settled),
  hit-test center+4 corners of the model rect, no phantom rect,
  pointer events window-local in X (MOTION/BTN with coords),
  pixels composited at the model rect, fullscreen model == X
  extent == pixel corners (no gap), interactive resize grant
  stable across the client redraw + X truth == granted.
  x11client gained: GEOM (XGetGeometry+translate truth), MOTION/
  BTN/ENTER window-local reports, border<N>, draw [color],
  readpix, move, mask.
- `geometry-native` (native Wayland window): hit-test == model,
  render bbox == model, pointer position window-local (toolkit
  truth), fullscreen fills corners after the client redraw, and
  the frame-callback liveness check (a MAPPED toplevel's
  wl_callback MUST fire — the native twin of the freeze).

Also: the focus-routing test's stale comment claimed "click on A
through injected pointer events" while calling
xw_wm_focus_window directly — the pointer path was never
exercised for X11 windows; the geometry battery now covers it
for real (that is why the suite was green while the desktop was
wrong: no test asserted compositor-side geometry or pixels, only
X-side truth).

**Validation**: 125/125 in-process (12 xwm tests + the native
geometry test), 144/144 session, 51/0/1 build regressions,
XWayland stack PASS, xwm-loss PASS, realapps 27/27, full
ASan/UBSan/LSan round PASS (125/125 + 144/144, zero reports).

**Honest acceptance status**: everything headlessly verifiable is
now verified (render/hit/pointer/fullscreen/resize invariants for
both window families, real pixels, real Xwayland). The physical
NVIDIA checklist (drag with a real hand, Mirage's menus, xterm
selection with a real mouse) still requires hands on the machine:
the geometry model those interactions depend on is now single,
canonical, and regression-pinned, but "desktop functional" must
be claimed only after a physical pass. Not yet implemented (same
backlog as before): X11 clipboard bridge, EWMH workspace
mirroring, X-side activation channel.

## 2026-09-06 — the input/focus/cursor/window-state round

**The physical reports**: geometry improved, but the desktop was
still not usable — unreliable cursor state (stuck until another
cursor region), "most clicks do not register", right-click KILLING
Wayland applications, X11 windows flashing on click, taskbar
buttons not restoring windows, Mirage/browser unusable.

**Root causes found and fixed** (each pinned by a new regression):

1. **Hit-test did not follow the render order.** `surface_at`
   scanned ALL FOUR layer-shell levels BEFORE the windows, while
   the renderer paints background/bottom BELOW windows. A
   full-screen background layer (a wallpaper) sat above every
   window in the hit-test: clicks landed on the wallpaper client,
   not the window under the cursor. Hit-testing now mirrors
   `xw_render_output` exactly: popups → overlay → top → OR
   windows → managed windows → bottom → background.
   (`input-hit-test-order`)

2. **The cursor was never reset on focus changes.** The Wayland
   cursor is per-enter; the old code kept the previous client's
   image until some OTHER client happened to change it — the
   stuck cursor. New state machine (requested / applied /
   displayed) with serial validation (fabricated serials
   rejected, stale cross-client requests rejected, roled
   surfaces rejected — never fatal) and default-arrow reset on
   every cross-client focus transition.
   (`input-cursor-state`, `pointer-set-cursor`)

3. **Double `popup_done` killed clients.** The outside-press
   dismissal AND the client's own null-buffer unmap each sent
   `popup_done`; the second landed on an xdg_popup object the
   client had already destroyed — invalid object — libwayland
   killed the client. THE right-click killer: popup_done is now
   once-per-lifetime. (`input-popup-done-once`, revert-verified
   red)

4. **The null-buffer unmap transition did not exist.** A
   role surface committing NULL after displaying a buffer is
   HIDING (the protocol's hide): the old code ignored it for
   toplevels (GTK-hidden windows stayed visible, clickable and
   in the taskbar forever) and `popup_apply_commit` RE-MAPPED an
   unmapped popup with no buffer — a dismissed menu came back
   as an invisible input-eating rect at its old position (the
   ghost popup that swallowed clicks). Plus the re-show path:
   a buffer commit on an unmapped toplevel now restarts the
   configure cycle (previously the window could never come
   back without destroying its surface).
   (`input-toplevel-null-unmap`)

5. **`wl_surface.attach` is sticky.** Every no-attach commit was
   treated as a detach: the displayed buffer got RELEASED and
   the window blanked mid-frame (a flash source on redraw).
   Only an explicit `attach(NULL)` detaches now.
   (`subsurface-lifecycle`, `geometry-native` — both went red
   with the old semantics)

6. **Buffer-destroy UAF in the render path.** A client
   destroying a committed buffer left `s->shm` pointing into the
   freed `wl_shm_buffer`; the next repaint composited freed
   memory (ASan: SEGV in pixman from blit_surface). Xwayland
   frees buffers/pools aggressively on redraw/resize — the
   white-window/flash family. Content now clears with damage on
   destroy. (pinned by every rawc test that destroys its buffer
   post-commit)

7. **Taskbar activation was refused.** `xw_wm_focus_window`
   tested visibility BEFORE unminimizing: minimized == invisible
   → "refusing to focus invisible window" → the unminimize call
   below was unreachable for exactly the windows that needed it.
   A bare `activate` (no `unset_minimized`) on a minimized
   window now restores + focuses + raises. The taskbar's
   unset_minimized+activate pair was already working; the
   bare-activate path (xdg-activation, other taskbars) was dead.
   (`input-taskbar-activate`)

8. **`popup_grab`'s iterator could plant a sentinel.** On a
   no-match, `wl_list_for_each` leaves the iterator at the list
   head; assigning it to `s->ptr_grab` stored a fake wl_resource
   in the grab state. Found-variable pattern (same class as the
   panel-click UBSan finding).

9. **The dismissing press is consumed by the grab** (xdg-shell
   semantics): the press that dismisses a popup is not delivered
   to the surface below; focus still follows the surface under
   the cursor. (panel-menu updated accordingly)

**Instrument**: `XW_INPUT_TRACE=1` — every pointer event's raw
coordinates, hit-test pick, delivery target, and every cursor
state transition (old/requested/applied, serial, rejection
reason) on stderr, alongside `XW_GEOMETRY_TRACE`.

**New battery** (`tests/suite/test_input.c`, real libwayland
clients, full event recording): the priority-1 event matrix
(enter/leave/motion/L-M-R buttons/axis with serials and
surface-local coordinates), the hit-test order, the cursor state
machine, the right-click context-menu flow (press → popup +
grab → enter → item click → outside-press dismissal → parent
refocus → destroy), popup-destroy-under-grab, popup_done-once,
toplevel null-commit hide/show, taskbar activation cycles
(incl. cross-workspace), and a REAL GTK4 process (zenity) driven
with motion + L/R/M clicks that must survive. The harness now
records the exact protocol error when a test client dies
(`xwt_record_death`).

**Validation**: 124/124 in-process (release), 124/124 under
ASan/UBSan/LSan with zero reports, 144/144 session, XWayland
stack PASS, xwm-loss PASS, realapps 27/27, build-regressions
51/0/1.

**Honest acceptance status**: the input/focus/cursor/state
invariants are headlessly pinned for real clients (raw protocol
+ GTK4). The physical NVIDIA pass remains the authority: the
right-click kill is fixed at the protocol level (double-done +
ghost-popups + UAF all reproduced headlessly and reverted-red),
but Wine/Heroic/Browser-class workloads could not be exercised
in-container — their remaining failures, if any, will trace
through XW_INPUT_TRACE / XW_GEOMETRY_TRACE. Backlog unchanged:
X11 clipboard bridge, EWMH workspace mirroring, X-side
activation channel.

---

## Round 3 — the A/B/C physical-debug round (2026-09-06)

The three physical reports after f49614f: (A) Backspace types "u",
(B) the move/resize hit-offset (resize cursor on the title bar, MOVE
only engages deeper inside the app, both stacks), (C) fullscreen
still leaves a large one-sided gap on the physical NVIDIA box.
Method for each: instrument the whole chain, RED-reproduce
headlessly, fix the narrow root cause, pin with a regression, keep
the physical box as the final authority.

### A — keyboard: the compositor is exonerated, byte-perfect

The full-chain instrument (XW_INPUT_TRACE=1, stderr): libinput's raw
keycode at the source, the seat's entry line (raw linux keycode, the
+8 wl keycode, the keysym a compliant client computes, press/release,
all four modifier masks, focus surface, destination client pid), the
modifiers line with its serial, the kb-bind line (keymap size,
version, repeat), the kb-focus transitions, and the per-outcome line
(delivered serial / consumed-by-VT / shortcut / interactive /
dropped). A physical run now answers "wrong event from the
compositoritor, or wrong interpretation in the client" by diffing
these lines against what the app shows.

Two new deliverables: `tests/keyboardprobe.c` — the minimal RAW
Wayland keyboard client (no toolkit, no libxwcl; its own xkb state
from the delivered keymap; spot-checks the keymap keycode space at
startup; reports every enter/leave/modifiers/key/repeat_info with
serials and the client-side decode; flags anomalies: keycode < 8,
non-monotonic serials, unpaired release, double delivery, BackSpace
decoding to a printable letter — the literal symptom) — runs against
any compositor socket, container or NVIDIA box: `keyboardprobe
<socket> [seconds]`. And `scripts/test-physical-kbd.sh` +
`tests/kbddriver.c` (XSendEvent, libX11-only): drives the report
matrix through the REAL X keycode space (evdev+8, the same
convention XWayland uses) into the X11-backend compositor while the
probe records the wire — PASS: wl 22 = BackSpace (not 'u'), 16/16
events, 0 anomalies.

The in-process matrix (`seat-keyboard-matrix`): the keymap
spot-table (22 BackSpace, 23 Tab, 30 u, 36 Return, 38 a, 50 Shift_L,
37 Control_L, 67 F1 — pins the evdev+8 keycode space itself) plus
the exact 16-event stream for Backspace / u / a / Shift+Backspace /
Ctrl+Backspace / Alt press-release: keycode, client-computed
keysym, press/release, mods at delivery, exactly-16 (no stale or
replayed events). GREEN. Headless conclusion: raw 14 -> wl 22 ->
BackSpace is correct through every layer this container can
exercise, including the X keycode space. The physical "u" must
therefore live in what the container cannot run: the physical
libinput device, the physical config's RMLVO, or the client app
itself — the probe + trace decide which on the next physical run.

### B — THE move/resize hit-offset root cause: found, RED, fixed

Symptom reproduction: a CSD toplevel (set_window_geometry with a
shadow/header offset — every GTK/kitty/Firefox window) received
pointer events whose surface-local coordinates were WINDOW-RECT
relative. The protocol's surface-local space is the BUFFER origin:
with a geometry offset (geo_x, geo_y) the buffer origin sits that
far left/above the window rect, so every delivered coordinate was
shifted up-left by exactly (geo_x, geo_y). The client's widget,
header-bar and resize-margin zones live in buffer space, so the
user hovering the VISIBLE title bar made the client believe the
pointer was in its shadow/resize margin — the resize cursor on the
title bar — and a window MOVE only engaged once the pointer was
pushed (geo_y) pixels deeper into the app. Render and hit-test
already honored the offset; only the event stream was wrong.

Fix (one canonical conversion, no special cases):
`xw_surface_to_local()` in xw-surface.c is now the ONE global ->
surface-local translation (buffer-relative, geometry offset
included) — `xw_surface_has_input_at` uses it, and every delivery
site switched to it: wl_pointer enter (focus transition + late-bind
replay), wl_pointer motion, and the data-device drag enter. The
inverse `xw_surface_buffer_pos()` (buffer origin in global coords)
fixes the popup family: xdg_positioner anchors are parent-SURFACE
(buffer) coordinates, so `popup_place` now anchors off the buffer
origin (menus land under their buttons, not (geo_x, geo_y) off) and
`popup_send_configure` reports x/y relative to the same origin.

Regressions (RED -> GREEN): `input-csd-pointer-geometry` (enter,
motion, re-enter must all be buffer-relative: delivered 20,15 ->
30,29 was the failing shape) and `input-csd-popup-anchor` (server
placement + configure coordinates; the configure x/y alone cancels
the origin, so the assertion checks the placed rect — placed
(700,360) vs correct (690,346) was the failing shape). XWayland-role
surfaces have no geometry offset: same helper, unchanged numbers.

### C — fullscreen: every headless layer pinned, the DRM leg traced

The zero-gap battery `geometry-fullscreen`, three legs:
(A) native xdg fullscreen on a SECOND output at a non-zero layout
origin (1000,200 640x480 — the global (0,0) assumption trap): model
== output rect exactly, pixels cover the four corners AND four edge
midpoints of that output, unfullscreen restores.
(B) the XWayland role driven through the REAL protocols — a raw
client binding xwayland_shell_v1 + xw_window_control_manager_v1 v3
(the exact channel the WM helper uses): set_serial, map, EWMH-style
set_fullscreen — model goes to its output rect, the grant mirrors
back through the geometry event, a stale client commit cannot
shrink it, leave restores.
(C) CSD geometry + fullscreen: the state-held rect wins over the
geometry declaration, the render covers edge-to-edge.
All GREEN — the model + render chain is correct for both stacks
headlessly. What the container cannot exercise is the DRM scanout
leg, so it is now instrumented: XW_GEOMETRY_TRACE prints the
physical chain's first link at output setup (mode, CRTC, FB size,
pitch, layout origin, logical size, scale). A one-sided physical
gap with a correct model rect now points at exactly one place.

### Instrument set for the physical NVIDIA run

XW_INPUT_TRACE=1 (libinput raw -> seat chain -> delivery, serials,
pids, cursor/focus/popup state) and XW_GEOMETRY_TRACE=1 (wm
geometry transitions, motion translation, output/mode/scanout
setup). build/tests/keyboardprobe against the live socket.
TESTING.md's matrix unchanged; the physical bar stays the authority.

### Validation (this container: wayland+xkbcommon+pixman+X11 only —
no libdrm/libudev/libinput/libXtst dev files, no Xwayland binary)

127/127 in-process (release), 127/127 under ASan/UBSan/LSan with
zero reports (one leak fixed: the new raw xwayland-role client in
test_core.c must destroy every proxy — reg/shm/comp — like
rc_destroy). Child-process leak check PASS.
scripts/test-physical-kbd.sh PASS (the X-keycode-space matrix
end-to-end against a live compositor). link-deps 7/7.
Environment-gated (not runnable HERE, unchanged code paths):
session drm rounds 8 checks (needs the drm backend compiled),
build-regressions R6 (libinput/udev variants), the XWayland stack
and realapps rounds (need .apps-root). `make all` no longer breaks
where libXtst is absent: x11probe is now auto-gated on XTest
availability (kbddriver covers the same key matrix with plain
libX11). Backlog unchanged: X11 clipboard bridge, EWMH workspace
mirroring, X-side activation channel.

Push addendum (2026-09-06): 854d49e pushed to origin/main clean
(f49614f..854d49e) with a fresh one-shot PAT via
scripts/git-askpass-env.sh — nothing persisted to disk or config,
verified no ~/.git-credentials. Remote head confirmed at 854d49e.

**Next — the physical NVIDIA decision run** (this container is done;
every headless layer is pinned): build and run on the physical box
with `XW_INPUT_TRACE=1 XW_GEOMETRY_TRACE=1` (stderr), and
`build/tests/keyboardprobe <socket> [seconds]` while typing
Backspace / u / a / Shift+Backspace / Ctrl+Backspace in both a probe
window and the reporting app. Decision table: (A) probe decodes wl 22
-> BackSpace while the app shows 'u' — the app/keymap client-side,
look at its RMLVO/xkb config; probe shows wl 30 or anomalies — capture
the trace lines, the compositor input chain is implicated; no events
reach the probe — focus/delivery, XW_INPUT_TRACE's per-outcome lines
decide. (B) confirm the CSD fix with real apps (title bar -> MOVE
cursor + drag, edges/corners -> resize, client area inert) on both
stacks. (C) with the model rect == output rect and the gap still on
screen, the XW_GEOMETRY_TRACE output-setup line (mode/CRTC/FB/pitch/
layout/scale) is the one place left to look — one-sided gap with a
zero-pitch/stride mismatch points at the scanout FB pitch leg.

---

## Round 4 — the trace-observability round (2026-09-06)

The physical report: with
`XW_INPUT_TRACE=1 XW_GEOMETRY_TRACE=1 ./build/bin/xw-compositor -B drm`
graphical logout becomes impossible/awkward; without the variables it
works. That is by definition NOT a TTY/session consequence — the
instruments were changing behavior.

**Root cause (reproduced headlessly, RED first)**: every trace line
(and the default `xw_log` sink) was synchronous stdio on stderr. The
compositor's signal sources live in the event loop
(wl_event_loop_add_signal, signalfd-style), so a compositor blocked
inside `write(2, ...)` to a stalled stderr NEVER dispatches SIGTERM.
xw-session's logout path (stop_compositor) gives 1s grace then
SIGKILLs: no VT restore (still graphics mode), no KDSKBMODE XLATE
(keyboard still RAW), no DRM teardown — "logout impossible". The
regression `trace-shutdown-observational` (in the suite) reproduces
it deterministically: forked compositor children with stderr = a
pipe the parent never reads, storm of keys + motions under each
variable alone and both together, then SIGTERM. Pre-fix all three
trace variants wedge (verified by /proc diagnostics: `syscall: 1
0x2 ... wchan: pipe_write` — literally blocked writing stderr), the
no-trace control passes. Answer to "which variable": BOTH,
independently — input carries the per-event volume on a live
desktop, geometry's per-motion line does the same; whichever fills
the stalled sink first kills the logout.

**The fix — one never-blocking diagnostic sink**
(`xw_diag_line`/`xw_diag_vline`, xw-util.c): every line-level
diagnostic (input trace, geometry trace, drm setup trace, default
log sink) formats into a bounded stack buffer and emits ONE write.
Sink selection: regular files use raw fd 2 (never blocks, keeps the
shared offset with the binary's own fprintf); pipes and terminals
get a PRIVATE reopen through /proc/self/fd/2 with O_NONBLOCK (a new
file description — fd 2, children, nothing else changes semantics);
sockets fall back to fd 2 guarded by a zero-timeout poll. A line
that cannot be written now is DROPPED and counted; the first
successful write after a stall reports `[trace] N diagnostic lines
dropped (stderr stalled)` so physical runs know the trace is
incomplete. Line formats are byte-identical to the old ones (the
physical decision tables in Round 3 depend on them). Also fixed
along the way: the move-begin grab-offset line printed
UNCONDITIONALLY (a trace leak into un-instrumented runs — now gated
like its siblings), the per-motion getenv in the seat is cached,
and the compositor's crash handler writes with raw write() instead
of fprintf (a fault landing inside any stdio writer used to
deadlock the handler on the stderr lock and swallow the backtrace).

**keyboardprobe (the physical key-wire recorder) under the asan
build**: LSan found the probe's own leaks (xkb_context/keymap/state
on every exit path — round 3's "destroys every proxy" missed the
xkb side), and LSan's _exit skips the stdio flush, which silently
ate the probe's summary line and failed the physical-kbd script.
Fixed: full teardown on every exit path (connect failure, missing
globals, buffer failure, normal exit) + explicit fflush before
teardown. physical-kbd now PASSes under BOTH profiles.

**Validation**: 128/128 in-process release, 128/128 under
ASan/UBSan/LSan with zero reports (the new test forks children;
_exit keeps their teardown out of the parent's leak check),
physical-kbd PASS (asan and release), link-deps 7/7. The instrument
set and decision tables from Round 3 are unchanged — same lines,
same gates, now guaranteed observational.

**Next**: unchanged — the physical NVIDIA decision run (A: probe +
input trace diff against the reporting app; B: real-app CSD hit
confirmation; C: the DRM output-setup line when a gap persists).

---

## Round 5 — the corrected session-path audit: the supervisor was the missing leg (2026-09-06)

The correction that reframed Round 4: the physical machine runs
`./build/bin/xw-session`, NOT a bare compositor — the earlier physical
comparison (`xw-compositor -B drm` ± trace variables) never touched the
real logout path (a bare compositor has no session manager for xw-exit
to talk to; its "graphical logout" cannot work at all). Round 4's
in-suite regression therefore modeled a REAL mechanism but an
INCOMPLETE chain: the compositor with a stalled stderr pipe — with no
supervisor in the picture. This round audited the real path and
reproduced it end to end.

### The real logout chain (audited, code-pinned)

xw-exit (the graphical dialog, Ctrl+Alt+Del / panel exit button)
writes `logout\n` to the session control socket -> xw-session's
handle_ctl_line replies "ok ending session" and runs
session_shutdown(false): SIGTERM to autostart/spawned children, then
stop_compositor() — SIGTERM, 1s grace, SIGKILL — then the session bus,
reap, cleanup_sockets, exit 0. The compositor's SIGTERM arrives via
its event loop (wl_event_loop_add_signal/signalfd); its CLEAN exit
runs the seat teardown that restores the VT (KD_TEXT, KDSKBMODE
XLATE, VT_AUTO). Answers to the audit questions:

A. **Trace variable propagation: YES.** start_compositor forks and
execs without touching XW_INPUT_TRACE/XW_GEOMETRY_TRACE — the
compositor child inherits the session's environment verbatim (only
XDG_*/DISPLAY are adjusted). Empirically pinned: the file-sink control
of the new harness shows 234,887 `[geom] xdg-commit-size` lines in the
SESSION's stderr file — lines only the compositor child emits.

B. **What blocks: the session manager itself, before any signal is
sent.** The compositor leg (Round 4's fix, cdcf2c5) holds: during the
wedge the compositor sits healthy in epoll_wait, still storming at
~118k commits/s, dropping trace lines per the never-blocking sink.
But xw-session's own `log_msg` was plain stdio fprintf on the
inherited stderr — and the compositor child's trace flood fills that
SAME sink (64KB in ~1ms under the storm). The FIRST shutdown log line
("session ending") blocks forever: /proc shows `write(2, fd 2, 18)`
wchan `pipe_write` — the 18 bytes are literally "[xw-session info] ".
SIGTERM is never sent, the compositor never exits, the session never
exits, and the ctl connection is never closed (xw-session-ctl /
xw-exit's xw_ctl_send reads until EOF — the dialog client hangs too).
That is "logout impossible" through the REAL path.

C. **Logout initiation: the session manager, via the ctl socket** —
not a compositor protocol request, not a raw signal. SIGTERM is
xw-session's stop_compositor, the supervisor's first move against the
compositor. Ctrl+Alt+Del only SPAWNS xw-exit (a compositor shortcut
action); the decision then travels the control-socket line protocol.

D. **Does the trace-only failure reproduce under xw-session?** YES —
given a stalled stderr sink (any non-draining consumer: `2>&1 | tee`
into a jammed reader, a held fifo, a stopped pipeline). Pre-fix
(396d41f): BOTH processes wedge — the compositor in
`write(fd 2, 128 bytes)` pipe_write AND the session in
`write(fd 2, 18 bytes)` pipe_write. On cdcf2c5 as committed: the
compositor is healthy, the session still wedges — cdcf2c5 alone does
NOT fix the real reproduction. On this round's build: neither wedges;
logout completes in ~104ms under all four permutations. With the
DEFAULT sinks (inherited TTY — writes drain; regular file — never
blocks) it never reproduced at any revision: the stalled-pipe sink is
the one class that blocks, and it only occurs through user-side
redirection choices.

E. **Does cdcf2c5 fix the real xw-session reproduction?** No — see D.
It fixed the compositor-only leg its RED test modeled. The complete
fix is cdcf2c5 + this round's session leg (both changes are now in).

### The two regressions, explicitly separated

1. **compositor-only stderr-pipe deadlock** (Round 4, cdcf2c5): real
   mechanism, partial coverage — a compositor whose stderr stalls
   wedges in its own diagnostic writes; fixed by the never-blocking
   xw_diag sink. This is what the in-suite `trace-shutdown-
   observational` white-box regression pins (both variables,
   independently, storm in-process).
2. **the actual xw-session physical logout regression** (this round):
   the same stalled sink ALSO wedges the supervisor — which is the
   leg that owns the SIGTERM, the 1s grace, the SIGKILL, and the
   control-socket close. A wedged supervisor turns "slow logout" into
   "logout impossible" and (on DRM) "black screen with a RAW keyboard".
   Fixed by giving xw-session's log_msg the same never-blocking
   contract.

Which one explains the NVIDIA/Artix observation: if the physical run
piped or redirected the session's stderr into a non-draining consumer
(the natural way to CAPTURE a trace whose TTY is hidden behind the DRM
desktop), the full chain above is the explanation — and notably,
cdcf2c5 alone would NOT have fixed it (the session leg still wedges).
If the physical run left stderr on the TTY or wrote it to a file, the
stderr-blocking theory explains nothing — and with every headless leg
now pinned, the corrected 4-permutation physical matrix through
xw-session (below) is the decisive experiment. Either way the
observational contract is now enforced at BOTH ends: trace volume can
cost at most dropped lines (reported with a count), never a wedged
session.

### The fixes

`xw-session.c` (narrow, no behavior change beyond the contract):
- log_msg funnels through a private never-blocking sink that mirrors
  libxw's xw_diag discipline exactly: one bounded buffer, one write;
  regular files use raw fd 2 (never blocks, shared offset); pipes and
  terminals get a private O_NONBLOCK reopen of /proc/self/fd/2 (a new
  file description — fd 2, the compositor child, every child keep
  their semantics; O_CLOEXEC so exec starts clean); sockets fall back
  to fd 2 behind a zero-timeout poll. Unwritable-now lines are
  dropped and counted; the next successful write reports
  "[xw-session] N diagnostic lines dropped (stderr stalled — output
  incomplete)".
- stop_compositor's SIGKILL leg now runs the crash-console-restore
  net (restore_console_after_crash) when the compositor died by
  signal: stop_compositor reaps internally, so the main loop's
  crash-restore branch could never see this exit — on DRM, a
  compositor that misses the 1s grace left the VT in graphics mode
  and the keyboard RAW with NO net. Clean SIGTERM exits still do
  their own (proper) seat teardown; the net is best-effort and a
  no-op when /dev/tty is not a VT.

### The instrument set (new)

- `tests/geomstorm.c` — the external trace-volume driver: a raw
  wayland client that commits alternating buffer sizes (96x96 /
  110x84) in a flush-paced loop (~118k commits/s, one
  `xdg-commit-size` [geom] line each). Pacing = the harness's second
  observable: a wedged compositor stops consuming and geomstorm parks
  in POLLOUT (heartbeat freezes). Also drains the server's event
  stream — a client that never reads overflows the server's 4096-byte
  connection buffer and gets dropped mid-storm.
- `scripts/test-session-trace.sh` — the session-level regression:
  the REAL chain (xw-session -B headless, real compositor child, real
  ctl-socket logout) x the four trace permutations x a stalled-fifo
  stderr (read end held open by a sleeping holder, never drained) +
  a file-sink control. Assertions: session exits promptly (measured
  ms), exit code 0, ctl socket removed; on failure it prints /proc
  syscall+wchan for BOTH the session and the compositor child, plus
  the ctl client's fate — the wedge is attributed, not guessed.
  Takes an optional bin-dir argument (how the pre-fix builds were
  compared). 21 checks.

### Validation

- harness on this round's build: 21/21 (all four stalled-pipe
  permutations logout in ~104ms, exit 0, sockets cleaned; file
  control: 234,887 propagated trace lines).
- harness on pristine cdcf2c5: 15/21 — geometry/both stall variants
  FAIL, session wedged in write(fd 2) pipe_write, compositor healthy
  in epoll_wait, ctl client dead in read (rc=124).
- harness on pristine 396d41f: 15/21 — both processes wedged in
  write(fd 2) pipe_write (compositor 128 bytes, session 18 bytes).
- in-process suite: 128/128 release and 128/128 under
  ASan/UBSan/LSan with zero reports (the `make asan` pass fails only
  at test-session.sh's 8 pre-existing environment-gated DRM checks —
  byte-identical failure count on the pristine 396d41f/cdcf2c5
  builds, verified).
- scripts/test-session.sh: 116/124 (same 8 gated), physical-kbd PASS,
  link-deps 7/7.

### The corrected physical procedure (A/B/C, now through xw-session)

The decision tables from Round 3 are unchanged — only the RUNNER
changes (plus the trace-capture guidance):

0. **The logout matrix first** (the observational contract checkout):
   on the NVIDIA/Artix box, for each of (no trace / XW_INPUT_TRACE=1 /
   XW_GEOMETRY_TRACE=1 / both): start `./build/bin/xw-session`, use
   the desktop briefly, then Ctrl+Alt+Del -> Log Out (or
   `build/bin/xw-session-ctl logout` from a second VT). PASS = the
   dialog responds, the session ends, the shell prompt returns, the
   keyboard types (not RAW), no black screen. Capture with
   `2> trace.log` (file — always safe) — never an unread pipe.
1. **A (backspace-u)**: `XW_INPUT_TRACE=1 XW_GEOMETRY_TRACE=1
   ./build/bin/xw-session 2> trace.log`, then
   `build/tests/keyboardprobe wayland-0` (the session's socket lives
   in $XDG_RUNTIME_DIR) while typing Backspace / u / a /
   Shift+Backspace / Ctrl+Backspace in both a probe window and the
   reporting app. Same decision table as Round 3: probe decodes wl 22
   -> BackSpace while the app shows 'u' — client-side keymap/RMLVO;
   probe shows wl 30 or anomalies — compositor chain (capture the
   [input] lines); no events reach the probe — focus/delivery, the
   [input] per-outcome lines decide.
2. **B (CSD hits)**: real apps on both stacks (title bar -> MOVE
   cursor + drag, edges/corners -> resize, client area inert).
3. **C (fullscreen gap)**: model rect == output rect but the gap on
   screen — read the XW_GEOMETRY_TRACE output-setup line (mode/CRTC/
   FB/pitch/layout/scale) from the trace.log; one-sided gap with a
   pitch/stride mismatch points at the scanout FB pitch leg.

## Round 6 — the first physical matrix feedback: logout ① PASS, and the 11 pixman BUG blocks (2026-09-06)

The physical box delivered its first matrix leg: a full no-trace
session log through `./build/bin/xw-session` (the REAL runner, the
corrected procedure from Round 5). Read in order:

### What the physical log says

- **Permutation ① (no trace): PASS.** The logout ran the complete
  audited chain — exit click -> ctl -> `session ending` -> autostarts
  stopped in order -> compositor stopped -> clients saw the pipe break
  (`Conductă(pipe) ruptă` = the GTK locale string for EPIPE on the
  compositor's socket death) -> session d-bus stopped -> `session
  manager exiting`, all inside ~191ms (10:43:04.849 ->
  10:43:05.040). The supervisor leg holds on real hardware. The
  stderr sink was the TTY itself (no pipe), so the stalled-sink class
  was not in play — permutations ②③④ with `2> trace.log` remain the
  open legs of the matrix.
- **Eleven pixman BUG blocks, exactly while kitty was the active
  client**: `*** BUG *** In pixman_region_union_rect: Invalid
  rectangle passed` — a NEW physical signal, this round's root cause
  (below).
- The NVIDIA page-flip timeout fired once at startup and the fallback
  engaged as designed: `page flip on HDMI-A-1 never completed ... the
  driver ACCEPTED the flip but no vblank event arrived within 300ms ->
  immediate buffer updates`. Expected on the legacy flip path; the
  desktop rendered and interacted normally after it.
- **XWayland is missing on the box** (`no Xwayland binary found`): X11
  apps cannot start, so the B-leg (CSD hits on BOTH stacks) is blocked
  until Xwayland is installed or `$XW_XWAYLAND_CMD` points at one.
- Expected noise, no action: wlr-output-power-management and
  wlr-output-management unsupported (unimplemented protocols —
  XFCE DPMS and display-settings tools degrade, documented), RTKit
  `ServiceUnknown` (no rtkit service on the box; PipeWire falls back
  to priority 1), icon-name fallbacks (missing theme icons).

### The 11 BUG blocks: root cause

All region16 call sites in libxw are the 16-bit pixman API (the error
message has no `32`) — and three client-facing handlers forwarded
protocol ints to pixman VERBATIM: `wl_surface.damage`,
`wl_surface.damage_buffer` (÷ scale, sign preserved), and
`wl_region.add`/`subtract`. pixman's failure classes for
out-of-domain input (pinned empirically, scripts/pixman-semantics.c
et al. during the round, then distilled into the committed harness):

- negative w/h: `*** BUG *** Invalid rectangle passed` logged, rect
  DROPPED (the unsigned width parameter wraps, x2 < x1);
- zero w/h: silently dropped;
- coordinates beyond the int16 box domain: SILENTLY WRAPPED into
  garbage extents (x=40000 becomes x=-25536 — wrong damage, no
  diagnostic at all);
- negative ORIGINS: legal (surface-local coords) and always worked.

The physical 11: kitty streamed protocol-invalid damage rects; our
handlers handed them to pixman; pixman logged ~10-11 and then went
quiet — the count SATURATES (measured: `union(bad)+clear` cycled on
one region produces exactly ~10 blocks whether the cycle count is 50
or 225,373 — pixman's internal validation settles into a
non-erroring accumulation). The magnitude match (physical 11, storm
10) is the attribution: same mechanism, one surface, saturating
validator.

The in-suite twin: `tests/geomstorm.c` sends
`damage(0, 0, INT32_MAX, INT32_MAX)` on EVERY commit (x2 truncates
into int16 as -1 — inverted box), so every session-trace harness run
has been carrying the same ~10 BUG blocks in its stderr sinks all
along; we only grepped [geom] lines and never noticed until the
physical log made pixman's own voice audible.

### The fix (protocol-boundary sanitation, zero behavior change)

`xw_region16_rect_clamp()` (xw-internal.h, static inline): 64-bit
corners first (x+w must not overflow int before clamping), then clamp
into the int16 box domain; drop empty/inverted rects and rects
entirely outside the domain; valid rects — including negative origins
— pass through UNCHANGED. Applied at the four protocol boundary
handlers: `surface_damage`, `surface_damage_buffer` (after the scale
division), `region_add`, `region_subtract` (a dropped rect subtracts
nothing — semantically identical to pixman's own drop, minus the BUG
spam; a clamped rect gives init_rect a domain-valid box so it can
never wrap either). The internal union sites keep their existing
guards (`w > 0 && h > 0` at every caller of xw_output_damage_rect;
full-output damage at (0,0,w,h)).

`pending_damage` is write-only in the render path today (cleared at
commit; repaint extent comes from window-extent damage), so the
clamp/drop distinction has no rendering consequence — but the
accumulator now holds HONEST state for the day something consumes it:
the storm's full-damage rect survives as (0,0,32767,32767) instead of
being dropped, and nothing pixman-side can log or wrap.

### The instrument set (new)

- `tests/suite/test_core.c: damage-rect-boundary` — the white-box RED:
  fresh surface, valid-rect positive control (union + extents, commit
  clears), then the invalid batch under an intercepted `stderr` (the
  libc `stderr` FILE* swapped to a pipe — pixman writes its BUG
  blocks through that pointer, verified before designing the test;
  the swap window contains no asserts so it is always undone).
  Assertions: zero captured BUG text, and the post-fix accumulation
  semantics (negative/zero dropped, overflow clamped+coalesced to
  (0,0,32767,32767)). Pre-fix: 4 failures, 9 BUG blocks captured
  (1042 bytes). Post-fix: clean.
- `scripts/pixman-bug-count.sh` — the session-level RED through the
  REAL chain (test-session-trace environment minus trace variables):
  storm + file sink + ctl logout, then count `Invalid rectangle`
  blocks in the session's stderr. Pre-fix: 10. Post-fix: 0, exit 0.

### Validation

- in-process: 130/130 (13 skips — the XWayland apps-root gates),
  ASan/UBSan/LSan clean (`make asan` PASS + 130/130).
- session-trace harness: 21/21 (all four stalled-pipe permutations
  logout ~104ms; file-sink control 190,630 propagated [geom] lines).
- pixman-bug-count: 0 (pre-fix 10).
- test-session.sh: 128/128 (the environment-gated legs now PASS in
  this container — the sysroot bootstrap installed the DRM dev stack,
  so the "fails honestly without KMS" checks exercise the real
  refusal path; Xvfb legs SKIP).
- link-deps: 7/7.

Build-hygiene note (pre-existing, not this round): the Makefile has no
header dependency tracking — xw-internal.h edits do not rebuild the
objects that include it; clean profiles (`make` / `make asan`) remain
the guaranteed path.

### Physical next steps (the matrix continues)

Permutations ② (XW_INPUT_TRACE=1) ③ (XW_GEOMETRY_TRACE=1) ④ (both),
each `2> trace.log` through `./build/bin/xw-session`, brief use,
Ctrl+Alt+Del -> Log Out; then the A/B/C probes per the Round 5
procedure. kitty's damage stream is now a NON-issue (this fix), so a
re-run of ① that USED to show 11 blocks should now show 0 — a quick
confirmation that the physical box picked up the new build. B needs
Xwayland installed on the box first.

---

## Round 7 — the wire keycode space and the activation lifetime: two spec-convention bugs, both reproduced headlessly (2026-09-06)

The physical box delivered its second full session log: permutation ①
(no trace) PASS again — the complete logout chain, ~191ms class; **zero
pixman BUG blocks** (the Round 6 boundary fix confirmed on the box, 11
→ 0); XWayland now LIVE on the box (started by the session, WM helper
up, X11 socket present — the B-leg is unblocked). Two open wounds came
with it: "the Keyboard is still fucked" and "the web browser still
crashes". Both fell this round, headlessly, plus one honest
non-reproduction.

### The read of the physical log

- LibreWolf (pid 7827) was a **direct Wayland client** — not X11: the
  compositor's client-death line `error in client communication (pid
  7827)` is libwayland-server's own EPIPE report and it names the
  browser's pid, not Xwayland's (7526). Firefox ≥121 auto-selects
  Wayland; the session exports XDG_SESSION_TYPE=wayland. Xwayland is a
  red herring for the browser crash.
- Two `[xw-warn] xdg-activation: invalid or used token` lines sit
  between LibreWolf's startup and its death.
- The xkbcomp blocks (FK23/FK24 redefined, unresolved
  XF86ElectronicPrivacyScreen*/ActionOnSelection/Contextual* keysyms,
  `Unsupported maximum keycode 709, clipping`) are **benign noise**:
  our (correct, xkbcommon-generated, X-space) keymap compiled by the
  box's Xwayland against a slightly older keysym table. Documented so
  it stops alarming anyone.

### Bug 1 — the keyboard: the wire keycode space (the actual "Backspace types u")

`wl_keyboard_send_key(k, serial, time, keycode + 8, ...)` — the seat
put **evdev+8** on the wire, with a comment codifying the belief
("wayland/xkbcommon keycodes are evdev + 8"). The truth is split:
xkbcommon STATE lookups use evdev+8 (the shared keymap's keycode space
is the classic X space, `<BKSP>=22`) — but the WIRE keycode is the RAW
evdev code, and every real client (Xwayland, GTK, kitty, Firefox)
adds the 8 itself before its own lookups. We sent 22 for Backspace;
clients looked up 30 and typed 'u'. Every key, every app, both stacks
(X clients see wire+8 → evdev+16).

**Why four rounds of tests never saw it: circular validation.** The
probe (tests/keyboardprobe.c), libxwcl's kb_key (decoded the wire code
as X-space and passed `key - 8` to the callback), the in-suite matrix
and scripts/test-physical-kbd.sh ALL shared the compositor's
convention — the probe's spot table asserted "wl 22 = BackSpace" and
its anomaly check literally flagged raw-evdev codes as "raw linux code
sent un-offset?". Round 3's decision table was therefore poisoned at
the root: branch (A) — "probe decodes wl 22 -> BackSpace while the app
shows 'u' — the app/keymap client-side, look at its RMLVO" — was the
shared-convention blindness talking, not evidence. The probe was never
"the way a compliant client must decode"; it decoded the way OUR
compositor encodes. Meanwhile the nested backends' own comments
("the parent delivers evdev keycodes / Wayland button codes [verbatim]")
documented the REAL convention all along — including
xw-backend-nested.c, i.e. running xw nested under sway/weston fed raw
codes into a +8-sending seat.

**The fix (convention alignment, zero behavior change on the seat's
internal xkb math):**
- seat: both `wl_keyboard_send_key` sites send the raw keycode; the
  xkb_state updates stay at +8 (shortcut engine, modifiers — all
  unchanged, which is why shortcuts worked while apps typed 'u').
- trace: `raw=%u wire=%u xk=%u` (raw = what libinput reported, wire =
  what's on the protocol, xk = the lookup address a compliant client
  computes); the doc block rewritten around the true contract.
- libxwcl: decode at wire+8, callback layer gets the raw wire code —
  the callback VALUES are identical to before (the old code passed
  `wire - 8` = raw), so the panel, lock passphrase and exit dialog key
  handling is untouched.
- keyboardprobe: compliant-client decode (+8), spot table in raw evdev
  with both shift detectors: wire 14 decoding printable = shifted LOW;
  wire 22 decoding to BackSpace = an X-space keycode on the wire —
  "THIS IS the backspace-types-u bug".
- scripts/test-physical-kbd.sh expectations flipped to the raw matrix
  (wire 14 = BackSpace, 22 = u, 30 = a).

**The pins (so it can never silently regress):**
- `seat-wire-keycode-space` (test_seat.c): a SECOND raw wl_seat/
  wl_keyboard binding on the test client's own connection, asserting
  the LITERAL wire bytes for injected 14/22/30 — an observer that does
  NOT move with libxwcl's decode. This is the test that would have
  caught the bug on day one.
- `xwm-x11-keys` (test_xwm.c): the X leg through a REAL Xwayland —
  x11client learned `keys on` (per-(client,window) event masks, the
  WM's own selections never clobber these) and reports
  `KEY <code> <keysym> press|release [text=]`; injected raw 14/22/30
  must arrive as X keycodes 22/30/38 with BackSpace/u/a. The nested-X
  matrix could never cover this leg.

### Bug 2 — xdg-activation: the token credential died with the token object

Reproduced headlessly first: zenity (GTK4) through the realapps
harness produced the SAME two `invalid or used token` warnings — one
per presented window. The spec (staging/xdg-activation) says two
things our implementation violated:

1. `xdg_activation_token_v1.destroy` is a destructor whose text ends
   "**The received token stays valid.**" — our token_resource_destroy
   removed and freed the credential with the resource. GTK4 and
   Firefox destroy the token object immediately after `done` and call
   `activate()` later with the bare string → lookup always failed →
   "invalid or used token" → every real activation (window present,
   focus handover) rejected, forever.
2. `set_surface` is "the surface **requesting** the activation. Note,
   this is different from the surface that will be activated." — our
   validation gate was `t->surface == s`, i.e. an equality demand
   between the requester and the target, which is precisely the
   launcher→app handover shape the protocol exists for.

**The fix (xw-activation.c, restructured):** credential/wrapper split —
`struct xw_activation_token` (the string, requester pointer (advisory,
never deref'd), created_ms, link, res_dead flag) stays in
`comp->activation_tokens` past the object's death;
`struct token_res` (the wl_resource wrapper) dies with the resource and
only flips `cred->res_dead`. GC frees dead-resource credentials after a
60s TTL (live-object entries are never freed under their own handlers —
bounded by client behavior); `xw_activation_fin` frees everything left
(SAFE: `wl_display_destroy_clients` at compositor teardown runs BEFORE
fin, so every wrapper is already dead — ordering pinned in
xw-compositor.c). Validation: string match, single-use consumption on
match, the equality gate REMOVED. Rejection stays warning-only (the
spec gives clients "no way to discover the validity of the token" —
posting an error would kill well-behaved clients).

Pinned by the new destroy-before-activate leg in
`foreign-toplevel-activation` (the exact GTK4/Firefox shape), and
verified against the real thing: zenity's warnings GONE from the
realapps log, zero rejections in the firefox run below.

### The browser: an honest non-reproduction

firefox-esr 140.14.0 fetched into .apps-root (5 debs — the container's
system stack already satisfies the closure) and
`scripts/repro-firefox.sh` written: a REAL Firefox as a native Wayland
client of a headless xw-session, fresh profile, **its own stderr
captured separately** (the physical box mixed all output into one TTY
sink — that is why the death looked silent). Result: alive 40s+,
window MAPPED, zero client-connection deaths, zero activation
rejections, and a CLEAN session logout with the browser still attached.
Then the A/B: the SAME firefox run against the PRE-ROUND build (git
stash) — **also survives**. Conclusions:

1. The activation bug and the keycode bug, while real, are NOT the
   physical browser killer (activation rejection is non-fatal; wrong
   keysyms don't crash Firefox).
2. The physical crash is not reproducible on the software-rendering
   path. The environmental delta is the **NVIDIA proprietary EGL
   stack**: physical Firefox initializes WebRender via client-side EGL
   on our socket, and NVIDIA's EGL needs wl_drm / linux-dmabuf /
   egl-stream interfaces the compositor does not implement (the
   documented dmabuf/GL backlog item — the same class of failure as
   the box's own `Xwayland glamor: GBM Wayland interfaces not
   available` line). A segfault in that path can't be caught by
   Firefox and produces exactly the silent EPIPE we saw.

The physical decision experiment (next round's first input):
`MOZ_WEBRENDER_SOFTWARE=1 librewolf` (and/or
`LIBGL_ALWAYS_SOFTWARE=1`), stderr captured separately
(`librewolf ... 2> ff.log`), plus `dmesg | tail` after a crash (rule
out the OOM killer — SIGKILL is equally silent). Survives with
software WR → the fix is linux-dmabuf (backlog), interim workaround =
the env var; still dies → the ff.log decides.

### Validation

- in-process: 132/132 release (13 skips only when apps-root is absent;
  with it populated the XWayland legs including the two new pins run),
  `make asan` PASS (132/132 + test-session 128/128, zero reports).
- session-trace harness: 21/21. pixman-bug-count: 0.
- scripts/test-physical-kbd.sh: PASS — the full X->seat->wire chain now
  reads wire 14 = BackSpace, 22 = u, 30 = a, 16/16 events, 0 anomalies
  (kbddriver's X-injected matrix decoded by the flipped probe).
- test-realapps: no failures (wmctrl + x11-utils fetched into
  apps-root; the fullscreen leg now runs).
- repro-firefox: PASS on both builds (see above).
- Build hygiene note (pre-existing): no header dependency tracking —
  clean `make` / `make asan` remain the guaranteed path; `make asan`'s
  restore step does not rebuild build/tests/x11client, so run
  `make build/tests/x11client` after profile switches (the xwm tests
  gate on its existence and silently skip otherwise).
- scripts/test-physical-kbd.sh now sources env.sh (runtime libs when a
  local sysroot provides libinput's dependencies — libmtdev).

### Physical next steps (the Round 7 matrix)

0. Rebuild + install on the box; re-run a no-trace session: the two
   `xdg-activation: invalid or used token` lines must be GONE.
1. Keyboard, the literal report: type Backspace / u / a /
   Shift+Backspace / Ctrl+Backspace in kitty AND in LibreWolf AND in
   an X11 app (xterm through the now-installed XWayland) — all three
   stacks must agree. `build/tests/keyboardprobe <socket>` while
   typing: zero ANOMALY lines, and its keymap spot table reads
   "wire 14 (KEY_BACKSPACE, xk 22) -> BackSpace".
2. The browser decision run: `MOZ_WEBRENDER_SOFTWARE=1 librewolf`
   (+ separate stderr capture + dmesg check) — see the experiment
   above; the answer routes the next round (linux-dmabuf vs.
   ff.log-driven triage).
3. Optional confirmation of the Round 7 keyboard fix shape:
   `XW_INPUT_TRACE=1 ./build/bin/xw-session 2> trace.log`, type one
   Backspace in kitty: the trace line must read
   `key: raw=14 wire=14 xk=22 sym=0xff08 'BackSpace'`.

---

## Round 8 — the LibreWolf interaction-crash root cause: the unilateral wl_subsurface destroy (2026-09-06)

Physical report: "browser opens, renders, dies the moment I interact
with anything"; browser stderr 6x "Exiting due to channel error.",
session trace 3x "error in client communication (pid ...)". The
keyboard was already fixed (Round 5); this round was the browser.

### Rig work (scripts/repro-browser-crash.sh)

- The 7-step battery from the last session never exercised the
  popup/subsurface paths, so it was vacuously green. Extended to 16
  steps: link hover (set_cursor + tooltip popup), link navigation,
  right-click context menu (xdg_popup + grab), outside-press
  dismissal, Escape dismissal, hamburger-menu cycle, URL-bar
  autocomplete (popup + reposition), drag selection, ctrl+a/ctrl+c
  (set_selection).
- Backend detection fixed: the old stat-inode compare could never
  match (client fds carry their own endpoint inode); now ss-based
  (firefox shows as "wayland (direct)").
- New legs: --wl (client trace; Firefox's libwayland ignores
  WAYLAND_DEBUG), --wlsrv (server-side wire trace — the one that
  worked), --a11y (session dbus + at-spi, which also silences
  Firefox's org.a11y.Bus autolaunch warning).
- Fixed a bash trap: an EMPTY array prefix before an assignment
  prefix (DISPLAY=...) makes bash re-parse the assignment as a
  command name at execution time ("DISPLAY=:96: command not
  found"). The prefix is now always `env` / `env WAYLAND_DEBUG=1`.

### Reproduction and root cause

- Deterministic repro: right-click context menu, dismiss with
  outside click, right-click again, Escape -> Firefox parent dies
  (SIGSEGV, 7 children "Exiting due to channel error.", compositor
  "error in client communication (pid N)") — the exact physical
  signature. GFX1: "Wayland protocol error: wl_display#1: error 0:
  invalid object 47".
- The --wlsrv wire trace: wl_surface#40.destroy() (the dismissed
  menu's surface, PARENT of wl_subsurface#47) made our compositor
  unilaterally wl_resource_destroy the wl_subsurface#47 resource ->
  delete_id(47) for an object the client still owns. Firefox's
  widget dispose later sends wl_subsurface#47.destroy(); the request
  fails demarshal server-side ("invalid object", untraceable — the
  print happens only after successful demarshal), the server posts
  the fatal protocol error, libwayland kills the client.
- spec check (wayland.xml): wl_subsurface.destroy is a CLIENT
  request ("remove sub-surface interface"); wl_surface.destroy
  "deletes the surface and invalidates ITS object ID" — neither
  grants the server a unilateral wl_subsurface retirement. The
  pre-fix comment ("the client's object is defunct") was simply
  wrong: Firefox/GTK finalize subsurface objects after the surfaces,
  from a later dispose cycle. Legal client behavior.

### Fix

- src/libxw/xw-subcompositor.c: xw_subsurface_parent_destroyed() and
  xw_subsurface_role_destroy() now only DETACH (damage + unrole +
  unlink + null the resource user_data) — the wl_subsurface object
  stays alive and inert until the client destroys it; every request
  on a detached object is a no-op via the existing NULL-user_data
  guards. The OWNERSHIP RULE is documented above
  subsurface_destroy().

### Validation

- 16/16 steps survive in all three legs (plain / --wlsrv / --a11y),
  0 "error in client communication" lines.
- wire proof post-fix: wl_subsurface#47.destroy() arrives 1.1s AFTER
  the popup teardown and is accepted cleanly (pre-fix: death); the
  second menu repeats the pattern clean.
- make check: test-session 128/128, build-regressions 51/51.

### Physical next steps

- Pull + rebuild + retest LibreWolf: menus, URL bar autocomplete,
  tooltips, right-click + Escape — the interaction battery above.
  The previously-reported crash should be gone.

---

## Round 8 addendum — the f86ea88 push (2026-09-06)

Pushed to origin/main (7b06cd3..f86ea88) for the physical box: the
repro-script commit plus the subsurface fix, rebased clean on top of
the remote keyboard round (only WORKLOG.md conflicted — two parallel
"Round 7" narratives; the keyboard round kept Round 7, the subsurface
round became Round 8). The .upstream checkouts carry pure file-mode
noise (0644→0755, zero content lines) — not pushed, not load-bearing.
Next: pull, rebuild, run the 16-step LibreWolf interaction battery
(right-click + outside-dismiss + right-click again + Escape is the
deterministic killer; add tooltips, URL-bar autocomplete, drag
selection).
