/* xw-panel — the desktop panel (xfce4-panel parity, v1 layout engine).
 *
 * A layer-shell bar (top or bottom, exclusive zone = bar height) with
 * the XFCE region order:
 *
 *   [Applications] [launchers...]      [task buttons...]      [pager][clock][Exit]
 *   \_______ start region ______/     \__ flexible middle __/ \_ right region _/
 *
 * The layout is computed from metrics (panel_metrics_for): the bar
 * height derives from the output's LOGICAL size (auto mode) so it is
 * comfortable at 1080p/1440p/4K and stays proportional when the
 * compositor exposes a scale; text uses the build-time bitmap font
 * (16 px or the 24 px raster on tall bars), icons the XDG icon
 * pipeline. Everything renders through libxwcl; no toolkit.
 *
 * Modules: applications menu (panel-menu.c, popup under Start),
 * clock/calendar (panel-clock.c), graphical pager (panel-pager.c),
 * taskbar (panel-taskbar.c). This file is the core: metrics, regions,
 * input routing, the fallback widgets and the main loop.
 */
#include "panel.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>

#include "wlr-layer-shell-unstable-v1.h"
#include <xkbcommon/xkbcommon-keysyms.h>

/* ------------------------------------------------------------------ */
/* Terminal resolution for the no-applications fallback — the shared
 * implementation lives in the apps database (panel-apps.c). */

static void resolve_terminal(char *out, size_t n) {
    int style = 0;
    if (!xwapp_resolve_terminal(out, n, &style))
        snprintf(out, n, "%s", "x-terminal-emulator"); /* last resort */
}

/* ------------------------------------------------------------- look */

/* dark "xfce-ish" palette */
#define COL_BAR_BG 0xff22262e
#define COL_BTN_BG 0xff2e3440
#define COL_BTN_HOVER 0xff3b4252
#define COL_BTN_ACTIVE 0xff3584e4
#define COL_BTN_BORDER 0xff3c4454
#define COL_TEXT 0xffe6e6e6
#define COL_TEXT_DIM 0xff9aa5b1
#define COL_EXIT_BG 0xffa33434
#define COL_EXIT_HOVER 0xffc94b4b

/* -------------------------------------------------------------- state */

enum {
    BTN_NONE = 0,
    BTN_START,
    BTN_LAUNCHER,
    BTN_WS,
    BTN_TASK,
    BTN_TASKOVER,
    BTN_CLOCK,
    BTN_EXIT,
};

struct btn {
    int x, w;            /* bar-local geometry; y/h from the metrics */
    int kind;            /* BTN_* */
    void *data;          /* task ptr / ws index (as intptr) */
    bool hover;
    bool active;         /* active workspace / focused window */
    char label[48];      /* truncated text ("" = icon-only) */
};

#define MAX_BTNS 80

static struct panel *g_panel; /* signal handler context */

#define trace_log(...) panel_trace(__VA_ARGS__)

int panel_text_width(const struct panel *p, const char *s) {
    return p->m.big_font ? xwc_text_width2(s) : xwc_text_width(s);
}

int panel_draw_text(const struct panel *p, uint32_t *pix, int stride, int w,
                    int h, int x, int y, const char *s, uint32_t color) {
    return p->m.big_font
               ? xwc_draw_text2(pix, stride, w, h, x, y, s, color)
               : xwc_draw_text(pix, stride, w, h, x, y, s, color);
}

/* -------------------------------------------------------------- metrics */

/* -------------------------------------------------------------- layout */

/* widget list of the bar (the flat model keeps hit-testing trivial);
 * modules own their popups, this array is the bar strip itself */
static struct btn btns[MAX_BTNS];
static int n_btns;

#define btn_y(p) (3)
static int btn_h(const struct panel *p) { return p->m.H - 6; }

/* the standard XFCE start button: themed icon + label */
static int start_button_w(const struct panel *p) {
    return p->m.icon + 10 + panel_text_width(p, "Applications") +
           2 * p->m.btn_pad_x;
}

