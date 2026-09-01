/* xw-backend-x11.c — nested X11 backend.
 *
 * Runs the compositor as a top-level window inside an X11 (or XLibre)
 * server: the user's existing desktop. Same rationale as the nested
 * Wayland backend, for the (still common) case where the host session
 * is X11.
 *
 *  - one top-level X window per output; v0 maps exactly one output,
 *    sized by the initial request and resized on ConfigureNotify
 *  - presenting = XPutImage straight from the output's native buffer
 *    (a8r8g8b8 and 32bpp ZPixmap LSBFirst have identical byte layout,
 *    so the copy is zero-conversion; XShm is a future optimization)
 *  - input: X keycodes are evdev codes + 8 (the same convention XWayland
 *    and every X input driver use); buttons 1-3 map to the linux button
 *    codes of the Wayland protocol, 4/5 become scroll axes
 *  - detectable auto-repeat is enabled so held keys arrive as presses
 *    without synthetic release/release pairs that would confuse the
 *    xkb state machine
 *  - an invisible X cursor is installed: our software cursor (rendered
 *    into the frame) must be the only visible one
 *  - the X connection fd is multiplexed on the compositor's event loop
 */
#include "xw-internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

struct x11_backend {
    struct xw_backend base;

    Display *dpy;
    Window win;
    GC gc;
    XImage *img;          /* wraps the output's native buffer */
    Atom wm_protocols, wm_delete;
    Cursor null_cursor;

    struct xw_output *output; /* single output, 1:1 with the window */
    struct wl_event_source *xsrc; /* X fd on our loop */

    int win_w, win_h;
};

/* ------------------------------------------------------------- helpers */

static struct xw_seat *xb_seat(struct x11_backend *xb) {
    return xw_seat_first(xb->base.comp);
}

static void xb_stop(struct x11_backend *xb, const char *why) {
    xw_log(XW_LOG_INFO, "x11: stopping (%s)", why);
    if (xb->base.comp)
        xw_compositor_stop(xb->base.comp);
}

static void xb_invalidate_image(struct x11_backend *xb) {
    if (xb->img) {
        /* XDestroyImage frees the data pointer; ours is owned by the
         * output, so clear it before destroying */
        xb->img->data = NULL;
        XDestroyImage(xb->img);
        xb->img = NULL;
    }
}

/* (re)create the XImage wrapping the output's native buffer */
static void xb_update_image(struct x11_backend *xb) {
    struct xw_output *o = xb->output;
    if (!o)
        return;
    xb_invalidate_image(xb);
    int depth = DefaultDepth(xb->dpy, DefaultScreen(xb->dpy));
    if (depth < 24) {
        xw_log(XW_LOG_ERROR, "x11: display depth %d < 24 unsupported",
               depth);
        return;
    }
    /* 32bpp ZPixmap on a 24+ depth visual: byte layout B,G,R,X (LSBFirst)
     * — identical to our a8r8g8b8 buffers */
    Visual *vis = DefaultVisual(xb->dpy, DefaultScreen(xb->dpy));
    char *dummy = (char *)o->native_data; /* replaced per present */
    xb->img = XCreateImage(xb->dpy, vis, depth >= 32 ? 32 : 24, ZPixmap, 0,
                           dummy, o->width * o->scale, o->height * o->scale,
                           32, o->width * o->scale * 4);
    if (!xb->img)
        xw_log(XW_LOG_ERROR, "x11: XCreateImage failed");
}

/* ------------------------------------------------------------- input */

#define XW_BTN_LEFT   0x110
#define XW_BTN_RIGHT  0x111
#define XW_BTN_MIDDLE 0x112

static void xb_key(struct x11_backend *xb, unsigned int keycode, bool down) {
    struct xw_seat *s = xb_seat(xb);
    if (!s)
        return;
    if (keycode < 8 || keycode > 8 + 255)
        return;
    uint32_t linux_keycode = keycode - 8;
    xw_seat_key(s, linux_keycode, down);
}

