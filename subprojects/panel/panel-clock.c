/* panel-clock.c — the clock widget: format engine + calendar popup.
 *
 * The format engine is a strftime-lite with an ASCII-safe token set
 * (day/month names from our own tables — the bitmap font has no
 * locale glyphs, so strftime output could contain unrenderable
 * characters). Clicking the bar clock opens a one-month calendar
 * popup (anchored to the clock button): month navigation via the
 * header arrows, the mouse wheel or Left/Right keys, today
 * highlighted, other-month days dimmed, Escape/outside dismissal
 * through the popup's seat grab. libc time APIs only — no external
 * calendar stack. */
#include "panel.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wlr-layer-shell-unstable-v1.h"
#include <xkbcommon/xkbcommon-keysyms.h>

static const char *const DAYS[7] = {"Sun", "Mon", "Tue", "Wed",
                                    "Thu", "Fri", "Sat"};
static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
static const char *const MONTHS_FULL[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
/* weekday headers, Monday-first (the XFCE calendar default) */
static const char *const WDAYS[7] = {"Mo", "Tu", "We", "Th", "Fr",
                                     "Sa", "Su"};

void panel_clock_format(const struct panel_config *cfg, char *buf, size_t n) {
    if (!buf || n < 2)
        return;
    time_t now = time(NULL);
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) {
        snprintf(buf, n, "--:--");
        return;
    }
    const char *fmt = cfg && cfg->clock_format[0] ? cfg->clock_format
                                                  : "%a %d %b %H:%M";
    size_t used = 0;
    for (const char *f = fmt; *f && used + 1 < n; f++) {
        if (*f != '%') {
            buf[used++] = *f;
            continue;
        }
        f++;
        char tmp[16];
        switch (*f) {
        case 'a': /* abbreviated weekday */
            snprintf(tmp, sizeof(tmp), "%s", DAYS[tmv.tm_wday]);
            break;
        case 'd':
            snprintf(tmp, sizeof(tmp), "%02d", tmv.tm_mday);
            break;
        case 'b': /* abbreviated month */
            snprintf(tmp, sizeof(tmp), "%s", MONTHS[tmv.tm_mon]);
            break;
        case 'm':
            snprintf(tmp, sizeof(tmp), "%02d", tmv.tm_mon + 1);
            break;
        case 'H':
            snprintf(tmp, sizeof(tmp), "%02d", tmv.tm_hour);
            break;
        case 'I': { /* 12-hour */
            int h = tmv.tm_hour % 12;
            if (h == 0)
                h = 12;
            snprintf(tmp, sizeof(tmp), "%02d", h);
            break;
        }
        case 'M':
            snprintf(tmp, sizeof(tmp), "%02d", tmv.tm_min);
            break;
        case 'S':
            snprintf(tmp, sizeof(tmp), "%02d", tmv.tm_sec);
            break;
        case 'p':
            snprintf(tmp, sizeof(tmp), "%s", tmv.tm_hour < 12 ? "AM" : "PM");
            break;
        case 'y':
            snprintf(tmp, sizeof(tmp), "%02d", tmv.tm_year % 100);
            break;
        case 'Y':
            snprintf(tmp, sizeof(tmp), "%d", 1900 + tmv.tm_year);
            break;
        case '%':
            snprintf(tmp, sizeof(tmp), "%%");
            break;
        case 0:
            buf[used] = 0;
            return;
        default: /* unknown token: kept verbatim */
            snprintf(tmp, sizeof(tmp), "%%%c", *f ? *f : '?');
            break;
        }
        size_t tl = strlen(tmp);
        if (used + tl >= n)
            break;
        memcpy(buf + used, tmp, tl);
        used += tl;
    }
    buf[used] = 0;
}

/* ------------------------------------------------------------ calendar */

#define CAL_CELL_W 30
#define CAL_CELL_H 26
#define CAL_W (8 + 7 * CAL_CELL_W)
#define CAL_HEADER_H 31
#define CAL_WDAY_H 20
#define CAL_H (4 + CAL_HEADER_H + CAL_WDAY_H + 6 * CAL_CELL_H + 4)

