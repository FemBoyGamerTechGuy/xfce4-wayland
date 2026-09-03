/* panel-util.c — helpers shared by every panel module (trace, time,
 * ctl wire). Lives in libpanelcore.a so the test suite links it too. */
#include "panel.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool g_trace;
void panel_trace_enable(bool on) { g_trace = on; }

void panel_trace(const char *fmt, ...) {
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

int64_t panel_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct panel_metrics panel_metrics_for(int height_cfg, int output_h) {
    struct panel_metrics m;
    memset(&m, 0, sizeof(m));
    int H;
    if (height_cfg > 0) {
        H = height_cfg;
        if (H > 200)
            H = 200;
    } else {
        /* auto: ~1/32 of the logical height, clamped to a comfortable
         * band: 30 px at 720p/1080p-class logical sizes, 45 at 1440p,
         * 52 (cap) at 4K without compositor scaling */
        H = (output_h + 16) / 32;
        if (H < 30)
            H = 30;
        if (H > 52)
            H = 52;
    }
    m.H = H;
    m.edge = 3;
    m.gap = 4;
    m.btn_pad_x = 8;
    m.big_font = H > 40;
    m.font_h = m.big_font ? XWC_LINE2_H : XWC_LINE_H;
    m.font_ascent = m.big_font ? XWC_ASCENT2 : XWC_ASCENT;
    m.icon = H - 12;
    if (m.icon < 16)
        m.icon = 16;
    return m;
}

int panel_text_width(const struct panel *p, const char *s) {
    return p->m.big_font ? xwc_text_width2(s) : xwc_text_width(s);
}

int panel_draw_text(const struct panel *p, uint32_t *pix, int stride, int w,
                    int h, int x, int y, const char *s, uint32_t color) {
    return p->m.big_font
               ? xwc_draw_text2(pix, stride, w, h, x, y, s, color)
               : xwc_draw_text(pix, stride, w, h, x, y, s, color);
}

bool panel_ctl_send(const char *cmd) {
    if (!xw_ctl_send_async(cmd)) {
        fprintf(stderr, "xw-panel: ctl round trip failed (fork): '%s'\n",
                cmd);
        return false;
    }
    return true;
}
