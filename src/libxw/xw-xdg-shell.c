/* xw-xdg-shell.c — xdg_wm_base / xdg_surface / xdg_toplevel / xdg_popup.
 *
 * Window lifecycle follows the protocol:
 *   get_toplevel → commit → configure(0,0) → ack → commit-with-buffer → map
 *   state change → configure(size,states) → ack → commit → apply
 *
 * Popups are placed from the positioner (anchor point + gravity + offset,
 * then flip/slide constraint adjustment against the usable area of the
 * output containing the anchor point) and can grab the seat.
 *
 * xw_role_commit/destroy/unmap dispatch over all surface roles here;
 * layer-shell roles are forwarded to xw-layer-shell.c.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

#define XDG_VERSION 5

struct xw_xdg_surface {
    struct wl_resource *res;   /* xdg_surface */
    struct xw_surface *surface;
    struct xw_compositor *comp;
};

/* ------------------------------------------------------------ positioner */

struct xw_positioner {
    int32_t ax, ay, aw, ah; /* anchor rect, parent-geometry coords */
    uint32_t anchor;
    uint32_t gravity;
    uint32_t constraint;
    int32_t off_x, off_y;
    int32_t size_w, size_h;
    bool size_set, anchor_set;
};

static void pos_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}
static void pos_set_size(struct wl_client *client, struct wl_resource *res,
                         int32_t w, int32_t h) {
    (void)client;
    struct xw_positioner *p = wl_resource_get_user_data(res);
    if (w < 1 || h < 1) {
        wl_resource_post_error(res, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "positioner size must be positive");
        return;
    }
    p->size_w = w;
    p->size_h = h;
    p->size_set = true;
}
static void pos_set_anchor_rect(struct wl_client *client,
                                struct wl_resource *res, int32_t x, int32_t y,
                                int32_t w, int32_t h) {
    (void)client;
    struct xw_positioner *p = wl_resource_get_user_data(res);
    if (w < 0 || h < 0) {
        wl_resource_post_error(res, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "positioner anchor rect size must not be "
                               "negative");
        return;
    }
    p->ax = x;
    p->ay = y;
    p->aw = w;
    p->ah = h;
    p->anchor_set = true;
}
static void pos_set_anchor(struct wl_client *client, struct wl_resource *res,
                           uint32_t anchor) {
    (void)client;
    struct xw_positioner *p = wl_resource_get_user_data(res);
    if (anchor > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) {
        wl_resource_post_error(res, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "invalid anchor");
        return;
    }
    p->anchor = anchor;
}
static void pos_set_gravity(struct wl_client *client, struct wl_resource *res,
                            uint32_t gravity) {
    (void)client;
    struct xw_positioner *p = wl_resource_get_user_data(res);
    if (gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT) {
        wl_resource_post_error(res, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "invalid gravity");
        return;
    }
    p->gravity = gravity;
}
static void pos_set_constraint(struct wl_client *client,
                               struct wl_resource *res,
                               uint32_t constraint_adjustment) {
    (void)client;
    struct xw_positioner *p = wl_resource_get_user_data(res);
    if (constraint_adjustment > 63) {
        wl_resource_post_error(res, XDG_POSITIONER_ERROR_INVALID_INPUT,
                               "invalid constraint adjustment");
        return;
    }
    p->constraint = constraint_adjustment;
}
static void pos_set_offset(struct wl_client *client, struct wl_resource *res,
                           int32_t x, int32_t y) {
    (void)client;
    struct xw_positioner *p = wl_resource_get_user_data(res);
    p->off_x = x;
    p->off_y = y;
}
static void pos_set_reactive(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    (void)res; /* accepted: positions recomputed on parent moves */
}
static void pos_set_parent_size(struct wl_client *client,
                                struct wl_resource *res, int32_t pw,
                                int32_t ph) {
    (void)client;
    (void)res;
    (void)pw;
    (void)ph;
}
static void pos_set_parent_configure(struct wl_client *client,
                                     struct wl_resource *res, uint32_t serial) {
    (void)client;
    (void)res;
    (void)serial;
}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = pos_destroy,
    .set_size = pos_set_size,
    .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_anchor,
    .set_gravity = pos_set_gravity,
    .set_constraint_adjustment = pos_set_constraint,
    .set_offset = pos_set_offset,
    .set_reactive = pos_set_reactive,
    .set_parent_size = pos_set_parent_size,
    .set_parent_configure = pos_set_parent_configure,
};

static void positioner_resource_destroy(struct wl_resource *res) {
    free(wl_resource_get_user_data(res));
}

/* ------------------------------------------------------------ toplevel */

static struct xw_window *window_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void toplevel_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void toplevel_set_parent(struct wl_client *client,
                                struct wl_resource *res,
                                struct wl_resource *parent) {
    (void)client;
    (void)res;
    (void)parent; /* transient stacking accepted, not applied (v0) */
}