#define COL_CAL_BG 0xff262b33
#define COL_CAL_BORDER 0xff3c4454
#define COL_CAL_HOVER 0xff3584e4
#define COL_CAL_TODAY 0xff3584e4
#define COL_CAL_TEXT 0xffe6e6e6
#define COL_CAL_DIM 0xff707a86
#define COL_CAL_NAV_HOVER 0xff3b4252

static struct {
    struct xwc_popup *popup; /* NULL = closed */
    struct panel *p;
    int month, year;  /* the month being viewed (1..12, full year) */
    int hover_cell;   /* day grid cell 0..41, -1 = none */
    int hover_nav;    /* 0 = prev arrow, 1 = next arrow, -1 = none */
    int64_t closed_ms; /* dismissal timestamp (toggle guard) */
} g_cal;

static int days_in_month(int y, int m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return d[m - 1];
}

/* weekday (Monday = 0) of the 1st of the month */
static int first_weekday(int y, int m) {
    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = 1;
    mktime(&tmv);
    return (tmv.tm_wday + 6) % 7;
}

static void month_step(int delta) {
    g_cal.month += delta;
    if (g_cal.month < 1) {
        g_cal.month = 12;
        g_cal.year--;
    } else if (g_cal.month > 12) {
        g_cal.month = 1;
        g_cal.year++;
    }
}

/* today in the viewed month? returns the day (1..31) or 0 */
static int today_in_view(void) {
    time_t now = time(NULL);
    struct tm tmv;
    if (!localtime_r(&now, &tmv))
        return 0;
    if (tmv.tm_year + 1900 != g_cal.year || tmv.tm_mon + 1 != g_cal.month)
        return 0;
    return tmv.tm_mday;
}

/* ------------------------------------------------------------ drawing */

static void nav_triangle(uint32_t *pix, int stride, int w, int h, int cx,
                         int cy, int size, bool left, uint32_t color) {
    /* rows grow away from the tip: a left-pointing triangle has its
     * tip at (cx, cy) and widens downward-right */
    for (int j = 0; j < size; j++) {
        int run = size - j;
        if (left)
            xwc_fill_rect(pix, stride, w, h, cx, cy + j, run, 1, color);
        else
            xwc_fill_rect(pix, stride, w, h, cx - run + 1, cy + j, run, 1,
                          color);
    }
}

