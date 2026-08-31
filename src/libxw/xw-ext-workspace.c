/* xw-ext-workspace.c — ext-workspace-v1 (workspace awareness for panels).
 *
 * One workspace group ("Default") mirrors the wm's workspace list.
 * activate() requests switch workspaces immediately; manager.commit
 * resends state (the full commit/rebuild semantics of the protocol are
 * simplified — we apply on request and re-sync on commit; documented
 * deviation, sufficient for panel switchers).
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

struct xw_ws_manager {
    struct wl_resource *res;  /* ext_workspace_manager_v1 */
    struct wl_list link;      /* comp.ws_managers */
    struct wl_resource *group; /* one shared group per manager */
    struct wl_list workspaces; /* struct xw_ws_ws.manager_link */
};

static struct xw_compositor *g_ws_comp; /* set in init */


struct xw_ws_ws {
    struct wl_resource *res;  /* ext_workspace_handle_v1 */
    int index;                /* wm workspace index */
    struct wl_list manager_link; /* manager.workspaces */
    struct wl_list link;      /* comp-agnostic: node of manager list */
};

/* -------------------------------------------------------- workspace handle */

static void ws_destroy_req(struct wl_client *client, struct wl_resource *res);
static void ws_activate(struct wl_client *client, struct wl_resource *res);
static void ws_deactivate(struct wl_client *client, struct wl_resource *res);
static void ws_assign(struct wl_client *client, struct wl_resource *res,
                      struct wl_resource *group);
static void ws_remove(struct wl_client *client, struct wl_resource *res);

static const struct ext_workspace_handle_v1_interface ws_impl = {
    .destroy = ws_destroy_req,
    .activate = ws_activate,
    .deactivate = ws_deactivate,
    .assign = ws_assign,
    .remove = ws_remove,
};

static struct xw_ws_ws *ws_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void ws_resource_destroy(struct wl_resource *res) {
    struct xw_ws_ws *ws = ws_from_res(res);
    if (!ws)
        return;
    wl_list_remove(&ws->manager_link);
    free(ws);
}

