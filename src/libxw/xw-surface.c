/* xw-surface.c — wl_surface server implementation, buffers, damage. */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

static void surface_destroy_request(struct wl_client *client,
                                    struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *res,
                           struct wl_resource *buffer, int32_t dx, int32_t dy) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);
    /* offset is only meaningful for buffer-coordinate shifts in GL paths;
     * for shm commits it must be 0 per spec. Tolerate and warn. */
    if (dx || dy)
        xw_log(XW_LOG_WARN, "surface attach offset %d,%d ignored", dx, dy);
    s->pending_buffer = buffer; /* NULL detaches */
}

static void surface_damage(struct wl_client *client, struct wl_resource *res,
                           int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);
    pixman_region_union_rect(&s->pending_damage, &s->pending_damage, x, y, w, h);
}

static void surface_frame(struct wl_client *client, struct wl_resource *res,
                          uint32_t id) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);
    struct xw_frame *f = calloc(1, sizeof(*f));
    if (!f) {
        wl_client_post_no_memory(client);
        return;
    }
    f->res = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (!f->res) {
        free(f);
        wl_client_post_no_memory(client);
        return;
    }
    wl_list_insert(s->frames.prev, &f->link);
}

static void surface_set_opaque_region(struct wl_client *client,
                                      struct wl_resource *res,
                                      struct wl_resource *region) {
    (void)client;
    (void)res;
    (void)region; /* accepted; not required for correctness (software path) */
}

static void surface_set_input_region(struct wl_client *client,
                                     struct wl_resource *res,
                                     struct wl_resource *region) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);
    if (region) {
        pixman_region16_t *src = wl_resource_get_user_data(region);
        pixman_region_copy(&s->input, src);
        s->input_set = true;
    } else {
        pixman_region_clear(&s->input);
        s->input_set = false;
    }
}

static void surface_commit(struct wl_client *client, struct wl_resource *res);

static void surface_set_transform(struct wl_client *client,
                                  struct wl_resource *res, int32_t transform) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);
    (void)s;
    if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
        xw_log(XW_LOG_WARN, "buffer transform %d unsupported; rejecting",
               transform);
        wl_resource_post_error(res, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "unsupported buffer transform");
    }
}

static void surface_set_scale(struct wl_client *client, struct wl_resource *res,
                              int32_t scale) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);
    if (scale < 1) {
        wl_resource_post_error(res, WL_SURFACE_ERROR_INVALID_SCALE,
                               "invalid buffer scale");
        return;
    }
    s->scale = scale;
}

static void surface_damage_buffer(struct wl_client *client,
                                  struct wl_resource *res, int32_t x, int32_t y,
                                  int32_t w, int32_t h) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);
    int scale = s->scale > 0 ? s->scale : 1;
    pixman_region_union_rect(&s->pending_damage, &s->pending_damage, x / scale,
                             y / scale, w / scale, h / scale);
}

static void surface_offset(struct wl_client *client, struct wl_resource *res,
                           int32_t x, int32_t y) {
    (void)client;
    (void)res;
    (void)x;
    (void)y; /* wl_surface.offset shifts buffer coords; v0 no-op */
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_request,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_transform,
    .set_buffer_scale = surface_set_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

/* ------------------------------------------------------------- surface api */

static void set_committed_buffer(struct xw_surface *s,
                                 struct wl_resource *b);

struct wl_resource *xw_surface_create(struct wl_client *client,
                                      struct xw_compositor *c, uint32_t id,
                                      uint32_t version) {
    struct xw_surface *s = calloc(1, sizeof(*s));
    if (!s) {
        wl_client_post_no_memory(client);
        return NULL;
    }
    struct wl_resource *res =
        wl_resource_create(client, &wl_surface_interface, version, id);
    if (!res) {
        free(s);
        wl_client_post_no_memory(client);
        return NULL;
    }
    s->res = res;
    s->comp = c;
    s->scale = 1;
    pixman_region_init(&s->input);
    pixman_region_init(&s->pending_damage);
    wl_list_init(&s->frames);
    wl_list_init(&s->subsurfaces);
    wl_list_insert(c->surfaces.prev, &s->link);
    wl_resource_set_implementation(res, &surface_impl, s,
                                   xw_surface_resource_destroyed);
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, NULL, NULL);
    xw_log(XW_LOG_DEBUG, "surface %u created (client pid %d)", id, (int)pid);
    return res;
}

