/* xw-compositor.c — display server bootstrap, globals, event loop. */
#include "xw-internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------- wl_compositor */

static void compositor_create_surface(struct wl_client *client,
                                      struct wl_resource *res, uint32_t id) {
    struct xw_compositor *c = wl_resource_get_user_data(res);
    xw_surface_create(client, c, id, wl_resource_get_version(res));
}

static void region_destroy(struct wl_client *client, struct wl_resource *res);
static void region_add(struct wl_client *client, struct wl_resource *res,
                       int32_t x, int32_t y, int32_t w, int32_t h);
static void region_sub(struct wl_client *client, struct wl_resource *res,
                       int32_t x, int32_t y, int32_t w, int32_t h);

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_sub,
};

static void region_resource_destroy(struct wl_resource *res) {
    pixman_region16_t *r = wl_resource_get_user_data(res);
    pixman_region_fini(r);
    free(r);
}

static void compositor_create_region(struct wl_client *client,
                                     struct wl_resource *res, uint32_t id) {
    (void)res;
    pixman_region16_t *r = calloc(1, sizeof(*r));
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    pixman_region_init(r);
    struct wl_resource *rr =
        wl_resource_create(client, &wl_region_interface, 1, id);
    if (!rr) {
        pixman_region_fini(r);
        free(r);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(rr, &region_impl, r, region_resource_destroy);
}

static void region_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}
static void region_add(struct wl_client *client, struct wl_resource *res,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)client;
    pixman_region16_t *r = wl_resource_get_user_data(res);
    pixman_region_union_rect(r, r, x, y, w, h);
}
static void region_sub(struct wl_client *client, struct wl_resource *res,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)client;
    pixman_region16_t *r = wl_resource_get_user_data(res);
    pixman_region16_t tmp;
    pixman_region_init_rect(&tmp, x, y, w, h);
    pixman_region_subtract(r, r, &tmp);
    pixman_region_fini(&tmp);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
    struct xw_compositor *c = data;
    struct wl_resource *res = wl_resource_create(client, &wl_compositor_interface,
                                                 version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &compositor_impl, c, NULL);
}

/* ------------------------------------------------ single pixel buffer v1 */

static void single_pixel_destroy(struct wl_client *client,
                                 struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct wl_buffer_interface single_pixel_buffer_impl = {
    .destroy = single_pixel_destroy,
};

static void single_pixel_resource_destroy(struct wl_resource *res) {
    free(wl_resource_get_user_data(res));
}

static void single_pixel_create(struct wl_client *client,
                                struct wl_resource *res, uint32_t id,
                                uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    (void)res;
    uint32_t *data = calloc(1, sizeof(*data));
    if (!data) {
        wl_client_post_no_memory(client);
        return;
    }
    /* premultiplied ARGB in wl_shm byte order (little endian) */
    uint32_t ar = (a * r) / 0xffffffffu, ag = (a * g) / 0xffffffffu,
             ab = (a * b) / 0xffffffffu;
    *data = (a << 24) | (ab << 16) | (ag << 8) | ar;
    if (a == 0)
        *data = 0;
    struct wl_resource *br = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!br) {
        free(data);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(br, &single_pixel_buffer_impl, data,
                                   single_pixel_resource_destroy);
}

static const struct wp_single_pixel_buffer_manager_v1_interface
    single_pixel_mgr_impl = {
    .destroy = single_pixel_destroy,
    .create_u32_rgba_buffer = single_pixel_create,
};

static void bind_single_pixel(struct wl_client *client, void *data,
                              uint32_t version, uint32_t id) {
    struct wl_resource *res = wl_resource_create(
        client, &wp_single_pixel_buffer_manager_v1_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &single_pixel_mgr_impl, data, NULL);
}

/* ------------------------------------------------------------ lifecycle */

static int on_signal(int sig, void *data) {
    struct xw_compositor *c = data;
    xw_log(XW_LOG_INFO, "received signal %d, stopping", sig);
    c->running = false;
    return 0;
}

static int reap_children(int sig, void *data) {
    /* Reap only children the compositor itself spawned. Embedder children
     * (harness, session manager) must stay reapable by their owner: a
     * blanket waitpid(-1) here silently steals their exit statuses. */
    (void)sig;
    struct xw_compositor *c = data;
    for (int i = 0; i < c->n_children;) {
        pid_t r = waitpid(c->children[i], NULL, WNOHANG);
        if (r == c->children[i] || (r < 0 && errno == ECHILD)) {
            xw_log(XW_LOG_DEBUG, "child %d exited", (int)c->children[i]);
            c->children[i] = c->children[--c->n_children];
        } else {
            i++;
        }
    }
    return 0;
}

void xw_compositor_track_child(struct xw_compositor *c, pid_t pid) {
    if (pid <= 0)
        return;
    if (c->n_children >= XW_MAX_CHILDREN) {
        xw_log(XW_LOG_WARN, "child table full; %d will not be reaped",
               (int)pid);
        return;
    }
    c->children[c->n_children++] = pid;
}

static void on_repaint_idle(void *data) {
    struct xw_compositor *c = data;
    c->repaint_scheduled = false;
    c->repaint_idle = NULL; /* idle sources free themselves after dispatch */
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link) {
        if (pixman_region_not_empty(&o->damage))
            xw_output_repaint(o);
    }
    /* session lock: now that this cycle presented every damaged
     * output, a pending `locked` event can be flushed (the spec
     * forbids sending it before a locked frame was presented). Runs
     * even when nothing repainted: with zero outputs the presentation
     * requirement is vacuously met. */
    xw_session_lock_after_present(c);
    wl_display_flush_clients(c->display);
    /* idle sources are one-shot */
}