static void toplevel_set_title(struct wl_client *client,
                               struct wl_resource *res, const char *title) {
    (void)client;
    struct xw_window *w = window_from_res(res);
    if (!w)
        return;
    snprintf(w->title, sizeof(w->title), "%s", title ? title : "");
    xw_foreign_toplevel_notify(w->comp, w);
}

static void toplevel_set_app_id(struct wl_client *client,
                                struct wl_resource *res, const char *app_id) {
    (void)client;
    struct xw_window *w = window_from_res(res);
    if (!w)
        return;
    snprintf(w->app_id, sizeof(w->app_id), "%s", app_id ? app_id : "");
    xw_foreign_toplevel_notify(w->comp, w);
}

static void toplevel_show_menu(struct wl_client *client,
                               struct wl_resource *res,
                               struct wl_resource *seat, uint32_t serial,
                               int32_t x, int32_t y) {
    /* server-side window menus are a documented v0 gap: clients render
     * their own menus (all xw clients do) */
    (void)client;
    (void)res;
    (void)seat;
    (void)serial;
    (void)x;
    (void)y;
    xw_log(XW_LOG_DEBUG, "xdg: show_window_menu not implemented (v0)");
}

static void toplevel_move(struct wl_client *client, struct wl_resource *res,
                          struct wl_resource *seat, uint32_t serial) {
    (void)client;
    (void)serial;
    struct xw_window *w = window_from_res(res);
    if (!w || !w->comp->wm)
        return;
    struct xw_seat *s = xw_seat_first(w->comp);
    (void)seat;
    if (!s || !s->ptr_grab) /* interactive move needs a button held */
        return;
    xw_wm_interactive_begin_move(w->comp->wm, w, s->cursor_x, s->cursor_y);
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *res,
                            struct wl_resource *seat, uint32_t serial,
                            uint32_t edges) {
    (void)client;
    (void)serial;
    struct xw_window *w = window_from_res(res);
    if (!w || !w->comp->wm)
        return;
    struct xw_seat *s = xw_seat_first(w->comp);
    (void)seat;
    if (!s || !s->ptr_grab)
        return;
    int e = 0;
    if (edges & XDG_TOPLEVEL_RESIZE_EDGE_LEFT) e |= XW_EDGE_L;
    if (edges & XDG_TOPLEVEL_RESIZE_EDGE_RIGHT) e |= XW_EDGE_R;
    if (edges & XDG_TOPLEVEL_RESIZE_EDGE_TOP) e |= XW_EDGE_T;
    if (edges & XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM) e |= XW_EDGE_B;
    xw_wm_interactive_begin_resize(w->comp->wm, w, e, s->cursor_x,
                                   s->cursor_y);
}

static void toplevel_set_max_size(struct wl_client *client,
                                  struct wl_resource *res, int32_t w,
                                  int32_t h) {
    (void)client;
    struct xw_window *win = window_from_res(res);
    if (win) {
        win->max_w = w;
        win->max_h = h;
    }
}

static void toplevel_set_min_size(struct wl_client *client,
                                  struct wl_resource *res, int32_t w,
                                  int32_t h) {
    (void)client;
    struct xw_window *win = window_from_res(res);
    if (win) {
        win->min_w = w;
        win->min_h = h;
    }
}

static void toplevel_set_maximized(struct wl_client *client,
                                   struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_from_res(res);
    if (w && w->comp->wm)
        xw_wm_maximize(w->comp->wm, w, true);
}

static void toplevel_unset_maximized(struct wl_client *client,
                                     struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_from_res(res);
    if (w && w->comp->wm)
        xw_wm_maximize(w->comp->wm, w, false);
}

static void toplevel_set_fullscreen(struct wl_client *client,
                                    struct wl_resource *res,
                                    struct wl_resource *output) {
    (void)client;
    (void)output; /* fullscreen on the window's current output (v0) */
    struct xw_window *w = window_from_res(res);
    if (w && w->comp->wm)
        xw_wm_fullscreen(w->comp->wm, w, true);
}

static void toplevel_unset_fullscreen(struct wl_client *client,
                                      struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_from_res(res);
    if (w && w->comp->wm)
        xw_wm_fullscreen(w->comp->wm, w, false);
}

