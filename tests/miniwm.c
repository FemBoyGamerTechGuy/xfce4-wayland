/* miniwm.c — minimal reparenting window manager to reproduce the user's
 * nested-session environment under Xvfb.
 *
 * Behaves like a real WM (openbox/xfwm4 style):
 *   - grabs SubstructureRedirect on the root
 *   - on MapRequest: creates a decorative frame, reparents the client
 *     into it, RESIZES the client (as WMs do when honoring size hints
 *     or maximizing), then maps everything
 *   - honors "close" via WM_DELETE_WINDOW
 *
 * usage: miniwm <display> [client_w client_h]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: miniwm <display> [client_w client_h]\n");
        return 2;
    }
    Display *dpy = XOpenDisplay(argv[1]);
    if (!dpy) {
        printf("miniwm: cannot open display\n");
        return 2;
    }
    int force_w = argc > 2 ? atoi(argv[2]) : 0;
    int force_h = argc > 3 ? atoi(argv[3]) : 0;

    Window root = DefaultRootWindow(dpy);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);

    (void)XInternAtom(dpy, "WM_PROTOCOLS", False);
    (void)XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    printf("miniwm: managing root 0x%lx\n", (unsigned long)root);
    fflush(stdout);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case MapRequest: {
            XMapRequestEvent *m = &ev.xmaprequest;
            printf("miniwm: MapRequest for 0x%lx\n", (unsigned long)m->window);
            fflush(stdout);
            /* frame with a 1px border "decoration" */
            XWindowAttributes ca;
            if (!XGetWindowAttributes(dpy, m->window, &ca))
                break;
            int cw = force_w ? force_w : ca.width;
            int ch = force_h ? force_h : ca.height;
            Window frame = XCreateSimpleWindow(
                dpy, root, 40, 40, (unsigned)(cw + 2), (unsigned)(ch + 2), 1,
                WhitePixel(dpy, 0), WhitePixel(dpy, 0));
            XReparentWindow(dpy, m->window, frame, 1, 1);
            XResizeWindow(dpy, m->window, (unsigned)cw, (unsigned)ch);
            /* synthetic ConfigureNotify so the client learns its size */
            XEvent ce = {0};
            ce.type = ConfigureNotify;
            ce.xconfigure.event = m->window;
            ce.xconfigure.window = m->window;
            ce.xconfigure.x = 41;
            ce.xconfigure.y = 41;
            ce.xconfigure.width = cw;
            ce.xconfigure.height = ch;
            ce.xconfigure.border_width = 0;
            ce.xconfigure.above = None;
            ce.xconfigure.override_redirect = False;
            XSendEvent(dpy, m->window, False, StructureNotifyMask, &ce);
            XMapWindow(dpy, frame);
            XMapWindow(dpy, m->window);
            XSync(dpy, False);
            break;
        }
        case DestroyNotify:
            printf("miniwm: window 0x%lx destroyed\n",
                   (unsigned long)ev.xdestroywindow.window);
            fflush(stdout);
            break;
        default:
            break;
        }
    }
    XCloseDisplay(dpy);
    return 0;
}
