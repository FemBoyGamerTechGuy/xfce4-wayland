/* xwc.c — libxwcl implementation: connection, windows, layers, input. */
#include "xwc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wayland-client.h"
#include "xdg-shell.h"
#include "wlr-layer-shell-unstable-v1.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"
#include "ext-workspace.h"
#include "ext-session-lock.h"
#include "ext-idle-notify.h"

/* xwc-input.c */
extern void xwc_seat_init(struct xwc *c);
extern void xwc_output_init(struct xwc *c, struct wl_registry *r, uint32_t name,
                            uint32_t version);

/* ------------------------------------------------- surface owner map */

struct xwc_owner {
    struct wl_surface *surface;
    void *owner;              /* struct xwc_win or struct xwc_layer */
    struct xwc_callbacks cb;
};

#define XWC_MAX_OWNERS 16

static struct xwc_owner g_owners[XWC_MAX_OWNERS];

void xwc_register_surface(struct xwc *c, void *owner,
                          const struct xwc_callbacks *cb,
                          struct wl_surface *surface) {
    (void)c;
    for (int i = 0; i < XWC_MAX_OWNERS; i++) {
        if (!g_owners[i].surface) {
            g_owners[i].surface = surface;
            g_owners[i].owner = owner;
            g_owners[i].cb = *cb;
            return;
        }
    }
}

void xwc_unregister_surface(struct xwc *c, struct wl_surface *surface) {
    (void)c;
    for (int i = 0; i < XWC_MAX_OWNERS; i++) {
        if (g_owners[i].surface == surface) {
            if (c->focused_owner == g_owners[i].owner) {
                c->focused_owner = NULL;
                c->has_focus = false;
            }
            g_owners[i].surface = NULL;
            g_owners[i].owner = NULL;
            return;
        }
    }
}

/* record the owner of a surface as the input-focus target */
void xwc_surface_focus(struct xwc *c, struct wl_surface *surface) {
    if (!surface) {
        c->focused_owner = NULL;
        c->has_focus = false;
        return;
    }
    for (int i = 0; i < XWC_MAX_OWNERS; i++) {
        if (g_owners[i].surface == surface) {
            c->focused_owner = g_owners[i].owner;
            c->focused_cb = g_owners[i].cb;
            c->has_focus = true;
            return;
        }
    }
}

/* --------------------------------------------------------- registry */

struct xwc_win {
    struct xwc *c;
    struct xwc_callbacks cb;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_callback *frame_cb;

    struct wl_shm_pool *pool;
    struct wl_buffer *bufs[2];
    int cur;              /* current back buffer index */
    uint32_t *data;       /* pool mapping */
    size_t pool_size;
    /* previous pool awaiting safe destruction (see win_pool_swap): the
     * compositor keeps rendering from the attached wl_buffer until the
     * next commit, so the old buffers must not be destroyed before the
     * new one is committed */
    struct wl_shm_pool *old_pool;
    struct wl_buffer *old_bufs[2];
    uint32_t *old_data;
    size_t old_pool_size;
    int w, h;
    bool mapped;
    bool closed;
    int conf_w, conf_h;   /* last configure size (0 = pick) */
    bool need_ack;
    uint32_t ack_serial;
};

