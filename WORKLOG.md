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
