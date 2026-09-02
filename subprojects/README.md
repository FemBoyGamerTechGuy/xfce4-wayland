# subprojects/ — the desktop components

xfce4-wayland is a **general-purpose Wayland compositor platform** plus
an XFCE-flavored client ecosystem built on top of it. This directory
maps the components and, more importantly, the dependency contract
between them.

```
                      +----------------------+
                      | xw-compositor        |   src/libxw + src/compositor
                      | Wayland compositor,  |   (libxw — server library)
                      | WM platform          |
                      +----------+-----------+
                                 |  Wayland protocols only
      +--------------------------+--------------------------+
      |                          |                          |
      v                          v                          v
+-------------+          +-------------+          +------------------+
| subprojects/|          | xw-session  |          | other clients    |
| panel       |          | session     |          | src/clients/     |
| (this dir)  |          | manager     |          | xw-exit, xw-lock,|
+-------------+          +-------------+          | xw-demo          |
| libxwcl     |                          +------------------+
| (client lib,|                           session-ctl line protocol
|  src/libxwcl)|                          (unix socket) for actions
+-------------+
```

## Component map

| Component | Sources | Builds | Notes |
|-----------|---------|--------|-------|
| compositor | `src/libxw/`, `src/compositor/` | `make compositor` | server library + main; knows nothing about panels, sessions or XFCE |
| panel | `subprojects/panel/` | `make panel` | layer-shell client; links ONLY `libxwcl` + `xw-ctl` |
| session manager | `src/session/` | `make session` | starts/stops the compositor and desktop components; owns the ctl socket |
| session utilities | `src/clients/` | `make clients` | exit dialog, lock, demo — clients of both compositor (protocols) and session (ctl) |
| client library | `src/libxwcl/` | (library) | shared Wayland client helper: connection, xdg-shell, layer-shell, input, drawing |
| desktop | *planned* | — | wallpaper / desktop icons (see ROADMAP.md) |
| settings | *planned* | — | xsettings/daemon equivalents |
| file manager | *planned* | — | thunar-like client |

## Dependency contract (enforced by the build)

1. **The compositor never depends on any desktop component.**
   `build/bin/xw-compositor` links `libxw` + `libxwcl` and references no
   panel/session binary. `make compositor` succeeds with
   `subprojects/` deleted. Run it with no clients at all: it is a
   complete compositor/WM platform (try `build/bin/xw-compositor -B
   headless` or the nested backends).
2. **Clients never link compositor objects.** `make panel` builds from
   `libxwcl` + `xw-ctl` only; no `libxw` object is compiled. The panel
   reaches the compositor exclusively through Wayland protocols
   (`wl_seat`, `zwlr_layer_shell_v1`, `wlr-foreign-toplevel-management`,
   `ext-workspace`).
3. **Cross-component actions go through the session manager's ctl
   socket** (a 0700 unix socket at `$XDG_RUNTIME_DIR/xw-session.sock`,
   line protocol: `run <cmd>`, `exit-dialog`, `logout`, `status`, ...).
   No component `system()`s another, shells out behind the user's back,
   or reaches into another component's memory.
4. **Deliberate duplication over coupling**: the panel and the
   compositor each own a copy of the terminal-fallback list, because
   sharing it would make the panel link compositor code. Facts about
   the ecosystem (terminal names) are not behavior worth coupling for.
5. **WM policy is data + config, not clients**: stacking/tiling rules
   live in the compositor's WM (`src/libxw/xw-wm.c`, `rules.conf`);
   a different policy is a compositor-side change, never a panel
   privilege.

## Using the compositor without the XFCE desktop

- `make compositor && build/bin/xw-compositor -B drm` — a bare KMS
  session, no panel, no session manager (for testing or kiosk use).
- `xw-session --no-panel` — full session lifecycle without the bar.
- `$XW_PANEL_CMD=<anything>` — replace the panel with your own
  layer-shell client; the session starts it instead.
- The nested backends (X11/Wayland parents) let the compositor run as
  a window inside another desktop — handy for developing a custom
  shell or an alternative panel against it.