struct xwc_layer {
    struct xwc *c;
    struct xwc_callbacks cb;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *ls;
    struct wl_shm_pool *pool;
    struct wl_buffer *bufs[2];
    int cur;
    uint32_t *data;
    size_t pool_size;
    /* previous pool awaiting safe destruction (see layer_configure) */
    struct wl_shm_pool *old_pool;
    struct wl_buffer *old_bufs[2];
    uint32_t *old_data;
    size_t old_pool_size;
    int w, h;
    bool mapped;
    bool conf_pending;
};

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t version) {
    struct xwc *c = data;
    if (strcmp(iface, "wl_compositor") == 0) {
        c->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, "wl_shm") == 0) {
        c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, "wl_seat") == 0) {
        c->seat = wl_registry_bind(r, name, &wl_seat_interface, 8);
        xwc_seat_init(c); /* in xwc-input part below */
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        c->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 5);
    } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
        c->layer_shell =
            wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 4);
    } else if (strcmp(iface, "zwlr_foreign_toplevel_manager_v1") == 0) {
        /* recorded, NOT bound: binding would immediately create new_id
         * proxies for every announcement (window handles) that clients
         * not using a tasklist would leak — found by LSan in the
         * exit-dialog child.  xwc_tasklist_create binds on demand. */
        c->ftm_global = name;
    } else if (strcmp(iface, "ext_workspace_manager_v1") == 0) {
        /* same: xwc_wspaces_create binds on demand (1 group + N
         * workspace proxies arrive as new_id events on bind) */
        c->wsm_global = name;
    } else if (strcmp(iface, "xw_workspace_info_v1") == 0) {
        /* same pattern: the tasklist binds on demand and annotates its
         * tasks with per-toplevel workspace events */
        c->wsi_global = name;
    } else if (strcmp(iface, "ext_session_lock_manager_v1") == 0) {
        c->lock_mgr =
            wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface,
                             1);
    } else if (strcmp(iface, "ext_idle_notifier_v1") == 0) {
        c->idle_notifier =
            wl_registry_bind(r, name, &ext_idle_notifier_v1_interface, 1);
    } else if (strcmp(iface, "wl_data_device_manager") == 0) {
        c->ddm =
            wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
    } else if (strcmp(iface, "wl_output") == 0 && c->n_outputs == 0) {
        /* first output only: size learned via the output events */
        xwc_output_init(c, r, name, version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *r,
                                   uint32_t name) {
    (void)r;
    struct xwc *c = data;
    if (c->output && name == c->output_global) {
        /* the bound output is gone: release the proxy and its event
         * state so a later output (hotplug) can be bound again; the
         * size stays as the last known value. (The state is NOT kept
         * alive here, unlike the same-output mode-change re-announce:
         * this global is gone for good.) */
        wl_output_destroy((struct wl_output *)c->output);
        c->output = NULL;
        c->output_global = 0;
        c->n_outputs = 0;
        free(c->output_state);
        c->output_state = NULL;
    }
    if (name == c->wsi_global)
        c->wsi_global = 0;
    if (name == c->ftm_global)
        c->ftm_global = 0;
    if (name == c->wsm_global)
        c->wsm_global = 0;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void sync_done(void *data, struct wl_callback *cb, uint32_t serial) {
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_done,
};

int xwc_sync(struct xwc *c) {
    bool done = false;
    struct wl_callback *cb = wl_display_sync(c->display);
    if (!cb)
        return -1;
    wl_callback_add_listener(cb, &sync_listener, &done);
    wl_display_flush(c->display);
    while (!done) {
        if (c->pump) {
            /* embedded (in-process) server: the pump only drives the
             * server; draining OUR side of the connection is ours to
             * do, otherwise the sync answer never gets processed */
            c->pump(c->pump_ud);
            if (xwc_drain(c) < 0)
                return -1;
        } else if (wl_display_dispatch(c->display) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Non-blocking drain: read whatever arrived and dispatch it. Returns 0,
 * or -1 on connection error. */
int xwc_drain(struct xwc *c) {
    /* send whatever requests we have queued first: poll() alone never
     * flushes, and a request stuck in the out buffer stalls the whole
     * handshake (bit us as a silently missing keymap event) */
    wl_display_flush(c->display);
    while (wl_display_prepare_read(c->display) != 0) {
        if (wl_display_dispatch_pending(c->display) < 0)
            return -1;
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(c->display), .events = POLLIN};
    poll(&pfd, 1, 0);
    if (pfd.revents & POLLIN)
        wl_display_read_events(c->display);
    else
        wl_display_cancel_read(c->display);
    if (wl_display_dispatch_pending(c->display) < 0)
        return -1;
    return 0;
}

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

int xwc_connect_pumped(struct xwc *c, const char *socket_name,
                       void (*pump)(void *ud), void *pump_ud) {
    memset(c, 0, sizeof(*c));
    c->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    c->display = wl_display_connect(socket_name);
    if (!c->display) {
        fprintf(stderr, "xwc: cannot connect to display %s\n",
                socket_name ? socket_name : "(default)");
        return -1;
    }
    c->pump = pump;
    c->pump_ud = pump_ud;
    c->registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    if (xwc_sync(c) < 0) {
        xwc_disconnect(c);
        return -1;
    }
    /* second roundtrip: the enumeration triggers binds (output,
     * seat), whose events follow in the next batch — without this,
     * output geometry/capabilities are only visible after the
     * caller's first dispatch, which surprised clients that read
     * the output size right after connect (the panel's layout
     * engine computed its height from a zero-size output) */
    if (xwc_sync(c) < 0) {
        xwc_disconnect(c);
        return -1;
    }
    xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
    if (!c->compositor || !c->shm || !c->seat || !c->wm_base) {
        fprintf(stderr, "xwc: missing globals (compositor=%p shm=%p seat=%p "
                        "wm_base=%p)\n",
                c->compositor, c->shm, c->seat, c->wm_base);
        xwc_disconnect(c);
        return -1;
    }
    c->running = true;
    return 0;
}

int xwc_connect(struct xwc *c, const char *socket_name) {
    return xwc_connect_pumped(c, socket_name, NULL, NULL);
}

void xwc_disconnect(struct xwc *c) {
    c->running = false;
    if (c->xkb_state)
        xkb_state_unref(c->xkb_state);
    if (c->xkb_keymap)
        xkb_keymap_unref(c->xkb_keymap);
    if (c->keymap_mmap && c->keymap_size)
        munmap(c->keymap_mmap, c->keymap_size);
    if (c->xkb_ctx)
        xkb_context_unref(c->xkb_ctx);
    /* destroy our proxies (windows/layers free their own) */
    if (c->keyboard)
        wl_keyboard_destroy(c->keyboard);
    if (c->pointer)
        wl_pointer_destroy(c->pointer);
    if (c->output)
        wl_output_destroy(c->output);
    free(c->output_state);
    if (c->ddm)
        wl_data_device_manager_destroy(c->ddm);
    if (c->layer_shell)
        zwlr_layer_shell_v1_destroy(c->layer_shell);
    /* ftm/wsm proxies are lazily bound and owned (and destroyed) by the
     * tasklist / workspace objects — normally NULL here; destroy
     * defensively if a caller ever stores one */
    if (c->ftm)
        zwlr_foreign_toplevel_manager_v1_destroy(c->ftm);
    if (c->wsm)
        ext_workspace_manager_v1_destroy(c->wsm);
    if (c->idle_notifier)
        ext_idle_notifier_v1_destroy(c->idle_notifier);
    if (c->lock_mgr)
        ext_session_lock_manager_v1_destroy(c->lock_mgr);
    if (c->wm_base)
        xdg_wm_base_destroy(c->wm_base);
    if (c->seat)
        wl_seat_destroy(c->seat);
    if (c->shm)
        wl_shm_destroy(c->shm);
    if (c->compositor)
        wl_compositor_destroy(c->compositor);
    if (c->registry)
        wl_registry_destroy(c->registry);
    if (c->display)
        wl_display_disconnect(c->display);
    memset(c, 0, sizeof(*c));
}

int xwc_dispatch(struct xwc *c, int timeout_ms) {
    if (c->pump) {
        /* embedded (in-process) server: it runs in this thread and
         * cannot make progress while we block — pump one cycle, drain
         * whatever arrived, then flush (wl_display_dispatch used to
         * do this implicitly) */
        c->pump(c->pump_ud);
        if (wl_display_dispatch_pending(c->display) < 0)
            return -1;
        if (wl_display_flush(c->display) < 0)
            return -1;
        return 0;
    }
    /* standalone client: flush requests queued by event handlers,
     * then wait up to timeout_ms for events so timer-driven redraws
     * (panel clocks) can fire; timeout_ms < 0 blocks.  NOTE:
     * wl_display_dispatch flushed implicitly; poll() does not. */
    if (wl_display_flush(c->display) < 0)
        return -1;
    while (wl_display_prepare_read(c->display) != 0) {
        if (wl_display_dispatch_pending(c->display) < 0)
            return -1;
    }
    struct pollfd pfd = {
        .fd = wl_display_get_fd(c->display), .events = POLLIN,
    };
    int n = poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
    if (n < 0) {
        wl_display_cancel_read(c->display);
        return errno == EINTR ? 0 : -1; /* signal: not fatal */
    }
    if (n > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
        if (wl_display_read_events(c->display) < 0)
            return -1; /* connection lost */
    } else {
        wl_display_cancel_read(c->display);
    }
    if (wl_display_dispatch_pending(c->display) < 0)
        return -1;
    return 0;
}

void xwc_flush(struct xwc *c) { wl_display_flush(c->display); }

/* ---------------------------------------------------------- shm pool */

static bool pool_create(struct xwc *c, void *owner, int w, int h,
                        struct wl_shm_pool **pool_out,
                        struct wl_buffer **bufs, uint32_t **data_out,
                        size_t *size_out) {
    size_t buf_size = (size_t)w * h * 4;
    size_t pool_size = buf_size * 2;
    int fd = memfd_create("xwc-pool", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, (off_t)pool_size) < 0) {
        close(fd);
        return false;
    }
    uint32_t *data = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)pool_size);
    close(fd);
    if (!pool) {
        munmap(data, pool_size);
        return false;
    }
    bufs[0] = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
                                        WL_SHM_FORMAT_XRGB8888);
    bufs[1] = wl_shm_pool_create_buffer(pool, (int32_t)buf_size, w, h, w * 4,
                                        WL_SHM_FORMAT_XRGB8888);
    *pool_out = pool;
    *data_out = data;
    *size_out = pool_size;
    (void)owner;
    return bufs[0] && bufs[1];
}

static void pool_destroy(struct wl_shm_pool *pool, struct wl_buffer **bufs,
                         uint32_t *data, size_t size) {
    if (bufs[0])
        wl_buffer_destroy(bufs[0]);
    if (bufs[1])
        wl_buffer_destroy(bufs[1]);
    bufs[0] = bufs[1] = NULL;
    if (pool)
        wl_shm_pool_destroy(pool);
    if (data && size)
        munmap(data, size);
}

/* buffer release: simply mark usable (single-threaded double buffer) */
static void buf_release(void *data, struct wl_buffer *b) {
    (void)data;
    (void)b;
}
static const struct wl_buffer_listener buf_listener = {
    .release = buf_release,
};

/* ------------------------------------------------------------- window */

/* Destroy a retired pool. Called only after the replacement buffer has
 * been committed (or the surface is gone): the server's wl_shm_buffer
 * dies with the wl_buffer resource, and the surface keeps using it
 * until a commit swaps it — destroying earlier leaves the compositor
 * reading freed memory on its next repaint. */
static void pool_retired_destroy(struct wl_shm_pool *pool,
                                 struct wl_buffer **bufs, uint32_t *data,
                                 size_t size) {
    if (bufs[0])
        wl_buffer_destroy(bufs[0]);
    if (bufs[1])
        wl_buffer_destroy(bufs[1]);
    bufs[0] = bufs[1] = NULL;
    if (pool)
        wl_shm_pool_destroy(pool);
    if (data && size)
        munmap(data, size);
}

/* move the current pool into the retirement slot (new buffers are not
 * created yet) */
static void pool_retire(struct wl_shm_pool **pool, struct wl_buffer **bufs,
                        uint32_t **data, size_t *size,
                        struct wl_shm_pool **old_pool,
                        struct wl_buffer **old_bufs, uint32_t **old_data,
                        size_t *old_size) {
    pool_retired_destroy(*old_pool, old_bufs, *old_data, *old_size);
    *old_pool = *pool;
    old_bufs[0] = bufs[0];
    old_bufs[1] = bufs[1];
    *old_data = *data;
    *old_size = *size;
    *pool = NULL;
    *data = NULL;
    bufs[0] = bufs[1] = NULL;
}

static void win_configure_apply(struct xwc_win *w) {
    int nw = w->conf_w, nh = w->conf_h;
    if (nw < 1)
        nw = w->w;
    if (nh < 1)
        nh = w->h;
    if (nw == w->w && nh == w->h && w->data)
        return;
    pool_retire(&w->pool, w->bufs, &w->data, &w->pool_size, &w->old_pool,
                w->old_bufs, &w->old_data, &w->old_pool_size);
    w->w = nw;
    w->h = nh;
    if (!pool_create(w->c, w, nw, nh, &w->pool, w->bufs, &w->data,
                     &w->pool_size)) {
        fprintf(stderr, "xwc: pool allocation failed (%dx%d)\n", nw, nh);
        return;
    }
    wl_buffer_add_listener(w->bufs[0], &buf_listener, w);
    wl_buffer_add_listener(w->bufs[1], &buf_listener, w);
}

/* destroy the retired pool after the replacement buffer was committed
 * (called right after the configure callback, which draws + commits) */
static void win_retired_release(struct xwc_win *w) {
    pool_retired_destroy(w->old_pool, w->old_bufs, w->old_data,
                         w->old_pool_size);
    w->old_pool = NULL;
    w->old_bufs[0] = w->old_bufs[1] = NULL;
    w->old_data = NULL;
    w->old_pool_size = 0;
}

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
                               int32_t h, struct wl_array *states) {
    (void)t;
    (void)states;
    struct xwc_win *win = data;
    win->conf_w = w;
    win->conf_h = h;
}

