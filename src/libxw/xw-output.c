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
        wl_output_send_description(res, "xw headless output");
    if (wl_resource_get_version(res) < WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(res);
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
    pixman_image_fill_rects(PIXMAN_OP_SRC, o->logical, &c->bg_color, 1, 0, 0,
                            o->width, o->height);

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
}
