/* x11client.c — a controllable real X11 client for the XWayland WM
 * regression tests (Xlib: this is TEST tooling, not compositor code;
 * the compositor itself still speaks no X11).
 *
 * Modes (argv): map [WxH] [flags...] | or X Y WxH
 *   map flags: nodelete (no WM_DELETE protocol), takefocus (WM_TAKE_FOCUS
 *   listed), noinput (WM_HINTS input=False), fullscreen (initial
 *   _NET_WM_STATE contains _NET_WM_STATE_FULLSCREEN — the map-time
 *   EWMH path)
 *
 * The client prints lifecycle events to stdout (line-buffered, every
 * line flushed) and then executes stdin commands, one per line:
 *   resize WxH | title NAME | move X Y | queryfocus | fullscreen |
 *   unfullscreen | state | wait | exit
 * Output lines the tests assert on:
 *   WINDOW 0x...     (the toplevel id, right after creation)
 *   MAPPED
 *   CONFIGURE WxH+X+Y   (ConfigureNotify, the granted geometry)
 *   DELETE           (WM_DELETE_WINDOW received — client exits itself)
 *   TAKEFOCUS        (WM_TAKE_FOCUS received)
 *   FOCUS 0x...      (XGetInputFocus result)
 *   STATE a,b,...    (_NET_WM_STATE atoms; "STATE (none)" when empty —
 *                     printed on demand AND on every PropertyNotify of
 *                     _NET_WM_STATE, so tests can see the WM sync it)
 *   EXIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdbool.h>

/* WM_DELETE_WINDOW / WM_TAKE_FOCUS are NOT predefined atoms — they are
 * interned by name (the same way the WM helper interns them) */
static Atom atom_wm_delete, atom_wm_take_focus;
static Atom atom_net_wm_state, atom_fs;

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void set_str_prop(Display *dpy, Window w, Atom prop, const char *val) {
    XChangeProperty(dpy, w, prop, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)val, (int)strlen(val));
}

/* print _NET_WM_STATE as comma-separated atom names — the EWMH state
 * the WM maintains; tests assert the fullscreen atom appears/disappears
 * exactly when the WM grants the transition */
static void report_state(Display *dpy, Window w) {
    Atom actual;
    int fmt;
    unsigned long n, bytes;
    unsigned char *raw = NULL;
    if (XGetWindowProperty(dpy, w, atom_net_wm_state, 0, 64, False,
                           XA_ATOM, &actual, &fmt, &n, &bytes, &raw)
            != Success) {
        say("STATE (get-failed)");
        return;
    }
    if (n == 0 || !raw) {
        say("STATE (none)");
        if (raw)
            XFree(raw);
        return;
    }
    char buf[512] = "";
    size_t off = 0;
    Atom *atoms = (Atom *)raw;
    for (unsigned long i = 0; i < n && off + 2 < sizeof(buf); i++) {
        char *nm = XGetAtomName(dpy, atoms[i]);
        int w = snprintf(buf + off, sizeof(buf) - off, "%s%s",
                         i ? "," : "", nm ? nm : "?");
        if (w > 0)
            off += (size_t)w;
        if (nm)
            XFree(nm);
    }
    XFree(raw);
    say("STATE %s", buf);
}

/* the EWMH runtime state change request — exactly what GTK/Qt/SDL apps
 * send: ClientMessage(_NET_WM_STATE) with l[0]=1 add / 0 remove / 2
 * toggle, l[1]=the state atom, l[3]=1 (source: application) */
