/* xw-subcompositor.c — wl_subcompositor / wl_subsurface (core protocol).
 *
 * Real toolkits require wl_subcompositor at startup (foot aborts with
 * "no sub compositor" without it; GTK/Qt/Chromium use subsurfaces for
 * popups, video overlays, IME panels and decoration). Subsurfaces are
 * wl_surface children of another surface, positioned parent-relative,
 * stacked above or below the parent's own buffer, updated synchronously
 * (state applied at the parent's commit) or asynchronously.
 *
 * Rendering: xw-render.c walks parent->subsurfaces around the parent
 * blit (below-parent children first, then the parent buffer, then
 * above-parent children, tail of the list = topmost). Input: the seat's
 * hit-test drills into children topmost-first after the parent.
 *
 * The wl_subsurface requests:
 *   destroy / set_position / place_above / place_below / set_sync /
 *   set_desync — the full protocol set; nothing is left NULL (a NULL
 *   request handler makes libwayland abort the whole compositor).
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- helpers */

static struct xw_subsurface *sub_of(struct xw_surface *s) {
    return s->role == XW_SURFACE_ROLE_SUBSURFACE ? s->role_data : NULL;
}

/* damage the child's rect at its committed position */
static void damage_subsurface(struct xw_subsurface *sub) {
    int px = 0, py = 0;
    xw_surface_get_pos(sub->parent, &px, &py, NULL, NULL);
    int sc = sub->surface->scale > 0 ? sub->surface->scale : 1;
    xw_damage_outputs_rect(sub->comp, px + sub->x, py + sub->y,
                           sub->surface->buf_w / sc,
                           sub->surface->buf_h / sc);
}

/* global position + extent of a subsurface (parent pos + offset) */
static void subsurface_pos(struct xw_subsurface *sub, int *x, int *y,
                           int *w, int *h) {
    int px = 0, py = 0;
    xw_surface_get_pos(sub->parent, &px, &py, NULL, NULL);
    if (x) *x = px + sub->x;
    if (y) *y = py + sub->y;
    int sc = sub->surface->scale > 0 ? sub->surface->scale : 1;
    if (w) *w = sub->surface->buf_w / sc;
    if (h) *h = sub->surface->buf_h / sc;
}

/* ---------------------------------------------------- wl_subsurface impl */

static void sub_destroy_req(struct wl_client *client,
                            struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void sub_set_position(struct wl_client *client,
                             struct wl_resource *res, int32_t x, int32_t y) {
    (void)client;
    struct xw_subsurface *sub = wl_resource_get_user_data(res);
    if (!sub)
        return;
    sub->pending_x = x;
    sub->pending_y = y;
    if (!sub->synced) {
        damage_subsurface(sub); /* old rect */
        sub->x = x;
        sub->y = y;
        damage_subsurface(sub); /* new rect */
    }
}

static void sub_place(struct wl_resource *res, struct wl_resource *sibling_res,
                      bool above) {
    struct xw_subsurface *sub = wl_resource_get_user_data(res);
    if (!sub)
        return;
    struct xw_surface *parent = sub->parent;
    if (!parent)
        return;
    /* detach, then reinsert at the requested stacking slot */
    wl_list_remove(&sub->parent_link);
    wl_list_init(&sub->parent_link);
    struct xw_subsurface *sib = NULL;
    if (sibling_res) {
        /* validate the sibling: a subsurface of the SAME parent */
        struct xw_surface *sib_surface = wl_resource_get_user_data(sibling_res);
        if (sib_surface) {
            sib = sub_of(sib_surface);
            if (sib && sib->parent != parent)
                sib = NULL; /* foreign sibling: treat as NULL */
        }
    }
    if (above) {
        if (sib && sib != sub)
            wl_list_insert(&sib->parent_link, &sub->parent_link);
        else
            wl_list_insert(parent->subsurfaces.prev, &sub->parent_link);
    } else {
        if (sib && sib != sub)
            wl_list_insert(sib->parent_link.prev, &sub->parent_link);
        else
            wl_list_insert(&parent->subsurfaces, &sub->parent_link);
    }
    /* both requests also place the subsurface above the parent buffer
     * again unless the client asked to go below via place_below(NULL) */
    sub->below_parent = false;
    damage_subsurface(sub);
}

static void sub_place_above(struct wl_client *client,
                            struct wl_resource *res,
                            struct wl_resource *sibling) {
    (void)client;
    sub_place(res, sibling, true);
}

static void sub_place_below(struct wl_client *client,
                            struct wl_resource *res,
                            struct wl_resource *sibling) {
    (void)client;
    struct xw_subsurface *sub = wl_resource_get_user_data(res);
    sub_place(res, sibling, false);
    if (!sibling && sub)
        sub->below_parent = true; /* below the parent's own buffer */
}

static void sub_set_sync(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_subsurface *sub = wl_resource_get_user_data(res);
    if (sub)
        sub->synced = true;
}

static void sub_set_desync(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_subsurface *sub = wl_resource_get_user_data(res);
    if (sub && !sub->synced)
        return;
    if (sub) {
        sub->synced = false;
        if (sub->has_pending) {
            /* pending state becomes effective right away */
            sub->has_pending = false;
            damage_subsurface(sub);
        }
    }
}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = sub_destroy_req,
    .set_position = sub_set_position,
    .place_above = sub_place_above,
    .place_below = sub_place_below,
    .set_sync = sub_set_sync,
    .set_desync = sub_set_desync,
};

