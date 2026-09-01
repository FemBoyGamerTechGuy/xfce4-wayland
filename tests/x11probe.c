/* x11probe.c — X11 nested-backend process test helper.
 *
 * Runs against a live Xvfb (or any X server) where an xw-compositor
 * --backend x11 window lives. Verifies:
 *   1. the compositor window exists (by title)
 *   2. its pixels actually carry the compositor's framebuffer content
 *      (background color at the window center)
 *   3. XTEST-synthesized keyboard input reaches the compositor's
 *      shortcut engine (the dispatch INFO log is checked by the
 *      driving shell script)
 *
 * Build-time linkage: libX11 + libXtst (sysroot).
 * Exit code: 0 = all checks passed, 1 = failure (message on stdout).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

/* linux keycodes used by the injected shortcut (Ctrl+Alt+D →
 * show-desktop in the default table); X keycodes are evdev + 8 */
#define KEY_LEFTCTRL 29
#define KEY_LEFTALT  56
#define KEY_D        32

static Window find_window(Display *dpy, Window root, const char *title) {
    Window parent, *children = NULL;
    unsigned int nchildren = 0;
    Window found = None;
    if (XQueryTree(dpy, root, &root, &parent, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            char *name = NULL;
            if (XFetchName(dpy, children[i], &name) && name) {
                if (strcmp(name, title) == 0) {
                    found = children[i];
                    XFree(name);
                    break;
                }
                XFree(name);
            }
            /* recurse into subwindows (WM reparenting) */
            if ((found = find_window(dpy, children[i], title)) != None)
                break;
        }
        if (children)
            XFree(children);
    }
    return found;
}

static int probe_bg(Display *dpy, Window w) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr))
        return 1;
    XImage *img = XGetImage(dpy, w, 0, 0, (unsigned)attr.width,
                            (unsigned)attr.height, AllPlanes, ZPixmap);
    if (!img)
        return 1;
    if (img->depth < 24) {
        printf("probe: window depth %d < 24 unsupported\n", img->depth);
        XDestroyImage(img);
        return 1;
    }
    /* center pixel; a8r8g8b8 0xff202530 → X pixel 0x00202530 */
    unsigned long px =
        XGetPixel(img, attr.width / 2, attr.height / 2);
    XDestroyImage(img);
    if ((px & 0xffffff) != 0x202530) {
        printf("probe: center pixel 0x%lx, want 0x202530 (bg)\n", px);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: x11probe <display> [inject]\n");
        return 1;
    }
    Display *dpy = XOpenDisplay(argv[1]);
    if (!dpy) {
        printf("probe: cannot open display %s\n", argv[1]);
        return 1;
    }
    Window root = DefaultRootWindow(dpy);
    Window w = find_window(dpy, root, "XFCE-Wayland (nested X11)");
    if (w == None) {
        printf("probe: compositor window not found\n");
        XCloseDisplay(dpy);
        return 1;
    }

    if (probe_bg(dpy, w) != 0) {
        XCloseDisplay(dpy);
        return 1;
    }
    printf("probe: background pixels OK\n");

    if (argc > 2 && strcmp(argv[2], "inject") == 0) {
        int evt_base = 0, err_base = 0, major = 0, minor = 0;
        if (!XTestQueryExtension(dpy, &evt_base, &err_base, &major, &minor)) {
            printf("probe: XTEST unavailable on this server\n");
            XCloseDisplay(dpy);
            return 1;
        }
        /* the compositor window must be focused to receive keys:
         * XTEST input focus follows the server-side focus, so raise
         * and set input focus first */
        XRaiseWindow(dpy, w);
        XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
        XWarpPointer(dpy, None, w, 0, 0, 0, 0, 10, 10);
        XFlush(dpy);
        usleep(100000);

        XTestFakeKeyEvent(dpy, KEY_LEFTCTRL + 8, True, CurrentTime);
        XTestFakeKeyEvent(dpy, KEY_LEFTALT + 8, True, CurrentTime);
        XTestFakeKeyEvent(dpy, KEY_D + 8, True, CurrentTime);
        XFlush(dpy);
        usleep(200000);
        XTestFakeKeyEvent(dpy, KEY_D + 8, False, CurrentTime);
        XTestFakeKeyEvent(dpy, KEY_LEFTALT + 8, False, CurrentTime);
        XTestFakeKeyEvent(dpy, KEY_LEFTCTRL + 8, False, CurrentTime);
        XFlush(dpy);
        printf("probe: injected Ctrl+Alt+D via XTEST\n");
    }
    XCloseDisplay(dpy);
    return 0;
}
