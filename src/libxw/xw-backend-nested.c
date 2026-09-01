/* xw-backend-nested.c — nested Wayland backend.
 *
 * Runs the whole compositor (WM, shells, shortcuts, session protocols)
 * as a toplevel window inside a parent Wayland compositor. This is the
 * safe development/testing workflow: the real desktop runs inside the
 * user's existing session instead of replacing it.
 *
 * Design: the backend is a *client* of the parent via libxwcl (the same
 * library the panel and exit dialog use — dogfooded, so any client-side
 * bug found here is a bug fixed for every xw client).
 *
 *  - one parent window (xdg toplevel) per output; v0 maps exactly one
 *    output, sized by the parent's configure
 *  - presenting = copying the output's native buffer into the window's
 *    SHM back buffer and committing (damage-driven; no frame pacing yet)
 *  - input: the parent delivers evdev keycodes / Wayland button codes;
 *    they are forwarded verbatim into our own seat, where the xkb state
 *    machine and the shortcut engine process them like real hardware
 *  - the parent connection's socket is multiplexed on the compositor's
 *    own event loop, so both sides make progress in one thread
 *
 * The parent handshake is normally blocking (like any client); an
 * in-process parent (tests) can supply a pump callback via
 * xw_compositor_config.nested_pump.
 */
#include "xw-internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client-core.h>

#include "xwc.h"

struct nested_backend {
    struct xw_backend base;

    struct xwc xwc;        /* parent connection (libxwcl) */
    struct xwc_win *win;   /* the desktop window in the parent */
    struct xw_output *output; /* our single output, 1:1 with the window */
    struct wl_event_source *client_src; /* parent fd on our loop */

    int last_conf_w, last_conf_h; /* size the parent configured */
};

/* ------------------------------------------------------------- helpers */

static struct xw_seat *nb_seat(struct nested_backend *nb) {
    return xw_seat_first(nb->base.comp);
}

static void nb_stop(struct nested_backend *nb, const char *why) {
    xw_log(nb->base.comp == NULL ? XW_LOG_ERROR : XW_LOG_INFO,
           "nested: stopping (%s)", why);
    if (nb->base.comp)
        xw_compositor_stop(nb->base.comp);
}

/* --------------------------------------------------------- xwc callbacks */

static void nb_on_key(struct xwc_win *w, uint32_t keycode, bool down,
                      xkb_keysym_t sym, uint32_t mods, void *ud);
static void nb_on_button(struct xwc_win *w, uint32_t button, bool down, int x,
                         int y, void *ud);
static void nb_on_motion(struct xwc_win *w, int x, int y, void *ud);
static void nb_on_configure(struct xwc_win *w, int w_, int h_, void *ud);
static void nb_on_close(struct xwc_win *w, void *ud);

static const struct xwc_callbacks nb_win_cb = {
    .key = nb_on_key,
    .button = nb_on_button,
    .motion = nb_on_motion,
    .configure = nb_on_configure,
    .close = nb_on_close,
};

static void nb_on_key(struct xwc_win *w, uint32_t keycode, bool down,
                      xkb_keysym_t sym, uint32_t mods, void *ud) {
    (void)w;
    (void)sym;
    (void)mods;
    struct nested_backend *nb = ud;
    struct xw_seat *s = nb_seat(nb);
    if (s && keycode < 512) /* evdev codes; defensive bound */
        xw_seat_key(s, keycode, down);
}

static void nb_on_button(struct xwc_win *w, uint32_t button, bool down, int x,
                         int y, void *ud) {
    (void)w;
    struct nested_backend *nb = ud;
    struct xw_seat *s = nb_seat(nb);
    if (!s)
        return;
    /* xwc passes Wayland protocol button codes = linux button codes */
    if (button == 0x110 || button == 0x111 || button == 0x112) {
        xw_seat_pointer_button(s, button, down);
        (void)x;
        (void)y;
    }
}

static void nb_on_motion(struct xwc_win *w, int x, int y, void *ud) {
    (void)w;
    struct nested_backend *nb = ud;
    struct xw_seat *s = nb_seat(nb);
    if (s)
        xw_seat_pointer_motion(s, x + nb->output->x, y + nb->output->y);
}

static void nb_on_configure(struct xwc_win *w, int w_, int h_, void *ud) {
    (void)w;
    struct nested_backend *nb = ud;
    if (w_ < 16)
        w_ = 16;
    if (h_ < 16)
        h_ = 16;
    if (w_ == nb->last_conf_w && h_ == nb->last_conf_h)
        return;
    nb->last_conf_w = w_;
    nb->last_conf_h = h_;
    if (nb->output)
        xw_output_resize(nb->output, w_, h_);
}

static void nb_on_close(struct xwc_win *w, void *ud) {
    (void)w;
    nb_stop(ud, "parent closed the window");
}

/* ------------------------------------------------------- loop integration */

