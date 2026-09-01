/* xw-demo — a minimal xdg-shell toplevel client: a solid-colored
 * window with a title, drawn through libxwcl. Useful as the canonical
 * "normal application" for desktop testing (panel tasklist, stacking,
 * exclusive zone, window rules) — and as a template for real clients.
 *
 * Usage: xw-demo [--socket NAME] [COLOR [WIDTH HEIGHT [TITLE]]]
 *   COLOR: 0xRRGGBB or 0xAARRGGBB (default steel blue)
 */
#include "xwc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct demo {
    struct xwc c;
    struct xwc_win *win;
    uint32_t color;
};

static void draw(struct demo *d) {
    int w = 0, h = 0, stride = 0;
    xwc_win_size(d->win, &w, &h);
    uint32_t *pix = xwc_win_pixels(d->win, &stride);
    if (!pix || w < 1 || h < 1)
        return;
    xwc_fill_rect(pix, stride, w, h, 0, 0, w, h, d->color);
    xwc_draw_box(pix, stride, w, h, 8, 8, w - 16, h - 16, d->color,
                 0xffe6e6e6);
    xwc_draw_text(pix, stride, w, h, 16, 16, "xw-demo", 0xffffffff);
    xwc_win_commit(d->win);
}

static void on_configure(struct xwc_win *win, int w, int h, void *ud) {
    (void)w;
    (void)h;
    struct demo *d = ud;
    if (win)
        d->win = win;
    draw(d);
}

static void on_close(struct xwc_win *win, void *ud) {
    (void)win;
    struct demo *d = ud;
    d->c.running = false;
}

int main(int argc, char **argv) {
    const char *socket_name = NULL;
    uint32_t color = 0xff5e81ac;
    int w = 480, h = 320;
    const char *title = "Demo";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_name = argv[++i];
        } else if (argv[i][0] != '-' && i >= 1) {
            char *end = NULL;
            unsigned long v = strtoul(argv[i], &end, 0);
            if (end && *end == 0) {
                color = (uint32_t)v | 0xff000000u;
                if (i + 2 < argc) {
                    w = atoi(argv[i + 1]);
                    h = atoi(argv[i + 2]);
                    if (i + 3 < argc)
                        title = argv[i + 3];
                    i += 3;
                }
            } else {
                title = argv[i];
            }
        }
    }
    if (w < 50 || h < 50) {
        fprintf(stderr, "xw-demo: unreasonable size %dx%d\n", w, h);
        return 1;
    }

    struct demo d = {0};
    d.color = color;

    if (xwc_connect(&d.c, socket_name) < 0)
        return 1;

    struct xwc_callbacks cb = {
        .configure = on_configure,
        .close = on_close,
        .ud = &d,
    };
    d.win = xwc_win_create(&d.c, &cb, title, "org.xfce.xw.demo", w, h);
    if (!d.win) {
        fprintf(stderr, "xw-demo: window creation failed\n");
        xwc_disconnect(&d.c);
        return 1;
    }
    draw(&d);

    while (d.c.running) {
        if (xwc_dispatch(&d.c, 500) < 0)
            break; /* compositor went away */
    }

    xwc_win_destroy(d.win);
    xwc_disconnect(&d.c);
    return 0;
}
