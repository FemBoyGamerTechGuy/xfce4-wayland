/* panel-menu.c — the applications menu, v2: a two-pane browser in one
 * popup (the Whisker-menu shape — the XFCE-native way to present the
 * XDG application database):
 *
 *   +---------------------------------------------+
 *   | [magnifier] type to search...               |
 *   +--------------+------------------------------+
 *   | Favorites    |  [icon] App name             |
 *   | All          |  [icon] App name             |
 *   | Accessories  |  ...                         |
 *   | Internet     |  (wheel / arrow keys scroll) |
 *   | ...          |                              |
 *   +--------------+------------------------------+
 *
 * Left pane: categories (favorites from the config, All, then the
 * non-empty XFCE-style groups). Right pane: the selected category's
 * applications with icons. Typing filters (name, generic name,
 * comment, id); Escape clears the filter or closes the menu; Enter
 * launches the hovered row; the wheel scrolls long lists.
 *
 * One popup, one seat grab, idempotent by construction: repeated
 * Start clicks toggle; a press that lands through the outside
 * dismissal cannot reopen (the close-timestamp guard). */
#include "panel-menu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wlr-layer-shell-unstable-v1.h"
#include <xkbcommon/xkbcommon-keysyms.h>

/* geometry (all values derive from the metrics; the popup is one
 * surface — panes are drawn regions of it) */
#define MENU_CAT_W 176
#define MENU_APP_W 336
#define MENU_W (2 + MENU_CAT_W + MENU_APP_W + 2)
#define MENU_PAD 2

#define COL_MENU_BG 0xff262b33
#define COL_MENU_PANE 0xff2b313b
#define COL_MENU_BORDER 0xff3c4454
#define COL_MENU_HOVER 0xff3584e4
#define COL_MENU_SEP 0xff384050
#define COL_MENU_SEL 0xff2f5a8f
#define COL_MENU_TEXT 0xffe6e6e6
#define COL_MENU_DIM 0xff9aa5b1

static struct {
    struct xwc_popup *popup; /* NULL = closed */
    struct panel *p;
    int w, h;
    int header_h; /* search row height */
    int row_h;

    /* category list actually shown (built at open) */
    int cats[XWAPP_CAT_COUNT];
    int n_cats;
    int sel_cat;    /* XWAPP_CAT_* currently listed in the app pane */
    int hover_cat;  /* row index in the left pane, -1 = none */
    int hover_app;  /* row index in the app pane, -1 = none */

    /* the app pane's model: indices into db->apps (category listing
     * or search hits) */
    int apps[XWAPP_MAX];
    int n_apps;
    int scroll; /* first visible row */

    char search[72];
    int64_t closed_ms; /* dismissal timestamp (toggle guard) */
} g_menu;

static int row_h_of(const struct panel *p) {
    return p->m.font_h + 10;
}

/* build the visible category list: Favorites (when non-empty), All,
 * then every non-empty group in display order */