static int ws_box_w(const struct panel *p) {
    int w = (int)(p->m.H * 1.5);
    if (w < 44)
        w = 44;
    if (w > 84)
        w = 84;
    return w;
}

static int clock_w(const struct panel *p) {
    return panel_text_width(p, p->clock) + 2 * p->m.btn_pad_x;
}

static int exit_w(const struct panel *p) {
    return p->m.icon + 8 + panel_text_width(p, "Exit") + 2 * p->m.btn_pad_x;
}

/* truncate into b->label so the text fits `room` px, with a trailing
 * '~' ellipsis stand-in (ASCII font) */
static void fit_label(const struct panel *p, char *label, size_t n,
                       const char *text, int room) {
    snprintf(label, n, "%.46s", text);
    for (int used = panel_text_width(p, label);
         used > room && strlen(label) > 1;
         used = panel_text_width(p, label))
        label[strlen(label) - 1] = 0;
    size_t len = strlen(label);
    if (len + 1 < n && strcmp(label, text) != 0) {
        label[len] = '~';
        label[len + 1] = 0;
    }
}

/* rebuild btns[] + the region records from the current state */
static void layout(struct panel *p) {
    n_btns = 0;
    const int w = p->bar_w;
    const int gap = p->m.gap;

    /* --- start region: Start button, then configured launchers */
    p->x_start = p->m.edge;
    int x = p->x_start;
    if (n_btns < MAX_BTNS) {
        struct btn *b = &btns[n_btns++];
        *b = (struct btn){.x = x,
                          .w = start_button_w(p),
                          .kind = BTN_START,
                          .data = NULL,
                          .hover = false};
        snprintf(b->label, sizeof(b->label), "Applications");
        x += b->w + gap;
    }
    /* launchers: resolved desktop ids from the config */
    char lcopy[512];
    snprintf(lcopy, sizeof(lcopy), "%.500s", p->cfg.launchers);
    char *save = NULL;
    for (char *tok = strtok_r(lcopy, ",", &save);
         tok && n_btns < MAX_BTNS - 6; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ')
            tok++;
        const struct xwapp *app = xwapp_by_id(&p->apps, tok);
        if (!app)
            continue;
        struct btn *b = &btns[n_btns++];
        *b = (struct btn){.x = x,
                          .w = p->m.icon + 2 * p->m.btn_pad_x,
                          .kind = BTN_LAUNCHER,
                          .data = (void *)app,
                          .hover = false};
        snprintf(b->label, sizeof(b->label), "%.44s", app->name);
        x += b->w + gap;
    }
    p->w_start = x - p->m.edge - gap;
    x += gap;

    /* --- right region (right-aligned): [pager][gap][clock][gap][exit]
     * — the exit button sits on the opposite side of the clock from
     * the workspace pager (XFCE order) */
    int ew = exit_w(p);
    int cw = clock_w(p);
    int nws = p->wsp ? xwc_wspaces_count(p->wsp) : 0;
    int wsw = ws_box_w(p);
    int pager_w = nws > 0 ? nws * wsw + (nws - 1) * gap : 0;
    int right_w = pager_w + (nws > 0 ? gap : 0) + cw + gap + ew;
    int x_right = w - p->m.edge - right_w;
    p->x_right = x_right;
    p->w_right = right_w;

    /* --- taskbar fills the middle */
    int avail = x_right - gap - x;
    p->x_tasks = x;
    p->w_tasks = avail > 0 ? avail : 0;

    /* build the widget array left-to-right */
    /* tasks */
    struct xwc_task *task = p->tl ? xwc_tasklist_first(p->tl) : NULL;
    int n_tasks = 0;
    for (struct xwc_task *t = task; t; t = xwc_task_next(t))
        n_tasks++;
    int task_w = 0;
    if (n_tasks > 0 && p->w_tasks > 60) {
        task_w = (p->w_tasks - (n_tasks - 1) * gap) / n_tasks;
        if (task_w > 240)
            task_w = 240;
    }
    int n_shown = 0;
    for (struct xwc_task *t = task; t && n_btns < MAX_BTNS; t = xwc_task_next(t)) {
        if (task_w < 44) {
            /* center too narrow: show only as many as fit icon-only,
             * the rest behind a +N overflow button (stage 8 popup) */
            break;
        }
        struct btn *b = &btns[n_btns++];
        *b = (struct btn){.x = x,
                          .w = task_w,
                          .kind = BTN_TASK,
                          .data = t,
                          .hover = false,
                          .active = xwc_task_active(t)};
        const char *title = xwc_task_title(t);
        if (!*title)
            title = xwc_task_app_id(t);
        if (!*title)
            title = "(window)";
        if (task_w < 96 || p->cfg.tasklist_style == 1) {
            b->label[0] = 0; /* icon-only */
        } else {
            fit_label(p, b->label, sizeof(b->label), title,
                      task_w - p->m.icon - 12 - 2 * p->m.btn_pad_x);
        }
        x += task_w + gap;
        n_shown++;
    }
    if (n_shown < n_tasks && n_btns < MAX_BTNS) {
        /* overflow indicator */
        struct btn *b = &btns[n_btns++];
        *b = (struct btn){.x = x, .w = p->m.icon + 2 * p->m.btn_pad_x,
                          .kind = BTN_TASKOVER,
                          .data = (void *)(intptr_t)(n_tasks - n_shown)};
        snprintf(b->label, sizeof(b->label), "+%d", n_tasks - n_shown);
        x += b->w + gap;
    }

    /* right region widgets: pager boxes, clock, exit */
    int rx = x_right;
    for (int i = 0; i < nws && n_btns < MAX_BTNS; i++) {
        struct btn *b = &btns[n_btns++];
        *b = (struct btn){.x = rx,
                          .w = wsw,
                          .kind = BTN_WS,
                          .data = (void *)(intptr_t)i,
                          .hover = false,
                          .active = xwc_wspaces_active(p->wsp, i)};
        const char *name = xwc_wspaces_name(p->wsp, i);
        const char *sp = strrchr(name, ' ');
        const char *label = name && *name ? name : "?";
        if (sp && sp[1])
            label = sp + 1;
        snprintf(b->label, sizeof(b->label), "%s", label);
        rx += wsw + gap;
    }
    if (n_btns < MAX_BTNS) {
        struct btn *b = &btns[n_btns++];
        *b = (struct btn){.x = rx + (nws > 0 ? 0 : 0), .w = cw,
                          .kind = BTN_CLOCK, .hover = false};
        snprintf(b->label, sizeof(b->label), "%s", p->clock);
        rx += cw + gap;
    }
    if (n_btns < MAX_BTNS) {
        struct btn *b = &btns[n_btns++];
        *b = (struct btn){.x = w - p->m.edge - ew, .w = ew,
                          .kind = BTN_EXIT, .hover = false};
        snprintf(b->label, sizeof(b->label), "%s", "Exit");
    }
}

