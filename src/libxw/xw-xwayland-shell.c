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
    struct xw_compositor *comp;
    struct wl_list link; /* comp.wc_managers */
};

/* identity/hints arriving on the helper's own connection BEFORE the
 * xwayland_surface set_serial lands on Xwayland's connection (the two
 * sockets race; whichever wins, the window must end up with the same
 * state). Applied and freed when the serial association arrives. */
struct xw_pending_ident {
    uint64_t serial;
    char title[XW_TITLE_MAX];
    char app_id[XW_TITLE_MAX];
    int min_w, min_h, max_w, max_h, inc_w, inc_h;
    int or_x, or_y, or_w, or_h;
    int gx, gy, gw, gh;
    bool has_title, has_app_id, has_hints, has_or, has_geom;
    struct wl_list link; /* comp.xw_pending_idents */
};

#define XW_MAX_PENDING_IDENTS 64

static void wc_apply_or(struct xw_window *w, int x, int y, int width,
                        int height); /* fwd: pending paths use it */

static struct xw_window *xw_window_by_serial(struct xw_compositor *c,
                                              uint64_t serial) {
    if (!serial)
        return NULL;
    struct xw_window *w;
    wl_list_for_each(w, &c->wm->windows, link) {
        if (w->surface && w->surface->role == XW_SURFACE_ROLE_XWAYLAND &&
            w->xw_has_serial && w->xw_serial == serial)
            return w;
    }
    wl_list_for_each(w, &c->wm->or_windows, link) {
        if (w->surface && w->surface->role == XW_SURFACE_ROLE_XWAYLAND &&
            w->xw_has_serial && w->xw_serial == serial)
            return w;
    }
    return NULL;
}

static struct xw_pending_ident *pending_ident_get(struct xw_compositor *c,
                                                   uint64_t serial) {
    struct xw_pending_ident *p;
    wl_list_for_each(p, &c->xw_pending_idents, link) {
        if (p->serial == serial)
            return p;
    }
    /* bounded: evict the oldest entry when full (a serial that never
     * resolves is a dead window's identity; dropping it is safe) */
    int n = 0;
    wl_list_for_each(p, &c->xw_pending_idents, link)
        n++;
    if (n >= XW_MAX_PENDING_IDENTS) {
        p = wl_container_of(c->xw_pending_idents.prev, p, link);
        wl_list_remove(&p->link);
        free(p);
    }
    p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->serial = serial;
    wl_list_insert(c->xw_pending_idents.prev, &p->link);
    return p;
}

static void pending_ident_apply(struct xw_compositor *c,
                                struct xw_pending_ident *p,
                                struct xw_window *w) {
    if (p->has_title) {
        snprintf(w->title, sizeof(w->title), "%s", p->title);
        xw_foreign_toplevel_notify(c, w);
    }
    if (p->has_app_id) {
        snprintf(w->app_id, sizeof(w->app_id), "%s", p->app_id);
        xw_foreign_toplevel_notify(c, w);
    }
    if (p->has_hints) {
        w->min_w = p->min_w;
        w->min_h = p->min_h;
        w->max_w = p->max_w;
        w->max_h = p->max_h;
        w->xw_inc_w = p->inc_w;
        w->xw_inc_h = p->inc_h;
    }
    if (p->has_or)
        wc_apply_or(w, p->or_x, p->or_y, p->or_w, p->or_h);
    if (p->has_geom && !w->xw_override_redirect && p->gw > 0 && p->gh > 0) {
        if (w->mapped) {
            xw_wm_damage_window(c->wm, w);
            w->x = p->gx;
            w->y = p->gy;
            w->w = p->gw;
            w->h = p->gh;
            xw_wm_damage_window(c->wm, w);
            xw_foreign_toplevel_notify(c, w);
        } else {
            w->x = p->gx;
            w->y = p->gy;
            w->w = p->gw;
            w->h = p->gh;
        }
    }
    wl_list_remove(&p->link);
    free(p);
}

static void pending_ident_flush(struct xw_compositor *c, uint64_t serial,
                                struct xw_window *w) {
    struct xw_pending_ident *p, *p2;
    wl_list_for_each_safe(p, p2, &c->xw_pending_idents, link) {
        if (p->serial == serial)
            pending_ident_apply(c, p, w);
    }
}