static void rebuild_cats(void) {
    struct panel *p = g_menu.p;
    g_menu.n_cats = 0;
    if (xwapp_cat_count(&p->apps, XWAPP_CAT_FAVORITES) > 0)
        g_menu.cats[g_menu.n_cats++] = XWAPP_CAT_FAVORITES;
    g_menu.cats[g_menu.n_cats++] = XWAPP_CAT_ALL;
    static const int order[] = {
        XWAPP_CAT_ACCESSORIES,  XWAPP_CAT_DEVELOPMENT,
        XWAPP_CAT_EDUCATION,    XWAPP_CAT_GAMES,
        XWAPP_CAT_GRAPHICS,     XWAPP_CAT_MULTIMEDIA,
        XWAPP_CAT_NETWORK,      XWAPP_CAT_OFFICE,
        XWAPP_CAT_SCIENCE,      XWAPP_CAT_SETTINGS,
        XWAPP_CAT_SYSTEM,       XWAPP_CAT_TERMINAL,
        XWAPP_CAT_OTHER,
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (xwapp_cat_count(&p->apps, order[i]) > 0)
            g_menu.cats[g_menu.n_cats++] = order[i];
}

/* rebuild the app pane's index list: the selected category, or the
 * search hits when a filter is typed */
static void rebuild_apps(void) {
    struct panel *p = g_menu.p;
    g_menu.n_apps = 0;
    g_menu.scroll = 0;
    g_menu.hover_app = -1;
    if (g_menu.search[0]) {
        g_menu.n_apps = xwapp_search(&p->apps, g_menu.search, g_menu.apps,
                                     XWAPP_MAX);
        return;
    }
    int cat = g_menu.sel_cat;
    int n = xwapp_cat_count(&p->apps, cat);
    for (int i = 0; i < n && g_menu.n_apps < XWAPP_MAX; i++) {
        const struct xwapp *a = xwapp_cat_at(&p->apps, cat, i);
        if (!a)
            continue;
        /* index into apps[]: find via pointer arithmetic (the category
         * API yields pointers; the search API yields indices — both
         * address the same sorted array) */
        g_menu.apps[g_menu.n_apps++] = (int)(a - p->apps.apps);
    }
}

static void select_cat(int cat) {
    g_menu.sel_cat = cat;
    rebuild_apps();
}

static int visible_rows(void) {
    int pane_h = g_menu.h - MENU_PAD - g_menu.header_h - MENU_PAD;
    int rows = pane_h / g_menu.row_h;
    return rows > 0 ? rows : 1;
}

/* clamp the scroll so the selection and content stay visible */
static void clamp_scroll(void) {
    int vis = visible_rows();
    if (g_menu.n_apps <= vis) {
        g_menu.scroll = 0;
        return;
    }
    if (g_menu.scroll > g_menu.n_apps - vis)
        g_menu.scroll = g_menu.n_apps - vis;
    if (g_menu.scroll < 0)
        g_menu.scroll = 0;
}

/* ---------------------------------------------------------------- draw */

/* procedural magnifier icon for the search box */
static void draw_magnifier(uint32_t *pix, int stride, int w, int h, int x,
                           int y, int cell) {
    int r = cell / 3;
    int cx = x + cell / 2 - r / 2, cy = y + cell / 2 - r / 2;
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r && i * i + j * j > (r - 2) * (r - 2))
                xwc_fill_rect(pix, stride, w, h, cx + i, cy + j, 1, 1,
                              COL_MENU_DIM);
    /* handle */
    for (int k = 0; k <= r; k++)
        xwc_fill_rect(pix, stride, w, h, cx + r - k / 2 + k, cy + r - k / 2 + k,
                      2, 2, COL_MENU_DIM);
}

static void draw_header(struct panel *p, uint32_t *pix, int stride, int bw,
                        int bh) {
    int y = MENU_PAD, h = g_menu.header_h;
    xwc_draw_box(pix, stride, bw, bh, MENU_PAD, y, g_menu.w - 2 * MENU_PAD, h,
                 COL_MENU_PANE, COL_MENU_BORDER);
    int icon = p->m.icon;
    if (icon > h - 6)
        icon = h - 6;
    draw_magnifier(pix, stride, bw, bh, MENU_PAD + 6, y + (h - icon) / 2,
                   icon);
    int tx = MENU_PAD + 6 + icon + 8;
    const char *txt = g_menu.search[0] ? g_menu.search : "type to search...";
    uint32_t col = g_menu.search[0] ? COL_MENU_TEXT : COL_MENU_DIM;
    /* the search text + caret */
    char withcaret[80];
    snprintf(withcaret, sizeof(withcaret), "%.70s_", txt);
    panel_draw_text(p, pix, stride, bw, bh, tx,
                    y + (h - p->m.font_h) / 2 + 1, withcaret, col);
}

