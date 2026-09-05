/* xw-activation.c — xdg-activation-v1 (focus handover between clients).
 *
 * The CREDENTIAL (the token string delivered in xdg_activation_token_v1.
 * .done) and the OBJECT (the wl_resource) have different lifetimes by
 * spec: destroying the token object "the received token stays valid"
 * (xdg-activation-v1.xml, destroy request) — GTK4 and Firefox destroy
 * the object immediately after done and call activate() with the bare
 * string afterwards. The string is also designed to travel out-of-band
 * between clients (XDG_ACTIVATION_TOKEN env handoff from a launcher to
 * a freshly started app), so it can not be tied to the creator's
 * connection either.
 *
 * v0 policy: any unexpired, unused token string may focus+raise the
 * surface named in activate(). set_surface on the token is the
 * REQUESTING surface (spec: "different from the surface that will be
 * activated") — advisory metadata for a future focus-stealing policy,
 * never an equality gate (the old gate rejected every launcher->app
 * handover and every GTK4 self-activation whose object was destroyed
 * first). Focus stealing prevention, serial validation and expiry
 * tightening are documented backlog items; expiry is currently a
 * generous TTL.
 */
#include "xw-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* a token older than this is expired (the physical clients activate
 * within seconds of done; launchers pass the string to a child which
 * presents it during startup — 60s covers both with room) */
#define XW_ACTIVATION_TTL_MS 60000

struct xw_activation {
    struct wl_global *global;
};

/* the credential: outlives the token object (and the creator's
 * resource). Owned by comp->activation_tokens. */
struct xw_activation_token {
    struct xw_compositor *comp;
    char *token;                /* the string; NULL once used or never committed */
    struct xw_surface *requester; /* set_surface — advisory, never deref'd */
    int64_t created_ms;
    struct wl_list link;        /* comp->activation_tokens */
    bool res_dead;              /* the token object was destroyed */
};

/* the protocol-object wrapper: dies with the wl_resource; hands its
 * handlers the credential while alive. */
struct token_res {
    struct wl_resource *res;
    struct xw_activation_token *cred;
};

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

static struct token_res *token_res_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

/* free credentials whose object is already dead and whose TTL is over.
 * Live-object credentials are never freed here (their handlers can
 * still fire); they are bounded by client behavior and die at fin. */
static void tokens_gc(struct xw_compositor *c) {
    int64_t now = xw_now_ms();
    struct xw_activation_token *t, *t2;
    wl_list_for_each_safe(t, t2, &c->activation_tokens, link) {
        if (t->res_dead && now - t->created_ms > XW_ACTIVATION_TTL_MS) {
            wl_list_remove(&t->link);
            free(t->token);
            free(t);
        }
    }
}

/* the resource destructor: the OBJECT dies, the CREDENTIAL stays (see
 * the file header). Runs for every live token object during
 * wl_display_destroy_clients, i.e. always before xw_activation_fin. */
static void token_resource_destroy(struct wl_resource *res) {
    struct token_res *w = token_res_from_res(res);
    if (!w)
        return;
    if (w->cred)
        w->cred->res_dead = true;
    free(w);
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
    struct token_res *w = token_res_from_res(res);
    if (!w || !w->cred)
        return;
    /* spec: the REQUESTING surface — advisory only, never compared
     * against the surface passed to activate (that comparison is what
     * broke every real activation flow) */
    struct xw_surface *s = wl_resource_get_user_data(surface);
    if (s)
        w->cred->requester = s;
}

static void token_commit(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct token_res *w = token_res_from_res(res);
    if (!w || !w->cred)
        return;
    struct xw_activation_token *t = w->cred;
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
    tokens_gc(c);
    struct xw_activation_token *t = calloc(1, sizeof(*t));
    if (!t) {
        wl_client_post_no_memory(client);
        return;
    }
    struct token_res *w = calloc(1, sizeof(*w));
    if (!w) {
        free(t);
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *tres =
        wl_resource_create(client, &xdg_activation_token_v1_interface,
                           wl_resource_get_version(res), id);
    if (!tres) {
        free(w);
        free(t);
        wl_client_post_no_memory(client);
        return;
    }
    t->comp = c;
    t->created_ms = xw_now_ms();
    w->res = tres;
    w->cred = t;
    wl_resource_set_implementation(tres, &token_impl, w,
                                   token_resource_destroy);
    wl_list_insert(c->activation_tokens.prev, &t->link);
}

static void act_activate(struct wl_client *client, struct wl_resource *res,
                         const char *token, struct wl_resource *surface) {
    (void)client;
    (void)res;
    struct xw_compositor *c = wl_resource_get_user_data(res);
    struct xw_surface *s = wl_resource_get_user_data(surface);
    if (!token || !s)
        return;

    tokens_gc(c);
    struct xw_activation_token *t, *t2;
    bool valid = false;
    wl_list_for_each_safe(t, t2, &c->activation_tokens, link) {
        if (t->token && strcmp(t->token, token) == 0) {
            /* single-use: consume the credential. The entry itself
             * stays linked until its TTL GC (or fin) — nothing about
             * the string's validity depends on the token object, which
             * the client may have destroyed long ago (spec-sanctioned). */
            valid = true;
            free(t->token);
            t->token = NULL;
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
    struct xw_activation *act = calloc(1, sizeof(*act));
    if (!act)
        return;
    act->global = wl_global_create(c->display, &xdg_activation_v1_interface, 1,
                                   c, bind_activation);
    if (!act->global) {
        xw_log(XW_LOG_ERROR, "xdg-activation global creation failed");
        free(act);
        return;
    }
    c->activation_state = act;
}

/* runs after wl_display_destroy_clients: every token object (wrapper)
 * is dead already, so every remaining credential is ours to free. */
void xw_activation_fin(struct xw_compositor *c) {
    struct xw_activation *act = c->activation_state;
    if (act) {
        if (act->global)
            wl_global_destroy(act->global);
        free(act);
        c->activation_state = NULL;
    }
    struct xw_activation_token *t, *t2;
    wl_list_for_each_safe(t, t2, &c->activation_tokens, link) {
        wl_list_remove(&t->link);
        free(t->token);
        free(t);
    }
}