static void toplevel_set_minimized(struct wl_client *client,
                                   struct wl_resource *res) {
    (void)client;
    struct xw_window *w = window_from_res(res);
    if (w && w->comp->wm)
        xw_wm_minimize(w->comp->wm, w, true);
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = toplevel_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

static void toplevel_resource_destroy(struct wl_resource *res) {
    /* actual role teardown happens in the xdg_surface destructor; this
     * handler only detaches from the window */
    struct xw_window *w = wl_resource_get_user_data(res);
    if (w && w->toplevel_res == res)
        w->toplevel_res = NULL;
}

/* --------------------------------------------------------------- popup */

static void popup_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void popup_grab(struct wl_client *client, struct wl_resource *res,
                       struct wl_resource *seat, uint32_t serial);

static void popup_reposition(struct wl_client *client, struct wl_resource *res,
                             struct wl_resource *positioner, uint32_t token);

static const struct xdg_popup_interface popup_impl = {
    .destroy = popup_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

static struct xw_popup *popup_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

/* ------------------------------------------------- popup geometry math */

/* anchor point from the anchor rect + anchor enum */
static void positioner_anchor_point(const struct xw_positioner *p, int *px,
                                    int *py) {
    int x = p->ax, y = p->ay, w = p->aw, h = p->ah;
    switch (p->anchor) {
    case XDG_POSITIONER_ANCHOR_TOP:
        *px = x + w / 2;
        *py = y;
        break;
    case XDG_POSITIONER_ANCHOR_BOTTOM:
        *px = x + w / 2;
        *py = y + h;
        break;
    case XDG_POSITIONER_ANCHOR_LEFT:
        *px = x;
        *py = y + h / 2;
        break;
    case XDG_POSITIONER_ANCHOR_RIGHT:
        *px = x + w;
        *py = y + h / 2;
        break;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT:
        *px = x;
        *py = y;
        break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
        *px = x;
        *py = y + h;
        break;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
        *px = x + w;
        *py = y;
        break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
        *px = x + w;
        *py = y + h;
        break;
    default: /* NONE: center of the anchor rect */
        *px = x + w / 2;
        *py = y + h / 2;
        break;
    }
}

/* gravity: the popup edge/corner that touches the anchor point.
 * LEFT → child's left edge at the anchor (x = px - cw); RIGHT → right
 * edge (x = px); TOP → top edge (y = py - ch); BOTTOM → bottom edge
 * (y = py); corners combine both axes; NONE → centered. */
static void gravity_offset(uint32_t gravity, int cw, int ch, int *ox,
                           int *oy) {
    switch (gravity) {
    case XDG_POSITIONER_GRAVITY_LEFT:
    case XDG_POSITIONER_GRAVITY_TOP_LEFT:
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
        *ox = -cw;
        break;
    case XDG_POSITIONER_GRAVITY_RIGHT:
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
        *ox = 0;
        break;
    default: /* NONE: centered */
        *ox = -cw / 2;
        break;
    }
    switch (gravity) {
    case XDG_POSITIONER_GRAVITY_TOP:
    case XDG_POSITIONER_GRAVITY_TOP_LEFT:
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
        *oy = -ch;
        break;
    case XDG_POSITIONER_GRAVITY_BOTTOM:
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
        *oy = 0;
        break;
    default: /* NONE: centered */
        *oy = -ch / 2;
        break;
    }
}

/* usable area of the output containing a global point */
static struct xw_output *output_containing(struct xw_compositor *c, int x,
                                           int y) {
    struct xw_output *best = NULL, *o;
    int64_t best_area = -1;
    wl_list_for_each(o, &c->outputs, link) {
        int x0 = x > o->x ? x : o->x, y0 = y > o->y ? y : o->y;
        int x1 = x < o->x + o->width ? x : o->x + o->width;
        int y1 = y < o->y + o->height ? y : o->y + o->height;
        if (x1 > x0 && y1 > y0) {
            int64_t a = (int64_t)(x1 - x0) * (y1 - y0);
            if (a > best_area) {
                best_area = a;
                best = o;
            }
        }
    }
    return best;
}

/* mirror gravity on one axis (used by the FLIP constraint adjustment) */
static uint32_t mirror_gravity_x(uint32_t g) {
    switch (g) {
    case XDG_POSITIONER_GRAVITY_LEFT: return XDG_POSITIONER_GRAVITY_RIGHT;
    case XDG_POSITIONER_GRAVITY_RIGHT: return XDG_POSITIONER_GRAVITY_LEFT;
    case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
    default: return g;
    }
}
static uint32_t mirror_gravity_y(uint32_t g) {
    switch (g) {
    case XDG_POSITIONER_GRAVITY_TOP: return XDG_POSITIONER_GRAVITY_BOTTOM;
    case XDG_POSITIONER_GRAVITY_BOTTOM: return XDG_POSITIONER_GRAVITY_TOP;
    case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
    default: return g;
    }
}

/* Compute the popup position in global coordinates, applying flip and
 * slide constraint adjustments. px/py: anchor point in global coords. */
static void compute_popup_position(struct xw_compositor *c,
                                   const struct xw_positioner *p, int px,
                                   int py, int cw, int ch, int *out_x,
                                   int *out_y) {
    int gox, goy;
    gravity_offset(p->gravity, cw, ch, &gox, &goy);
    int x = px + gox + p->off_x;
    int y = py + goy + p->off_y;

    struct xw_output *o = output_containing(c, px, py);
    int ux = 0, uy = 0, uw = 0, uh = 0;
    if (o) {
        ux = o->usable.x;
        uy = o->usable.y;
        uw = o->usable.w;
        uh = o->usable.h;
    } else {
        wl_list_for_each(o, &c->outputs, link) { /* layout bounds */
            uw = o->x + o->width;
            uh = o->y + o->height;
        }
    }
    if (uw > 0 && uh > 0) {
        /* flip X: mirror gravity, recompute the x placement */
        if ((p->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) &&
            (x < ux || x + cw > ux + uw)) {
            int fx, fy;
            gravity_offset(mirror_gravity_x(p->gravity), cw, ch, &fx, &fy);
            x = px + fx + p->off_x;
        }
        /* flip Y */
        if ((p->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) &&
            (y < uy || y + ch > uy + uh)) {
            int fx, fy;
            gravity_offset(mirror_gravity_y(p->gravity), cw, ch, &fx, &fy);
            y = py + fy + p->off_y;
        }
        /* slide X/Y: shift into the usable area */
        if (p->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X) {
            if (x + cw > ux + uw)
                x = ux + uw - cw;
            if (x < ux)
                x = ux;
        }
        if (p->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y) {
            if (y + ch > uy + uh)
                y = uy + uh - ch;
            if (y < uy)
                y = uy;
        }
        if (p->constraint & (XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y))
            xw_log(XW_LOG_DEBUG,
                   "xdg: resize constraint adjustment unimplemented");
    }
    *out_x = x;
    *out_y = y;
}

/* ------------------------------------------------------ popup lifecycle */

static void popup_send_configure(struct xw_popup *p) {
    if (!p->res)
        return;
    int parent_x = 0, parent_y = 0;
    if (p->parent)
        xw_surface_get_pos(p->parent, &parent_x, &parent_y, NULL, NULL);
    xdg_popup_send_configure(p->res, p->anchor_x - parent_x,
                             p->anchor_y - parent_y, p->w, p->h);
    uint32_t serial = wl_display_next_serial(p->comp->display);
    xdg_surface_send_configure(p->xdg_surface_res, serial);
    if (p->surface) {
        p->surface->pending_config = true;
        p->surface->pending_serial = serial;
    }
}

/* full placement: parent origin + anchor rect + anchor point + gravity +
 * offset + constraint adjustment */
static void popup_place(struct xw_popup *p) {
    struct xw_positioner *pos = p->pos;
    if (p->parent) {
        int px, py;
        xw_surface_get_pos(p->parent, &px, &py, NULL, NULL);
        int apx, apy;
        /* anchor point is in parent-surface coordinates (rect origin
         * included) — do not add the rect origin twice */
        positioner_anchor_point(pos, &apx, &apy);
        compute_popup_position(p->comp, pos, px + apx, py + apy, p->w, p->h,
                               &p->anchor_x, &p->anchor_y);
    } else {
        struct xw_seat *seat = xw_seat_first(p->comp);
        int cx = seat ? seat->cursor_x : 0, cy = seat ? seat->cursor_y : 0;
        compute_popup_position(p->comp, pos, cx, cy, p->w, p->h, &p->anchor_x,
                               &p->anchor_y);
    }
}

void xw_popup_reposition(struct xw_popup *p) {
    if (!p)
        return;
    popup_place(p);
    xw_damage_outputs_rect(p->comp, p->anchor_x, p->anchor_y, p->w, p->h);
    popup_send_configure(p);
}

void xw_popup_dismiss(struct xw_popup *p) {
    if (!p || !p->res)
        return;
    xw_log(XW_LOG_DEBUG, "xdg: dismissing popup");
    if (p->mapped) {
        xw_damage_outputs_rect(p->comp, p->anchor_x, p->anchor_y, p->w, p->h);
        p->mapped = false;
    }
    xdg_popup_send_popup_done(p->res);
    /* release the seat grab if this popup owns it */
    struct xw_seat *seat = xw_seat_first(p->comp);
    if (seat && seat->grab_surface == p->surface) {
        seat->ptr_grab = NULL;
        seat->grab_surface = NULL;
        struct xw_window *fw = p->comp->wm ? p->comp->wm->focused : NULL;
        xw_seat_set_kb_focus(seat, fw ? fw->surface : NULL);
    }
}

static void popup_resource_destroy(struct wl_resource *res) {
    struct xw_popup *p = wl_resource_get_user_data(res);
    if (!p)
        return;
    /* full teardown happens via the xdg_surface destructor; here only if
     * the popup object died first */
    if (p->res == res) {
        if (p->mapped) {
            xw_damage_outputs_rect(p->comp, p->anchor_x, p->anchor_y, p->w,
                                   p->h);
            p->mapped = false;
        }
        struct xw_seat *seat = xw_seat_first(p->comp);
        if (seat && seat->grab_surface == p->surface) {
            seat->ptr_grab = NULL;
            seat->grab_surface = NULL;
        }
        p->res = NULL;
        wl_list_remove(&p->link);
        if (p->surface) {
            p->surface->role = XW_SURFACE_ROLE_NONE;
            p->surface->role_data = NULL;
        }
        free(p->pos);
        free(p);
    }
}

static void popup_grab(struct wl_client *client, struct wl_resource *res,
                       struct wl_resource *seat, uint32_t serial) {
    (void)client;
    (void)seat;
    struct xw_popup *p = popup_from_res(res);
    if (!p || !p->surface || !p->mapped)
        return;
    struct xw_compositor *c = p->comp;
    struct xw_seat *s = xw_seat_first(c);
    if (!s)
        return;
    if (serial != s->serial)
        xw_log(XW_LOG_DEBUG, "xdg: popup grab serial %u (last %u)", serial,
               s->serial);
    p->grabbed = true;
    /* find the grabbing client's pointer resource */
    struct wl_resource *pr = NULL;
    wl_list_for_each(pr, &s->pointers, link) {
        if (wl_resource_get_client(pr) == client)
            break;
    }
    s->ptr_grab = pr;
    s->ptr_grab_is_drag = false;
    s->grab_surface = p->surface;
    xw_seat_set_kb_focus(s, p->surface);
}

static void popup_reposition(struct wl_client *client, struct wl_resource *res,
                             struct wl_resource *positioner, uint32_t token) {
    (void)client;
    struct xw_popup *p = popup_from_res(res);
    struct xw_positioner *pos = wl_resource_get_user_data(positioner);
    if (!p || !pos || !pos->size_set)
        return;
    p->w = pos->size_w;
    p->h = pos->size_h;
    xw_popup_reposition(p);
    if (wl_resource_get_version(res) >=
        XDG_POPUP_REPOSITIONED_SINCE_VERSION)
        xdg_popup_send_repositioned(res, token);
}

/* --------------------------------------------------------- xdg_surface */

static void xs_destroy(struct wl_client *client, struct wl_resource *res);
static void xs_get_toplevel(struct wl_client *client, struct wl_resource *res,
                            uint32_t id);
static void xs_get_popup(struct wl_client *client, struct wl_resource *res,
                         uint32_t id, struct wl_resource *parent,
                         struct wl_resource *positioner);
static void xs_set_window_geometry(struct wl_client *client,
                                   struct wl_resource *res, int32_t x,
                                   int32_t y, int32_t w, int32_t h);
static void xs_ack_configure(struct wl_client *client, struct wl_resource *res,
                             uint32_t serial);

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xs_destroy,
    .get_toplevel = xs_get_toplevel,
    .get_popup = xs_get_popup,
    .set_window_geometry = xs_set_window_geometry,
    .ack_configure = xs_ack_configure,
};

static struct xw_xdg_surface *xs_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void xdg_surface_resource_destroy(struct wl_resource *res) {
    struct xw_xdg_surface *xs = xs_from_res(res);
    if (!xs)
        return;
    /* if the wl_surface died first (client teardown destroys resources
     * in creation order), it already orphaned us */
    if (!xs->surface) {
        free(xs);
        return;
    }
    /* role teardown: the xdg_surface owns the role object lifetime */
    if (xs->surface->xdg_surface_res != res) {
        free(xs);
        return;
    }
    if (xs->surface->role != XW_SURFACE_ROLE_NONE) {
        if (xs->surface->role == XW_SURFACE_ROLE_XDG_TOPLEVEL) {
            struct xw_window *w = xs->surface->role_data;
            if (w) {
                if (w->toplevel_res) {
                    struct wl_resource *tr = w->toplevel_res;
                    w->toplevel_res = NULL; /* prevent recursion */
                    wl_resource_destroy(tr);
                }
                if (xs->surface->comp->wm)
                    xw_wm_unmanage(xs->surface->comp->wm, w, true);
            }
        } else if (xs->surface->role == XW_SURFACE_ROLE_XDG_POPUP) {
            struct xw_popup *p = xs->surface->role_data;
            if (p) {
                if (p->res) {
                    struct wl_resource *pr = p->res;
                    p->res = NULL;
                    wl_resource_destroy(pr);
                }
                wl_list_remove(&p->link);
                struct xw_seat *seat = xw_seat_first(xs->surface->comp);
                if (seat && seat->grab_surface == p->surface) {
                    seat->ptr_grab = NULL;
                    seat->grab_surface = NULL;
                }
                free(p->pos);
                free(p);
            }
        }
        xs->surface->role = XW_SURFACE_ROLE_NONE;
        xs->surface->role_data = NULL;
        xs->surface->xdg_surface_res = NULL;
    }
    xs->surface = NULL;
    free(xs);
}

static void xs_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void xs_get_toplevel(struct wl_client *client, struct wl_resource *res,
                            uint32_t id) {
    struct xw_xdg_surface *xs = xs_from_res(res);
    struct xw_surface *s = xs->surface;
    if (s->role != XW_SURFACE_ROLE_NONE) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "surface already has a role");
        return;
    }
    struct xw_window *w = calloc(1, sizeof(*w));
    if (!w) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *tl = wl_resource_create(client, &xdg_toplevel_interface,
                                                wl_resource_get_version(res),
                                                id);
    if (!tl) {
        free(w);
        wl_client_post_no_memory(client);
        return;
    }
    w->comp = s->comp;
    w->client = client;
    w->surface = s;
    w->xdg_surface_res = res;
    w->toplevel_res = tl;
    w->ws = s->comp->wm->ws_current;
    w->output = NULL;
    w->geo_x = -1;
    w->geo_y = -1;
    wl_list_init(&w->toplevel_handles);
    wl_resource_set_implementation(tl, &toplevel_impl, w,
                                   toplevel_resource_destroy);
    s->role = XW_SURFACE_ROLE_XDG_TOPLEVEL;
    s->role_data = w;
    s->xdg_surface_res = res;
    xw_wm_manage_toplevel(s->comp->wm, w);
}

