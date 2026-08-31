/* xw-backend-headless.c — deterministic headless backend.
 *
 * Owns output creation from the configuration and the input injection
 * entry points. There is no real input device or KMS device involved;
 * frame timing is damage-driven (repaint scheduled whenever damage
 * arrives), which keeps tests fully deterministic.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

static struct xw_seat *first_seat(struct xw_compositor *c) {
    struct xw_seat *s;
    wl_list_for_each(s, &c->seats, link)
        return s;
    return NULL;
}

struct xw_backend *xw_backend_headless_create(struct xw_compositor *c,
                                              const struct xw_compositor_config *cfg) {
    struct xw_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->comp = c;
    b->name = "headless";

    /* config is const; build a private spec array so a default output can
     * be synthesized when none is configured */
    int n = cfg->n_outputs > 0 ? cfg->n_outputs : 1;
    struct xw_output_spec defspecs[1] = {
        { .name = "HEADLESS-1", .width = 1280, .height = 720, .scale = 1 }
    };
    const struct xw_output_spec *specs =
        cfg->outputs ? cfg->outputs : defspecs;

    int layout_x = 0;
    for (int i = 0; i < n; i++) {
        const struct xw_output_spec *sp = &specs[i];
        struct xw_output *o = calloc(1, sizeof(*o));
        if (!o)
            goto fail;
        o->comp = c;
        snprintf(o->name, sizeof(o->name), "%s",
                 sp->name && *sp->name ? sp->name : "HEADLESS");
        o->x = layout_x;
        o->y = 0;
        o->width = sp->width > 0 ? sp->width : 1280;
        o->height = sp->height > 0 ? sp->height : 720;
        o->scale = sp->scale > 0 ? sp->scale : 1;
        layout_x += o->width;
        o->usable.x = o->x;
        o->usable.y = o->y;
        o->usable.w = o->width;
        o->usable.h = o->height;
        pixman_region_init(&o->damage);
        wl_list_init(&o->resources);

        int stride = o->width * 4;
        uint32_t *logical_data = calloc((size_t)stride / 4 * o->height, 4);
        uint32_t *native_data = calloc((size_t)stride / 4 * o->height *
                                           (size_t)o->scale * o->scale, 4);
        if (!logical_data || !native_data) {
            free(logical_data);
            free(native_data);
            free(o);
            goto fail;
        }
        o->logical = pixman_image_create_bits(PIXMAN_a8r8g8b8, o->width, o->height,
                                              logical_data, stride);
        int nstride = stride * o->scale;
        o->native = pixman_image_create_bits(PIXMAN_a8r8g8b8, o->width * o->scale,
                                             o->height * o->scale, native_data,
                                             nstride);
        o->native_data = native_data;

        o->global = wl_global_create(c->display, &wl_output_interface, 4, o,
                                     xw_output_bind);
        if (!o->global) {
            pixman_image_unref(o->logical);
            pixman_image_unref(o->native);
            free(o);
            goto fail;
        }
        wl_list_insert(c->outputs.prev, &o->link);
        xw_log(XW_LOG_INFO, "output %s: %dx%d+%d+%d scale %d", o->name, o->width,
               o->height, o->x, o->y, o->scale);
    }
    return b;
fail:
    xw_log(XW_LOG_ERROR, "headless backend init failed");
    free(b);
    return NULL;
}

void xw_backend_headless_destroy(struct xw_backend *b) {
    if (!b)
        return;
    struct xw_compositor *c = b->comp;
    struct xw_output *o, *o2;
    wl_list_for_each_safe(o, o2, &c->outputs, link) {
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
    free(b);
}

/* ----------------------------------------------------------- injection API */

void xw_compositor_inject_key(struct xw_compositor *c, uint32_t linux_keycode,
                              bool down) {
    struct xw_seat *s = first_seat(c);
    if (s)
        xw_seat_key(s, linux_keycode, down);
}

void xw_compositor_inject_pointer_motion(struct xw_compositor *c, int x, int y) {
    struct xw_seat *s = first_seat(c);
    if (s)
        xw_seat_pointer_motion(s, x, y);
}

void xw_compositor_inject_pointer_button(struct xw_compositor *c,
                                         uint32_t linux_button, bool down) {
    struct xw_seat *s = first_seat(c);
    if (s)
        xw_seat_pointer_button(s, linux_button, down);
}

void xw_compositor_inject_pointer_axis(struct xw_compositor *c, uint32_t axis,
                                       double value) {
    struct xw_seat *s = first_seat(c);
    if (s)
        xw_seat_pointer_axis(s, axis, value);
}
