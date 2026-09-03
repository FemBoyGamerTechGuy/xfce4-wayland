/* panel-taskbar.c — the window-buttons overflow popup (see
 * panel-taskbar.h). One xdg_popup, the menu's lifecycle pattern:
 * configure draws + grabs, Escape/outside dismissal, click activates
 * the window, idempotent toggle with the same-click guard. */
#include "panel-taskbar.h"

#include <stdio.h>
#include <string.h>

#include "wlr-layer-shell-unstable-v1.h"
#include <xkbcommon/xkbcommon-keysyms.h>

#define TASKOVER_W 240
#define TASKOVER_ROW_EXTRA 10

#define COL_TO_BG 0xff262b33
#define COL_TO_BORDER 0xff3c4454
#define COL_TO_HOVER 0xff3584e4
#define COL_TO_SEP 0xff384050
#define COL_TO_TEXT 0xffe6e6e6
#define COL_TO_DIM 0xff9aa5b1

static struct {
    struct xwc_popup *popup; /* NULL = closed */
    struct panel *p;
    int from;      /* first hidden task index */
    int n_tasks;   /* tasks listed */
    int hover;     /* row index, -1 = none */
    int row_h;
    int w, h;
    int64_t closed_ms;
} g_to;

static struct xwc_task *task_at_row(int row) {
    if (!g_to.p || !g_to.p->tl || row < 0)
        return NULL;
    struct xwc_task *t = xwc_tasklist_first(g_to.p->tl);
    for (int i = 0; t && i < g_to.from + row; i++)
        t = xwc_task_next(t);
    return t;
}

static void to_draw(void) {
    struct panel *p = g_to.p;
    if (!p || !g_to.popup)
        return;
    int stride = 0;
    uint32_t *pix = xwc_popup_pixels(g_to.popup, &stride);
    if (!pix || stride < 1)
        return;
    int w = stride, h = g_to.h;
    xwc_draw_box(pix, stride, w, h, 0, 0, w, h, COL_TO_BG, COL_TO_BORDER);
    int icon = p->m.icon;
    int ty_off = (g_to.row_h - p->m.font_h) / 2 + 1;
    for (int i = 0; i < g_to.n_tasks; i++) {
        struct xwc_task *t = task_at_row(i);
        if (!t)
            break;
        int iy = 2 + i * g_to.row_h;
        if (i == g_to.hover)
            xwc_fill_rect(pix, stride, w, h, 1, iy, w - 2, g_to.row_h,
                          COL_TO_HOVER);
        else if (i > 0)
            xwc_draw_hline(pix, stride, w, h, 8, iy, w - 16, COL_TO_SEP);
        /* icon: the app's themed icon with the letter fallback */
        const char *app_id = xwc_task_app_id(t);
        const struct xwapp *app =
            app_id && *app_id ? xwapp_by_id(&p->apps, app_id) : NULL;
        const char *in = app ? app->icon : app_id;
        const struct xwc_icon *ic =
            (in && *in) ? xwc_icon_get(in, icon) : NULL;
        if (ic)
            xwc_draw_icon(pix, stride, w, h, 6, iy + (g_to.row_h - icon) / 2,
                          ic, icon);
        char label[64];
        const char *title = xwc_task_title(t);
        if (!*title)
            title = app_id;
        snprintf(label, sizeof(label), "%.60s", *title ? title : "(window)");
        for (int used = panel_text_width(p, label);
             used > TASKOVER_W - 16 && strlen(label) > 1;
             used = panel_text_width(p, label))
            label[strlen(label) - 1] = 0;
        panel_draw_text(p, pix, stride, w, h, 6 + icon + 8, iy + ty_off, label,
                        i == g_to.hover ? 0xffffffff : COL_TO_TEXT);
    }
    xwc_popup_commit(g_to.popup);
}

static void to_close(struct panel *p, const char *why) {
    (void)p;
    if (!g_to.popup)
        return;
    panel_trace("overflow list closing (%s)", why);
    xwc_popup_destroy(g_to.popup);
    g_to.popup = NULL;
    g_to.hover = -1;
    g_to.closed_ms = panel_mono_ms();
}

