/* xwc-input.c — libxwcl: seat/keyboard/pointer and output handling. */
#include "xwc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wayland-client.h"

/* forward decls from xwc.c */
extern void xwc_seat_init(struct xwc *c);
extern void xwc_output_init(struct xwc *c, struct wl_registry *r, uint32_t name,
                            uint32_t version);
extern void xwc_surface_focus(struct xwc *c, struct wl_surface *surface);
extern void xwc_input_key(struct xwc *c, uint32_t keycode, bool down,
                          xkb_keysym_t sym, uint32_t mods);
extern void xwc_input_button(struct xwc *c, uint32_t button, bool down, int x,
                             int y);
extern void xwc_input_motion(struct xwc *c, int x, int y);
extern void xwc_input_axis(struct xwc *c, uint32_t axis, double value);

/* ------------------------------------------------------------ keyboard */

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                      int32_t fd, uint32_t size) {
    (void)kb;
    struct xwc *c = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    if (c->xkb_keymap) {
        xkb_keymap_unref(c->xkb_keymap);
        xkb_state_unref(c->xkb_state);
        c->xkb_keymap = NULL;
        c->xkb_state = NULL;
        if (c->keymap_mmap && c->keymap_size)
            munmap(c->keymap_mmap, c->keymap_size);
    }
    c->xkb_keymap = xkb_keymap_new_from_buffer(
        c->xkb_ctx, map, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    c->keymap_mmap = map;
    c->keymap_size = size;
    if (c->xkb_keymap)
        c->xkb_state = xkb_state_new(c->xkb_keymap);
    close(fd);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface, struct wl_array *keys) {
    (void)kb;
    (void)serial;
    (void)keys;
    struct xwc *c = data;
    xwc_surface_focus(c, surface);
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface) {
    (void)kb;
    (void)serial;
    (void)surface;
    struct xwc *c = data;
    c->focused_owner = NULL;
    c->has_focus = false;
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
    (void)kb;
    (void)serial;
    (void)time;
    struct xwc *c = data;
    bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (c->xkb_state) {
        xkb_state_update_key(c->xkb_state, key,
                             down ? XKB_KEY_DOWN : XKB_KEY_UP);
        xkb_keysym_t sym = xkb_state_key_get_one_sym(c->xkb_state, key);
        uint32_t mods = xkb_state_serialize_mods(c->xkb_state,
                                                 XKB_STATE_MODS_DEPRESSED);
        /* back to raw linux keycode for the callback layer */
        xwc_input_key(c, key - 8, down, sym, mods);
    }
}

static void kb_modifiers(void *data, struct wl_keyboard *kb,
                         uint32_t serial, uint32_t depressed, uint32_t latched,
                         uint32_t locked, uint32_t group) {
    (void)kb;
    (void)serial;
    struct xwc *c = data;
    if (c->xkb_state)
        xkb_state_update_mask(c->xkb_state, depressed, latched, locked, 0, 0,
                              group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                           int32_t delay) {
    (void)kb;
    struct xwc *c = data;
    c->repeat_rate_hz = rate;
    c->repeat_delay_ms = delay;
    c->repeat_info_received = true;
}

static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

/* ------------------------------------------------------------- pointer */

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t sx,
                      wl_fixed_t sy) {
    (void)p;
    (void)serial;
    struct xwc *c = data;
    xwc_surface_focus(c, surface);
    xwc_input_motion(c, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surface) {
    (void)data;
    (void)p;
    (void)serial;
    (void)surface;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy) {
    (void)p;
    (void)time;
    struct xwc *c = data;
    xwc_input_motion(c, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
    (void)p;
    (void)serial;
    (void)time;
    struct xwc *c = data;
    xwc_input_button(c, button,
                     state == WL_POINTER_BUTTON_STATE_PRESSED, c->ptr_x,
                     c->ptr_y);
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {
    (void)p;
    (void)time;
    struct xwc *c = data;
    xwc_input_axis(c, axis, wl_fixed_to_double(value));
}

static void ptr_frame(void *data, struct wl_pointer *p) {
    (void)data;
    (void)p;
}

static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
};

/* ---------------------------------------------------------------- seat */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    struct xwc *c = data;
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!c->keyboard) {
            c->keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(c->keyboard, &kb_listener, c);
        }
    } else if (c->keyboard) {
        wl_keyboard_destroy(c->keyboard);
        c->keyboard = NULL;
    }
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (!c->pointer) {
            c->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(c->pointer, &ptr_listener, c);
        }
    } else if (c->pointer) {
        wl_pointer_destroy(c->pointer);
        c->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

void xwc_seat_init(struct xwc *c) {
    wl_seat_add_listener(c->seat, &seat_listener, c);
}

/* -------------------------------------------------------------- output */

struct xwc_output_state {
    struct xwc *c;
    int w, h;
    int scale;
};

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t subpixel,
                         const char *make, const char *model,
                         int32_t transform) {
    (void)data;
    (void)o;
    (void)x;
    (void)y;
    (void)pw;
    (void)ph;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void out_mode(void *data, struct wl_output *o, uint32_t flags,
                     int32_t width, int32_t height, int32_t refresh) {
    (void)o;
    (void)refresh;
    struct xwc_output_state *st = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        st->w = width;
        st->h = height;
    }
}

static void out_done(void *data, struct wl_output *o) {
    (void)o;
    struct xwc_output_state *st = data;
    struct xwc *c = st->c;
    if (c->n_outputs == 0) {
        c->n_outputs = 1;
        c->output_w = st->w / (st->scale > 0 ? st->scale : 1);
        c->output_h = st->h / (st->scale > 0 ? st->scale : 1);
    }
    if (c->output_state == st)
        c->output_state = NULL;
    free(st);
}

static void out_scale(void *data, struct wl_output *o, int32_t factor) {
    (void)o;
    struct xwc_output_state *st = data;
    st->scale = factor;
}

static void out_name(void *data, struct wl_output *o, const char *name) {
    (void)data;
    (void)o;
    (void)name;
}

static void out_description(void *data, struct wl_output *o,
                            const char *description) {
    (void)data;
    (void)o;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_description,
};

void xwc_output_init(struct xwc *c, struct wl_registry *r, uint32_t name,
                     uint32_t version) {
    if (version > 4)
        version = 4;
    struct wl_output *out = wl_registry_bind(r, name, &wl_output_interface,
                                             version);
    struct xwc_output_state *st = calloc(1, sizeof(*st));
    if (!st) {
        wl_output_destroy(out);
        return;
    }
    st->c = c;
    st->scale = 1;
    c->output = out;
    c->output_state = st;
    wl_output_add_listener(out, &output_listener, st);
}