static void draw_cat_row(struct panel *p, uint32_t *pix, int stride, int bw,
                         int bh, int row) {
    int y = MENU_PAD + g_menu.header_h + row * g_menu.row_h;
    int cat = g_menu.cats[row];
    int selected = (cat == g_menu.sel_cat && !g_menu.search[0]);
    if (row == g_menu.hover_cat)
        xwc_fill_rect(pix, stride, bw, bh, MENU_PAD, y, MENU_CAT_W,
                      g_menu.row_h, COL_MENU_HOVER);
    else if (selected)
        xwc_fill_rect(pix, stride, bw, bh, MENU_PAD, y, MENU_CAT_W,
                      g_menu.row_h, COL_MENU_SEL);
    int icon = p->m.icon > g_menu.row_h - 6 ? g_menu.row_h - 6 : p->m.icon;
    const struct xwc_icon *ic = xwc_icon_get(xwapp_cat_icon(cat), icon);
    if (ic)
        xwc_draw_icon(pix, stride, bw, bh, MENU_PAD + 6, y + (g_menu.row_h - icon) / 2,
                      ic, icon);
    else {
        /* procedural: a small filled roundrect keyed by the category */
        uint32_t tints[] = {0xffe6b450, 0xff8fa3b8, 0xff7d9c5a, 0xffb8576d,
                            0xff6d85b8, 0xffc98f4a, 0xff5ab89a, 0xff8f6db8,
                            0xff4aa5c9, 0xffb86d4a, 0xff9aa5b1, 0xff5a8fb8,
                            0xffc9b04a, 0xff7d7d7d};
        xwc_draw_box(pix, stride, bw, bh, MENU_PAD + 6,
                     y + (g_menu.row_h - icon) / 2, icon, icon,
                     tints[cat % 14], tints[cat % 14]);
    }
    char label[XWAPP_NAME_MAX];
    panel_text_fit(p, label, sizeof(label), xwapp_cat_name(cat),
                   MENU_CAT_W - 16);
    panel_draw_text(p, pix, stride, bw, bh, MENU_PAD + 6 + icon + 8,
                    y + (g_menu.row_h - p->m.font_h) / 2 + 1, label,
                    COL_MENU_TEXT);
}

static void draw_app_row(struct panel *p, uint32_t *pix, int stride, int bw,
                         int bh, int vis_row) {
    int idx = g_menu.scroll + vis_row;
    if (idx >= g_menu.n_apps)
        return;
    const struct xwapp *a = xwapp_at(&p->apps, g_menu.apps[idx]);
    if (!a)
        return;
    int x = 2 + MENU_CAT_W;
    int y = MENU_PAD + g_menu.header_h + vis_row * g_menu.row_h;
    if (idx == g_menu.hover_app)
        xwc_fill_rect(pix, stride, bw, bh, x, y, MENU_APP_W, g_menu.row_h,
                      COL_MENU_HOVER);
    else if (vis_row > 0)
        xwc_draw_hline(pix, stride, bw, bh, x + 8, y, MENU_APP_W - 16,
                       COL_MENU_SEP);
    int icon = p->m.icon;
    if (icon > g_menu.row_h - 6)
        icon = g_menu.row_h - 6;
    if (p->cfg.menu_icons) {
        const struct xwc_icon *ic =
            a->icon[0] ? xwc_icon_get(a->icon, icon) : NULL;
        if (ic)
            xwc_draw_icon(pix, stride, bw, bh, x + 8, y + (g_menu.row_h - icon) / 2,
                          ic, icon);
    }
    char label[XWAPP_NAME_MAX];
    panel_text_fit(p, label, sizeof(label), a->name, MENU_APP_W - 20);
    panel_draw_text(p, pix, stride, bw, bh, x + 8 + icon + 8,
                    y + (g_menu.row_h - p->m.font_h) / 2 + 1, label,
                    COL_MENU_TEXT);
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

    draw_header(p, pix, stride, w, h);

    /* pane backgrounds */
    int pane_y = MENU_PAD + g_menu.header_h;
    int pane_h = h - pane_y - MENU_PAD;
    xwc_fill_rect(pix, stride, w, h, MENU_PAD, pane_y, MENU_CAT_W, pane_h,
                  COL_MENU_PANE);
    xwc_fill_rect(pix, stride, w, h, 2 + MENU_CAT_W, pane_y, MENU_APP_W,
                  pane_h, COL_MENU_PANE);
    /* separator between the panes */
    xwc_draw_vline(pix, stride, w, h, 2 + MENU_CAT_W, pane_y, pane_h,
                   COL_MENU_SEP);

    for (int i = 0; i < g_menu.n_cats && i < visible_rows(); i++)
        draw_cat_row(p, pix, stride, w, h, i);
    for (int i = 0; i < visible_rows(); i++)
        draw_app_row(p, pix, stride, w, h, i);

    /* scrollbar when the app list overflows */
    int vis = visible_rows();
    if (g_menu.n_apps > vis) {
        int track_y = pane_y + 1, track_h = pane_h - 2;
        int thumb_h = track_h * vis / g_menu.n_apps;
        if (thumb_h < 12)
            thumb_h = 12;
        int thumb_y =
            track_y + (track_h - thumb_h) * g_menu.scroll /
                          (g_menu.n_apps - vis);
        xwc_fill_rect(pix, stride, w, h, 2 + MENU_CAT_W + MENU_APP_W - 5,
                      thumb_y, 3, thumb_h, COL_MENU_DIM);
    }
    /* search-active hint: the category pane dims while filtering */
    if (g_menu.search[0]) {
        for (int j = pane_y; j < pane_y + pane_h; j++)
            for (int i = MENU_PAD; i < MENU_PAD + MENU_CAT_W; i++)
                pix[j * stride + i] =
                    (pix[j * stride + i] & 0xfefefefe) >> 1 | 0xff000000u;
    }

    xwc_popup_commit(g_menu.popup);
}

