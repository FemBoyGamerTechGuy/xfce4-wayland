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

