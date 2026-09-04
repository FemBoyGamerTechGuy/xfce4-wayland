/* test_core.c — lifecycle, windows, rendering, input, shortcuts. */
#include "xwtest.h"

#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ------------------------------------------------------------- lifecycle */

static void test_lifecycle(struct xwt_ctx *t) {
    XWT_ASSERT(t->comp != NULL);
    XWT_CHECK(xw_compositor_n_outputs(t->comp) == 1, "expected 1 output");
    int x, y, w, h, scale;
    XWT_CHECK(xw_compositor_output_info(t->comp, 0, &x, &y, &w, &h, &scale),
              "output 0 info");
    XWT_CHECK(w == 1280 && h == 720, "default output 1280x720, got %dx%d", w,
              h);
    XWT_CHECK(xw_compositor_workspace_count(t->comp) == 4,
              "default 4 workspaces");
    XWT_CHECK(xw_compositor_workspace_current(t->comp) == 0, "ws 0 active");
    /* clean stop */
    xw_compositor_stop(t->comp);
}

static void test_multi_output(struct xwt_ctx *t) {
    XWT_ASSERT(t->comp);
    /* (this test uses the default single output; multi-output layout is
     * exercised in a dedicated config below) */
    XWT_CHECK(xw_compositor_n_outputs(t->comp) >= 1, "outputs >= 1");
}

/* --------------------------------------------------------------- window */

static void test_window_map(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, 0xff3366cc, 300, 200, "First");
    XWT_ASSERT(win);
    bool ok = XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    XWT_CHECK(ok, "window count 1");
    char title[64];
    xw_compositor_focused_title(t->comp, title, sizeof(title));
    XWT_CHECK(strcmp(title, "First") == 0, "focused title 'First', got '%s'",
              title);
    /* window visible on current workspace */
    struct xw_wm *wm = t->comp->wm;
    struct xw_window *w = wl_container_of(wm->windows.next, w, link);
    XWT_CHECK(w->mapped, "window mapped");
    XWT_CHECK(w->w == 300 && w->h == 200, "window 300x200, got %dx%d", w->w,
              w->h);
    XWT_CHECK(w->x >= 0 && w->y >= 0 && w->x + w->w <= 1280 &&
                  w->y + w->h <= 720,
              "window inside output");
}

static bool server_window_ready(struct xwt_ctx *t) {
    struct xw_wm *wm = t->comp->wm;
    if (wl_list_empty(&wm->windows))
        return false;
    struct xw_window *w = wl_container_of(wm->windows.next, w, link);
    return w->mapped && w->surface && w->surface->shm;
}

static void frame_done_cb(void *data, struct wl_callback *cb,
                           uint32_t callback_data) {
    (void)callback_data;
    *(int *)data += 1;
    wl_callback_destroy(cb);
}


/* --------------------------------------------------------- geometry ---- */

/* THE native-window twin of the X11 geometry battery (2026-09-05):
 * the same three coordinate agreements for a native Wayland window —
 * hit-test == render rect, pointer events window-local, pixels at the
 * model rect — plus fullscreen pixel coverage and the frame-callback
 * liveness that every frame-clocked client (GTK's frame clock,
 * Xwayland's present path) depends on. */
