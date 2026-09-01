/* test_input.c — real-input pipeline and key-repeat tests.
 *
 * Level 1 (unit / in-process, this file): the full event pipeline is
 * exercised through the actual translation handlers and the actual
 * seat delivery path, using libinput path-mode contexts with no
 * devices (deterministic on any machine). The only part not covered
 * here is the thin libinput_event -> handler decoder, which requires
 * physical or uinput devices (Level 3, see TESTING.md).
 *
 * Key repeat is protocol-correct: clients receive wl_keyboard.
 * repeat_info and repeat themselves; the server-side timer only
 * repeats keys used by interactive keyboard move/resize.
 */
#include "xwtest.h"

#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- record */

struct rec {
    int keys, key_downs;
    int buttons, button_downs;
    uint32_t last_button;
    int motions;
    int last_mx, last_my;
    int axes;
    uint32_t last_axis;
    double last_axis_value;
};

static void rec_key(struct xwc_win *w, uint32_t keycode, bool down,
                    xkb_keysym_t sym, uint32_t mods, void *ud) {
    (void)w;
    (void)keycode;
    (void)sym;
    (void)mods;
    struct rec *r = ud;
    r->keys++;
    if (down)
        r->key_downs++;
}

static void rec_button(struct xwc_win *w, uint32_t button, bool down, int x,
                       int y, void *ud) {
    (void)w;
    (void)x;
    (void)y;
    struct rec *r = ud;
    r->buttons++;
    if (down)
        r->button_downs++;
    r->last_button = button;
}

static void rec_motion(struct xwc_win *w, int x, int y, void *ud) {
    (void)w;
    struct rec *r = ud;
    r->motions++;
    r->last_mx = x;
    r->last_my = y;
}

static void rec_axis(struct xwc_win *w, uint32_t axis, double value, void *ud) {
    (void)w;
    struct rec *r = ud;
    r->axes++;
    r->last_axis = axis;
    r->last_axis_value = value;
}