void xw_surface_resource_destroyed(struct wl_resource *res) {
    struct xw_surface *s = wl_resource_get_user_data(res);
    /* drop seat focus/grab references BEFORE anything is freed: a
     * surface dying under the cursor must not leave a dangling
     * ptr_focus behind (use-after-free on the next pointer event) */
    xw_seat_forget_surface(s->comp, s);
    xw_seat_forget_cursor_surface(s->comp, s);
    /* subsurface children outlive the parent surface only as roleless
     * wl_surfaces: tear their role down now (the parent position is
     * still valid for the final damage pass) */
    if (!wl_list_empty(&s->subsurfaces))
        xw_subsurface_parent_destroyed(s);
    xw_role_destroy(s);
    struct xw_frame *f, *f2;
    wl_list_for_each_safe(f, f2, &s->frames, link) {
        wl_list_remove(&f->link);
        free(f); /* the wl_callback resource is already being destroyed */
    }
    /* release the buffer we still hold (if the client did not destroy
     * it first — the destroy listener cleared the pointer then) */
    set_committed_buffer(s, NULL);
    wl_list_remove(&s->link);
    pixman_region_fini(&s->input);
    pixman_region_fini(&s->pending_damage);
    free(s);
}

/* ------------------------------------------------- buffer ownership */

/* The client destroyed a buffer this surface still references: drop the
 * pointer WITHOUT sending release (the object is gone; the destroy
 * signal fires before internal destructors run). */
static void committed_buffer_destroyed(struct wl_listener *l, void *data) {
    (void)data;
    struct xw_surface *s =
        wl_container_of(l, s, committed_buffer_destroy);
    s->committed_buffer = NULL;
}

/* Swap the surface's committed buffer. The PREVIOUS buffer gets
 * wl_buffer.release the moment it stops being referenced: clients that
 * rotate a pool of 2+ shm buffers (foot, GTK, XWayland) treat release
 * as the reuse permission and simply stop committing once every pool
 * buffer is outstanding — a frozen application with no error anywhere.
 * NULL detaches. */
static void set_committed_buffer(struct xw_surface *s,
                                 struct wl_resource *b) {
    if (s->committed_buffer == b)
        return;
    if (s->committed_buffer) {
        wl_list_remove(&s->committed_buffer_destroy.link);
        wl_buffer_send_release(s->committed_buffer);
    }
    s->committed_buffer = b;
    if (b) {
        s->committed_buffer_destroy.notify = committed_buffer_destroyed;
        wl_resource_add_destroy_listener(b, &s->committed_buffer_destroy);
    }
}

static void apply_buffer(struct xw_surface *s) {
    struct wl_resource *b = s->pending_buffer;
    s->pending_buffer = NULL;
    s->shm = NULL;
    s->has_single_pixel = false;
    if (b) {
        set_committed_buffer(s, b);
        struct wl_shm_buffer *shm = wl_shm_buffer_get(b);
        if (shm) {
            s->shm = shm;
            s->buf_w = wl_shm_buffer_get_width(shm);
            s->buf_h = wl_shm_buffer_get_height(shm);
            return;
        }
        /* not shm: our single-pixel buffer (implementation data = color) */
        void *ud = wl_resource_get_user_data(b);
        if (ud) {
            memcpy(&s->single_pixel.color, ud, sizeof(uint32_t));
            s->has_single_pixel = true;
            s->buf_w = 1;
            s->buf_h = 1;
            return;
        }
    } else {
        set_committed_buffer(s, NULL);
    }
    /* an unrecognized or NULL buffer: no content */
    s->buf_w = 0;
    s->buf_h = 0;
}

static void damage_surface_extent(struct xw_surface *s, int old_x, int old_y,
                                  int old_w, int old_h) {
    int x, y, w, h;
    xw_surface_get_pos(s, &x, &y, &w, &h);
    struct xw_compositor *c = s->comp;
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link) {
        if (old_w > 0 && old_h > 0)
            xw_output_damage_rect(o, old_x, old_y, old_w, old_h);
        if (w > 0 && h > 0)
            xw_output_damage_rect(o, x, y, w, h);
    }
}