void xw_schedule_repaint(struct xw_compositor *c) {
    if (c->repaint_scheduled)
        return;
    c->repaint_scheduled = true;
    c->repaint_idle = wl_event_loop_add_idle(c->loop, on_repaint_idle, c);
}

/* ------------------------------------------------------------- public API */

struct xw_compositor *xw_compositor_create(const struct xw_compositor_config *cfg) {
    struct xw_compositor *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->conf = *cfg;
    if (cfg->log_level)
        xw_log_set_level(cfg->log_level);
    c->running = false;
    c->exit_code = 0;
    c->bg_color = 0xff202530;
    wl_list_init(&c->outputs);
    wl_list_init(&c->surfaces);
    wl_list_init(&c->seats);
    wl_list_init(&c->popups);
    wl_list_init(&c->ft_managers);
    wl_list_init(&c->ws_managers);
    wl_list_init(&c->activation_tokens);

    c->display = wl_display_create();
    if (!c->display)
        goto fail;
    c->loop = wl_display_get_event_loop(c->display);

    /* arm signals as early as possible: a TERM arriving during the rest
     * of create (backend handshake, shell setup) must still lead to a
     * clean exit instead of killing the process with the default
     * disposition. SIGHUP is in the set for the real-session case: the
     * controlling terminal going away (shell exited, agetty recycled
     * the login) must run the FULL teardown — the direct seat provider
     * restores KDSETMODE/VT_SETMODE/termios in its destroy path, and
     * dying with SIGHUP's default disposition would leave the VT in
     * graphics mode holding a dead owner (the trapped-console failure
     * mode). */
    c->sigint_src = wl_event_loop_add_signal(c->loop, SIGINT, on_signal, c);
    c->sigterm_src = wl_event_loop_add_signal(c->loop, SIGTERM, on_signal, c);
    c->sighup_src = wl_event_loop_add_signal(c->loop, SIGHUP, on_signal, c);
    c->sigchld_src = wl_event_loop_add_signal(c->loop, SIGCHLD, reap_children, c);
    if (!c->sigint_src || !c->sigterm_src || !c->sighup_src ||
        !c->sigchld_src)
        goto fail;

    if (wl_display_init_shm(c->display) < 0) {
        xw_log(XW_LOG_ERROR, "wl_display_init_shm failed");
        goto fail;
    }

