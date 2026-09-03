/* panel-pager.h — the graphical workspace pager.
 *
 * Each workspace renders as a miniature of the desktop: a bordered
 * box whose interior carries small window tiles (2 per row, 2 rows,
 * the focused window's tile accented, sticky windows dimmed on every
 * box). The active workspace is visually obvious (bright border and
 * background); clicking a box switches workspaces through
 * ext-workspace activate. */
#ifndef PANEL_PAGER_H
#define PANEL_PAGER_H

#include "panel.h"

/* draw one workspace box at (x, y, w, h) in the bar buffer. ws_idx is
 * the workspace index the box represents; tasks come from the
 * tasklist (xw-workspace-info annotations). */
void pm_pager_draw_box(struct panel *p, uint32_t *pix, int stride, int bw,
                       int bh, int x, int y, int w, int h, int ws_idx,
                       bool active, bool hover);

#endif /* PANEL_PAGER_H */