static void ws_destroy_req(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void ws_activate(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_ws_ws *ws = ws_from_res(res);
    if (!ws || !g_ws_comp || !g_ws_comp->wm)
        return;
    if (ws->index >= 0 && ws->index < g_ws_comp->wm->ws_count)
        xw_wm_switch_workspace(g_ws_comp->wm, ws->index);
}

static void ws_deactivate(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    (void)res; /* at least one workspace must stay active: no-op */
}

static void ws_assign(struct wl_client *client, struct wl_resource *res,
                      struct wl_resource *group) {
    (void)client;
    (void)res;
    (void)group; /* single group: no-op */
}

static void ws_remove(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    (void)res; /* workspace removal is a v0 gap (workspaces.conf instead) */
}

/* ------------------------------------------------------------- group */

static void group_create_workspace(struct wl_client *client,
                                   struct wl_resource *res,
                                   const char *workspace) {
    (void)client;
    (void)res;
    (void)workspace; /* dynamic creation is a v0 gap: workspaces.conf */
    xw_log(XW_LOG_DEBUG, "ext-workspace: create_workspace not implemented");
}

static void group_destroy_req(struct wl_client *client,
                              struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct ext_workspace_group_handle_v1_interface group_impl = {
    .create_workspace = group_create_workspace,
    .destroy = group_destroy_req,
};

static void group_resource_destroy(struct wl_resource *res) {
    (void)res; /* freed with the manager */
}

/* ----------------------------------------------------------- manager */

static void manager_commit(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    (void)res;
    if (g_ws_comp)
        xw_ext_workspace_changed(g_ws_comp);
}

static void manager_stop(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    ext_workspace_manager_v1_send_finished(res);
    wl_resource_destroy(res);
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
    .commit = manager_commit,
    .stop = manager_stop,
};

static void manager_resource_destroy(struct wl_resource *res) {
    struct xw_ws_manager *m = wl_resource_get_user_data(res);
    if (!m)
        return;
    struct xw_ws_ws *ws, *ws2;
    wl_list_for_each_safe(ws, ws2, &m->workspaces, manager_link) {
        struct wl_resource *r = ws->res;
        ws->res = NULL;
        wl_resource_destroy(r); /* destructor frees node */
    }
    wl_list_remove(&m->link);
    free(m);
}

/* (re)announce all workspaces to one manager */
static void manager_sync(struct xw_ws_manager *m, struct xw_compositor *c) {
    if (!m->group) {
        m->group = wl_resource_create(
            wl_resource_get_client(m->res), &ext_workspace_group_handle_v1_interface,
            wl_resource_get_version(m->res), 0);
        if (!m->group)
            return;
        wl_resource_set_implementation(m->group, &group_impl, m,
                                       group_resource_destroy);
        ext_workspace_manager_v1_send_workspace_group(m->res, m->group);
        ext_workspace_group_handle_v1_send_capabilities(
            m->group, EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE);
    }

    struct xw_ws_ws *ws;
    wl_list_for_each(ws, &m->workspaces, manager_link) {
        ext_workspace_handle_v1_send_name(ws->res,
                                          c->wm->ws_names[ws->index]);
        uint32_t state = ws->index == c->wm->ws_current
                             ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE
                             : 0;
        ext_workspace_handle_v1_send_state(ws->res, state);
        ext_workspace_handle_v1_send_capabilities(
            ws->res,
            EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
                EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE);
    }
    ext_workspace_manager_v1_send_done(m->res);
}

/* ensure one handle per wm workspace exists, creating missing ones */
static void manager_ensure_workspaces(struct xw_ws_manager *m,
                                      struct xw_compositor *c) {
    for (int i = 0; i < c->wm->ws_count; i++) {
        bool found = false;
        struct xw_ws_ws *ws;
        wl_list_for_each(ws, &m->workspaces, manager_link) {
            if (ws->index == i) {
                found = true;
                break;
            }
        }
        if (found)
            continue;
        struct xw_ws_ws *nws = calloc(1, sizeof(*nws));
        if (!nws)
            continue;
        nws->res = wl_resource_create(
            wl_resource_get_client(m->res), &ext_workspace_handle_v1_interface,
            wl_resource_get_version(m->res), 0);
        if (!nws->res) {
            free(nws);
            continue;
        }
        nws->index = i;
        wl_resource_set_implementation(nws->res, &ws_impl, nws,
                                       ws_resource_destroy);
        wl_list_insert(m->workspaces.prev, &nws->manager_link);
        ext_workspace_manager_v1_send_workspace(m->res, nws->res);
        char id[32];
        snprintf(id, sizeof(id), "xw-ws-%d", i + 1);
        ext_workspace_handle_v1_send_id(nws->res, id);
    }
}

static void bind_manager(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id) {
    struct xw_compositor *c = data;
    if (version > 1)
        version = 1;
    struct xw_ws_manager *m = calloc(1, sizeof(*m));
    if (!m) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *res = wl_resource_create(
        client, &ext_workspace_manager_v1_interface, version, id);
    if (!res) {
        free(m);
        wl_client_post_no_memory(client);
        return;
    }
    m->res = res;
    wl_list_init(&m->workspaces);
    wl_resource_set_implementation(res, &manager_impl, m,
                                   manager_resource_destroy);
    wl_list_insert(c->ws_managers.prev, &m->link);
    manager_ensure_workspaces(m, c);
    manager_sync(m, c);
}

/* --------------------------------------------------- compositor hooks */

void xw_ext_workspace_init(struct xw_compositor *c) {
    g_ws_comp = c;
    struct wl_global *g = wl_global_create(
        c->display, &ext_workspace_manager_v1_interface, 1, c, bind_manager);
    if (!g)
        xw_log(XW_LOG_ERROR, "ext workspace global creation failed");
}

void xw_ext_workspace_fin(struct xw_compositor *c) {
    (void)c;
    if (g_ws_comp == c)
        g_ws_comp = NULL;
}

void xw_ext_workspace_changed(struct xw_compositor *c) {
    struct xw_ws_manager *m;
    wl_list_for_each(m, &c->ws_managers, link) {
        manager_ensure_workspaces(m, c);
        manager_sync(m, c);
    }
}