    c->g_compositor = wl_global_create(c->display, &wl_compositor_interface, 4,
                                       c, bind_compositor);
    c->g_single_pixel = wl_global_create(
        c->display, &wp_single_pixel_buffer_manager_v1_interface, 1, c,
        bind_single_pixel);
    if (!c->g_compositor || !c->g_single_pixel)
        goto fail;

    /* config */
    if (c->conf.config_dir) {
        c->conf_dir_owned = strdup(c->conf.config_dir);
        char path[512];
        snprintf(path, sizeof(path), "%s/compositor.conf", c->conf_dir_owned);
        struct xw_ini *ini = xw_ini_load(path);
        if (ini) {
            const char *v = xw_ini_get(ini, "background", "color");
            if (v)
                c->bg_color = (uint32_t)strtoul(v, NULL, 0);
            xw_ini_free(ini);
        }
        snprintf(path, sizeof(path), "%s/keyboard.conf", c->conf_dir_owned);
        ini = xw_ini_load(path);
        if (ini) {
            const char *v;
            if ((v = xw_ini_get(ini, "keyboard", "repeat_delay_ms")) &&
                atoi(v) > 0)
                c->conf.repeat_delay_ms = atoi(v);
            if ((v = xw_ini_get(ini, "keyboard", "repeat_rate_hz")) &&
                atoi(v) > 0)
                c->conf.repeat_rate_hz = atoi(v);
            xw_ini_free(ini);
        }
    }
    /* env overrides (debug/testing knobs, documented in BUILDING.md) */
    {
        const char *e = getenv("XW_REPEAT_DELAY_MS");
        if (e && atoi(e) > 0)
            c->conf.repeat_delay_ms = atoi(e);
        e = getenv("XW_REPEAT_RATE_HZ");
        if (e && atoi(e) > 0)
            c->conf.repeat_rate_hz = atoi(e);
    }

    /* backend + outputs. The DRM backend needs a seat/session provider
     * FIRST (device fds come from it), so the order is: seat, then
     * backend (which registers its session-event hooks), then the rest. */
    switch (cfg->backend) {
    case XW_BACKEND_NESTED:
        c->backend = xw_backend_nested_create(c, cfg);
        break;
    case XW_BACKEND_X11:
#ifdef XW_HAVE_X11_BACKEND
        c->backend = xw_backend_x11_create(c, cfg);
#else
        xw_log(XW_LOG_ERROR, "x11 backend not compiled in (no libX11 at "
                            "build time)");
        c->backend = NULL;
#endif
        break;
    case XW_BACKEND_DRM:
#ifdef XW_HAVE_DRM_BACKEND
        c->seat = xw_seat_session_open(c,
                               cfg->seat_provider ? cfg->seat_provider
                                                  : XW_SEAT_PROVIDER_AUTO,
                               cfg->seat_name);
        if (!c->seat)
            goto fail; /* honest diagnostics were logged by the seat */
        c->backend = xw_backend_drm_create(c, cfg);
#else
        xw_log(XW_LOG_ERROR,
               "drm backend not compiled in (libdrm development files "
               "were absent at build time) — headless/nested still work");
        c->backend = NULL;
#endif
        break;
    default:
        c->backend = xw_backend_headless_create(c, cfg);
        break;
    }
    if (!c->backend)
        goto fail;

    /* window manager before shells (shells call into wm) */
    c->wm = xw_wm_create(c, c->conf.config_dir);
    if (!c->wm)
        goto fail;

    /* seats */
    const char *seat_name = cfg->seat_name ? cfg->seat_name : "seat0";
    struct xw_seat *seat = xw_seat_create(c, seat_name);
    if (!seat)
        goto fail;

    /* shells and desktop integration protocols */
    xw_actions_init(c);
    xw_xdg_shell_init(c);
    xw_layer_shell_init(c);
    xw_session_lock_init(c);
    xw_idle_init(c);
    xw_foreign_toplevel_init(c);
    xw_ext_workspace_init(c);
    xw_activation_init(c);
    xw_data_device_init(c);

    /* shortcuts */
    c->shortcuts = xw_shortcuts_create(c, c->conf.config_dir);
    if (!c->shortcuts)
        goto fail;

