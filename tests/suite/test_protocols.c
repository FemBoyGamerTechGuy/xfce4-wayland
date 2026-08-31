/* test_protocols.c — desktop-integration protocol coverage:
 * layer-shell (panel geometry + exclusive keyboard), xdg popups
 * (positioning + outside-click dismissal), wl_data_device selection
 * (clipboard owner tracking), wlr-foreign-toplevel management (announce,
 * get_toplevel, title events, handle activation) and xdg-activation
 * (token focus switch + single-use policy).
 *
 * Raw wayland protocol objects are driven directly (a second registry
 * binds the protocols libxwcl does not wrap) next to the white-box
 * server-side assertions.
 */
#include "xwtest.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "xdg-shell.h"
#include "xdg-activation.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"

/* -------------------------------------------------------- white-box helpers */

static struct xw_layer_surface *first_layer(struct xw_compositor *c, int idx) {
    struct xw_layer_surface *ls;
    if (wl_list_empty(&c->wm->layers[idx]))
        return NULL;
    ls = wl_container_of(c->wm->layers[idx].next, ls, link);
    return ls;
}

static bool layer_mapped(struct xwt_ctx *t, int idx) {
    struct xw_layer_surface *ls = first_layer(t->comp, idx);
    return ls && ls->mapped;
}

static uint32_t pixel_at(struct xwt_ctx *t, int x, int y) {
    int w = 0, h = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &w, &h);
    if (!pix || x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    return pix[y * w + x];
}

static struct xw_window *find_window(struct xwt_ctx *t, const char *title) {
    struct xw_window *w;
    wl_list_for_each(w, &t->comp->wm->windows, link) {
        if (strcmp(w->title, title) == 0)
            return w;
    }
    return NULL;
}

#define PANEL_COLOR 0xff27405a

/* fills the layer with a solid color and commits (configure callback) */
static void solid_layer_configure(struct xwc_win *win, int w, int h, void *ud) {
    (void)ud;
    struct xwc_layer *l = (struct xwc_layer *)win;
    int stride = 0;
    uint32_t *pix = xwc_layer_pixels(l, &stride);
    if (!pix || w < 1 || h < 1)
        return;
    xwc_fill_rect(pix, stride, w, h, 0, 0, w, h, PANEL_COLOR);
    xwc_layer_commit(l);
}

/* ------------------------------------------------------ layer-shell tests */

