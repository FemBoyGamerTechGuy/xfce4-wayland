/* test_input.c — the input/focus/cursor/window-state round (the
 * physical-NVIDIA round-2 findings: unreliable buttons, right-click
 * killing Wayland clients, stuck cursors, dead taskbar activation).
 *
 * Every test here drives the REAL protocol from a raw libwayland
 * client (second connection) with full event recording:
 *
 *   input-event-matrix         the priority-1 matrix: enter/leave/
 *                              motion/button(L/M/R)/axis, serials,
 *                              surface-local coordinates — a client
 *                              must see every event, correctly
 *                              translated, and stay alive
 *   input-hit-test-order       pointer focus must follow the RENDER
 *                              order (popups > overlay > top > OR >
 *                              windows > bottom > background): a
 *                              full-screen background must not eat
 *                              window clicks
 *   input-cursor-state         requested vs applied vs displayed:
 *                              set_cursor adoption, reset-to-default
 *                              on focus change (the stuck cursor),
 *                              serial validation, roled-surface
 *                              rejection, cursor-surface destroy
 *   input-right-click-menu     the exact context-menu flow: press ->
 *                              popup + grab(serial) -> enter ->
 *                              item click -> outside press dismissal
 *                              -> parent refocus -> destroy
 *   input-popup-destroy-grab   client destroys the popup (and its
 *                              surface) while the grab is active
 *   input-taskbar-activate     taskbar activation semantics:
 *                              minimize -> activate (unset_minimized
 *                              + activate) -> focus/raise/restore;
 *                              cross-workspace activation
 *   input-real-gtk-clicks      a REAL GTK4 process (zenity) against
 *                              the compositor: motion + L/M/R clicks
 *                              must never kill the toolkit
 */
#include "xwtest.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xdg-shell.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"

#define BTN_L 0x110
#define BTN_R 0x111
#define BTN_M 0x112

/* ------------------------------------------------------------ raw client */

/* a small shm buffer (XRGB8888) on the raw client */
struct rc_buf {
    struct wl_shm_pool *pool;
    struct wl_buffer *buf;
    int w, h;
};

/* a second, fully instrumented connection: its wl_pointer listener
 * records EVERY event (serial, button, state, coords, surface) so a
 * test can assert exactly what the client saw — the truth the
 * physical round demanded, not just server-side state. */
struct pev {
    char kind; /* 'e' enter, 'l' leave, 'm' motion, 'b' button, 'a' axis */
    uint32_t serial;
    uint32_t time;
    uint32_t button;
    uint32_t state;
    int sx, sy;
    bool on_popup; /* enter/button landed on the popup surface */
    bool on_self;  /* ... or on this client's toplevel surface */
};

struct rawc {
    struct wl_display *d;
    struct wl_registry *reg;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *ptr;
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *tl;
    struct wl_surface *cursor_surf; /* roleless cursor image */
    struct xdg_wm_base *wm_base;

    struct pev ev[256];
    int n_ev;
    bool dead;
    char death[160];

    /* toplevel configure state */
    bool configured;
    uint32_t config_serial;
    bool state_activated;
    int cfg_w, cfg_h;

    /* popup machinery (created on demand) */
    struct wl_surface *pop_surf;
    struct xdg_surface *pop_xs;
    struct xdg_popup *pop;
    struct rc_buf pop_buf; /* held until the menu closes (real clients
                            * keep the buffer; destroying it mid-life
                            * clears the content server-side) */
    bool pop_configured, pop_done;
    int pop_done_count; /* popup_done is once-per-lifetime (xdg-shell) */
    int pop_cfg_x, pop_cfg_y; /* configure position (parent-buffer-relative) */
};

static void rc_die(struct rawc *rc) {
    if (rc->dead)
        return;
    rc->dead = true;
    const struct wl_interface *iface = NULL;
    uint32_t id = 0;
    uint32_t perr = wl_display_get_protocol_error(rc->d, &iface, &id);
    int err = wl_display_get_error(rc->d);
    if (perr || iface)
        snprintf(rc->death, sizeof(rc->death),
                 "protocol error %u on %s object %u (errno %d)", perr,
                 iface ? iface->name : "?", id, err);
    else
        snprintf(rc->death, sizeof(rc->death), "connection error (errno %d)",
                 err);
}

static void rc_record(struct rawc *rc, char kind, uint32_t serial,
                      uint32_t button, uint32_t state, int sx, int sy,
                      struct wl_surface *on) {
    if (rc->n_ev >= 256)
        return;
    struct pev *e = &rc->ev[rc->n_ev++];
    e->kind = kind;
    e->serial = serial;
    e->button = button;
    e->state = state;
    e->sx = sx;
    e->sy = sy;
    e->on_self = on && on == rc->surf;
    e->on_popup = on && on == rc->pop_surf;
}

/* ---------------- protocol listeners (raw, all-state) ---------------- */

static void rc_ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                         struct wl_surface *surf, wl_fixed_t sx,
                         wl_fixed_t sy) {
    struct rawc *rc = data;
    rc_record(rc, 'e', serial, 0, 0, wl_fixed_to_int(sx), wl_fixed_to_int(sy),
              surf);
    /* protocol-correct client: adopt a cursor on enter (what every
     * toolkit does — the response our cursor machine must handle) */
    if (rc->cursor_surf)
        wl_pointer_set_cursor(p, serial, rc->cursor_surf, 3, 2);
}
static void rc_ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                         struct wl_surface *surf) {
    (void)p;
    struct rawc *rc = data;
    rc_record(rc, 'l', serial, 0, 0, 0, 0, surf);
}
static void rc_ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void)p;
    (void)time;
    struct rawc *rc = data;
    rc_record(rc, 'm', 0, 0, 0, wl_fixed_to_int(sx), wl_fixed_to_int(sy),
              NULL);
}
static void rc_ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                          uint32_t time, uint32_t button, uint32_t state) {
    (void)p;
    (void)time;
    struct rawc *rc = data;
    rc_record(rc, 'b', serial, button, state, 0, 0, NULL);
}
static void rc_ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                        uint32_t axis, wl_fixed_t value) {
    (void)p;
    (void)time;
    struct rawc *rc = data;
    rc_record(rc, 'a', 0, 0, axis, wl_fixed_to_int(value), 0, NULL);
}
static void rc_ptr_frame(void *data, struct wl_pointer *p) {
    (void)data;
    (void)p;
}
static const struct wl_pointer_listener rc_ptr_listener = {
    .enter = rc_ptr_enter,
    .leave = rc_ptr_leave,
    .motion = rc_ptr_motion,
    .button = rc_ptr_button,
    .axis = rc_ptr_axis,
    .frame = rc_ptr_frame,
};

static void rc_seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    struct rawc *rc = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !rc->ptr) {
        rc->ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(rc->ptr, &rc_ptr_listener, rc);
    }
}
static void rc_seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}
static const struct wl_seat_listener rc_seat_listener = {
    .capabilities = rc_seat_caps,
    .name = rc_seat_name,
};

static void rc_tl_configure(void *data, struct xdg_toplevel *tl, int32_t w,
                            int32_t h, struct wl_array *states) {
    (void)tl;
    struct rawc *rc = data;
    rc->cfg_w = w;
    rc->cfg_h = h;
    rc->state_activated = false;
    uint32_t *st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_ACTIVATED)
            rc->state_activated = true;
    }
}
static void rc_tl_close(void *data, struct xdg_toplevel *tl) {
    (void)data;
    (void)tl;
}
static const struct xdg_toplevel_listener rc_tl_listener = {
    .configure = rc_tl_configure,
    .close = rc_tl_close,
};

static void rc_xs_configure(void *data, struct xdg_surface *xs,
                            uint32_t serial) {
    struct rawc *rc = data;
    rc->configured = true;
    rc->config_serial = serial;
    xdg_surface_ack_configure(xs, serial);
}
static const struct xdg_surface_listener rc_xs_listener = {
    .configure = rc_xs_configure,
};

static void rc_popup_configure(void *data, struct xdg_popup *pop, int32_t x,
                               int32_t y, int32_t w, int32_t h) {
    (void)pop;
    (void)w;
    (void)h;
    struct rawc *rc = data;
    rc->pop_configured = true;
    rc->pop_cfg_x = x; /* popup position relative to the parent
                        * SURFACE (buffer) origin — the client composes
                        * its menu exactly here */
    rc->pop_cfg_y = y;
}
static void rc_popup_done(void *data, struct xdg_popup *pop) {
    (void)pop;
    struct rawc *rc = data;
    rc->pop_done = true;
    rc->pop_done_count++;
}
static const struct xdg_popup_listener rc_pop_listener = {
    .configure = rc_popup_configure,
    .popup_done = rc_popup_done,
};