/* condition wait driving both sides of the connection */
#define ICTX_WAIT(ic, cond)                                                   \
    ({                                                                        \
        bool _ok = false;                                                     \
        for (int _i = 0; _i < 2000 && !(_ok = (cond)); _i++) {                \
            xw_compositor_dispatch((ic).comp, 0);                             \
            xwc_drain(&(ic).client);                                          \
        }                                                                     \
        XWT_CHECK(_ok, "timeout waiting for: %s", #cond);                    \
        _ok;                                                                  \
    })

/* --------------------------------------------------------------- helpers */

static void write_file(const char *dir, const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static int s_instance;

/* manual compositor + client setup (custom configs, not xwt_begin) */
struct ictx {
    struct xw_compositor *comp;
    struct xwc client;
    struct xwc_win *win;
    struct rec rec;
    char sock[32];
};

static void pump_wrap(void *ud) {
    xw_compositor_dispatch(ud, 0);
}

/* draw + commit on configure (a window never maps without content) */
static uint32_t g_fill_color = 0xff4a6a8a;
static void rec_configure(struct xwc_win *w, int width, int height, void *ud) {
    (void)ud;
    (void)width;
    (void)height;
    int ww = 0, wh = 0, stride = 0;
    xwc_win_size(w, &ww, &wh);
    uint32_t *pix = xwc_win_pixels(w, &stride);
    if (!pix || ww < 1 || wh < 1)
        return;
    xwc_fill_rect(pix, stride, ww, wh, 0, 0, ww, wh, g_fill_color);
    xwc_win_commit(w);
}

static bool ictx_start(struct ictx *ic, const char *conf_dir,
                       struct xw_compositor_config *extra) {
    memset(ic, 0, sizeof(*ic));
    snprintf(ic->sock, sizeof(ic->sock), "xwt-in-%d-%d", getpid(), s_instance++);

    struct xw_compositor_config cfg = {0};
    cfg.socket_name = ic->sock;
    cfg.log_level = XW_LOG_ERROR;
    cfg.config_dir = conf_dir;
    if (extra)
        cfg = *extra;
    cfg.socket_name = ic->sock; /* never overridden */
    ic->comp = xw_compositor_create(&cfg);
    if (!ic->comp)
        return false;

    if (xwc_connect_pumped(&ic->client, ic->sock, pump_wrap, ic->comp) != 0)
        return false;

    struct xwc_callbacks cb = {0};
    cb.key = rec_key;
    cb.button = rec_button;
    cb.motion = rec_motion;
    cb.axis = rec_axis;
    cb.configure = rec_configure;
    cb.ud = &ic->rec;
    ic->win = xwc_win_create(&ic->client, &cb, "rec", "xw.rec", 200, 150);
    for (int i = 0; i < 40; i++) {
        xw_compositor_dispatch(ic->comp, 0);
        xwc_drain(&ic->client);
    }
    return ic->win != NULL;
}

static void ictx_stop(struct ictx *ic) {
    if (ic->win) {
        xwc_win_destroy(ic->win);
        ic->win = NULL;
    }
    xwc_disconnect(&ic->client);
    if (ic->comp) {
        xw_compositor_destroy(ic->comp);
        ic->comp = NULL;
    }
}

/* paced pump: gives real time to the seat repeat timer and the loop */
static void paced(struct ictx *ic, int ms) {
    for (int done = 0; done < ms; done += 10) {
        xw_compositor_dispatch(ic->comp, 10);
        xwc_drain(&ic->client);
        usleep(10000);
    }
}

/* ------------------------------------------------ key repeat (protocol) */

static void test_repeat_info(struct xwt_ctx *t) {
    (void)t;
    struct ictx ic;
    XWT_ASSERT(ictx_start(&ic, NULL, NULL));

    ICTX_WAIT(ic, ic.client.repeat_info_received);
    XWT_CHECK(ic.client.repeat_info_received,
              "wl_keyboard.repeat_info not delivered after keymap");
    XWT_CHECK(ic.client.repeat_delay_ms == 500, "default delay (got %d)",
              ic.client.repeat_delay_ms);
    XWT_CHECK(ic.client.repeat_rate_hz == 30, "default rate (got %d)",
              ic.client.repeat_rate_hz);

    ictx_stop(&ic);
}

static void test_repeat_info_config(struct xwt_ctx *t) {
    (void)t;
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/in-conf-%d", g_runtimedir(), s_instance++);
    mkdir(dir, 0700);
    write_file(dir, "keyboard.conf",
               "[keyboard]\nrepeat_delay_ms = 250\nrepeat_rate_hz = 40\n");

    struct ictx ic;
    XWT_ASSERT(ictx_start(&ic, dir, NULL));

    ICTX_WAIT(ic, ic.client.repeat_info_received);
    XWT_CHECK(ic.client.repeat_delay_ms == 250, "configured delay (got %d)",
              ic.client.repeat_delay_ms);
    XWT_CHECK(ic.client.repeat_rate_hz == 40, "configured rate (got %d)",
              ic.client.repeat_rate_hz);

    ictx_stop(&ic);
}

/* ------------------------------- interactive keyboard move auto-repeat */

static void test_wm_key_repeat(struct xwt_ctx *t) {
    (void)t;
    struct xw_compositor_config extra = {0};
    extra.repeat_delay_ms = 20;  /* fast for testing */
    extra.repeat_rate_hz = 200;  /* 5 ms period */
    struct ictx ic;
    XWT_ASSERT(ictx_start(&ic, NULL, &extra));

    /* find the managed window (white-box) and start keyboard move */
    struct xw_window *w = NULL;
    wl_list_for_each(w, &ic.comp->wm->windows, link)
        break;
    XWT_ASSERT(w != NULL);
    int x0 = w->x;
    XWT_ASSERT(xw_wm_interactive_begin_move(ic.comp->wm, w, w->x + 10, w->y + 10));

    /* hold Left without release: one move on press, then timer repeats */
    xw_compositor_inject_key(ic.comp, K_LEFT, true);
    paced(&ic, 200); /* ~36 repeat ticks at 5 ms */
    int x1 = w->x;
    XWT_CHECK(x0 - x1 >= 50, "held Left moved the window repeatedly "
              "(moved %d px, expected >= 50)", x0 - x1);
    XWT_CHECK(x0 - x1 <= 400, "movement bounded (moved %d px)", x0 - x1);

    /* release: repetition must stop */
    xw_compositor_inject_key(ic.comp, K_LEFT, false);
    paced(&ic, 60);
    int x2 = w->x;
    paced(&ic, 100);
    XWT_CHECK(w->x == x2, "no further movement after release");

    /* client never sees server-side repeats of the move keys */
    XWT_CHECK(ic.rec.keys == 0, "interactive keys stay server-side (got %d "
              "client key events)", ic.rec.keys);

    ictx_stop(&ic);
}

/* ------------------------------- client keys are NOT server-repeated */

static void test_client_no_double_repeat(struct xwt_ctx *t) {
    (void)t;
    struct xw_compositor_config extra = {0};
    extra.repeat_delay_ms = 20;
    extra.repeat_rate_hz = 200;
    struct ictx ic;
    XWT_ASSERT(ictx_start(&ic, NULL, &extra));

    /* focus the window: move the pointer onto it (actual position,
     * white-box) and click */
    struct xw_window *w = NULL;
    wl_list_for_each(w, &ic.comp->wm->windows, link)
        break;
    XWT_ASSERT(w != NULL);
    xw_compositor_inject_pointer_motion(ic.comp, w->x + w->w / 2,
                                        w->y + w->h / 2);
    xw_compositor_inject_pointer_button(ic.comp, 0x110, true);
    xw_compositor_inject_pointer_button(ic.comp, 0x110, false);
    for (int i = 0; i < 20; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }

    /* an ordinary typing key held for a long time: exactly ONE press
     * event to the client (the client repeats it via repeat_info) */
    xw_compositor_inject_key(ic.comp, K_D, true);
    paced(&ic, 120); /* would produce ~20 server repeats if buggy */
    xw_compositor_inject_key(ic.comp, K_D, false);
    for (int i = 0; i < 20; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }
    XWT_CHECK(ic.rec.keys == 2, "exactly press+release reach the client "
              "(got %d)", ic.rec.keys);
    XWT_CHECK(ic.rec.key_downs == 1, "exactly one press (got %d)",
              ic.rec.key_downs);

    ictx_stop(&ic);
}

/* ------------------------------------------------- libinput pipeline */

#ifdef XW_HAVE_LIBINPUT

/* path-mode source over nonexistent devices: deterministic everywhere,
 * exercises context/fd/loop lifecycle with zero devices. */
static void test_input_lifecycle(struct xwt_ctx *t) {
    (void)t;
    setenv("XW_INPUT_DEVICES", "/nonexistent/xw-test-event0", 1);
    struct xw_compositor_config extra = {0};
    extra.input_mode = XW_INPUT_LIBINPUT;
    struct ictx ic;
    bool ok = ictx_start(&ic, NULL, &extra);
    unsetenv("XW_INPUT_DEVICES");
    XWT_ASSERT(ok);

    XWT_CHECK(ic.comp->input != NULL, "input source created in path mode");
    XWT_CHECK(ic.comp->input && wl_list_length(&ic.comp->outputs) == 1,
              "compositor fully functional with input source attached");

    ictx_stop(&ic);
    XWT_CHECK(1, "lifecycle clean");
}

/* AUTO mode never touches devices without an explicit device list */
static void test_input_auto_off(struct xwt_ctx *t) {
    (void)t;
    struct ictx ic;
    XWT_ASSERT(ictx_start(&ic, NULL, NULL));
    XWT_CHECK(ic.comp->input == NULL,
              "auto mode creates no input source (no XW_INPUT_DEVICES)");
    ictx_stop(&ic);
}

static void test_input_motion_pipeline(struct xwt_ctx *t) {
    (void)t;
    setenv("XW_INPUT_DEVICES", "/nonexistent/xw-test-event1", 1);
    struct xw_compositor_config extra = {0};
    extra.input_mode = XW_INPUT_LIBINPUT;
    struct ictx ic;
    bool ok = ictx_start(&ic, NULL, &extra);
    unsetenv("XW_INPUT_DEVICES");
    XWT_ASSERT(ok);
    struct xw_input_libinput *in = ic.comp->input;
    XWT_ASSERT(in != NULL);

    struct xw_seat *s = xw_seat_first(ic.comp);
    XWT_ASSERT(s != NULL);

    /* absolute: normalized over the layout (single 1280x720 output) */
    xw_input_handle_pointer_abs(in, 0.5, 0.5);
    for (int i = 0; i < 5; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }
    XWT_CHECK(s->cursor_x == 640 && s->cursor_y == 360,
              "abs motion mapped to layout (got %d,%d)", s->cursor_x,
              s->cursor_y);

    /* sub-pixel accumulation (away from the clamped edges): 3 x 0.4px
     * = 1 full pixel */
    int y0 = s->cursor_y;
    xw_input_handle_pointer_rel(in, 0.0, 0.4);
    xw_input_handle_pointer_rel(in, 0.0, 0.4);
    XWT_CHECK(s->cursor_y == y0, "sub-pixel motion buffered, not applied");
    xw_input_handle_pointer_rel(in, 0.0, 0.4);
    XWT_CHECK(s->cursor_y == y0 + 1, "accumulated sub-pixel motion applied "
              "(got %d, want %d)", s->cursor_y, y0 + 1);

    /* relative: clamped at the layout edges */
    xw_input_handle_pointer_rel(in, -5000.0, -5000.0);
    xw_input_handle_pointer_rel(in, -5000.0, -5000.0);
    XWT_CHECK(s->cursor_x == 0 && s->cursor_y == 0, "rel motion clamps low "
              "(got %d,%d)", s->cursor_x, s->cursor_y);
    xw_input_handle_pointer_rel(in, 9000.0, 9000.0);
    xw_input_handle_pointer_rel(in, 9000.0, 9000.0);
    XWT_CHECK(s->cursor_x == 1279 && s->cursor_y == 719, "rel motion clamps "
              "high (got %d,%d)", s->cursor_x, s->cursor_y);

    /* pointer over the window: client motion + button delivery (aim at
     * the actual window position, read white-box) */
    struct xw_window *w = NULL;
    wl_list_for_each(w, &ic.comp->wm->windows, link)
        break;
    XWT_ASSERT(w != NULL);
    int wx = w->x + w->w / 2, wy = w->y + w->h / 2;
    xw_input_handle_pointer_abs(in, wx / 1279.0, wy / 719.0);
    for (int i = 0; i < 10; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }
    int motions = ic.rec.motions;
    XWT_CHECK(motions > 0, "client received pointer motion events");
    xw_input_handle_button(in, 0x110, true);
    xw_input_handle_button(in, 0x110, false);
    for (int i = 0; i < 10; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }
    XWT_CHECK(ic.rec.buttons == 2 && ic.rec.last_button == 0x110,
              "linux button codes delivered verbatim (got %d events, "
              "last 0x%x)", ic.rec.buttons, ic.rec.last_button);

    /* wheel: v120 clicks translate to axis values in the same units the
     * nested backends use (1.0 = one notch) */
    xw_input_handle_axis(in, WL_POINTER_AXIS_VERTICAL_SCROLL, -3.0, false);
    for (int i = 0; i < 10; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }
    XWT_CHECK(ic.rec.axes == 1 && ic.rec.last_axis_value == -3.0,
              "axis delivered with wheel units (got %d events, value %f)",
              ic.rec.axes, ic.rec.last_axis_value);

    ictx_stop(&ic);
}

/* keys through the real handler path reach the focused client */
static void test_input_key_pipeline(struct xwt_ctx *t) {
    (void)t;
    setenv("XW_INPUT_DEVICES", "/nonexistent/xw-test-event2", 1);
    struct xw_compositor_config extra = {0};
    extra.input_mode = XW_INPUT_LIBINPUT;
    struct ictx ic;
    bool ok = ictx_start(&ic, NULL, &extra);
    unsetenv("XW_INPUT_DEVICES");
    XWT_ASSERT(ok);
    struct xw_input_libinput *in = ic.comp->input;
    XWT_ASSERT(in != NULL);

    xw_input_handle_pointer_abs(in, 0.5, 0.5);
    xw_input_handle_button(in, 0x110, true);
    xw_input_handle_button(in, 0x110, false);
    for (int i = 0; i < 10; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }

    xw_input_handle_key(in, K_L, true);
    xw_input_handle_key(in, K_L, false);
    for (int i = 0; i < 10; i++) {
        xw_compositor_dispatch(ic.comp, 0);
        xwc_drain(&ic.client);
    }
    XWT_CHECK(ic.rec.keys == 2 && ic.rec.key_downs == 1,
              "key press/release delivered via the real handler path "
              "(got %d events)", ic.rec.keys);

    ictx_stop(&ic);
}

#endif /* XW_HAVE_LIBINPUT */

__attribute__((constructor)) static void register_input(void) {
    static const struct xwt_test tests[] = {
        {"repeat-info", test_repeat_info},
        {"repeat-info-config", test_repeat_info_config},
        {"wm-key-repeat", test_wm_key_repeat},
        {"client-no-double-repeat", test_client_no_double_repeat},
#ifdef XW_HAVE_LIBINPUT
        {"input-lifecycle", test_input_lifecycle},
        {"input-auto-off", test_input_auto_off},
        {"input-motion-pipeline", test_input_motion_pipeline},
        {"input-key-pipeline", test_input_key_pipeline},
#endif
    };
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