static void cal_draw(void) {
    struct panel *p = g_cal.p;
    if (!p || !g_cal.popup)
        return;
    int stride = 0;
    uint32_t *pix = xwc_popup_pixels(g_cal.popup, &stride);
    if (!pix || stride < 1)
        return;
    int w = stride, h = CAL_H;
    xwc_draw_box(pix, stride, w, h, 0, 0, w, h, COL_CAL_BG, COL_CAL_BORDER);

    /* header: [<] Month YYYY [>] */
    int hy = 2, hh = CAL_HEADER_H;
    int nav_cy = hy + hh / 2;
    int nav_size = 7;
    int prev_cx = 16, next_cx = w - 16;
    if (g_cal.hover_nav == 0)
        xwc_fill_rect(pix, stride, w, h, prev_cx - nav_size - 4,
                      nav_cy - nav_size - 4, 2 * nav_size + 8,
                      2 * nav_size + 6, COL_CAL_NAV_HOVER);
    if (g_cal.hover_nav == 1)
        xwc_fill_rect(pix, stride, w, h, next_cx - nav_size - 4,
                      nav_cy - nav_size - 4, 2 * nav_size + 8,
                      2 * nav_size + 6, COL_CAL_NAV_HOVER);
    nav_triangle(pix, stride, w, h, prev_cx, nav_cy - nav_size / 2, nav_size,
                 true, COL_CAL_TEXT);
    nav_triangle(pix, stride, w, h, next_cx, nav_cy - nav_size / 2, nav_size,
                 false, COL_CAL_TEXT);

    char title[48];
    snprintf(title, sizeof(title), "%s %d", MONTHS_FULL[g_cal.month - 1],
             g_cal.year);
    panel_draw_text(p, pix, stride, w, h,
                    (w - panel_text_width(p, title)) / 2,
                    hy + (hh - p->m.font_h) / 2 + 1, title, COL_CAL_TEXT);

    /* weekday headers */
    int wy = 2 + CAL_HEADER_H;
    for (int i = 0; i < 7; i++) {
        int tx = 8 + i * CAL_CELL_W +
                 (CAL_CELL_W - panel_text_width(p, WDAYS[i])) / 2;
        panel_draw_text(p, pix, stride, w, h, tx,
                        wy + (CAL_WDAY_H - p->m.font_h) / 2 + 1, WDAYS[i],
                        COL_CAL_DIM);
    }

    /* day grid: 6 rows x 7 columns */
    int first = first_weekday(g_cal.year, g_cal.month);
    int dim = days_in_month(g_cal.year, g_cal.month);
    int prev_dim = days_in_month(
        g_cal.month == 1 ? g_cal.year - 1 : g_cal.year,
        g_cal.month == 1 ? 12 : g_cal.month - 1);
    int today = today_in_view();
    int gy = 2 + CAL_HEADER_H + CAL_WDAY_H;
    for (int cell = 0; cell < 42; cell++) {
        int col = cell % 7, row = cell / 7;
        int day = cell - first + 1;
        bool other = false;
        if (day < 1) {
            day = prev_dim + day;
            other = true;
        } else if (day > dim) {
            day -= dim;
            other = true;
        }
        int cx = 8 + col * CAL_CELL_W;
        int cy = gy + row * CAL_CELL_H;
        if (!other && day == today) {
            xwc_draw_box(pix, stride, w, h, cx + 2, cy + 1, CAL_CELL_W - 4,
                         CAL_CELL_H - 2, COL_CAL_TODAY, 0xff88b0ef);
        } else if (g_cal.hover_cell == cell) {
            xwc_fill_rect(pix, stride, w, h, cx + 2, cy + 1, CAL_CELL_W - 4,
                          CAL_CELL_H - 2, COL_CAL_HOVER);
        }
        char ds[12];
        snprintf(ds, sizeof(ds), "%d", day);
        panel_draw_text(
            p, pix, stride, w, h,
            cx + (CAL_CELL_W - panel_text_width(p, ds)) / 2,
            cy + (CAL_CELL_H - p->m.font_h) / 2 + 1, ds,
            other ? COL_CAL_DIM
                  : (!other && day == today ? 0xffffffff : COL_CAL_TEXT));
    }
    xwc_popup_commit(g_cal.popup);
}

/* ------------------------------------------------------------- events */

static void cal_close(struct panel *p, const char *why) {
    (void)p;
    if (!g_cal.popup)
        return;
    panel_trace("calendar closing (%s)", why);
    xwc_popup_destroy(g_cal.popup);
    g_cal.popup = NULL;
    g_cal.hover_cell = -1;
    g_cal.hover_nav = -1;
    g_cal.closed_ms = panel_mono_ms();
}

static void cal_on_configure(struct xwc_win *win, int w, int h, void *ud) {
    (void)ud;
    struct xwc_popup *popup = (struct xwc_popup *)win;
    if (popup)
        g_cal.popup = popup;
    (void)w;
    (void)h;
    panel_trace("calendar surface created: %dx%d (viewing %d-%02d)", w, h,
                g_cal.year, g_cal.month);
    cal_draw();
    xwc_popup_grab(g_cal.popup);
}

static void cal_on_done(struct xwc_win *win, void *ud) {
    struct panel *p = ud;
    (void)win;
    cal_close(p, "dismissed (outside press or compositor)");
}

/* nav arrow hit areas */
static int nav_at(int x, int y) {
    if (y < 2 || y > 2 + CAL_HEADER_H)
        return -1;
    if (x >= 2 && x <= 32)
        return 0; /* prev */
    if (x >= CAL_W - 32 && x < CAL_W - 2)
        return 1; /* next */
    return -1;
}