static void to_on_configure(struct xwc_win *win, int w, int h, void *ud) {
    (void)ud;
    struct xwc_popup *popup = (struct xwc_popup *)win;
    if (popup)
        g_to.popup = popup;
    g_to.w = w > 0 ? w : TASKOVER_W;
    g_to.h = h > 0 ? h : 2 + g_to.n_tasks * g_to.row_h;
    panel_trace("overflow surface created: %dx%d, tasks=%d", g_to.w, g_to.h,
                g_to.n_tasks);
    to_draw();
    xwc_popup_grab(g_to.popup);
}

static void to_on_done(struct xwc_win *win, void *ud) {
    struct panel *p = ud;
    (void)win;
    to_close(p, "dismissed (outside press or compositor)");
}

static int to_row_at(int x, int y) {
    if (x < 0 || x > g_to.w || y < 2)
        return -1;
    int row = (y - 2) / g_to.row_h;
    if (row >= g_to.n_tasks || y >= 2 + row * g_to.row_h + g_to.row_h)
        return -1;
    return row;
}

#define BTN_LEFT 0x110

static void to_on_button(struct xwc_win *win, uint32_t button, bool down,
                         int x, int y, void *ud) {
    struct panel *p = ud;
    (void)win;
    if (!down || button != BTN_LEFT)
        return;
    int row = to_row_at(x, y);
    struct xwc_task *t = task_at_row(row);
    panel_trace("overflow click at %d,%d -> row=%d task=%s", x, y, row,
                t ? xwc_task_title(t) : "(none)");
    if (t) {
        xwc_tasklist_activate(p->tl, t);
        to_close(p, "window selected");
    }
}

static void to_on_motion(struct xwc_win *win, int x, int y, void *ud) {
    (void)win;
    (void)ud;
    int row = to_row_at(x, y);
    if (row != g_to.hover) {
        g_to.hover = row;
        if (g_to.popup)
            to_draw();
    }
}

static void to_on_key(struct xwc_win *win, uint32_t keycode, bool down,
                      xkb_keysym_t sym, uint32_t mods, void *ud) {
    struct panel *p = ud;
    (void)win;
    (void)keycode;
    (void)mods;
    if (!down)
        return;
    if (sym == XKB_KEY_Escape) {
        to_close(p, "escape key");
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        struct xwc_task *t = task_at_row(g_to.hover);
        if (t) {
            xwc_tasklist_activate(p->tl, t);
            to_close(p, "window selected");
        }
    }
}

void pm_taskover_toggle(struct panel *p, int ax, int ay, int aw, int ah,
                        int hidden_from) {
    if (g_to.popup) {
        to_close(p, "toggle");
        return;
    }
    if (g_to.closed_ms && panel_mono_ms() - g_to.closed_ms < 250) {
        panel_trace("overflow press follows the dismissal of the same click "
                    "— reopen suppressed");
        return;
    }
    /* count the hidden tasks */
    int total = 0;
    for (struct xwc_task *t = p->tl ? xwc_tasklist_first(p->tl) : NULL; t;
         t = xwc_task_next(t))
        total++;
    g_to.n_tasks = total - hidden_from;
    if (g_to.n_tasks <= 0)
        return;
    g_to.p = p;
    g_to.from = hidden_from;
    g_to.row_h = p->m.font_h + TASKOVER_ROW_EXTRA;
    g_to.hover = -1;
    int h = 2 + g_to.n_tasks * g_to.row_h;
    panel_trace("overflow button clicked — listing %d hidden windows",
                g_to.n_tasks);

    struct xwc_callbacks cb = {
        .button = to_on_button,
        .motion = to_on_motion,
        .configure = to_on_configure,
        .close = to_on_done,
        .key = to_on_key,
        .ud = p,
    };
    g_to.popup = xwc_popup_create_dir(&p->c, p->layer, ax, ay, aw, ah,
                                      TASKOVER_W, h, &cb, p->cfg.bottom);
    if (!g_to.popup)
        fprintf(stderr, "xw-panel: overflow popup creation failed\n");
}

void pm_taskover_shutdown(struct panel *p) {
    to_close(p, "compositor gone");
}