static void test_layer_shell_panel(struct xwt_ctx *t) {
    struct xwc_callbacks cb = {0};
    cb.configure = solid_layer_configure;
    struct xwc_layer *panel = xwc_layer_create(
        &t->client, &cb, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        32, 1280, 32);
    XWT_ASSERT(panel);
    XWT_WAIT(t, layer_mapped(t, 2));

    struct xw_layer_surface *ls = first_layer(t->comp, 2);
    XWT_CHECK(ls->w == 1280 && ls->h == 32,
              "panel size %dx%d (want 1280x32)", ls->w, ls->h);
    XWT_CHECK(ls->x == 0 && ls->y == 0, "panel at %d,%d (want 0,0)", ls->x,
              ls->y);

    /* the exclusive zone shrinks the usable area below the panel */
    struct xw_output *o;
    o = wl_container_of(t->comp->outputs.next, o, link);
    XWT_CHECK(o->usable.y >= 32, "usable area starts below the panel (y=%d)",
              o->usable.y);
    XWT_CHECK(o->usable.h <= o->height - 32,
              "usable height reduced (%d of %d)", o->usable.h, o->height);

    /* and the panel actually renders */
    XWT_WAIT(t, pixel_at(t, 5, 10) == PANEL_COLOR);
    XWT_CHECK(pixel_at(t, 5, 10) == PANEL_COLOR, "panel pixels rendered");

    /* set_layer moves the surface between layer lists (v2 request) */
    xwc_layer_set_layer(panel, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    xwc_sync(&t->client);
    xwt_pump(t);
    XWT_CHECK(first_layer(t->comp, 2) == NULL, "panel left the top layer");
    XWT_CHECK(layer_mapped(t, 1), "panel moved to the bottom layer");
    XWT_CHECK(pixel_at(t, 5, 10) == PANEL_COLOR, "panel still renders");

    xwc_layer_destroy(panel);
    xwt_pump(t);
    XWT_CHECK(first_layer(t->comp, 1) == NULL, "panel layer removed");
}

static void test_layer_shell_exclusive_keyboard(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, 0xff445566, 300, 200, "Under");
    XWT_ASSERT(win);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    struct xw_window *w = find_window(t, "Under");
    XWT_ASSERT(w);

    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);
    XWT_WAIT(t, seat->kb_focus == w->surface);
    XWT_CHECK(seat->kb_focus == w->surface, "window owns the keyboard");

    /* modal overlay covering the output, exclusive keyboard */
    struct xwc_callbacks cb = {0};
    cb.configure = solid_layer_configure;
    struct xwc_layer *overlay = xwc_layer_create(
        &t->client, &cb, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        0, 0, 0);
    XWT_ASSERT(overlay);
    xwc_layer_set_keyboard(overlay, 1);
    XWT_WAIT(t, layer_mapped(t, 3));
    struct xw_layer_surface *ls = first_layer(t->comp, 3);
    XWT_ASSERT(ls);
    XWT_WAIT(t, seat->kb_focus == ls->surface);
    XWT_CHECK(seat->kb_focus == ls->surface, "overlay takes keyboard focus");

    /* clicking the window cannot steal focus from the modal overlay */
    xw_compositor_inject_pointer_motion(t->comp, w->x + w->w / 2,
                                        w->y + w->h / 2);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    xwt_pump(t);
    XWT_CHECK(seat->kb_focus == ls->surface,
              "exclusive layer keeps keyboard focus");

    /* teardown releases the keyboard (save the surface pointer: the
     * layer struct itself is freed by the destroy) */
    struct xw_surface *ls_surface = ls->surface;
    xwc_layer_destroy(overlay);
    xwt_pump(t);
    XWT_CHECK(seat->kb_focus != ls_surface,
              "focus released when the overlay dies");
}

/* ------------------------------------------------------------- popup test */

struct popup_state {
    bool configured, done;
    int x, y, w, h;
};

struct popup_ctx {
    struct wl_surface *surface;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
};

static void popup_configure_ev(void *data, struct xdg_popup *p, int32_t x,
                               int32_t y, int32_t w, int32_t h) {
    struct popup_state *st = data;
    st->configured = true;
    st->x = x;
    st->y = y;
    st->w = w;
    st->h = h;
    (void)p;
}

static void popup_done_ev(void *data, struct xdg_popup *p) {
    struct popup_state *st = data;
    st->done = true;
    (void)p;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure_ev,
    .popup_done = popup_done_ev,
};

static void popup_surface_configure(void *data, struct xdg_surface *xs,
                                    uint32_t serial) {
    struct popup_ctx *pc = data;
    xdg_surface_ack_configure(xs, serial);
    if (pc->buffer) {
        wl_surface_attach(pc->surface, pc->buffer, 0, 0);
        wl_surface_damage(pc->surface, 0, 0, 120, 60);
        wl_surface_commit(pc->surface);
    }
}

static const struct xdg_surface_listener popup_xs_listener = {
    .configure = popup_surface_configure,
};

