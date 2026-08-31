/* xw-foreign-toplevel.c — wlr foreign toplevel management.
 *
 * The panel tasklist and window-switcher consume this protocol: every
 * mapped toplevel gets a handle per bound manager with title/app_id/
 * state events; handle requests map to wm operations.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

struct xw_ft_manager {
    struct wl_resource *res; /* zwlr_foreign_toplevel_manager_v1 */
    struct wl_list link;     /* comp.ft_managers */
    struct wl_list handles;  /* xw_foreign_toplevel_res.mgr_link */
};

/* ------------------------------------------------------------ handle */

static void h_destroy(struct wl_client *client, struct wl_resource *res);
static void h_set_maximized(struct wl_client *client, struct wl_resource *res);
static void h_unset_maximized(struct wl_client *client, struct wl_resource *res);
static void h_set_minimized(struct wl_client *client, struct wl_resource *res);
static void h_unset_minimized(struct wl_client *client, struct wl_resource *res);
static void h_activate(struct wl_client *client, struct wl_resource *res,
                       struct wl_resource *seat);
static void h_close(struct wl_client *client, struct wl_resource *res);
static void h_set_rectangle(struct wl_client *client, struct wl_resource *res,
                            struct wl_resource *surface, int32_t x, int32_t y,
                            int32_t width, int32_t height);
static void h_set_fullscreen(struct wl_client *client, struct wl_resource *res,
                             struct wl_resource *output);
static void h_unset_fullscreen(struct wl_client *client,
                               struct wl_resource *res);

static const struct zwlr_foreign_toplevel_handle_v1_interface handle_impl = {
    .destroy = h_destroy,
    .set_maximized = h_set_maximized,
    .unset_maximized = h_unset_maximized,
    .set_minimized = h_set_minimized,
    .unset_minimized = h_unset_minimized,
    .activate = h_activate,
    .close = h_close,
    .set_rectangle = h_set_rectangle,
    .set_fullscreen = h_set_fullscreen,
    .unset_fullscreen = h_unset_fullscreen,
};

static struct xw_window *window_of_handle(struct wl_resource *res) {
    struct xw_foreign_toplevel_res *f = wl_resource_get_user_data(res);
    struct xw_compositor *c = f ? f->comp : NULL;
    if (!c || !c->wm)
        return NULL;
    /* find the window owning this handle */
    struct xw_window *w;
    wl_list_for_each(w, &c->wm->windows, link) {
        struct xw_foreign_toplevel_res *it;
        wl_list_for_each(it, &w->toplevel_handles, link) {
            if (it == f)
                return w;
        }
    }
    return NULL;
}

static void h_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void h_set_maximized(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm)
        xw_wm_maximize(w->comp->wm, w, true);
}

static void h_unset_maximized(struct wl_client *client,
                              struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm)
        xw_wm_maximize(w->comp->wm, w, false);
}

static void h_set_minimized(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm)
        xw_wm_minimize(w->comp->wm, w, true);
}

static void h_unset_minimized(struct wl_client *client,
                              struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm)
        xw_wm_minimize(w->comp->wm, w, false);
}

static void h_activate(struct wl_client *client, struct wl_resource *res,
                       struct wl_resource *seat) {
    (void)client;
    (void)seat; /* single seat; v3 activation tokens not validated (v0) */
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm) {
        /* tasklist activation also restores minimized windows */
        if (w->ws != -1 && w->ws != w->comp->wm->ws_current)
            xw_wm_switch_workspace(w->comp->wm, w->ws);
        xw_wm_focus_window(w->comp->wm, w, true);
    }
}

static void h_close(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm)
        xw_wm_close(w->comp->wm, w);
}

static void h_set_rectangle(struct wl_client *client, struct wl_resource *res,
                            struct wl_resource *surface, int32_t x, int32_t y,
                            int32_t width, int32_t height) {
    /* minimap hint for switchers; accepted, unused (v0) */
    (void)client;
    (void)res;
    (void)surface;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void h_set_fullscreen(struct wl_client *client, struct wl_resource *res,
                             struct wl_resource *output) {
    (void)client;
    (void)output;
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm)
        xw_wm_fullscreen(w->comp->wm, w, true);
}

static void h_unset_fullscreen(struct wl_client *client,
                               struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_of_handle(res);
    if (w && w->comp->wm)
        xw_wm_fullscreen(w->comp->wm, w, false);
}

static void handle_resource_destroy(struct wl_resource *res) {
    struct xw_foreign_toplevel_res *f = wl_resource_get_user_data(res);
    if (!f)
        return;
    wl_list_remove(&f->link);
    wl_list_remove(&f->mgr_link);
    free(f);
}

/* ----------------------------------------------------------- manager */