static void test_geometry_native(struct xwt_ctx *t) {
    uint32_t color = 0xff9a3c2e;
    struct xwc_win *win = xwt_window_solid(t, color, 320, 220, "GeoN");
    XWT_ASSERT(win);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    struct xw_wm *wm = t->comp->wm;
    struct xw_window *w = wl_container_of(wm->windows.next, w, link);
    XWT_WAIT(t, w->mapped);

    /* hit-test: model rect center + 4 inner corners */
    int hits = 0;
    for (int i = 0; i < 5; i++) {
        int px = (i & 1) ? (w->x + w->w - 3) : (w->x + 2);
        int py = (i & 2) ? (w->y + w->h - 3) : (w->y + 2);
        if (i == 4) {
            px = w->x + w->w / 2;
            py = w->y + w->h / 2;
        }
        if (xw_wm_window_at(wm, px, py, NULL) == w)
            hits++;
    }
    XWT_CHECK(hits == 5, "hit-test covers the model rect: %d/5 at "
              "%dx%d+%d+%d", hits, w->w, w->h, w->x, w->y);

    /* pixels: bbox of the window color == model rect (no border for
     * native full-buffer fills) */
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    int pw = 0, ph = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
    XWT_ASSERT(pix);
    int x0 = pw, y0 = ph, x1 = -1, y1 = -1;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            if ((pix[y * pw + x] & 0xffffff) == (color & 0xffffff)) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    XWT_CHECK(x1 >= x0 && x0 == w->x && y0 == w->y &&
              x1 - x0 + 1 == w->w && y1 - y0 + 1 == w->h,
              "render bbox == model rect: (%d,%d %dx%d) vs model "
              "(%d,%d %dx%d)", x0, y0, x1 - x0 + 1, y1 - y0 + 1,
              w->x, w->y, w->w, w->h);

    /* pointer events are window-local: the toolkit's surface-local
     * position after injection at the model center must be the
     * model-relative point */
    t->client.ptr_x = -9999;
    t->client.ptr_y = -9999;
    int mx = w->x + w->w / 2, my = w->y + w->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, mx, my);
    XWT_WAIT(t, t->client.ptr_x != -9999);
    XWT_CHECK(t->client.ptr_x == mx - w->x && t->client.ptr_y == my - w->y,
              "pointer position is window-local: client (%d,%d), want "
              "(%d,%d)", t->client.ptr_x, t->client.ptr_y, mx - w->x,
              my - w->y);

    /* fullscreen: model == output rect AND pixels fill edge-to-edge
     * (the client redraws into the granted size; no gap may remain) */
    struct xw_output *o = w->output;
    xw_wm_fullscreen(wm, w, true);
    bool sized = false;
    for (int i = 0; i < 200 &&
                    !(sized = (w->w == o->width && w->h == o->height));
         i++) {
        xwt_pump(t);
        usleep(5 * 1000);
    }
    XWT_CHECK(sized, "fullscreen model == output rect");
    bool filled = false;
    for (int i = 0; i < 300 && !filled; i++) {
        xwt_pump(t);
        usleep(5 * 1000);
        pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
        int m = 2;
        filled = (pix[m * pw + m] & 0xffffff) == (color & 0xffffff) &&
                 (pix[m * pw + (pw - m - 1)] & 0xffffff) ==
                     (color & 0xffffff) &&
                 (pix[(ph - m - 1) * pw + m] & 0xffffff) ==
                     (color & 0xffffff) &&
                 (pix[(ph - m - 1) * pw + (pw - m - 1)] & 0xffffff) ==
                     (color & 0xffffff);
    }
    XWT_CHECK(filled, "fullscreen fills all four corners (no gap)");

    /* frame-callback liveness: a MAPPED toplevel's wl_callback MUST
     * fire on the next repaint. This is the native twin of the
     * Xwayland frozen-content bug: deliver_frame_callbacks skipped
     * unmapped surfaces and nothing ever set surface->mapped for
     * toplevels — every frame-clocked client presented one frame and
     * stopped. */
    static int frames_done;
    frames_done = 0;
    struct wl_surface *wsurf = xwc_win_surface(win);
    struct wl_callback *cb = wl_surface_frame(wsurf);
    static const struct wl_callback_listener fl = {
        .done = frame_done_cb,
    };
    wl_callback_add_listener(cb, &fl, &frames_done);
    wl_surface_commit(wsurf);
    XWT_WAIT(t, frames_done > 0);
    XWT_CHECK(frames_done > 0, "frame callback delivered on repaint");

    xw_wm_fullscreen(wm, w, false);
    for (int i = 0; i < 200; i++)
        xwt_pump(t);
}

static void test_render_pixels(struct xwt_ctx *t) {
    uint32_t color = 0xffcc6633;
    struct xwc_win *win = xwt_window_solid(t, color, 400, 300, "Render");
    XWT_ASSERT(win);
    if (!XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1))
        return;
    XWT_WAIT(t, server_window_ready(t));
    /* let the damage-driven repaint idle source run */
    xwt_pump(t);
    xwt_pump(t);
    xwt_pump(t);

    int pw = 0, ph = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &pw, &ph);
    XWT_ASSERT(pix);
    struct xw_wm *wm = t->comp->wm;
    struct xw_window *w = wl_container_of(wm->windows.next, w, link);
    /* sample the window center: composited pixel must equal the client
     * color (client draws straight color; renderer converts) */
    int cx = w->x + w->w / 2, cy = w->y + w->h / 2;
    uint32_t got = pix[cy * (pw) + cx];
    /* premultiplied ARGB from xwc_fill_rect: 0xffcc6633 straight →
     * XRGB8888 buffer keeps RGB, alpha forced opaque */
    XWT_CHECK((got & 0xffffff) == (color & 0xffffff),
              "center pixel 0x%06x, want 0x%06x", got & 0xffffff,
              color & 0xffffff);
    /* outside the window (bottom-right corner, clear of the cursor):
     * background color 0x202530 */
    uint32_t bg = pix[(ph - 10) * pw + (pw - 10)];
    XWT_CHECK((bg & 0xffffff) == 0x202530, "background pixel 0x%06x",
              bg & 0xffffff);
}

