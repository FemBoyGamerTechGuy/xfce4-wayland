/* test_backends.c — nested backend: a real compositor running inside
 * another real compositor, in one process.
 *
 * Parent A (headless, 400x300) hosts child B (nested backend, 200x150)
 * as a regular xdg-shell client. Asserted end to end:
 *
 *   1. topology: B has one output sized like its window in A
 *   2. present pipeline: B's background shows up in A's framebuffer
 *   3. nested clients: a client connected to B's socket renders through
 *      B into A's framebuffer (client -> B -> present -> A -> pixels)
 *   4. input routing: input injected into A reaches B's focused window,
 *      B's shortcut engine consumes a binding A does not have
 *      (parent shortcuts must not shadow the child desktop)
 */
#include "xwtest.h"

#include <sys/stat.h>
#include <unistd.h>

#define PARENT_BG 0xff112233u
#define CHILD_BG  0xff445566u
#define CLIENT_GREEN 0xff00ff00u

static int s_instance = 0;

/* ------------------------------------------------------------------ utils */

static void write_file(const char *dir, const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* count pixels of `color` in an ARGB8888 buffer */
static int count_pixels(const uint32_t *pix, int n, uint32_t color) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (pix[i] == color)
            c++;
    return c;
}

/* pump used during B's blocking parent handshake: drives A only
 * (xwc_sync drains B's side itself) */
static void pump_parent(void *ud) {
    struct xw_compositor *a = ud;
    xw_compositor_dispatch(a, 0);
}

/* steady-state pump: both compositors make progress per round */
static void pump_both(struct xw_compositor *a, struct xw_compositor *b) {
    xw_compositor_dispatch(a, 0);
    xw_compositor_dispatch(b, 0);
}

/* hook that counts and suppresses execution */
static bool bool_hook_count(int action, const char *arg, void *ud) {
    (void)action;
    (void)arg;
    (*(int *)ud)++;
    return false;
}

/* pump wrapper with the signature xwc expects (parent kept in a global
 * for the duration of the client connect) */
static struct xw_compositor *g_a_for_wrap;
static void pump_both_wrap(void *ud) {
    pump_both(g_a_for_wrap, ud);
}

/* ------------------------------------------------------------------ test */

