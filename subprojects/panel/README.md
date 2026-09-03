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
  then closes. Launching parses the Exec line per the
  desktop-entry spec (quoting, escapes, field codes, `Terminal=true`
  hosting with a per-terminal strategy) and executes the parsed argv
  **directly** with `posix_spawn` (panel-launch.c): no `/bin/sh -c`,
  no session-manager round trip, signal state reset
  (POSIX_SPAWN_SETSIGDEF|SETSIGMASK — an inherited blocked mask can
  never break the child's own Ctrl+C), stdio on /dev/null. Failed
  launches are visible: a red status line on the bar plus a full
  diagnostic trail (launch requested / desktop file / escaped exec /
  pid / failure reason); the menu stays open so another entry can be
  picked. When NO applications exist at all, the button falls back to
  launching the resolved terminal (`$XW_TERMINAL` or the first
  common terminal on PATH) the same direct way.
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
- Every panel action is **non-blocking**: the direct launcher spawns
  without ever waiting (the panel's SIGCHLD reaper collects children
  asynchronously — an app that crashes or exits immediately cannot
  hurt the panel), and the exit request round-trips through a forked
  ctl helper, so no session action can stall the Wayland dispatch.

## Documented deviations from xfce4-panel

- Two-pane menu instead of cascading flyout submenus (one popup, one
  grab — simpler and equally discoverable).
- SVG icons are not renderable (no vector stack): PNG (libpng,
  `XW_PNG`) and XPM icons render, everything else falls back to the
  generic app-grid glyph.

## Icon resolution (libxwcl `xwc-icon.c`)

Freedesktop icon-theme lookup, following the desktop's own theme
choice: `$XW_ICON_THEME` (set from `panel.conf [panel] icon_theme`) >
`gtk-3.0/settings.ini` `gtk-icon-theme-name` > `gtk-4.0` > XFCE
xfconf `xsettings.xml` `IconThemeName` > `hicolor`. `Inherit=` chains
in each theme's `index.theme` are walked (depth-limited, cycle-safe;
hicolor terminates every chain). `Icon=` values with an extension are
tried verbatim and stripped. Icon themes and sizes are selected per
the spec (smallest size >= request, else the largest available),
decoded PNGs/XPMs are rescaled with alpha-correct box filtering and
cached by `name@size`. Every miss is logged once
(`xw: icon missing: <name> — using fallback`) and renders the generic
app-grid glyph — a missing icon can never look like a rendering bug
or crash the panel.
- Calendar days are display-only (no event list — no calendar data
  source exists in the stack).
- No plugin API, no drag-reordering, single output, fixed plugin
  order (see ROADMAP.md).

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
