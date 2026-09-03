/* xw-xwayland-shell.c — xwayland_shell_v1 (staging protocol).
 *
 * Xwayland 24+ creates ONE wl_surface per X11 toplevel and assigns it
 * the xwayland_surface role through this protocol — the old path (the
 * X-side WL_SURFACE_ID atom, or xdg-shell toplevels) is gone. Without
 * this global, Xwayland runs headless: X11 clients connect, render,
 * and their windows never reach the compositor.
 *
 * The role is deliberately NOT a parallel window-management path: the
 * surface's role_data is a plain struct xw_window, managed by the same
 * xw_wm_* calls as xdg toplevels — one focus/raise/stack/workspace/
 * taskbar model for native and X11 windows alike. Differences from the
 * xdg role, all protocol-mandated:
 *   - no configure/ack state machine: a commit carrying a buffer maps
 *     the window at its current size; a null-buffer commit unmaps it
 *   - no set_title/set_app_id requests (the X side owns the window
 *     name; the compositor deliberately speaks no X11), so windows
 *     carry the app_id "xwayland" and a generic title
 *   - set_serial stores the compositor-side half of the Xwayland
 *     surface/window correlation: the same 64-bit value Xwayland sends
 *     to the X WM helper via the WL_SURFACE_SERIAL client message. The
 *     compositor's window-control channel keys off it.
 *
 * Closing: xwayland_surface_v1 has no close event, so the compositor
 * forwards closes to the session's xw-xwm helper over the private
 * xw_window_control_v1 protocol (which also mirrors compositor
 * geometry into X11 so input coordinates stay aligned).
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------- window control channel */

static pid_t pid_of_client(struct wl_client *client) {
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, NULL, NULL);
    return pid;
}

/* manager bindings: xw-xwm (session helper) subscribes to geometry and
 * close events. Geometry changes are pushed by the WM paths; closes by
 * xw_xwayland_window_close. */
struct xw_wc_manager {
    struct wl_resource *res;
    struct wl_list link; /* comp.wc_managers */
};