static void xb_button(struct x11_backend *xb, unsigned int button, bool down) {
    struct xw_seat *s = xb_seat(xb);
    if (!s)
        return;
    switch (button) {
    case 1:
        xw_seat_pointer_button(s, XW_BTN_LEFT, down);
        break;
    case 2:
        xw_seat_pointer_button(s, XW_BTN_MIDDLE, down);
        break;
    case 3:
        xw_seat_pointer_button(s, XW_BTN_RIGHT, down);
        break;
    case 4: /* wheel up */
        if (down)
            xw_seat_pointer_axis(s, 0, +1.0);
        break;
    case 5: /* wheel down */
        if (down)
            xw_seat_pointer_axis(s, 0, -1.0);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------- X event pump */

static void xb_handle_event(struct x11_backend *xb, XEvent *ev) {
    switch (ev->type) {
    case KeyPress:
        xb_key(xb, ev->xkey.keycode, true);
        break;
    case KeyRelease:
        xb_key(xb, ev->xkey.keycode, false);
        break;
    case ButtonPress:
        xb_button(xb, ev->xbutton.button, true);
        break;
    case ButtonRelease:
        xb_button(xb, ev->xbutton.button, false);
        break;
    case MotionNotify: {
        struct xw_seat *s = xb_seat(xb);
        if (s)
            xw_seat_pointer_motion(s, ev->xmotion.x + xb->output->x,
                                   ev->xmotion.y + xb->output->y);
        break;
    }
    case Expose:
        /* X lost part of our pixels: recomposite */
        if (xb->output)
            xw_output_damage_rect(xb->output, xb->output->x, xb->output->y,
                                  xb->output->width, xb->output->height);
        break;
    case ConfigureNotify:
        if (ev->xconfigure.width != xb->win_w ||
            ev->xconfigure.height != xb->win_h) {
            xb->win_w = ev->xconfigure.width;
            xb->win_h = ev->xconfigure.height;
            if (xb->output) {
                xw_output_resize(xb->output, xb->win_w, xb->win_h);
                xb_update_image(xb);
            }
        }
        break;
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == xb->wm_delete)
            xb_stop(xb, "window manager closed the window");
        break;
    case DestroyNotify:
        xb_stop(xb, "window destroyed");
        break;
    default:
        break;
    }
}

static int xb_on_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct x11_backend *xb = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        xb_stop(xb, "X connection hangup");
        return 0;
    }
    /* Xlib buffers internally; XPending flushes requests and reads the
     * socket, so a poll wakeup is drained with this loop */
    while (XPending(xb->dpy)) {
        XEvent ev;
        XNextEvent(xb->dpy, &ev);
        xb_handle_event(xb, &ev);
    }
    XFlush(xb->dpy);
    return 0;
}

/* ------------------------------------------------------------- backend ops */

static void xb_present(struct xw_backend *b, struct xw_output *o) {
    struct x11_backend *xb = (struct x11_backend *)b;
    if (!xb->img || !xb->win)
        return;
    if (o->width * o->scale != xb->img->width ||
        o->height * o->scale != xb->img->height) {
        xb_update_image(xb);
        if (!xb->img)
            return;
    }
    xb->img->data = (char *)o->native_data;
    XPutImage(xb->dpy, xb->win, xb->gc, xb->img, 0, 0, 0, 0,
              (unsigned)o->width * o->scale, (unsigned)o->height * o->scale);
    XFlush(xb->dpy);
}

static void xb_destroy(struct xw_backend *b) {
    struct x11_backend *xb = (struct x11_backend *)b;
    if (xb->xsrc)
        wl_event_source_remove(xb->xsrc);
    xb_invalidate_image(xb);
    /* the output is freed by the generic backend teardown */
    xb->output = NULL;
    if (xb->win) {
        XUnmapWindow(xb->dpy, xb->win);
        XDestroyWindow(xb->dpy, xb->win);
    }
    if (xb->null_cursor)
        XFreeCursor(xb->dpy, xb->null_cursor);
    if (xb->gc)
        XFreeGC(xb->dpy, xb->gc);
    if (xb->dpy)
        XCloseDisplay(xb->dpy);
    free(xb);
}

static const struct xw_backend_ops x11_ops = {
    .present = xb_present,
    .destroy = xb_destroy,
};

/* ------------------------------------------------------------------ create */

