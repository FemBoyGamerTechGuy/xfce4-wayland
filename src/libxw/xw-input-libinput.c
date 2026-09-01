/* xw-input-libinput.c — real input devices through libinput.
 *
 * This is an input SOURCE, deliberately orthogonal to the output
 * backends: libinput is what makes keyboards and mice real when the
 * compositor owns the hardware (DRM/KMS sessions, or a headless
 * compositor driven from real devices for debugging). The nested
 * Wayland/X11 backends never use it — the parent session feeds input
 * through their own event paths.
 *
 * Device discovery:
 *   - udev seat mode (default when -I libinput): libinput enumerates
 *     the seat's devices itself, including hotplug. This is the mode a
 *     real session uses. Requires a working udev instance.
 *   - path mode (XW_INPUT_DEVICES="/dev/input/event3:/dev/input/event5"):
 *     explicit device nodes, no hotplug. Deterministic; used for
 *     development, testing and locked-down setups.
 *
 * Event translation is kept in small handler functions (xw_input_handle_*)
 * so the pipeline (clamping, cursor math, injection into the seat) can be
 * exercised white-box by the test suite without physical devices. The thin
 * libinput_event decoder above them is the only hardware-only part.
 *
 * Key repeat is NOT done here: clients repeat themselves via
 * wl_keyboard.repeat_info (sent by the seat), and the seat runs a
 * server-side repeat timer only for interactive keyboard move/resize.
 * libinput itself never generates key repeat events.
 */
#include "xw-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libinput.h>
#include <libudev.h>

struct xw_input_libinput {
    struct xw_compositor *comp;
    struct libinput *li;
    struct udev *udev;           /* owned; NULL in path mode */
    struct wl_event_source *fd_src;
    char seat[32];
    /* sub-pixel pointer accumulation (touchpads move < 1px per event) */
    double acc_x, acc_y;
    /* touch: last absolute position for relative deltas (unused yet) */
};

/* ------------------------------------------------------- open/close glue */

static int open_restricted(const char *path, int flags, void *ud) {
    (void)ud;
    int fd = open(path, flags | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        xw_log(XW_LOG_WARN, "input: cannot open device %s: %s", path,
               strerror(errno));
    return fd;
}

static void close_restricted(int fd, void *ud) {
    (void)ud;
    close(fd);
}

static const struct libinput_interface input_iface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

/* route libinput's internal logging into our log (it prints to stderr
 * by default, which pollutes test output and gives users no level
 * control); device-open failures in path mode are expected, so ERROR
 * maps to our WARN) */
static void li_log(struct libinput *li, enum libinput_log_priority prio,
                   const char *format, va_list args) {
    (void)li;
    char buf[256];
    vsnprintf(buf, sizeof(buf), format, args);
    xw_log(prio <= LIBINPUT_LOG_PRIORITY_ERROR ? XW_LOG_WARN : XW_LOG_DEBUG,
           "input: %s", buf);
}

/* ------------------------------------------------------ event translation */

/* Output layout bounding box (logical coordinates). Returns false when
 * the compositor has no outputs (motion is dropped, not clamped). */
static bool layout_bounds(struct xw_compositor *c, int *bx, int *by, int *bw,
                          int *bh) {
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link) {
        if (o->x < min_x)
            min_x = o->x;
        if (o->y < min_y)
            min_y = o->y;
        if (o->x + o->width > max_x)
            max_x = o->x + o->width;
        if (o->y + o->height > max_y)
            max_y = o->y + o->height;
    }
    if (min_x == INT_MAX)
        return false;
    *bx = min_x;
    *by = min_y;
    *bw = max_x - min_x;
    *bh = max_y - min_y;
    return true;
}

