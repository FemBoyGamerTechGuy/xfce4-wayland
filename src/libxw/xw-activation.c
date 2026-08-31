/* xw-activation.c — xdg-activation-v1 (focus handover between clients).
 *
 * Launchers generate a token at commit time; the activated client
 * presents it via activate(). v0 policy: any recently-committed unused
 * token may focus+raise its target window (focus stealing prevention
 * and timeout expiry are documented backlog items).
 */
#include "xw-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct xw_activation {
    struct wl_global *global;
};

struct xw_activation_token {
    struct xw_compositor *comp;
    struct wl_resource *res;
    char *token;             /* assigned at commit */
    struct xw_surface *surface; /* optional target set by the launcher */
    int64_t created_ms;
    struct wl_list link;     /* comp.activation_tokens */
};

static struct xw_activation *g_act;

/* ------------------------------------------------------------ token */

static void token_destroy_req(struct wl_client *client,
                              struct wl_resource *res);
static void token_set_serial(struct wl_client *client, struct wl_resource *res,
                             uint32_t serial, struct wl_resource *seat);
static void token_set_app_id(struct wl_client *client, struct wl_resource *res,
                             const char *app_id);
static void token_set_surface(struct wl_client *client, struct wl_resource *res,
                              struct wl_resource *surface);
static void token_commit(struct wl_client *client, struct wl_resource *res);

static const struct xdg_activation_token_v1_interface token_impl = {
    .destroy = token_destroy_req,
    .set_serial = token_set_serial,
    .set_app_id = token_set_app_id,
    .set_surface = token_set_surface,
    .commit = token_commit,
};

static struct xw_activation_token *token_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void token_resource_destroy(struct wl_resource *res) {
    struct xw_activation_token *t = token_from_res(res);
    if (!t)
        return;
    wl_list_remove(&t->link);
    free(t->token);
    free(t);
}

static void token_destroy_req(struct wl_client *client,
                              struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void token_set_serial(struct wl_client *client, struct wl_resource *res,
                             uint32_t serial, struct wl_resource *seat) {
    (void)client;
    (void)res;
    (void)serial;
    (void)seat; /* serial binding accepted, unused (v0) */
}

static void token_set_app_id(struct wl_client *client, struct wl_resource *res,
                             const char *app_id) {
    (void)client;
    (void)res;
    (void)app_id; /* accepted, unused (v0) */
}

static void token_set_surface(struct wl_client *client, struct wl_resource *res,
                              struct wl_resource *surface) {
    (void)client;
    struct xw_activation_token *t = token_from_res(res);
    if (!t)
        return;
    struct xw_surface *s = wl_resource_get_user_data(surface);
    if (s)
        t->surface = s;
}

static void token_commit(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_activation_token *t = token_from_res(res);
    if (!t)
        return;
    if (t->token) {
        xw_log(XW_LOG_WARN, "xdg-activation: token committed twice");
        return;
    }
    /* random-ish token string */
    char tok[33];
    unsigned rnd = (unsigned)(xw_now_ms() ^ (uintptr_t)t);
    for (int i = 0; i < 8; i++) {
        rnd = rnd * 1664525u + 1013904223u;
        snprintf(tok + i * 4, 5, "%04x", rnd & 0xffff);
    }
    t->token = strdup(tok);
    t->created_ms = xw_now_ms();
    xdg_activation_token_v1_send_done(res, tok);
}

/* ------------------------------------------------------------- manager */

static void act_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void act_activate(struct wl_client *client, struct wl_resource *res,
                         const char *token, struct wl_resource *surface);

static void act_get_token(struct wl_client *client, struct wl_resource *res,
                          uint32_t id) {
    struct xw_compositor *c = wl_resource_get_user_data(res);
    struct xw_activation_token *t = calloc(1, sizeof(*t));
    if (!t) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *tres =
        wl_resource_create(client, &xdg_activation_token_v1_interface,
                           wl_resource_get_version(res), id);
    if (!tres) {
        free(t);
        wl_client_post_no_memory(client);
        return;
    }
    t->comp = c;
    t->res = tres;
    wl_resource_set_implementation(tres, &token_impl, t,
                                   token_resource_destroy);
    wl_list_insert(c->activation_tokens.prev, &t->link);
    (void)g_act;
}

static void act_activate(struct wl_client *client, struct wl_resource *res,
                         const char *token, struct wl_resource *surface) {
    (void)client;
    (void)res;
    struct xw_compositor *c = wl_resource_get_user_data(res);
    struct xw_surface *s = wl_resource_get_user_data(surface);
    if (!token || !s)
        return;

    struct xw_activation_token *t, *t2;
    bool valid = false;
    wl_list_for_each_safe(t, t2, &c->activation_tokens, link) {
        if (t->token && strcmp(t->token, token) == 0) {
            valid = t->surface == NULL || t->surface == s;
            /* tokens are single-use */
            wl_list_remove(&t->link);
            struct wl_resource *tr = t->res;
            t->res = NULL;
            free(t->token);
            t->token = NULL;
            wl_resource_destroy(tr);
            break;
        }
    }
    if (!valid) {
        xw_log(XW_LOG_WARN, "xdg-activation: invalid or used token");
        return;
    }

    /* find the window owning this surface and focus it */
    if (!c->wm)
        return;
    struct xw_window *w;
    wl_list_for_each(w, &c->wm->windows, link) {
        if (w->surface == s) {
            if (w->ws != -1 && w->ws != c->wm->ws_current)
                xw_wm_switch_workspace(c->wm, w->ws);
            xw_wm_focus_window(c->wm, w, true);
            return;
        }
    }
    /* layer surfaces (panel) may also be activation targets */
    xw_seat_set_kb_focus(xw_seat_first(c), s);
}

/* fill in the interface (C89-style ordering workaround) */
static const struct xdg_activation_v1_interface activation_impl_filled = {
    .destroy = act_destroy,
    .get_activation_token = act_get_token,
    .activate = act_activate,
};

static void bind_activation(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    if (version > 1)
        version = 1;
    struct wl_resource *res =
        wl_resource_create(client, &xdg_activation_v1_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &activation_impl_filled, data, NULL);
}

/* --------------------------------------------------- compositor hooks */

void xw_activation_init(struct xw_compositor *c) {
    g_act = calloc(1, sizeof(*g_act));
    if (!g_act)
        return;
    g_act->global = wl_global_create(c->display, &xdg_activation_v1_interface, 1,
                                     c, bind_activation);
    if (!g_act->global)
        xw_log(XW_LOG_ERROR, "xdg-activation global creation failed");
}

void xw_activation_fin(struct xw_compositor *c) {
    (void)c;
    if (g_act) {
        if (g_act->global)
            wl_global_destroy(g_act->global);
        free(g_act);
        g_act = NULL;
    }
}