static void test_popup_positioning(struct xwt_ctx *t) {
    struct xwc_win *parent = xwt_window_solid(t, 0xff708090, 300, 200,
                                              "PopParent");
    XWT_ASSERT(parent);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    struct xw_window *pw = find_window(t, "PopParent");
    XWT_ASSERT(pw);

    struct wl_surface *surf =
        wl_compositor_create_surface((struct wl_compositor *)t->client.compositor);
    struct xdg_surface *xs =
        xdg_wm_base_get_xdg_surface((struct xdg_wm_base *)t->client.wm_base, surf);
    struct xdg_positioner *pos =
        xdg_wm_base_create_positioner((struct xdg_wm_base *)t->client.wm_base);
    xdg_positioner_set_size(pos, 120, 60);
    xdg_positioner_set_anchor_rect(pos, 40, 30, 20, 10);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);

    struct popup_state st = {0};
    struct popup_ctx pc = {.surface = surf};
    struct xdg_popup *popup =
        xdg_surface_get_popup(xs, (struct xdg_surface *)xwc_win_xdg_surface(parent), pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(popup, &popup_listener, &st);
    xdg_surface_add_listener(xs, &popup_xs_listener, &pc);

    /* shm buffer for the popup content */
    int fd = memfd_create("xwt-popup", MFD_CLOEXEC);
    XWT_ASSERT(fd >= 0);
    size_t sz = (size_t)120 * 60 * 4;
    XWT_ASSERT(ftruncate(fd, (off_t)sz) == 0);
    pc.pool = wl_shm_create_pool((struct wl_shm *)t->client.shm, fd, (int32_t)sz);
    close(fd);
    XWT_ASSERT(pc.pool);
    pc.buffer = wl_shm_pool_create_buffer(pc.pool, 0, 120, 60, 120 * 4,
                                          WL_SHM_FORMAT_XRGB8888);
    XWT_ASSERT(pc.buffer);

    /* initial (bufferless) commit triggers the configure cycle */
    wl_surface_commit(surf);
    xwc_sync(&t->client);
    xwt_pump(t);

    XWT_CHECK(!wl_list_empty(&t->comp->popups), "popup exists server-side");
    struct xw_popup *p = NULL;
    if (!wl_list_empty(&t->comp->popups))
        p = wl_container_of(t->comp->popups.next, p, link);
    XWT_ASSERT(p);
    XWT_CHECK(p->mapped, "popup mapped");
    XWT_CHECK(p->w == 120 && p->h == 60, "popup size %dx%d", p->w, p->h);

    /* anchor rect (40,30,20,10) BOTTOM_LEFT point (40,40) + BOTTOM_RIGHT
     * gravity (popup grows right/down from the point):
     * x = parent.x + 40, y = parent.y + 40 */
    int want_x = pw->x + 40;
    int want_y = pw->y + 40;
    XWT_CHECK(p->anchor_x == want_x && p->anchor_y == want_y,
              "popup position %d,%d (want %d,%d)", p->anchor_x, p->anchor_y,
              want_x, want_y);
    XWT_CHECK(st.configured && st.x == 40 && st.y == 40,
              "client configure parent-relative %d,%d", st.x, st.y);

    /* press outside the popup dismisses it (menu parity) */
    xw_compositor_inject_pointer_motion(t->comp, 1270, 710);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    xwt_pump(t);
    XWT_CHECK(!p->mapped, "popup dismissed by outside click");
    XWT_CHECK(st.done, "client received popup_done");

    xdg_popup_destroy(popup);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_buffer_destroy(pc.buffer);
    wl_shm_pool_destroy(pc.pool);
    xwt_pump(t);
}

/* --------------------------------------------------------- clipboard test */

static void test_clipboard_selection(struct xwt_ctx *t) {
    struct wl_data_device_manager *ddm =
        (struct wl_data_device_manager *)t->client.ddm;
    XWT_ASSERT(ddm);

    struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
    XWT_ASSERT(src);
    wl_data_source_offer(src, "text/plain");

    struct wl_data_device *dev =
        wl_data_device_manager_get_data_device(ddm, (struct wl_seat *)t->client.seat);
    XWT_ASSERT(dev);

    wl_data_device_set_selection(dev, src, 0);
    xwc_sync(&t->client);
    xwt_pump(t);

    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_ASSERT(seat);
    XWT_CHECK(seat->selection_source != NULL, "selection recorded");
    XWT_CHECK(seat->selection_source &&
                  wl_resource_get_id(seat->selection_source) ==
                      wl_proxy_get_id((struct wl_proxy *)src),
              "selection owner is the client's source");
    XWT_CHECK(seat->selection_source && seat->selection_client ==
                  wl_resource_get_client(seat->selection_source),
              "selection client tracked");

    /* clearing the selection */
    wl_data_device_set_selection(dev, NULL, 1);
    xwc_sync(&t->client);
    xwt_pump(t);
    XWT_CHECK(seat->selection_source == NULL, "selection cleared");

    wl_data_device_release(dev);
    wl_data_source_destroy(src);
    xwt_pump(t);
}

