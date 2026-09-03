/* xwc-tasklist.c — libxwcl bindings for wlr-foreign-toplevel-management
 * and ext-workspace: everything a panel tasklist and a workspace
 * switcher need, with no toolkit.
 *
 * Ownership model: the registry records the manager globals
 * (xwc.ftm_global / xwc.wsm_global) without binding — binding would
 * immediately materialize new_id announcement proxies (workspace
 * group + handles) that clients not using these modules would leak
 * (found by LSan).  xwc_tasklist_create / xwc_wspaces_create bind on
 * demand, own the manager proxy, and every handle they create.
 */
#include "xwc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wayland-client.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"
#include "ext-workspace.h"
#include "xw-workspace-info-v1.h"

/* ------------------------------------------------------------ tasklist */

struct xwc_task {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct xw_workspace_toplevel_v1 *wsi; /* workspace annotation */
    char title[256];
    char app_id[256];
    bool active;
    bool minimized;
    bool maximized;
    bool fullscreen;
    int ws; /* xw_workspace_info_v1 index; -1 sticky, -2 unknown */
    struct xwc_tasklist *tl;
    struct xwc_task *prev, *next;
};

struct xwc_tasklist {
    struct xwc *c;
    struct zwlr_foreign_toplevel_manager_v1 *mgr;
    struct xw_workspace_info_v1 *wsi; /* manager, when offered */
    struct xwc_task *head, *tail;
    void (*changed)(void *ud);
    void *ud;
    bool finished;
};

static void tasklist_notify(struct xwc_tasklist *tl) {
    if (tl && tl->changed && !tl->finished)
        tl->changed(tl->ud);
}

static void task_unlink(struct xwc_tasklist *tl, struct xwc_task *task) {
    if (task->prev)
        task->prev->next = task->next;
    else
        tl->head = task->next;
    if (task->next)
        task->next->prev = task->prev;
    else
        tl->tail = task->prev;
}

/* the wl_seat proxy of the connection (stored as void* in xwc) */
static struct wl_seat *seat_of(struct xwc *c) { return c->seat; }

static void handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                         const char *title) {
    (void)h;
    struct xwc_task *task = data;
    snprintf(task->title, sizeof(task->title), "%s", title ? title : "");
}

static void handle_app_id(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *h,
                          const char *app_id) {
    (void)h;
    struct xwc_task *task = data;
    snprintf(task->app_id, sizeof(task->app_id), "%s", app_id ? app_id : "");
}

static void handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                         struct wl_array *state) {
    (void)h;
    struct xwc_task *task = data;
    task->active = task->minimized = task->maximized = task->fullscreen = false;
    uint32_t *st;
    wl_array_for_each(st, state) {
        switch (*st) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            task->maximized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            task->minimized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            task->fullscreen = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            task->active = true;
            break;
        default:
            break; /* future states: ignored */
        }
    }
}

static void handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h;
    struct xwc_task *task = data;
    if (task->tl)
        tasklist_notify(task->tl);
}

static void handle_closed(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *h) {
    struct xwc_task *task = data;
    struct xwc_tasklist *tl = task->tl;
    /* destroying the proxy inside its own event handler is safe: the
     * event has already been demarshalled (weston/GTK pattern) */
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    task->handle = NULL;
    if (task->wsi)
        xw_workspace_toplevel_v1_destroy(task->wsi);
    task->wsi = NULL;
    task_unlink(tl, task);
    free(task);
    tasklist_notify(tl);
}

