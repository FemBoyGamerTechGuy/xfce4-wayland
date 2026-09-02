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
| **libinput** | real input backend: keyboard/pointer events, device discovery, hotplug, pointer acceleration, v120 wheel protocol; devices are opened through the seat provider | 1.19 | MIT | both (backend optional) | – (parent feeds input) | ✔ (device ownership) | – | **yes** (`XW_LIBINPUT`) | none |
| **libudev** | udev-seat context the input AND DRM backends create themselves: `udev_new()` for device discovery/hotplug; the DRM backend runs its own udev monitor for display hotplug | 183 | LGPL-2.1+ | both (with libinput or DRM) | – (parent feeds input) | ✔ (device enumeration + hotplug) | – | with `XW_LIBINPUT`/`XW_DRM` | LGPL: relinking rights + license notice on redistribution |
| **libX11** | nested X11 backend: window, XPutImage present path, XKB detectable-autorepeat handling | any (1.6+) | MIT | both (backend optional) | ✔ (X11 parents, XLibre) | – | ✔ | **yes** (`XW_X11`) | none |
| **libXtst / libXi** | XTEST synthetic input for the process-level X11 backend tests | 1.2 / 1.5 | MIT | dev/test only | ✔ (tests) | – | – | yes (`make check` only) | none |
| **python3 + Pillow** | build-time bitmap font rasterization (`tools/genfont.py`) of the bundled `assets/fonts/DejaVuSans-ascii.ttf`; no runtime font stack, **no system font package** | py3.8+ / Pillow 9+ | PSF / HPND-MIT (font asset: DejaVu license, see THIRD-PARTY-LICENSES.md) | build only | ✔ | ✔ | ✔ | no (build) | none |
| **xkeyboard-config** | xkb keymap data (`evdev/pc105/us` + configured layouts) | any recent | MIT/HPND | **runtime only** | ✔ | ✔ | – | no (runtime) | none |
| **loginctl (systemd-logind or elogind)** | power actions (suspend/hibernate/poweroff/reboot) without root. Seat/session acquisition does NOT go through it: the seat providers handle that (see "Seat and session management" below) | any | LGPL-2.1+ (systemd) / MIT-0 (elogind) | **runtime only** (probed, never linked) | ✔ | ✔ | – | yes (fails honestly without it) | none (external program) |
| **seatd (daemon)** | a seat manager for real TTY sessions: owns the DRM/input devices and hands fds to unprivileged compositors over its unix socket. Our built-in wire-protocol client talks to it directly — no library needed | 0.9 | MIT | **runtime only** (never linked, never built) | – | ✔ (one seat provider) | – | yes | none (external daemon) |
| **Xvfb** | virtual X server driving the X11-backend process checks | any | MIT | dev/test only | ✔ (tests) | – | – | yes | none |
| **libdrm** | DRM/KMS backend: device discovery, connector/CRTC/mode enumeration, dumb-buffer scanout, page flips, master management | 2.4.110+ | MIT | both (backend optional) | – | ✔ | – | **yes** (`XW_DRM`) | none |
| **libseat** | optional seat-provider portability layer: wraps systemd-logind, elogind and seatd behind one API. Not required: the built-in seatd client and direct-VT provider cover the same ground without it | 0.7 | MIT | both (provider optional) | – | ✔ (one seat provider) | – | **yes** (`XW_LIBSEAT`) | none |
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
- **libudev**: a *direct* dependency of our input backend, not an
  incidental libinput runtime dep. The backend calls `udev_new()`
  itself (udev seat mode), so the udev symbols are referenced by
  `xw-input-libinput.o` inside `libxw.a` and must be linked
  explicitly onto the final executables — upstream `libinput.pc`
  does not hand out `-ludev` (`Requires.private` only), and relying
  on libinput's indirect DT_NEEDED fails with modern ld
  (`--no-copy-dt-needed-entries`: the "DSO missing from command
  line" class). The Makefile therefore detects, validates and links
  `libudev` as its own pkg-config module whenever `XW_LIBINPUT` is
  on.
- **libX11**: only for the *nested X11* backend — the development
  workflow for users living in X11/XLibre sessions. The core never
  links it when built with `XW_X11=0`.
- **libdrm**: the kernel's own display API — the only honest way to
  drive a physical monitor. The backend uses dumb buffers + page
  flips (software scanout over the pixman pipeline); EGL/GPU rendering
  is a later accelerator, not a foundation.
- **libseat**: optional *by design*. The compositor ships its own
  seatd wire-protocol client (plain libc, protocol verified against
  upstream libseat in the test suite) and a direct-VT provider, so
  headless/nested/seatd/direct sessions all work without libseat.
  libseat adds its logind/elogind backends where the system provides
  them, selected at runtime by libseat itself.
- **Pillow**: build-time only, rasterizes the bundled font subset
  (a 43 KB licensed DejaVu Sans asset ships in the repository) into a
  95-glyph generated bitmap — the entire client surface stays
  font-free and the build never touches a system font (which made
  builds fail on distributions with different font layouts).

## Seat and session management

These concepts are deliberately distinct; confusing them is how
"requires systemd" assumptions sneak into compositors:

| Concept | What it is | Examples | Does this compositor assume one? |
|---|---|---|---|
| **seat manager** | grants unprivileged access to the seat's devices (DRM, evdev) and manages session/VT switching | seatd, systemd-logind, elogind | **no** — abstracted behind the seat providers, probed at runtime |
| **session manager** | supervises the desktop's processes, environment, autostart, shutdown | xfce4-session, xw-session (ours) | xw-session is ours, it assumes nothing about the seat |
| **display manager** | greeter that starts a graphical session at boot | SDDM, GDM, LightDM, greetd, Ly | **no** — a TTY login is a first-class launch path |
| **login manager / init** | PID 1 and the service supervisor around it | systemd, runit, OpenRC, s6, dinit | **no** — zero service-manager assumptions in code; docs show per-distro examples |

The compositor acquires its seat through one of three providers
(`src/libxw/xw-session-seat.c`), selected at runtime by capability
probing (or explicitly with `--seat-provider`):

1. **libseat** (if compiled in, `XW_LIBSEAT`): delegates to libseat,
   which itself picks logind / elogind / seatd per the system's
   configuration. Preferred where installed because it handles
   logind's device pause/resume semantics properly.
2. **built-in seatd client**: speaks the seatd 0.9 unix-socket wire
   protocol directly (no library, no D-Bus, no root). First-class
   support for seatd systems (Artix, Void, sway setups) with zero
   additional dependencies. Device fds arrive via `SCM_RIGHTS`.
3. **direct VT session**: opens the controlling tty, takes it into
   `KD_GRAPHICS` + `VT_PROCESS` mode, handles VT switching itself
   (SIGUSR1/SIGUSR2 via the event loop), and opens devices with the
   permissions the login already has — logind/elogind device ACLs of
   the active session, or traditional video/input group membership.
   This is the classic path for plain TTY logins without any seat
   daemon.

None of these ever escalates to root, and a total failure produces
the combined "unable to acquire a seat" diagnostic listing what was
tried (see BUILDING.md, "Troubleshooting").

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
   current case — a direct build/link dependency of the libinput
   backend, accepted with eyes open) — obligations documented
   above and in THIRD-PARTY-LICENSES.md.
3. Runtime dependency must degrade honestly (probe + actionable
   message), like loginctl does.
4. Build dependency must be optional or trivially satisfied on all
   target distributions.
5. The decision is recorded in this file with the reason.