/* ----------------------------------------------------------- workspaces */

static void test_workspace_switch(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, 0xff3366cc, 200, 150, "WS");
    (void)win;
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);

    /* Ctrl+Alt+Right → workspace 1 (0-based) */
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, true);
    xw_compositor_inject_key(t->comp, K_LEFTALT, true);
    xw_compositor_inject_key(t->comp, K_RIGHT, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_RIGHT, false);
    xw_compositor_inject_key(t->comp, K_LEFTALT, false);
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, false);
    xwt_pump(t);
    XWT_CHECK(xw_compositor_workspace_current(t->comp) == 1, "ws 1 after "
              "Ctrl+Alt+Right, got %d",
              xw_compositor_workspace_current(t->comp));

    /* the window is not visible on ws 1 */
    struct xw_wm *wm = t->comp->wm;
    struct xw_window *w = wl_container_of(wm->windows.next, w, link);
    XWT_CHECK(!xw_wm_window_visible(wm, w), "window invisible on ws 1");

    /* Ctrl+F1 → workspace 0 */
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, true);
    xw_compositor_inject_key(t->comp, K_F1, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_F1, false);
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, false);
    xwt_pump(t);
    XWT_CHECK(xw_compositor_workspace_current(t->comp) == 0, "ws 0 after "
              "Ctrl+F1");
    XWT_CHECK(xw_wm_window_visible(wm, w), "window visible on ws 0");

    /* wrap-around: Ctrl+Alt+Left from ws 0 → ws 3 */
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, true);
    xw_compositor_inject_key(t->comp, K_LEFTALT, true);
    xw_compositor_inject_key(t->comp, K_LEFT, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_LEFT, false);
    xw_compositor_inject_key(t->comp, K_LEFTALT, false);
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, false);
    xwt_pump(t);
    XWT_CHECK(xw_compositor_workspace_current(t->comp) == 3,
              "wrap to ws 3, got %d", xw_compositor_workspace_current(t->comp));
}

/* ------------------------------------------------------------ shortcuts */

static int g_hook_actions[16];
static int g_hook_n;
static char g_hook_args[16][64];

static bool action_hook(int action, const char *arg, void *ud) {
    (void)ud;
    if (g_hook_n < 16) {
        g_hook_actions[g_hook_n] = action;
        snprintf(g_hook_args[g_hook_n], 64, "%s", arg ? arg : "");
        g_hook_n++;
    }
    return false; /* suppress built-in handlers */
}

static void test_shortcut_table(struct xwt_ctx *t) {
    xw_compositor_set_action_hook(t->comp, action_hook, NULL);
    g_hook_n = 0;

    /* Alt+F4 → close window: create one first */
    struct xwc_win *win = xwt_window_solid(t, 0xff3366cc, 200, 150, "Close");
    (void)win;
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    g_hook_n = 0;

    xw_compositor_inject_key(t->comp, K_LEFTALT, true);
    xw_compositor_inject_key(t->comp, K_F4, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_F4, false);
    xw_compositor_inject_key(t->comp, K_LEFTALT, false);
    xwt_pump(t);

    XWT_CHECK(g_hook_n >= 1, "Alt+F4 dispatched an action");
    if (g_hook_n >= 1)
        XWT_CHECK(g_hook_actions[0] == XW_ACTION_WINDOW_CLOSE,
                  "Alt+F4 → close (%d)", g_hook_actions[0]);
}

static void test_shortcut_release_suppression(struct xwt_ctx *t) {
    /* pressing Alt+F10 (maximize) must not deliver keys to the client */
    xw_compositor_set_action_hook(t->comp, action_hook, NULL);
    g_hook_n = 0;
    xw_compositor_inject_key(t->comp, K_LEFTALT, true);
    xw_compositor_inject_key(t->comp, K_F10, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_F10, false);
    xw_compositor_inject_key(t->comp, K_LEFTALT, false);
    xwt_pump(t);
    XWT_CHECK(g_hook_n == 1, "exactly one action for Alt+F10 (got %d)",
              g_hook_n);
    if (g_hook_n >= 1)
        XWT_CHECK(g_hook_actions[0] == XW_ACTION_WINDOW_MAXIMIZE_TOGGLE,
                  "Alt+F10 → maximize (%d)", g_hook_actions[0]);
}