static void test_nested(struct xwt_ctx *t) {
    (void)t; /* this test builds its own topology */

    char dir_a[256], dir_b[256], rtd[128];
    snprintf(rtd, sizeof(rtd), "%s/nest-%d", g_runtimedir(), s_instance);
    mkdir(rtd, 0700);
    snprintf(dir_a, sizeof(dir_a), "%s/conf-a", rtd);
    snprintf(dir_b, sizeof(dir_b), "%s/conf-b", rtd);
    mkdir(dir_a, 0700);
    mkdir(dir_b, 0700);
    s_instance++;

    write_file(dir_a, "compositor.conf",
               "[background]\ncolor = 0xff112233\n");
    write_file(dir_b, "compositor.conf",
               "[background]\ncolor = 0xff445566\n");
    /* B: show-desktop on Ctrl+Alt+P — NOT bound in A's defaults, so the
     * parent must pass the key through to the focused child window */
    write_file(dir_b, "shortcuts.conf",
               "[shortcuts]\nshow-desktop = <Ctrl><Alt>p\n");

    /* ---- parent A: headless 400x300 */
    struct xw_output_spec specs_a[1] = {{.name = "A-1", .width = 400, .height = 300, .scale = 1}};
    struct xw_compositor_config cfg_a = {0};
    cfg_a.config_dir = dir_a;
    cfg_a.socket_name = "xwt-nest-a";
    cfg_a.log_level = XW_LOG_ERROR;
    cfg_a.outputs = specs_a;
    cfg_a.n_outputs = 1;
    struct xw_compositor *a = xw_compositor_create(&cfg_a);
    XWT_ASSERT(a != NULL);

    /* ---- child B: nested into A, wants 200x150 */
    struct xw_output_spec specs_b[1] = {{.name = "B-1", .width = 200, .height = 150, .scale = 1}};
    struct xw_compositor_config cfg_b = {0};
    cfg_b.config_dir = dir_b;
    cfg_b.socket_name = "xwt-nest-b";
    cfg_b.log_level = XW_LOG_ERROR;
    cfg_b.backend = XW_BACKEND_NESTED;
    cfg_b.parent_display = "xwt-nest-a";
    cfg_b.outputs = specs_b;
    cfg_b.n_outputs = 1;
    cfg_b.nested_pump = pump_parent;
    cfg_b.nested_pump_ud = a;
    struct xw_compositor *b = xw_compositor_create(&cfg_b);
    XWT_ASSERT(b != NULL);
    XWT_CHECK(strcmp(a->backend->name, "headless") == 0, "parent backend");
    XWT_CHECK(strcmp(b->backend->name, "nested") == 0, "child backend");

    /* ---- settle */
    for (int i = 0; i < 50; i++)
        pump_both(a, b);

    /* 1. topology */
    XWT_CHECK(xw_compositor_n_outputs(b) == 1, "child has one output");
    int bw = 0, bh = 0;
    xw_compositor_output_info(b, 0, NULL, NULL, &bw, &bh, NULL);
    XWT_CHECK(bw == 200 && bh == 150, "child output is window-sized (%dx%d)", bw, bh);
    XWT_CHECK(xw_compositor_window_count(a) == 1, "parent sees the child window");

    /* 2. present pipeline: B's background in A's framebuffer */
    int aw = 0, ah = 0;
    const uint32_t *apix = xw_compositor_output_pixels(a, 0, &aw, &ah);
    XWT_ASSERT(apix != NULL);
    {
        bool found = false;
        for (int i = 0; i < 2000 && !(found = count_pixels(apix, aw * ah, CHILD_BG) > 1000); i++) {
            pump_both(a, b);
            apix = xw_compositor_output_pixels(a, 0, &aw, &ah);
        }
        XWT_CHECK(found, "child background presented into the parent (%d px)",
                  count_pixels(apix, aw * ah, CHILD_BG));
    }

    /* 3. a client of B renders through B into A. The pump drives both
     * compositors; it must stay valid for the whole lifetime of
     * client_b's connection (xwc_sync inside win_create uses it too),
     * so g_a_for_wrap is only cleared at teardown. */
    struct xwc client_b;
    g_a_for_wrap = a;
    XWT_ASSERT(xwc_connect_pumped(&client_b, "xwt-nest-b", pump_both_wrap, b) == 0);

    struct xwc_win *cw = xwc_win_create(&client_b, NULL, "green", "test.green", 100, 80);
    XWT_ASSERT(cw != NULL);
    int stride = 0;
    uint32_t *pix = xwc_win_pixels(cw, &stride);
    XWT_ASSERT(pix != NULL);
    for (int i = 0; i < stride * 80; i++)
        pix[i] = CLIENT_GREEN;
    xwc_win_commit(cw);
    xwc_flush(&client_b);

    {
        bool found = false;
        for (int i = 0; i < 2000 && !(found = count_pixels(apix, aw * ah, CLIENT_GREEN) > 1000); i++) {
            pump_both(a, b);
            /* the test client needs its side drained too */
            xwc_drain(&client_b);
            apix = xw_compositor_output_pixels(a, 0, &aw, &ah);
        }
        XWT_CHECK(found, "client of the child rendered into the parent");
    }

    /* 4. input routing: inject into A, B's shortcut engine consumes */
    int hook_b_actions = 0, hook_a_actions = 0;
    xw_compositor_set_action_hook(
        b, bool_hook_count, &hook_b_actions); /* returns false: suppress */
    xw_compositor_set_action_hook(a, bool_hook_count, &hook_a_actions);
    /* find B's window in A and focus it with a click */
    if (wl_list_empty(&a->wm->windows)) {
        XWT_CHECK(false, "no child window in the parent");
        g_a_for_wrap = NULL;
        xwc_win_destroy(cw);
        xwc_disconnect(&client_b);
        xw_compositor_destroy(b);
        xw_compositor_destroy(a);
        return;
    }
    struct xw_window *nested_win = NULL;
    wl_list_for_each(nested_win, &a->wm->windows, link)
        break; /* first (only) window */
    XWT_ASSERT(nested_win != NULL);
    int cx = nested_win->x + nested_win->w / 2;
    int cy = nested_win->y + nested_win->h / 2;
    xw_compositor_inject_pointer_motion(a, cx, cy);
    xw_compositor_inject_pointer_button(a, 0x110, true);
    xw_compositor_inject_pointer_button(a, 0x110, false);
    for (int i = 0; i < 20; i++)
        pump_both(a, b);

    /* Ctrl+Alt+P: unbound in A, bound in B */
    xw_compositor_inject_key(a, 29, true);  /* LeftCtrl */
    xw_compositor_inject_key(a, 56, true);  /* LeftAlt */
    xw_compositor_inject_key(a, 25, true);  /* P */
    xw_compositor_inject_key(a, 25, false);
    xw_compositor_inject_key(a, 56, false);
    xw_compositor_inject_key(a, 29, false);
    for (int i = 0; i < 50 && hook_b_actions == 0; i++)
        pump_both(a, b);

    XWT_CHECK(hook_b_actions == 1, "child consumed its shortcut (hook=%d)",
              hook_b_actions);
    XWT_CHECK(hook_a_actions == 0, "parent did not consume the key");

    /* teardown (reverse order) */
    g_a_for_wrap = NULL;
    xwc_win_destroy(cw);
    xwc_disconnect(&client_b);
    xw_compositor_destroy(b);
    xw_compositor_destroy(a);
}