static void toplevel_close(void *data, struct xdg_toplevel *t) {
    (void)t;
    struct xwc_win *win = data;
    if (win->cb.close)
        win->cb.close(win, win->cb.ud);
    win->closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial) {
    struct xwc_win *win = data;
    xdg_surface_ack_configure(xs, serial);
    win->need_ack = false;
    win->ack_serial = serial;
    win_configure_apply(win);
    /* draw on xdg_surface.configure (after the pool is ready); the
     * toplevel.configure handler only records the size */
    if (win->cb.configure)
        win->cb.configure(win, win->conf_w, win->conf_h, win->cb.ud);
    /* the callback drew into the NEW buffer and committed it — the
     * retired pool can go now (see pool_retire). Only when the new
     * pool exists: on allocation failure nothing was committed and
     * the old buffer must stay alive. */
    if (win->data)
        win_retired_release(win);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

struct xwc_win *xwc_win_create(struct xwc *c, const struct xwc_callbacks *cb,
                              const char *title, const char *app_id, int w,
                              int h) {
    struct xwc_win *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;
    win->c = c;
    if (cb)
        win->cb = *cb;
    win->w = w;
    win->h = h;

    win->surface = wl_compositor_create_surface(c->compositor);
    xwc_register_surface(c, win, &win->cb, win->surface);
    win->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, win->surface);
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);
    win->toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
    xdg_toplevel_set_title(win->toplevel, title ? title : "xw");
    xdg_toplevel_set_app_id(win->toplevel, app_id ? app_id : "org.xfce.xw");
    xdg_toplevel_set_min_size(win->toplevel, 50, 50);
    /* initial commit with no buffer: triggers the configure cycle */
    wl_surface_commit(win->surface);
    xwc_sync(c);
    return win;
}