static void test_shortcut_show_desktop(struct xwt_ctx *t) {
    xw_compositor_set_action_hook(t->comp, action_hook, NULL);
    g_hook_n = 0;
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, true);
    xw_compositor_inject_key(t->comp, K_LEFTALT, true);
    xw_compositor_inject_key(t->comp, K_D, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, K_D, false);
    xw_compositor_inject_key(t->comp, K_LEFTALT, false);
    xw_compositor_inject_key(t->comp, K_LEFTCTRL, false);
    xwt_pump(t);
    XWT_CHECK(g_hook_n == 1 && g_hook_actions[0] == XW_ACTION_SHOW_DESKTOP,
              "Ctrl+Alt+D → show desktop");
}

/* ---------------------------------------------------------------- input */

static void test_pointer_focus(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, 0xff3366cc, 300, 300, "Pointer");
    XWT_ASSERT(win);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    struct xw_wm *wm = t->comp->wm;
    struct xw_window *w = wl_container_of(wm->windows.next, w, link);

    int cx = w->x + w->w / 2, cy = w->y + w->h / 2;
    xw_compositor_inject_pointer_motion(t->comp, cx, cy);
    xwt_pump(t);
    XWT_CHECK(wm->focused == w, "click-to-focus target is the window");
    XWT_CHECK(t->comp->wm->focused != NULL, "pointer motion focuses window");

    /* click on the window */
    xw_compositor_inject_pointer_button(t->comp, 0x110 /*BTN_LEFT*/, true);
    xwt_pump(t);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    xwt_pump(t);
    XWT_CHECK(w->activated, "window activated after click");
}

/* every binding in the default table must dispatch to its action
 * through the real key pipeline (modifiers pressed as real keys) */
static uint32_t keycode_for_keysym(struct xw_seat *s, xkb_keysym_t sym) {
    for (uint32_t kc = 1; kc <= 255; kc++) {
        int nlevels = xkb_keymap_num_levels_for_key(s->keymap, kc + 8, 0);
        for (int lvl = 0; lvl < nlevels; lvl++) {
            const xkb_keysym_t *syms;
            int n = xkb_keymap_key_get_syms_by_level(s->keymap, kc + 8, 0,
                                                     lvl, &syms);
            for (int i = 0; i < n; i++)
                if (syms[i] == sym)
                    return kc;
        }
    }
    return 0;
}

static void test_all_default_shortcuts(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, 0xffcc3366, 200, 200, "Defs");
    XWT_ASSERT(win);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);

    struct xw_shortcuts *sc = t->comp->shortcuts;
    struct xw_seat *s = xw_seat_first(t->comp);
    XWT_ASSERT(s && sc);

    xw_compositor_set_action_hook(t->comp, action_hook, NULL);

    int total = 0, bad = 0, nosym = 0;
    struct xw_shortcut *b;
    wl_list_for_each(b, &sc->bindings, link) {
        /* keypad bindings fire with NumLock on (their keysyms live at
         * level 1, like on every real keyboard) */
        bool kp = strstr(b->binding_str, "KP_") != NULL;
        if (kp) {
            xw_compositor_inject_key(t->comp, K_NUMLOCK, true);
            xw_compositor_inject_key(t->comp, K_NUMLOCK, false);
            xwt_pump(t);
        }
        uint32_t kc = keycode_for_keysym(s, b->keysym);
        if (!kc) {
            /* keysym exists in the table but not the evdev keymap */
            nosym++;
            XWT_CHECK(false, "no keycode for binding '%s' (keysym 0x%x)",
                      b->binding_str, (unsigned)b->keysym);
            continue;
        }
        total++;

        /* press the binding's modifiers as real keys */
        bool shift = b->mods & (1u << sc->mod_shift);
        bool ctrl = b->mods & (1u << sc->mod_ctrl);
        bool alt = b->mods & (1u << sc->mod_alt);
        bool super_ = b->mods & (1u << sc->mod_super);
        if (shift)
            xw_compositor_inject_key(t->comp, K_LEFTSHIFT, true);
        if (ctrl)
            xw_compositor_inject_key(t->comp, K_LEFTCTRL, true);
        if (alt)
            xw_compositor_inject_key(t->comp, K_LEFTALT, true);
        if (super_)
            xw_compositor_inject_key(t->comp, K_LEFTMETA, true);

        g_hook_n = 0;
        xw_compositor_inject_key(t->comp, kc, true);
        xwt_pump(t);
        xw_compositor_inject_key(t->comp, kc, false);
        xwt_pump(t);

        if (super_)
            xw_compositor_inject_key(t->comp, K_LEFTMETA, false);
        if (alt)
            xw_compositor_inject_key(t->comp, K_LEFTALT, false);
        if (ctrl)
            xw_compositor_inject_key(t->comp, K_LEFTCTRL, false);
        if (shift)
            xw_compositor_inject_key(t->comp, K_LEFTSHIFT, false);
        xwt_pump(t);

        if (g_hook_n != 1 || g_hook_actions[0] != b->action) {
            bad++;
            XWT_CHECK(false, "binding '%s' (action %d) did not dispatch "
                      "(hook fired %d, first action %d)", b->binding_str,
                      b->action, g_hook_n,
                      g_hook_n > 0 ? g_hook_actions[0] : -1);
        }
        if (kp) { /* NumLock off again for the non-KP bindings */
            xw_compositor_inject_key(t->comp, K_NUMLOCK, true);
            xw_compositor_inject_key(t->comp, K_NUMLOCK, false);
            xwt_pump(t);
        }
    }
    XWT_CHECK(bad == 0 && total > 25,
              "default table: %d/%d bindings dispatch (%d unmapped)",
              total - bad, total, nosym);
    xw_compositor_set_action_hook(t->comp, NULL, NULL);
}