static void surface_commit(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_surface *s = wl_resource_get_user_data(res);

    int old_x = 0, old_y = 0, old_w = 0, old_h = 0;
    bool had_extent = s->buf_w > 0 && s->buf_h > 0;
    if (had_extent)
        xw_surface_get_pos(s, &old_x, &old_y, &old_w, &old_h);

    apply_buffer(s);

    if (s->role == XW_SURFACE_ROLE_NONE) {
        if (s->is_cursor) {
            /* cursor surfaces commit rolelessly: the new image (or the
             * detach) must erase the old cursor pixels and deliver
             * frame callbacks (animated cursors drive off them) */
            xw_seat_damage_cursor(s->comp);
            s->mapped = s->buf_w > 0 && s->buf_h > 0;
            xw_seat_damage_cursor(s->comp);
        }
        /* role-less commits only update buffer state; nothing else is
         * mapped */
        pixman_region_clear(&s->pending_damage);
        return;
    }
    xw_role_commit(s);

    if (s->role == XW_SURFACE_ROLE_XDG_TOPLEVEL)
        xw_subsurface_parent_committed(s);

    if (had_extent || (s->buf_w > 0 && s->buf_h > 0))
        damage_surface_extent(s, old_x, old_y, old_w, old_h);
    pixman_region_clear(&s->pending_damage);

    if (s->shm && s->buf_w > 0) {
        xw_log(XW_LOG_DEBUG, "surface %u: commit buffer %dx%d stride %u",
               wl_resource_get_id(res), s->buf_w, s->buf_h,
               wl_shm_buffer_get_stride(s->shm));
    }
}

/* --------------------------------------------------------- role helpers */

void xw_surface_get_pos(struct xw_surface *s, int *x, int *y, int *w, int *h) {
    if (x) *x = 0;
    if (y) *y = 0;
    int sc = s->scale > 0 ? s->scale : 1;
    if (w) *w = s->buf_w / sc;
    if (h) *h = s->buf_h / sc;
    if (s->role == XW_SURFACE_ROLE_XDG_TOPLEVEL && s->role_data) {
        struct xw_window *win = s->role_data;
        if (x) *x = win->x;
        if (y) *y = win->y;
        if (w) *w = win->w;
        if (h) *h = win->h;
    } else if (s->role == XW_SURFACE_ROLE_XDG_POPUP && s->role_data) {
        struct xw_popup *p = s->role_data;
        if (x) *x = p->anchor_x;
        if (y) *y = p->anchor_y;
        if (w) *w = p->w;
        if (h) *h = p->h;
    } else if (s->role == XW_SURFACE_ROLE_LAYER && s->role_data) {
        struct xw_layer_surface *ls = s->role_data;
        if (x) *x = ls->x;
        if (y) *y = ls->y;
        if (w) *w = ls->w;
        if (h) *h = ls->h;
    } else if (s->role == XW_SURFACE_ROLE_SUBSURFACE) {
        xw_subsurface_get_pos(s, x, y, w, h);
    }
}

pixman_image_t *xw_surface_get_image(struct xw_surface *s) {
    if (s->shm) {
        uint32_t format = wl_shm_buffer_get_format(s->shm);
        pixman_format_code_t pf = PIXMAN_a8r8g8b8;
        if (format == WL_SHM_FORMAT_XRGB8888)
            pf = PIXMAN_x8r8g8b8;
        return pixman_image_create_bits(pf, s->buf_w, s->buf_h,
                                        wl_shm_buffer_get_data(s->shm),
                                        wl_shm_buffer_get_stride(s->shm));
    }
    if (s->has_single_pixel) {
        uint32_t *d = malloc(4);
        if (!d)
            return NULL;
        memcpy(d, &s->single_pixel.color, 4);
        return pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, d, 4);
    }
    return NULL;
}

bool xw_surface_has_input_at(struct xw_surface *s, int gx, int gy) {
    int x, y, w, h;
    xw_surface_get_pos(s, &x, &y, &w, &h);
    if (w <= 0 || h <= 0)
        return false;
    /* surface-local coordinates are relative to the BUFFER origin. For
     * CSD toplevels with set_window_geometry the buffer origin sits
     * geo_* left/above the content origin, and the default interactive
     * rect is the geometry (content) rect, not the whole buffer --
     * clicks on client-side shadows must fall through. */
    int gl = 0, gt = 0;
    if (s->role == XW_SURFACE_ROLE_XDG_TOPLEVEL && s->role_data) {
        struct xw_window *win = s->role_data;
        if (win->geometry_set) {
            gl = win->geo_x;
            gt = win->geo_y;
        }
    }
    int lx = gx - x + gl, ly = gy - y + gt; /* surface-local */
    if (!s->input_set) {
        /* no input region set: the geometry rect is interactive */
        return lx >= gl && lx < gl + w && ly >= gt && ly < gt + h;
    }
    if (gx < x || gx >= x + w || gy < y || gy >= y + h)
        return false;
    return pixman_region_contains_point(&s->input, lx, ly, NULL) != 0;
}

/* damage every output intersecting a rect (global coords) */
void xw_damage_outputs_rect(struct xw_compositor *c, int x, int y, int w, int h) {
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link)
        xw_output_damage_rect(o, x, y, w, h);
}
