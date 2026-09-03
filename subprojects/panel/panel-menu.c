/* panel-menu.c — the applications menu (v1: the flat list under the
 * Start button; the two-pane category browser is the next stage).
 *
 * One xdg_popup parented to the bar layer via the layer-shell's
 * get_popup; the popup takes the seat grab (keyboard + outside-press
 * dismissal) after the first commit. Idempotent by construction: an
 * open menu is exactly one popup, the Start button toggles it, and
 * a press that lands through the dismissal path cannot reopen it
 * (the close timestamp suppresses the same-click reopen). */
#include "panel-menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wlr-layer-shell-unstable-v1.h"
#include <xkbcommon/xkbcommon-keysyms.h>

/* v1 geometry: flat list, capped rows (the v2 two-pane menu scrolls) */
#define MENU_W 300
#define MENU_ITEM_H_EXTRA 10
#define MENU_MAX_ROWS 26

static struct {
    struct xwc_popup *popup; /* NULL = closed */
    struct panel *p;
    int hover;              /* hovered item index, -1 = none */
    int w, h;               /* popup geometry */
    int rows;               /* visible row count */
    int64_t closed_ms;      /* dismissal timestamp (toggle guard) */
} g_menu;

#define COL_MENU_BG 0xff262b33
#define COL_MENU_BORDER 0xff3c4454
#define COL_MENU_HOVER 0xff3584e4
#define COL_MENU_SEP 0xff384050
#define COL_MENU_TEXT 0xffe6e6e6

static int menu_row_h(const struct panel *p) {
    return p->m.font_h + MENU_ITEM_H_EXTRA;
}

static void menu_draw(void) {
    struct panel *p = g_menu.p;
    if (!p || !g_menu.popup)
        return;
    int stride = 0;
    uint32_t *pix = xwc_popup_pixels(g_menu.popup, &stride);
    if (!pix || stride < 1)
        return;
    int w = stride, h = g_menu.h;
    xwc_draw_box(pix, stride, w, h, 0, 0, w, h, COL_MENU_BG, COL_MENU_BORDER);

    int icon = p->m.icon;
    int row_h = menu_row_h(p);
    int ty_off = (row_h - p->m.font_h) / 2 + 1;
    for (int i = 0; i < g_menu.rows; i++) {
        const struct xwapp *a = xwapp_at(&p->apps, i);
        if (!a)
            break;
        int iy = 2 + i * row_h;
        if (i == g_menu.hover)
            xwc_fill_rect(pix, stride, w, h, 1, iy, w - 2, row_h,
                          COL_MENU_HOVER);
        else if (i > 0)
            xwc_draw_hline(pix, stride, w, h, 8, iy, w - 16, COL_MENU_SEP);
        /* icon */
        if (p->cfg.menu_icons) {
            const struct xwc_icon *ic =
                a->icon[0] ? xwc_icon_get(a->icon, icon) : NULL;
            if (ic)
                xwc_draw_icon(pix, stride, w, h, 6, iy + (row_h - icon) / 2,
                              ic, icon);
        }
        /* label, truncated to the menu width */
        char label[XWAPP_NAME_MAX];
        snprintf(label, sizeof(label), "%.120s", a->name);
        for (int used = panel_text_width(p, label);
             used > MENU_W - 20 && strlen(label) > 1;
             used = panel_text_width(p, label))
            label[strlen(label) - 1] = 0;
        int tx = 6 + icon + 8;
        panel_draw_text(p, pix, stride, w, h, tx, iy + ty_off, label,
                        i == g_menu.hover ? 0xffffffff : COL_MENU_TEXT);
    }
    xwc_popup_commit(g_menu.popup);
}

static void menu_launch_idx(int idx) {
    struct panel *p = g_menu.p;
    const struct xwapp *a = xwapp_at(&p->apps, idx);
    if (!a)
        return;
    pm_menu_launch_app(p, a);
}

void pm_menu_launch_app(struct panel *p, const struct xwapp *app) {
    char args[XWAPP_MAX_ARGS][XWAPP_ARG_MAX];
    char err[192];
    int n = xwapp_launch_argv(app, args, XWAPP_MAX_ARGS, err, sizeof(err));
    if (n < 1) {
        fprintf(stderr, "xw-panel: cannot launch '%s': %s\n", app->name,
                err[0] ? err : "unusable entry");
        return;
    }
    char line[1024];
    if (!xwapp_argv_to_shell((const char(*)[XWAPP_ARG_MAX])args, n, line,
                             sizeof(line))) {
        fprintf(stderr, "xw-panel: cannot launch '%s': command line too "
                        "long\n",
                app->name);
        return;
    }
    char cmd[1100];
    snprintf(cmd, sizeof(cmd), "run %.1020s", line);
    fprintf(stderr, "xw-panel: launching '%s' (ctl \"%.80s...\")\n", app->name,
            line);
    panel_ctl_send(cmd);
    pm_menu_shutdown(p);
}

