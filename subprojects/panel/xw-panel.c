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
#include "clients/xw-ctl.h" /* client-side ctl wire (src/clients, via -Isrc) */

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>

#include "wlr-layer-shell-unstable-v1.h"
#include <xkbcommon/xkbcommon-keysyms.h>

/* ------------------------------------------------------------------ */
/* Terminal resolution — deliberately a local copy of the compositor's
 * logic (src/libxw/xw-actions.c): the panel is an independent client
 * and MUST NOT link compositor code. The candidate list is a fact
 * about the terminal ecosystem, not shared behavior. */

static bool path_has_binary(const char *name) {
    if (strchr(name, '/'))
        return access(name, X_OK) == 0; /* already a path */
    const char *path = getenv("PATH");
    if (!path || !*path)
        return false;
    char probe[PATH_MAX];
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > sizeof(probe) - 64)
            len = sizeof(probe) - 64; /* clamp dir for the join below */
        if (len > 0) {
            snprintf(probe, sizeof(probe), "%.*s/%.32s", (int)len, p,
                     name);
            if (access(probe, X_OK) == 0)
                return true;
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return false;
}

/* $XW_TERMINAL wins if it exists; else the first common terminal on
 * PATH. x-terminal-emulator alone is a Debian-ism — on Arch/Artix it
 * made the launcher spawn sh -c -> 127 with no visible effect. */
static void resolve_terminal(char *out, size_t n) {
    const char *env = getenv("XW_TERMINAL");
    if (env && *env) {
        snprintf(out, n, "%.240s", env);
        return;
    }
    static const char *const candidates[] = {
        "xfce4-terminal", "konsole",    "gnome-terminal", "kitty",
        "alacritty",      "foot",      "wezterm",        "lxterminal",
        "terminology",    "st",        "xterm",          "x-terminal-emulator",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]);
         i++) {
        if (path_has_binary(candidates[i])) {
            snprintf(out, n, "%s", candidates[i]);
            return;
        }
    }
    snprintf(out, n, "%s", "x-terminal-emulator"); /* last resort */
}

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

/* Startup chain trace ($XW_PANEL_TRACE=1, or inherited from
 * xw-session --verbose): one stderr line per step of the
 * connect -> globals -> layer create -> configure -> buffer ->
 * commit chain, so "panel missing" can be localized to the exact
 * broken step from a single real-TTY run. */
static bool g_trace;
static void trace(const char *fmt, ...) {
    if (!g_trace)
        return;
    fputs("xw-panel: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

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

    /* the applications menu (xdg_popup parented to the bar layer) */
    struct xwc_popup *menu; /* NULL = closed */
    int menu_hover;          /* hovered item index, -1 = none */
    int menu_w, menu_h;      /* current popup geometry */
    int64_t menu_closed_ms;  /* dismissal timestamp: a Start press in
                                the same click that dismissed the menu
                                must not reopen it (toggle) */
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
    static bool first = true;
    if (first) {
        first = false;
        trace("first buffer ready: %dx%d — attach+commit follows",
              stride, p->bar_h);
    }
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

/* ------------------------------------------------------------ menu */

/* The Start/Applications menu: an xdg_popup parented to the bar layer
 * (xfce4-panel's applications-menu parity). Items come from XDG
 * .desktop entries; selecting one launches it through the session
 * manager's ctl "run" (same wire the old terminal-launcher used).
 * Open/close is idempotent; Escape and any outside press dismiss it
 * (outside-press dismissal is compositor-side: it sends popup_done,
 * the close callback runs). */

#define MENU_MAX_ITEMS 24 /* v0 cap: no scrolling yet (ROADMAP) */
#define MENU_W 230
#define MENU_ITEM_H 26
#define MENU_PAD 4

struct menu_item {
    char name[72];
    char exec[192];
};

static struct menu_item g_menu_items[MENU_MAX_ITEMS];
static int g_menu_n_items;

#define COL_MENU_BG 0xff262b33
#define COL_MENU_BORDER 0xff3c4454
#define COL_MENU_HOVER 0xff3584e4
#define COL_MENU_SEP 0xff384050

/* one .desktop field (first match in [Desktop Entry]); a local copy
 * of the session manager's reader — the panel links no session code */
static char *menu_desktop_field(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char line[1024];
    bool in_entry = false;
    size_t klen = strlen(key);
    char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            in_entry = strncmp(line, "[Desktop Entry]", 15) == 0;
            continue;
        }
        if (!in_entry)
            continue;
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *val = line + klen + 1;
            char *nl = strchr(val, '\n');
            if (nl)
                *nl = 0;
            result = strdup(val);
            break;
        }
    }
    fclose(f);
    return result;
}