/* output_enter/leave and parent are irrelevant for a v0 panel tasklist */
static void handle_output_enter(void *data,
                                struct zwlr_foreign_toplevel_handle_v1 *h,
                                struct wl_output *output) {
    (void)data;
    (void)h;
    (void)output;
}
static void handle_output_leave(void *data,
                                struct zwlr_foreign_toplevel_handle_v1 *h,
                                struct wl_output *output) {
    (void)data;
    (void)h;
    (void)output;
}
static void handle_parent(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data;
    (void)h;
    (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed,
    .parent = handle_parent,
};

/* ---------------------------------------- workspace annotation events */

static void wsi_workspace_ev(void *data, struct xw_workspace_toplevel_v1 *w,
                             int32_t index) {
    (void)w;
    struct xwc_task *task = data;
    task->ws = index;
}

static void wsi_done_ev(void *data, struct xw_workspace_toplevel_v1 *w) {
    (void)w;
    struct xwc_task *task = data;
    if (task->tl)
        tasklist_notify(task->tl);
}

static const struct xw_workspace_toplevel_v1_listener wsi_task_listener = {
    .workspace = wsi_workspace_ev,
    .done = wsi_done_ev,
};

static void mgr_toplevel(void *data,
                         struct zwlr_foreign_toplevel_manager_v1 *mgr,
                         struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)mgr;
    struct xwc_tasklist *tl = data;
    struct xwc_task *task = calloc(1, sizeof(*task));
    if (!task)
        return;
    task->handle = handle;
    task->tl = tl;
    task->ws = -2; /* unknown until the annotation's first event */
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener,
                                                 task);
    /* annotate with the workspace when the compositor offers it; the
     * initial workspace event arrives with the next dispatch */
    if (tl->wsi) {
        task->wsi = xw_workspace_info_v1_get_toplevel_workspace(tl->wsi,
                                                                handle);
        if (task->wsi)
            xw_workspace_toplevel_v1_add_listener(task->wsi, &wsi_task_listener,
                                                  task);
    }
    task->prev = tl->tail;
    if (tl->tail)
        tl->tail->next = task;
    else
        tl->head = task;
    tl->tail = task;
    tasklist_notify(tl);
}

