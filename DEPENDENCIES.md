# Dependencies

Philosophy: **minimal dependencies, but not at the expense of
functionality, maintainability, security or correctness.** Every entry
below earns its place; the build system makes optional ones degradable
with actionable messages (see [BUILDING.md](BUILDING.md)). No wlroots,
no GLib, no D-Bus linkage, no toolkit — see
[ARCHITECTURE.md](ARCHITECTURE.md) for the rationale.

Licenses were audited against the upstream projects; provenance of
vendored files lives in
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md). "Copyleft" flags
the *obligations using the library triggers* — linking a (L)GPL library
does not force our proprietary license to change, but redistribution
rules apply and are documented.

## Matrix

| Library / tool | Why it is required | Min version | License | Build / runtime | Nested | DRM/KMS | X11 compat | Optional | Copyleft obligations |
|---|---|---|---|---|---|---|---|---|---|
| **libwayland-server** | the display server core: client connections, protocol marshalling, event loop | 1.21 | MIT | both | ✔ | ✔ | ✔ | no | none (permissive) |
| **libwayland-client** | the nested backend is a client of the parent compositor; `libxwcl` client library; tests | 1.21 | MIT | both | ✔ | – | – | no | none |
| **wayland-scanner** | generates protocol C glue from the XML definitions at build time | matches libwayland | MIT | build only | ✔ | ✔ | ✔ | no | none |
| **wayland-protocols** | protocol XMLs we implement: xdg-shell, xdg-activation, ext-workspace, single-pixel-buffer | 1.36 (1.44 recommended) | MIT | build only (data) | ✔ | ✔ | ✔ | no | none |
| **libxkbcommon** | keymap compilation, modifier state, keysym resolution for the shortcut engine | 1.0 | MIT | both | ✔ | ✔ | – | no | none |
| **pixman-1** | software renderer: compositing, damage regions, image ops (output-pixel introspection used by tests) | 0.42 | MIT | both | ✔ | ✔ | ✔ | no | none |
| **libinput** | real input backend: keyboard/pointer events, device discovery, hotplug, pointer acceleration, v120 wheel protocol | 1.19 | MIT | both (backend optional) | – (parent feeds input) | ✔ (device ownership) | – | **yes** (`XW_LIBINPUT`) | none |
| **libudev** (via libinput) | udev-seat device discovery + hotplug for libinput | 183 | LGPL-2.1+ | runtime (libinput dep) | – | ✔ | – | with libinput | LGPL: relinking rights + license notice on redistribution |
| **libX11** | nested X11 backend: window, XPutImage present path, XKB detectable-autorepeat handling | any (1.6+) | MIT | both (backend optional) | ✔ (X11 parents, XLibre) | – | ✔ | **yes** (`XW_X11`) | none |
| **libXtst / libXi** | XTEST synthetic input for the process-level X11 backend tests | 1.2 / 1.5 | MIT | dev/test only | ✔ (tests) | – | – | yes (`make check` only) | none |
| **python3 + Pillow** | build-time bitmap font rasterization (`tools/genfont.py`); no runtime font stack | py3.8+ / Pillow 9+ | PSF / HPND-MIT | build only | ✔ | ✔ | ✔ | no (build) | none |
| **xkeyboard-config** | xkb keymap data (`evdev/pc105/us` + configured layouts) | any recent | MIT/HPND | **runtime only** | ✔ | ✔ | – | no (runtime) | none |
| **loginctl (systemd-logind or elogind)** | power actions (suspend/hibernate/poweroff/reboot) without root; later: session/seat/DRM-master acquisition | any | LGPL-2.1+ (systemd) / MIT-0 (elogind) | **runtime only** (probed, never linked) | ✔ | ✔ (planned) | – | yes (fails honestly without it) | none (external program) |
| **Xvfb** | virtual X server driving the X11-backend process checks | any | MIT | dev/test only | ✔ (tests) | – | – | yes | none |
| **libdrm** | *(future)* DRM/KMS backend: device discovery, CRTCs, planes, atomic modesetting, page flips | 2.4.110+ | MIT | future (Phase 4) | – | ✔ | – | **yes** (planned) | none |
| **XWayland** | *(future)* optional compatibility server for legacy X11 applications only — never a foundation of this desktop | 22+ | MIT/X11 | future (Phase 8) | – | – | ✔ | **yes** (planned) | none |

✔ = required for that mode · – = not used in that mode.

## Why these and nothing else

- **libwayland + wayland-protocols**: the definition of a Wayland
  compositor; there is no Wayland without them. Implementing the wire
  format by hand would be weeks of code with zero user-visible gain.
- **libxkbcommon**: the only maintained XKB keymap engine; keyboard
  maps and modifiers are a correctness surface, not a place for a
  hand-rolled subset.
- **pixman**: a battle-tested software rasterizer. Software rendering
  keeps every pixel deterministic and testable (the whole test suite
  asserts real framebuffer contents); a GL renderer path is a *later*
  accelerator, not a foundation.
- **libinput**: the input stack every non-wlroots Wayland compositor
  and every X server uses; it solves acceleration curves, wheel
  protocols, device quirks and hotplug correctly. Our input module is
  deliberately thin over it.
- **libX11**: only for the *nested X11* backend — the development
  workflow for users living in X11/XLibre sessions. The core never
  links it when built with `XW_X11=0`.
- **Pillow**: build-time only, replaces a runtime font stack
  (fontconfig/freetype/harfbuzz) with a 95-glyph generated bitmap —
  the entire client surface stays font-free.

## Explicitly rejected

| Candidate | Reason |
|---|---|
| wlroots | the project's core constraint: original implementation on libwayland-server, no wlroots code (behavioral inspiration from protocol specs only) |
| GLib / GIO | event loop + utilities we already have (libwayland's loop, our own INI/log/region code); D-Bus-free power path via loginctl CLI |
| GTK / Qt | clients are native Wayland on `libxwcl` with a generated bitmap font; a toolkit would be a larger dependency than the panel itself |
| cairo | pixman already does everything the software renderer needs; cairo would add a second rasterizer |
| D-Bus (libdbus/sdbus) | power + session actions go through `loginctl` (works with systemd-logind *and* elogind, explicit argv, no shell). A direct D-Bus `Can*` query is on the roadmap as an optional refinement |
| systemd libraries | loginctl CLI only; the project hard-codes no systemd assumptions and works with elogind |

## Adding a dependency: the bar

1. It must be impossible (or clearly harmful) to implement correctly
   ourselves (see "rejected" for the counter-examples).
2. Permissive license preferred; copyleft accepted only when no
   equally capable permissive alternative exists (libudev is the only
   current case, inherited via libinput) — obligations documented
   above and in THIRD-PARTY-LICENSES.md.
3. Runtime dependency must degrade honestly (probe + actionable
   message), like loginctl does.
4. Build dependency must be optional or trivially satisfied on all
   target distributions.
5. The decision is recorded in this file with the reason.
