/* xw-exit — the graphical session-exit dialog (xfce4-session-logout
 * parity): Log Out, Restart Session, Suspend, Hibernate, Reboot,
 * Shutdown and Cancel, selectable with mouse or keyboard (arrows,
 * Enter, Escape, and per-button hotkeys).
 *
 * It is a layer-shell overlay (modal over the desktop, exclusive
 * keyboard) that sends the chosen command to the xw-session control
 * socket and exits. Rendered with libxwcl + the build-time bitmap
 * font. No toolkit.
 */
#include "xwc.h"
#include "xw-ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1.h"

enum {
    BTN_LOGOUT = 0,
    BTN_RESTART,
    BTN_SUSPEND,
    BTN_HIBERNATE,
    BTN_REBOOT,
    BTN_SHUTDOWN,
    BTN_CANCEL,
    N_BUTTONS,
};

static const struct {
    const char *label;
    const char *cmd; /* NULL = cancel */
    char hotkey;     /* alt-less single key */
} buttons[N_BUTTONS] = {
    {"Log Out", "logout", 'l'},
    {"Restart Session", "restart", 'r'},
    {"Suspend", "suspend", 's'},
    {"Hibernate", "hibernate", 'h'},
    {"Reboot", "reboot", 'b'},
    {"Shutdown", "shutdown", 'u'},
    {"Cancel", NULL, 'c'},
};

#define BTN_W 190
#define BTN_H 36
#define BTN_GAP 10
#define MARGIN 24
#define TITLE_H 46

/* keysyms we care about */
#define XK_Escape 0xff1b
#define XK_Return 0xff0d
#define XK_Left 0xff51
#define XK_Right 0xff53
#define XK_Up 0xff52
#define XK_Down 0xff54

struct exit_dialog {
    struct xwc c;
    struct xwc_layer *layer;
    int sel;
    bool done;
    char reply[256];
};

/* ------------------------------------------------------------- drawing */

static void draw(struct exit_dialog *d, struct xwc_layer *layer) {
    if (!layer)
        return;
    int stride = 0;
    uint32_t *pix = xwc_layer_pixels(layer, &stride);
    if (!pix)
        return;
    int w = 0, h = 0;
    /* layer size: query via pixels stride == width */
    w = stride;
    (void)h;

    /* modal backdrop: dark translucent panel */
    xwc_fill_rect(pix, stride, w, 720, 0, 0, w, 720, 0xd8222226);
    int pw = BTN_W * 2 + BTN_GAP + MARGIN * 2; /* two columns */
    int ph = TITLE_H + (BTN_H + BTN_GAP) * ((N_BUTTONS + 1) / 2) + MARGIN;
    int px = (w - pw) / 2, py = (720 - ph) / 2;
    xwc_draw_box(pix, stride, w, 720, px, py, pw, ph, 0xff2e3440,
                 0xff8fa4b8);

    /* title */
    int tx = px + (pw - xwc_text_width("End Session")) / 2;
    xwc_draw_text(pix, stride, w, 720, tx, py + 16, "End Session",
                  0xffe6e6e6);

    for (int i = 0; i < N_BUTTONS; i++) {
        int col = i % 2, row = i / 2;
        int bx = px + MARGIN + col * (BTN_W + BTN_GAP);
        int by = py + TITLE_H + row * (BTN_H + BTN_GAP);
        uint32_t fill = i == d->sel ? 0xff3584e4 : 0xff3b4252;
        uint32_t border = i == d->sel ? 0xffffffff : 0xff4c566a;
        xwc_draw_box(pix, stride, w, 720, bx, by, BTN_W, BTN_H, fill,
                     border);
        const char *label = buttons[i].label;
        int lx = bx + (BTN_W - xwc_text_width(label)) / 2;
        xwc_draw_text(pix, stride, w, 720, lx, by + 8, label, 0xffffffff);
        /* hotkey hint */
        char hint[3] = {buttons[i].hotkey, ')', 0};
        xwc_draw_text(pix, stride, w, 720, bx + 8, by + 8, hint, 0x88c0d0ff);
    }
    xwc_layer_commit(layer);
}

/* ------------------------------------------------------------- input */