/* role teardown for the child surface: unrole + damage + list removal.
 *
 * OWNERSHIP RULE (learned the hard way, 2026-09-06): the wl_subsurface
 * OBJECT is CLIENT-OWNED — only the client's wl_subsurface.destroy
 * request may retire its id. Teardowns triggered by wl_surface
 * destruction (either side of the pair) must DETACH the association
 * and unrole the child, but leave the wl_subsurface resource alive
 * and inert (user_data nulled below). Reference compositors behave
 * the same way, and real toolkits (Firefox/GTK menu + tooltip
 * teardown) destroy the surfaces first and the subsurface objects
 * later, from a later widget-dispose cycle. */
static void subsurface_destroy(struct xw_subsurface *sub) {
    if (!sub)
        return;
    if (sub->surface) {
        damage_subsurface(sub);
        sub->surface->role = XW_SURFACE_ROLE_NONE;
        sub->surface->role_data = NULL;
        if (sub->surface->mapped)
            sub->surface->mapped = false;
    }
    if (sub->parent)
        wl_list_remove(&sub->parent_link);
    wl_list_remove(&sub->link);
    if (sub->res)
        wl_resource_set_user_data(sub->res, NULL);
    free(sub);
}

static void sub_resource_destroy(struct wl_resource *res) {
    struct xw_subsurface *sub = wl_resource_get_user_data(res);
    if (sub) {
        sub->res = NULL;
        subsurface_destroy(sub);
    }
}

/* -------------------------------------------------- wl_subcompositor impl */

static void sc_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void sc_get_subsurface(struct wl_client *client,
                              struct wl_resource *res, uint32_t id,
                              struct wl_resource *surface_res,
                              struct wl_resource *parent_res) {
    (void)client;
    struct xw_compositor *c = wl_resource_get_user_data(res);
    struct xw_surface *s = wl_resource_get_user_data(surface_res);
    struct xw_surface *parent = wl_resource_get_user_data(parent_res);

    if (!s || !parent) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "invalid surface in get_subsurface");
        return;
    }
    if (s == parent) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "surface cannot be its own subsurface");
        return;
    }
    if (s->role != XW_SURFACE_ROLE_NONE) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "surface already has a role");
        return;
    }
    /* a parent that is itself an unmapped subsurface is fine (it may
     * map later); a parent that is a cursor surface is not a window
     * tree member — reject it to keep render walks sane */
    if (parent->is_cursor) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "parent is a cursor surface");
        return;
    }

    struct xw_subsurface *sub = calloc(1, sizeof(*sub));
    if (!sub) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *sres =
        wl_resource_create(client, &wl_subsurface_interface, 1, id);
    if (!sres) {
        free(sub);
        wl_client_post_no_memory(client);
        return;
    }

    sub->comp = c;
    sub->surface = s;
    sub->parent = parent;
    sub->res = sres;
    sub->synced = true; /* spec default */
    wl_resource_set_implementation(sres, &subsurface_impl, sub,
                                   sub_resource_destroy);

    /* stacking: above the parent, at the tail (topmost sibling) */
    wl_list_insert(parent->subsurfaces.prev, &sub->parent_link);
    wl_list_insert(c->subcomps.prev, &sub->link);

    s->role = XW_SURFACE_ROLE_SUBSURFACE;
    s->role_data = sub;

    xw_log(XW_LOG_DEBUG,
           "subcompositor: subsurface %u under parent %u (synced)",
           wl_resource_get_id(s->res), wl_resource_get_id(parent->res));
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = sc_destroy,
    .get_subsurface = sc_get_subsurface,
};

static void bind_subcompositor(struct wl_client *client, void *data,
                               uint32_t version, uint32_t id) {
    struct xw_compositor *c = data;
    if (version > 1)
        version = 1;
    struct wl_resource *res =
        wl_resource_create(client, &wl_subcompositor_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &subcompositor_impl, c, NULL);
}