void xwc_win_destroy(struct xwc_win *w) {
    if (!w)
        return;
    xwc_unregister_surface(w->c, w->surface);
    /* role objects and surface first, pools after: requests are
     * processed in order, so the server stops rendering from the
     * attached buffer before the wl_buffer (and its server-side
     * wl_shm_buffer) is destroyed */
    if (w->toplevel)
        xdg_toplevel_destroy(w->toplevel);
    if (w->xdg_surface)
        xdg_surface_destroy(w->xdg_surface);
    if (w->surface)
        wl_surface_destroy(w->surface);
    pool_destroy(w->pool, w->bufs, w->data, w->pool_size);
    pool_retired_destroy(w->old_pool, w->old_bufs, w->old_data,
                         w->old_pool_size);
    if (w->c->focused_owner == w)
        w->c->focused_owner = NULL;
    free(w);
}

void xwc_win_size(struct xwc_win *w, int *w_out, int *h_out) {
    if (w_out) *w_out = w->w;
    if (h_out) *h_out = w->h;
}

uint32_t *xwc_win_pixels(struct xwc_win *w, int *stride) {
    if (!w->data)
        return NULL;
    if (stride)
        *stride = w->w;
    return w->data + (size_t)w->cur * w->w * w->h;
}

void xwc_win_commit(struct xwc_win *w) {
    if (!w->bufs[0])
        return;
    wl_surface_attach(w->surface, w->bufs[w->cur], 0, 0);
    wl_surface_damage(w->surface, 0, 0, w->w, w->h);
    wl_surface_commit(w->surface);
    w->cur ^= 1;
    w->mapped = true;
}