static void rc_wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(b, serial);
}

static void rc_global(void *data, struct wl_registry *r, uint32_t name,
                      const char *iface, uint32_t version) {
    (void)version;
    struct rawc *rc = data;
    if (strcmp(iface, "wl_compositor") == 0)
        rc->compositor = wl_registry_bind(r, name, &wl_compositor_interface,
                                          4);
    else if (strcmp(iface, "wl_shm") == 0)
        rc->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(iface, "wl_seat") == 0)
        rc->seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
    else if (strcmp(iface, "xdg_wm_base") == 0)
        rc->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 5);
}
static void rc_global_remove(void *data, struct wl_registry *r,
                             uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener rc_reg_listener = {
    .global = rc_global,
    .global_remove = rc_global_remove,
};

static const struct xdg_wm_base_listener rc_wm_base_listener = {
    .ping = rc_wm_ping,
};

static void rc_pump(struct xwt_ctx *t, struct rawc *rc);
static bool rc_buf_create(struct rawc *rc, struct rc_buf *b, int w, int h);
static void rc_buf_destroy(struct rc_buf *b);

/* connect + bind everything needed for a toplevel. NEVER a blocking
 * roundtrip: the server is in-process and only runs in our pump. */
static bool rc_connect(struct rawc *rc, struct xwt_ctx *t) {
    memset(rc, 0, sizeof(*rc));
    rc->d = wl_display_connect(t->socket_name);
    if (!rc->d)
        return false;
    rc->reg = wl_display_get_registry(rc->d);
    wl_registry_add_listener(rc->reg, &rc_reg_listener, rc);
    wl_display_flush(rc->d);
    bool bound = false;
    for (int i = 0; i < 500 && !bound; i++) {
        rc_pump(t, rc);
        xwt_pump(t);
        bound = rc->compositor && rc->shm && rc->seat && rc->wm_base;
    }
    if (!bound)
        return false;
    xdg_wm_base_add_listener(rc->wm_base, &rc_wm_base_listener, rc);
    wl_seat_add_listener(rc->seat, &rc_seat_listener, rc);
    wl_display_flush(rc->d);
    for (int i = 0; i < 500 && !rc->ptr; i++) {
        rc_pump(t, rc);
        xwt_pump(t);
    }
    return rc->ptr != NULL;
}

static void rc_destroy(struct rawc *rc) {
    /* full teardown, child-most first: every proxy the raw client
     * created is destroyed so LeakSanitizer sees a clean connection
     * (a leaked wl_proxy keeps its queue and fd bookkeeping alive) */
    rc_buf_destroy(&rc->pop_buf);
    if (rc->pop)
        xdg_popup_destroy(rc->pop);
    if (rc->pop_xs)
        xdg_surface_destroy(rc->pop_xs);
    if (rc->pop_surf)
        wl_surface_destroy(rc->pop_surf);
    if (rc->tl)
        xdg_toplevel_destroy(rc->tl);
    if (rc->xs)
        xdg_surface_destroy(rc->xs);
    if (rc->cursor_surf)
        wl_surface_destroy(rc->cursor_surf);
    if (rc->surf)
        wl_surface_destroy(rc->surf);
    if (rc->ptr)
        wl_pointer_destroy(rc->ptr);
    if (rc->seat)
        wl_seat_destroy(rc->seat);
    if (rc->wm_base)
        xdg_wm_base_destroy(rc->wm_base);
    if (rc->shm)
        wl_shm_destroy(rc->shm);
    if (rc->compositor)
        wl_compositor_destroy(rc->compositor);
    if (rc->reg)
        wl_registry_destroy(rc->reg);
    if (rc->d)
        wl_display_disconnect(rc->d);
    memset(rc, 0, sizeof(*rc));
}

/* one non-blocking dispatch cycle of the raw client against the
 * embedded server (mirrors xwt_pump but for this connection) */
static bool rc_buf_create(struct rawc *rc, struct rc_buf *b, int w, int h) {
    int stride = w * 4;
    size_t sz = (size_t)stride * h;
    int fd = memfd_create("xwt-rc", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)sz) < 0) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    b->pool = wl_shm_create_pool(rc->shm, fd, (int32_t)sz);
    close(fd);
    if (!b->pool)
        return false;
    b->buf = wl_shm_pool_create_buffer(b->pool, 0, w, h, stride,
                                       WL_SHM_FORMAT_XRGB8888);
    b->w = w;
    b->h = h;
    return b->buf != NULL;
}

static void rc_buf_destroy(struct rc_buf *b) {
    if (b->buf)
        wl_buffer_destroy(b->buf);
    if (b->pool)
        wl_shm_pool_destroy(b->pool);
    memset(b, 0, sizeof(*b));
}

static void rc_pump(struct xwt_ctx *t, struct rawc *rc) {
    xw_compositor_dispatch(t->comp, 0);
    if (rc->dead || !rc->d)
        return;
    wl_display_flush(rc->d);
    while (wl_display_prepare_read(rc->d) != 0) {
        if (wl_display_dispatch_pending(rc->d) < 0) {
            rc_die(rc);
            return;
        }
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(rc->d), .events = POLLIN};
    poll(&pfd, 1, 0);
    if (pfd.revents & POLLIN) {
        if (wl_display_read_events(rc->d) < 0) {
            rc_die(rc);
            return;
        }
    } else {
        wl_display_cancel_read(rc->d);
    }
    if (wl_display_dispatch_pending(rc->d) < 0)
        rc_die(rc);
    xw_compositor_dispatch(t->comp, 0);
}

/* wait for a condition with real time passing (the condition is
 * RE-EVALUATED every iteration — a bool parameter would freeze the
 * state at call time, which is exactly how the first version of this
 * helper managed to ignore events it had just received) */
