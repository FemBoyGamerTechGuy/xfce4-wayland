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

- The `>_` Start button opens the **applications menu**: an
  `xdg_popup` parented to the bar layer, anchored under the button,
  listing XDG `.desktop` entries (`$XDG_DATA_HOME`,
  `$XDG_DATA_DIRS`; `NoDisplay`/`Hidden`/`OnlyShowIn`/`NotShowIn`
  filtered like xfce4-panel). Selecting an item launches it through
  the session manager (ctl `run <exec>`); Escape, an outside press
  or the Start button itself closes the menu; Enter launches the
  hovered item. When NO applications exist at all, the button falls
  back to launching the resolved terminal (`$XW_TERMINAL` or the
  first common terminal on PATH). v0 deviations: 24-item cap (no
  scrolling) and `Terminal=true` entries are hidden (no terminal
  wrapper yet). See ROADMAP.md.
- The clock is **display-only**: a click on it intentionally does
  nothing (no popup/calendar). See ROADMAP.md.
- The workspace switcher clicks switch workspaces via ext-workspace;
  hover highlights are drawn client-side from wl_pointer events.
- The Exit button asks the session manager for the exit dialog
  (ctl `exit-dialog`, fire-and-forget), the same path as
  Ctrl+Alt+Del.
- Every panel action is **non-blocking**: menu launches and the exit
  request round-trip through forked ctl helpers, so no session
  action can ever stall the panel's Wayland dispatch.

## Diagnostics

`XW_PANEL_TRACE=1` (set automatically by `xw-session --verbose`) prints
the full startup chain and the interaction chain: pointer enter,
button press with widget identity, and every activated action.

The menu lifecycle is logged unconditionally (user-triggered, rare,
high-value): `start clicked`, `menu opening (item count=N ...)`,
`menu already open`, `menu surface created (WxH, item count=N)`,
`menu closing (why) — menu surface destroyed`, `menu item selected:
'name' -> ctl "run ..."`. A panel crash on repeated Start activation
is debuggable from these lines plus the compositor's own log alone.
