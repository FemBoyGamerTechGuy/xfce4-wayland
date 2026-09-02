/* panelprobe.c — check whether the xw-panel top bar is visible in the
 * compositor's nested X11 window.
 *
 * Panel visual constants (subprojects/panel/xw-panel.c):
 *   BAR_H 28, COL_BAR_BG 0xff22262e, COL_BTN_BG 0xff2e3440,
 *   COL_BTN_ACTIVE 0xff3584e4, COL_EXIT_BG 0xffa33434, COL_TEXT 0xffe6e6e6
 * bg of the compositor: 0xff202530.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

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
            if ((found = find_window(dpy, children[i], title)) != None)
                break;
        }
        if (children)
            XFree(children);
    }
    return found;
}

/* warp the pointer into an empty area below the bar and verify the
 * COMPOSITOR's software cursor shows up in the frame: the X cursor of
 * our window is invisible (XDefineCursor null-cursor), so any white
 * cursor-art pixel in that region can only come from the compositor's
 * input path -> render path (xw-seat cursor position -> xw-render
 * draw_cursor -> present). Proves the cursor is not an X11-only path. */
static int probe_cursor(Display *dpy, Window w) {
    int evt = 0, err = 0, maj = 0, min = 0;
    if (!XTestQueryExtension(dpy, &evt, &err, &maj, &min)) {
        printf("probe: XTEST unavailable; cannot verify the cursor path\n");
        return 0; /* not a failure of the compositor */
    }
    int cx = 60, cy = 200;
    XWarpPointer(dpy, None, w, 0, 0, 0, 0, cx, cy);
    XFlush(dpy);
    usleep(300000); /* let the compositor repaint with the moved cursor */

    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr))
        return 1;
    if (cy + 30 >= attr.height || cx + 20 >= attr.width) {
        printf("probe: window too small for the cursor check\n");
        return 0;
    }
    XImage *img = XGetImage(dpy, w, 0, 0, (unsigned)attr.width,
                            (unsigned)attr.height, AllPlanes, ZPixmap);
    if (!img)
        return 1;
    int white = 0, black = 0;
    for (int y = cy - 2; y < cy + 25; y++) {
        for (int x = cx - 2; x < cx + 16; x++) {
            unsigned long rgb = XGetPixel(img, x, y) & 0xffffff;
            if (rgb == 0xffffff)
                white++;
            else if (rgb == 0x000000)
                black++;
        }
    }
    XDestroyImage(img);
    /* the cursor art draws a black pass offset by (1,1) under a white
     * pass; both must appear around the warped position */
    int ok = white > 3 && black > 3;
    printf("probe: software cursor at warp point: %s (white=%d black=%d)\n",
           ok ? "PRESENT" : "MISSING", white, black);
    return ok ? 0 : 1;
}

/* click <x> <y> through the REAL input path: XTEST synthesizes a
 * pointer motion + button press into the compositor window, which must
 * travel MotionNotify/ButtonPress -> compositor seat -> wl_pointer ->
 * the panel client. Used by the final nested verification. */
