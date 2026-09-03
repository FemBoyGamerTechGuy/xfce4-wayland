/* panel.h — shared types of the xw panel subproject.
 *
 * The panel is one client process built from cooperating modules
 * (applications menu, clock + calendar, workspace pager, taskbar,
 * launchers, action button) that each own a horizontal region of the
 * bar. panel.c owns the layer surface, computes the regions from the
 * metrics, routes input to the module whose region was hit, and runs
 * the clock tick. No module links compositor or session code: windows
 * arrive through wlr-foreign-toplevel + xw-workspace-info, workspaces
 * through ext-workspace, everything else is client-local.
 */
#ifndef PANEL_H
#define PANEL_H

#include "xwc.h"
#include "clients/xw-ctl.h" /* ctl wire (src/clients, via -Isrc) */
#include "panel-apps.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct panel;

/* ------------------------------------------------------------ config */
/* $XDG_CONFIG_HOME/xw-panel/panel.conf (XW_PANEL_CONF overrides), INI
 * sections [panel] [clock] [tasklist] [menu]. Defaults render a sane
 * XFCE-like bar with no file present. */
struct panel_config {
    int height;          /* 0 = auto (derive from the output size) */
    bool bottom;         /* false = top bar */
    char clock_format[64];
    bool clock_seconds;  /* also makes the bar tick every second */
    int tasklist_style;  /* 0 icons+text, 1 icons only, 2 text only */
    bool menu_icons;
    bool tasklist_sort;  /* not yet used; reserved */
    char launchers[512]; /* comma-separated desktop ids */
    char favorites[512]; /* comma-separated desktop ids */
    char icon_theme[64]; /* XW_ICON_THEME override from the file */
};

void panel_config_load(struct panel_config *cfg);
/* reset to the built-in defaults (no file) */
void panel_config_defaults(struct panel_config *cfg);
/* parse one INI line (testing hook); returns false on syntax junk */
bool panel_config_line(struct panel_config *cfg, const char *line);

/* ------------------------------------------------------------ metrics */
/* All values are LOGICAL pixels, derived from the bar height H and
 * the active font raster. DPI awareness: H itself derives from the
 * output's logical size, and a compositor-side scale (wl_output
 * scale) shrinks the logical size proportionally — so the panel grows
 * with the physical screen without any hardcoded magic numbers. */
struct panel_metrics {
    int H;          /* bar height */
    int edge;       /* padding from the screen edge */
    int gap;        /* gap between widgets/regions */
    int btn_pad_x;  /* horizontal padding inside buttons */
    int font_h;     /* active font line height */
    int font_ascent;
    int icon;       /* icon cell inside buttons */
    bool big_font;  /* the 24 px raster is active */
};

struct panel_metrics panel_metrics_for(int height_cfg, int output_h);

/* --------------------------------------------------------- shared core */

/* diagnostics: $XW_PANEL_TRACE=1 (the core enables it at startup) */
void panel_trace_enable(bool on);
void panel_trace(const char *fmt, ...);
int64_t panel_mono_ms(void);

/* async ctl round trip with the panel's logging; never blocks the
 * dispatch loop (forks, like every menu/exit action) */
bool panel_ctl_send(const char *cmd);

/* module structs (private to their files, referenced opaquely here) */
struct pm_menu;
struct pm_clock;
struct pm_pager;
struct pm_tasks;

struct panel {
    struct xwc c;
    struct xwc_layer *layer;
    struct xwc_tasklist *tl;
    struct xwc_wspaces *wsp;
    struct xwapp_db apps;

    struct panel_config cfg;
    struct panel_metrics m;

    /* horizontal regions, recomputed by layout() */
    int x_start, w_start; /* start button + launchers */
    int x_tasks, w_tasks; /* taskbar (consumes the free middle) */
    int x_right, w_right; /* pager | clock | exit block, right-aligned */

    char clock[48];    /* formatted clock string */
    int bar_w, bar_h;  /* current surface geometry */
    bool redraw;
    bool quit;

    char terminal_cmd[256]; /* fallback launcher resolution */

    /* module state */
    struct pm_menu *menu;
    struct pm_clock *clockm;
    struct pm_pager *pager;
    struct pm_tasks *tasks;
};

/* ----------------------------------------------------- module helpers */
/* text with the active font raster */
int panel_text_width(const struct panel *p, const char *s);
int panel_draw_text(const struct panel *p, uint32_t *pix, int stride, int w,
                    int h, int x, int y, const char *s, uint32_t color);

/* the clock format engine (panel-clock.c): strftime-lite with an
 * ASCII-safe token set (%a %b %d %m %H %I %M %S %p %y %Y %%) */
void panel_clock_format(const struct panel_config *cfg, char *buf, size_t n);
/* clock button pressed: open/toggle the calendar popup (panel-clock.c) */
void pm_clock_click(struct panel *p, int ax, int ay, int aw, int ah);
/* calendar popup teardown (compositor/panel shutdown) */
void pm_clock_shutdown(struct panel *p);

#endif /* PANEL_H */