void xwc_win_set_title(struct xwc_win *w, const char *title) {
    if (w->toplevel)
        xdg_toplevel_set_title(w->toplevel, title);
}

void xwc_win_close(struct xwc_win *w) {
    if (w->toplevel)
        xdg_toplevel_destroy(w->toplevel), w->toplevel = NULL;
}

void xwc_win_maximize(struct xwc_win *w, bool on) {
    if (!w->toplevel)
        return;
    if (on)
        xdg_toplevel_set_maximized(w->toplevel);
    else
        xdg_toplevel_unset_maximized(w->toplevel);
}

void xwc_win_fullscreen(struct xwc_win *w, bool on) {
    if (!w->toplevel)
        return;
    if (on)
        xdg_toplevel_set_fullscreen(w->toplevel, NULL);
    else
        xdg_toplevel_unset_fullscreen(w->toplevel);
}

void xwc_win_minimize(struct xwc_win *w) {
    if (w->toplevel)
        xdg_toplevel_set_minimized(w->toplevel);
}

void *xwc_win_toplevel(struct xwc_win *w) { return w->toplevel; }
/* wl_surface and xdg_surface proxies (for xdg-activation targets and
 * xdg_popup parents) */
void *xwc_win_surface(struct xwc_win *w) { return w->surface; }
void *xwc_win_xdg_surface(struct xwc_win *w) { return w->xdg_surface; }

bool xwc_win_mapped(struct xwc_win *w) { return w->mapped; }

bool xwc_win_closed(struct xwc_win *w) { return w->closed; }

