/* xw-output.c — wl_output global, backbuffer management, repaint cycle. */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

static void output_send_details(struct wl_resource *res, struct xw_output *o) {
    wl_output_send_geometry(res, o->x, o->y, 0, 0, 0, "xw", o->name,
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        o->width * o->scale, o->height * o->scale, 60000);
    if (wl_resource_get_version(res) >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(res, o->scale);
    if (wl_resource_get_version(res) >= WL_OUTPUT_NAME_SINCE_VERSION)
        wl_output_send_name(res, o->name);
    if (wl_resource_get_version(res) >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
        wl_output_send_description(res, "xw output");
    if (wl_resource_get_version(res) < WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(res);
}

static void output_reannounce(struct xw_output *o) {
    struct wl_resource *res;
    wl_list_for_each(res, &o->resources, link) {
        output_send_details(res, o);
        if (wl_resource_get_version(res) >= WL_OUTPUT_DONE_SINCE_VERSION)
            wl_output_send_done(res);
    }
}

static void output_resource_destroy(struct wl_resource *res) {
    wl_list_remove(wl_resource_get_link(res));
}

static void output_release(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct wl_output_interface output_impl = {
    .release = output_release,
};

void xw_output_bind(struct wl_client *client, void *data, uint32_t version,
                    uint32_t id) {
    struct xw_output *o = data;
    if (version > 4)
        version = 4;
    struct wl_resource *res =
        wl_resource_create(client, &wl_output_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &output_impl, o, output_resource_destroy);
    wl_list_insert(o->resources.prev, wl_resource_get_link(res));
    output_send_details(res, o);
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(res);
}

void xw_output_set_usable(struct xw_output *o, int x, int y, int w, int h) {
    o->usable.x = x;
    o->usable.y = y;
    o->usable.w = w;
    o->usable.h = h;
}

void xw_output_damage_rect(struct xw_output *o, int x, int y, int w, int h) {
    /* translate from layout coords to output-local logical coords */
    pixman_region_union_rect(&o->damage, &o->damage, x - o->x, y - o->y, w, h);
    xw_schedule_repaint(o->comp);
}

static void deliver_frame_callbacks(struct xw_output *o) {
    struct xw_compositor *c = o->comp;
    int64_t now = xw_now_ms();
    struct xw_surface *s;
    wl_list_for_each(s, &c->surfaces, link) {
        if (!s->mapped || wl_list_empty(&s->frames))
            continue;
        struct xw_frame *f, *f2;
        wl_list_for_each_safe(f, f2, &s->frames, link) {
            wl_callback_send_done(f->res, (uint32_t)now);
            wl_list_remove(&f->link);
            wl_resource_destroy(f->res);
            free(f);
        }
    }
}

void xw_output_repaint(struct xw_output *o) {
    struct xw_compositor *c = o->comp;

    /* 1. clear logical buffer to the background color */
    xw_render_fill_rect(o->logical, PIXMAN_OP_SRC, c->bg_color, 0, 0, o->width,
                        o->height);

    /* 2. draw everything (layers, windows, popups, snap preview, cursor) */
    xw_render_output(o);

    /* 3. scale to native resolution (integer scale) */
    if (o->scale == 1) {
        memcpy(o->native_data, pixman_image_get_data(o->logical),
               (size_t)o->width * o->height * 4);
    } else {
        pixman_transform_t t;
        pixman_transform_init_scale(&t, pixman_double_to_fixed(1.0 / o->scale),
                                    pixman_double_to_fixed(1.0 / o->scale));
        pixman_image_set_transform(o->logical, &t);
        pixman_image_composite(PIXMAN_OP_SRC, o->logical, NULL, o->native, 0, 0,
                               0, 0, 0, 0, o->width * o->scale,
                               o->height * o->scale);
        pixman_image_set_transform(o->logical, NULL);
    }

    o->last_frame_ms = xw_now_ms();

    /* 4. damage accounting + frame callbacks */
    pixman_region_clear(&o->damage);
    deliver_frame_callbacks(o);

    /* 5. backend present (headless has none; nested/x11 hand the frame
     * to the parent display) */
    if (c->backend && c->backend->ops && c->backend->ops->present)
        c->backend->ops->present(c->backend, o);
}

/* ------------------------------------------------------ shared lifecycle */

struct xw_output *xw_output_create(struct xw_compositor *c, const char *name,
                                   int x, int y, int w, int h, int scale) {
    if (w < 16 || h < 16 || scale < 1)
        return NULL;
    struct xw_output *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->comp = c;
    o->x = x;
    o->y = y;
    o->width = w;
    o->height = h;
    o->scale = scale;
    snprintf(o->name, sizeof(o->name), "%s", name && *name ? name : "OUTPUT");
    o->usable.x = x;
    o->usable.y = y;
    o->usable.w = w;
    o->usable.h = h;
    pixman_region_init(&o->damage);
    wl_list_init(&o->resources);

    size_t stride = (size_t)w * 4;
    uint32_t *logical_data = calloc(stride / 4 * (size_t)h, 4);
    uint32_t *native_data = calloc(stride / 4 * (size_t)h * (size_t)scale *
                                       (size_t)scale, 4);
    if (!logical_data || !native_data) {
        free(logical_data);
        free(native_data);
        free(o);
        return NULL;
    }
    o->logical = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, logical_data,
                                          (int)stride);
    o->native = pixman_image_create_bits(PIXMAN_a8r8g8b8, w * scale, h * scale,
                                         native_data, (int)stride * scale);
    o->native_data = native_data;

    o->global = wl_global_create(c->display, &wl_output_interface, 4, o,
                                 xw_output_bind);
    if (!o->global) {
        pixman_image_unref(o->logical);
        pixman_image_unref(o->native);
        free(o);
        return NULL;
    }
    wl_list_insert(c->outputs.prev, &o->link);
    xw_log(XW_LOG_INFO, "output %s: %dx%d+%d+%d scale %d", o->name, o->width,
           o->height, o->x, o->y, o->scale);
    return o;
}

void xw_output_destroy(struct xw_output *o) {
    if (!o)
        return;
    wl_list_remove(&o->link);
    if (o->global)
        wl_global_destroy(o->global);
    pixman_region_fini(&o->damage);
    free(pixman_image_get_data(o->logical));
    free(o->native_data);
    pixman_image_unref(o->logical);
    pixman_image_unref(o->native);
    free(o);
}

void xw_output_resize(struct xw_output *o, int w, int h) {
    if (!o || w < 16 || h < 16 || (w == o->width && h == o->height))
        return;
    struct xw_compositor *c = o->comp;

    /* reallocate backbuffers */
    size_t stride = (size_t)w * 4;
    uint32_t *logical_data = calloc(stride / 4 * (size_t)h, 4);
    uint32_t *native_data = calloc(stride / 4 * (size_t)h * (size_t)o->scale *
                                       (size_t)o->scale, 4);
    if (!logical_data || !native_data) {
        free(logical_data);
        free(native_data);
        xw_log(XW_LOG_ERROR, "output %s: resize OOM, keeping %dx%d", o->name,
               o->width, o->height);
        return;
    }
    free(pixman_image_get_data(o->logical));
    free(o->native_data);
    pixman_image_unref(o->logical);
    pixman_image_unref(o->native);
    o->logical = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, logical_data,
                                          (int)stride);
    o->native = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, w * o->scale, h * o->scale, native_data,
        (int)stride * o->scale);
    o->native_data = native_data;
    o->width = w;
    o->height = h;

    xw_log(XW_LOG_INFO, "output %s: resized to %dx%d", o->name, w, h);

    /* re-announce mode to bound clients */
    output_reannounce(o);

    /* relayout: layer surfaces first (anchored bars must learn the new
     * output size and recommit; this also recalculates the usable
     * area), then maximized/fullscreen windows, then damage */
    if (c->wm)
        xw_layer_reconfigure_output(c, o);
    else
        xw_output_set_usable(o, o->x, o->y, w, h);
    pixman_region_union_rect(&o->damage, &o->damage, 0, 0, w, h);
    xw_schedule_repaint(c);
}