    /* real-input source (libinput). AUTO keeps tests and nested
     * sessions off system devices; only an explicit XW_INPUT_DEVICES
     * list, XW_INPUT_LIBINPUT, or the DRM backend (a real session
     * needs real input) opt in. */
#ifdef XW_HAVE_LIBINPUT
    {
        bool want_input = false;
        switch (cfg->input_mode) {
        case XW_INPUT_LIBINPUT:
            want_input = true;
            break;
        case XW_INPUT_NONE:
            want_input = false;
            break;
        default: {
            const char *devs = getenv("XW_INPUT_DEVICES");
            want_input = (devs && *devs &&
                          cfg->backend != XW_BACKEND_NESTED &&
                          cfg->backend != XW_BACKEND_X11) ||
                         cfg->backend == XW_BACKEND_DRM;
            break;
        }
        }
        if (want_input) {
            c->input = xw_input_libinput_create(c);
            if (!c->input)
                xw_log(XW_LOG_WARN, "input: real-input source unavailable; "
                                    "continuing with injection-only input");
        }
    }
#endif

    /* socket */
    const char *sock = NULL;
    if (c->conf.socket_name) {
        if (wl_display_add_socket(c->display, c->conf.socket_name) == 0)
            sock = c->conf.socket_name;
    } else {
        sock = wl_display_add_socket_auto(c->display);
    }
    if (!sock) {
        xw_log(XW_LOG_ERROR, "failed to add wayland socket");
        goto fail;
    }
    const char *rtd = getenv("XDG_RUNTIME_DIR");
    if (rtd)
        snprintf(c->socket_path, sizeof(c->socket_path), "%s/%s", rtd, sock);
    else
        snprintf(c->socket_path, sizeof(c->socket_path), "%s", sock);
    xw_log(XW_LOG_INFO, "compositor ready: socket %s, %d output(s)",
           c->socket_path, xw_compositor_n_outputs(c));
    return c;

fail:
    xw_log(XW_LOG_ERROR, "compositor creation failed");
    xw_compositor_destroy(c);
    return NULL;
}

void xw_compositor_destroy(struct xw_compositor *c) {
    if (!c)
        return;
    /* order matters: client resources first (role teardown unmanages
     * windows and clears seat focus), then seats/wm, then the backend
     * (output globals must die while the display is still alive —
     * wl_display_destroy would already have freed them) */
    if (c->display)
        wl_display_destroy_clients(c->display);

    if (c->sigint_src)
        wl_event_source_remove(c->sigint_src);
    if (c->sigterm_src)
        wl_event_source_remove(c->sigterm_src);
    if (c->sighup_src)
        wl_event_source_remove(c->sighup_src);
    if (c->sigchld_src)
        wl_event_source_remove(c->sigchld_src);
    if (c->repaint_idle)
        wl_event_source_remove(c->repaint_idle);

#ifdef XW_HAVE_LIBINPUT
    if (c->input) {
        xw_input_libinput_destroy(c->input);
        c->input = NULL;
    }
#endif

    xw_shortcuts_destroy(c->shortcuts);
    c->shortcuts = NULL;

    struct xw_seat *seat, *seat2;
    wl_list_for_each_safe(seat, seat2, &c->seats, link)
        xw_seat_destroy(seat);

    xw_wm_destroy(c->wm);
    c->wm = NULL;

    xw_backend_destroy(c->backend);
    c->backend = NULL;

    /* the seat provider outlives the backend: the backend's destroy
     * released its devices through it */
    if (c->seat) {
        xw_seat_session_destroy(c->seat);
        c->seat = NULL;
    }

    xw_layer_shell_fin(c);
    xw_session_lock_fin(c);
    xw_idle_fin(c);
    xw_activation_fin(c);

    if (c->display)
        wl_display_destroy(c->display);
    free(c->conf_dir_owned);
    free(c);
}

/* ------------------------------------------------------- backend generic */

void xw_backend_destroy(struct xw_backend *b) {
    if (!b)
        return;
    struct xw_compositor *c = b->comp;
    if (b->ops && b->ops->destroy)
        b->ops->destroy(b);
    else
        free(b);
    /* contract: outputs are owned by the compositor, not the backend;
     * freed here while the display is still alive */
    xw_backend_destroy_outputs(c);
}