/* ---------------------------------------------------- fullscreen gap */

/* THE physical fullscreen-gap battery. The NVIDIA box still sees a
 * one-sided gap after the canonical-geometry round, so every layer of
 * the chain is pinned here headlessly, for BOTH stacks:
 *
 *   A. native xdg fullscreen on a NON-ZERO-ORIGIN output (the layout
 *      origin must never be assumed to be global (0,0)): the model
 *      rect must equal the output rect exactly, and the output's four
 *      corners + edge midpoints must carry window pixels.
 *   B. the XWayland-role leg, driven through the REAL protocols (the
 *      xwayland_surface role + window-control v3 set_fullscreen, the
 *      exact requests the X WM helper sends): the model goes
 *      fullscreen, the granted rect is mirrored back through the
 *      geometry event, and a stale client commit cannot shrink it.
 *   C. CSD geometry (set_window_geometry offset) + fullscreen: the
 *      render must still cover the granted rect (no gap via the
 *      buffer/geometry interaction).
 */
#include "xwayland-shell.h"
#include "xw-window-control-v1.h"
#include "xdg-shell.h"

/* do the output's 4 corners + 4 edge midpoints carry window pixels? */
static bool fs_pixels_cover(const uint32_t *pix, int pw, int ph,
                            uint32_t color) {
    color &= 0xffffff;
    int px[8] = {2, pw - 3, 2, pw - 3, pw / 2, pw / 2, 2, pw - 3};
    int py[8] = {2, 2, ph - 3, ph - 3, 2, ph - 3, ph / 2, ph / 2};
    for (int i = 0; i < 8; i++)
        if ((pix[py[i] * pw + px[i]] & 0xffffff) != color)
            return false;
    return true;
}

static bool fs_wait_cover(struct xwt_ctx *t, int out_index, uint32_t color) {
    for (int i = 0; i < 400; i++) {
        xwt_pump(t);
        usleep(5 * 1000);
        int pw = 0, ph = 0;
        const uint32_t *pix =
            xw_compositor_output_pixels(t->comp, out_index, &pw, &ph);
        if (pix && pw > 4 && ph > 4 && fs_pixels_cover(pix, pw, ph, color))
            return true;
    }
    return false;
}

/* -- the raw xwayland-role client (leg B) -- */
struct xr {
    struct wl_display *d;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xwayland_shell_v1 *shell;
    struct xw_window_control_manager_v1 *wcm;
    struct wl_surface *surf;
    struct xwayland_surface_v1 *xs;
    struct wl_buffer *buf;
    int n_geom;
    uint32_t gs_hi, gs_lo;
    int gx, gy, gw, gh;
};

static void xr_geom(void *data, struct xw_window_control_manager_v1 *m,
                    uint32_t hi, uint32_t lo, int32_t x, int32_t y,
                    int32_t width, int32_t height) {
    (void)m;
    struct xr *r = data;
    r->n_geom++;
    r->gs_hi = hi;
    r->gs_lo = lo;
    r->gx = x;
    r->gy = y;
    r->gw = width;
    r->gh = height;
}
static void xr_close(void *data, struct xw_window_control_manager_v1 *m,
                     uint32_t hi, uint32_t lo) {
    (void)data;
    (void)m;
    (void)hi;
    (void)lo;
}
static void xr_focus(void *data, struct xw_window_control_manager_v1 *m,
                     uint32_t hi, uint32_t lo) {
    (void)data;
    (void)m;
    (void)hi;
    (void)lo;
}
static const struct xw_window_control_manager_v1_listener xr_wcm = {
    .geometry = xr_geom,
    .close = xr_close,
    .focus = xr_focus,
};

