/* panel-taskbar.h — the window-buttons overflow popup.
 *
 * When the middle region is too narrow for every window button, the
 * bar squeezes to icon-only and finally hides buttons behind a "+N"
 * indicator; clicking that indicator opens this list popup with the
 * hidden windows (icon + title, click to focus). */
#ifndef PANEL_TASKBAR_H
#define PANEL_TASKBAR_H

#include "panel.h"

/* open/toggle the overflow list for the tasks hidden by the layout.
 * (ax, ay, aw, ah) is the overflow button's rect in bar coordinates;
 * hidden_from is the index of the first hidden task in the tasklist. */
void pm_taskover_toggle(struct panel *p, int ax, int ay, int aw, int ah,
                        int hidden_from);

/* tear the overflow popup down (compositor/panel shutdown) */
void pm_taskover_shutdown(struct panel *p);

#endif /* PANEL_TASKBAR_H */