/* ------------------------------------------------------------- drawing */

/* procedural fallback icon: an app-grid (four rounded squares) in a
 * tinted cell — used when the themed icon is missing */
static void draw_grid_icon(uint32_t *pix, int stride, int bw, int bh, int x,
                           int y, int cell, uint32_t tint) {
    int q = cell / 2 - 2;
    if (q < 3)
        q = 3;
    int ox = x + (cell - 2 * q - 2) / 2;
    int oy = y + (cell - 2 * q - 2) / 2;
    xwc_draw_box(pix, stride, bw, bh, ox, oy, q + 1, q + 1, tint, tint);
    xwc_draw_box(pix, stride, bw, bh, ox + q + 2, oy, q + 1, q + 1, tint,
                 tint);
    xwc_draw_box(pix, stride, bw, bh, ox, oy + q + 2, q + 1, q + 1, tint,
                 tint);
    xwc_draw_box(pix, stride, bw, bh, ox + q + 2, oy + q + 2, q + 1, q + 1,
                 tint, tint);
}

/* one standard bar button: box + optional icon + optional label */
static void draw_btn(struct panel *p, uint32_t *pix, int stride, int bw,
                     int bh, const struct btn *b) {
    int y = btn_y(p), h = btn_h(p);
    uint32_t fill = COL_BTN_BG, border = COL_BTN_BORDER;
    if (b->kind == BTN_EXIT) {
        fill = b->hover ? COL_EXIT_HOVER : COL_EXIT_BG;
        border = COL_EXIT_HOVER;
    } else if (b->hover) {
        fill = COL_BTN_HOVER;
    } else if (b->active && b->kind == BTN_WS) {
        fill = COL_BTN_ACTIVE;
        border = 0xff88b0ef;
    } else if (b->active && b->kind == BTN_TASK) {
        fill = 0xff434c5e;
        border = 0xff5b6a80;
    }
    xwc_draw_box(pix, stride, bw, bh, b->x, y, b->w, h, fill, border);

    int icon = p->m.icon;
    int ty = y + (h - p->m.font_h) / 2 + 1;
    uint32_t fg = b->active ? 0xffffffff : COL_TEXT;

    /* icon + label composition per kind */
    const char *icon_name = NULL;
    switch (b->kind) {
    case BTN_START:
        icon_name = "start-here";
        break;
    case BTN_EXIT:
        icon_name = "system-shutdown";
        break;
    default:
        break;
    }

    if (b->kind == BTN_LAUNCHER || b->kind == BTN_TASK ||
        b->kind == BTN_TASKOVER) {
        const struct xwapp *app = b->kind == BTN_LAUNCHER
                                      ? (const struct xwapp *)b->data
                                      : NULL;
        /* task icons resolve from the window's app_id (stage 8
         * improves the matching; v1 tries the id as an icon name) */
        const char *in = NULL;
        if (app)
            in = app->icon[0] ? app->icon : app->desktop_id;
        else if (b->kind == BTN_TASK)
            in = xwc_task_app_id((struct xwc_task *)b->data);
        const struct xwc_icon *ic =
            (in && *in && p->cfg.menu_icons) ? xwc_icon_get(in, icon) : NULL;
        if (ic) {
            xwc_draw_icon(pix, stride, bw, bh, b->x + p->m.btn_pad_x,
                          y + (h - icon) / 2, ic, icon);
        } else if (b->kind == BTN_TASKOVER) {
            /* +N indicator: the label carries the count */
        } else {
            /* procedural fallback: initial letter or a generic glyph */
            const char *src =
                app ? app->name
                    : (b->kind == BTN_TASK ? xwc_task_title((struct xwc_task *)b->data) : "?");
            char one[2] = {src && *src ? (char)toupper((unsigned char)src[0]) : '?', 0};
            draw_grid_icon(pix, stride, bw, bh, b->x + p->m.btn_pad_x,
                           y + (h - icon) / 2, icon, 0xff3584e4);
            panel_draw_text(p, pix, stride, bw, bh,
                            b->x + p->m.btn_pad_x + (icon - 12) / 2,
                            ty + (icon - p->m.font_h) / 2, one, 0xffffffff);
        }
        if (b->label[0] &&
            (b->kind != BTN_TASK || p->cfg.tasklist_style != 1)) {
            int tx = b->x + p->m.btn_pad_x + icon + 6;
            panel_draw_text(p, pix, stride, bw, bh, tx, ty, b->label,
                            b->kind == BTN_TASKOVER ? COL_TEXT_DIM : fg);
        } else if (b->kind == BTN_TASKOVER) {
            int tx = b->x + (b->w - panel_text_width(p, b->label)) / 2;
            panel_draw_text(p, pix, stride, bw, bh, tx, ty, b->label,
                            COL_TEXT_DIM);
        }
        return;
    }

    /* start / ws / clock / exit: icon (when themed) + centered label */
    const struct xwc_icon *ic =
        icon_name ? xwc_icon_get(icon_name, icon) : NULL;
    if (b->kind == BTN_START || b->kind == BTN_EXIT) {
        if (ic) {
            xwc_draw_icon(pix, stride, bw, bh, b->x + p->m.btn_pad_x,
                          y + (h - icon) / 2, ic, icon);
        } else {
            /* procedural: grid logo for Start, a power glyph for Exit */
            if (b->kind == BTN_START)
                draw_grid_icon(pix, stride, bw, bh, b->x + p->m.btn_pad_x,
                               y + (h - icon) / 2, icon, 0xff3584e4);
            else
                xwc_draw_box(pix, stride, bw, bh, b->x + p->m.btn_pad_x + 3,
                             y + (h - icon) / 2 + 3, icon - 6, icon - 6, 0,
                             0xffffffff);
        }
        int tx = b->x + p->m.btn_pad_x + icon + 6;
        panel_draw_text(p, pix, stride, bw, bh, tx, ty, b->label, fg);
        return;
    }

    /* centered text: ws boxes, clock */
    uint32_t c_fg = b->kind == BTN_CLOCK ? COL_TEXT_DIM : fg;
    int tx = b->x + (b->w - panel_text_width(p, b->label)) / 2;
    panel_draw_text(p, pix, stride, bw, bh, tx, ty, b->label, c_fg);
}