static void xr_global(void *data, struct wl_registry *r, uint32_t name,
                      const char *iface, uint32_t version) {
    struct xr *x = data;
    if (strcmp(iface, "wl_compositor") == 0)
        x->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, "wl_shm") == 0)
        x->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(iface, "xwayland_shell_v1") == 0)
        x->shell = wl_registry_bind(r, name, &xwayland_shell_v1_interface, 1);
    else if (strcmp(iface, "xw_window_control_manager_v1") == 0) {
        if (version > 3)
            version = 3;
        x->wcm = wl_registry_bind(r, name,
                                  &xw_window_control_manager_v1_interface,
                                  version);
        xw_window_control_manager_v1_add_listener(x->wcm, &xr_wcm, x);
    }
}
static const struct wl_registry_listener xr_reg = {
    .global = xr_global,
    .global_remove = NULL,
};

static void xr_pump(struct xr *x, struct xwt_ctx *t);

static bool xr_connect(struct xr *x, struct xwt_ctx *t) {
    memset(x, 0, sizeof(*x));
    x->d = wl_display_connect(t->socket_name);
    if (!x->d)
        return false;
    x->reg = wl_display_get_registry(x->d);
    wl_registry_add_listener(x->reg, &xr_reg, x);
    wl_display_flush(x->d);
    /* NEVER a blocking roundtrip: the server is in-process and only
     * runs inside our pump */
    bool bound = false;
    for (int i = 0; i < 500 && !bound; i++) {
        xr_pump(x, t);
        bound = x->comp && x->shm && x->shell && x->wcm;
    }
    return bound;
}

/* non-blocking client pump, server-first (wl_display_dispatch would
 * BLOCK the test whenever the server has nothing to say, and the
 * in-process server only runs when WE run it) */
static void xr_pump(struct xr *x, struct xwt_ctx *t) {
    xw_compositor_dispatch(t->comp, 0);
    wl_display_flush(x->d);
    while (wl_display_prepare_read(x->d) != 0) {
        if (wl_display_dispatch_pending(x->d) < 0)
            return;
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(x->d), .events = POLLIN};
    poll(&pfd, 1, 0);
    if (pfd.revents & POLLIN) {
        if (wl_display_read_events(x->d) < 0)
            return;
    } else {
        wl_display_cancel_read(x->d);
    }
    if (wl_display_dispatch_pending(x->d) < 0)
        return;
    xw_compositor_dispatch(t->comp, 0);
}

static bool xr_map(struct xr *x, int w, int h, uint32_t color,
                   uint32_t serial_hi, uint32_t serial_lo) {
    x->surf = wl_compositor_create_surface(x->comp);
    x->xs = xwayland_shell_v1_get_xwayland_surface(x->shell, x->surf);
    xwayland_surface_v1_set_serial(x->xs, serial_lo, serial_hi);
    int fd = memfd_create("xr-buf", 0);
    if (fd < 0)
        return false;
    size_t len = (size_t)w * h * 4;
    if (ftruncate(fd, (off_t)len) < 0) {
        close(fd);
        return false;
    }
    uint32_t *pix = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pix == MAP_FAILED) {
        close(fd);
        return false;
    }
    for (int i = 0; i < w * h; i++)
        pix[i] = color;
    munmap(pix, len);
    struct wl_shm_pool *pool = wl_shm_create_pool(x->shm, fd, (int32_t)len);
    x->buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
                                       WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(x->surf, x->buf, 0, 0);
    wl_surface_damage(x->surf, 0, 0, w, h);
    wl_surface_commit(x->surf);
    wl_display_flush(x->d);
    return true;
}

static void xr_destroy(struct xr *x) {
    /* full teardown, child-most first: every proxy destroyed so
     * LeakSanitizer sees a clean connection (rc_destroy's contract) */
    if (x->buf)
        wl_buffer_destroy(x->buf);
    if (x->xs)
        xwayland_surface_v1_destroy(x->xs);
    if (x->surf)
        wl_surface_destroy(x->surf);
    if (x->wcm)
        xw_window_control_manager_v1_destroy(x->wcm);
    if (x->shell)
        xwayland_shell_v1_destroy(x->shell);
    if (x->shm)
        wl_shm_destroy(x->shm);
    if (x->comp)
        wl_compositor_destroy(x->comp);
    if (x->reg)
        wl_registry_destroy(x->reg);
    if (x->d)
        wl_display_disconnect(x->d);
    memset(x, 0, sizeof(*x));
}