#ifdef XW_HAVE_X11_BACKEND
/* X synthetic-repeat filter: with detectable auto-repeat the server
 * re-sends KeyPress for held keys; those must be dropped because
 * clients already repeat via wl_keyboard.repeat_info. */
static void test_x11_repeat_filter(struct xwt_ctx *t) {
    (void)t;
    uint32_t pressed[8] = {0};

    XWT_CHECK(xw_x11_key_filter(pressed, 30, true), "first press forwarded");
    XWT_CHECK(!xw_x11_key_filter(pressed, 30, true),
              "repeat press for a held key dropped");
    XWT_CHECK(!xw_x11_key_filter(pressed, 30, true),
              "another repeat still dropped");
    XWT_CHECK(xw_x11_key_filter(pressed, 30, false), "release forwarded");
    XWT_CHECK(xw_x11_key_filter(pressed, 30, true),
              "press after release forwarded again");

    /* different keys held at the same time do not interfere */
    XWT_CHECK(xw_x11_key_filter(pressed, 42, true), "second key press");
    XWT_CHECK(!xw_x11_key_filter(pressed, 42, true),
              "second key repeat dropped");
    XWT_CHECK(xw_x11_key_filter(pressed, 30, false),
              "release of the other held key forwards");
    XWT_CHECK(xw_x11_key_filter(pressed, 30, true),
              "press after that release forwards");

    /* out-of-bitmap range: forwarded untouched, no crash */
    XWT_CHECK(xw_x11_key_filter(pressed, 999, true), "large keycode passes");
    XWT_CHECK(xw_x11_key_filter(pressed, 999, false), "large keycode release");
}
#endif

__attribute__((constructor)) static void register_backends(void) {
    static const struct xwt_test tests[] = {
        {"nested-compositor", test_nested},
#ifdef XW_HAVE_X11_BACKEND
        {"x11-repeat-filter", test_x11_repeat_filter},
#endif
    };
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