#define RC_WAIT(t, rc, cond, ms)                                                \
    ({                                                                          \
        bool _ok = false;                                                        \
        for (int _i = 0; _i < (ms) / 2 && !(_ok = (cond)); _i++) {              \
            rc_pump((t), (rc));                                                 \
            xwt_pump((t));                                                      \
            usleep(2000);                                                        \
        }                                                                        \
        XWT_CHECK(_ok || (rc)->dead, "timeout waiting for: %s", #cond);         \
        _ok;                                                                     \
    })



/* map a window: surface + xdg toplevel + first buffer commit */
static bool rc_map_window(struct xwt_ctx *t, struct rawc *rc, int w, int h) {
    rc->surf = wl_compositor_create_surface(rc->compositor);
    rc->xs = xdg_wm_base_get_xdg_surface(rc->wm_base, rc->surf);
    xdg_surface_add_listener(rc->xs, &rc_xs_listener, rc);
    rc->tl = xdg_surface_get_toplevel(rc->xs);
    xdg_toplevel_add_listener(rc->tl, &rc_tl_listener, rc);
    xdg_toplevel_set_title(rc->tl, "rawc");
    wl_surface_commit(rc->surf);
    if (!RC_WAIT(t, rc, rc->configured, 3000))
        return false;

    struct rc_buf b;
    if (!rc_buf_create(rc, &b, w, h))
        return false;
    wl_surface_attach(rc->surf, b.buf, 0, 0);
    wl_surface_damage(rc->surf, 0, 0, w, h);
    wl_surface_commit(rc->surf);
    rc_buf_destroy(&b);

    struct xw_window *win = NULL;
    bool mapped = false;
    for (int i = 0; i < 1500 && !mapped; i++) {
        rc_pump(t, rc);
        xwt_pump(t);
        win = NULL;
        wl_list_for_each(win, &t->comp->wm->windows, link) {
            if (strcmp(win->title, "rawc") == 0)
                break;
            win = NULL;
        }
        mapped = win && win->mapped;
        if (!mapped)
            usleep(2000);
    }
    return mapped;
}

/* --------------------------------------------------- event lookup helpers */

static int rc_count(struct rawc *rc, char kind) {
    int n = 0;
    for (int i = 0; i < rc->n_ev; i++)
        if (rc->ev[i].kind == kind)
            n++;
    return n;
}

static struct pev *rc_last(struct rawc *rc, char kind) {
    for (int i = rc->n_ev - 1; i >= 0; i--)
        if (rc->ev[i].kind == kind)
            return &rc->ev[i];
    return NULL;
}

static struct pev *rc_find_button(struct rawc *rc, uint32_t button,
                                  uint32_t state) {
    for (int i = 0; i < rc->n_ev; i++)
        if (rc->ev[i].kind == 'b' && rc->ev[i].button == button &&
            rc->ev[i].state == state)
            return &rc->ev[i];
    return NULL;
}

/* assert the client connection is still healthy; print the exact
 * protocol error if it is not (the priority-1 requirement: a right
 * click must never kill a client, and if it does the test says WHY) */
#define RC_ALIVE(rc)                                                           \
    do {                                                                        \
        if ((rc)->dead) {                                                       \
            XWT_CHECK(false, "client died: %s", (rc)->death);                   \
            return;                                                             \
        }                                                                       \
    } while (0)

static struct xw_window *find_by_title(struct xwt_ctx *t, const char *title) {
    struct xw_window *w;
    wl_list_for_each(w, &t->comp->wm->windows, link) {
        if (strcmp(w->title, title) == 0)
            return w;
    }
    return NULL;
}

/* background layer fill for the hit-test-order test */
static void input_bg_layer_configure(struct xwc_win *win, int w, int h,
                                     void *ud) {
    (void)ud;
    struct xwc_layer *l = (struct xwc_layer *)win;
    int stride = 0;
    uint32_t *pix = xwc_layer_pixels(l, &stride);
    if (!pix || w < 1 || h < 1)
        return;
    xwc_fill_rect(pix, stride, w, h, 0, 0, w, h, 0xff1a3a2a);
    xwc_layer_commit(l);
}

/* is any surface mapped in the given layer? */
static bool layer_in_list(struct xwt_ctx *t, int layer) {
    struct xw_layer_surface *ls;
    wl_list_for_each(ls, &t->comp->wm->layers[layer], link) {
        if (ls->mapped)
            return true;
    }
    return false;
}

/* ===================================================== 1: event matrix */

/* THE priority-1 test: every pointer event type, correctly
 * translated to surface-local coordinates, with valid serials, to a
 * real client. Any compositor protocol violation on this path is the
 * "most clicks do not register" bug made measurable. */
static void test_input_event_matrix(struct xwt_ctx *t) {
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 200, 150));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);

    int cx = w->x + w->w / 2, cy = w->y + w->h / 2;

    /* --- enter + motion: surface-local translation */
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);
    RC_ALIVE(&rc);
    struct pev *e = rc_last(&rc, 'e');
    XWT_CHECK(e && e->on_self, "enter delivered on the window surface");
    XWT_CHECK(e && e->sx == cx - w->x && e->sy == cy - w->y,
              "enter surface-local coords %d,%d (want %d,%d)",
              e ? e->sx : -1, e ? e->sy : -1, cx - w->x, cy - w->y);
    XWT_CHECK(e && e->serial > 0, "enter carries serial %u",
              e ? e->serial : 0);

    int n_motion = rc_count(&rc, 'm');
    xw_compositor_inject_pointer_motion(t->comp, cx + 10, cy + 5);
    RC_WAIT(t, &rc, rc_count(&rc, 'm') > n_motion, 1000);
    RC_ALIVE(&rc);
    struct pev *m = rc_last(&rc, 'm');
    XWT_CHECK(m && m->sx == 110 && m->sy == 80,
              "motion surface-local coords %d,%d (want 110,80)",
              m ? m->sx : -99, m ? m->sy : -99);

    /* --- the full button matrix: left, middle, right; press+release */
    const struct {
        uint32_t btn;
        const char *name;
    } buttons[] = {
        {BTN_L, "left"},
        {BTN_R, "right"},
        {BTN_M, "middle"},
    };
    uint32_t last_serial = e ? e->serial : 0;
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        xw_compositor_inject_pointer_button(t->comp, buttons[i].btn, true);
        RC_WAIT(t, &rc,
                rc_find_button(&rc, buttons[i].btn,
                               WL_POINTER_BUTTON_STATE_PRESSED) != NULL,
                1000);
        RC_ALIVE(&rc);
        xw_compositor_inject_pointer_button(t->comp, buttons[i].btn, false);
        RC_WAIT(t, &rc,
                rc_find_button(&rc, buttons[i].btn,
                               WL_POINTER_BUTTON_STATE_RELEASED) != NULL,
                1000);
        RC_ALIVE(&rc);
        struct pev *press = rc_find_button(&rc, buttons[i].btn,
                                           WL_POINTER_BUTTON_STATE_PRESSED);
        struct pev *rel = rc_find_button(&rc, buttons[i].btn,
                                         WL_POINTER_BUTTON_STATE_RELEASED);
        XWT_CHECK(press, "%s press delivered to the client", buttons[i].name);
        XWT_CHECK(rel, "%s release delivered to the client",
                  buttons[i].name);
        if (press)
            XWT_CHECK(press->serial > last_serial,
                      "%s press serial %u > previous %u", buttons[i].name,
                      press->serial, last_serial);
        if (press && rel)
            XWT_CHECK(rel->serial > press->serial,
                      "%s release serial %u > press %u", buttons[i].name,
                      rel->serial, press->serial);
        if (rel)
            last_serial = rel->serial;
    }

    /* --- axis (scroll) */
    int n_axis0 = rc_count(&rc, 'a');
    xw_compositor_inject_pointer_axis(t->comp, 0, 10.0);
    RC_WAIT(t, &rc, rc_count(&rc, 'a') > n_axis0, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(rc_count(&rc, 'a') > n_axis0, "axis event delivered");

    /* --- leave on exit */
    xw_compositor_inject_pointer_motion(t->comp, 5, 5); /* outside any window */
    RC_WAIT(t, &rc, rc_count(&rc, 'l') >= 1, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(rc_count(&rc, 'l') >= 1, "leave delivered on exit");

    /* --- re-enter: enter/leave must alternate (no double enter) */
    int enters = rc_count(&rc, 'e'), leaves = rc_count(&rc, 'l');
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') > enters, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(rc_count(&rc, 'e') == enters + 1 && rc_count(&rc, 'l') == leaves,
              "re-enter delivered exactly once (e=%d l=%d)",
              rc_count(&rc, 'e'), rc_count(&rc, 'l'));

    /* server-side truth: focus is the rawc window */
    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_CHECK(seat && seat->ptr_focus == w->surface,
              "server pointer focus is the window");

    rc_destroy(&rc);
}

/* ===================================================== 2: hit-test order */

/* pointer focus must follow the RENDER order. The renderer paints:
 * background(0) < bottom(1) < windows < OR windows < top(2) <
 * overlay(3) < popups. A full-screen background layer must NOT steal
 * clicks from windows rendered above it. */
static void test_input_hit_test_order(struct xwt_ctx *t) {
    /* full-screen background layer via the xwc client library */
    struct xwc_callbacks lcb = {0};
    lcb.configure = input_bg_layer_configure;
    struct xwc_layer *bg = xwc_layer_create(
        &t->client, &lcb, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        -1, 1280, 720);
    XWT_ASSERT(bg);
    XWT_WAIT(t, layer_in_list(t, 0));

    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 200, 150));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);

    struct xw_seat *seat = xw_seat_first(t->comp);

    /* pointer over the WINDOW: focus must be the window, not the
     * full-screen background beneath it (a point strictly inside BOTH
     * rects: the compositor must pick the window, which renders ABOVE
     * the background) */
    xw_compositor_inject_pointer_motion(t->comp, w->x + w->w / 2,
                                        w->y + 20);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->ptr_focus == w->surface,
              "focus over the window IS the window (got %p, window %p)",
              (void *)seat->ptr_focus, (void *)w->surface);

    /* pointer over background-only area: the background takes it */
    xw_compositor_inject_pointer_motion(t->comp, 20, 300);
    RC_WAIT(t, &rc, rc_count(&rc, 'l') >= 1, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->ptr_focus && seat->ptr_focus->role == XW_SURFACE_ROLE_LAYER,
              "focus over empty desktop IS the background layer");

    rc_destroy(&rc);
    xwc_layer_destroy(bg);
    xwt_pump(t);
}

/* ===================================================== 3: cursor state */

/* requested vs applied vs displayed. The stuck-cursor physical bug:
 * the old client's cursor image survives focus changes because
 * nothing resets the cursor when the pointer leaves. On every focus
 * change the compositor must revert to the DEFAULT cursor until the
 * newly focused client sets one (spec: the cursor is per-enter). */
