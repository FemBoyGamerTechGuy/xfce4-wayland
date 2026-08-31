# SECURITY

## Threat model (v0, headless/dev focus)

The compositor (`xw-compositor`) is the privileged-ish component in the
session: it mediates input, window state, and pixels between mutually
distrusting client processes. The realistic threats for a Wayland
session apply:

1. **Malicious/compromised client** trying to read or spoof other
   clients' content, steal focus, grab global input, or crash the
   compositor (DoS).
2. **Protocol parsing bugs** in our server-side implementations (custom
   code, so custom bugs — wl_display gives us safe demarshalling, but
   our request handlers must validate arguments).
3. **Local attack surface**: session control socket permissions; state
   files in the runtime dir; launched processes.

## Current posture

- **Sandboxing by Wayland design**: clients cannot address each other
  directly; content only flows through the compositor, and we never
  forward one client's buffers to another (screensharing protocols are
  not yet enabled; when enabled, ext-image-copy-capture access will be
  gated).
- **Input**: global shortcuts are consumed before delivery to clients;
  clients only receive input for surfaces they own. Activation (focus
  stealing) is permission-gated through xdg-activation tokens tied to
  real input events.
- **Control socket** (`xw-session ctl.sock`): created under the user's
  `$XDG_RUNTIME_DIR` (0700) with 0600 socket permissions; only
  same-UID processes may issue session/power commands. Power actions
  require logind policy anyway (loginctl applies PAM/polkit rules).
- **Launched processes**: autostart entries execute with the user's own
  privileges; Exec= lines are parsed per the XDG spec subset; no shell
  interpolation is performed on .desktop fields.
- **Memory**: wl_resource destruction is bound to client lifecycle via
  wl_display destroy hooks; test suite runs with leak counting enabled.
- **Build**: -Wall -Wextra -Werror; no compiler warnings tolerated.

## Known gaps (explicit, not silent)

- No client resource limits yet (fd/buffer quotas) — a client can
  currently exhaust memory by committing many large buffers; planned
  per-client accounting in M9.
- Popup grabs and xdg_popup positioning follow a conservative subset of
  the spec; untrusted-client hardening review pending.
- Screenshot/lock protocols not yet exposed (see ROADMAP M4) — by
  design, they are security-sensitive and only land with a review.
- XWayland (future) will require careful isolation (separate Xauthority
  per session, no shared X sockets).
- Fuzzing of our request handlers is planned (ROADMAP M9).

Report issues in the tracker; security-relevant reproducers are welcome.
