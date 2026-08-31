/* xw-panel — the desktop panel (xfce4-panel parity, v0).
 *
 * A layer-shell top bar (exclusive zone = bar height, so windows never
 * render under it) with the v0 plugin set:
 *
 *   [ >_ ]  launcher (spawns the configured terminal via ctl "run")
 *   [ 1 ][ 2 ][ 3 ]  workspace switcher (ext-workspace)
 *   [ Terminal — bash ]  tasklist (wlr-foreign-toplevel: click =
 *                         activate, right/middle click = close)
 *              [ 12:34 ]  clock (HH:MM, redrawn when the minute flips)
 *                [Exit]  exit button (spawns the session exit dialog
 *                         via ctl "exit-dialog" — the same
 *                         XW_ACTION_EXIT_DIALOG behavior as Ctrl+Alt+Del)
 *
 * Everything renders through libxwcl + the build-time bitmap font; no
 * toolkit, no fontconfig, no D-Bus. One process, one surface.
 *
 * v0 deviations from xfce4-panel (documented in ROADMAP.md): single
 * output, fixed plugin order, no plugin API, no drag-reordering, no
 * preferences dialog, icon-less buttons (text labels until an icon
 * blitter exists).
 */
#include "xwc.h"
#include "xw-ctl.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wlr-layer-shell-unstable-v1.h"

/* ---------------------------------------------------------------- look */

#define BAR_H 28
#define BTN_PAD_X 7
#define BTN_GAP 3
#define EDGE_PAD 3
#define WS_BTN_W 24
#define LAUNCHER_W 36

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

struct btn {
    int x, w;            /* bar-local geometry (y is the whole bar) */
    int kind;            /* BTN_* */
    void *data;          /* task ptr / ws index (as intptr) */
    bool hover;
    bool active;         /* active workspace / focused window */
    char label[48];      /* truncated text */
};

enum {
    BTN_NONE = 0,
    BTN_LAUNCHER,
    BTN_WS,
    BTN_TASK,
    BTN_CLOCK,
    BTN_EXIT,
};

#define MAX_BTNS 64

struct panel {
    struct xwc c;
    struct xwc_layer *layer;
    struct xwc_tasklist *tl;
    struct xwc_wspaces *wsp;

    struct btn btns[MAX_BTNS];
    int n_btns;

    char clock[8];    /* "HH:MM" */
    int bar_w, bar_h; /* current geometry */

    char terminal_cmd[256];
    bool redraw;      /* coalesced redraw request */
    bool quit;
};

static struct panel *g_panel; /* signal handler context */

/* -------------------------------------------------------------- clock */