/* ------------------------------------------------------------- actions */

/* Launch the selected application. Everything the launch needs is
 * copied first (panel_launch_app), the process is spawned directly
 * (posix_spawn, no shell, no ctl relay), and only then is the menu
 * closed — the callback's data is never freed under it. A refused
 * launch (malformed Exec, missing executable) keeps the menu open and
 * reports visibly on the bar. */
void pm_menu_launch_app(struct panel *p, const struct xwapp *app) {
    bool ok = panel_launch_app(p, app);
    if (ok)
        pm_menu_shutdown(p);
}

static void menu_close(struct panel *p, const char *why) {
    (void)p;
    if (!g_menu.popup)
        return;
    panel_trace("menu closing (%s) — menu surface destroyed", why);
    xwc_popup_destroy(g_menu.popup);
    g_menu.popup = NULL;
    g_menu.search[0] = 0;
    g_menu.n_apps = 0;
    g_menu.closed_ms = panel_mono_ms();
}

/* ---------------------------------------------------------- menu events */

static void menu_on_configure(struct xwc_win *win, int w, int h, void *ud) {
    (void)ud;
    /* the configure fires DURING xwc_popup_create (before the caller
     * stored the pointer): the popup arrives as the callback's own
     * first argument (the exact bug that kept the first menu
     * invisible in the v0 round) */
    struct xwc_popup *popup = (struct xwc_popup *)win;
    if (popup)
        g_menu.popup = popup;
    g_menu.w = w > 0 ? w : MENU_W;
    g_menu.h = h > 0 ? h : 2 * MENU_PAD + g_menu.header_h +
                                visible_rows() * g_menu.row_h;
    panel_trace("menu surface created: %dx%d, cats=%d apps=%d", g_menu.w,
                g_menu.h, g_menu.n_cats, g_menu.n_apps);
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

static int app_row_at(int x, int y) {
    int pane_y = MENU_PAD + g_menu.header_h;
    if (x < 2 + MENU_CAT_W || x >= 2 + MENU_CAT_W + MENU_APP_W ||
        y < pane_y)
        return -1;
    int row = (y - pane_y) / g_menu.row_h;
    if (row >= visible_rows() || y >= pane_y + row * g_menu.row_h + g_menu.row_h)
        return -1;
    if (g_menu.scroll + row >= g_menu.n_apps)
        return -1;
    return g_menu.scroll + row;
}

static int cat_row_at(int x, int y) {
    int pane_y = MENU_PAD + g_menu.header_h;
    if (x < MENU_PAD || x >= MENU_PAD + MENU_CAT_W || y < pane_y)
        return -1;
    int row = (y - pane_y) / g_menu.row_h;
    if (row >= g_menu.n_cats || row >= visible_rows() ||
        y >= pane_y + row * g_menu.row_h + g_menu.row_h)
        return -1;
    return row;
}

#define BTN_LEFT 0x110

static void menu_on_button(struct xwc_win *win, uint32_t button, bool down,
                           int x, int y, void *ud) {
    struct panel *p = ud;
    (void)win;
    if (!down || button != BTN_LEFT)
        return;
    int cat = cat_row_at(x, y);
    if (cat >= 0 && !g_menu.search[0]) {
        panel_trace("menu category %s selected", xwapp_cat_name(g_menu.cats[cat]));
        select_cat(g_menu.cats[cat]);
        g_menu.hover_cat = cat;
        menu_draw();
        return;
    }
    int app = app_row_at(x, y);
    panel_trace("menu click at %d,%d -> category row=%d app row=%d", x, y,
                cat, app);
    if (app >= 0) {
        const struct xwapp *a = xwapp_at(&p->apps, g_menu.apps[app]);
        if (a)
            pm_menu_launch_app(p, a);
    }
}

static void menu_on_motion(struct xwc_win *win, int x, int y, void *ud) {
    (void)win;
    (void)ud;
    int cat = cat_row_at(x, y);
    int app = app_row_at(x, y);
    /* while filtering, the category pane is inert */
    if (g_menu.search[0])
        cat = -1;
    if (cat != g_menu.hover_cat || app != g_menu.hover_app) {
        g_menu.hover_cat = cat;
        g_menu.hover_app = app;
        if (g_menu.popup)
            menu_draw();
    }
}

static void menu_on_axis(struct xwc_win *win, uint32_t axis, double value,
                         void *ud) {
    (void)win;
    (void)ud;
    if (axis != 0 || value == 0) /* WL_POINTER_AXIS_VERTICAL_SCROLL */
        return;
    int vis = visible_rows();
    if (g_menu.n_apps <= vis)
        return;
    int step = value > 0 ? 3 : -3;
    g_menu.scroll += step;
    clamp_scroll();
    /* keep the hover within the visible window */
    if (g_menu.hover_app >= 0) {
        if (g_menu.hover_app < g_menu.scroll)
            g_menu.hover_app = g_menu.scroll;
        if (g_menu.hover_app >= g_menu.scroll + vis)
            g_menu.hover_app = g_menu.scroll + vis - 1;
    }
    if (g_menu.popup)
        menu_draw();
}

static void search_apply(void) {
    rebuild_apps();
    g_menu.hover_cat = -1;
    g_menu.hover_app = g_menu.n_apps > 0 ? 0 : -1;
    clamp_scroll();
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
        if (g_menu.search[0]) {
            panel_trace("menu key Escape -> clear search");
            g_menu.search[0] = 0;
            search_apply();
            menu_draw();
        } else {
            panel_trace("menu key Escape -> close");
            menu_close(p, "escape key");
        }
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (g_menu.hover_app >= 0 && g_menu.hover_app < g_menu.n_apps) {
            const struct xwapp *a =
                xwapp_at(&p->apps, g_menu.apps[g_menu.hover_app]);
            if (a) {
                panel_trace("menu key Enter -> launch '%s'", a->name);
                pm_menu_launch_app(p, a);
            }
        }
        return;
    }
    if (sym == XKB_KEY_BackSpace) {
        size_t len = strlen(g_menu.search);
        if (len) {
            g_menu.search[len - 1] = 0;
            search_apply();
            menu_draw();
        }
        return;
    }
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
        int vis = visible_rows();
        if (g_menu.n_apps < 1)
            return;
        int dir = sym == XKB_KEY_Up ? -1 : 1;
        if (g_menu.hover_app < 0)
            g_menu.hover_app = g_menu.scroll;
        else
            g_menu.hover_app += dir;
        if (g_menu.hover_app < 0)
            g_menu.hover_app = 0;
        if (g_menu.hover_app >= g_menu.n_apps)
            g_menu.hover_app = g_menu.n_apps - 1;
        /* scroll to keep the hover visible */
        if (g_menu.hover_app < g_menu.scroll)
            g_menu.scroll = g_menu.hover_app;
        if (g_menu.hover_app >= g_menu.scroll + vis)
            g_menu.scroll = g_menu.hover_app - vis + 1;
        clamp_scroll();
        menu_draw();
        return;
    }
    /* typing: printable ASCII joins the search buffer */
    if (sym >= 0x20 && sym < 0x7f) {
        size_t len = strlen(g_menu.search);
        if (len + 1 < sizeof(g_menu.search)) {
            g_menu.search[len] = (char)tolower((unsigned char)sym);
            g_menu.search[len + 1] = 0;
            search_apply();
            menu_draw();
        }
    }
}