void xw_backend_destroy_outputs(struct xw_compositor *c) {
    struct xw_output *o, *o2;
    wl_list_for_each_safe(o, o2, &c->outputs, link)
        xw_output_destroy(o);
}

/* ----------------------------------------------------------- input inject */

void xw_compositor_inject_key(struct xw_compositor *c, uint32_t linux_keycode,
                              bool down) {
    struct xw_seat *s = xw_seat_first(c);
    if (s)
        xw_seat_key(s, linux_keycode, down);
}

void xw_compositor_inject_pointer_motion(struct xw_compositor *c, int x, int y) {
    xw_log(XW_LOG_DEBUG, "compositor: cursor position updated -> %d,%d", x, y);
    struct xw_seat *s = xw_seat_first(c);
    if (s)
        xw_seat_pointer_motion(s, x, y);
}

void xw_compositor_inject_pointer_button(struct xw_compositor *c,
                                         uint32_t linux_button, bool down) {
    struct xw_seat *s = xw_seat_first(c);
    if (s)
        xw_seat_pointer_button(s, linux_button, down);
}

void xw_compositor_inject_pointer_axis(struct xw_compositor *c,
                                       uint32_t axis, double value) {
    struct xw_seat *s = xw_seat_first(c);
    if (s)
        xw_seat_pointer_axis(s, axis, value);
}

int xw_compositor_run(struct xw_compositor *c) {
    c->running = true;
    while (c->running) {
        wl_event_loop_dispatch(c->loop, 100);
        wl_display_flush_clients(c->display);
    }
    xw_log(XW_LOG_INFO, "compositor exiting cleanly");
    return c->exit_code;
}

void xw_compositor_stop(struct xw_compositor *c) {
    c->running = false;
}

void xw_compositor_dispatch(struct xw_compositor *c, int timeout_ms) {
    wl_event_loop_dispatch(c->loop, timeout_ms);
    wl_display_flush_clients(c->display);
}

const char *xw_compositor_socket_path(const struct xw_compositor *c) {
    return c->socket_path;
}

int xw_compositor_n_outputs(const struct xw_compositor *c) {
    int n = 0;
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link)
        n++;
    return n;
}

bool xw_compositor_output_info(const struct xw_compositor *c, int index,
                               int *x, int *y, int *w, int *h, int *scale) {
    int i = 0;
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link) {
        if (i++ != index)
            continue;
        if (x) *x = o->x;
        if (y) *y = o->y;
        if (w) *w = o->width;
        if (h) *h = o->height;
        if (scale) *scale = o->scale;
        return true;
    }
    return false;
}

const uint32_t *xw_compositor_output_pixels(const struct xw_compositor *c,
                                            int index, int *w, int *h) {
    int i = 0;
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link) {
        if (i++ != index)
            continue;
        if (w) *w = pixman_image_get_width(o->native);
        if (h) *h = pixman_image_get_height(o->native);
        return (const uint32_t *)pixman_image_get_data(o->native);
    }
    return NULL;
}

void xw_compositor_set_action_hook(struct xw_compositor *c,
                                    bool (*hook)(int action, const char *arg,
                                                 void *ud),
                                    void *ud) {
    c->action.hook = hook;
    c->action.ud = ud;
}

int xw_compositor_window_count(const struct xw_compositor *c) {
    if (!c->wm)
        return 0;
    int n = 0;
    struct xw_window *w;
    wl_list_for_each(w, &c->wm->windows, link)
        n++;
    return n;
}

int xw_compositor_workspace_count(const struct xw_compositor *c) {
    return c->wm ? c->wm->ws_count : 0;
}

int xw_compositor_workspace_current(const struct xw_compositor *c) {
    return c->wm ? c->wm->ws_current : 0;
}

void xw_compositor_focused_title(const struct xw_compositor *c, char *buf,
                                 size_t len) {
    if (!buf || !len)
        return;
    buf[0] = 0;
    if (c->wm && c->wm->focused)
        snprintf(buf, len, "%s", c->wm->focused->title);
}
