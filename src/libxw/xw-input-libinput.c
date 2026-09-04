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

#include <dirent.h>
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
    /* seat-provider device bookkeeping: libinput hands us only the fd
     * in close_restricted, so map fd -> seat device id here */
    struct {
        int fd;
        int dev_id;
    } seat_devs[16];
    int n_seat_devs;
    bool path_mode; /* XW_INPUT_DEVICES set: device list is fixed */
    /* acquisition bookkeeping for the startup report: every device
     * libinput successfully added (with its capabilities) and every
     * failed device open, so "cursor visible but input dead" has an
     * explicit, structured explanation in the log instead of a bare
     * permission-denied line the user cannot act on */
    int n_added, n_kbd, n_ptr;
    int n_open_fail;
    char last_fail_path[128];
    int last_fail_errno;
    /* startup diagnostics window: per-type event counters printed at
     * INFO level on a 2s timer for the first 30s. The per-event DEBUG
     * lines are invisible at the default log level, which made
     * "devices are open, but is anything arriving?" unanswerable
     * from a default-level capture — these counters answer it. */
    uint64_t n_motion, n_abs, n_key, n_button, n_axis;
    int64_t started_ms;   /* session start (stats reference) */
    int64_t first_ptr_ms, first_key_ms; /* 0 = not yet seen */
    struct wl_event_source *stats_src;
    int stats_ticks;
};

/* how many evdev nodes exist at all (diagnostics only; libinput owns
 * real discovery) */
static int count_event_nodes(void) {
    DIR *d = opendir("/dev/input");
    if (!d)
        return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)))
        if (strncmp(de->d_name, "event", 5) == 0)
            n++;
    closedir(d);
    return n;
}

/* ------------------------------------------------------- open/close glue */

/* Devices are opened through the compositor's seat/session provider
 * when one exists (real DRM sessions): the seat manager owns device
 * access, and libinput's open_restricted is exactly the hook it uses.
 * Without a seat (headless + XW_INPUT_DEVICES debugging) this is a
 * plain privileged-free open(). */
static void note_open_failure(struct xw_input_libinput *in, const char *path,
                              int e) {
    in->n_open_fail++;
    snprintf(in->last_fail_path, sizeof(in->last_fail_path), "%s", path);
    in->last_fail_errno = e;
}

static int open_restricted(const char *path, int flags, void *ud) {
    struct xw_input_libinput *in = ud;
    int fd = -1;
    if (in->comp->seat) {
        int dev_id = xw_seat_session_open_device(in->comp->seat, path, &fd);
        if (dev_id < 0) {
            note_open_failure(in, path, errno);
            xw_log(XW_LOG_ERROR,
                   "input: the seat provider (%s) refused device %s: %s",
                   xw_seat_session_desc(in->comp->seat), path,
                   strerror(errno));
            return -1;
        }
        if (in->n_seat_devs < 16) {
            in->seat_devs[in->n_seat_devs].fd = fd;
            in->seat_devs[in->n_seat_devs].dev_id = dev_id;
            in->n_seat_devs++;
        }
        /* the anti-pattern this line rules out: acquire through the
         * seat, ignore the returned fd, then open /dev/input/event*
         * directly. libinput will read THIS fd — the one the seat
         * manager granted — and nothing else. */
        xw_log(XW_LOG_INFO,
               "input: %s opened through seat provider %s (fd %d, "
               "seat device id %d)",
               path, xw_seat_session_desc(in->comp->seat), fd, dev_id);
    } else {
        fd = open(path, flags | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            note_open_failure(in, path, errno);
            xw_log(XW_LOG_WARN, "input: cannot open device %s: %s", path,
                   strerror(errno));
        }
    }
    return fd;
}

