/* xw-layer-shell.c — wlr-layer-shell (panels, notifications, wallpaper).
 *
 * Panels and desktop chrome anchor to output edges; exclusive zones
 * shrink the wm usable area (recomputed in xw-wm.c). Keyboard
 * interactivity: exclusive layers steal keyboard focus from windows
 * (handled in the seat); on-demand layers get focus on click.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

#define LAYER_SHELL_VERSION 4

struct xw_layer_shell {
    struct wl_global *global;
    struct xw_compositor *comp;
};

static struct xw_layer_shell *g_shell;

/* ---------------------------------------------------- zwlr_layer_surface */

static void ls_destroy_req(struct wl_client *client, struct wl_resource *res);
static void ls_set_size(struct wl_client *client, struct wl_resource *res,
                        uint32_t width, uint32_t height);
static void ls_set_anchor(struct wl_client *client, struct wl_resource *res,
                          uint32_t anchor);
static void ls_set_exclusive_zone(struct wl_client *client,
                                  struct wl_resource *res, int32_t zone);
static void ls_set_margin(struct wl_client *client, struct wl_resource *res,
                          int32_t top, int32_t right, int32_t bottom,
                          int32_t left);
static void ls_set_keyboard_interactivity(struct wl_client *client,
                                          struct wl_resource *res,
                                          uint32_t interactivity);
static void ls_get_popup(struct wl_client *client, struct wl_resource *res,
                         struct wl_resource *popup);
static void ls_ack_configure(struct wl_client *client,
                             struct wl_resource *res, uint32_t serial);
static void ls_set_layer(struct wl_client *client, struct wl_resource *res,
                         uint32_t layer);

static const struct zwlr_layer_surface_v1_interface layer_surface_impl = {
    .destroy = ls_destroy_req,
    .set_size = ls_set_size,
    .set_anchor = ls_set_anchor,
    .set_exclusive_zone = ls_set_exclusive_zone,
    .set_margin = ls_set_margin,
    .set_keyboard_interactivity = ls_set_keyboard_interactivity,
    .get_popup = ls_get_popup,
    .ack_configure = ls_ack_configure,
    .set_layer = ls_set_layer,
};

