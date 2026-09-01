/* fdtest2 — replicate the compositor's X11 backend create sequence
 * exactly, then run the loop and see whether X events arrive. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <wayland-server-core.h>

static Display *g_dpy;
static int g_got_cb = 0, g_got_events = 0;

static int on_x_readable(int fd, uint32_t mask, void *data) {
    (void)fd; (void)data;
    g_got_cb++;
    fprintf(stderr, "fdtest2: CALLBACK FIRED (mask=%u)\n", mask);
    while (XPending(g_dpy)) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        g_got_events++;
        fprintf(stderr, "fdtest2: event type=%d\n", ev.type);
    }
    XFlush(g_dpy);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: fdtest2 <display> [xkb] [gc] [cursor]\n"); return 2; }
    int use_xkb = argc > 2 && strcmp(argv[2], "xkb") == 0;
    struct wl_display *wd = wl_display_create();
    struct wl_event_loop *loop = wl_display_get_event_loop(wd);
    g_dpy = XOpenDisplay(argv[1]);
    if (!g_dpy) { printf("fdtest2: no display\n"); return 2; }
    fprintf(stderr, "fdtest2: X fd = %d\n", ConnectionNumber(g_dpy));

    if (use_xkb) {
        Bool det = False;
        XkbQueryExtension(g_dpy, NULL, NULL, NULL, NULL, NULL);
        XkbSetDetectableAutoRepeat(g_dpy, True, &det);
        fprintf(stderr, "fdtest2: xkb detectable=%d\n", det);
    }
    int scr = DefaultScreen(g_dpy);
    int w = DisplayWidth(g_dpy, scr) / 2, h = DisplayHeight(g_dpy, scr) / 2;
    Window win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, scr), 0, 0,
                                     (unsigned)w, (unsigned)h, 0,
                                     BlackPixel(g_dpy, scr), BlackPixel(g_dpy, scr));
    XStoreName(g_dpy, win, "fdtest2");
    XSetWMProtocols(
        g_dpy, win,
        (Atom[]){XInternAtom(g_dpy, "WM_DELETE_WINDOW", False)}, 1);
    XSelectInput(g_dpy, win, KeyPressMask | KeyReleaseMask | ButtonPressMask |
                    ButtonReleaseMask | PointerMotionMask | ExposureMask |
                    StructureNotifyMask);
    { Pixmap pm = XCreatePixmap(g_dpy, win, 1, 1, 1);
      XColor black = {0};
      Cursor cur = XCreatePixmapCursor(g_dpy, pm, pm, &black, &black, 0, 0);
      XFreePixmap(g_dpy, pm);
      if (cur) XDefineCursor(g_dpy, win, cur); }
    GC gc = XCreateGC(g_dpy, win, 0, NULL);
    (void)gc;

    struct wl_event_source *src = wl_event_loop_add_fd(
        loop, ConnectionNumber(g_dpy), WL_EVENT_READABLE, on_x_readable, NULL);
    XMapRaised(g_dpy, win);
    XFlush(g_dpy);
    fprintf(stderr, "fdtest2: mapped\n");
    if (argc > 3 && strcmp(argv[3], "present") == 0) {
        /* replicate the compositor's initial present: XPutImage+XFlush */
        XImage *im = XCreateImage(g_dpy, DefaultVisual(g_dpy, scr),
                                  24, ZPixmap, 0, calloc(1, 400 * 400 * 4),
                                  400, 400, 32, 1600);
        XPutImage(g_dpy, win, XCreateGC(g_dpy, win, 0, NULL), im, 0, 0, 0, 0,
                  400, 400);
        XFlush(g_dpy);
        fprintf(stderr, "fdtest2: initial present done\n");

    }
    fprintf(stderr, "fdtest2: loop for 4s\n");
    for (int i = 0; i < 16; i++) {
        wl_event_loop_dispatch(loop, 250);
        if (argc > 4 && strncmp(argv[4], "sync", 4) == 0 && i == 2) {
            fprintf(stderr, "fdtest2: delayed XSync round trip now\n");
            XSync(g_dpy, False);
            fprintf(stderr, "fdtest2: delayed XSync done; XPending=%d\n",
                    XPending(g_dpy));
            while (XPending(g_dpy)) {
                XEvent ev;
                XNextEvent(g_dpy, &ev);
                g_got_events++;
                fprintf(stderr, "fdtest2: [post-sync drain] event type=%d\n", ev.type);
            }
        }
        if (i == 3 || i == 15)
            fprintf(stderr, "fdtest2: cycle %d: cb=%d events=%d\n", i, g_got_cb, g_got_events);
    }
    fprintf(stderr, "fdtest2: DONE cb=%d events=%d\n", g_got_cb, g_got_events);
    wl_event_source_remove(src);
    wl_display_destroy(wd);
    XCloseDisplay(g_dpy);
    return g_got_events > 0 ? 0 : 1;
}