static void test_input_cursor_state(struct xwt_ctx *t) {
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 200, 150));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    struct xw_seat *seat = xw_seat_first(t->comp);

    /* a second window from the harness client (a DIFFERENT client) */
    struct xwc_win *other = xwt_window_solid(t, 0xff404040, 180, 120, "curB");
    XWT_ASSERT(other);
    struct xw_window *wb = find_by_title(t, "curB");
    XWT_ASSERT(wb);

    /* a point inside rawc but OUTSIDE the cascade-offset curB window
     * (curB covers rawc's center: the later window stacks on top) */
    int cx = w->x + 5, cy = w->y + 5;

    /* the client's cursor surface exists BEFORE the enter (toolkits
     * keep one ready); rc_ptr_enter responds to enter with set_cursor
     * carrying the enter serial */
    rc.cursor_surf = wl_compositor_create_surface(rc.compositor);
    struct rc_buf cimg;
    XWT_ASSERT(rc_buf_create(&rc, &cimg, 24, 24));
    wl_surface_attach(rc.cursor_surf, cimg.buf, 0, 0);
    wl_surface_commit(rc.cursor_surf);
    rc_buf_destroy(&cimg);

    /* over the rawc window: enter; the client adopts a cursor (the
     * rc_ptr_enter handler sends set_cursor with the enter serial) */
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);
    RC_ALIVE(&rc);
    RC_WAIT(t, &rc, seat->cursor_surface != NULL, 1000);
    XWT_CHECK(seat->cursor_surface != NULL,
              "client cursor adopted after enter+set_cursor");

    /* move to the OTHER client's window: the rawc cursor must be
     * dropped (default arrow restored). This is the regression: the
     * stale I-beam/resize image from the previous client stuck
     * around until some other client changed it. */
    xw_compositor_inject_pointer_motion(t->comp, wb->x + wb->w / 2,
                                        wb->y + wb->h / 2);
    RC_WAIT(t, &rc, rc_count(&rc, 'l') >= 1, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->cursor_surface == NULL,
              "cursor reset to default when focus crosses clients "
              "(stale=%p)",
              (void *)seat->cursor_surface);

    /* back to the rawc window: enter fires, the client re-sets the
     * cursor (rc_ptr_enter does it automatically) and the image is
     * re-adopted */
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 2, 1000);
    RC_ALIVE(&rc);
    RC_WAIT(t, &rc, seat->cursor_surface != NULL, 1000);
    XWT_CHECK(seat->cursor_surface != NULL,
              "cursor re-adopted after re-enter + set_cursor");

    /* an IMPOSSIBLE serial (never issued) must be ignored: the client
     * tries to HIDE the cursor with a fabricated serial — validation
     * keeps the adopted image; a compositor that skips validation
     * would clear it */
    wl_pointer_set_cursor(rc.ptr, 0x7ffffff0, NULL, 0, 0);
    wl_display_flush(rc.d);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->cursor_surface != NULL,
              "set_cursor with a fabricated serial ignored (cursor kept)");

    /* a ROLED surface as cursor: rejected, client stays alive. The
     * rejected request must leave the CURRENT cursor in place. */
    struct pev *ent = rc_last(&rc, 'e');
    XWT_ASSERT(ent);
    wl_pointer_set_cursor(rc.ptr, ent->serial, rc.surf, 0, 0);
    wl_display_flush(rc.d);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->cursor_surface &&
                  seat->cursor_surface->role == XW_SURFACE_ROLE_NONE,
              "roled-surface cursor rejected (cursor stays roleless)");

    /* cursor hidden via NULL surface: default restored */
    ent = rc_last(&rc, 'e');
    XWT_ASSERT(ent);
    wl_pointer_set_cursor(rc.ptr, ent->serial, NULL, 0, 0);
    wl_display_flush(rc.d);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->cursor_surface == NULL, "NULL surface hides the cursor");

    /* the cursor surface dies: the seat forgets it (no dangling) */
    ent = rc_last(&rc, 'e');
    XWT_ASSERT(ent);
    wl_pointer_set_cursor(rc.ptr, ent->serial, rc.cursor_surf, 3, 2);
    wl_display_flush(rc.d);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    XWT_CHECK(seat->cursor_surface != NULL, "cursor re-adopted before destroy");
    wl_surface_destroy(rc.cursor_surf);
    rc.cursor_surf = NULL;
    wl_display_flush(rc.d);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->cursor_surface == NULL,
              "seat forgot the destroyed cursor surface");

    xwc_win_destroy(other);
    rc_destroy(&rc);
}

/* ============================================== 4: right-click menu flow */

/* create the context-menu popup on the raw client (parented to its
 * own toplevel, the browser/menu shape) and grab with the press
 * serial — exactly what every toolkit does on a right-click */
static bool rc_open_menu(struct xwt_ctx *t, struct rawc *rc,
                         struct xw_window *w, uint32_t grab_serial) {
    rc->pop_surf = wl_compositor_create_surface(rc->compositor);
    rc->pop_xs = xdg_wm_base_get_xdg_surface(rc->wm_base, rc->pop_surf);
    xdg_surface_add_listener(rc->pop_xs, &rc_xs_listener, rc);
    struct xdg_positioner *pos =
        xdg_wm_base_create_positioner(rc->wm_base);
    xdg_positioner_set_size(pos, 80, 50);
    xdg_positioner_set_anchor_rect(pos, w->w / 2 - 10, w->h / 2 - 10, 20, 20);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    rc->pop = xdg_surface_get_popup(rc->pop_xs, rc->xs, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(rc->pop, &rc_pop_listener, rc);

    /* bufferless commit first: the configure cycle precedes content */
    wl_surface_commit(rc->pop_surf);
    if (!RC_WAIT(t, rc, rc->pop_configured, 2000))
        return false;
    if (!rc_buf_create(rc, &rc->pop_buf, 80, 50))
        return false;
    wl_surface_attach(rc->pop_surf, rc->pop_buf.buf, 0, 0);
    wl_surface_damage(rc->pop_surf, 0, 0, 80, 50);
    wl_surface_commit(rc->pop_surf);

    xdg_popup_grab(rc->pop, rc->seat, grab_serial);
    wl_display_flush(rc->d);
    return true;
}

static void rc_close_menu(struct rawc *rc) {
    if (rc->pop)
        xdg_popup_destroy(rc->pop);
    if (rc->pop_xs)
        xdg_surface_destroy(rc->pop_xs);
    if (rc->pop_surf)
        wl_surface_destroy(rc->pop_surf);
    rc_buf_destroy(&rc->pop_buf);
    rc->pop = NULL;
    rc->pop_xs = NULL;
    rc->pop_surf = NULL;
}

/* THE priority-1 flow: right press -> popup + grab -> item click ->
 * outside press -> dismissal -> parent refocus -> destroy. Every
 * stage asserts both client-side events and server-side grab state,
 * and the client must be ALIVE at every step (a right click must
 * never kill a Wayland client). */
static void test_input_right_click_menu(struct xwt_ctx *t) {
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 300, 220));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    struct xw_seat *seat = xw_seat_first(t->comp);

    int cx = w->x + w->w / 2, cy = w->y + w->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);
    RC_ALIVE(&rc);

    /* RIGHT press: delivered to the client (the trigger) */
    xw_compositor_inject_pointer_button(t->comp, BTN_R, true);
    RC_WAIT(t, &rc, rc_find_button(&rc, BTN_R,
                                   WL_POINTER_BUTTON_STATE_PRESSED) != NULL,
            1000);
    RC_ALIVE(&rc);
    uint32_t press_serial =
        rc_find_button(&rc, BTN_R, WL_POINTER_BUTTON_STATE_PRESSED)->serial;

    /* the toolkit opens the menu: popup + grab(press_serial) */
    XWT_ASSERT(rc_open_menu(t, &rc, w, press_serial));
    for (int i = 0; i < 30; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);

    /* server-side grab state: the popup owns the seat */
    struct xw_popup *p = NULL;
    if (!wl_list_empty(&t->comp->popups))
        p = wl_container_of(t->comp->popups.next, p, link);
    XWT_CHECK(p && p->mapped, "popup mapped server-side");
    XWT_CHECK(p && p->grabbed, "popup grab registered");
    XWT_CHECK(seat->grab_surface && seat->grab_surface == p->surface,
              "seat grab surface IS the popup");
    XWT_CHECK(seat->ptr_grab != NULL, "seat grab pointer resource set");

    /* client got the enter on the popup surface */
    RC_WAIT(t, &rc, rc_last(&rc, 'e') && rc_last(&rc, 'e')->on_popup, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(rc_last(&rc, 'e') && rc_last(&rc, 'e')->on_popup,
              "pointer enter delivered on the popup");

    /* release of the right button: the grab outlives the click */
    xw_compositor_inject_pointer_button(t->comp, BTN_R, false);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->grab_surface && seat->grab_surface == p->surface,
              "grab persists through the opening release");

    /* left click ON the popup (menu item): delivered to the popup's
     * client with the pointer focused there */
    int px = p->anchor_x + p->w / 2, py = p->anchor_y + p->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, px, py);
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    xw_compositor_inject_pointer_button(t->comp, BTN_L, true);
    RC_WAIT(t, &rc, rc_find_button(&rc, BTN_L,
                                   WL_POINTER_BUTTON_STATE_PRESSED) != NULL,
            1000);
    RC_ALIVE(&rc);
    xw_compositor_inject_pointer_button(t->comp, BTN_L, false);
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);

    /* left press OUTSIDE the popup: dismisses the menu (popup_done),
     * the parent regains pointer focus, the grab is released */
    rc.pop_done = false;
    xw_compositor_inject_pointer_motion(t->comp, 5, 700); /* empty area */
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    int enters_before = rc_count(&rc, 'e');
    xw_compositor_inject_pointer_button(t->comp, BTN_L, true);
    RC_WAIT(t, &rc, rc.pop_done, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(rc.pop_done, "popup_done delivered on outside press");
    XWT_CHECK(seat->grab_surface == NULL && seat->ptr_grab == NULL,
              "seat grab released at dismissal");
    XWT_CHECK(seat->ptr_focus == NULL,
              "pointer focus cleared over empty space after dismissal");
    xw_compositor_inject_pointer_button(t->comp, BTN_L, false);
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    (void)enters_before;

    /* the client destroys the dismissed popup: state cleans up */
    rc_close_menu(&rc);
    wl_display_flush(rc.d);
    for (int i = 0; i < 30; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(wl_list_empty(&t->comp->popups),
              "popup list empty after client destroy");

    /* back onto the window: enter+set_cursor work again (full cycle) */
    int e2 = rc_count(&rc, 'e');
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') > e2, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(rc_last(&rc, 'e') && rc_last(&rc, 'e')->on_self,
              "parent window regains pointer focus after dismissal");

    rc_destroy(&rc);
}