static void draw(struct panel *p) {
    int stride = 0;
    uint32_t *pix = xwc_layer_pixels(p->layer, &stride);
    if (!pix || stride < 1)
        return;
    int w = stride, h = p->m.H;
    xwc_fill_rect(pix, stride, w, h, 0, 0, w, h, COL_BAR_BG);
    for (int i = 0; i < n_btns; i++)
        draw_btn(p, pix, stride, w, h, &btns[i]);
    xwc_layer_commit(p->layer);
    p->redraw = false;
}

static void request_layout(struct panel *p) {
    layout(p);
    p->redraw = true;
}

static void on_tasklist_changed(void *ud) {
    struct panel *p = ud;
    request_layout(p);
}

static void on_wspaces_changed(void *ud) {
    struct panel *p = ud;
    request_layout(p);
}

/* --------------------------------------------------------------- input */

static struct btn *btn_at(struct panel *p, int x, int y) {
    if (y < btn_y(p) || y >= btn_y(p) + btn_h(p))
        return NULL;
    for (int i = 0; i < n_btns; i++)
        if (x >= btns[i].x && x < btns[i].x + btns[i].w)
            return &btns[i];
    return NULL;
}

#define BTN_LEFT 0x110
#define BTN_MIDDLE 0x112
#define BTN_RIGHT 0x111