static void menu_close(struct panel *p, const char *why) {
    (void)p;
    if (!g_menu.popup)
        return;
    panel_trace("menu closing (%s) — menu surface destroyed", why);
    xwc_popup_destroy(g_menu.popup);
    g_menu.popup = NULL;
    g_menu.hover = -1;
    g_menu.rows = 0;
    g_menu.closed_ms = panel_mono_ms();
}

static void menu_on_configure(struct xwc_win *win, int w, int h, void *ud) {
    struct panel *p = ud;
    /* the configure fires DURING xwc_popup_create (before the caller
     * stored the pointer): the popup arrives as the callback's own
     * first argument (the exact bug that kept the first menu
     * invisible in the v0 round) */
    struct xwc_popup *popup = (struct xwc_popup *)win;
    if (popup)
        g_menu.popup = popup;
    g_menu.w = w > 0 ? w : MENU_W;
    g_menu.h = h > 0 ? h : 2 + g_menu.rows * menu_row_h(p);
    panel_trace("menu surface created: %dx%d, rows=%d", g_menu.w, g_menu.h,
                g_menu.rows);
    menu_draw();
    /* the grab (keyboard + outside-press dismissal) follows the
     * mapping commit — the ordering the server requires */
    xwc_popup_grab(g_menu.popup);
}

static void menu_on_done(struct xwc_win *win, void *ud) {
    struct panel *p = ud;
    (void)win;
    /* popup_done: the compositor dismissed us (press outside). The
     * close timestamp suppresses an instant reopen from the same
     * click; destroying the proxy inside its own event handler is
     * the established safe pattern. */
    menu_close(p, "dismissed (outside press or compositor)");
}

static int menu_item_at(struct panel *p, int x, int y) {
    if (x < 0 || x > g_menu.w || y < 2)
        return -1;
    int row_h = menu_row_h(p);
    int idx = (y - 2) / row_h;
    if (idx >= g_menu.rows || y >= 2 + idx * row_h + row_h)
        return -1;
    return idx;
}

#define BTN_LEFT 0x110

static void menu_on_button(struct xwc_win *win, uint32_t button, bool down,
                           int x, int y, void *ud) {
    struct panel *p = ud;
    (void)win;
    int idx = menu_item_at(p, x, y);
    panel_trace("menu button %u %s at %d,%d -> item=%d", button,
                down ? "press" : "release", x, y, idx);
    if (down && button == BTN_LEFT && idx >= 0)
        menu_launch_idx(idx);
}

static void menu_on_motion(struct xwc_win *win, int x, int y, void *ud) {
    struct panel *p = ud;
    (void)win;
    int idx = menu_item_at(p, x, y);
    if (idx != g_menu.hover) {
        g_menu.hover = idx;
        if (g_menu.popup)
            menu_draw();
    }
}

static void menu_on_key(struct xwc_win *win, uint32_t keycode, bool down,
                        xkb_keysym_t sym, uint32_t mods, void *ud) {
    struct panel *p = ud;
    (void)win;
    (void)keycode;
    (void)mods;
    if (!down)
        return;
    if (sym == XKB_KEY_Escape) {
        panel_trace("menu key Escape -> close");
        menu_close(p, "escape key");
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        panel_trace("menu key Enter -> launch hovered item");
        menu_launch_idx(g_menu.hover);
    }
}

void pm_menu_toggle(struct panel *p, int ax, int ay, int aw, int ah) {
    if (g_menu.popup) {
        panel_trace("start clicked — menu already open; toggling closed");
        menu_close(p, "start toggle");
        return;
    }
    /* rescan: applications may have been installed/removed since */
    xwapp_db_scan(&p->apps);
    int total = xwapp_db_count(&p->apps);
    if (total == 0) {
        fprintf(stderr,
                "xw-panel: start clicked — menu opening aborted: no "
                "applications found ($XDG_DATA_DIRS/applications, "
                "~/.local/share/applications)\n");
        return;
    }
    g_menu.p = p;
    g_menu.rows = total < MENU_MAX_ROWS ? total : MENU_MAX_ROWS;
    int h = 2 + g_menu.rows * menu_row_h(p);
    fprintf(stderr,
            "xw-panel: start clicked — menu opening (rows=%d of %d, %dx%d "
            "popup under the button)\n",
            g_menu.rows, total, MENU_W, h);

    struct xwc_callbacks cb = {
        .button = menu_on_button,
        .motion = menu_on_motion,
        .configure = menu_on_configure,
        .close = menu_on_done,
        .key = menu_on_key,
        .ud = p,
    };
    /* anchor rect = the Start button spanning the full bar height, in
     * bar-surface coordinates; bottom bars open the popup upward */
    g_menu.popup = xwc_popup_create_dir(&p->c, p->layer, ax, ay, aw, ah,
                                        MENU_W, h, &cb, p->cfg.bottom);
    g_menu.hover = -1;
    if (!g_menu.popup)
        fprintf(stderr, "xw-panel: menu popup creation failed\n");
}

void pm_menu_shutdown(struct panel *p) {
    menu_close(p, "compositor gone");
}

bool pm_menu_open(void) { return g_menu.popup != NULL; }