static void xs_get_popup(struct wl_client *client, struct wl_resource *res,
                         uint32_t id, struct wl_resource *parent,
                         struct wl_resource *positioner) {
    struct xw_xdg_surface *xs = xs_from_res(res);
    struct xw_surface *s = xs->surface;
    struct xw_positioner *pos = wl_resource_get_user_data(positioner);

    if (s->role != XW_SURFACE_ROLE_NONE) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "surface already has a role");
        return;
    }
    if (!pos || !pos->size_set || !pos->anchor_set) {
        wl_resource_post_error(positioner,
                               XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                               "positioner not fully configured");
        return;
    }

    struct xw_popup *p = calloc(1, sizeof(*p));
    if (!p) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *pr = wl_resource_create(client, &xdg_popup_interface,
                                                wl_resource_get_version(res),
                                                id);
    if (!pr) {
        free(p);
        wl_client_post_no_memory(client);
        return;
    }
    p->comp = s->comp;
    p->surface = s;
    p->res = pr;
    p->xdg_surface_res = res;
    p->pos = malloc(sizeof(*pos));
    if (!p->pos) {
        free(p);
        wl_client_post_no_memory(client);
        return;
    }
    memcpy(p->pos, pos, sizeof(*pos));
    p->w = pos->size_w;
    p->h = pos->size_h;

    /* parent: xdg_surface of a toplevel/popup, or NULL */
    if (parent) {
        struct xw_xdg_surface *pxs = wl_resource_get_user_data(parent);
        p->parent = pxs ? pxs->surface : NULL;
        if (p->parent && p->parent->role == XW_SURFACE_ROLE_NONE)
            p->parent = NULL; /* parent lost its role; treat as unparented */
    }

    /* place the popup (anchor rect is relative to parent geometry) */
    popup_place(p);

    wl_resource_set_implementation(pr, &popup_impl, p,
                                   popup_resource_destroy);
    wl_list_insert(s->comp->popups.prev, &p->link);
    s->role = XW_SURFACE_ROLE_XDG_POPUP;
    s->role_data = p;
    s->xdg_surface_res = res;

    /* initial configure */
    popup_send_configure(p);
}