static void wc_destroy_req(struct wl_client *client,
                           struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct xw_window_control_manager_v1_interface wc_impl = {
    .destroy = wc_destroy_req,
};

static void wc_bind(struct wl_client *client, void *data, uint32_t version,
                    uint32_t id) {
    struct xw_compositor *c = data;
    if (version > 1)
        version = 1;
    struct wl_resource *res =
        wl_resource_create(client, &xw_window_control_manager_v1_interface,
                           version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    struct xw_wc_manager *m = calloc(1, sizeof(*m));
    if (!m) {
        wl_client_post_no_memory(client);
        return;
    }
    m->res = res;
    wl_resource_set_implementation(res, &wc_impl, m, NULL);
    wl_list_insert(c->wc_managers.prev, &m->link);
    xw_log(XW_LOG_INFO, "xwayland: window-control manager bound (pid %d)",
           (int)pid_of_client(client));
}

void xw_xwayland_notify_geometry(struct xw_window *w) {
    if (!w || !w->surface || w->surface->role != XW_SURFACE_ROLE_XWAYLAND)
        return;
    if (!w->xw_has_serial)
        return;
    if (w->w <= 0 || w->h <= 0)
        return; /* not mapped yet: nothing meaningful to mirror */
    struct xw_wc_manager *m;
    wl_list_for_each(m, &w->comp->wc_managers, link)
        xw_window_control_manager_v1_send_geometry(
            m->res, (uint32_t)(w->xw_serial >> 32), (uint32_t)w->xw_serial,
            w->x, w->y, w->w, w->h);
}

/* ------------------------------------------------------ xwayland_surface */

static void xw_surface_role_destroy_request(struct wl_client *client,
                                            struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void xw_surface_set_serial(struct wl_client *client,
                                  struct wl_resource *res, uint32_t serial_lo,
                                  uint32_t serial_hi) {
    (void)client;
    struct xw_window *w = wl_resource_get_user_data(res);
    if (!w)
        return;
    w->xw_serial = ((uint64_t)serial_hi << 32) | serial_lo;
    w->xw_has_serial = true;
    xw_log(XW_LOG_INFO,
           "xwayland: window %u serial %08x%08x",
           w->id, serial_hi, serial_lo);
    /* the serial just (re)associated the surface with an X11 window:
     * push the current geometry so the X side tracks our placement */
    xw_xwayland_notify_geometry(w);
}

static const struct xwayland_surface_v1_interface xw_surface_impl = {
    .destroy = xw_surface_role_destroy_request,
    .set_serial = xw_surface_set_serial,
};

/* the xwayland_surface object died (client destroy): unmanage now if
 * the wl_surface still lives (its own destructor path would also run
 * through xw_xwayland_role_destroy; this path covers object-first
 * ordering) */
static void xw_surface_resource_destroy(struct wl_resource *res) {
    struct xw_window *w = wl_resource_get_user_data(res);
    if (!w)
        return;
    struct xw_surface *s = w->surface;
    if (s && s->role_data == w && s->role == XW_SURFACE_ROLE_XWAYLAND) {
        s->role = XW_SURFACE_ROLE_NONE;
        s->role_data = NULL;
        if (s->xdg_surface_res == res)
            s->xdg_surface_res = NULL;
        if (w->toplevel_res == res)
            w->toplevel_res = NULL;
        if (s->comp->wm)
            xw_wm_unmanage(s->comp->wm, w, true);
        else
            free(w);
    }
}

/* --------------------------------------------------------- role dispatch */

void xw_xwayland_role_commit(struct xw_surface *s) {
    struct xw_window *w = s->role_data;
    struct xw_compositor *c = s->comp;
    if (!w || !c->wm)
        return;
    int sc = s->scale > 0 ? s->scale : 1;
    int bw = s->buf_w / sc, bh = s->buf_h / sc;

    if (bw > 0 && bh > 0) {
        if (!w->mapped) {
            w->w = bw;
            w->h = bh;
            xw_wm_window_map(c->wm, w);
            /* placement just ran (cascade): mirror it to the X side so
             * X input coordinates match our geometry */
            xw_xwayland_notify_geometry(w);
        } else if (w->w != bw || w->h != bh) {
            xw_wm_damage_window(c->wm, w);
            w->w = bw;
            w->h = bh;
            xw_wm_damage_window(c->wm, w);
            xw_foreign_toplevel_notify(c, w);
            xw_xwayland_notify_geometry(w);
        }
        xw_subsurface_parent_committed(s);
    } else {
        /* null commit: X11 window unmapped */
        if (w->mapped)
            xw_wm_window_unmap(c->wm, w);
    }
}

void xw_xwayland_role_destroy(struct xw_surface *s) {
    struct xw_window *w = s->role_data;
    if (!w)
        return;
    s->role = XW_SURFACE_ROLE_NONE;
    s->role_data = NULL;
    if (w->toplevel_res) {
        struct wl_resource *tr = w->toplevel_res;
        w->toplevel_res = NULL;
        wl_resource_set_user_data(tr, NULL); /* its destructor is a no-op now */
    }
    if (s->comp->wm)
        xw_wm_unmanage(s->comp->wm, w, true);
    else
        free(w);
}

bool xw_xwayland_window_close(struct xw_window *w) {
    if (!w || !w->surface)
        return false;
    struct xw_surface *s = w->surface;
    struct xw_compositor *c = s->comp;
    xw_log(XW_LOG_INFO,
           "xwayland: close requested for window %u (serial %08x%08x) — "
           "forwarding to the session WM helper", w->id,
           (uint32_t)(w->xw_serial >> 32), (uint32_t)w->xw_serial);
    /* visual feedback immediately; the helper delivers WM_DELETE (or
     * destroys the window) on the X side, which tears the surface down
     * through the normal unmap path */
    if (w->mapped)
        xw_wm_window_unmap(c->wm, w);
    if (w->xw_has_serial) {
        struct xw_wc_manager *m;
        wl_list_for_each(m, &c->wc_managers, link)
            xw_window_control_manager_v1_send_close(
                m->res, (uint32_t)(w->xw_serial >> 32),
                (uint32_t)w->xw_serial);
    }
    return true;
}

/* ------------------------------------------------------ xwayland_shell_v1 */

static void shell_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void shell_get_xwayland_surface(struct wl_client *client,
                                       struct wl_resource *res, uint32_t id,
                                       struct wl_resource *surface_res) {
    struct xw_compositor *c = wl_resource_get_user_data(res);
    struct xw_surface *s = wl_resource_get_user_data(surface_res);
    if (!s) {
        wl_resource_post_error(res, XWAYLAND_SHELL_V1_ERROR_ROLE,
                               "invalid wl_surface");
        return;
    }
    if (s->role != XW_SURFACE_ROLE_NONE) {
        wl_resource_post_error(res, XWAYLAND_SHELL_V1_ERROR_ROLE,
                               "surface already has a role");
        return;
    }

    struct xw_window *w = calloc(1, sizeof(*w));
    if (!w) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *xres =
        wl_resource_create(client, &xwayland_surface_v1_interface, 1, id);
    if (!xres) {
        free(w);
        wl_client_post_no_memory(client);
        return;
    }

    w->comp = c;
    w->client = client;
    w->surface = s;
    w->toplevel_res = xres; /* the xwayland_surface object acts as the
                               window's protocol handle (close/unmap) */
    w->xdg_surface_res = xres;
    w->ws = c->wm ? c->wm->ws_current : 0;
    w->output = NULL;
    w->geo_x = -1;
    w->geo_y = -1;
    snprintf(w->app_id, sizeof(w->app_id), "xwayland");
    snprintf(w->title, sizeof(w->title), "X11 window");
    wl_list_init(&w->toplevel_handles);
    wl_list_init(&w->wsi_handles);

    wl_resource_set_implementation(xres, &xw_surface_impl, w,
                                   xw_surface_resource_destroy);

    s->role = XW_SURFACE_ROLE_XWAYLAND;
    s->role_data = w;
    xw_wm_manage_toplevel(c->wm, w);
    xw_log(XW_LOG_INFO, "xwayland: surface %u associated with window %u",
           wl_resource_get_id(s->res), w->id);
}

static const struct xwayland_shell_v1_interface shell_impl = {
    .destroy = shell_destroy,
    .get_xwayland_surface = shell_get_xwayland_surface,
};

static void bind_xwayland_shell(struct wl_client *client, void *data,
                                uint32_t version, uint32_t id) {
    struct xw_compositor *c = data;
    if (version > 1)
        version = 1;
    struct wl_resource *res =
        wl_resource_create(client, &xwayland_shell_v1_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &shell_impl, c, NULL);
}

void xw_xwayland_shell_init(struct xw_compositor *c) {
    c->g_xwayland_shell = wl_global_create(c->display,
                                           &xwayland_shell_v1_interface, 1, c,
                                           bind_xwayland_shell);
    if (!c->g_xwayland_shell)
        xw_log(XW_LOG_ERROR, "xwayland_shell_v1 global creation failed");
    c->g_window_control =
        wl_global_create(c->display, &xw_window_control_manager_v1_interface,
                         1, c, wc_bind);
    if (!c->g_window_control)
        xw_log(XW_LOG_ERROR,
               "xw_window_control_v1 global creation failed");
}

void xw_xwayland_shell_fin(struct xw_compositor *c) {
    if (c->g_xwayland_shell) {
        wl_global_destroy(c->g_xwayland_shell);
        c->g_xwayland_shell = NULL;
    }
    if (c->g_window_control) {
        wl_global_destroy(c->g_window_control);
        c->g_window_control = NULL;
    }
    struct xw_wc_manager *m, *m2;
    wl_list_for_each_safe(m, m2, &c->wc_managers, link) {
        wl_list_remove(&m->link);
        free(m);
    }
}