/* ------------------------------------------------------ layer surface */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
    struct xwc_layer *l = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    l->conf_pending = false;
    if ((int)w != l->w || (int)h != l->h || !l->data) {
        if ((int)w > 0 && (int)h > 0) {
            /* retire the old pool (destroyed only after the new buffer
             * is committed below — the compositor renders from the
             * attached wl_buffer until then; destroying it earlier
             * would free the server-side wl_shm_buffer under the
             * renderer) */
            pool_retire(&l->pool, l->bufs, &l->data, &l->pool_size,
                        &l->old_pool, l->old_bufs, &l->old_data,
                        &l->old_pool_size);
            l->w = w;
            l->h = h;
            if (pool_create(l->c, l, l->w, l->h, &l->pool, l->bufs, &l->data,
                            &l->pool_size)) {
                wl_buffer_add_listener(l->bufs[0], &buf_listener, l);
                wl_buffer_add_listener(l->bufs[1], &buf_listener, l);
            }
        }
    }
    if (l->cb.configure)
        l->cb.configure((struct xwc_win *)l, l->w, l->h, l->cb.ud);
    /* the configure callback drew + committed the new buffer: release
     * the retired pool. Only when the new pool exists — if allocation
     * failed nothing was committed and the surface still renders from
     * the old buffer, which must stay alive (released at destroy time
     * instead, after the surface itself is gone). */
    if (l->data && (l->old_pool || l->old_data)) {
        pool_retired_destroy(l->old_pool, l->old_bufs, l->old_data,
                             l->old_pool_size);
        l->old_pool = NULL;
        l->old_bufs[0] = l->old_bufs[1] = NULL;
        l->old_data = NULL;
        l->old_pool_size = 0;
    }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    struct xwc_layer *l = data;
    if (l->cb.close)
        l->cb.close((struct xwc_win *)l, l->cb.ud);
    l->mapped = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

struct xwc_layer *xwc_layer_create(struct xwc *c,
                                  const struct xwc_callbacks *cb,
                                  uint32_t layer, uint32_t anchors,
                                  int exclusive_zone, int w, int h) {
    /* a compositor without the layer-shell global (wrong
     * WAYLAND_DISPLAY, bare wl_compositor) must fail cleanly, not
     * dereference a NULL proxy */
    if (!c->layer_shell) {
        fprintf(stderr,
                "xwc: no zwlr_layer_shell_v1 global — this compositor has "
                "no layer-shell; cannot create a layer surface\n");
        return NULL;
    }
    struct xwc_layer *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->c = c;
    if (cb)
        l->cb = *cb;
    l->w = w;
    l->h = h;
    l->surface = wl_compositor_create_surface(c->compositor);
    xwc_register_surface(c, l, &l->cb, l->surface);
    l->ls = zwlr_layer_shell_v1_get_layer_surface(
        c->layer_shell, l->surface, NULL, layer, "xw");
    zwlr_layer_surface_v1_set_anchor(l->ls, anchors);
    zwlr_layer_surface_v1_set_exclusive_zone(l->ls, exclusive_zone);
    /* send set_size if either dimension is fixed; the anchored pair
     * (e.g. LEFT|RIGHT) lets the compositor dictate the other one */
    if (w > 0 || h > 0)
        zwlr_layer_surface_v1_set_size(l->ls, w, h);
    zwlr_layer_surface_v1_add_listener(l->ls, &layer_listener, l);
    wl_surface_commit(l->surface);
    xwc_sync(c);
    return l;
}

void xwc_layer_destroy(struct xwc_layer *l) {
    if (!l)
        return;
    xwc_unregister_surface(l->c, l->surface);
    /* surface before pools (see xwc_win_destroy) */
    if (l->ls)
        zwlr_layer_surface_v1_destroy(l->ls);
    if (l->surface)
        wl_surface_destroy(l->surface);
    pool_destroy(l->pool, l->bufs, l->data, l->pool_size);
    pool_retired_destroy(l->old_pool, l->old_bufs, l->old_data,
                         l->old_pool_size);
    free(l);
}

uint32_t *xwc_layer_pixels(struct xwc_layer *l, int *stride) {
    if (!l->data)
        return NULL;
    if (stride)
        *stride = l->w;
    return l->data + (size_t)l->cur * l->w * l->h;
}

void xwc_layer_commit(struct xwc_layer *l) {
    if (!l->bufs[0])
        return;
    wl_surface_attach(l->surface, l->bufs[l->cur], 0, 0);
    wl_surface_damage(l->surface, 0, 0, l->w, l->h);
    wl_surface_commit(l->surface);
    l->cur ^= 1;
    l->mapped = true;
}

void xwc_layer_resize(struct xwc_layer *l, int w, int h) {
    zwlr_layer_surface_v1_set_size(l->ls, w, h);
}

void xwc_layer_set_keyboard(struct xwc_layer *l, uint32_t mode) {
    if (l->ls)
        zwlr_layer_surface_v1_set_keyboard_interactivity(l->ls, mode);
}

void xwc_layer_set_layer(struct xwc_layer *l, uint32_t layer) {
    if (l->ls)
        zwlr_layer_surface_v1_set_layer(l->ls, layer);
}

/* ------------------------------------------------------------ popup */