/* ============================================ 5: popup destroy under grab */

/* the client tears the menu down itself (item selected, Escape)
 * while the grab is active: no crash, no dangling grab, focus
 * returns somewhere sane, the client survives. */
static void test_input_popup_destroy_grab(struct xwt_ctx *t) {
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 300, 220));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    struct xw_seat *seat = xw_seat_first(t->comp);

    int cx = w->x + w->w / 2, cy = w->y + w->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);
    xw_compositor_inject_pointer_button(t->comp, BTN_R, true);
    RC_WAIT(t, &rc, rc_find_button(&rc, BTN_R,
                                   WL_POINTER_BUTTON_STATE_PRESSED) != NULL,
            1000);
    uint32_t s = rc_find_button(&rc, BTN_R, WL_POINTER_BUTTON_STATE_PRESSED)
                     ->serial;
    XWT_ASSERT(rc_open_menu(t, &rc, w, s));
    for (int i = 0; i < 30; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    struct xw_popup *p = NULL;
    if (!wl_list_empty(&t->comp->popups))
        p = wl_container_of(t->comp->popups.next, p, link);
    XWT_ASSERT(p && p->mapped && p->grabbed);
    XWT_CHECK(seat->grab_surface && seat->grab_surface == p->surface,
              "grab active before the destroy");

    /* destroy the popup AND its surface mid-grab */
    rc_close_menu(&rc);
    wl_display_flush(rc.d);
    for (int i = 0; i < 30; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);

    XWT_CHECK(wl_list_empty(&t->comp->popups), "popup unregistered");
    XWT_CHECK(seat->grab_surface == NULL,
              "no dangling grab after popup destroy");
    XWT_CHECK(seat->ptr_focus == w->surface || seat->ptr_focus == NULL,
              "pointer focus either back on the parent or NULL (got %p)",
              (void *)seat->ptr_focus);

    /* motion + click still work afterwards (nothing references the
     * dead surface) */
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    xw_compositor_inject_pointer_button(t->comp, BTN_L, true);
    RC_WAIT(t, &rc, rc_find_button(&rc, BTN_L,
                                   WL_POINTER_BUTTON_STATE_PRESSED) != NULL,
            1000);
    RC_ALIVE(&rc);
    xw_compositor_inject_pointer_button(t->comp, BTN_L, false);
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    rc_destroy(&rc);
}

/* ============================================ 6: taskbar activation */

struct ft_state {
    struct zwlr_foreign_toplevel_manager_v1 *mgr;
    struct zwlr_foreign_toplevel_handle_v1 *handle; /* rawc window's */
    struct zwlr_foreign_toplevel_handle_v1 *all[8]; /* every announced
                                                       (destroyed at
                                                       teardown: a leaked
                                                       wl_proxy holds its
                                                       queue + fd state) */
    int n_all;
    char last_title[128];
    bool minimized, activated;
};