static void clock_read(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) {
        snprintf(buf, n, "--:--");
        return;
    }
    snprintf(buf, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* ------------------------------------------------------------- layout */

/* rebuild btns[] from the current tasklist + workspace state; the
 * layout is deterministic: launcher, workspaces, tasks, then
 * right-aligned clock + exit */
static void layout(struct panel *p) {
    p->n_btns = 0;
    int x = EDGE_PAD;
    int w = p->bar_w;

    /* launcher */
    if (p->n_btns < MAX_BTNS) {
        struct btn *b = &p->btns[p->n_btns++];
        *b = (struct btn){.x = x,
                          .w = LAUNCHER_W,
                          .kind = BTN_LAUNCHER,
                          .data = NULL,
                          .hover = false};
        snprintf(b->label, sizeof(b->label), ">_");
        x += LAUNCHER_W + BTN_GAP;
    }

    /* workspace switcher */
    int nws = p->wsp ? xwc_wspaces_count(p->wsp) : 0;
    for (int i = 0; i < nws && p->n_btns < MAX_BTNS; i++) {
        struct btn *b = &p->btns[p->n_btns++];
        *b = (struct btn){.x = x,
                          .w = WS_BTN_W,
                          .kind = BTN_WS,
                          .data = (void *)(intptr_t)i,
                          .hover = false,
                          .active = xwc_wspaces_active(p->wsp, i)};
        const char *name = xwc_wspaces_name(p->wsp, i);
        /* "Workspace N" is long: show the trailing number, else the
         * name truncated */
        const char *sp = strrchr(name, ' ');
        const char *label = name && *name ? name : "?";
        if (sp && sp[1])
            label = sp + 1;
        snprintf(b->label, sizeof(b->label), "%s", label);
        x += WS_BTN_W + BTN_GAP;
    }
    x += BTN_GAP; /* gap between plugin groups */

    /* right side: exit + clock */
    const char *exit_lbl = "Exit";
    int exit_w = xwc_text_width(exit_lbl) + 2 * BTN_PAD_X;
    int clock_w = xwc_text_width("00:00") + 2 * BTN_PAD_X;
    int right = w - EDGE_PAD - exit_w - BTN_GAP - clock_w;

    /* tasklist fills the middle */
    int avail = right - x;
    struct xwc_task *task = p->tl ? xwc_tasklist_first(p->tl) : NULL;
    int n_tasks = 0;
    for (struct xwc_task *t = task; t; t = xwc_task_next(t))
        n_tasks++;
    int task_w = 0;
    if (n_tasks > 0 && avail > 40)
        task_w = (avail - (n_tasks - 1) * BTN_GAP) / n_tasks;
    if (task_w > 220)
        task_w = 220; /* xfce caps task buttons; keeps rows readable */
    for (struct xwc_task *t = task; t; t = xwc_task_next(t)) {
        if (p->n_btns >= MAX_BTNS || task_w < 40)
            break;
        struct btn *b = &p->btns[p->n_btns++];
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
        /* truncate with ellipsis for the bitmap font */
        snprintf(b->label, sizeof(b->label), "%s", title);
        for (int used = xwc_text_width(b->label);
             used > task_w - 2 * BTN_PAD_X - 4 && strlen(b->label) > 1;
             used = xwc_text_width(b->label))
            b->label[strlen(b->label) - 1] = 0;
        size_t len = strlen(b->label);
        if (len + 1 < sizeof(b->label) && strcmp(b->label, title) != 0) {
            b->label[len] = '~'; /* ellipsis stand-in (ASCII font) */
            b->label[len + 1] = 0;
        }
        x += task_w + BTN_GAP;
    }

    /* clock */
    if (p->n_btns < MAX_BTNS) {
        struct btn *b = &p->btns[p->n_btns++];
        *b = (struct btn){.x = right + BTN_GAP,
                          .w = clock_w,
                          .kind = BTN_CLOCK,
                          .hover = false};
        snprintf(b->label, sizeof(b->label), "%s", p->clock);
    }
    /* exit */
    if (p->n_btns < MAX_BTNS) {
        struct btn *b = &p->btns[p->n_btns++];
        *b = (struct btn){.x = w - EDGE_PAD - exit_w,
                          .w = exit_w,
                          .kind = BTN_EXIT,
                          .hover = false};
        snprintf(b->label, sizeof(b->label), "%s", exit_lbl);
    }
}

/* ------------------------------------------------------------- drawing */

static void draw(struct panel *p) {
    int stride = 0;
    uint32_t *pix = xwc_layer_pixels(p->layer, &stride);
    if (!pix || stride < 1)
        return;
    int w = stride, h = p->bar_h;

    xwc_fill_rect(pix, stride, w, h, 0, 0, w, h, COL_BAR_BG);

    int ty = (h - XWC_LINE_H) / 2 + 1;
    for (int i = 0; i < p->n_btns; i++) {
        struct btn *b = &p->btns[i];
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
            fill = 0xff434c5e; /* focused task: subtle highlight */
            border = 0xff5b6a80;
        }
        xwc_draw_box(pix, stride, w, h, b->x, 2, b->w, h - 4, fill, border);
        uint32_t fg = b->active ? 0xffffffff : COL_TEXT;
        if (b->kind == BTN_CLOCK)
            fg = COL_TEXT_DIM;
        int tx = b->x + (b->w - xwc_text_width(b->label)) / 2;
        xwc_draw_text(pix, stride, w, h, tx, ty, b->label, fg);
    }
    xwc_layer_commit(p->layer);
    p->redraw = false;
}

/* ------------------------------------------------------------ triggers */

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
    if (y < 0 || y >= p->bar_h)
        return NULL;
    for (int i = 0; i < p->n_btns; i++)
        if (x >= p->btns[i].x && x < p->btns[i].x + p->btns[i].w)
            return &p->btns[i];
    return NULL;
}

#define BTN_LEFT 0x110
#define BTN_MIDDLE 0x112
#define BTN_RIGHT 0x111