static void mgr_stop(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    zwlr_foreign_toplevel_manager_v1_send_finished(res);
    wl_resource_destroy(res);
}

static const struct zwlr_foreign_toplevel_manager_v1_interface manager_impl = {
    .stop = mgr_stop,
};

static void manager_resource_destroy(struct wl_resource *res) {
    struct xw_ft_manager *m = wl_resource_get_user_data(res);
    if (!m)
        return;
    /* destroy all handles owned by this manager */
    struct xw_foreign_toplevel_res *f, *f2;
    wl_list_for_each_safe(f, f2, &m->handles, mgr_link) {
        struct wl_resource *hres = f->res;
        f->res = NULL;
        wl_resource_destroy(hres); /* destructor frees the node */
    }
    wl_list_remove(&m->link);
    free(m);
}

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id) {
    struct xw_compositor *c = data;
    if (version > 3)
        version = 3;
    struct xw_ft_manager *m = calloc(1, sizeof(*m));
    if (!m) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *res = wl_resource_create(
        client, &zwlr_foreign_toplevel_manager_v1_interface, version, id);
    if (!res) {
        free(m);
        wl_client_post_no_memory(client);
        return;
    }
    m->res = res;
    wl_list_init(&m->handles);
    wl_resource_set_implementation(res, &manager_impl, m,
                                   manager_resource_destroy);
    wl_list_insert(c->ft_managers.prev, &m->link);

    /* announce all existing mapped windows to the new manager */
    struct xw_window *w;
    wl_list_for_each(w, &c->wm->windows, link) {
        if (w->mapped)
            xw_foreign_toplevel_window_mapped(c, w);
    }
}

/* --------------------------------------------------- compositor hooks */

void xw_foreign_toplevel_init(struct xw_compositor *c) {
    struct wl_global *g = wl_global_create(
        c->display, &zwlr_foreign_toplevel_manager_v1_interface, 3, c,
        bind_manager);
    if (!g)
        xw_log(XW_LOG_ERROR, "foreign toplevel manager global creation failed");
}

void xw_foreign_toplevel_fin(struct xw_compositor *c) {
    /* manager resources are destroyed with their clients */
    (void)c;
}

void xw_foreign_toplevel_window_mapped(struct xw_compositor *c,
                                       struct xw_window *w) {
    struct xw_ft_manager *m;
    wl_list_for_each(m, &c->ft_managers, link) {
        struct xw_foreign_toplevel_res *f = calloc(1, sizeof(*f));
        if (!f)
            continue;
        struct wl_resource *hres = wl_resource_create(
            wl_resource_get_client(m->res),
            &zwlr_foreign_toplevel_handle_v1_interface,
            wl_resource_get_version(m->res), 0);
        if (!hres) {
            free(f);
            continue;
        }
        f->res = hres;
        f->comp = c;
        wl_list_insert(w->toplevel_handles.prev, &f->link);
        wl_list_insert(m->handles.prev, &f->mgr_link);
        wl_resource_set_implementation(hres, &handle_impl, f,
                                       handle_resource_destroy);
        zwlr_foreign_toplevel_manager_v1_send_toplevel(m->res, hres);
        xw_foreign_toplevel_notify(c, w);
    }
}

void xw_foreign_toplevel_window_unmapped(struct xw_compositor *c,
                                         struct xw_window *w) {
    (void)c;
    (void)w; /* handles persist across minimize/unmap (tasklist UX) */
}

void xw_foreign_toplevel_notify(struct xw_compositor *c,
                                struct xw_window *w) {
    if (!w)
        return;
    struct xw_foreign_toplevel_res *f;
    wl_list_for_each(f, &w->toplevel_handles, link) {
        struct wl_resource *res = f->res;
        if (!res)
            continue;
        if (w->title[0])
            zwlr_foreign_toplevel_handle_v1_send_title(res, w->title);
        if (w->app_id[0])
            zwlr_foreign_toplevel_handle_v1_send_app_id(res, w->app_id);
        struct wl_array states;
        wl_array_init(&states);
        uint32_t *st;
        if (w->maximized) {
            st = wl_array_add(&states, sizeof(*st));
            *st = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED;
        }
        if (w->minimized) {
            st = wl_array_add(&states, sizeof(*st));
            *st = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED;
        }
        if (w->fullscreen) {
            st = wl_array_add(&states, sizeof(*st));
            *st = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN;
        }
        if (w->activated) {
            st = wl_array_add(&states, sizeof(*st));
            *st = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
        }
        zwlr_foreign_toplevel_handle_v1_send_state(res, &states);
        wl_array_release(&states);
        if (wl_resource_get_version(res) >=
            ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_DONE_SINCE_VERSION)
            zwlr_foreign_toplevel_handle_v1_send_done(res);
    }
    (void)c;
}