static void send_state_msg(Display *dpy, Window w, long action) {
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = atom_net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = action;
    ev.xclient.data.l[1] = (long)atom_fs;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1;
    ev.xclient.data.l[4] = 0;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <display> map [WxH] [nodelete|takefocus|noinput]\n"
                "       %s <display> or X Y WxH\n",
                argv[0], argv[0]);
        return 2;
    }
    Display *dpy = XOpenDisplay(argv[1]);
    if (!dpy) {
        say("FAIL open %s", argv[1]);
        return 1;
    }
    atom_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    atom_wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    Window root = DefaultRootWindow(dpy);

    if (strcmp(argv[2], "or") == 0) {
        if (argc < 6) {
            say("FAIL or-args");
            return 2;
        }
        int x = atoi(argv[3]), y = atoi(argv[4]);
        char *end = NULL;
        int w = (int)strtol(argv[5], &end, 10);
        int h = (end && *end == 'x') ? (int)strtol(end + 1, NULL, 10) : 0;
        XSetWindowAttributes attrs;
        attrs.override_redirect = True;
        attrs.event_mask = StructureNotifyMask;
        Window win = XCreateWindow(
            dpy, root, x, y, (unsigned)w, (unsigned)h, 0, CopyFromParent,
            InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask,
            &attrs);
        XMapWindow(dpy, win);
        XFlush(dpy);
        say("WINDOW 0x%lx", (unsigned long)win);
        say("MAPPED");
        char line[128];
        while (fgets(line, sizeof(line), stdin)) {
            if (strncmp(line, "move ", 5) == 0) {
                int nx = atoi(line + 5);
                char *sp = strchr(line + 5, ' ');
                int ny = sp ? atoi(sp + 1) : 0;
                XMoveWindow(dpy, win, nx, ny);
            } else if (strncmp(line, "destroy", 7) == 0) {
                XDestroyWindow(dpy, win);
                XFlush(dpy);
                say("EXIT");
                break;
            } else if (strncmp(line, "exit", 4) == 0) {
                say("EXIT");
                break;
            }
            XFlush(dpy);
        }
        XCloseDisplay(dpy);
        return 0;
    }

    /* map mode */
    int w = 300, h = 200;
    if (argc > 3 && strchr(argv[3], 'x')) {
        w = atoi(argv[3]);
        char *x_ = strchr(argv[3], 'x');
        h = atoi(x_ + 1);
    }
    bool want_delete = true, want_takefocus = false, want_input = true;
    bool want_fs = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "nodelete") == 0)
            want_delete = false;
        if (strcmp(argv[i], "takefocus") == 0)
            want_takefocus = true;
        if (strcmp(argv[i], "noinput") == 0)
            want_input = false;
        if (strcmp(argv[i], "fullscreen") == 0)
            want_fs = true;
    }

    XSetWindowAttributes attrs;
    attrs.event_mask = StructureNotifyMask | PropertyChangeMask;
    Window win = XCreateWindow(dpy, root, 0, 0, (unsigned)w, (unsigned)h, 1,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWEventMask, &attrs);
    set_str_prop(dpy, win, XA_WM_NAME, "Probe Initial");
    set_str_prop(dpy, win, XA_WM_ICON_NAME, "Probe");
    XClassHint class = {.res_name = "probe", .res_class = "Probe"};
    XSetClassHint(dpy, win, &class);
    XSizeHints size = {.flags = 0};
    XSetWMNormalHints(dpy, win, &size);
    XWMHints hints = {.flags = InputHint | StateHint, .input = want_input,
                      .initial_state = NormalState};
    XSetWMHints(dpy, win, &hints);
    Atom protocols[2];
    int nprot = 0;
    if (want_delete)
        protocols[nprot++] = atom_wm_delete;
    if (want_takefocus)
        protocols[nprot++] = atom_wm_take_focus;
    if (nprot)
        XSetWMProtocols(dpy, win, protocols, nprot);
    if (want_fs) {
        /* map-time EWMH state: apps that start fullscreen (games,
         * players) set the property before mapping instead of sending
         * the runtime message */
        Atom st[1] = {atom_fs};
        XChangeProperty(dpy, win, atom_net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (const unsigned char *)st, 1);
    }

    XMapWindow(dpy, win);
    XFlush(dpy);
    say("WINDOW 0x%lx", (unsigned long)win);
    say("MAPPED");

    /* event + command loop: drain events between stdin reads */
    char line[128];
    bool done = false;
    while (!done) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ConfigureNotify) {
                say("CONFIGURE %dx%d+%d+%d", ev.xconfigure.width,
                    ev.xconfigure.height, ev.xconfigure.x, ev.xconfigure.y);
            } else if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == atom_wm_delete) {
                    say("DELETE");
                    XDestroyWindow(dpy, win);
                    XFlush(dpy);
                    say("EXIT");
                    return 0;
                } else if ((Atom)ev.xclient.data.l[0] == atom_wm_take_focus) {
                    say("TAKEFOCUS");
                }
            } else if (ev.type == PropertyNotify) {
                if ((Atom)ev.xproperty.atom == atom_net_wm_state &&
                    ev.xproperty.window == win) {
                    /* the WM re-synced the EWMH state property */
                    report_state(dpy, win);
                }
            } else if (ev.type == DestroyNotify) {
                say("DESTROYED");
                /* a real application exits when its main window is
                 * destroyed externally (WM kill path) — the process
                 * must not linger serving stdin after that */
                say("EXIT");
                return 0;
            }
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) == 1) {
            if (!fgets(line, sizeof(line), stdin))
                break;
            if (strncmp(line, "resize ", 7) == 0) {
                int nw = atoi(line + 7);
                char *x_ = strchr(line + 7, 'x');
                int nh = x_ ? atoi(x_ + 1) : 0;
                /* XResizeWindow: ConfigureRequest with CWWidth|CWHeight
                 * ONLY — the exact mask-only shape that the v0 parser
                 * misread (the 3x14 xterm bug) */
                XResizeWindow(dpy, win, (unsigned)nw, (unsigned)nh);
            } else if (strncmp(line, "title ", 6) == 0) {
                line[strcspn(line, "\n")] = 0;
                set_str_prop(dpy, win, XA_WM_NAME, line + 6);
            } else if (strncmp(line, "draw", 4) == 0) {
                /* paint the whole window + flush: forces Xwayland to
                 * re-attach a fresh damage-driven buffer (the probe
                 * normally never draws, which is exactly what makes it
                 * different from every real X11 client) */
                XGCValues gcv = {.foreground = 0x3366CC};
                GC gc = XCreateGC(dpy, win, GCForeground, &gcv);
                XFillRectangle(dpy, win, gc, 0, 0, (unsigned)w,
                               (unsigned)h);
                XFreeGC(dpy, gc);
                XFlush(dpy);
                say("DREW %dx%d", w, h);
            } else if (strncmp(line, "fullscreen", 10) == 0) {
                send_state_msg(dpy, win, 1 /* _NET_WM_STATE_ADD */);
            } else if (strncmp(line, "unfullscreen", 12) == 0) {
                send_state_msg(dpy, win, 0 /* _NET_WM_STATE_REMOVE */);
            } else if (strncmp(line, "state", 5) == 0) {
                report_state(dpy, win);
            } else if (strncmp(line, "queryfocus", 10) == 0) {
                Window focus = None;
                int revert = 0;
                XGetInputFocus(dpy, &focus, &revert);
                say("FOCUS 0x%lx", (unsigned long)focus);
            } else if (strncmp(line, "destroy", 7) == 0) {
                XDestroyWindow(dpy, win);
            } else if (strncmp(line, "exit", 4) == 0) {
                say("EXIT");
                done = true;
            } else if (strncmp(line, "wait", 4) == 0) {
                /* just drain events for a round */
            }
            XFlush(dpy);
        }
    }
    XCloseDisplay(dpy);
    return 0;
}
