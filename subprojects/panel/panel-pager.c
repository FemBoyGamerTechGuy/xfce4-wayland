/* panel-pager.c — the graphical workspace pager (see panel-pager.h).
 *
 * The boxes deliberately do NOT carry numbers anymore (the XFCE
 * pager's own representation at panel sizes is the miniature): a
 * workspace with windows shows window tiles, an empty workspace shows
 * an empty miniature, and the active workspace is lit. */
#include "panel-pager.h"

#include <string.h>

/* palette (shared with the bar) */
#define COL_BOX_BG 0xff2e3440
#define COL_BOX_HOVER 0xff3b4252
#define COL_BOX_ACTIVE_BG 0xff3a4a63
#define COL_BOX_BORDER 0xff3c4454
#define COL_BOX_ACTIVE_BORDER 0xff88b0ef
#define COL_BOX_SCREEN 0xff232830
#define COL_TILE 0xff55627a
#define COL_TILE_FOCUSED 0xff3584e4
#define COL_TILE_STICKY 0xff454f60

/* how many window tiles one box shows (2x2 grid) */
#define PAGER_MAX_TILES 4

void pm_pager_draw_box(struct panel *p, uint32_t *pix, int stride, int bw,
                       int bh, int x, int y, int w, int h, int ws_idx,
                       bool active, bool hover) {
    uint32_t fill = active ? COL_BOX_ACTIVE_BG
                           : (hover ? COL_BOX_HOVER : COL_BOX_BG);
    uint32_t border =
        active ? COL_BOX_ACTIVE_BORDER : COL_BOX_BORDER;
    xwc_draw_box(pix, stride, bw, bh, x, y, w, h, fill, border);

    /* the miniature screen: inset from the box frame */
    int sx = x + 4, sy = y + 3, sw = w - 8, sh = h - 6;
    if (sw < 6 || sh < 4)
        return; /* extremely small bar: the border alone speaks */
    xwc_fill_rect(pix, stride, bw, bh, sx, sy, sw, sh, COL_BOX_SCREEN);

    /* window tiles: tasks assigned to this workspace (sticky windows
     * appear on every workspace, dimmed) */
    if (!p->tl)
        return;
    struct {
        bool focused;
        bool sticky;
    } tiles[PAGER_MAX_TILES];
    int n = 0;
    for (struct xwc_task *t = xwc_tasklist_first(p->tl); t && n < PAGER_MAX_TILES;
         t = xwc_task_next(t)) {
        int ws = xwc_task_workspace(t);
        if (ws == ws_idx || ws == -1) {
            tiles[n].focused = xwc_task_active(t);
            tiles[n].sticky = ws == -1;
            n++;
        }
    }
    if (n == 0)
        return;

    /* tile grid: 1 column for one window, 2 columns beyond, wrapping
     * at two rows; the last tile absorbs the remainder */
    int cols = n <= 1 ? 1 : 2;
    int rows = (n + cols - 1) / cols;
    int gap = 1;
    int tile_w = (sw - gap * (cols - 1)) / cols;
    int tile_h = (sh - gap * (rows - 1)) / rows;
    for (int i = 0; i < n; i++) {
        int col = i % cols, row = i / cols;
        int tx = sx + col * (tile_w + gap);
        int ty = sy + row * (tile_h + gap);
        uint32_t tcol = tiles[i].sticky
                            ? COL_TILE_STICKY
                            : (tiles[i].focused ? COL_TILE_FOCUSED : COL_TILE);
        xwc_fill_rect(pix, stride, bw, bh, tx, ty, tile_w, tile_h, tcol);
        /* the focused tile gets a 1px inner border to pop */
        if (tiles[i].focused && tile_w > 4 && tile_h > 4) {
            xwc_fill_rect(pix, stride, bw, bh, tx, ty, tile_w, 1,
                          0xff88b0ef);
            xwc_fill_rect(pix, stride, bw, bh, tx, ty, 1, tile_h,
                          0xff88b0ef);
        }
    }
}