static void ft_toplevel(void *data,
                        struct zwlr_foreign_toplevel_manager_v1 *m,
                        struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)m;
    struct ft_state *ft = data;
    if (ft->n_all < 8)
        ft->all[ft->n_all++] = h;
    if (!ft->handle)
        ft->handle = h; /* first announced = the only window we track */
}
static void ft_closed(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)data;
    (void)h;
}
static void ft_done_ev(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)data;
    (void)h;
}
static void ft_title(void *data,
                     struct zwlr_foreign_toplevel_handle_v1 *h,
                     const char *title) {
    (void)h;
    struct ft_state *ft = data;
    snprintf(ft->last_title, sizeof(ft->last_title), "%s", title ? title : "");
}
static void ft_app_id(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *h,
                      const char *app_id) {
    (void)data;
    (void)h;
    (void)app_id;
}
static void ft_state_ev(void *data,
                        struct zwlr_foreign_toplevel_handle_v1 *h,
                        struct wl_array *state) {
    (void)h;
    struct ft_state *ft = data;
    ft->minimized = ft->activated = false;
    uint32_t *st;
    wl_array_for_each(st, state) {
        if (*st == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
            ft->minimized = true;
        if (*st == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            ft->activated = true;
    }
}
static void ft_output_enter(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *o) {
    (void)data;
    (void)h;
    (void)o;
}
static void ft_output_leave(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *o) {
    (void)data;
    (void)h;
    (void)o;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener ft_handle_l = {
    .closed = ft_closed,
    .title = ft_title,
    .app_id = ft_app_id,
    .state = ft_state_ev,
    .output_enter = ft_output_enter,
    .output_leave = ft_output_leave,
    .done = ft_done_ev,
};

static void ft_manager_finished(
    void *data, struct zwlr_foreign_toplevel_manager_v1 *m) {
    (void)data;
    (void)m;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener ft_mgr_l = {
    .toplevel = ft_toplevel,
    .finished = ft_manager_finished,
};

/* one-shot registry listener that binds the foreign-toplevel manager */
struct ft_bind {
    struct zwlr_foreign_toplevel_manager_v1 *mgr;
    bool found;
};

static void ftb_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    struct ft_bind *fb = data;
    if (strcmp(iface, "zwlr_foreign_toplevel_manager_v1") == 0) {
        fb->mgr = wl_registry_bind(
            r, name, &zwlr_foreign_toplevel_manager_v1_interface,
            version > 3 ? 3 : version);
        fb->found = true;
    }
}
static void ftb_global_remove(void *data, struct wl_registry *r,
                              uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}
static const struct wl_registry_listener ftb_reg_listener = {
    .global = ftb_global,
    .global_remove = ftb_global_remove,
};

/* the taskbar click: exactly what xwc_tasklist_activate sends
 * (unset_minimized + activate on the handle for the window) */
static void test_input_taskbar_activate(struct xwt_ctx *t) {
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 260, 180));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    struct xw_wm *wm = t->comp->wm;
    struct xw_seat *seat = xw_seat_first(t->comp);

    /* a second window so focus has somewhere to go on minimize */
    struct xwc_win *other = xwt_window_solid(t, 0xff305070, 200, 140, "tbB");
    XWT_ASSERT(other);
    struct xw_window *wb = find_by_title(t, "tbB");
    XWT_ASSERT(wb);

    /* bind foreign-toplevel on the raw client (the taskbar's view) */
    struct ft_state ft = {0};
    struct ft_bind fb = {0};
    struct wl_registry *reg = wl_display_get_registry(rc.d);
    wl_registry_add_listener(reg, &ftb_reg_listener, &fb);
    wl_display_flush(rc.d);
    bool bound = false;
    for (int i = 0; i < 500 && !bound; i++) {
        rc_pump(t, &rc);
        bound = fb.found;
    }
    XWT_ASSERT(bound);
    zwlr_foreign_toplevel_manager_v1_add_listener(fb.mgr, &ft_mgr_l, &ft);
    wl_display_flush(rc.d);
    /* the manager announces existing windows: wait for the handle */
    bool announced = false;
    for (int i = 0; i < 500 && !announced; i++) {
        rc_pump(t, &rc);
        announced = ft.handle != NULL;
    }
    XWT_ASSERT(announced);
    zwlr_foreign_toplevel_handle_v1_add_listener(ft.handle, &ft_handle_l,
                                                  &ft);
    wl_display_flush(rc.d);
    for (int i = 0; i < 100; i++)
        rc_pump(t, &rc);

    /* --- the minimize half: xdg_toplevel.set_minimized (real path) */
    xdg_toplevel_set_minimized(rc.tl);
    wl_display_flush(rc.d);
    XWT_WAIT(t, w->minimized);
    XWT_CHECK(w->minimized, "window minimized via protocol request");
    XWT_CHECK(wm->focused == wb, "focus moved to the other window");
    XWT_CHECK(seat->kb_focus == wb->surface, "keyboard focus follows");

    /* --- the activation half: the taskbar's exact request pair */
    zwlr_foreign_toplevel_handle_v1_unset_minimized(ft.handle);
    zwlr_foreign_toplevel_handle_v1_activate(ft.handle, rc.seat);
    wl_display_flush(rc.d);
    bool restored = false;
    for (int i = 0; i < 500 && !restored; i++) {
        rc_pump(t, &rc);
        restored = !w->minimized && wm->focused == w;
    }
    XWT_CHECK(!w->minimized, "taskbar activation unminimizes the window");
    XWT_CHECK(wm->focused == w,
              "taskbar activation focuses the window (focused=%p want %p)",
              (void *)wm->focused, (void *)w);
    XWT_CHECK(seat->kb_focus == w->surface,
              "keyboard focus follows the activation");
    XWT_CHECK(xw_wm_window_visible(wm, w),
              "the activated window is visible");
    /* raised to the top of the stack (stack head = topmost) */
    struct xw_window *top =
        wl_container_of(wm->stack.next, top, stack_link);
    XWT_CHECK(top == w, "the activated window was raised");
    /* the client saw the ACTIVATED toplevel state */
    XWT_CHECK(rc.state_activated,
              "client received configure with ACTIVATED");
    /* the taskbar client saw the state flip too */
    XWT_CHECK(ft.activated && !ft.minimized,
              "foreign-toplevel state reflects activation (act=%d min=%d)",
              ft.activated, ft.minimized);

    /* --- activate WITHOUT unset_minimized: a taskbar/activation
     * path that only sends activate must still restore the window
     * (focus refused on a minimized window = the dead taskbar
     * button) */
    xdg_toplevel_set_minimized(rc.tl);
    wl_display_flush(rc.d);
    XWT_WAIT(t, w->minimized);
    zwlr_foreign_toplevel_handle_v1_activate(ft.handle, rc.seat);
    wl_display_flush(rc.d);
    bool act_only = false;
    for (int i = 0; i < 500 && !act_only; i++) {
        rc_pump(t, &rc);
        act_only = !w->minimized && wm->focused == w;
    }
    XWT_CHECK(act_only,
              "bare activate on a minimized window restores+focuses it "
              "(min=%d focused=%p want %p)",
              w->minimized, (void *)wm->focused, (void *)w);

    /* --- minimize + activate AGAIN (repeat cycles must hold) */
    xdg_toplevel_set_minimized(rc.tl);
    wl_display_flush(rc.d);
    XWT_WAIT(t, w->minimized);
    zwlr_foreign_toplevel_handle_v1_unset_minimized(ft.handle);
    zwlr_foreign_toplevel_handle_v1_activate(ft.handle, rc.seat);
    wl_display_flush(rc.d);
    bool again = false;
    for (int i = 0; i < 500 && !again; i++) {
        rc_pump(t, &rc);
        again = !w->minimized && wm->focused == w;
    }
    XWT_CHECK(again, "second minimize/activate cycle restores focus");

    /* --- cross-workspace activation */
    xw_wm_window_to_workspace(wm, w, 1);
    xwt_pump(t);
    XWT_CHECK(w->ws == 1 && wm->ws_current == 0,
              "window moved to ws1 (ws=%d current=%d)", w->ws,
              wm->ws_current);
    zwlr_foreign_toplevel_handle_v1_activate(ft.handle, rc.seat);
    wl_display_flush(rc.d);
    bool ws_switched = false;
    for (int i = 0; i < 500 && !ws_switched; i++) {
        rc_pump(t, &rc);
        ws_switched = wm->ws_current == 1 && wm->focused == w;
    }
    XWT_CHECK(ws_switched,
              "activating a window on another workspace switches to it "
              "and focuses it (ws=%d focused=%p)",
              wm->ws_current, (void *)wm->focused);

    /* cleanup: handles first (the manager's stop finished them),
     * then the manager binding, then the registry */
    for (int i = 0; i < ft.n_all; i++)
        zwlr_foreign_toplevel_handle_v1_destroy(ft.all[i]);
    zwlr_foreign_toplevel_manager_v1_stop(fb.mgr);
    zwlr_foreign_toplevel_manager_v1_destroy(fb.mgr);
    wl_registry_destroy(reg);
    xwc_win_destroy(other);
    rc_destroy(&rc);
}

/* ================================================== 7: real GTK clicks */

/* the physical-truth analog: a REAL GTK4 process (zenity) connected
 * to the compositor; motion + left/right/middle clicks must never
 * kill the toolkit. If GTK dies, the exit status + the log say so. */
static void test_input_real_gtk_clicks(struct xwt_ctx *t) {
    const char *apps = "/home/z/my-project/.apps-root";
    struct stat st;
    if (stat(apps, &st) != 0 || !S_ISDIR(st.st_mode)) {
        XWT_SKIP("apps-root not present (real-client round)");
        return;
    }
    char zenity_path[128];
    snprintf(zenity_path, sizeof(zenity_path), "%s/usr/bin/zenity", apps);
    if (stat(zenity_path, &st) != 0) {
        XWT_SKIP("zenity not present");
        return;
    }

    /* restore default signal dispositions in the child (the
     * compositor blocks signals via signalfd; the mask survives
     * fork+exec — same lesson as test_xwm). stderr goes to a log so a
     * toolkit death is attributable, not a mystery. */
    char logpath[128];
    snprintf(logpath, sizeof(logpath), "%s/zenity-input.log", g_runtimedir());
    FILE *zlog = fopen(logpath, "w");
    int wx = 0, wy = 0, ww = 0, wh = 0;
    pid_t pid = fork();
    XWT_ASSERT(pid >= 0);
    if (pid == 0) {
        for (int i = 1; i < 32; i++)
            signal(i, SIG_DFL);
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("LD_LIBRARY_PATH",
               "/home/z/my-project/.apps-root/usr/lib/x86_64-linux-gnu:"
               "/home/z/my-project/.toolchain/sysroot/usr/lib/x86_64-linux-gnu",
               1);
        unsetenv("DISPLAY");
        if (zlog) {
            dup2(fileno(zlog), 1);
            dup2(fileno(zlog), 2);
        }
        char *const argv[] = {zenity_path, (char *)"--info",
                              (char *)"--text=clicktest", NULL};
        execv(zenity_path, argv);
        _exit(127);
    }
    if (zlog)
        fclose(zlog);

    /* wait for the GTK window to map (real startup time); snapshot
     * the geometry the moment it appears — the window object must
     * never be held across dispatches (a dead client frees it) */
    bool mapped = false;
    for (int i = 0; i < 1500 && !mapped; i++) {
        xw_compositor_dispatch(t->comp, 0);
        struct xw_window *w = NULL;
        wl_list_for_each(w, &t->comp->wm->windows, link) {
            if (w->mapped && w->surface && w->surface->shm) {
                wx = w->x;
                wy = w->y;
                ww = w->w;
                wh = w->h;
                mapped = true;
                break;
            }
        }
        if (!mapped)
            usleep(2000);
    }
    if (!mapped) {
        int status = 0;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        XWT_CHECK(false, "zenity never mapped a window (log: %s)", logpath);
        return;
    }
    XWT_CHECK(mapped, "GTK4 window mapped %dx%d+%d+%d", ww, wh, wx, wy);

    /* drive the pointer: motion over the window, then L/R/M clicks */
    int cx = wx + ww / 2, cy = wy + wh / 3;
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    for (int i = 0; i < 50; i++) {
        xw_compositor_dispatch(t->comp, 0);
        usleep(2000);
    }
    xw_compositor_inject_pointer_button(t->comp, BTN_L, true);
    xw_compositor_dispatch(t->comp, 0);
    xw_compositor_inject_pointer_button(t->comp, BTN_L, false);
    xw_compositor_inject_pointer_button(t->comp, BTN_R, true);
    xw_compositor_dispatch(t->comp, 0);
    xw_compositor_inject_pointer_button(t->comp, BTN_R, false);
    xw_compositor_inject_pointer_button(t->comp, BTN_M, true);
    xw_compositor_dispatch(t->comp, 0);
    xw_compositor_inject_pointer_button(t->comp, BTN_M, false);
    xw_compositor_inject_pointer_axis(t->comp, 0, 5.0);

    /* the toolkit must survive all of it (2s of real pumping) */
    for (int i = 0; i < 1000; i++) {
        xw_compositor_dispatch(t->comp, 0);
        usleep(2000);
    }
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0) {
        XWT_CHECK(true, "GTK4 client alive after motion+L/R/M clicks");
        kill(pid, SIGTERM);
        for (int i = 0; i < 100; i++) {
            if (waitpid(pid, &status, WNOHANG) != 0)
                break;
            usleep(5000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    } else {
        XWT_CHECK(false,
                  "GTK4 client DIED on the input battery (exit %d, sig %d, "
                  "log: %s)",
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                  WIFSIGNALED(status) ? WTERMSIG(status) : 0, logpath);
    }
}

/* ==================================================== 8: popup_done once */

/* the right-click killer regression: after the compositor's outside-
 * press dismissal (popup_done #1), a spec-typical client ALSO
 * detaches the popup's buffer (a null commit = unmap). The old code
 * sent a SECOND popup_done from the unmap path -- landing on an
 * xdg_popup object the client had often already destroyed -> invalid
 * object -> libwayland KILLED THE CLIENT. popup_done is once per
 * popup lifetime; the client must survive the whole dance. */
static void test_input_popup_done_once(struct xwt_ctx *t) {
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 300, 220));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    struct xw_seat *seat = xw_seat_first(t->comp);

    int cx = w->x + w->w / 2, cy = w->y + w->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);
    xw_compositor_inject_pointer_button(t->comp, BTN_R, true);
    RC_WAIT(t, &rc,
            rc_find_button(&rc, BTN_R,
                           WL_POINTER_BUTTON_STATE_PRESSED) != NULL,
            1000);
    uint32_t s = rc_find_button(&rc, BTN_R,
                                WL_POINTER_BUTTON_STATE_PRESSED)
                     ->serial;
    XWT_ASSERT(rc_open_menu(t, &rc, w, s));
    /* wait for the SERVER-side state (mapped + grab processed), not
     * the client-side configure echo -- the commit+grab requests need
     * their own dispatch cycles */
    struct xw_popup *p = NULL;
    bool ready = false;
    for (int i = 0; i < 1000 && !ready; i++) {
        rc_pump(t, &rc);
        p = NULL;
        if (!wl_list_empty(&t->comp->popups))
            p = wl_container_of(t->comp->popups.next, p, link);
        ready = p && p->mapped && p->grabbed;
    }
    XWT_ASSERT(ready);

    /* outside press -> dismissal -> popup_done #1 */
    xw_compositor_inject_pointer_motion(t->comp, 5, 700);
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    xw_compositor_inject_pointer_button(t->comp, BTN_L, true);
    RC_WAIT(t, &rc, rc.pop_done, 1000);
    RC_ALIVE(&rc);
    xw_compositor_inject_pointer_button(t->comp, BTN_L, false);
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    XWT_CHECK(rc.pop_done_count == 1,
              "exactly ONE popup_done at dismissal (got %d)",
              rc.pop_done_count);

    /* the toolkit's own teardown: detach the buffer (unmap commit),
     * THEN destroy the objects -- every order must stay at one done */
    wl_surface_attach(rc.pop_surf, NULL, 0, 0);
    wl_surface_commit(rc.pop_surf);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(rc.pop_done_count == 1,
              "client unmap after dismissal sent NO second popup_done "
              "(got %d)",
              rc.pop_done_count);
    XWT_CHECK(!p->mapped || p->res == NULL || p->done_sent,
              "server state consistent after the unmap");

    rc_close_menu(&rc);
    wl_display_flush(rc.d);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(rc.pop_done_count == 1,
              "object destroy after unmap still exactly one done (got %d)",
              rc.pop_done_count);
    XWT_CHECK(wl_list_empty(&t->comp->popups), "popup list clean");
    (void)seat;
    rc_destroy(&rc);
}