/* ------------------------------------------- foreign-toplevel + activation */

struct ft_state {
    struct zwlr_foreign_toplevel_manager_v1 *mgr;
    struct xdg_activation_v1 *act;
    struct zwlr_foreign_toplevel_handle_v1 *handles[8];
    int n_handles;
    char title[128];
    char app_id[128];
    int n_titles;
    char token[64];
    bool token_done;
};

static void h_title_ev(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                       const char *title) {
    struct ft_state *st = data;
    snprintf(st->title, sizeof(st->title), "%s", title ? title : "");
    st->n_titles++;
    (void)h;
}

static void h_app_id_ev(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                        const char *app_id) {
    struct ft_state *st = data;
    snprintf(st->app_id, sizeof(st->app_id), "%s", app_id ? app_id : "");
    (void)h;
}

static void h_state_ev(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                       struct wl_array *state) {
    (void)data;
    (void)h;
    (void)state;
}

static void h_done_ev(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)data;
    (void)h;
}

static void h_closed_ev(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)data;
    (void)h;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = h_title_ev,
    .app_id = h_app_id_ev,
    .state = h_state_ev,
    .done = h_done_ev,
    .closed = h_closed_ev,
};

static void mgr_toplevel_ev(void *data,
                            struct zwlr_foreign_toplevel_manager_v1 *m,
                            struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct ft_state *st = data;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, st);
    if (st->n_handles < 8)
        st->handles[st->n_handles] = handle;
    st->n_handles++;
    (void)m;
}