/* ----------------------------------------------------------------- api */

void pm_menu_toggle(struct panel *p, int ax, int ay, int aw, int ah) {
    if (g_menu.popup) {
        panel_trace("start clicked — menu already open; toggling closed");
        menu_close(p, "start toggle");
        return;
    }
    /* rescan: applications may have been installed/removed since */
    xwapp_db_scan(&p->apps);
    if (xwapp_db_count(&p->apps) == 0) {
        fprintf(stderr,
                "xw-panel: start clicked — menu opening aborted: no "
                "applications found ($XDG_DATA_DIRS/applications, "
                "~/.local/share/applications)\n");
        return;
    }
    memset(&g_menu, 0, sizeof(g_menu));
    g_menu.p = p;
    g_menu.row_h = row_h_of(p);
    g_menu.header_h = g_menu.row_h;
    g_menu.hover_cat = -1;
    g_menu.hover_app = -1;

    rebuild_cats();
    select_cat(g_menu.cats[0]); /* default: All (or Favorites) */

    /* height: as many rows as fit in ~3/4 of the output, min 6 */
    int out_h = p->c.output_h > 0 ? p->c.output_h : 720;
    int rows = (out_h * 3 / 4 - 2 * MENU_PAD - g_menu.header_h) / g_menu.row_h;
    if (rows < 6)
        rows = 6;
    if (rows > g_menu.n_cats + 2)
        rows = g_menu.n_cats + 2; /* show every category, cap the pane */
    int h = 2 * MENU_PAD + g_menu.header_h + rows * g_menu.row_h;

    fprintf(stderr,
            "xw-panel: start clicked — menu opening (categories=%d, "
            "%d apps, popup %dx%d under the button)\n",
            g_menu.n_cats, g_menu.n_apps, MENU_W, h);

    struct xwc_callbacks cb = {
        .button = menu_on_button,
        .motion = menu_on_motion,
        .axis = menu_on_axis,
        .configure = menu_on_configure,
        .close = menu_on_done,
        .key = menu_on_key,
        .ud = p,
    };
    /* anchor rect = the Start button spanning the full bar height;
     * bottom bars open the popup upward */
    g_menu.popup = xwc_popup_create_dir(&p->c, p->layer, ax, ay, aw, ah,
                                        MENU_W, h, &cb, p->cfg.bottom);
    if (!g_menu.popup)
        fprintf(stderr, "xw-panel: menu popup creation failed\n");
}

void pm_menu_shutdown(struct panel *p) {
    menu_close(p, "compositor gone");
}

bool pm_menu_open(void) { return g_menu.popup != NULL; }