static void xs_set_window_geometry(struct wl_client *client,
                                   struct wl_resource *res, int32_t x,
                                   int32_t y, int32_t w, int32_t h) {
    (void)client;
    struct xw_xdg_surface *xs = xs_from_res(res);
    struct xw_surface *s = xs->surface;
    if (w <= 0 || h <= 0) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_INVALID_SIZE,
                               "invalid window geometry");
        return;
    }
    if (s->role == XW_SURFACE_ROLE_XDG_TOPLEVEL && s->role_data) {
        struct xw_window *win = s->role_data;
        win->geo_x = x;
        win->geo_y = y;
        win->geo_w = w;
        win->geo_h = h;
        win->geometry_set = true;
        if (win->mapped) {
            xw_wm_damage_window(s->comp->wm, win);
            win->w = w;
            win->h = h;
            xw_wm_damage_window(s->comp->wm, win);
        }
    } else if (s->role == XW_SURFACE_ROLE_XDG_POPUP && s->role_data) {
        struct xw_popup *p = s->role_data;
        p->w = w;
        p->h = h;
    }
}

static void xs_ack_configure(struct wl_client *client, struct wl_resource *res,
                             uint32_t serial) {
    (void)client;
    struct xw_xdg_surface *xs = xs_from_res(res);
    if (!xs || !xs->surface)
        return;
    struct xw_surface *s = xs->surface;
    if (s->pending_config && serial != s->pending_serial)
        xw_log(XW_LOG_DEBUG, "xdg: ack serial %u, expected %u", serial,
               s->pending_serial);
    if (s->role == XW_SURFACE_ROLE_XDG_TOPLEVEL && s->role_data) {
        struct xw_window *w = s->role_data;
        w->acked_serial = serial;
    }
}