static int nb_on_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct nested_backend *nb = data;
    struct wl_display *d = nb->xwc.display;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        nb_stop(nb, "parent connection hangup");
        return 0;
    }
    /* Canonical integrated-loop pattern (libwayland requires the read
     * intent before wl_display_read_events; read_events decrements
     * reader_count unconditionally — calling it without prepare_read
     * corrupts the count to -1 and deadlocks on the reader futex):
     *   1. claim the intent (refused while events are pending to
     *      dispatch, so dispatch them in the loop)
     *   2. complete the read (the fd is readable per the event loop)
     *   3. dispatch what arrived (callbacks may queue requests)
     *   4. flush best-effort (EAGAIN just means the parent is slow) */
    while (wl_display_prepare_read(d) != 0) {
        if (wl_display_dispatch_pending(d) < 0) {
            nb_stop(nb, "parent dispatch error");
            return 0;
        }
    }
    if (wl_display_read_events(d) < 0) {
        nb_stop(nb, "parent protocol error");
        return 0;
    }
    if (wl_display_dispatch_pending(d) < 0) {
        nb_stop(nb, "parent dispatch error");
        return 0;
    }
    if (wl_display_flush(d) < 0 && errno != EAGAIN && errno != EINTR) {
        nb_stop(nb, "parent connection lost");
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------- backend ops */

static void nb_present(struct xw_backend *b, struct xw_output *o) {
    struct nested_backend *nb = (struct nested_backend *)b;

    int ww = 0, wh = 0;
    xwc_win_size(nb->win, &ww, &wh);
    if (ww != o->width * o->scale || wh != o->height * o->scale)
        return; /* window resize in flight; next repaint will match */

    int stride = 0;
    uint32_t *pix = xwc_win_pixels(nb->win, &stride);
    if (!pix)
        return;

    size_t n = (size_t)o->width * o->scale * (size_t)o->height * o->scale;
    memcpy(pix, o->native_data, n * 4);
    /* the window buffer is XRGB8888: the parent ignores the alpha byte
     * (by spec), so no per-pixel fixup is needed */
    xwc_win_commit(nb->win);
    wl_display_flush(nb->xwc.display);
}

static void nb_destroy(struct xw_backend *b) {
    struct nested_backend *nb = (struct nested_backend *)b;
    if (nb->client_src)
        wl_event_source_remove(nb->client_src);
    /* the output is freed by the generic backend teardown (it belongs
     * to the compositor's display, not to the parent connection) */
    nb->output = NULL;
    if (nb->win)
        xwc_win_destroy(nb->win);
    xwc_disconnect(&nb->xwc);
    free(nb);
}

static const struct xw_backend_ops nested_ops = {
    .present = nb_present,
    .destroy = nb_destroy,
};

/* ------------------------------------------------------------------ create */

struct xw_backend *xw_backend_nested_create(struct xw_compositor *c,
                                             const struct xw_compositor_config *cfg) {
    struct nested_backend *nb = calloc(1, sizeof(*nb));
    if (!nb)
        return NULL;
    nb->base.comp = c;
    nb->base.name = "nested";
    nb->base.ops = &nested_ops;

    /* 1. connect to the parent (blocking handshake; pumped if the parent
     *    is in-process for tests) */
    if (xwc_connect_pumped(&nb->xwc, cfg->parent_display, cfg->nested_pump,
                           cfg->nested_pump_ud) < 0) {
        xw_log(XW_LOG_ERROR, "nested: cannot connect to a parent compositor "
                             "(WAYLAND_DISPLAY=%s)",
               cfg->parent_display ? cfg->parent_display
                                   : "(default)");
        free(nb);
        return NULL;
    }

    /* 2. decide the window size: explicit -o spec wins, else the parent's
     *    output size, else 800x600 */
    int w = 800, h = 600;
    if (cfg->outputs && cfg->n_outputs > 0 && cfg->outputs[0].width > 0) {
        w = cfg->outputs[0].width;
        h = cfg->outputs[0].height;
    } else if (nb->xwc.output_w > 0) {
        w = nb->xwc.output_w;
        h = nb->xwc.output_h;
    }

    /* 3. the desktop window (callbacks carry nb as user data) */
    struct xwc_callbacks cb = nb_win_cb;
    cb.ud = nb;
    nb->win = xwc_win_create(&nb->xwc, &cb, "XFCE-Wayland (nested)",
                             "org.xfce.xw.nested", w, h);
    if (!nb->win) {
        xwc_disconnect(&nb->xwc);
        free(nb);
        return NULL;
    }
    /* the parent's first configure is authoritative (tiling WMs) */
    xwc_win_size(nb->win, &w, &h);

    /* 4. one output, mirroring the window */
    nb->output = xw_output_create(c, "NESTED-1", 0, 0, w, h, 1);
    if (!nb->output) {
        xwc_win_destroy(nb->win);
        xwc_disconnect(&nb->xwc);
        free(nb);
        return NULL;
    }
    nb->last_conf_w = w;
    nb->last_conf_h = h;

    /* 5. multiplex the parent connection on our event loop */
    nb->client_src = wl_event_loop_add_fd(
        c->loop, wl_display_get_fd(nb->xwc.display), WL_EVENT_READABLE,
        nb_on_readable, nb);
    if (!nb->client_src) {
        xw_output_destroy(nb->output);
        xwc_win_destroy(nb->win);
        xwc_disconnect(&nb->xwc);
        free(nb);
        return NULL;
    }

    /* 6. damage everything so the first frame reaches the parent */
    xw_output_damage_rect(nb->output, nb->output->x, nb->output->y,
                          nb->output->width, nb->output->height);
    xw_schedule_repaint(c);

    xw_log(XW_LOG_INFO, "nested backend: window %dx%d in parent %s", w, h,
           cfg->parent_display ? cfg->parent_display
                               : "(default WAYLAND_DISPLAY)");
    return &nb->base;
}