static const char *btn_kind_name(int kind) {
    switch (kind) {
    case BTN_START: return "menu/launcher";
    case BTN_LAUNCHER: return "launcher";
    case BTN_WS: return "workspace";
    case BTN_TASK: return "task";
    case BTN_TASKOVER: return "task-overflow";
    case BTN_CLOCK: return "clock";
    case BTN_EXIT: return "exit";
    default: return "(none)";
    }
}

/* ------------------------------------------------------------- the menu */

/* The Start/Applications menu (panel-menu.c owns the module from the
 * next commit; the geometry plumbing lives here so the region order
 * is testable already). */

#include "panel-menu.h" /* pm_menu_*: the Start menu module */

/* ------------------------------------------------------------ actions */

static void do_exit_button(struct panel *p) {
    (void)p;
    fprintf(stderr, "xw-panel: exit clicked (ctl exit-dialog, async)\n");
    panel_ctl_send("exit-dialog");
}

static void do_launcher_fallback(struct panel *p) {
    /* no .desktop applications at all: the resolved terminal is the
     * one thing Start can still do */
    char cmd[320];
    resolve_terminal(p->terminal_cmd, sizeof(p->terminal_cmd));
    snprintf(cmd, sizeof(cmd), "run %s", p->terminal_cmd);
    fprintf(stderr, "xw-panel: start clicked — no applications found, "
                    "launching the fallback terminal (ctl '%s')\n", cmd);
    panel_ctl_send(cmd);
}