void xw_subcompositor_init(struct xw_compositor *c) {
    c->g_subcompositor = wl_global_create(c->display,
                                          &wl_subcompositor_interface, 1, c,
                                          bind_subcompositor);
    if (!c->g_subcompositor)
        xw_log(XW_LOG_ERROR, "wl_subcompositor global creation failed");
}

void xw_subcompositor_fin(struct xw_compositor *c) {
    if (c->g_subcompositor) {
        wl_global_destroy(c->g_subcompositor);
        c->g_subcompositor = NULL;
    }
}

/* --------------------------------------------------- parent commit hooks */

void xw_subsurface_parent_committed(struct xw_surface *parent) {
    struct xw_subsurface *sub;
    wl_list_for_each(sub, &parent->subsurfaces, parent_link) {
        /* apply the child's pending position; a synced child that
         * committed a buffer becomes visible with this parent commit */
        sub->x = sub->pending_x;
        sub->y = sub->pending_y;
        if (sub->has_pending || sub->surface->buf_w > 0) {
            sub->has_pending = false;
            sub->surface->mapped = true;
            damage_subsurface(sub);
        }
    }
}

void xw_subsurface_parent_destroyed(struct xw_surface *parent) {
    /* Detach every child's subsurface role: the wl_surface objects
     * stay alive (client-owned) and lose their role. The wl_subsurface
     * objects stay alive too — destroying them here (the old
     * "the client's object is defunct" wl_resource_destroy) sent
     * delete_id for an object the client still held; the client's
     * later wl_subsurface.destroy then failed demarshal with
     * "invalid object", the server posted a fatal protocol error, and
     * libwayland killed the whole client (the physical
     * "browser dies on interaction" crash: Firefox parent down, every
     * child printing "Exiting due to channel error", the compositor
     * logging "error in client communication"). */
    struct xw_subsurface *sub, *tmp;
    wl_list_for_each_safe(sub, tmp, &parent->subsurfaces, parent_link)
        subsurface_destroy(sub);
}

/* ------------------------------------------------ surface role dispatch */

/* called from xw-surface.c: a SUBSURFACE-role surface committed */
void xw_subsurface_role_commit(struct xw_surface *s) {
    struct xw_subsurface *sub = sub_of(s);
    if (!sub || !sub->parent)
        return;
    bool has_buffer = s->buf_w > 0 && s->buf_h > 0;
    if (sub->synced) {
        /* state applies at the parent's next commit; the buffer is
         * already stored by the generic commit path */
        sub->has_pending = true;
        return;
    }
    /* desync: effective immediately */
    sub->x = sub->pending_x;
    sub->y = sub->pending_y;
    if (has_buffer) {
        s->mapped = true;
        damage_subsurface(sub);
    } else {
        if (s->mapped)
            damage_subsurface(sub);
        s->mapped = false;
    }
}

/* called from xw-surface.c: a SUBSURFACE-role surface is destroyed */
void xw_subsurface_role_destroy(struct xw_surface *s) {
    /* the child surface died: detach the association, but the
     * wl_subsurface object stays client-owned (see the ownership
     * rule above subsurface_destroy) */
    struct xw_subsurface *sub = sub_of(s);
    if (!sub)
        return;
    subsurface_destroy(sub);
}

/* called from xw-surface.c: position query */
void xw_subsurface_get_pos(struct xw_surface *s, int *x, int *y, int *w,
                           int *h) {
    struct xw_subsurface *sub = sub_of(s);
    if (!sub) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    subsurface_pos(sub, x, y, w, h);
}

/* called from the seat hit-test: topmost subsurface of `parent`
 * (recursively) containing the point, else the parent itself */
struct xw_surface *xw_subsurface_at(struct xw_surface *parent, int gx, int gy) {
    struct xw_subsurface *sub;
    /* children above the parent, topmost (tail) first */
    wl_list_for_each_reverse(sub, &parent->subsurfaces, parent_link) {
        if (sub->below_parent)
            continue;
        if (!sub->surface || !sub->surface->mapped)
            continue;
        struct xw_surface *hit = xw_subsurface_at(sub->surface, gx, gy);
        if (hit)
            return hit;
    }
    if (xw_surface_has_input_at(parent, gx, gy))
        return parent;
    /* children below the parent still take input where the parent has
     * no input region */
    wl_list_for_each_reverse(sub, &parent->subsurfaces, parent_link) {
        if (!sub->below_parent)
            continue;
        if (!sub->surface || !sub->surface->mapped)
            continue;
        struct xw_surface *hit = xw_subsurface_at(sub->surface, gx, gy);
        if (hit)
            return hit;
    }
    return NULL;
}