static void mgr_finished(void *data,
                         struct zwlr_foreign_toplevel_manager_v1 *mgr) {
    (void)mgr;
    struct xwc_tasklist *tl = data;
    tl->finished = true;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_listener = {
    .toplevel = mgr_toplevel,
    .finished = mgr_finished,
};

struct xwc_tasklist *xwc_tasklist_create(struct xwc *c,
                                         void (*changed)(void *ud), void *ud) {
    if (!c || !c->registry || !c->ftm_global)
        return NULL; /* compositor without foreign-toplevel support */
    struct xwc_tasklist *tl = calloc(1, sizeof(*tl));
    if (!tl)
        return NULL;
    tl->c = c;
    tl->mgr = wl_registry_bind((struct wl_registry *)c->registry,
                               c->ftm_global,
                               &zwlr_foreign_toplevel_manager_v1_interface,
                               3);
    if (!tl->mgr) {
        free(tl);
        return NULL;
    }
    c->ftm_global = 0; /* consumed: one tasklist per connection */
    tl->changed = changed;
    tl->ud = ud;
    zwlr_foreign_toplevel_manager_v1_add_listener(tl->mgr, &mgr_listener, tl);
    /* bind the workspace annotator when advertised (compositors that
     * predate it simply leave every task at ws = -2, unknown) */
    if (c->wsi_global) {
        tl->wsi = wl_registry_bind((struct wl_registry *)c->registry,
                                   c->wsi_global,
                                   &xw_workspace_info_v1_interface, 1);
        if (tl->wsi)
            c->wsi_global = 0; /* consumed */
    }
    return tl;
}

void xwc_tasklist_destroy(struct xwc_tasklist *tl) {
    if (!tl)
        return;
    struct xwc_task *task = tl->head;
    while (task) {
        struct xwc_task *next = task->next;
        if (task->handle)
            zwlr_foreign_toplevel_handle_v1_destroy(task->handle);
        if (task->wsi)
            xw_workspace_toplevel_v1_destroy(task->wsi);
        free(task);
        task = next;
    }
    if (tl->wsi)
        xw_workspace_info_v1_destroy(tl->wsi);
    if (tl->mgr)
        zwlr_foreign_toplevel_manager_v1_destroy(tl->mgr);
    free(tl);
}

struct xwc_task *xwc_tasklist_first(struct xwc_tasklist *tl) {
    return tl ? tl->head : NULL;
}

struct xwc_task *xwc_task_next(struct xwc_task *task) {
    return task ? task->next : NULL;
}

const char *xwc_task_title(struct xwc_task *task) {
    return task ? task->title : "";
}

const char *xwc_task_app_id(struct xwc_task *task) {
    return task ? task->app_id : "";
}

bool xwc_task_active(struct xwc_task *task) { return task && task->active; }

bool xwc_task_minimized(struct xwc_task *task) {
    return task && task->minimized;
}

int xwc_task_workspace(struct xwc_task *task) {
    return task ? task->ws : -2;
}

void xwc_tasklist_activate(struct xwc_tasklist *tl, struct xwc_task *task) {
    if (!tl || !task || !task->handle)
        return;
    if (task->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(task->handle);
    zwlr_foreign_toplevel_handle_v1_activate(task->handle, seat_of(tl->c));
}

void xwc_tasklist_close(struct xwc_tasklist *tl, struct xwc_task *task) {
    (void)tl;
    if (task && task->handle)
        zwlr_foreign_toplevel_handle_v1_close(task->handle);
}

/* ---------------------------------------------------------- workspaces */

struct xwc_ws {
    struct ext_workspace_handle_v1 *handle;
    char name[128];
    char id[64];
    bool active;
    struct xwc_wspaces *owner;
    struct xwc_ws *prev, *next;
};

struct xwc_wspaces {
    struct xwc *c;
    struct ext_workspace_manager_v1 *mgr;
    struct ext_workspace_group_handle_v1 *group;
    struct xwc_ws *head, *tail;
    void (*changed)(void *ud);
    void *ud;
};

static void ws_notify(struct xwc_wspaces *wl) {
    if (wl && wl->changed)
        wl->changed(wl->ud);
}

static void ws_unlink(struct xwc_wspaces *wl, struct xwc_ws *ws) {
    if (ws->prev)
        ws->prev->next = ws->next;
    else
        wl->head = ws->next;
    if (ws->next)
        ws->next->prev = ws->prev;
    else
        wl->tail = ws->prev;
}

static void ws_handle_id(void *data, struct ext_workspace_handle_v1 *h,
                         const char *id) {
    (void)h;
    struct xwc_ws *ws = data;
    snprintf(ws->id, sizeof(ws->id), "%s", id ? id : "");
}

static void ws_handle_name(void *data, struct ext_workspace_handle_v1 *h,
                           const char *name) {
    (void)h;
    struct xwc_ws *ws = data;
    snprintf(ws->name, sizeof(ws->name), "%s", name ? name : "");
}

static void ws_handle_state(void *data, struct ext_workspace_handle_v1 *h,
                            uint32_t state) {
    (void)h;
    struct xwc_ws *ws = data;
    ws->active = (state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0;
}

static void ws_handle_removed(void *data, struct ext_workspace_handle_v1 *h) {
    struct xwc_ws *ws = data;
    struct xwc_wspaces *wl = ws->owner;
    ext_workspace_handle_v1_destroy(h);
    ws->handle = NULL;
    ws_unlink(wl, ws);
    free(ws);
    ws_notify(wl);
}

static void ws_handle_ignore(void *data, struct ext_workspace_handle_v1 *h) {
    (void)data;
    (void)h;
}
static void ws_handle_capabilities(void *data,
                                   struct ext_workspace_handle_v1 *h,
                                   uint32_t capabilities) {
    (void)capabilities;
    ws_handle_ignore(data, h);
}
static void ws_handle_coordinates(void *data,
                                  struct ext_workspace_handle_v1 *h,
                                  struct wl_array *coordinates) {
    (void)coordinates;
    ws_handle_ignore(data, h);
}

static const struct ext_workspace_handle_v1_listener ws_handle_listener = {
    .id = ws_handle_id,
    .name = ws_handle_name,
    .coordinates = ws_handle_coordinates,
    .state = ws_handle_state,
    .capabilities = ws_handle_capabilities,
    .removed = ws_handle_removed,
};

static void wsm_group(void *data, struct ext_workspace_manager_v1 *mgr,
                      struct ext_workspace_group_handle_v1 *group) {
    (void)mgr;
    struct xwc_wspaces *wl = data;
    if (!wl->group)
        wl->group = group;
}

static void wsm_workspace(void *data, struct ext_workspace_manager_v1 *mgr,
                          struct ext_workspace_handle_v1 *handle) {
    (void)mgr;
    struct xwc_wspaces *wl = data;
    struct xwc_ws *ws = calloc(1, sizeof(*ws));
    if (!ws)
        return;
    ws->handle = handle;
    ws->owner = wl;
    ext_workspace_handle_v1_add_listener(handle, &ws_handle_listener, ws);
    ws->prev = wl->tail;
    if (wl->tail)
        wl->tail->next = ws;
    else
        wl->head = ws;
    wl->tail = ws;
}

static void wsm_done(void *data, struct ext_workspace_manager_v1 *mgr) {
    (void)mgr;
    ws_notify(data);
}

static void wsm_finished(void *data, struct ext_workspace_manager_v1 *mgr) {
    (void)data;
    (void)mgr;
}

static const struct ext_workspace_manager_v1_listener wsm_listener = {
    .workspace_group = wsm_group,
    .workspace = wsm_workspace,
    .done = wsm_done,
    .finished = wsm_finished,
};

struct xwc_wspaces *xwc_wspaces_create(struct xwc *c,
                                       void (*changed)(void *ud), void *ud) {
    if (!c || !c->registry || !c->wsm_global)
        return NULL; /* compositor without ext-workspace support */
    struct xwc_wspaces *wl = calloc(1, sizeof(*wl));
    if (!wl)
        return NULL;
    wl->c = c;
    wl->mgr = wl_registry_bind((struct wl_registry *)c->registry,
                               c->wsm_global,
                               &ext_workspace_manager_v1_interface, 1);
    if (!wl->mgr) {
        free(wl);
        return NULL;
    }
    c->wsm_global = 0; /* consumed: one workspace tracker per connection */
    wl->changed = changed;
    wl->ud = ud;
    ext_workspace_manager_v1_add_listener(wl->mgr, &wsm_listener, wl);
    return wl;
}

void xwc_wspaces_destroy(struct xwc_wspaces *wl) {
    if (!wl)
        return;
    struct xwc_ws *ws = wl->head;
    while (ws) {
        struct xwc_ws *next = ws->next;
        if (ws->handle)
            ext_workspace_handle_v1_destroy(ws->handle);
        free(ws);
        ws = next;
    }
    if (wl->group)
        ext_workspace_group_handle_v1_destroy(wl->group);
    if (wl->mgr)
        ext_workspace_manager_v1_destroy(wl->mgr);
    free(wl);
}

static struct xwc_ws *ws_at(struct xwc_wspaces *wl, int idx) {
    if (!wl || idx < 0)
        return NULL;
    struct xwc_ws *ws = wl->head;
    for (int i = 0; ws && i < idx; i++)
        ws = ws->next;
    return ws;
}

int xwc_wspaces_count(struct xwc_wspaces *wl) {
    int n = 0;
    if (wl) {
        for (struct xwc_ws *ws = wl->head; ws; ws = ws->next)
            n++;
    }
    return n;
}

const char *xwc_wspaces_name(struct xwc_wspaces *wl, int idx) {
    struct xwc_ws *ws = ws_at(wl, idx);
    return ws && ws->name[0] ? ws->name : "";
}

bool xwc_wspaces_active(struct xwc_wspaces *wl, int idx) {
    struct xwc_ws *ws = ws_at(wl, idx);
    return ws && ws->active;
}

void xwc_wspaces_activate(struct xwc_wspaces *wl, int idx) {
    struct xwc_ws *ws = ws_at(wl, idx);
    if (ws && ws->handle)
        ext_workspace_handle_v1_activate(ws->handle);
}
