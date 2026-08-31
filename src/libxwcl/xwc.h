/* xwc.h — libxwcl: minimal native client helper library for xw.
 *
 * Provides: connection + global binding, shm-backed xdg toplevel
 * windows, layer-shell surfaces, keyboard (client-side xkb) and pointer
 * event forwarding, and ARGB pixel drawing with the build-time bitmap
 * font. No toolkit, no GLib, no fontconfig.
 */
#ifndef XWC_H
#define XWC_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct xwc;
struct xwc_win;
struct xwc_layer;

/* ---------------------------------------------------------- callbacks */
struct xwc_callbacks {
    void (*key)(struct xwc_win *win, uint32_t keycode, bool down,
                xkb_keysym_t keysym, uint32_t mods, void *ud);
    void (*button)(struct xwc_win *win, uint32_t button, bool down, int x,
                   int y, void *ud);
    void (*motion)(struct xwc_win *win, int x, int y, void *ud);
    void (*configure)(struct xwc_win *win, int w, int h, void *ud);
    void (*close)(struct xwc_win *win, void *ud);
    void *ud;
};

/* -------------------------------------------------------- connection */
struct xwc {
    void *display;    /* struct wl_display * */
    void *registry;
    void *compositor;
    void *shm;
    void *seat;
    void *wm_base;
    void *layer_shell;
    void *ddm;
    int n_outputs;
    int output_w, output_h; /* first output logical size */

    /* client-side xkb (keyboard focus state) */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    void *keymap_mmap; /* mapping of the server keymap */
    size_t keymap_size;

    /* pump hook: lets an embedded (in-process) server run while the
     * client waits for a sync; NULL = plain blocking dispatch */
    void (*pump)(void *ud);
    void *pump_ud;

    void *keyboard;         /* wl_keyboard */
    void *pointer;          /* wl_pointer */
    void *output;           /* first wl_output */
    void *output_state;     /* pending output state (xwc-input.c) */

    void *focused_owner;  /* win or layer with keyboard focus */
    struct xwc_callbacks focused_cb; /* callback block of the focused owner */
    bool has_focus;
    int ptr_x, ptr_y;     /* last pointer position (surface-local) */
    bool running;
};

/* Connect to the compositor (NULL socket = $WAYLAND_DISPLAY). Blocks
 * until globals arrive. Returns 0 on success. */
int xwc_connect(struct xwc *c, const char *socket_name);
/* Same, but yields to an in-process compositor via pump while waiting
 * (used by the test harness). */
int xwc_connect_pumped(struct xwc *c, const char *socket_name,
                        void (*pump)(void *ud), void *pump_ud);
/* sync request + wait, yielding through the pump if set */
int xwc_sync(struct xwc *c);
void xwc_disconnect(struct xwc *c);
/* Dispatch pending events (timeout_ms < 0 = default). Returns 0, or -1
 * on connection error. */
int xwc_dispatch(struct xwc *c, int timeout_ms);
/* Flush the request buffer. */
void xwc_flush(struct xwc *c);

/* ------------------------------------------------------------ window */
struct xwc_win *xwc_win_create(struct xwc *c, const struct xwc_callbacks *cb,
                              const char *title, const char *app_id, int w,
                              int h);
void xwc_win_destroy(struct xwc_win *w);
/* pixels of the current back buffer (ARGB, stride = width) */
uint32_t *xwc_win_pixels(struct xwc_win *w, int *stride);
/* current buffer size */
void xwc_win_size(struct xwc_win *w, int *w_out, int *h_out);
/* attach + damage-everything + commit */
void xwc_win_commit(struct xwc_win *w);
void xwc_win_set_title(struct xwc_win *w, const char *title);
void xwc_win_close(struct xwc_win *w);
/* request a (un)maximize / (un)fullscreen from the client side */
void xwc_win_maximize(struct xwc_win *w, bool on);
void xwc_win_fullscreen(struct xwc_win *w, bool on);
void xwc_win_minimize(struct xwc_win *w);
/* xdg toplevel resource (for foreign-toplevel / activation requests) */
void *xwc_win_toplevel(struct xwc_win *w);
/* mapped state (configure + first commit done) */
bool xwc_win_mapped(struct xwc_win *w);

/* ------------------------------------------------------ layer surface */
struct xwc_layer *xwc_layer_create(struct xwc *c, const struct xwc_callbacks *cb,
                                  uint32_t layer, uint32_t anchors,
                                  int exclusive_zone, int w, int h);
/* 0 = none, 1 = exclusive, 2 = on-demand (must precede mapping) */
void xwc_layer_set_keyboard(struct xwc_layer *l, uint32_t mode);
void xwc_layer_destroy(struct xwc_layer *l);
uint32_t *xwc_layer_pixels(struct xwc_layer *l, int *stride);
void xwc_layer_commit(struct xwc_layer *l);
void xwc_layer_resize(struct xwc_layer *l, int w, int h);

/* ------------------------------------------------------------- drawing */
/* All drawing operates on ARGB8888 buffers; colors are 0xAARRGGBB with
 * straight alpha composited over the destination. stride is in pixels. */
void xwc_fill_rect(uint32_t *pix, int stride, int w, int h, int x, int y,
                   int rw, int rh, uint32_t color);
void xwc_draw_hline(uint32_t *pix, int stride, int w, int h, int x, int y,
                    int len, uint32_t color);
/* Draws text (ASCII 0x20-0x7e). Returns the new x position. */
int xwc_draw_text(uint32_t *pix, int stride, int w, int h, int x, int y,
                  const char *text, uint32_t color);
int xwc_text_width(const char *text);
#define XWC_LINE_H 19
#define XWC_ASCENT 15

/* roundrect outline+fill, a UI primitive used by all clients */
void xwc_draw_box(uint32_t *pix, int stride, int w, int h, int x, int y,
                  int bw, int bh, uint32_t fill, uint32_t border);

#endif /* XWC_H */
