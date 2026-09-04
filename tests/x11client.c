/* x11client.c — a controllable real X11 client for the XWayland WM
 * regression tests (Xlib: this is TEST tooling, not compositor code;
 * the compositor itself still speaks no X11).
 *
 * Modes (argv): map [WxH] [flags...] | or X Y WxH
 *   map flags: nodelete (no WM_DELETE protocol), takefocus (WM_TAKE_FOCUS
 *   listed), noinput (WM_HINTS input=False)
 *
 * The client prints lifecycle events to stdout (line-buffered, every
 * line flushed) and then executes stdin commands, one per line:
 *   resize WxH | title NAME | move X Y | queryfocus | wait | exit
 * Output lines the tests assert on:
 *   WINDOW 0x...     (the toplevel id, right after creation)
 *   MAPPED
 *   CONFIGURE WxH+X+Y   (ConfigureNotify, the granted geometry)
 *   DELETE           (WM_DELETE_WINDOW received — client exits itself)
 *   TAKEFOCUS        (WM_TAKE_FOCUS received)
 *   FOCUS 0x...      (XGetInputFocus result)
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
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "nodelete") == 0)
            want_delete = false;
        if (strcmp(argv[i], "takefocus") == 0)
            want_takefocus = true;
        if (strcmp(argv[i], "noinput") == 0)
            want_input = false;
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