struct xwc_popup {
    struct xwc *c;
    struct xwc_callbacks cb;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wl_shm_pool *pool;
    struct wl_buffer *bufs[2];
    int cur;
    uint32_t *data;
    size_t pool_size;
    int w, h;
    bool mapped;
    bool done; /* popup_done received (compositor dismissed us) */
};

static void popup_xdg_configure(void *data, struct xdg_surface *xs,
                                uint32_t serial) {
    struct xwc_popup *p = data;
    xdg_surface_ack_configure(xs, serial);
    /* the popup.configure event (same batch) already recorded the size;
     * hand the buffer to the owner, which draws + commits (mapping) */
    if (p->cb.configure)
        p->cb.configure((struct xwc_win *)p, p->w, p->h, p->cb.ud);
}

static const struct xdg_surface_listener popup_xdg_surface_listener = {
    .configure = popup_xdg_configure,
};

static void popup_cfg(void *data, struct xdg_popup *pp, int32_t x, int32_t y,
                      int32_t w, int32_t h) {
    (void)pp;
    (void)x;
    (void)y;
    struct xwc_popup *p = data;
    if (w > 0)
        p->w = w;
    if (h > 0)
        p->h = h;
}

static void popup_done_ev(void *data, struct xdg_popup *pp) {
    (void)pp;
    struct xwc_popup *p = data;
    p->done = true;
    /* the compositor dismissed the popup (press outside / explicit):
     * destroying the proxy inside its own event handler is the
     * documented-safe pattern (see the tasklist's handle_closed) */
    if (p->cb.close)
        p->cb.close((struct xwc_win *)p, p->cb.ud);
}

static void popup_repositioned(void *data, struct xdg_popup *pp,
                               uint32_t token) {
    (void)data;
    (void)pp;
    (void)token;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_cfg,
    .popup_done = popup_done_ev,
    .repositioned = popup_repositioned,
};

struct xwc_popup *xwc_popup_create(struct xwc *c, struct xwc_layer *parent,
                                   int anchor_x, int anchor_y,
                                   int anchor_w, int anchor_h, int w, int h,
                                   const struct xwc_callbacks *cb) {
    return xwc_popup_create_dir(c, parent, anchor_x, anchor_y, anchor_w,
                                anchor_h, w, h, cb, false);
}

struct xwc_popup *xwc_popup_create_dir(struct xwc *c, struct xwc_layer *parent,
                                       int anchor_x, int anchor_y,
                                       int anchor_w, int anchor_h, int w,
                                       int h, const struct xwc_callbacks *cb,
                                       bool upward) {
    if (!c || !c->wm_base || !parent || w < 1 || h < 1)
        return NULL;

    /* positioner: the popup's top-left lands on the anchor rect's
     * bottom-left corner (menu under a panel button); flip up and/or
     * slide if the screen edge is in the way. Created LAST, right
     * before the request that consumes it, so no failure path in
     * between can leak it. */
    struct xwc_popup *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->c = c;
    if (cb)
        p->cb = *cb;
    p->w = w;
    p->h = h;

    p->surface = wl_compositor_create_surface(c->compositor);
    if (!p->surface)
        goto fail;
    xwc_register_surface(c, p, &p->cb, p->surface);

    p->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, p->surface);
    if (!p->xdg_surface)
        goto fail;
    xdg_surface_add_listener(p->xdg_surface, &popup_xdg_surface_listener, p);

    struct xdg_positioner *pos = xdg_wm_base_create_positioner(c->wm_base);
    if (!pos)
        goto fail;
    xdg_positioner_set_size(pos, w, h);
    xdg_positioner_set_anchor_rect(pos, anchor_x, anchor_y, anchor_w,
                                   anchor_h);
    if (upward) {
        /* bottom bars: the popup grows up from the anchor rect's top
         * edge, still left-aligned with the button */
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP_RIGHT);
    } else {
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    }
    xdg_positioner_set_constraint_adjustment(
        pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);

    /* parent = the layer surface (NULL here): the linkage goes through
     * zwlr_layer_surface.get_popup, the layer-shell way to parent a
     * menu to a panel. The positioner is consumed by this request
     * (the server copies it) and destroyed right after. */
    p->popup = xdg_surface_get_popup(p->xdg_surface, NULL, pos);
    xdg_positioner_destroy(pos);
    if (!p->popup)
        goto fail;
    zwlr_layer_surface_v1_get_popup(parent->ls, p->popup);
    xdg_popup_add_listener(p->popup, &popup_listener, p);

    /* deliberately NO round trip here: the configure (and the draw +
     * commit + grab the configure callback performs) arrives through
     * the caller's normal dispatch. Blocking in a sync from inside an
     * event handler (the panel opens the menu from its button
     * handler) wedges the client on signals: libwayland's dispatch
     * loop retries EINTR internally, so a SIGTERM arriving mid-sync
     * is only noticed after the next server event. Fire-and-forget
     * keeps every client callback non-blocking. */
    return p;