struct xw_backend *xw_backend_x11_create(struct xw_compositor *c,
                                          const struct xw_compositor_config *cfg) {
    struct x11_backend *xb = calloc(1, sizeof(*xb));
    if (!xb)
        return NULL;
    xb->base.comp = c;
    xb->base.name = "x11";
    xb->base.ops = &x11_ops;

    xb->dpy = XOpenDisplay(cfg->parent_display);
    if (!xb->dpy) {
        xw_log(XW_LOG_ERROR, "x11: cannot open display %s",
               cfg->parent_display ? cfg->parent_display : "(default $DISPLAY)");
        free(xb);
        return NULL;
    }
    XSynchronize(xb->dpy, False);

    /* held keys: presses without synthetic releases (xkb-friendly) */
    Bool det = False;
    if (XkbQueryExtension(xb->dpy, NULL, NULL, NULL, NULL, NULL))
        XkbSetDetectableAutoRepeat(xb->dpy, True, &det);
    if (!det)
        xw_log(XW_LOG_WARN, "x11: no detectable auto-repeat; held keys may "
                            "produce release-release pairs");

    /* window size: -o spec, else half the X screen (fits everywhere) */
    int w = 0, h = 0;
    if (cfg->outputs && cfg->n_outputs > 0 && cfg->outputs[0].width > 0) {
        w = cfg->outputs[0].width;
        h = cfg->outputs[0].height;
    } else {
        w = DisplayWidth(xb->dpy, DefaultScreen(xb->dpy)) / 2;
        h = DisplayHeight(xb->dpy, DefaultScreen(xb->dpy)) / 2;
    }
    xb->win_w = w;
    xb->win_h = h;

    int scr = DefaultScreen(xb->dpy);
    xb->win = XCreateSimpleWindow(
        xb->dpy, RootWindow(xb->dpy, scr), 0, 0, (unsigned)w, (unsigned)h, 0,
        BlackPixel(xb->dpy, scr), BlackPixel(xb->dpy, scr));
    if (!xb->win)
        goto fail;
    XStoreName(xb->dpy, xb->win, "XFCE-Wayland (nested X11)");

    /* graceful close via WM_DELETE_WINDOW */
    xb->wm_protocols = XInternAtom(xb->dpy, "WM_PROTOCOLS", False);
    xb->wm_delete = XInternAtom(xb->dpy, "WM_DELETE_WINDOW", False);
    Atom protos[1] = {xb->wm_delete};
    XSetWMProtocols(xb->dpy, xb->win, protos, 1);

    XSelectInput(xb->dpy, xb->win,
                 KeyPressMask | KeyReleaseMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | ExposureMask |
                     StructureNotifyMask);

    /* invisible cursor: our software cursor is the visible one */
    {
        Pixmap pm = XCreatePixmap(xb->dpy, xb->win, 1, 1, 1);
        XColor black = {0};
        xb->null_cursor = XCreatePixmapCursor(xb->dpy, pm, pm, &black, &black, 0, 0);
        XFreePixmap(xb->dpy, pm);
        if (xb->null_cursor)
            XDefineCursor(xb->dpy, xb->win, xb->null_cursor);
    }

    xb->gc = XCreateGC(xb->dpy, xb->win, 0, NULL);
    if (!xb->gc)
        goto fail;

    /* one output mirroring the window */
    xb->output = xw_output_create(c, "X11-1", 0, 0, w, h, 1);
    if (!xb->output)
        goto fail;
    xb_update_image(xb);

    /* the X fd on our loop */
    xb->xsrc = wl_event_loop_add_fd(c->loop, ConnectionNumber(xb->dpy),
                                    WL_EVENT_READABLE, xb_on_readable, xb);
    if (!xb->xsrc)
        goto fail;

    XMapRaised(xb->dpy, xb->win);
    XFlush(xb->dpy);

    /* damage everything so the first frame is drawn */
    xw_output_damage_rect(xb->output, xb->output->x, xb->output->y,
                          xb->output->width, xb->output->height);
    xw_schedule_repaint(c);

    xw_log(XW_LOG_INFO, "x11 backend: window %dx%d on display %s", w, h,
           DisplayString(xb->dpy));
    return &xb->base;

fail:
    xw_log(XW_LOG_ERROR, "x11 backend init failed");
    xb_destroy(&xb->base);
    return NULL;
}