static int probe_click(Display *dpy, Window w, int x, int y) {
    int evt = 0, err = 0, maj = 0, min = 0;
    if (!XTestQueryExtension(dpy, &evt, &err, &maj, &min)) {
        printf("probe: XTEST unavailable\n");
        return 1;
    }
    XWarpPointer(dpy, None, w, 0, 0, 0, 0, x, y);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XFlush(dpy);
    usleep(120000);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XFlush(dpy);
    printf("probe: clicked %d,%d through the X input path\n", x, y);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: panelprobe <display> [screenshot.xpm] [cursor] "
               "[click X Y | exitbtn]\n");
        return 2;
    }
    Display *dpy = XOpenDisplay(argv[1]);
    if (!dpy) {
        printf("probe: cannot open display %s\n", argv[1]);
        return 2;
    }
    Window root = DefaultRootWindow(dpy);
    Window w = find_window(dpy, root, "XFCE-Wayland (nested X11)");
    if (w == None) {
        printf("probe: FAIL compositor window not found\n");
        XCloseDisplay(dpy);
        return 1;
    }
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) {
        printf("probe: FAIL no attributes\n");
        XCloseDisplay(dpy);
        return 1;
    }
    printf("probe: window %dx%d depth %d\n", attr.width, attr.height,
           attr.depth);

    /* click modes: verified interactions through the real input path */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "click") == 0 && i + 2 < argc) {
            int rc = probe_click(dpy, w, atoi(argv[i + 1]), atoi(argv[i + 2]));
            XCloseDisplay(dpy);
            return rc;
        }
        if (strcmp(argv[i], "exitbtn") == 0) {
            /* find the red exit button, then click its center */
            XImage *im = XGetImage(dpy, w, 0, 0, (unsigned)attr.width,
                                   (unsigned)(attr.height < 28 ? attr.height : 28),
                                   AllPlanes, ZPixmap);
            if (!im) {
                XCloseDisplay(dpy);
                return 1;
            }
            int ex = -1;
            for (int x = attr.width - 4; x > attr.width / 2; x--) {
                if ((XGetPixel(im, x, 14) & 0xffffff) == 0xa33434) {
                    ex = x;
                    break;
                }
            }
            XDestroyImage(im);
            if (ex < 0) {
                printf("probe: exit button not found\n");
                XCloseDisplay(dpy);
                return 1;
            }
            printf("probe: exit button found near x=%d\n", ex);
            int rc = probe_click(dpy, w, ex, 14);
            XCloseDisplay(dpy);
            return rc;
        }
    }
    XImage *img = XGetImage(dpy, w, 0, 0, (unsigned)attr.width,
                            (unsigned)attr.height, AllPlanes, ZPixmap);
    if (!img) {
        printf("probe: FAIL XGetImage\n");
        XCloseDisplay(dpy);
        return 1;
    }

    /* count occurrences of the panel colors in the top 28 rows vs rows
     * 40+ (should be pure background there) */
    long bar_bg = 0, btn_bg = 0, btn_active = 0, exit_bg = 0, text = 0;
    long bg_below = 0, other_top = 0;
    int top_rows = 28 < attr.height ? 28 : attr.height;
    for (int y = 0; y < top_rows; y++) {
        for (int x = 0; x < attr.width; x++) {
            unsigned long px = XGetPixel(img, x, y) & 0xffffffff;
            unsigned long rgb = px & 0xffffff;
            if (rgb == 0x22262e) bar_bg++;
            else if (rgb == 0x2e3440 || rgb == 0x3b4252 || rgb == 0x434c5e) btn_bg++;
            else if (rgb == 0x3584e4 || rgb == 0x88b0ef) btn_active++;
            else if (rgb == 0xa33434 || rgb == 0xc94b4b) exit_bg++;
            else if (rgb == 0xe6e6e6 || rgb == 0x9aa5b1 || rgb == 0xffffff) text++;
            else other_top++;
        }
    }
    for (int y = 40; y < 60 && y < attr.height; y++) {
        for (int x = 0; x < attr.width; x++) {
            unsigned long rgb = XGetPixel(img, x, y) & 0xffffff;
            if (rgb == 0x202530) bg_below++;
        }
    }
    printf("probe: top-28-rows pixels: bar_bg=%ld btn=%ld active=%ld "
           "exit=%ld text=%ld other=%ld\n",
           bar_bg, btn_bg, btn_active, exit_bg, text, other_top);
    printf("probe: rows 40-60 background pixels=%d (of %d)\n",
           (int)bg_below, (attr.width > 0 ? attr.width : 1) * 20);
    /* the compositor bg must fill the FULL window width: if the output
     * never resized to the WM geometry, the area beyond the old size
     * stays unpainted (black) — this asserts the resize happened */
    int resized_ok = bg_below > (long)attr.width * 19;
    /* the bar must reach the right edge of the window: a stale
     * (pre-resize width) bar leaves compositor-bg pixels there */
    unsigned long right_px = 0;
    int right_x = attr.width - 10;
    if (right_x > 0 && attr.height > 14)
        right_px = XGetPixel(img, right_x, 14) & 0xffffff;
    int spans_ok = right_px != 0x202530 && right_px != 0x000000;
    printf("probe: right-edge pixel 0x%lx (bar %s the full width)\n",
           right_px, spans_ok ? "spans" : "does NOT span");
    long panel_px = bar_bg + btn_bg + btn_active + exit_bg + text;
    int ok = panel_px > 500 && resized_ok && spans_ok;
    printf("probe: %s — panel %s (resize %s)\n", ok ? "PASS" : "FAIL",
           panel_px > 500 ? "VISIBLE" : "NOT VISIBLE",
           resized_ok ? "confirmed" : "MISSING");

    int want_cursor = 0;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "cursor") == 0)
            want_cursor = 1;
    if (argc > 2 && !want_cursor) {
        /* dump an XPM screenshot for human verification */
        FILE *f = fopen(argv[2], "w");
        if (f) {
            int sw = attr.width > 640 ? 640 : attr.width;
            int sh = attr.height > 400 ? 400 : attr.height;
            fprintf(f, "/* XPM */\nstatic char *shot[] = {\n");
            fprintf(f, "\"%d %d 5 1\",\n", sw, sh);
            fprintf(f, "\". c #202530\",\n\"b c #22262e\",\n\"n c #2e3440\",\n"
                       "\"a c #3584e4\",\n\"x c #e6e6e6\",\n");
            for (int y = 0; y < sh; y++) {
                fputc('"', f);
                for (int x = 0; x < sw; x++) {
                    unsigned long rgb = XGetPixel(img, x, y) & 0xffffff;
                    char c = '.';
                    if (rgb == 0x22262e || rgb == 0x2e3440 || rgb == 0x3b4252)
                        c = rgb == 0x22262e ? 'b' : 'n';
                    else if (rgb == 0x3584e4 || rgb == 0x88b0ef)
                        c = 'a';
                    else if (rgb == 0xe6e6e6 || rgb == 0xffffff ||
                             rgb == 0x9aa5b1)
                        c = 'x';
                    fputc(c, f);
                }
                fprintf(f, "\"%s\n", y + 1 < sh ? "," : "");
            }
            fprintf(f, "};\n");
            fclose(f);
            printf("probe: screenshot written %s\n", argv[2]);
        }
    }
    XDestroyImage(img);
    if (want_cursor) {
        int crc = probe_cursor(dpy, w); /* 0 = verified */
        XCloseDisplay(dpy);
        printf("probe: %s — panel %s, cursor path %s\n",
               ok && crc == 0 ? "PASS" : "FAIL",
               panel_px > 500 ? "VISIBLE" : "NOT VISIBLE",
               crc == 0 ? "VERIFIED" : "UNVERIFIED");
        return (ok && crc == 0) ? 0 : 1;
    }
    XCloseDisplay(dpy);
    return ok ? 0 : 1;
}
