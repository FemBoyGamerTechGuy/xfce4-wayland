# subprojects/panel — the Wayland xfce4-panel

The desktop panel: a layer-shell bar (top or bottom, exclusive zone =
its height) with the XFCE plugin set — applications menu, launchers,
window buttons, graphical workspace pager, clock with calendar, exit
button. Layout is metric-driven (bar height derives from the output's
logical size; fonts 16/24 px rasters; icons scale) and configurable
through `$XDG_CONFIG_HOME/xw-panel/panel.conf` (see
data/examples/panel.conf; defaults are complete without any file).

## What this component is

- A **standalone Wayland client**. It links `libxwcl` (the client
  library in `src/libxwcl`) and nothing from the compositor.
- Buildable without the compositor: `make panel` produces
  `build/bin/xw-panel` from `libxwcl` + `xw-ctl` only — no `libxw`
  objects are compiled or linked.
- Talking ONLY protocols: `wl_seat`/`wl_pointer`, `zwlr_layer_shell_v1`,
  `wlr-foreign-toplevel-management`, `ext-workspace`,
  `xw-workspace-info-v1` (per-window workspace membership), plus the
  session-manager line protocol (`xw-ctl.c`) for launcher/exit actions.
- Replaceable: the compositor never references this binary (the
  session manager starts it, `--no-panel` / `$XW_PANEL_CMD=none`
  disables it). Any other layer-shell client can serve as "the panel"
  of a session.

## Behavior

Region order (the XFCE arrangement):

```
[Applications] [launchers...]   [window buttons...]   [pager][clock][Exit]
\_______ start region ______/  \__ flexible middle _/ \_ right region _/
```

- **Start** opens the applications menu: a two-pane popup (categories
  left, applications right — the Whisker shape) over the XDG
  `.desktop` database. Items: icons, names; categories: Favorites
  (from the config), All, Accessories, Internet, Multimedia, System,
  ... Typing filters across name/generic name/comment/id; the wheel
  and arrow keys scroll; Enter launches; Escape clears the search
  then closes. Launching parses the Exec line per the desktop-entry
  spec (quoting, field codes, `Terminal=true` wrapping with a
  per-terminal strategy) and goes through the forked ctl-run wire.
  When NO applications exist at all, the button falls back to
  launching the resolved terminal (`$XW_TERMINAL` or the first
  common terminal on PATH).
- **Window buttons** show icon + title per window; clicking the
  focused window minimizes it (the xfce4-tasklist default), clicking
  another restores + focuses it, middle/right click closes. When the
  middle region runs out of space the buttons squeeze to icon-only
  and finally overflow behind a "+N" indicator that opens a list of
  the hidden windows.
- **Pager**: every workspace is a miniature desktop — a bordered box
  with window tiles (the focused window accented, sticky windows
  dimmed on every box); the active workspace is outlined; a click
  switches. Membership comes from `xw-workspace-info-v1`.
- **Clock** shows date + time (config: format tokens, seconds);
  clicking it opens a one-month calendar popup (month navigation by
  arrows/wheel/keys, today highlighted, Monday-first grid).
- **Exit** asks the session manager for the exit dialog
  (ctl `exit-dialog`, fire-and-forget), the same path as
  Ctrl+Alt+Del.
- Every panel action is **non-blocking**: launches and the exit
  request round-trip through forked ctl helpers, so no session action
  can ever stall the panel's Wayland dispatch.

## Documented deviations from xfce4-panel

- Two-pane menu instead of cascading flyout submenus (one popup, one
  grab — simpler and equally discoverable).
- SVG icons are not renderable (no vector stack): PNG (optional
  libpng, `XW_PNG`) and XPM icons render, everything else falls back
  to procedural letter tiles.
- Calendar days are display-only (no event list — no calendar data
  source exists in the stack).
- No plugin API, no drag-reordering, single output, fixed plugin
  order (see ROADMAP.md).

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

The menu and calendar lifecycles are logged unconditionally
(user-triggered, rare, high-value): `start clicked`, `menu opening
(categories=N, M apps, popup WxH ...)`, `menu surface created`,
`menu closing (why)`, `launching 'name' (ctl ...)`, `clock clicked —
calendar opening (Month YYYY)`, `calendar closing (why)`, `overflow
button clicked — listing N hidden windows`. A panel crash on repeated
Start activation is debuggable from these lines plus the compositor's
own log alone.