static void test_geometry_fullscreen(struct xwt_ctx *t) {
    const uint32_t colorA = 0xff2d8f4e;
    const uint32_t colorB = 0xff8f2d4e;
    const int OX = 1000, OY = 200, OW = 640, OH = 480; /* output 2 */

    /* ---- A: native xdg fullscreen on a non-zero-origin output ---- */
    struct xw_output *o2 =
        xw_output_create(t->comp, "XW-T2", OX, OY, OW, OH, 1);
    XWT_ASSERT(o2);
    struct xwc_win *winA =
        xwt_window_solid(t, colorA, 300, 200, "FsNonZero");
    XWT_ASSERT(winA);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    struct xw_window *wA =
        wl_container_of(t->comp->wm->windows.next, wA, link);
    XWT_WAIT(t, wA->mapped);
    /* move the window into output 2's area and re-associate */
    t->comp->wm->focused = wA; /* keep it interactable after the move */
    xw_wm_damage_window(t->comp->wm, wA);
    wA->x = OX + 100;
    wA->y = OY + 100;
    xw_wm_update_window_output(t->comp->wm, wA);
    xw_wm_damage_window(t->comp->wm, wA);
    XWT_CHECK(wA->output == o2, "window associated with output 2");

    xw_wm_fullscreen(t->comp->wm, wA, true);
    for (int i = 0; i < 200 && !(wA->w == OW && wA->h == OH); i++)
        xwt_pump(t);
    XWT_CHECK(wA->x == OX && wA->y == OY && wA->w == OW && wA->h == OH,
              "fullscreen model == output 2 rect (non-zero origin): "
              "%d,%d %dx%d vs %d,%d %dx%d",
              wA->x, wA->y, wA->w, wA->h, OX, OY, OW, OH);
    XWT_CHECK(wA->output == o2, "fullscreen window stays on output 2");
    /* the output-2 pixels: 4 corners + 4 edge midpoints, no gap. The
     * composited frame of output 2 (index 1 in the outputs list). */
    XWT_CHECK(fs_wait_cover(t, 1, colorA),
              "fullscreen fills output 2 edge-to-edge (corners+edges, "
              "non-zero origin)");
    /* zero-width gap check on every side: the row/column just outside
     * the output edge must NOT show window pixels leaking, and every
     * pixel ON each edge must be window color (checked via the
     * corner/edge probes above). Restore: */
    xw_wm_fullscreen(t->comp->wm, wA, false);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    XWT_CHECK(wA->w == 300 && wA->h == 200,
              "unfullscreen restores the size (%dx%d)", wA->w, wA->h);
    xwc_win_destroy(winA);
    xw_output_destroy(o2);
    for (int i = 0; i < 20; i++)
        xwt_pump(t);

    /* ---- B: the XWayland role, real protocols, EWMH-style ---- */
    struct xr r;
    XWT_ASSERT(xr_connect(&r, t));
    const uint32_t S_HI = 0x11223344, S_LO = 0x55667788;
    XWT_ASSERT(xr_map(&r, 240, 160, colorB, S_HI, S_LO));
    struct xw_window *wB = NULL;
    for (int i = 0; i < 300 && !wB; i++) {
        xwt_pump(t);
        xr_pump(&r, t);
        wl_list_for_each(wB, &t->comp->wm->windows, link) {
            if (wB->surface && wB->surface->role == XW_SURFACE_ROLE_XWAYLAND)
                break;
            wB = NULL;
        }
    }
    XWT_ASSERT(wB);
    XWT_CHECK(wB->xw_has_serial && wB->xw_serial ==
              (((uint64_t)S_HI << 32) | S_LO),
              "serial associated");
    int restore_w = wB->w, restore_h = wB->h;

    /* the EWMH fullscreen request, exactly as the helper sends it */
    int geom_before = r.n_geom;
    xw_window_control_manager_v1_set_fullscreen(r.wcm, S_HI, S_LO, 1);
    wl_display_flush(r.d);
    bool fs = false;
    for (int i = 0; i < 300 && !fs; i++) {
        xwt_pump(t);
        xr_pump(&r, t);
        struct xw_output *ob = wB->output;
        fs = ob && wB->fullscreen && wB->x == ob->x && wB->y == ob->y &&
             wB->w == ob->width && wB->h == ob->height;
    }
    XWT_CHECK(fs, "X-role fullscreen model == its output rect "
                  "(%d,%d %dx%d)", wB->x, wB->y, wB->w, wB->h);
    /* the grant mirrors back through the geometry event (the X side
     * learns the fullscreen rect this way) */
    XWT_CHECK(r.n_geom > geom_before && r.gw == wB->w && r.gh == wB->h &&
              r.gx == wB->x && r.gy == wB->y,
              "geometry event mirrors the fullscreen grant "
              "(%d,%d %dx%d)", r.gx, r.gy, r.gw, r.gh);
    /* park the software cursor at the window center so its 12x17
     * sprite cannot cover a corner/edge probe (the default cursor
     * position is (0,0) — exactly the top-left probe) */
    xw_compositor_inject_pointer_motion(
        t->comp, wB->x + wB->w / 2, wB->y + wB->h / 2);
    for (int i = 0; i < 20; i++)
        xwt_pump(t);
    XWT_CHECK(fs_wait_cover(t, 0, colorB),
              "X-role fullscreen fills output 0 edge-to-edge");

    /* stale commit: the client re-commits its old 240x160 buffer —
     * the granted fullscreen rect must NOT be clobbered */
    wl_surface_attach(r.surf, r.buf, 0, 0);
    wl_surface_damage(r.surf, 0, 0, 240, 160);
    wl_surface_commit(r.surf);
    wl_display_flush(r.d);
    for (int i = 0; i < 100; i++) {
        xwt_pump(t);
        xr_pump(&r, t);
    }
    XWT_CHECK(wB->fullscreen && wB->w == wB->output->width &&
              wB->h == wB->output->height,
              "stale commit does not shrink the fullscreen rect "
              "(%dx%d)", wB->w, wB->h);

    /* leave fullscreen: restores, mirrors back */
    xw_window_control_manager_v1_set_fullscreen(r.wcm, S_HI, S_LO, 0);
    wl_display_flush(r.d);
    bool restored = false;
    for (int i = 0; i < 300 && !restored; i++) {
        xwt_pump(t);
        xr_pump(&r, t);
        restored = !wB->fullscreen && wB->w == restore_w &&
                   wB->h == restore_h;
    }
    XWT_CHECK(restored, "leave-fullscreen restores %dx%d (got %dx%d)",
              restore_w, restore_h, wB->w, wB->h);
    xr_destroy(&r);

    /* ---- C: CSD geometry + fullscreen (render must still cover) ---- */
    const uint32_t colorC = 0xff3355aa;
    struct xwc_win *winC =
        xwt_window_solid(t, colorC, 400, 300, "FsCsd");
    XWT_ASSERT(winC);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == 1);
    struct xw_window *wC =
        wl_container_of(t->comp->wm->windows.next, wC, link);
    XWT_WAIT(t, wC->mapped);
    struct xdg_surface *xsc = xwc_win_xdg_surface(winC);
    XWT_ASSERT(xsc);
    xdg_surface_set_window_geometry(xsc, 10, 14, 380, 280);
    for (int i = 0; i < 40; i++)
        xwt_pump(t);
    xw_wm_fullscreen(t->comp->wm, wC, true);
    struct xw_output *o0 = wC->output;
    for (int i = 0; i < 200 && !(wC->w == o0->width && wC->h == o0->height);
         i++)
        xwt_pump(t);
    /* re-commit with the CSD buffer (the geometry declaration rides
     * along): the state-held rect wins */
    xwc_win_commit(winC);
    for (int i = 0; i < 40; i++)
        xwt_pump(t);
    XWT_CHECK(wC->w == o0->width && wC->h == o0->height,
              "CSD fullscreen keeps the granted rect (%dx%d)", wC->w,
              wC->h);
    xw_compositor_inject_pointer_motion(
        t->comp, wC->x + wC->w / 2, wC->y + wC->h / 2);
    for (int i = 0; i < 20; i++)
        xwt_pump(t);
    XWT_CHECK(fs_wait_cover(t, 0, colorC),
              "CSD fullscreen fills the output edge-to-edge");
    xw_wm_fullscreen(t->comp->wm, wC, false);
    for (int i = 0; i < 100; i++)
        xwt_pump(t);
    xwc_win_destroy(winC);
}

/* ------------------------------------------------------------ registration */

static const struct xwt_test tests[] = {
    {"lifecycle", test_lifecycle},
    {"multi-output-info", test_multi_output},
    {"window-map", test_window_map},
    {"render-pixels", test_render_pixels},
    {"geometry-native", test_geometry_native},
    {"geometry-fullscreen", test_geometry_fullscreen},
    {"workspace-switch", test_workspace_switch},
    {"shortcut-close", test_shortcut_table},
    {"shortcut-suppression", test_shortcut_release_suppression},
    {"shortcut-all-defaults", test_all_default_shortcuts},
    {"shortcut-show-desktop", test_shortcut_show_desktop},
    {"pointer-focus", test_pointer_focus},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}

int main(void) { return xwt_run_all(); }