static int clampi(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* The handlers below are the real pipeline used by the libinput event
 * loop (and callable directly by tests). All coordinates are logical
 * (output layout) pixels. */

void xw_input_handle_key(struct xw_input_libinput *in, uint32_t linux_keycode,
                         bool down) {
    xw_compositor_inject_key(in->comp, linux_keycode, down);
}

void xw_input_handle_pointer_rel(struct xw_input_libinput *in, double dx,
                                 double dy) {
    struct xw_seat *s = xw_seat_first(in->comp);
    if (!s)
        return;
    in->acc_x += dx;
    in->acc_y += dy;
    int step_x = (int)in->acc_x;
    int step_y = (int)in->acc_y;
    in->acc_x -= step_x;
    in->acc_y -= step_y;
    if (!step_x && !step_y)
        return;
    int bx, by, bw, bh;
    if (!layout_bounds(in->comp, &bx, &by, &bw, &bh))
        return;
    int nx = clampi(s->cursor_x + step_x, bx, bx + bw - 1);
    int ny = clampi(s->cursor_y + step_y, by, by + bh - 1);
    xw_compositor_inject_pointer_motion(in->comp, nx, ny);
}

void xw_input_handle_pointer_abs(struct xw_input_libinput *in, double nx,
                                 double ny) {
    int bx, by, bw, bh;
    if (!layout_bounds(in->comp, &bx, &by, &bw, &bh))
        return;
    if (nx < 0.0)
        nx = 0.0;
    if (nx > 1.0)
        nx = 1.0;
    if (ny < 0.0)
        ny = 0.0;
    if (ny > 1.0)
        ny = 1.0;
    int x = bx + (int)lround(nx * (bw - 1));
    int y = by + (int)lround(ny * (bh - 1));
    xw_compositor_inject_pointer_motion(in->comp, x, y);
}

void xw_input_handle_button(struct xw_input_libinput *in, uint32_t linux_button,
                            bool down) {
    xw_compositor_inject_pointer_button(in->comp, linux_button, down);
}

void xw_input_handle_axis(struct xw_input_libinput *in, uint32_t axis,
                          double value, bool continuous) {
    (void)continuous; /* wheel clicks and touchpad scroll share the
                        value semantics of the seat's axis path */
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL &&
        axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        return;
    xw_compositor_inject_pointer_axis(in->comp, axis, value);
}

/* ------------------------------------------------------------ event loop */

static const char *device_name(struct libinput_device *dev) {
    const char *name = libinput_device_get_name(dev);
    return name ? name : "?";
}

static void handle_libinput_event(struct xw_input_libinput *in,
                                  struct libinput_event *ev) {
    struct xw_compositor *comp = in->comp;
    enum libinput_event_type type = libinput_event_get_type(ev);

    switch (type) {
    case LIBINPUT_EVENT_DEVICE_ADDED: {
        struct libinput_device *dev = libinput_event_get_device(ev);
        xw_log(XW_LOG_INFO, "input: device added: %s (%s)", device_name(dev),
               libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD)
                   ? "keyboard"
                   : libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER)
                         ? "pointer"
                         : "other");
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED: {
        struct libinput_device *dev = libinput_event_get_device(ev);
        xw_log(XW_LOG_INFO, "input: device removed: %s", device_name(dev));
        break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY: {
        struct libinput_event_keyboard *kev = libinput_event_get_keyboard_event(ev);
        uint32_t code = libinput_event_keyboard_get_key(kev);
        bool down =
            libinput_event_keyboard_get_key_state(kev) == LIBINPUT_KEY_STATE_PRESSED;
        xw_input_handle_key(in, code, down);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION: {
        struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
        double dx = libinput_event_pointer_get_dx(pev);
        double dy = libinput_event_pointer_get_dy(pev);
        xw_input_handle_pointer_rel(in, dx, dy);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
        struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
        double x = libinput_event_pointer_get_absolute_x(pev);
        double y = libinput_event_pointer_get_absolute_y(pev);
        xw_input_handle_pointer_abs(in, x, y);
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
        uint32_t btn = libinput_event_pointer_get_button(pev);
        bool down = libinput_event_pointer_get_button_state(pev) ==
                    LIBINPUT_BUTTON_STATE_PRESSED;
        xw_input_handle_button(in, btn, down);
        break;
    }
    case LIBINPUT_EVENT_POINTER_AXIS: {
        struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
        enum libinput_pointer_axis_source src =
            libinput_event_pointer_get_axis_source(pev);
        /* an axis event carries one or both scroll axes (libinput >= 1.19
         * API: has_axis + v120/scroll values) */
        static const struct {
            enum libinput_pointer_axis li;
            uint32_t wl;
        } axes[2] = {
            {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, WL_POINTER_AXIS_VERTICAL_SCROLL},
            {LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL, WL_POINTER_AXIS_HORIZONTAL_SCROLL},
        };
        for (int i = 0; i < 2; i++) {
            if (!libinput_event_pointer_has_axis(pev, axes[i].li))
                continue;
            double value;
            bool continuous;
            if (src == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL) {
                /* high-res wheels: 120 units per traditional notch */
                value = libinput_event_pointer_get_scroll_value_v120(pev, axes[i].li) / 120.0;
                continuous = false;
            } else {
                /* touchpad two-finger / continuous scroll: pixel distance */
                value = libinput_event_pointer_get_scroll_value(pev, axes[i].li);
                continuous = true;
            }
            xw_input_handle_axis(in, axes[i].wl, value, continuous);
        }
        break;
    }
    default:
        /* touch, gestures, tablets, switches: not supported yet (see
         * ROADMAP); events are dropped, never half-handled */
        break;
    }
    (void)comp;
}

static void drain_libinput(struct xw_input_libinput *in) {
    struct libinput_event *ev;
    while ((ev = libinput_get_event(in->li)))
        handle_libinput_event(in, ev);
}

static int on_libinput_fd(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct xw_input_libinput *in = data;
    if (mask & WL_EVENT_ERROR) {
        xw_log(XW_LOG_ERROR, "input: libinput fd error; disabling source");
        return 0;
    }
    if (mask & WL_EVENT_HANGUP) {
        xw_log(XW_LOG_ERROR, "input: libinput fd hangup; disabling source");
        return 0;
    }
    if (libinput_dispatch(in->li) < 0) {
        xw_log(XW_LOG_WARN, "input: libinput_dispatch failed: %s",
               strerror(errno));
        return 0;
    }
    drain_libinput(in);
    return 0;
}

/* --------------------------------------------------------------- create */

/* Splits a colon-separated device list. Returns the number of paths
 * written into argv slots (each strdup'd; caller frees). */
static int split_device_list(const char *list, char **out, int max) {
    int n = 0;
    const char *p = list;
    while (*p && n < max) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0) {
            char *path = malloc(len + 1);
            if (!path)
                break;
            memcpy(path, p, len);
            path[len] = 0;
            out[n++] = path;
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return n;
}

struct xw_input_libinput *xw_input_libinput_create(struct xw_compositor *c) {
    struct xw_input_libinput *in = calloc(1, sizeof(*in));
    if (!in)
        return NULL;
    in->comp = c;
    snprintf(in->seat, sizeof(in->seat), "%s",
             c->conf.seat_name ? c->conf.seat_name : "seat0");

    const char *devlist = getenv("XW_INPUT_DEVICES");

    if (devlist && *devlist) {
        /* path mode: explicit devices, no hotplug, no udev dependency */
        in->li = libinput_path_create_context(&input_iface, in);
        if (!in->li) {
            xw_log(XW_LOG_ERROR, "input: libinput path context failed");
            free(in);
            return NULL;
        }
        libinput_log_set_handler(in->li, li_log);
        char *paths[16];
        int n = split_device_list(devlist, paths, 16);
        int opened = 0;
        for (int i = 0; i < n; i++) {
            if (libinput_path_add_device(in->li, paths[i]))
                opened++;
            else
                xw_log(XW_LOG_WARN, "input: device %s not usable", paths[i]);
            free(paths[i]);
        }
        xw_log(XW_LOG_INFO, "input: libinput path mode: %d device(s) listed, "
                            "%d opened", n, opened);
    } else {
        /* udev seat mode: real sessions; discovery + hotplug via udev */
        in->udev = udev_new();
        if (!in->udev) {
            xw_log(XW_LOG_ERROR, "input: udev unavailable (%s) — no device "
                                 "discovery; run with XW_INPUT_DEVICES for "
                                 "explicit devices", strerror(errno));
            free(in);
            return NULL;
        }
        in->li = libinput_udev_create_context(&input_iface, in, in->udev);
        if (!in->li) {
            xw_log(XW_LOG_ERROR, "input: libinput udev context failed");
            udev_unref(in->udev);
            free(in);
            return NULL;
        }
        libinput_log_set_handler(in->li, li_log);
        if (libinput_udev_assign_seat(in->li, in->seat) != 0) {
            xw_log(XW_LOG_ERROR, "input: cannot assign seat '%s' — no "
                                 "devices will be available", in->seat);
            libinput_unref(in->li);
            udev_unref(in->udev);
            free(in);
            return NULL;
        }
        xw_log(XW_LOG_INFO, "input: libinput udev mode, seat '%s'", in->seat);
    }

    int fd = libinput_get_fd(in->li);
    if (fd < 0) {
        xw_log(XW_LOG_ERROR, "input: libinput has no fd");
        goto fail;
    }
    in->fd_src = wl_event_loop_add_fd(c->loop, fd, WL_EVENT_READABLE,
                                      on_libinput_fd, in);
    if (!in->fd_src)
        goto fail;

    /* events may already be pending (device enumeration happens during
     * assign_seat / path_add_device) */
    libinput_dispatch(in->li);
    drain_libinput(in);

    return in;

fail:
    libinput_unref(in->li);
    if (in->udev)
        udev_unref(in->udev);
    free(in);
    return NULL;
}

void xw_input_libinput_destroy(struct xw_input_libinput *in) {
    if (!in)
        return;
    if (in->fd_src)
        wl_event_source_remove(in->fd_src);
    /* draining unreturned events keeps the log honest on teardown */
    libinput_unref(in->li);
    if (in->udev)
        udev_unref(in->udev);
    free(in);
}
