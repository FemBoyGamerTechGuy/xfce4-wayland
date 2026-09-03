/* xw-workspace-info.c — the xw-workspace-info-v1 protocol.
 *
 * Panels and pagers need to know which workspace each toplevel lives
 * on. wlr-foreign-toplevel-management carries no workspace field and
 * ext-workspace does not assign toplevels, so this small read-only
 * companion protocol annotates the toplevel handles the panel already
 * holds: get_toplevel_workspace(handle) returns an object whose
 * `workspace` event fires whenever the compositor moves the window.
 *
 * The panel stays a pure protocol client: it never reads compositor
 * structures.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

#include "xw-workspace-info-v1-protocol.h"

struct xw_wsi_manager {
    struct wl_resource *res;  /* xw_workspace_info_v1 */
    struct xw_compositor *comp;
    struct wl_list link;      /* comp.wsi_managers */
    struct wl_list orphans;   /* xw_wsi_res.link (windows that vanished) */
};

/* -------------------------------------------------------- annotation obj */

static void wsi_handle_destroy_req(struct wl_client *client,
                                   struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct xw_workspace_toplevel_v1_interface wsi_handle_impl = {
    .destroy = wsi_handle_destroy_req,
};

static void wsi_handle_resource_destroy(struct wl_resource *res) {
    struct xw_wsi_res *r = wl_resource_get_user_data(res);
    if (!r)
        return;
    wl_list_remove(&r->link);
    free(r);
}

static void wsi_send(struct xw_wsi_res *r) {
    if (!r->res)
        return;
    int32_t ws = r->w ? r->w->ws : -1;
    xw_workspace_toplevel_v1_send_workspace(r->res, ws);
    xw_workspace_toplevel_v1_send_done(r->res);
}

/* ----------------------------------------------------------- the manager */

/* find the window owning a foreign toplevel handle resource (same
 * walk the wlr protocol's own requests use) */
static struct xw_window *window_of_ft_handle(struct xw_compositor *c,
                                             struct wl_resource *toplevel) {
    if (!c || !c->wm || !toplevel)
        return NULL;
    struct xw_foreign_toplevel_res *f = wl_resource_get_user_data(toplevel);
    if (!f)
        return NULL;
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

static void wsi_get_toplevel_workspace(struct wl_client *client,
                                       struct wl_resource *res, uint32_t id,
                                       struct wl_resource *toplevel) {
    struct xw_wsi_manager *m = wl_resource_get_user_data(res);
    if (!m)
        return;
    struct xw_window *w = window_of_ft_handle(m->comp, toplevel);

    struct xw_wsi_res *r = calloc(1, sizeof(*r));
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *hres = wl_resource_create(
        client, &xw_workspace_toplevel_v1_interface,
        wl_resource_get_version(res), id);
    if (!hres) {
        free(r);
        wl_client_post_no_memory(client);
        return;
    }
    r->res = hres;
    r->w = w;
    /* attach to the window's list when it exists, else to the
     * manager's orphan list so the resource still gets freed */
    wl_list_insert(w ? w->wsi_handles.prev : m->orphans.prev, &r->link);
    wl_resource_set_implementation(hres, &wsi_handle_impl, r,
                                   wsi_handle_resource_destroy);
    wsi_send(r);
}

static void wsi_manager_destroy_req(struct wl_client *client,
                                    struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct xw_workspace_info_v1_interface wsi_manager_impl = {
    .destroy = wsi_manager_destroy_req,
    .get_toplevel_workspace = wsi_get_toplevel_workspace,
};

static void wsi_manager_resource_destroy(struct wl_resource *res) {
    struct xw_wsi_manager *m = wl_resource_get_user_data(res);
    if (!m)
        return;
    /* release every orphaned annotation (their windows are gone) */
    struct xw_wsi_res *r, *r2;
    wl_list_for_each_safe(r, r2, &m->orphans, link) {
        struct wl_resource *hres = r->res;
        r->res = NULL;
        wl_resource_destroy(hres); /* the destructor frees the node */
    }
    wl_list_remove(&m->link);
    free(m);
}

static void bind_wsi_manager(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    struct xw_compositor *c = data;
    if (version > 1)
        version = 1;
    struct xw_wsi_manager *m = calloc(1, sizeof(*m));
    if (!m) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *res = wl_resource_create(
        client, &xw_workspace_info_v1_interface, version, id);
    if (!res) {
        free(m);
        wl_client_post_no_memory(client);
        return;
    }
    m->res = res;
    m->comp = c;
    wl_list_init(&m->orphans);
    wl_resource_set_implementation(res, &wsi_manager_impl, m,
                                   wsi_manager_resource_destroy);
    wl_list_insert(c->wsi_managers.prev, &m->link);
}

/* --------------------------------------------------- compositor hooks */

void xw_workspace_info_init(struct xw_compositor *c) {
    wl_list_init(&c->wsi_managers);
    struct wl_global *g = wl_global_create(
        c->display, &xw_workspace_info_v1_interface, 1, c, bind_wsi_manager);
    if (!g)
        xw_log(XW_LOG_ERROR, "xw workspace info global creation failed");
}

void xw_workspace_info_fin(struct xw_compositor *c) {
    /* annotation and manager resources die with their clients */
    (void)c;
}

void xw_workspace_info_notify(struct xw_compositor *c, struct xw_window *w) {
    if (!w)
        return;
    struct xw_wsi_res *r;
    wl_list_for_each(r, &w->wsi_handles, link)
        wsi_send(r);
    (void)c;
}

/* the window is going away: drop every annotation that still points at
 * it (the client releases them in due time, but the pointer must not
 * outlive the window) */
void xw_workspace_info_window_gone(struct xw_compositor *c, struct xw_window *w) {
    if (!w)
        return;
    struct xw_wsi_res *r, *r2;
    wl_list_for_each_safe(r, r2, &w->wsi_handles, link) {
        r->w = NULL;
        /* move to the owning manager's orphan list: the client may
         * still hold the proxy and will destroy it (or the manager
         * dies and releases it) */
        wl_list_remove(&r->link);
        /* the manager is reachable through the resource's client */
        struct wl_client *cl = r->res ? wl_resource_get_client(r->res) : NULL;
        struct xw_wsi_manager *m = NULL;
        if (cl && c) {
            wl_list_for_each(m, &c->wsi_managers, link) {
                if (m->res && wl_resource_get_client(m->res) == cl)
                    break;
            }
            if (&m->link == &c->wsi_managers)
                m = NULL; /* not found (manager already gone) */
        }
        if (m)
            wl_list_insert(m->orphans.prev, &r->link);
        else
            wl_list_init(&r->link); /* leaks the node only if both sides
                                       died: the resource destructor will
                                       still run and free it */
    }
    (void)c;
}