static void on_button(struct xwc_win *win, uint32_t button, bool down, int x,
                      int y, void *ud) {
    struct panel *p = ud;
    if (win)
        p->layer = (struct xwc_layer *)win;
    if (!down)
        return;
    struct btn *b = btn_at(p, x, y);
    trace_log("button %u %s at %d,%d -> widget=%s", button,
              down ? "press" : "release", x, y,
              b ? btn_kind_name(b->kind) : "(miss)");
    if (!b)
        return;
    switch (b->kind) {
    case BTN_START:
        if (button == BTN_LEFT) {
            bool had_menu = pm_menu_open();
            pm_menu_toggle(p, b->x, 0, b->w, p->m.H);
            if (!had_menu && !pm_menu_open() &&
                xwapp_db_count(&p->apps) == 0)
                do_launcher_fallback(p); /* no apps: try a terminal */
        }
        break;
    case BTN_LAUNCHER:
        if (button == BTN_LEFT)
            pm_menu_launch_app(p, (const struct xwapp *)b->data);
        break;
    case BTN_WS:
        if (button == BTN_LEFT) {
            trace_log("activate action=workspace index=%d",
                      (int)(intptr_t)b->data);
            xwc_wspaces_activate(p->wsp, (int)(intptr_t)b->data);
        }
        break;
    case BTN_TASK:
        if (button == BTN_MIDDLE || button == BTN_RIGHT)
            xwc_tasklist_close(p->tl, b->data);
        else if (button == BTN_LEFT)
            xwc_tasklist_activate(p->tl, b->data);
        break;
    case BTN_CLOCK:
        if (button == BTN_LEFT)
            pm_clock_click(p, b->x, 0, b->w, p->m.H);
        break;
    case BTN_EXIT:
        if (button == BTN_LEFT)
            do_exit_button(p);
        break;
    default:
        break;
    }
}

static void on_motion(struct xwc_win *win, int x, int y, void *ud) {
    struct panel *p = ud;
    if (win)
        p->layer = (struct xwc_layer *)win;
    struct btn *b = btn_at(p, x, y);
    bool changed = false;
    for (int i = 0; i < n_btns; i++) {
        bool want = &btns[i] == b;
        if (btns[i].hover != want) {
            btns[i].hover = want;
            changed = true;
        }
    }
    if (changed)
        p->redraw = true;
}

static void on_configure(struct xwc_win *win, int w, int h, void *ud) {
    struct panel *p = ud;
    if (win)
        p->layer = (struct xwc_layer *)win;
    trace_log("configure: %dx%d", w, h);
    p->bar_w = w;
    p->bar_h = h;
    /* the layer told us our height (exclusive zone request); the
     * metrics re-derive from the actual surface */
    if (p->cfg.height == 0)
        p->m = panel_metrics_for(0, p->c.output_h > 0 ? p->c.output_h : 720);
    else
        p->m = panel_metrics_for(p->cfg.height, 720);
    request_layout(p);
    draw(p); /* configure implies a fresh buffer: draw immediately */
}

static void on_close(struct xwc_win *win, void *ud) {
    (void)win;
    struct panel *p = ud;
    p->quit = true;
}

/* --------------------------------------------------------------- main */

static void on_sigterm(int sig) {
    (void)sig;
    if (g_panel)
        g_panel->quit = true;
}