/* ========================================= 9: toplevel null-commit hide */

/* the protocol hide: attach(NULL) + commit unmaps the toplevel (GTK
 * hides windows this way). The pre-2026-09-06 code IGNORED the
 * transition -- a hidden window stayed visible, clickable and in the
 * taskbar forever (ghost windows). Re-showing works with a new
 * buffer; a plain no-attach commit must NOT unmap (sticky attach). */
static void test_input_toplevel_null_unmap(struct xwt_ctx *t) {
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 240, 160));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    struct xw_seat *seat = xw_seat_first(t->comp);

    int cx = w->x + w->w / 2, cy = w->y + w->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);

    /* rc_map_window deliberately destroys its buffer right after the
     * mapping commit (that pins the displayed-buffer-destroyed
     * regression). Re-establish live content: a buffer that STAYS
     * alive (the real-client shape) for the hide/show cycle. */
    struct rc_buf b;
    XWT_ASSERT(rc_buf_create(&rc, &b, 240, 160));
    wl_surface_attach(rc.surf, b.buf, 0, 0);
    wl_surface_damage(rc.surf, 0, 0, 240, 160);
    wl_surface_commit(rc.surf);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);

    /* damage-only commit (NO attach): sticky buffer -- no unmap, no
     * content loss, the window stays interactive */
    wl_surface_damage(rc.surf, 0, 0, 10, 10);
    wl_surface_commit(rc.surf);
    for (int i = 0; i < 20; i++)
        rc_pump(t, &rc);
    RC_ALIVE(&rc);
    XWT_CHECK(w->mapped, "no-attach commit keeps the window mapped");
    XWT_CHECK(rc.n_ev >= 1 && seat->ptr_focus == w->surface,
              "no-attach commit keeps pointer focus");

    /* the explicit hide: attach(NULL) + commit */
    wl_surface_attach(rc.surf, NULL, 0, 0);
    wl_surface_commit(rc.surf);
    bool unmapped = false;
    for (int i = 0; i < 500 && !unmapped; i++) {
        rc_pump(t, &rc);
        unmapped = !w->mapped;
    }
    XWT_CHECK(!w->mapped, "null-attach commit unmaps the toplevel");
    /* focus dropped (nothing under the cursor now) */
    XWT_CHECK(seat->kb_focus != w->surface,
              "keyboard focus released from the hidden window");
    XWT_CHECK(seat->ptr_focus != w->surface,
              "pointer focus released from the hidden window");
    RC_ALIVE(&rc);

    /* the window's wl_surface still exists: clicks do NOT reach it */
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    int enters = rc_count(&rc, 'e');
    for (int i = 0; i < 10; i++)
        rc_pump(t, &rc);
    XWT_CHECK(seat->ptr_focus != w->surface,
              "hidden window is not hit-testable");

    /* re-show by re-attaching the released buffer: the compositor
     * restarts the configure cycle; the client acks (automatic in
     * rc_xs_configure) and re-commits -- that commit maps it (the
     * protocol's map-after-hide flow) */
    uint32_t acks_before = rc.config_serial;
    wl_surface_attach(rc.surf, b.buf, 0, 0);
    wl_surface_damage(rc.surf, 0, 0, 240, 160);
    wl_surface_commit(rc.surf);
    bool cfg2 = false;
    for (int i = 0; i < 500 && !cfg2; i++) {
        rc_pump(t, &rc);
        cfg2 = rc.config_serial != acks_before; /* a fresh configure+ack */
    }
    XWT_CHECK(cfg2, "configure cycle restarted for the re-show");
    wl_surface_attach(rc.surf, b.buf, 0, 0);
    wl_surface_damage(rc.surf, 0, 0, 240, 160);
    wl_surface_commit(rc.surf);
    bool remapped = false;
    for (int i = 0; i < 500 && !remapped; i++) {
        rc_pump(t, &rc);
        remapped = w->mapped;
    }
    XWT_CHECK(w->mapped, "re-attach maps the window again");
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') > enters, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(seat->ptr_focus == w->surface,
              "re-shown window takes pointer focus again");

    rc_buf_destroy(&b);
    rc_destroy(&rc);
}

/* ================================================== CSD pointer geometry */