static void wc_destroy_req(struct wl_client *client,
                           struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

/* the helper's client connection died (or it unbound): remove the
 * manager BEFORE libwayland frees the resource. Without this, the
 * compositor kept posting focus/geometry/close events into a dead
 * connection — the exact teardown segfault of the 2026-09-04 round
 * (kill Xwayland + helper together -> surface destructors run
 * focus-transition notifications -> post to a destroyed resource). */
static void wc_manager_resource_destroy(struct wl_resource *res) {
    struct xw_wc_manager *m = wl_resource_get_user_data(res);
    if (!m)
        return;
    wl_list_remove(&m->link);
    free(m);
    xw_log(XW_LOG_INFO,
           "xwayland: window-control manager gone (helper disconnected)");
}

/* OR application: mark popup-class, set X-owned position; a window that
 * already mapped as a managed toplevel (late conversion) is re-routed
 * through the OR flow so the taskbar/focus state follows. */
static void wc_apply_or(struct xw_window *w, int x, int y, int width,
                        int height) {
    (void)width;
    (void)height; /* the buffer commits carry the real size */
    struct xw_compositor *c = w->comp;
    if (!w->xw_override_redirect) {
        w->xw_override_redirect = true;
        xw_log(XW_LOG_INFO,
               "xwayland: window %u (serial %08x%08x) is override-redirect "
               "(popup-class): X-owned geometry %d+%d",
               w->id, (uint32_t)(w->xw_serial >> 32), (uint32_t)w->xw_serial,
               x, y);
        bool was_mapped = w->mapped;
        if (was_mapped)
            xw_wm_window_unmap(c->wm, w); /* leave the managed flow */
        w->x = x;
        w->y = y;
        if (was_mapped)
            xw_wm_or_map(c->wm, w);
        else
            /* classification must not wait for a buffer commit: a
             * popup that never draws is still a popup (taskbar /
             * Alt+Tab iterate wm->windows) */
            xw_wm_or_reclassify(c->wm, w);
    } else if (w->x != x || w->y != y) {
        xw_wm_damage_window(c->wm, w);
        w->x = x;
        w->y = y;
        xw_wm_damage_window(c->wm, w);
    }
}

/* find the window for a control request; NULL when the serial has no
 * association yet (the helper can win the set_serial race) */
static struct xw_window *wc_target(struct xw_compositor *c, uint32_t hi,
                                    uint32_t lo) {
    return xw_window_by_serial(c, ((uint64_t)hi << 32) | lo);
}

static void wc_set_title(struct wl_client *client, struct wl_resource *res,
                         uint32_t serial_hi, uint32_t serial_lo,
                         const char *title) {
    (void)client;
    struct xw_wc_manager *m = wl_resource_get_user_data(res);
    struct xw_compositor *c = m->comp;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    struct xw_window *w = wc_target(c, serial_hi, serial_lo);
    if (!w) {
        struct xw_pending_ident *p = pending_ident_get(c, serial);
        if (p) {
            snprintf(p->title, sizeof(p->title), "%s", title ? title : "");
            p->has_title = true;
        }
        return;
    }
    snprintf(w->title, sizeof(w->title), "%s", title ? title : "");
    xw_log(XW_LOG_INFO, "xwayland: window %u title '%s' (WM_NAME)",
           w->id, w->title);
    xw_foreign_toplevel_notify(c, w);
}

static void wc_set_app_id(struct wl_client *client, struct wl_resource *res,
                           uint32_t serial_hi, uint32_t serial_lo,
                           const char *app_id) {
    (void)client;
    struct xw_wc_manager *m = wl_resource_get_user_data(res);
    struct xw_compositor *c = m->comp;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    struct xw_window *w = wc_target(c, serial_hi, serial_lo);
    if (!w) {
        struct xw_pending_ident *p = pending_ident_get(c, serial);
        if (p) {
            snprintf(p->app_id, sizeof(p->app_id), "%s",
                     app_id ? app_id : "");
            p->has_app_id = true;
        }
        return;
    }
    snprintf(w->app_id, sizeof(w->app_id), "%s", app_id ? app_id : "");
    xw_log(XW_LOG_INFO, "xwayland: window %u app_id '%s' (WM_CLASS)",
           w->id, w->app_id);
    xw_foreign_toplevel_notify(c, w);
}

static void wc_set_override_redirect(struct wl_client *client,
                                     struct wl_resource *res,
                                     uint32_t serial_hi, uint32_t serial_lo,
                                     int32_t x, int32_t y, int32_t width,
                                     int32_t height) {
    (void)client;
    struct xw_wc_manager *m = wl_resource_get_user_data(res);
    struct xw_compositor *c = m->comp;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    struct xw_window *w = wc_target(c, serial_hi, serial_lo);
    if (!w) {
        struct xw_pending_ident *p = pending_ident_get(c, serial);
        if (p) {
            p->or_x = x;
            p->or_y = y;
            p->or_w = width;
            p->or_h = height;
            p->has_or = true;
        }
        return;
    }
    wc_apply_or(w, x, y, width, height);
}

static void wc_set_size_hints(struct wl_client *client,
                              struct wl_resource *res, uint32_t serial_hi,
                              uint32_t serial_lo, int32_t min_width,
                              int32_t min_height, int32_t max_width,
                              int32_t max_height, int32_t width_inc,
                              int32_t height_inc) {
    (void)client;
    struct xw_wc_manager *m = wl_resource_get_user_data(res);
    struct xw_compositor *c = m->comp;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    struct xw_window *w = wc_target(c, serial_hi, serial_lo);
    if (!w) {
        struct xw_pending_ident *p = pending_ident_get(c, serial);
        if (p) {
            p->min_w = min_width;
            p->min_h = min_height;
            p->max_w = max_width;
            p->max_h = max_height;
            p->inc_w = width_inc;
            p->inc_h = height_inc;
            p->has_hints = true;
        }
        return;
    }
    w->min_w = min_width;
    w->min_h = min_height;
    w->max_w = max_width;
    w->max_h = max_height;
    w->xw_inc_w = width_inc;
    w->xw_inc_h = height_inc;
    xw_log(XW_LOG_DEBUG,
           "xwayland: window %u size hints min %dx%d max %dx%d inc %dx%d",
           w->id, min_width, min_height, max_width, max_height, width_inc,
           height_inc);
}

static void wc_set_geometry(struct wl_client *client,
                            struct wl_resource *res, uint32_t serial_hi,
                            uint32_t serial_lo, int32_t x, int32_t y,
                            int32_t width, int32_t height) {
    (void)client;
    struct xw_wc_manager *m = wl_resource_get_user_data(res);
    struct xw_compositor *c = m->comp;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    struct xw_window *w = wc_target(c, serial_hi, serial_lo);
    if (!w) {
        /* the helper can win the set_serial race (different sockets) */
        struct xw_pending_ident *p = pending_ident_get(c, serial);
        if (p) {
            p->gx = x;
            p->gy = y;
            p->gw = width;
            p->gh = height;
            p->has_geom = true;
        }
        return;
    }
    if (w->xw_override_redirect)
        return; /* OR geometry flows through set_override_redirect */
    if (width <= 0 || height <= 0)
        return;
    if (w->x == x && w->y == y && w->w == width && w->h == height)
        return; /* converged — no damage churn */
    if (w->mapped) {
        xw_wm_damage_window(c->wm, w);
        w->x = x;
        w->y = y;
        w->w = width;
        w->h = height;
        xw_wm_damage_window(c->wm, w);
        xw_foreign_toplevel_notify(c, w);
    } else {
        w->x = x;
        w->y = y;
        w->w = width;
        w->h = height;
    }
    xw_log(XW_LOG_INFO,
           "xwayland: window %u adopted X geometry %dx%d+%d+%d (extent)",
           w->id, width, height, x, y);
    /* deliberately NO xw_xwayland_notify_geometry echo: the X side
     * already has this state; echoing would fight the helper's loop
     * guard on every client-initiated resize */
}

static const struct xw_window_control_manager_v1_interface wc_impl = {
    .destroy = wc_destroy_req,
    .set_title = wc_set_title,
    .set_app_id = wc_set_app_id,
    .set_override_redirect = wc_set_override_redirect,
    .set_size_hints = wc_set_size_hints,
    .set_geometry = wc_set_geometry,
};

static void wc_bind(struct wl_client *client, void *data, uint32_t version,
                    uint32_t id) {
    struct xw_compositor *c = data;
    if (version > 2)
        version = 2;
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
    m->comp = c;
    wl_resource_set_implementation(res, &wc_impl, m,
                                   wc_manager_resource_destroy);
    wl_list_insert(c->wc_managers.prev, &m->link);
    xw_log(XW_LOG_INFO, "xwayland: window-control manager bound (pid %d, "
           "v%u)", (int)pid_of_client(client), version);
    /* replay the current focus so a late-connecting helper routes keys
     * to the window the user is already typing into */
    if (version >= 2 && c->xw_focus_serial_set) {
        xw_window_control_manager_v1_send_focus(
            res, (uint32_t)(c->xw_focus_serial >> 32),
            (uint32_t)c->xw_focus_serial);
    }
}

void xw_xwayland_notify_geometry(struct xw_window *w) {
    if (!w || !w->surface || w->surface->role != XW_SURFACE_ROLE_XWAYLAND)
        return;
    if (!w->xw_has_serial)
        return;
    if (w->xw_override_redirect)
        return; /* X owns popup geometry; mirroring would fight it */
    if (w->w <= 0 || w->h <= 0)
        return; /* not mapped yet: nothing meaningful to mirror */
    struct xw_wc_manager *m;
    wl_list_for_each(m, &w->comp->wc_managers, link) {
        xw_window_control_manager_v1_send_geometry(
            m->res, (uint32_t)(w->xw_serial >> 32), (uint32_t)w->xw_serial,
            w->x, w->y, w->w, w->h);
    }
}

void xw_xwayland_notify_focus(struct xw_compositor *c,
                               struct xw_surface *focus_surface) {
    if (!c || wl_list_empty(&c->wc_managers))
        return;
    uint64_t serial = 0;
    if (focus_surface &&
        focus_surface->role == XW_SURFACE_ROLE_XWAYLAND &&
        focus_surface->role_data) {
        struct xw_window *w = focus_surface->role_data;
        if (w->xw_has_serial && !w->xw_override_redirect)
            serial = w->xw_serial;
    }
    if (c->xw_focus_serial_set && c->xw_focus_serial == serial)
        return; /* dedupe: the seat funnel calls on every transition */
    c->xw_focus_serial = serial;
    c->xw_focus_serial_set = true;
    xw_log(XW_LOG_DEBUG,
           "xwayland: compositor focus -> serial %08x%08x%s",
           (uint32_t)(serial >> 32), (uint32_t)serial,
           serial ? "" : " (X focus released)");
    struct xw_wc_manager *m;
    wl_list_for_each(m, &c->wc_managers, link) {
        if (wl_resource_get_version(m->res) < 2)
            continue; /* a v1 helper has no focus handler: sending the
                         event would abort it inside libwayland */
        xw_window_control_manager_v1_send_focus(
            m->res, (uint32_t)(serial >> 32), (uint32_t)serial);
    }
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
    /* identity that arrived on the helper's connection before this
     * association: apply it now (title/app_id/hints/OR state) */
    pending_ident_flush(w->comp, w->xw_serial, w);
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
            if (w->xw_override_redirect) {
                /* popup-class X11 window: X owns geometry, no managed
                 * toplevel flow (no rules/cascade/taskbar/focus) */
                xw_wm_or_map(c->wm, w);
            } else {
                xw_wm_window_map(c->wm, w);
                /* placement just ran (cascade): mirror it to the X side
                 * so X input coordinates match our geometry */
                xw_xwayland_notify_geometry(w);
            }
        } else if (w->w != bw || w->h != bh) {
            xw_wm_damage_window(c->wm, w);
            w->w = bw;
            w->h = bh;
            xw_wm_damage_window(c->wm, w);
            if (!w->xw_override_redirect) {
                xw_foreign_toplevel_notify(c, w);
                xw_xwayland_notify_geometry(w);
            }
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
                         2, c, wc_bind);
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
    struct xw_pending_ident *p, *p2;
    wl_list_for_each_safe(p, p2, &c->xw_pending_idents, link) {
        wl_list_remove(&p->link);
        free(p);
    }
}
