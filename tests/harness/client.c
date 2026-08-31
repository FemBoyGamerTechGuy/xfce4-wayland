/* client.c — shared test client helpers. */
#include "xwtest.h"

static void solid_configure(struct xwc_win *w, int width, int height, void *ud) {
    (void)width;
    (void)height;
    uint32_t color = *(uint32_t *)ud;
    int ww = 0, wh = 0, stride = 0;
    xwc_win_size(w, &ww, &wh);
    uint32_t *pix = xwc_win_pixels(w, &stride);
    if (!pix || ww < 1 || wh < 1)
        return;
    xwc_fill_rect(pix, stride, ww, wh, 0, 0, ww, wh, color);
    xwc_win_commit(w);
}

struct xwc_win *xwt_window_solid(struct xwt_ctx *t, uint32_t color, int w,
                                 int h, const char *title) {
    static uint32_t colors[16];
    static int n_colors;
    uint32_t *slot = &colors[n_colors++ % 16];
    *slot = color;
    struct xwc_callbacks cb = {0};
    cb.configure = solid_configure;
    cb.ud = slot;
    struct xwc_win *win = xwc_win_create(&t->client, &cb, title, "xw.test", w,
                                         h);
    xwt_pump(t);
    xwt_pump(t);
    return win;
}
