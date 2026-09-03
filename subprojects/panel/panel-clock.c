/* panel-clock.c — the clock widget: format engine + click hook.
 *
 * The format engine is a strftime-lite with an ASCII-safe token set
 * (day/month names from our own tables — the bitmap font has no
 * locale glyphs, so strftime output could contain unrenderable
 * characters). The calendar popup arrives with the clock module v2;
 * v1's click hook logs and no-ops, keeping the click path testable. */
#include "panel.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *const DAYS[7] = {"Sun", "Mon", "Tue", "Wed",
                                    "Thu", "Fri", "Sat"};
static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};

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

/* v1 click hook: the calendar popup lands in this file next */
void pm_clock_click(struct panel *p, int ax, int ay, int aw, int ah) {
    (void)ax;
    (void)ay;
    (void)aw;
    (void)ah;
    panel_trace("clock clicked (calendar popup: next stage)");
    (void)p;
}

void pm_clock_shutdown(struct panel *p) {
    (void)p; /* no popup yet */
}