/* --------------------------------------------------------- xdg_wm_base */

static void base_destroy(struct wl_client *client, struct wl_resource *res);
static void base_create_positioner(struct wl_client *client,
                                   struct wl_resource *res, uint32_t id);
static void base_get_xdg_surface(struct wl_client *client,
                                 struct wl_resource *res, uint32_t id,
                                 struct wl_resource *surface);
static void base_pong(struct wl_client *client, struct wl_resource *res,
                      uint32_t serial);

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = base_destroy,
    .create_positioner = base_create_positioner,
    .get_xdg_surface = base_get_xdg_surface,
    .pong = base_pong,
};

static void bind_wm_base(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id) {
    if (version > XDG_VERSION)
        version = XDG_VERSION;
    struct wl_resource *res = wl_resource_create(client,
                                                 &xdg_wm_base_interface,
                                                 version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &wm_base_impl, data, NULL);
}

static void base_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void base_create_positioner(struct wl_client *client,
                                   struct wl_resource *res, uint32_t id) {
    struct xw_positioner *p = calloc(1, sizeof(*p));
    if (!p) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *pres =
        wl_resource_create(client, &xdg_positioner_interface,
                           wl_resource_get_version(res), id);
    if (!pres) {
        free(p);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(pres, &positioner_impl, p,
                                   positioner_resource_destroy);
}

static void base_get_xdg_surface(struct wl_client *client,
                                 struct wl_resource *res, uint32_t id,
                                 struct wl_resource *surface) {
    struct xw_compositor *c = wl_resource_get_user_data(res);
    struct xw_surface *s = wl_resource_get_user_data(surface);
    if (!s) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                               "invalid wl_surface");
        return;
    }
    if (s->xdg_surface_res) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_surface already exists for this surface");
        return;
    }
    struct xw_xdg_surface *xs = calloc(1, sizeof(*xs));
    if (!xs) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *xres =
        wl_resource_create(client, &xdg_surface_interface,
                           wl_resource_get_version(res), id);
    if (!xres) {
        free(xs);
        wl_client_post_no_memory(client);
        return;
    }
    xs->res = xres;
    xs->surface = s;
    xs->comp = c;
    wl_resource_set_implementation(xres, &xdg_surface_impl, xs,
                                   xdg_surface_resource_destroy);
}

