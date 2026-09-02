# subprojects/panel — the Wayland xfce4-panel (v0)

The desktop panel: a layer-shell TOP bar (exclusive zone = its height)
with the v0 plugin set — launcher, workspace switcher, tasklist, clock,
exit button.

## What this component is

- A **standalone Wayland client**. It links `libxwcl` (the client
  library in `src/libxwcl`) and nothing from the compositor.
- Buildable without the compositor: `make panel` produces
  `build/bin/xw-panel` from `libxwcl` + `xw-ctl` only — no `libxw`
  objects are compiled or linked.
- Talking ONLY protocols: `wl_seat`/`wl_pointer`, `zwlr_layer_shell_v1`,
  `wlr-foreign-toplevel-management`, `ext-workspace`, plus the
  session-manager line protocol (`xw-ctl.c`) for launcher/exit actions.
- Replaceable: the compositor never references this binary (the
  session manager starts it, `--no-panel` / `$XW_PANEL_CMD=none`
  disables it). Any other layer-shell client can serve as "the panel"
  of a session.

## v0 behavior (documented deviations from xfce4-panel)

- The `>_` launcher button spawns a terminal (resolved from
  `$XW_TERMINAL` or the first common terminal on PATH) through the
  session manager's ctl socket — there is no application menu yet.
- The clock is **display-only**: a click on it intentionally does
  nothing (no popup/calendar). See ROADMAP.md.
- The workspace switcher clicks switch workspaces via ext-workspace;
  hover highlights are drawn client-side from wl_pointer events.
- The Exit button asks the session manager for the exit dialog
  (ctl `exit-dialog`), the same path as Ctrl+Alt+Del.

## Diagnostics

`XW_PANEL_TRACE=1` (set automatically by `xw-session --verbose`) prints
the full startup chain and the interaction chain: pointer enter,
button press with widget identity, and every activated action.