static void mgr_finished_ev(void *data,
                            struct zwlr_foreign_toplevel_manager_v1 *m) {
    (void)data;
    (void)m;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_listener = {
    .toplevel = mgr_toplevel_ev,
    .finished = mgr_finished_ev,
};

static void reg2_global(void *data, struct wl_registry *r, uint32_t name,
                        const char *iface, uint32_t version) {
    struct ft_state *st = data;
    if (strcmp(iface, "zwlr_foreign_toplevel_manager_v1") == 0) {
        st->mgr = wl_registry_bind(r, name,
                                   &zwlr_foreign_toplevel_manager_v1_interface,
                                   version < 3 ? version : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(st->mgr, &mgr_listener,
                                                      st);
    } else if (strcmp(iface, "xdg_activation_v1") == 0) {
        st->act = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
    }
}

static void reg2_global_remove(void *data, struct wl_registry *r,
                               uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener reg2_listener = {
    .global = reg2_global,
    .global_remove = reg2_global_remove,
};

static void token_done_ev(void *data, struct xdg_activation_token_v1 *tk,
                          const char *token) {
    struct ft_state *st = data;
    snprintf(st->token, sizeof(st->token), "%s", token ? token : "");
    st->token_done = true;
    (void)tk;
}

static const struct xdg_activation_token_v1_listener token_listener = {
    .done = token_done_ev,
};

static void test_foreign_toplevel_and_activation(struct xwt_ctx *t) {
    struct xwc_win *a = xwt_window_solid(t, 0xff3366aa, 240, 160, "Alpha");
    XWT_ASSERT(a);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);

    /* bind manager + activation through a second registry; the bind is
     * buffered during the sync's dispatch, so the announce events follow
     * on the next pump cycles (exactly what a real taskbar sees) */
    struct ft_state st = {0};
    struct wl_registry *reg2 =
        wl_display_get_registry((struct wl_display *)t->client.display);
    wl_registry_add_listener(reg2, &reg2_listener, &st);
    xwc_sync(&t->client);
    XWT_ASSERT(st.mgr && st.act);

    /* binding after the window mapped announces it */
    XWT_WAIT(t, st.n_handles == 1);
    XWT_CHECK(st.n_handles == 1, "existing window announced (handles=%d)",
              st.n_handles);
    XWT_CHECK(strcmp(st.title, "Alpha") == 0, "handle title '%s'", st.title);
    XWT_CHECK(strcmp(st.app_id, "xw.test") == 0, "handle app_id '%s'",
              st.app_id);

    struct xw_window *wa = find_window(t, "Alpha");
    XWT_ASSERT(wa);
    XWT_CHECK(!wl_list_empty(&wa->toplevel_handles),
              "server keeps the handle on the window");

    /* title changes propagate */
    xwc_win_set_title(a, "Alpha-Renamed");
    xwc_sync(&t->client);
    xwt_pump(t);
    XWT_CHECK(strcmp(st.title, "Alpha-Renamed") == 0,
              "title event after rename ('%s')", st.title);
    XWT_CHECK(st.n_titles >= 2, "title events received (%d)", st.n_titles);

    /* a second window gets announced on map */
    struct xwc_win *b = xwt_window_solid(t, 0xffaa6633, 240, 160, "Beta");
    XWT_ASSERT(b);
    XWT_WAIT(t, st.n_handles == 2);
    XWT_CHECK(st.n_handles == 2, "new window announced (%d)", st.n_handles);

    /* handle activation focuses the window (tasklist click) */
    struct xw_window *wb = find_window(t, "Beta");
    XWT_ASSERT(wb);
    zwlr_foreign_toplevel_handle_v1_activate(st.handles[1],
                                             (struct wl_seat *)t->client.seat);
    xwc_sync(&t->client);
    xwt_pump(t);
    XWT_CHECK(t->comp->wm->focused == wb, "handle activation focuses Beta");

    /* xdg-activation: token switches focus to Alpha */
    struct xdg_activation_token_v1 *tk =
        xdg_activation_v1_get_activation_token(st.act);
    XWT_ASSERT(tk);
    xdg_activation_token_v1_add_listener(tk, &token_listener, &st);
    xdg_activation_token_v1_commit(tk);
    xwc_sync(&t->client);
    XWT_CHECK(st.token_done && st.token[0], "activation token issued");
    xdg_activation_v1_activate(st.act, st.token,
                               (struct wl_surface *)xwc_win_surface(a));
    xwc_sync(&t->client);
    xwt_pump(t);
    XWT_CHECK(t->comp->wm->focused == wa, "activation focuses Alpha");

    /* tokens are single-use: replaying must not switch focus */
    xdg_activation_v1_activate(st.act, st.token,
                               (struct wl_surface *)xwc_win_surface(b));
    xwc_sync(&t->client);
    xwt_pump(t);
    XWT_CHECK(t->comp->wm->focused == wa,
              "replayed token is rejected (focus stays on Alpha)");

    /* cleanup: destroy the handle proxies while the objects are still
     * alive server-side, then stop the manager (its resources die with
     * it — a later destroy request would be a protocol error) */
    for (int i = 0; i < st.n_handles && i < 8; i++)
        if (st.handles[i])
            zwlr_foreign_toplevel_handle_v1_destroy(st.handles[i]);
    zwlr_foreign_toplevel_manager_v1_stop(st.mgr);
    /* the generated destroy is a plain wl_proxy_destroy (no request is
     * sent), so it is safe after stop even though the object is gone
     * server-side */
    zwlr_foreign_toplevel_manager_v1_destroy(st.mgr);
    xdg_activation_token_v1_destroy(tk);
    xdg_activation_v1_destroy(st.act);
    wl_registry_destroy(reg2);
    xwt_pump(t);
}

/* ------------------------------------------------------------ registration */

static const struct xwt_test tests[] = {
    {"layer-shell-panel", test_layer_shell_panel},
    {"layer-shell-focus", test_layer_shell_exclusive_keyboard},
    {"popup-positioning", test_popup_positioning},
    {"clipboard-selection", test_clipboard_selection},
    {"foreign-toplevel-activation", test_foreign_toplevel_and_activation},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
