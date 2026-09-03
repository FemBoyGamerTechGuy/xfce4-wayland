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

/* -------------------------------------------------- UTF-8-safe fitting */

/* decode the byte length of the complete UTF-8 sequence starting at s
 * (bounded by maxlen); returns 1 for malformed/lead bytes so the caller
 * can never split a multibyte character in half */
static size_t utf8_seq_len(const char *s, size_t maxlen) {
    unsigned char c = (unsigned char)*s;
    if (maxlen < 1 || c < 0x80 || (c & 0xC0) == 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return maxlen >= 2 ? 2 : 1;
    if ((c & 0xF0) == 0xE0)
        return maxlen >= 3 && ((unsigned char)s[1] & 0xC0) == 0x80 &&
                       ((unsigned char)s[2] & 0xC0) == 0x80
                   ? 3
                   : 1;
    if ((c & 0xF8) == 0xF0)
        return maxlen >= 4 && ((unsigned char)s[1] & 0xC0) == 0x80 &&
                       ((unsigned char)s[2] & 0xC0) == 0x80 &&
                       ((unsigned char)s[3] & 0xC0) == 0x80
                   ? 4
                   : 1;
    return 1;
}

/* UTF-8-safe string copy: never splits a multibyte sequence */
static void utf8_copy(char *dst, size_t n, const char *src, size_t srclen) {
    size_t di = 0;
    for (size_t si = 0; si < srclen && di + 1 < n;) {
        size_t l = utf8_seq_len(src + si, srclen - si);
        if (l > n - 1 - di)
            break;
        for (size_t k = 0; k < l; k++)
            dst[di + k] = src[si + k];
        di += l;
        si += l;
    }
    dst[di] = 0;
}

void panel_text_fit(const struct panel *p, char *dst, size_t n,
                    const char *src, int room) {
    if (n < 5) {
        if (n > 0)
            dst[0] = 0;
        return;
    }
    size_t srclen = strlen(src);
    utf8_copy(dst, n, src, srclen);
    /* peel complete codepoints off the end until it fits */
    size_t len = strlen(dst);
    while (len > 1 && panel_text_width(p, dst) > room) {
        size_t cut = 1;
        while (cut < len &&
               ((unsigned char)dst[len - cut] & 0xC0) == 0x80)
            cut++;
        /* cut now includes the lead byte of the last sequence */
        len -= cut;
        dst[len] = 0;
    }
    /* a real ellipsis when truncation happened (the glyph is in the
     * punctuation table; the old code cut raw bytes and could split a
     * multibyte letter, visibly losing characters) */
    if (len < srclen && strcmp(dst, src) != 0) {
        const char ell[] = "\xe2\x80\xa6"; /* U+2026 HORIZONTAL ELLIPSIS */
        size_t el = sizeof(ell) - 1;
        while (len > 1 && (len + el + 1 > n ||
                           panel_text_width(p, dst) +
                                   panel_text_width(p, ell) >
                               room)) {
            size_t cut = 1;
            while (cut < len && ((unsigned char)dst[len - cut] & 0xC0) == 0x80)
                cut++;
            len -= cut;
            dst[len] = 0;
        }
        if (len + el + 1 <= n) {
            memcpy(dst + len, ell, el + 1);
        }
    }
}

bool panel_ctl_send(const char *cmd) {
    if (!xw_ctl_send_async(cmd)) {
        fprintf(stderr, "xw-panel: ctl round trip failed (fork): '%s'\n",
                cmd);
        return false;
    }
    return true;
}