/* THE move/resize hit-offset physical bug, headlessly: a CSD client
 * with set_window_geometry (shadow margins / header offset) must
 * receive pointer coordinates relative to the BUFFER (wl_surface)
 * origin — that is the space its widgets, header bar and resize
 * margins live in. Delivering window-geometry-rect-relative coords
 * instead shifts every client-side hit zone up-left by (geo_x, geo_y):
 * the user hovers the visible title bar and the client believes the
 * pointer is in its shadow/resize margin (resize cursor!), and a
 * window MOVE only triggers once the pointer is pushed (geo_y) pixels
 * deeper into the app. Render and hit-test already honor the offset;
 * the event stream must match them. */
static void test_input_csd_pointer_geometry(struct xwt_ctx *t) {
    const int GX = 10, GY = 14; /* the client's shadow offset */
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 200, 150));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    int bx = w->x, by = w->y; /* the content rect origin after CSD */

    /* declare the content rect: offset (GX,GY) inside the 200x150
     * buffer, size 190x136 */
    xdg_surface_set_window_geometry(rc.xs, GX, GY, 190, 136);
    RC_WAIT(t, &rc, w->geometry_set, 1000);
    RC_ALIVE(&rc);
    XWT_CHECK(w->geometry_set && w->geo_x == GX && w->geo_y == GY,
              "geometry offset stored (%d,%d)", w->geo_x, w->geo_y);
    XWT_CHECK(w->w == 190 && w->h == 136, "window sized to the geometry "
              "(got %dx%d)", w->w, w->h);

    /* enter from outside: pointer at content (20,15) is buffer (30,29) */
    xw_compositor_inject_pointer_motion(t->comp, 5, 5); /* outside */
    for (int i = 0; i < 5; i++)
        xwt_pump(t);
    xw_compositor_inject_pointer_motion(t->comp, bx + 20, by + 15);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 1, 1000);
    RC_ALIVE(&rc);
    struct pev *e = rc_last(&rc, 'e');
    XWT_CHECK(e && e->on_self, "enter delivered on the CSD window");
    XWT_CHECK(e && e->sx == 20 + GX && e->sy == 15 + GY,
              "enter surface-local coords %d,%d — must be BUFFER-relative "
              "(%d,%d), not geometry-rect-relative (%d,%d)",
              e ? e->sx : -99, e ? e->sy : -99, 20 + GX, 15 + GY, 20, 15);

    /* motion across the content rect: every event stays buffer-relative */
    int n_motions = rc_count(&rc, 'm');
    xw_compositor_inject_pointer_motion(t->comp, bx + 60, by + 40);
    RC_WAIT(t, &rc, rc_count(&rc, 'm') > n_motions, 1000);
    RC_ALIVE(&rc);
    struct pev *m = rc_last(&rc, 'm');
    XWT_CHECK(m && m->sx == 60 + GX && m->sy == 40 + GY,
              "motion surface-local coords %d,%d — must be (%d,%d)",
              m ? m->sx : -99, m ? m->sy : -99, 60 + GX, 40 + GY);

    /* leave + re-enter at the other corner keeps the same convention */
    xw_compositor_inject_pointer_motion(t->comp, 5, 5);
    RC_WAIT(t, &rc, rc_count(&rc, 'l') >= 1, 1000);
    int enters = rc_count(&rc, 'e');
    xw_compositor_inject_pointer_motion(t->comp, bx + 189, by + 135);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') > enters, 1000);
    RC_ALIVE(&rc);
    e = rc_last(&rc, 'e');
    XWT_CHECK(e && e->sx == 189 + GX && e->sy == 135 + GY,
              "re-enter surface-local coords %d,%d — must be (%d,%d)",
              e ? e->sx : -99, e ? e->sy : -99, 189 + GX, 135 + GY);

    rc_destroy(&rc);
}

/* ================================================= CSD popup anchoring */

/* The other half of the CSD space family: xdg_popup anchors are
 * specified in PARENT-SURFACE (buffer) coordinates. With a CSD parent
 * (set_window_geometry offset) the parent buffer origin sits geo_*
 * left/above the window-rect origin — anchoring via the window-rect
 * origin shifts every menu/popover by exactly the shadow margin
 * (menus open (geo_x, geo_y) off from the button that opened them).
 * The configure reply must use the same space. */
static void test_input_csd_popup_anchor(struct xwt_ctx *t) {
    const int GX = 10, GY = 14;
    struct rawc rc;
    XWT_ASSERT(rc_connect(&rc, t));
    XWT_ASSERT(rc_map_window(t, &rc, 200, 150));
    struct xw_window *w = find_by_title(t, "rawc");
    XWT_ASSERT(w);
    xdg_surface_set_window_geometry(rc.xs, GX, GY, 190, 136);
    RC_WAIT(t, &rc, w->geometry_set, 1000);
    RC_ALIVE(&rc);

    /* a menu anchored at parent-buffer (40,50), extending down-right */
    rc.pop_surf = wl_compositor_create_surface(rc.compositor);
    rc.pop_xs = xdg_wm_base_get_xdg_surface(rc.wm_base, rc.pop_surf);
    xdg_surface_add_listener(rc.pop_xs, &rc_xs_listener, &rc);
    struct xdg_positioner *pos =
        xdg_wm_base_create_positioner(rc.wm_base);
    xdg_positioner_set_size(pos, 60, 40);
    xdg_positioner_set_anchor_rect(pos, 40, 50, 20, 20);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    rc.pop = xdg_surface_get_popup(rc.pop_xs, rc.xs, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(rc.pop, &rc_pop_listener, &rc);
    wl_surface_commit(rc.pop_surf);
    RC_WAIT(t, &rc, rc.pop_configured, 2000);
    RC_ALIVE(&rc);
    if (!rc_buf_create(&rc, &rc.pop_buf, 60, 40)) {
        XWT_CHECK(false, "popup buffer alloc failed");
        return;
    }
    wl_surface_attach(rc.pop_surf, rc.pop_buf.buf, 0, 0);
    wl_surface_damage(rc.pop_surf, 0, 0, 60, 40);
    wl_surface_commit(rc.pop_surf);
    RC_WAIT(t, &rc, rc_count(&rc, 'e') >= 0, 500); /* settle */

    /* the server-side placement is the authoritative check: the popup
     * must land at parent BUFFER origin + anchor point. The configure
     * x/y alone cannot catch this bug (it subtracts the same origin
     * both sides) — the rendered/hit-tested position is what shifts. */
    uint32_t pop_id =
        wl_proxy_get_id((struct wl_proxy *)rc.pop_surf);
    struct xw_popup *p = NULL, *it;
    wl_list_for_each(it, &t->comp->popups, link) {
        if (it->surface &&
            wl_resource_get_id(it->surface->res) == pop_id) {
            p = it;
            break;
        }
    }
    XWT_CHECK(p != NULL, "popup found in the compositor's list");
    if (p) {
        int want_x = w->x - GX + 40, want_y = w->y - GY + 50;
        XWT_CHECK(p->anchor_x == want_x && p->anchor_y == want_y,
                  "popup placed at (%d,%d) — must be parent buffer "
                  "origin + anchor (%d,%d): anchoring off the window-rect "
                  "origin shifts every menu by the shadow margin (%d,%d)",
                  p->anchor_x, p->anchor_y, want_x, want_y, GX, GY);
    }

    /* the client-visible truth: configure x,y must be (40,50) —
     * relative to the parent BUFFER origin, exactly the anchor the
     * client asked for */
    XWT_CHECK(rc.pop_cfg_x == 40 && rc.pop_cfg_y == 50,
              "popup configure position (%d,%d) — must be the "
              "parent-buffer-relative anchor (40,50), not the "
              "window-rect-relative (%d,%d)",
              rc.pop_cfg_x, rc.pop_cfg_y, 40 - GX, 50 - GY);

    rc_close_menu(&rc);
    rc_destroy(&rc);
}

static const struct xwt_test tests[] = {
    {"input-toplevel-null-unmap", test_input_toplevel_null_unmap},
    {"input-popup-done-once", test_input_popup_done_once},
    {"input-event-matrix", test_input_event_matrix},
    {"input-hit-test-order", test_input_hit_test_order},
    {"input-cursor-state", test_input_cursor_state},
    {"input-right-click-menu", test_input_right_click_menu},
    {"input-popup-destroy-grab", test_input_popup_destroy_grab},
    {"input-taskbar-activate", test_input_taskbar_activate},
    {"input-csd-pointer-geometry", test_input_csd_pointer_geometry},
    {"input-csd-popup-anchor", test_input_csd_popup_anchor},
    {"input-real-gtk-clicks", test_input_real_gtk_clicks},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
