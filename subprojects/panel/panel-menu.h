/* panel-menu.h — the applications menu module.
 *
 * A single xdg_popup parented to the bar layer (positioner anchored
 * under the Start button — or above it for a bottom bar). Items come
 * from the XDG application database; launching goes through the
 * session's ctl "run" wire, forked so the click never blocks. */
#ifndef PANEL_MENU_H
#define PANEL_MENU_H

#include "panel.h"

/* the Start button: toggle the menu open/closed (idempotent by
 * construction — an open menu is one popup; repeated clicks toggle).
 * (ax, ay, aw, ah) is the Start button's rect in bar coordinates. */
void pm_menu_toggle(struct panel *p, int ax, int ay, int aw, int ah);

/* launch one application directly (config launchers) */
void pm_menu_launch_app(struct panel *p, const struct xwapp *app);
/* is the menu popup currently open? (the start fallback decision) */
bool pm_menu_open(void);

/* tear the menu down (compositor or panel shutdown; before the
 * connection dies) */
void pm_menu_shutdown(struct panel *p);

#endif /* PANEL_MENU_H */