static struct xw_layer_surface *ls_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void ls_configure(struct xw_layer_surface *ls) {
    if (!ls->res || !ls->surface)
        return;
    int w = ls->configured_w, h = ls->configured_h;
    uint32_t a = ls->anchors;
    struct xw_output *o = ls->output;
    if (o) {
        /* anchored to both edges: the compositor dictates the size */
        if ((a & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) &&
            (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
            w = o->width - ls->margin.left - ls->margin.right;
        if ((a & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
            (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
            h = o->height - ls->margin.top - ls->margin.bottom;
    }
    uint32_t serial = wl_display_next_serial(ls->comp->display);
    zwlr_layer_surface_v1_send_configure(ls->res, serial, (uint32_t)w,
                                         (uint32_t)h);
    ls->surface->pending_config = true;
    ls->surface->pending_serial = serial;
}

/* apply anchors+size+margin to compute geometry in global coords */
static void ls_layout(struct xw_layer_surface *ls) {
    struct xw_output *o = ls->output;
    if (!o)
        return;
    int w = ls->surface->buf_w > 0 ? ls->surface->buf_w : ls->configured_w;
    int h = ls->surface->buf_h > 0 ? ls->surface->buf_h : ls->configured_h;
    uint32_t a = ls->anchors;
    int x = o->x, y = o->y;

    if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT &&
        a & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
        x = o->x;
    else if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
        x = o->x + o->width - w - ls->margin.right;
    else if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)
        x = o->x + ls->margin.left;
    else
        x = o->x + (o->width - w) / 2;

    if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP &&
        a & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
        y = o->y;
    else if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
        y = o->y + o->height - h - ls->margin.bottom;
    else if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
        y = o->y + ls->margin.top;
    else
        y = o->y + (o->height - h) / 2;

    /* anchored to both edges: span the full dimension minus margins */
    if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT &&
        a & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
        w = o->width - ls->margin.left - ls->margin.right;
    if (a & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP &&
        a & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
        h = o->height - ls->margin.top - ls->margin.bottom;

    ls->x = x;
    ls->y = y;
    ls->w = w > 0 ? w : 1;
    ls->h = h > 0 ? h : 1;
}

static void ls_destroy_req(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void ls_set_size(struct wl_client *client, struct wl_resource *res,
                        uint32_t width, uint32_t height) {
    (void)client;
    struct xw_layer_surface *ls = ls_from_res(res);
    ls->configured_w = (int)width;
    ls->configured_h = (int)height;
    if (!ls->mapped && ls->configured_sent) {
        /* the client resized itself before mapping: update the pending
         * configure so the committed geometry matches */
        ls_configure(ls);
    }
}

static void ls_set_layer(struct wl_client *client, struct wl_resource *res,
                         uint32_t layer) {
    (void)client;
    struct xw_layer_surface *ls = ls_from_res(res);
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(res, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "invalid layer");
        return;
    }
    if (ls->layer == (enum zwlr_layer_shell_v1_layer)layer)
        return;
    if (ls->mapped)
        xw_damage_outputs_rect(ls->comp, ls->x, ls->y, ls->w, ls->h);
    wl_list_remove(&ls->link);
    wl_list_insert(ls->comp->wm->layers[layer].prev, &ls->link);
    ls->layer = layer;
    if (ls->mapped) {
        xw_wm_recalculate_usable(ls->comp->wm);
        xw_damage_outputs_rect(ls->comp, ls->x, ls->y, ls->w, ls->h);
    }
}

static void ls_set_anchor(struct wl_client *client, struct wl_resource *res,
                          uint32_t anchor) {
    (void)client;
    struct xw_layer_surface *ls = ls_from_res(res);
    if (anchor > (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                  ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
        wl_resource_post_error(res,
                               ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
                               "invalid anchor");
        return;
    }
    ls->anchors = anchor;
    if (ls->mapped) {
        xw_damage_outputs_rect(ls->comp, ls->x, ls->y, ls->w, ls->h);
        ls_layout(ls);
        ls_configure(ls);
        xw_wm_recalculate_usable(ls->comp->wm);
        xw_damage_outputs_rect(ls->comp, ls->x, ls->y, ls->w, ls->h);
    } else if (ls->configured_sent) {
        /* anchors changed before mapping: update the pending configure
         * so the client sizes itself against the new geometry */
        ls_configure(ls);
    }
}

static void ls_set_exclusive_zone(struct wl_client *client,
                                  struct wl_resource *res, int32_t zone) {
    (void)client;
    struct xw_layer_surface *ls = ls_from_res(res);
    ls->exclusive_zone = zone;
    if (ls->mapped)
        xw_wm_recalculate_usable(ls->comp->wm);
}

static void ls_set_margin(struct wl_client *client, struct wl_resource *res,
                          int32_t top, int32_t right, int32_t bottom,
                          int32_t left) {
    (void)client;
    struct xw_layer_surface *ls = ls_from_res(res);
    ls->margin.top = top;
    ls->margin.right = right;
    ls->margin.bottom = bottom;
    ls->margin.left = left;
}

static void ls_set_keyboard_interactivity(struct wl_client *client,
                                          struct wl_resource *res,
                                          uint32_t interactivity) {
    (void)client;
    struct xw_layer_surface *ls = ls_from_res(res);
    if (interactivity >
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
        wl_resource_post_error(
            res, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
            "invalid keyboard interactivity");
        return;
    }
    ls->keyboard_interactivity = interactivity;
    /* exclusive layer appearing on top: refocus */
    if (ls->mapped && interactivity == 1) {
        struct xw_seat *seat = xw_seat_first(ls->comp);
        if (seat)
            xw_seat_set_kb_focus(seat, ls->surface);
    }
}

static void ls_get_popup(struct wl_client *client, struct wl_resource *res,
                         struct wl_resource *popup) {
    /* assign this layer surface as the popup's parent */
    (void)client;
    struct xw_layer_surface *ls = ls_from_res(res);
    struct xw_popup *p = wl_resource_get_user_data(popup);
    if (!ls || !p)
        return;
    p->parent = ls->surface;
    xw_popup_reposition(p);
}

static void ls_ack_configure(struct wl_client *client,
                             struct wl_resource *res, uint32_t serial) {
    (void)client;
    (void)res;
    (void)serial; /* configure serials are informational here */
}

static void layer_surface_resource_destroy(struct wl_resource *res) {
    struct xw_layer_surface *ls = ls_from_res(res);
    if (!ls)
        return;
    xw_layer_surface_destroy(ls);
}

void xw_layer_surface_destroy(struct xw_layer_surface *ls) {
    if (!ls)
        return;
    if (ls->mapped) {
        xw_damage_outputs_rect(ls->comp, ls->x, ls->y, ls->w, ls->h);
        ls->mapped = false;
    }
    struct xw_seat *seat = xw_seat_first(ls->comp);
    if (seat && ls->surface && seat->kb_focus == ls->surface)
        xw_seat_set_kb_focus(seat, NULL);
    if (seat && ls->surface && seat->grab_surface == ls->surface) {
        seat->ptr_grab = NULL;
        seat->grab_surface = NULL;
    }
    wl_list_remove(&ls->link);
    if (ls->surface) {
        ls->surface->role = XW_SURFACE_ROLE_NONE;
        ls->surface->role_data = NULL;
        ls->surface->xdg_surface_res = NULL;
        ls->surface = NULL;
    }
    free(ls);
}

/* ------------------------------------------------------ zwlr_layer_shell */

static void shell_get_layer_surface(struct wl_client *client,
                                    struct wl_resource *res, uint32_t id,
                                    struct wl_resource *surface,
                                    struct wl_resource *output,
                                    uint32_t layer, const char *namespace) {
    (void)res;
    struct xw_compositor *c = g_shell->comp;
    struct xw_surface *s = wl_resource_get_user_data(surface);

    if (s->role != XW_SURFACE_ROLE_NONE) {
        wl_resource_post_error(surface, 0, "surface already has a role");
        return;
    }
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(res, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "invalid layer");
        return;
    }

    /* output resolution: explicit (wl_output resource user data is the
     * struct xw_output) or the first output */
    struct xw_output *o = output ? wl_resource_get_user_data(output) : NULL;
    if (!o) {
        if (wl_list_empty(&c->outputs))
            return;
        o = wl_container_of(c->outputs.next, o, link);
    }

    struct xw_layer_surface *ls = calloc(1, sizeof(*ls));
    if (!ls) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *lres = wl_resource_create(
        client, &zwlr_layer_surface_v1_interface,
        wl_resource_get_version(res), id);
    if (!lres) {
        free(ls);
        wl_client_post_no_memory(client);
        return;
    }
    ls->comp = c;
    ls->surface = s;
    ls->res = lres;
    ls->output = o;
    ls->layer = layer;
    ls->exclusive_zone = -1; /* auto: derived from committed size */
    ls->configured_w = 0;
    ls->configured_h = 0;
    if (namespace && *namespace)
        xw_log(XW_LOG_DEBUG, "layer-shell: namespace '%s'", namespace);

    wl_resource_set_implementation(lres, &layer_surface_impl, ls,
                                   layer_surface_resource_destroy);
    wl_list_insert(c->wm->layers[layer].prev, &ls->link);
    s->role = XW_SURFACE_ROLE_LAYER;
    s->role_data = ls;
    /* no configure here: the client sends anchor/margin/size requests
     * first and commits; the configure is sent in response (with the
     * size computed from the anchors) */
}

static void shell_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct zwlr_layer_shell_v1_interface layer_shell_impl = {
    .get_layer_surface = shell_get_layer_surface,
    .destroy = shell_destroy,
};

static void bind_layer_shell(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id) {
    if (version > LAYER_SHELL_VERSION)
        version = LAYER_SHELL_VERSION;
    struct wl_resource *res = wl_resource_create(
        client, &zwlr_layer_shell_v1_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &layer_shell_impl, data, NULL);
}

/* ----------------------------------------------------------- role hooks */

void xw_layer_role_commit(struct xw_surface *s) {
    struct xw_layer_surface *ls = s->role_data;
    if (!ls)
        return;
    if (!ls->configured_sent) {
        /* first commit: respond with the configure (anchor-derived
         * size when anchored to opposite edges) */
        ls->configured_sent = true;
        ls_configure(ls);
        return;
    }
    ls_layout(ls);
    if (!ls->mapped) {
        /* a commit with a buffer maps the layer surface */
        if (s->buf_w > 0 || s->buf_h > 0 || ls->configured_w > 0) {
            ls->mapped = true;
            /* a keyboard-interactivity layer claims the keyboard when
             * it maps (the request typically arrives before mapping) */
            if (ls->keyboard_interactivity) {
                struct xw_seat *seat = xw_seat_first(s->comp);
                if (seat)
                    xw_seat_set_kb_focus(seat, s);
            }
            xw_wm_recalculate_usable(s->comp->wm);
            xw_damage_outputs_rect(s->comp, ls->x, ls->y, ls->w, ls->h);
        }
    } else {
        xw_damage_outputs_rect(s->comp, ls->x, ls->y, ls->w, ls->h);
        xw_wm_recalculate_usable(s->comp->wm);
        xw_damage_outputs_rect(s->comp, ls->x, ls->y, ls->w, ls->h);
    }
}

void xw_layer_role_unmap(struct xw_surface *s) {
    struct xw_layer_surface *ls = s->role_data;
    if (ls && ls->mapped) {
        xw_damage_outputs_rect(s->comp, ls->x, ls->y, ls->w, ls->h);
        ls->mapped = false;
        xw_wm_recalculate_usable(s->comp->wm);
    }
}

void xw_layer_role_destroy(struct xw_surface *s) {
    /* called when the wl_surface dies with the layer role attached:
     * destroy the zwlr_layer_surface resource; its destructor performs
     * the teardown and frees the layer surface */
    struct xw_layer_surface *ls = s->role_data;
    if (!ls)
        return;
    s->role = XW_SURFACE_ROLE_NONE;
    s->role_data = NULL;
    s->xdg_surface_res = NULL;
    if (ls->res) {
        struct wl_resource *r = ls->res;
        ls->res = NULL;
        wl_resource_destroy(r);
    } else {
        xw_layer_surface_destroy(ls);
    }
}

/* ---------------------------------------------------------------- init */

void xw_layer_shell_init(struct xw_compositor *c) {
    g_shell = calloc(1, sizeof(*g_shell));
    if (!g_shell)
        return;
    g_shell->comp = c;
    g_shell->global = wl_global_create(
        c->display, &zwlr_layer_shell_v1_interface, LAYER_SHELL_VERSION,
        g_shell, bind_layer_shell);
    if (!g_shell->global)
        xw_log(XW_LOG_ERROR, "layer shell global creation failed");
}

void xw_layer_shell_fin(struct xw_compositor *c) {
    (void)c;
    if (g_shell) {
        if (g_shell->global)
            wl_global_destroy(g_shell->global);
        free(g_shell);
        g_shell = NULL;
    }
}