static int cell_at(int x, int y) {
    int gy = 2 + CAL_HEADER_H + CAL_WDAY_H;
    if (x < 8 || x >= 8 + 7 * CAL_CELL_W || y < gy)
        return -1;
    int col = (x - 8) / CAL_CELL_W;
    int row = (y - gy) / CAL_CELL_H;
    if (row >= 6 || col >= 7)
        return -1;
    return row * 7 + col;
}

#define BTN_LEFT 0x110

static void cal_on_button(struct xwc_win *win, uint32_t button, bool down,
                          int x, int y, void *ud) {
    struct panel *p = ud;
    (void)win;
    if (!down || button != BTN_LEFT)
        return;
    int nav = nav_at(x, y);
    if (nav >= 0) {
        panel_trace("calendar nav %s", nav ? "next" : "prev");
        month_step(nav ? 1 : -1);
        cal_draw();
        return;
    }
    int cell = cell_at(x, y);
    if (cell >= 0)
        panel_trace("calendar day pressed (cell %d) — display-only", cell);
    (void)p;
}

static void cal_on_motion(struct xwc_win *win, int x, int y, void *ud) {
    (void)win;
    (void)ud;
    int nav = nav_at(x, y);
    int cell = nav >= 0 ? -1 : cell_at(x, y);
    if (nav != g_cal.hover_nav || cell != g_cal.hover_cell) {
        g_cal.hover_nav = nav;
        g_cal.hover_cell = cell;
        if (g_cal.popup)
            cal_draw();
    }
}

static void cal_on_axis(struct xwc_win *win, uint32_t axis, double value,
                        void *ud) {
    (void)win;
    (void)ud;
    if (axis != 0 || value == 0)
        return;
    month_step(value > 0 ? 1 : -1);
    if (g_cal.popup)
        cal_draw();
}

static void cal_on_key(struct xwc_win *win, uint32_t keycode, bool down,
                       xkb_keysym_t sym, uint32_t mods, void *ud) {
    struct panel *p = ud;
    (void)win;
    (void)keycode;
    (void)mods;
    if (!down)
        return;
    if (sym == XKB_KEY_Escape) {
        cal_close(p, "escape key");
        return;
    }
    if (sym == XKB_KEY_Left) {
        month_step(-1);
        cal_draw();
    } else if (sym == XKB_KEY_Right) {
        month_step(1);
        cal_draw();
    }
}

void pm_clock_click(struct panel *p, int ax, int ay, int aw, int ah) {
    if (g_cal.popup) {
        cal_close(p, "clock toggle");
        return;
    }
    if (g_cal.closed_ms && panel_mono_ms() - g_cal.closed_ms < 250) {
        panel_trace("clock press follows the calendar dismissal of the "
                    "same click — reopen suppressed");
        return;
    }
    g_cal.p = p;
    time_t now = time(NULL);
    struct tm tmv;
    if (!localtime_r(&now, &tmv))
        return;
    g_cal.month = tmv.tm_mon + 1;
    g_cal.year = tmv.tm_year + 1900;
    g_cal.hover_cell = -1;
    g_cal.hover_nav = -1;
    panel_trace("clock clicked — calendar opening (%s %d, %dx%d popup)",
                MONTHS_FULL[g_cal.month - 1], g_cal.year, CAL_W, CAL_H);

    struct xwc_callbacks cb = {
        .button = cal_on_button,
        .motion = cal_on_motion,
        .axis = cal_on_axis,
        .configure = cal_on_configure,
        .close = cal_on_done,
        .key = cal_on_key,
        .ud = p,
    };
    g_cal.popup = xwc_popup_create_dir(&p->c, p->layer, ax, ay, aw, ah, CAL_W,
                                       CAL_H, &cb, p->cfg.bottom);
    if (!g_cal.popup)
        fprintf(stderr, "xw-panel: calendar popup creation failed\n");
}

void pm_clock_shutdown(struct panel *p) {
    cal_close(p, "compositor gone");
}