static void on_key(struct xwc_win *win, uint32_t keycode, bool down,
                   xkb_keysym_t keysym, uint32_t mods, void *ud) {
    struct exit_dialog *d = ud;
    if (win && !d->layer)
        d->layer = (struct xwc_layer *)win;
    (void)keycode;
    (void)mods;
    if (!down)
        return;
    switch (keysym) {
    case XK_Escape:
        d->done = true;
        break;
    case XK_Return:
        if (buttons[d->sel].cmd)
            d->done = xw_ctl_send(buttons[d->sel].cmd, d->reply,
                                   sizeof(d->reply)) ||
                      true; /* done either way; reply carries the error */
        else
            d->done = true;
        break;
    case XK_Left:
    case XK_Up:
        d->sel = (d->sel + N_BUTTONS - 1) % N_BUTTONS;
        draw(d, d->layer);
        break;
    case XK_Right:
    case XK_Down:
        d->sel = (d->sel + 1) % N_BUTTONS;
        draw(d, d->layer);
        break;
    default:
        for (int i = 0; i < N_BUTTONS; i++) {
            if ((xkb_keysym_t)buttons[i].hotkey == keysym ||
                (keysym >= 'A' && keysym <= 'Z'
                     ? (xkb_keysym_t)(buttons[i].hotkey - 'a' + 'A')
                     : (xkb_keysym_t)0) == keysym) {
                d->sel = i;
                if (buttons[i].cmd)
                    d->done = xw_ctl_send(buttons[i].cmd, d->reply,
                                           sizeof(d->reply)) ||
                              true;
                else
                    d->done = true;
                return;
            }
        }
        break;
    }
}

static void on_button(struct xwc_win *win, uint32_t button, bool down, int x,
                      int y, void *ud) {
    struct exit_dialog *d = ud;
    if (win && !d->layer)
        d->layer = (struct xwc_layer *)win;
    if (!down || button != 0x110) /* left press */
        return;
    /* hit test against button rects */
    int w = 0, stride = 0;
    uint32_t *pix = xwc_layer_pixels(d->layer, &stride);
    (void)pix;
    w = stride;
    int pw = BTN_W * 2 + BTN_GAP + MARGIN * 2;
    int ph = TITLE_H + (BTN_H + BTN_GAP) * ((N_BUTTONS + 1) / 2) + MARGIN;
    int px = (w - pw) / 2, py = (720 - ph) / 2;
    for (int i = 0; i < N_BUTTONS; i++) {
        int col = i % 2, row = i / 2;
        int bx = px + MARGIN + col * (BTN_W + BTN_GAP);
        int by = py + TITLE_H + row * (BTN_H + BTN_GAP);
        if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) {
            if (buttons[i].cmd) {
                d->sel = i;
                d->done = xw_ctl_send(buttons[i].cmd, d->reply,
                                       sizeof(d->reply)) ||
                          true;
            } else {
                d->done = true;
            }
            return;
        }
    }
}

static void on_motion(struct xwc_win *win, int x, int y, void *ud) {
    struct exit_dialog *d = ud;
    if (win && !d->layer)
        d->layer = (struct xwc_layer *)win;
    int w = 0, stride = 0;
    uint32_t *pix = xwc_layer_pixels(d->layer, &stride);
    (void)pix;
    w = stride;
    int pw = BTN_W * 2 + BTN_GAP + MARGIN * 2;
    int ph = TITLE_H + (BTN_H + BTN_GAP) * ((N_BUTTONS + 1) / 2) + MARGIN;
    int px = (w - pw) / 2, py = (720 - ph) / 2;
    for (int i = 0; i < N_BUTTONS; i++) {
        int col = i % 2, row = i / 2;
        int bx = px + MARGIN + col * (BTN_W + BTN_GAP);
        int by = py + TITLE_H + row * (BTN_H + BTN_GAP);
        if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) {
            if (d->sel != i) {
                d->sel = i;
                draw(d, d->layer);
            }
            return;
        }
    }
}

static void on_configure(struct xwc_win *win, int w, int h, void *ud) {
    (void)w;
    (void)h;
    struct exit_dialog *d = ud;
    /* the configure callback fires during layer creation, before the
     * caller has stored the layer pointer — take it from the callback */
    if (win)
        d->layer = (struct xwc_layer *)win;
    draw(d, d->layer);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *socket_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socket_name = argv[++i];
    }
    struct exit_dialog d = {0};
    d.sel = BTN_LOGOUT;

    if (xwc_connect(&d.c, socket_name) < 0)
        return 1;

    struct xwc_callbacks cb = {
        .key = on_key,
        .button = on_button,
        .motion = on_motion,
        .configure = on_configure,
        .close = NULL,
        .ud = &d,
    };
    /* modal overlay: covers the output, takes keyboard; no exclusive
     * zone (a session-exit dialog must not shrink the usable area) */
    d.layer = xwc_layer_create(
        &d.c, &cb, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        0, 0, 0);
    if (!d.layer) {
        fprintf(stderr, "xw-exit: cannot create layer surface\n");
        xwc_disconnect(&d.c);
        return 1;
    }
    xwc_layer_set_keyboard(d.layer, 1); /* exclusive: it's modal */
    draw(&d, d.layer);

    while (!d.done && d.c.running) {
        if (xwc_dispatch(&d.c, 500) < 0)
            break;
    }

    xwc_layer_destroy(d.layer);
    xwc_disconnect(&d.c);

    if (d.reply[0] && strncmp(d.reply, "ok", 2) != 0) {
        fprintf(stderr, "xw-exit: %s\n", d.reply);
        return 1;
    }
    return 0;
}
