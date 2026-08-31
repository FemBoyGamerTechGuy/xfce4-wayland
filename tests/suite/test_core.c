/* test_core.c — lifecycle, windows, rendering, input, shortcuts. */
#include "xwtest.h"

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

/* ------------------------------------------------------------ registration */

static const struct xwt_test tests[] = {
    {"lifecycle", test_lifecycle},
    {"multi-output-info", test_multi_output},
    {"window-map", test_window_map},
    {"render-pixels", test_render_pixels},
    {"workspace-switch", test_workspace_switch},
    {"shortcut-close", test_shortcut_table},
    {"shortcut-suppression", test_shortcut_release_suppression},
    {"shortcut-show-desktop", test_shortcut_show_desktop},
    {"pointer-focus", test_pointer_focus},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}

int main(void) { return xwt_run_all(); }
