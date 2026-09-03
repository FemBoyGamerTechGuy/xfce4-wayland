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

/* ---- one-shot registry global capture: bind a named interface ---- */

struct bind_once {
    const char *iface;
    uint32_t name;
    bool found;
};

static void bo_global(void *data, struct wl_registry *r, uint32_t name,
                      const char *iface, uint32_t version) {
    (void)r;
    (void)version;
    struct bind_once *bo = data;
    if (strcmp(iface, bo->iface) == 0) {
        bo->name = name;
        bo->found = true;
    }
}
static void bo_global_remove(void *data, struct wl_registry *r,
                             uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener bo_listener = {
    .global = bo_global,
    .global_remove = bo_global_remove,
};

/* Bind wl_subcompositor on the test client (the xwc wrapper does not
 * pre-bind it). Enumerates a FRESH registry and pumps the in-process
 * loop while the announcements arrive — never a blocking roundtrip,
 * which would deadlock the embedded server. */
void xwt_bind_subcompositor(struct xwt_ctx *t, struct wl_subcompositor **out) {
    *out = NULL;
    struct xwc *c = &t->client;
    struct bind_once bo = {.iface = "wl_subcompositor"};
    struct wl_registry *reg = wl_display_get_registry(c->display);
    wl_registry_add_listener(reg, &bo_listener, &bo);
    wl_display_flush(c->display);
    for (int i = 0; i < 200 && !bo.found; i++)
        xwt_pump(t);
    if (bo.found) {
        *out = wl_registry_bind(reg, bo.name, &wl_subcompositor_interface,
                                1);
        wl_display_flush(c->display);
        xwt_pump(t);
    }
    wl_registry_destroy(reg);
}