static void base_pong(struct wl_client *client, struct wl_resource *res,
                      uint32_t serial) {
    (void)client;
    (void)res;
    (void)serial; /* liveness tracking is a backlog item */
}

/* ------------------------------------------------------------ configure */

void xw_xdg_send_configure(struct xw_window *w) {
    if (!w || !w->toplevel_res || !w->xdg_surface_res || !w->surface)
        return;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *st;
    if (w->maximized) {
        st = wl_array_add(&states, sizeof(*st));
        *st = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    if (w->fullscreen) {
        st = wl_array_add(&states, sizeof(*st));
        *st = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    if (w->resizing) {
        st = wl_array_add(&states, sizeof(*st));
        *st = XDG_TOPLEVEL_STATE_RESIZING;
    }
    if (w->activated) {
        st = wl_array_add(&states, sizeof(*st));
        *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    xdg_toplevel_send_configure(w->toplevel_res, w->w, w->h, &states);
    wl_array_release(&states);
    uint32_t serial = wl_display_next_serial(w->comp->display);
    xdg_surface_send_configure(w->xdg_surface_res, serial);
    w->last_serial = serial;
    w->have_config = true;
    w->surface->pending_config = true;
    w->surface->pending_serial = serial;
}

/* ------------------------------------------------------------ role glue */

static void toplevel_apply_commit(struct xw_surface *s) {
    struct xw_window *w = s->role_data;
    struct xw_compositor *c = s->comp;
    int sc = s->scale > 0 ? s->scale : 1;
    int bw = s->buf_w / sc, bh = s->buf_h / sc;
    if (w->geometry_set && w->geo_w > 0 && w->geo_h > 0) {
        bw = w->geo_w;
        bh = w->geo_h;
    }
    if (bw < 1 || bh < 1)
        return;

    if (!w->mapped) {
        w->w = bw;
        w->h = bh;
        xw_wm_window_map(c->wm, w);
    } else if (w->w != bw || w->h != bh) {
        xw_wm_damage_window(c->wm, w);
        w->w = bw;
        w->h = bh;
        xw_wm_damage_window(c->wm, w);
        xw_foreign_toplevel_notify(c, w);
    }
}

static void popup_apply_commit(struct xw_surface *s) {
    struct xw_popup *p = s->role_data;
    if (!p->mapped) {
        int sc = s->scale > 0 ? s->scale : 1;
        if (s->buf_w > 0 && s->buf_h > 0) {
            p->w = s->buf_w / sc;
            p->h = s->buf_h / sc;
        }
        p->mapped = true;
        xw_damage_outputs_rect(s->comp, p->anchor_x, p->anchor_y, p->w, p->h);
    }
}

void xw_role_commit(struct xw_surface *s) {
    switch (s->role) {
    case XW_SURFACE_ROLE_XDG_TOPLEVEL: {
        struct xw_window *w = s->role_data;
        if (!w)
            return;
        if (!w->first_commit_done) {
            /* protocol requires a configure before the first buffer
             * commit takes effect */
            w->first_commit_done = true;
            xw_xdg_send_configure(w);
            return;
        }
        if (s->pending_config) {
            if (w->acked_serial == w->last_serial) {
                s->pending_config = false;
                toplevel_apply_commit(s);
            } else if (w->mapped) {
                /* commit without ack of our configure: tolerated for
                 * already-mapped windows (same-state commit) */
                toplevel_apply_commit(s);
            }
            return;
        }
        if (w->mapped)
            toplevel_apply_commit(s);
        return;
    }
    case XW_SURFACE_ROLE_XDG_POPUP:
        popup_apply_commit(s);
        return;
    case XW_SURFACE_ROLE_LAYER:
        xw_layer_role_commit(s);
        return;
    default:
        return;
    }
}

void xw_role_unmap(struct xw_surface *s) {
    switch (s->role) {
    case XW_SURFACE_ROLE_XDG_TOPLEVEL:
        if (s->comp->wm && s->role_data)
            xw_wm_window_unmap(s->comp->wm, s->role_data);
        break;
    case XW_SURFACE_ROLE_XDG_POPUP:
        xw_popup_dismiss(s->role_data);
        break;
    case XW_SURFACE_ROLE_LAYER:
        xw_layer_role_unmap(s);
        break;
    default:
        break;
    }
}

/* orphan the xdg_surface object so its own destructor (which may run
 * after this surface is freed) cannot dereference us */
static void orphan_xdg_surface(struct xw_surface *s) {
    if (s->xdg_surface_res) {
        struct xw_xdg_surface *xs =
            wl_resource_get_user_data(s->xdg_surface_res);
        if (xs && xs->surface == s)
            xs->surface = NULL;
        s->xdg_surface_res = NULL;
    }
}

void xw_role_destroy(struct xw_surface *s) {
    /* the xdg_surface resource destructor performs role teardown; this
     * path triggers when the wl_surface dies with the role still
     * attached (protocol violation or client teardown ordering) */
    switch (s->role) {
    case XW_SURFACE_ROLE_XDG_TOPLEVEL: {
        struct xw_window *w = s->role_data;
        if (w) {
            orphan_xdg_surface(s);
            s->role = XW_SURFACE_ROLE_NONE;
            s->role_data = NULL;
            if (w->toplevel_res) {
                struct wl_resource *tr = w->toplevel_res;
                w->toplevel_res = NULL;
                wl_resource_destroy(tr);
            }
            if (s->comp->wm)
                xw_wm_unmanage(s->comp->wm, w, true);
            else
                free(w);
        }
        break;
    }
    case XW_SURFACE_ROLE_XDG_POPUP: {
        struct xw_popup *p = s->role_data;
        if (p) {
            orphan_xdg_surface(s);
            s->role = XW_SURFACE_ROLE_NONE;
            s->role_data = NULL;
            wl_list_remove(&p->link);
            if (p->res) {
                struct wl_resource *pr = p->res;
                p->res = NULL;
                wl_resource_destroy(pr);
            }
            struct xw_seat *seat = xw_seat_first(s->comp);
            if (seat && seat->grab_surface == s) {
                seat->ptr_grab = NULL;
                seat->grab_surface = NULL;
            }
            free(p->pos);
            free(p);
        }
        break;
    }
    case XW_SURFACE_ROLE_LAYER:
        xw_layer_role_destroy(s);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- init */

void xw_xdg_shell_init(struct xw_compositor *c) {
    struct wl_global *g = wl_global_create(c->display, &xdg_wm_base_interface,
                                           XDG_VERSION, c, bind_wm_base);
    if (!g)
        xw_log(XW_LOG_ERROR, "xdg_wm_base global creation failed");
}

void xw_xdg_shell_fin(struct xw_compositor *c) {
    (void)c; /* globals are destroyed with the display */
}