fail:
    xwc_popup_destroy(p);
    return NULL;
}

void xwc_popup_destroy(struct xwc_popup *p) {
    if (!p)
        return;
    if (p->popup)
        xdg_popup_destroy(p->popup);
    if (p->xdg_surface)
        xdg_surface_destroy(p->xdg_surface);
    xwc_unregister_surface(p->c, p->surface);
    if (p->surface)
        wl_surface_destroy(p->surface);
    pool_destroy(p->pool, p->bufs, p->data, p->pool_size);
    free(p);
}

uint32_t *xwc_popup_pixels(struct xwc_popup *p, int *stride) {
    if (!p)
        return NULL;
    /* allocate the pool on first use (the configure callback's first
     * draw); menus do not resize while open, so one pool serves the
     * popup's lifetime */
    if (!p->bufs[0]) {
        if (!pool_create(p->c, p, p->w, p->h, &p->pool, p->bufs, &p->data,
                         &p->pool_size))
            return NULL;
        wl_buffer_add_listener(p->bufs[0], &buf_listener, p);
        wl_buffer_add_listener(p->bufs[1], &buf_listener, p);
    }
    if (stride)
        *stride = p->w;
    return p->data + (size_t)p->cur * p->w * p->h;
}

void xwc_popup_commit(struct xwc_popup *p) {
    if (!p || !p->bufs[0])
        return;
    wl_surface_attach(p->surface, p->bufs[p->cur], 0, 0);
    wl_surface_damage(p->surface, 0, 0, p->w, p->h);
    wl_surface_commit(p->surface);
    p->cur ^= 1;
    p->mapped = true;
}

void xwc_popup_grab(struct xwc_popup *p) {
    if (!p || !p->popup || !p->c->seat)
        return;
    xdg_popup_grab(p->popup, p->c->seat, p->c->last_serial);
}

bool xwc_popup_done(struct xwc_popup *p) { return p && p->done; }


/* ------------------------------------------------------ input routing */

void xwc_input_key(struct xwc *c, uint32_t keycode, bool down,
                   xkb_keysym_t sym, uint32_t mods) {
    if (c->has_focus && c->focused_cb.key)
        c->focused_cb.key(c->focused_owner, keycode, down, sym, mods,
                          c->focused_cb.ud);
}

void xwc_input_button(struct xwc *c, uint32_t button, bool down, int x, int y) {
    if (c->has_focus && c->focused_cb.button)
        c->focused_cb.button(c->focused_owner, button, down, x, y,
                             c->focused_cb.ud);
}

void xwc_input_motion(struct xwc *c, int x, int y) {
    c->ptr_x = x;
    c->ptr_y = y;
    if (c->has_focus && c->focused_cb.motion)
        c->focused_cb.motion(c->focused_owner, x, y, c->focused_cb.ud);
}

void xwc_input_axis(struct xwc *c, uint32_t axis, double value) {
    if (c->has_focus && c->focused_cb.axis)
        c->focused_cb.axis(c->focused_owner, axis, value,
                           c->focused_cb.ud);
}

/* --------------------------------------- pool helpers shared with xwc-lock.c */
/* Thin non-static thunks over the static pool machinery so the
 * session-lock surface (xwc-lock.c) can reuse the exact double-buffer
 * + retire-on-configure lifecycle the windows and layers use. */

bool xwc_pool_create(struct xwc *c, int w, int h,
                     struct wl_shm_pool **pool_out, struct wl_buffer **bufs,
                     uint32_t **data_out, size_t *size_out) {
    return pool_create(c, NULL, w, h, pool_out, bufs, data_out, size_out);
}

void xwc_pool_destroy(struct wl_shm_pool *pool, struct wl_buffer **bufs,
                      uint32_t *data, size_t size) {
    pool_destroy(pool, bufs, data, size);
}

void xwc_pool_retired_destroy(struct wl_shm_pool *pool,
                              struct wl_buffer **bufs, uint32_t *data,
                              size_t size) {
    pool_retired_destroy(pool, bufs, data, size);
}

void xwc_pool_retire(struct wl_shm_pool **pool, struct wl_buffer **bufs,
                     uint32_t **data, size_t *size,
                     struct wl_shm_pool **old_pool, struct wl_buffer **old_bufs,
                     uint32_t **old_data, size_t *old_size) {
    pool_retire(pool, bufs, data, size, old_pool, old_bufs, old_data, old_size);
}

const struct wl_buffer_listener *xwc_buf_listener(void) {
    return &buf_listener;
}
