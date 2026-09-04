/* kbddriver.c — X-side keyboard driver for the physical-keyboard
 * regression: synthesizes the report matrix as X KeyPress/KeyRelease
 * events (XSendEvent — libX11 only, no XTest dependency) into the
 * nested-X11 compositor window, exactly as a physical keyboard would
 * surface through the X keycode space (X keycode = evdev + 8).
 *
 * Used by scripts/test-physical-kbd.sh together with
 * build/tests/keyboardprobe: the probe (a raw wl_keyboard client)
 * must observe the exact wire events for every key of the matrix.
 *
 * Usage: kbddriver <window-title>
 * Exit code: 0 = all events sent, 1 = window not found.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

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
            /* recurse: a reparenting WM may nest it */
            if ((found = find_window(dpy, children[i], title)) != None)
                break;
        }
        if (children)
            XFree(children);
    }
    return found;
}

static void send_key(Display *dpy, Window w, unsigned int keycode, bool down) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xkey.type = down ? KeyPress : KeyRelease;
    ev.xkey.display = dpy;
    ev.xkey.window = w;
    ev.xkey.root = DefaultRootWindow(dpy);
    ev.xkey.subwindow = None;
    ev.xkey.time = CurrentTime;
    ev.xkey.x = ev.xkey.y = ev.xkey.x_root = ev.xkey.y_root = 1;
    ev.xkey.state = 0;
    ev.xkey.keycode = keycode;
    ev.xkey.same_screen = True;
    XSendEvent(dpy, w, True,
               down ? KeyPressMask : KeyReleaseMask, &ev);
    XFlush(dpy);
}

/* X keycodes are evdev + 8, the same convention XWayland uses */
#define X_BKSP  (14 + 8)
#define X_U     (22 + 8)
#define X_A     (30 + 8)
#define X_LCTRL (29 + 8)
#define X_LSHIFT (42 + 8)
#define X_LALT  (56 + 8)

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <window-title>\n", argv[0]);
        return 2;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "kbddriver: cannot open display\n");
        return 3;
    }
    Window target = find_window(dpy, DefaultRootWindow(dpy), argv[1]);
    if (target == None) {
        fprintf(stderr, "kbddriver: window '%s' not found\n", argv[1]);
        return 1;
    }
    printf("kbddriver: target window 0x%lx\n", (unsigned long)target);
    fflush(stdout);

    /* the report matrix, in the same order the in-process
     * seat-keyboard-matrix test asserts it */
    send_key(dpy, target, X_BKSP, true);
    send_key(dpy, target, X_BKSP, false);
    send_key(dpy, target, X_U, true);
    send_key(dpy, target, X_U, false);
    send_key(dpy, target, X_A, true);
    send_key(dpy, target, X_A, false);
    send_key(dpy, target, X_LSHIFT, true);
    send_key(dpy, target, X_BKSP, true);
    send_key(dpy, target, X_BKSP, false);
    send_key(dpy, target, X_LSHIFT, false);
    send_key(dpy, target, X_LCTRL, true);
    send_key(dpy, target, X_BKSP, true);
    send_key(dpy, target, X_BKSP, false);
    send_key(dpy, target, X_LCTRL, false);
    send_key(dpy, target, X_LALT, true);
    send_key(dpy, target, X_LALT, false);
    XFlush(dpy);
    XCloseDisplay(dpy);
    printf("kbddriver: 16 events sent\n");
    return 0;
}