/* strip XDG field codes (%f %F %u %U %i %c %k ...) from an Exec line */
static void menu_clean_exec(char *exec) {
    char *r = exec, *w = exec;
    bool spaced = false;
    while (*r) {
        if (*r == '%') {
            r++; /* the code character */
            if (*r)
                r++; /* %f, %%%, ...: skip both */
            spaced = true; /* the removed placeholder was an argument */
            continue;
        }
        if (*r == ' ') {
            if (spaced) { /* collapse runs left by removed codes */
                r++;
                continue;
            }
            spaced = true;
        } else {
            spaced = false;
        }
        *w++ = *r++;
    }
    *w = 0;
    /* trim a trailing separator left by trailing field codes */
    while (w > exec && (w[-1] == ' '))
        *--w = 0;
}

/* scan one applications dir; dedup by desktop-file basename (the
 * user dir overrides the system one, same rule as XDG autostart) */
static void menu_scan_dir(const char *dir, char seen[][72], int *n_seen,
                          int seen_cap) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) && g_menu_n_items < MENU_MAX_ITEMS) {
        if (de->d_name[0] == '.' || !strstr(de->d_name, ".desktop"))
            continue;
        /* user entry wins: a same-named system entry is skipped */
        bool dup = false;
        for (int i = 0; i < *n_seen; i++) {
            if (strcmp(seen[i], de->d_name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        char *type = menu_desktop_field(path, "Type");
        char *name = menu_desktop_field(path, "Name");
        char *exec = menu_desktop_field(path, "Exec");
            char *nodisp = menu_desktop_field(path, "NoDisplay");
        char *hidden = menu_desktop_field(path, "Hidden");
        char *only = menu_desktop_field(path, "OnlyShowIn");
        char *notshow = menu_desktop_field(path, "NotShowIn");
        char *term = menu_desktop_field(path, "Terminal");

        bool ok = type && strcmp(type, "Application") == 0 && name && name[0] &&
                  exec && exec[0] &&
                  !(nodisp && strcmp(nodisp, "true") == 0) &&
                  !(hidden && strcmp(hidden, "true") == 0);
        /* xfce4-panel shows OnlyShowIn entries only when they name
         * XFCE; NotShowIn containing XFCE hides the entry */
        if (ok && only && *only && !strstr(only, "XFCE"))
            ok = false;
        if (ok && notshow && strstr(notshow, "XFCE"))
            ok = false;
        /* Terminal=true needs a terminal wrapper to host the command;
         * v0 has none (documented deviation, ROADMAP.md) — hide
         * instead of failing at launch time */
        if (ok && term && strcmp(term, "true") == 0)
            ok = false;

        if (ok) {
            struct menu_item *it = &g_menu_items[g_menu_n_items++];
            snprintf(it->name, sizeof(it->name), "%.68s", name);
            snprintf(it->exec, sizeof(it->exec), "%.188s", exec);
            menu_clean_exec(it->exec);
            if (*n_seen < seen_cap)
                snprintf(seen[(*n_seen)++], 72, "%.68s", de->d_name);
        }
        free(type);
        free(name);
        free(exec);
        free(nodisp);
        free(hidden);
        free(only);
        free(notshow);
        free(term);
    }
    closedir(d);
}

static int menu_item_cmp(const void *a, const void *b) {
    return strcasecmp(((const struct menu_item *)a)->name,
                      ((const struct menu_item *)b)->name);
}

static void menu_scan_apps(void) {
    g_menu_n_items = 0;
    static char seen[256][72];
    int n_seen = 0;

    /* XDG data dirs, user first (overrides same-named system entries) */
    char user_dir[512];
    const char *home = getenv("HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && *xdg_data)
        snprintf(user_dir, sizeof(user_dir), "%.480s/applications", xdg_data);
    else if (home)
        snprintf(user_dir, sizeof(user_dir),
                 "%.240s/.local/share/applications", home);
    if (user_dir[0])
        menu_scan_dir(user_dir, seen, &n_seen, 256);

    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs)
        dirs = "/usr/local/share:/usr/share";
    char dcopy[512];
    snprintf(dcopy, sizeof(dcopy), "%.500s", dirs);
    char *save = NULL;
    for (char *tok = strtok_r(dcopy, ":", &save); tok;
         tok = strtok_r(NULL, ":", &save)) {
        char sys_dir[560];
        snprintf(sys_dir, sizeof(sys_dir), "%.520s/applications", tok);
        menu_scan_dir(sys_dir, seen, &n_seen, 256);
    }

    qsort(g_menu_items, (size_t)g_menu_n_items, sizeof(g_menu_items[0]),
          menu_item_cmp);
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void menu_close(struct panel *p, const char *why) {
    if (!p->menu)
        return;
    fprintf(stderr,
            "xw-panel: menu closing (%s) — menu surface destroyed\n", why);
    xwc_popup_destroy(p->menu);
    p->menu = NULL;
    p->menu_hover = -1;
    p->menu_closed_ms = mono_ms();
}

static void menu_draw(struct panel *p) {
    int stride = 0;
    uint32_t *pix = xwc_popup_pixels(p->menu, &stride);
    if (!pix || stride < 1)
        return;
    int w = stride, h = p->menu_h;
    /* bordered panel-style box: the interior stays the menu background
     * (fill=0 would paint it black — draw_box always fills) */
    xwc_draw_box(pix, stride, w, h, 0, 0, w, h, COL_MENU_BG, COL_MENU_BORDER);

    int ty_off = (MENU_ITEM_H - XWC_LINE_H) / 2;
    for (int i = 0; i < g_menu_n_items; i++) {
        int iy = MENU_PAD + i * MENU_ITEM_H;
        if (i == p->menu_hover)
            xwc_fill_rect(pix, stride, w, h, 1, iy, w - 2, MENU_ITEM_H,
                          COL_MENU_HOVER);
        else if (i > 0)
            xwc_draw_hline(pix, stride, w, h, 8, iy, w - 16, COL_MENU_SEP);
        /* item label, truncated to the menu width */
        char label[sizeof(g_menu_items[0].name)];
        snprintf(label, sizeof(label), "%.68s", g_menu_items[i].name);
        for (int used = xwc_text_width(label);
             used > MENU_W - 20 && strlen(label) > 1;
             used = xwc_text_width(label))
            label[strlen(label) - 1] = 0;
        xwc_draw_text(pix, stride, w, h, 10, iy + ty_off, label,
                      i == p->menu_hover ? 0xffffffff : COL_TEXT);
    }
    xwc_popup_commit(p->menu);
}

static void menu_launch(struct panel *p, int idx) {
    if (idx < 0 || idx >= g_menu_n_items)
        return;
    struct menu_item *it = &g_menu_items[idx];
    fprintf(stderr, "xw-panel: menu item selected: '%s' -> ctl \"run %s\"\n",
            it->name, it->exec);
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "run %s", it->exec);
    if (!xw_ctl_send_async(cmd))
        fprintf(stderr, "xw-panel: cannot launch '%s' (fork failed)\n",
                it->name);
    menu_close(p, "item selected");
}

static void menu_on_configure(struct xwc_win *win, int w, int h, void *ud) {
    struct panel *p = ud;
    /* the configure fires DURING xwc_popup_create (before menu_open
     * could store the pointer): the popup arrives as the callback's
     * own first argument — store it now, or the draw below would see
     * a NULL menu and silently never commit (the exact bug that kept
     * the first menu invisible) */
    struct xwc_popup *menu = (struct xwc_popup *)win;
    if (menu)
        p->menu = menu;
    p->menu_w = w > 0 ? w : MENU_W;
    p->menu_h = h > 0 ? h : MENU_PAD * 2 + g_menu_n_items * MENU_ITEM_H;
    fprintf(stderr,
            "xw-panel: menu surface created: %dx%d, item count=%d%s\n",
            p->menu_w, p->menu_h, g_menu_n_items,
            g_menu_n_items == MENU_MAX_ITEMS ? " (v0 cap reached — more "
                                               "applications exist, see "
                                               "ROADMAP)"
                                             : "");
    menu_draw(p);
    /* the grab (keyboard + outside-press dismissal) must follow the
     * mapping commit — request ordering carries it after the commit,
     * which is exactly the ordering the server requires */
    xwc_popup_grab(p->menu);
}

static void menu_on_done(struct xwc_win *win, void *ud) {
    struct panel *p = ud;
    (void)win;
    /* popup_done: the compositor dismissed us (press outside). This
     * runs from inside the popup's own event handler; destroying the
     * proxy there is the safe established pattern (the tasklist does
     * the same) — and the panel's own button handler for the SAME
     * click follows right behind it (event order: popup_done, then
     * the button). The close timestamp suppresses an instant reopen. */
    menu_close(p, "dismissed (outside press or compositor)");
}

static int menu_item_at(struct panel *p, int x, int y) {
    if (x < 0 || x > p->menu_w || y < MENU_PAD)
        return -1;
    int idx = (y - MENU_PAD) / MENU_ITEM_H;
    int iy = MENU_PAD + idx * MENU_ITEM_H;
    if (idx >= g_menu_n_items || y >= iy + MENU_ITEM_H)
        return -1;
    return idx;
}

static void menu_on_button(struct xwc_win *win, uint32_t button, bool down,
                           int x, int y, void *ud) {
    struct panel *p = ud;
    (void)win;
    int idx = menu_item_at(p, x, y);
    trace("menu button %u %s at %d,%d -> item=%d", button,
          down ? "press" : "release", x, y, idx);
    if (down && button == BTN_LEFT && idx >= 0)
        menu_launch(p, idx);
}

static void menu_on_motion(struct xwc_win *win, int x, int y, void *ud) {
    struct panel *p = ud;
    (void)win;
    int idx = menu_item_at(p, x, y);
    if (idx != p->menu_hover) {
        p->menu_hover = idx;
        if (p->menu)
            menu_draw(p);
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
        trace("menu key Escape -> close");
        menu_close(p, "escape key");
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        trace("menu key Enter -> launch hovered item");
        menu_launch(p, p->menu_hover);
    }
}

static void menu_open(struct panel *p) {
    if (p->menu) {
        fprintf(stderr, "xw-panel: start clicked — menu already open; "
                        "toggling closed\n");
        menu_close(p, "start toggle");
        return;
    }
    menu_scan_apps();
    if (g_menu_n_items == 0) {
        fprintf(stderr,
                "xw-panel: start clicked — menu opening aborted: no "
                "applications found ($XDG_DATA_DIRS/applications, "
                "~/.local/share/applications)\n");
        return;
    }
    int h = MENU_PAD * 2 + g_menu_n_items * MENU_ITEM_H;
    fprintf(stderr, "xw-panel: start clicked — menu opening "
                    "(item count=%d, %dx%d popup under the button)\n",
            g_menu_n_items, MENU_W, h);

    struct xwc_callbacks cb = {
        .button = menu_on_button,
        .motion = menu_on_motion,
        .configure = menu_on_configure,
        .close = menu_on_done,
        .key = menu_on_key,
        .ud = p,
    };
    /* anchor rect = the Start button, in bar-surface coordinates */
    p->menu = xwc_popup_create(&p->c, p->layer, EDGE_PAD, 0, LAUNCHER_W,
                               p->bar_h, MENU_W, h, &cb);
    p->menu_hover = -1;
    if (!p->menu)
        fprintf(stderr, "xw-panel: menu popup creation failed\n");
}

static const char *btn_kind_name(int kind) {
    switch (kind) {
    case BTN_LAUNCHER: return "menu/launcher";
    case BTN_WS: return "workspace";
    case BTN_TASK: return "task";
    case BTN_CLOCK: return "clock";
    case BTN_EXIT: return "exit";
    default: return "(none)";
    }
}

static void do_exit_button(struct panel *p) {
    (void)p;
    fprintf(stderr, "xw-panel: exit clicked (ctl exit-dialog, async)\n");
    /* async (forked) round trip: never block the Wayland dispatch on
     * the session manager's poll cycle — the click must keep the bar
     * responsive */
    if (!xw_ctl_send_async("exit-dialog"))
        fprintf(stderr, "xw-panel: exit dialog: cannot spawn the ctl "
                        "round trip\n");
}

static void do_launcher(struct panel *p) {
    /* fallback when no .desktop applications exist at all (a minimal
     * box without an applications dir): the resolved terminal is the
     * one thing the old Start behavior could still do */
    char cmd[320];
    resolve_terminal(p->terminal_cmd, sizeof(p->terminal_cmd));
    snprintf(cmd, sizeof(cmd), "run %s", p->terminal_cmd);
    fprintf(stderr, "xw-panel: start clicked — no applications found, "
                    "launching the fallback terminal (ctl '%s')\n", cmd);
    if (!xw_ctl_send_async(cmd))
        fprintf(stderr, "xw-panel: terminal launch failed (fork)\n");
}

static void on_button(struct xwc_win *win, uint32_t button, bool down, int x,
                      int y, void *ud) {
    struct panel *p = ud;
    if (win)
        p->layer = (struct xwc_layer *)win;
    if (!down)
        return;
    struct btn *b = btn_at(p, x, y);
    trace("button %u %s at %d,%d -> widget=%s", button,
          down ? "press" : "release", x, y,
          b ? btn_kind_name(b->kind) : "(miss)");
    if (!b) {
        trace("button landed on no widget (bar background) — ignored");
        return;
    }
    switch (b->kind) {
    case BTN_LAUNCHER:
        if (button == BTN_LEFT) {
            fprintf(stderr, "xw-panel: start clicked\n");
            if (p->menu) {
                menu_close(p, "start toggle");
            } else if (mono_ms() - p->menu_closed_ms < 250) {
                /* the compositor dismissed the menu with the SAME
                 * press that reached this handler (outside-press
                 * dismissal runs server-side, popup_done lands just
                 * before the button event): this click CLOSES, it
                 * must not immediately reopen the menu */
                trace("start press follows the menu dismissal of the "
                      "same click — reopen suppressed");
            } else {
                menu_open(p);
                if (!p->menu)
                    do_launcher(p); /* fallback: no apps, try terminal */
            }
        }
        break;
    case BTN_WS:
        if (button == BTN_LEFT) {
            trace("activate action=workspace index=%d",
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
    case BTN_EXIT:
        if (button == BTN_LEFT)
            do_exit_button(p);
        break;
    default:
        /* BTN_CLOCK: v0 clock is display-only (no popup) — a documented
         * deviation from xfce4-panel's clock plugin, see ROADMAP.md */
        trace("activate action=clock — display-only in v0 (no popup)");
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
            if (want)
                trace("pointer motion over widget=%s at %d,%d",
                      btn_kind_name(p->btns[i].kind), x, y);
        }
    }
    if (changed)
        p->redraw = true;
}

static void on_configure(struct xwc_win *win, int w, int h, void *ud) {
    struct panel *p = ud;
    if (win)
        p->layer = (struct xwc_layer *)win;
    trace("configure: %dx%d (drawing into a fresh buffer)", w, h);
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
    /* SIGTERM (session shutdown), SIGINT (Ctrl+C on the session's
     * terminal), SIGHUP (terminal gone): all mean "exit cleanly" —
     * the compositor notices the closed connection and reaps the
     * surfaces */
    (void)sig;
    if (g_panel)
        g_panel->quit = true;
}

/* reap the forked ctl round-trip helpers (xw_ctl_send_async). The
 * panel spawns no other children, so a blanket non-blocking reap is
 * exact; without it every launch would leave a zombie until exit. */
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
    g_trace = getenv("XW_PANEL_TRACE") != NULL;
    const char *wd = getenv("WAYLAND_DISPLAY");
    trace("starting: WAYLAND_DISPLAY=%s socket=%s", wd ? wd : "(unset)",
          socket_name ? socket_name : "(default)");
    clock_read(p.clock, sizeof(p.clock));
    /* resolve the terminal ONCE at startup (re-resolved on every click
     * in do_launcher); the env var always wins */
    resolve_terminal(p.terminal_cmd, sizeof(p.terminal_cmd));
    trace("launcher terminal resolved: '%s'", p.terminal_cmd);
    p.bar_h = BAR_H;

    struct sigaction sa = {0};
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGTERM, on_sigterm);
    signal(SIGINT, on_sigterm);
    signal(SIGHUP, on_sigterm);
    /* clear any signal mask inherited across fork+exec: when the panel
     * is launched by a signalfd-using parent (the compositor's own
     * spawner, an in-process test embedding a compositor), the blocked
     * mask survives exec and these handlers would never run */
    {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
    }

    if (xwc_connect(&p.c, socket_name) < 0)
        return 1;
    trace("connected: registry round-trip done (globals bound)");

    p.tl = xwc_tasklist_create(&p.c, on_tasklist_changed, &p);
    p.wsp = xwc_wspaces_create(&p.c, on_wspaces_changed, &p);
    trace("tasklist %s, workspaces %s", p.tl ? "bound" : "unavailable",
          p.wsp ? "bound" : "unavailable");

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
    trace("requesting layer: TOP bar, anchors T|L|R, exclusive %d, fixed "
          "height %d — commit sent, awaiting configure",
          BAR_H, BAR_H);
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
     * compositor driving events (title changes, workspace switches,
     * menu interaction) wakes us immediately */
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

    /* compositor shutdown with the menu open: destroy it before the
     * connection teardown (the popup teardown needs a live display) */
    if (p.menu)
        menu_close(&p, "compositor gone");
    xwc_layer_destroy(p.layer);
    xwc_tasklist_destroy(p.tl);
    xwc_wspaces_destroy(p.wsp);
    xwc_disconnect(&p.c);
    g_panel = NULL;
    return 0;
}