static void close_restricted(int fd, void *ud) {
    struct xw_input_libinput *in = ud;
    if (in->comp->seat) {
        for (int i = 0; i < in->n_seat_devs; i++) {
            if (in->seat_devs[i].fd == fd) {
                xw_seat_session_close_device(in->comp->seat,
                                     in->seat_devs[i].dev_id);
                in->seat_devs[i] = in->seat_devs[--in->n_seat_devs];
                return; /* the seat manager closed the fd */
            }
        }
    }
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
    xw_log(XW_LOG_DEBUG, "xw-input: pointer motion -> %d,%d", nx, ny);
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
    xw_log(XW_LOG_DEBUG, "xw-input: pointer abs -> %d,%d", x, y);
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

/* one-time INFO marker for the first pointer/key event of the session.
 * With the per-event lines at DEBUG, a default-level log gives zero
 * evidence either way; this single line separates "events never
 * arrive" from "events arrive but the cursor still does not move". */
static void note_first_pointer_event(struct xw_input_libinput *in) {
    if (in->first_ptr_ms)
        return;
    in->first_ptr_ms = xw_now_ms();
    xw_log(XW_LOG_INFO,
           "input: first pointer event %.1fs after startup — pointer "
           "events ARE reaching the compositor",
           (in->first_ptr_ms - in->started_ms) / 1000.0);
}

static void note_first_key_event(struct xw_input_libinput *in) {
    if (in->first_key_ms)
        return;
    in->first_key_ms = xw_now_ms();
    xw_log(XW_LOG_INFO,
           "input: first key event %.1fs after startup — key events ARE "
           "reaching the compositor",
           (in->first_key_ms - in->started_ms) / 1000.0);
}

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
        bool kbd = libinput_device_has_capability(
            dev, LIBINPUT_DEVICE_CAP_KEYBOARD);
        bool ptr = libinput_device_has_capability(
            dev, LIBINPUT_DEVICE_CAP_POINTER);
        in->n_added++;
        if (kbd)
            in->n_kbd++;
        if (ptr)
            in->n_ptr++;
        xw_log(XW_LOG_INFO, "input: device added: %s (%s%s%s)",
               device_name(dev), kbd ? "keyboard" : "",
               kbd && ptr ? "+" : "", !kbd && !ptr ? "other"
                                               : ptr ? "pointer" : "");
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
        in->n_key++;
        note_first_key_event(in);
        xw_log(XW_LOG_DEBUG, "libinput: KEY %u %s", code,
               down ? "down" : "up");
        /* XW_INPUT_TRACE=1 physical chain, step 1 of 3: what libinput
         * itself reported (raw linux keycode + device). Steps 2/3 are
         * the seat's entry/outcome lines. A keycode mismatch between
         * this line and the seat's "raw=" pinpoints translation damage
         * in between; equal raw codes with a wrong client keysym pin
         * the bug to the client or the keymap it compiled. */
        xw_input_trace("libinput: KEY raw=%u %s", code,
                       down ? "down" : "up");
        xw_input_handle_key(in, code, down);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION: {
        struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
        double dx = libinput_event_pointer_get_dx(pev);
        double dy = libinput_event_pointer_get_dy(pev);
        in->n_motion++;
        note_first_pointer_event(in);
        xw_log(XW_LOG_DEBUG, "libinput: POINTER_MOTION dx=%.2f dy=%.2f",
               dx, dy);
        xw_input_handle_pointer_rel(in, dx, dy);
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
        struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
        double x = libinput_event_pointer_get_absolute_x(pev);
        double y = libinput_event_pointer_get_absolute_y(pev);
        in->n_abs++;
        note_first_pointer_event(in);
        xw_log(XW_LOG_DEBUG,
               "libinput: POINTER_MOTION_ABSOLUTE x=%.4f y=%.4f", x, y);
        xw_input_handle_pointer_abs(in, x, y);
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        struct libinput_event_pointer *pev = libinput_event_get_pointer_event(ev);
        uint32_t btn = libinput_event_pointer_get_button(pev);
        bool down = libinput_event_pointer_get_button_state(pev) ==
                    LIBINPUT_BUTTON_STATE_PRESSED;
        in->n_button++;
        xw_log(XW_LOG_DEBUG, "libinput: BUTTON %u %s", btn,
               down ? "down" : "up");
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
            in->n_axis++; /* per axis: one wheel tick can carry both */
            if (src == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL) {
                /* high-res wheels: 120 units per traditional notch */
                value = libinput_event_pointer_get_scroll_value_v120(pev, axes[i].li) / 120.0;
                continuous = false;
            } else {
                /* touchpad two-finger / continuous scroll: pixel distance */
                value = libinput_event_pointer_get_scroll_value(pev, axes[i].li);
                continuous = true;
            }
            xw_log(XW_LOG_DEBUG, "libinput: AXIS %s%s value=%.2f",
                   axes[i].wl == WL_POINTER_AXIS_VERTICAL_SCROLL
                       ? "vertical"
                       : "horizontal",
                   continuous ? " (continuous)" : " (wheel)", value);
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
    while ((ev = libinput_get_event(in->li))) {
        handle_libinput_event(in, ev);
        /* libinput contract: every event gotten must be destroyed by
         * the caller; dropping this leaks one event object per key
         * press, motion sample and wheel tick in real sessions */
        libinput_event_destroy(ev);
    }
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

/* ------------------------------------------------ acquisition report */

/* The explicit, structured "input is dead and here is why" report the
 * real-session contract requires. Printed once at startup when no
 * keyboard AND no pointer were acquired: a visible cursor proves only
 * scanout, not input, so this is the line the user must be able to
 * find and act on. Suggested fixes name the legitimate mechanisms
 * (seat manager configuration, group membership) — never chmod or
 * root. */
void xw_input_log_acquisition_failure(const char *backend_name,
                                      const char *seat_provider,
                                      const char *seat_name,
                                      bool session_active, int nodes_present,
                                      int devices_acquired, int keyboards,
                                      int pointers, const char *fail_path,
                                      int fail_errno) {
    xw_log(XW_LOG_ERROR,
           "input: failed to acquire keyboard/pointer devices through "
           "the active seat\n"
           "  seat provider:    %s\n"
           "  seat name:        %s\n"
           "  session state:    %s\n"
           "  backend:          %s\n"
           "  /dev/input:       %d event node(s) present, %d device(s) "
           "acquired (%d keyboard, %d pointer)",
           seat_provider ? seat_provider : "none",
           seat_name ? seat_name : "?",
           session_active ? "active" : "inactive",
           backend_name ? backend_name : "?", nodes_present, devices_acquired,
           keyboards, pointers);
    if (fail_path && fail_errno)
        xw_log(XW_LOG_ERROR, "  last open failure: %s: %s", fail_path,
               strerror(fail_errno));
    xw_log(XW_LOG_ERROR,
           "  suggested legitimate configuration fixes:\n"
           "    - seatd: enable the seatd service and add this user to the\n"
           "      'seat' group (device fds are granted per active session)\n"
           "    - logind/elogind: ensure it is running and this login is a\n"
           "      registered, active session (device ACLs follow it)\n"
           "    - direct VT sessions (no seat manager): the login's own\n"
           "      permissions apply — add the user to the 'input' group\n"
           "      (e.g. usermod -aG input <user>), then log out and back in\n"
           "  The compositor keeps running (Ctrl+C returns to the TTY); it\n"
           "  never falls back to running as root or relaxing device "
           "permissions.");
}

/* -------------------------------------------- startup stats timer --- */

/* 2s INFO summary of everything the input path has seen — the input
 * half of the "cursor does not move" bisect, visible at the DEFAULT
 * log level. Runs for the first 30s (a diagnostic capture window),
 * then removes itself and goes silent. */
#define XW_INPUT_STATS_MS 2000
#define XW_INPUT_STATS_TICKS 15

static int input_stats_timer_cb(void *data) {
    struct xw_input_libinput *in = data;
    double up = (xw_now_ms() - in->started_ms) / 1000.0;
    struct xw_seat *s = xw_seat_first(in->comp);
    xw_log(XW_LOG_INFO,
           "input: stats %.0fs: motion=%llu abs=%llu key=%llu button=%llu "
           "axis=%llu cursor=%d,%d%s",
           up, (unsigned long long)in->n_motion,
           (unsigned long long)in->n_abs, (unsigned long long)in->n_key,
           (unsigned long long)in->n_button,
           (unsigned long long)in->n_axis,
           s ? (int)s->cursor_x : -1, s ? (int)s->cursor_y : -1,
           (in->n_motion || in->n_abs)
               ? ""
               : " [no pointer events yet — wiggle the mouse]");
    if (++in->stats_ticks >= XW_INPUT_STATS_TICKS) {
        wl_event_source_remove(in->stats_src);
        in->stats_src = NULL;
        return 0; /* source removed; nothing more to schedule */
    }
    wl_event_source_timer_update(in->stats_src, XW_INPUT_STATS_MS);
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
    in->started_ms = xw_now_ms();
    /* the udev seat grouping tag: prefer the name the seat provider
     * actually negotiated (seatd servers name their seat in the
     * SEAT_OPENED reply); fall back to the config value, then "seat0" */
    const char *seat_for_udev =
        c->seat ? xw_seat_session_name(c->seat)
                : (c->conf.seat_name ? c->conf.seat_name : "seat0");
    snprintf(in->seat, sizeof(in->seat), "%s", seat_for_udev);

    const char *devlist = getenv("XW_INPUT_DEVICES");

    if (devlist && *devlist) {
        /* path mode: explicit devices, no hotplug, no udev dependency */
        in->path_mode = true;
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
        /* udev seat mode: real sessions; discovery + hotplug via udev.
         * Device opens still go through the seat provider (see
         * open_restricted): libinput calls it for every device it
         * enumerates. */
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
        xw_log(XW_LOG_INFO, "input: libinput udev mode, seat '%s'%s", in->seat,
               c->seat ? " (devices opened through the seat provider)" : "");
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

    /* acquisition report: a summary line always, the structured
     * failure report when neither keyboard nor pointer could be
     * acquired ("cursor visible but input dead" must be explicit) */
    {
        int nodes = count_event_nodes();
        const char *via = c->seat ? xw_seat_session_desc(c->seat)
                                  : "direct open (no seat provider)";
        xw_log(XW_LOG_INFO,
               "input: %d device(s) acquired through %s (%d keyboard, %d "
               "pointer; %d /dev/input event node(s) present%s)",
               in->n_added, via, in->n_kbd, in->n_ptr, nodes,
               in->path_mode ? "; path mode" : "");
        if (in->n_kbd == 0 && in->n_ptr == 0) {
            /* build capability context: an elogind/logind session path
             * silently missing from the binary is a build problem the
             * user can only see if we say so */
#ifdef XW_HAVE_LIBSEAT
            xw_log(XW_LOG_ERROR,
                   "input: note: this build has libseat support compiled "
                   "in (elogind/logind sessions are reachable)");
#else
            xw_log(XW_LOG_ERROR,
                   "input: note: this build has NO libseat support — the "
                   "elogind/logind seat path is compiled out; rebuild "
                   "with libseat development files installed (see "
                   "BUILDING.md)");
#endif
            xw_input_log_acquisition_failure(
                c->backend ? c->backend->name : "?", via, in->seat,
                xw_seat_session_active(c->seat), nodes, in->n_added,
                in->n_kbd, in->n_ptr,
                in->n_open_fail ? in->last_fail_path : NULL,
                in->n_open_fail ? in->last_fail_errno : 0);
        }
    }

    /* startup stats window (see input_stats_timer_cb): the definitive
     * "events or no events" line at default log level */
    in->stats_src = wl_event_loop_add_timer(c->loop, input_stats_timer_cb, in);
    if (in->stats_src)
        wl_event_source_timer_update(in->stats_src, XW_INPUT_STATS_MS);

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
    if (in->stats_src)
        wl_event_source_remove(in->stats_src);
    if (in->fd_src)
        wl_event_source_remove(in->fd_src);
    /* draining unreturned events keeps the log honest on teardown */
    libinput_unref(in->li);
    if (in->udev)
        udev_unref(in->udev);
    free(in);
}

/* ------------------------------------------------------ session lifecycle */

/* VT switch away: stop reading input devices. libinput_suspend closes
 * them (through close_restricted, i.e. through the seat provider) so
 * another session's compositor can read them. */
void xw_input_libinput_suspend(struct xw_input_libinput *in) {
    if (!in)
        return;
    xw_log(XW_LOG_INFO, "input: suspending (session inactive)");
    libinput_suspend(in->li);
}

/* VT switch back: re-open the devices (udev mode re-attaches the seat;
 * path mode re-adds each listed device) */
int xw_input_libinput_resume(struct xw_input_libinput *in) {
    if (!in)
        return 0;
    xw_log(XW_LOG_INFO, "input: resuming (session active)");
    if (libinput_resume(in->li) == 0)
        return 0;
    xw_log(XW_LOG_ERROR, "input: libinput resume failed: %s",
           strerror(errno));
    return -1;
}