static void do_exit_button(struct panel *p) {
    (void)p;
    char reply[256];
    if (!xw_ctl_send("exit-dialog", reply, sizeof(reply)))
        fprintf(stderr, "xw-panel: exit dialog: %s\n", reply);
}

static void do_launcher(struct panel *p) {
    char cmd[320], reply[256];
    snprintf(cmd, sizeof(cmd), "run %s", p->terminal_cmd);
    if (!xw_ctl_send(cmd, reply, sizeof(reply)))
        fprintf(stderr, "xw-panel: launcher: %s\n", reply);
}

static void on_button(struct xwc_win *win, uint32_t button, bool down, int x,
                      int y, void *ud) {
    struct panel *p = ud;
    if (win)
        p->layer = (struct xwc_layer *)win;
    if (!down)
        return;
    struct btn *b = btn_at(p, x, y);
    if (!b)
        return;
    switch (b->kind) {
    case BTN_LAUNCHER:
        if (button == BTN_LEFT)
            do_launcher(p);
        break;
    case BTN_WS:
        if (button == BTN_LEFT)
            xwc_wspaces_activate(p->wsp, (int)(intptr_t)b->data);
        break;
    case BTN_TASK:
        if (button == BTN_MIDDLE || button == BTN_RIGHT)
            xwc_tasklist_close(p->tl, b->data);
        else if (button == BTN_LEFT)
            xwc_tasklist_activate(p->tl, b->data);
        break;
    case BTN_EXIT:
        if (button == BTN_LEFT)
            do_exit_button(p);
        break;
    default: /* clock */
        break;
    }
}

static void on_motion(struct xwc_win *win, int x, int y, void *ud) {
    struct panel *p = ud;
    if (win)
        p->layer = (struct xwc_layer *)win;
    struct btn *b = btn_at(p, x, y);
    bool changed = false;
    for (int i = 0; i < p->n_btns; i++) {
        bool want = &p->btns[i] == b;
        if (p->btns[i].hover != want) {
            p->btns[i].hover = want;
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
    p->bar_w = w;
    p->bar_h = h;
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
    clock_read(p.clock, sizeof(p.clock));
    const char *term = getenv("XW_TERMINAL");
    snprintf(p.terminal_cmd, sizeof(p.terminal_cmd), "%s",
             term && *term ? term : "x-terminal-emulator");
    p.bar_h = BAR_H;

    signal(SIGTERM, on_sigterm);
    signal(SIGINT, on_sigterm);

    if (xwc_connect(&p.c, socket_name) < 0)
        return 1;

    p.tl = xwc_tasklist_create(&p.c, on_tasklist_changed, &p);
    p.wsp = xwc_wspaces_create(&p.c, on_wspaces_changed, &p);

    struct xwc_callbacks cb = {
        .button = on_button,
        .motion = on_motion,
        .configure = on_configure,
        .close = on_close,
        .key = NULL, /* the panel never takes keyboard (v0) */
        .ud = &p,
    };
    /* top bar spanning the output, reserving its height as exclusive
     * zone; width comes from the anchors (LEFT|RIGHT) */
    p.layer = xwc_layer_create(
        &p.c, &cb, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        BAR_H, 0, BAR_H);
    if (!p.layer) {
        fprintf(stderr, "xw-panel: cannot create the bar layer\n");
        xwc_tasklist_destroy(p.tl);
        xwc_wspaces_destroy(p.wsp);
        xwc_disconnect(&p.c);
        return 1;
    }
    xwc_layer_set_keyboard(p.layer, 0); /* keyboard stays with windows */

    /* main loop: dispatch with a 1s ceiling so the clock ticks; the
     * compositor driving events (title changes, workspace switches)
     * wakes us immediately */
    while (!p.quit && p.c.running) {
        if (xwc_dispatch(&p.c, 1000) < 0)
            break; /* compositor went away */
        char now[8];
        clock_read(now, sizeof(now));
        if (strcmp(now, p.clock) != 0) {
            snprintf(p.clock, sizeof(p.clock), "%s", now);
            request_layout(&p);
        }
        if (p.redraw && p.layer)
            draw(&p);
    }

    xwc_layer_destroy(p.layer);
    xwc_tasklist_destroy(p.tl);
    xwc_wspaces_destroy(p.wsp);
    xwc_disconnect(&p.c);
    g_panel = NULL;
    return 0;
}