static void on_sigchld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char **argv) {
    const char *socket_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socket_name = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: xw-panel [--socket NAME]\n");
            return 0;
        }
    }

    struct panel p = {0};
    g_panel = &p;
    panel_trace_enable(getenv("XW_PANEL_TRACE") != NULL);
    const char *wd = getenv("WAYLAND_DISPLAY");
    trace_log("starting: WAYLAND_DISPLAY=%s socket=%s", wd ? wd : "(unset)",
              socket_name ? socket_name : "(default)");

    panel_config_load(&p.cfg);
    trace_log("config: height=%s position=%s clock_format='%s'",
              p.cfg.height ? "fixed" : "auto", p.cfg.bottom ? "bottom" : "top",
              p.cfg.clock_format);

    /* clock + terminal resolved once; the clock string refreshes on
     * the format/second tick in the main loop */
    panel_clock_format(&p.cfg, p.clock, sizeof(p.clock));
    resolve_terminal(p.terminal_cmd, sizeof(p.terminal_cmd));
    trace_log("launcher terminal resolved: '%s'", p.terminal_cmd);

    struct sigaction sa = {0};
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGTERM, on_sigterm);
    signal(SIGINT, on_sigterm);
    signal(SIGHUP, on_sigterm);
    /* clear any signal mask inherited across fork+exec (the panel can
     * be launched by a signalfd-using parent) */
    {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
    }

    if (xwc_connect(&p.c, socket_name) < 0)
        return 1;
    trace_log("connected: globals bound");

    p.tl = xwc_tasklist_create(&p.c, on_tasklist_changed, &p);
    p.wsp = xwc_wspaces_create(&p.c, on_wspaces_changed, &p);
    trace_log("tasklist %s, workspaces %s", p.tl ? "bound" : "unavailable",
              p.wsp ? "bound" : "unavailable");

    /* the XDG application database (built once; the menu rescans on
     * open when the directories changed) */
    xwapp_db_scan(&p.apps);
    {
        const char *fav[64];
        int n_fav = 0;
        char fcopy[512];
        snprintf(fcopy, sizeof(fcopy), "%.500s", p.cfg.favorites);
        char *save = NULL;
        for (char *tok = strtok_r(fcopy, ",", &save);
             tok && n_fav < 64; tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ')
                tok++;
            fav[n_fav++] = tok;
        }
        xwapp_db_set_favorites(&p.apps, fav, n_fav);
    }
    trace_log("application database: %d entries", xwapp_db_count(&p.apps));

    p.m = panel_metrics_for(p.cfg.height,
                            p.c.output_h > 0 ? p.c.output_h : 720);
    trace_log("metrics: H=%d font=%s icon=%d (output %dx%d)", p.m.H,
              p.m.big_font ? "24px" : "16px", p.m.icon, p.c.output_w,
              p.c.output_h);

    struct xwc_callbacks cb = {
        .button = on_button,
        .motion = on_motion,
        .configure = on_configure,
        .close = on_close,
        .key = NULL, /* the bar never takes keyboard (popups do) */
        .ud = &p,
    };
    /* bar spanning the output, reserving its height as exclusive zone;
     * top or bottom per the config */
    uint32_t anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                       (p.cfg.bottom
                            ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                            : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
    trace_log("requesting layer: %s bar, anchors L|R|%s, exclusive %d",
              p.cfg.bottom ? "BOTTOM" : "TOP", p.cfg.bottom ? "B" : "T",
              p.m.H);
    p.layer = xwc_layer_create(&p.c, &cb, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                               anchors, p.m.H, 0, p.m.H);
    if (!p.layer) {
        fprintf(stderr, "xw-panel: cannot create the bar layer\n");
        xwc_tasklist_destroy(p.tl);
        xwc_wspaces_destroy(p.wsp);
        xwc_disconnect(&p.c);
        return 1;
    }
    xwc_layer_set_keyboard(p.layer, 0); /* keyboard stays with windows */

    /* main loop: dispatch with a 1s ceiling so the clock ticks */
    while (!p.quit && p.c.running) {
        if (xwc_dispatch(&p.c, 1000) < 0)
            break; /* compositor went away */
        char now[48];
        panel_clock_format(&p.cfg, now, sizeof(now));
        if (strcmp(now, p.clock) != 0) {
            snprintf(p.clock, sizeof(p.clock), "%s", now);
            request_layout(&p);
        }
        if (p.redraw && p.layer)
            draw(&p);
    }

    /* compositor shutdown with popups open: destroy them before the
     * connection teardown */
    pm_menu_shutdown(&p);
    pm_clock_shutdown(&p);
    xwc_layer_destroy(p.layer);
    xwc_tasklist_destroy(p.tl);
    xwc_wspaces_destroy(p.wsp);
    xwc_disconnect(&p.c);
    g_panel = NULL;
    return 0;
}
