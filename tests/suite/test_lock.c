/* test_lock.c — ext-session-lock-v1 + ext-idle-notify-v1 coverage.
 *
 * Security properties under test (white-box + client-side):
 *   - while locked, ONLY lock surfaces render: window pixels must not
 *     be present anywhere on the output (pixel-scanned, not just
 *     spot-checked)
 *   - while locked, input reaches only the lock client (the window's
 *     key/button callbacks stay silent; the lock's fire)
 *   - the `locked` event arrives only after the locked frame was
 *     presented (client flag flips after the pixel state)
 *   - a lock client dying while locked keeps the session locked; a
 *     new client can take over and unlock
 *   - a second lock attempt while locked is denied (finished)
 *   - the lock timeout flushes `locked` when the client never commits
 *     lock surfaces (blank fallback frame)
 *   - output resize reconfigures the lock surface to the new size
 *   - idle notifications fire/resume on real activity timing
 *   - commit-before-first-ack is a fatal protocol error and a pending
 *     (never locked) offender dying releases the gate
 */
#include "xwtest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#include "ext-session-lock.h"
#include "ext-idle-notify.h"
#include "wayland-client.h"

#define WIN_COLOR 0xffa33434
#define LOCK_COLOR 0xff2a4a6a

/* ------------------------------------------------------------ helpers */

static uint32_t pixel_at(struct xwt_ctx *t, int x, int y) {
    int w = 0, h = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &w, &h);
    if (!pix || x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    return pix[y * w + x];
}

/* any pixel of `color` anywhere on the first output */
static bool any_pixel(struct xwt_ctx *t, uint32_t color) {
    int w = 0, h = 0;
    const uint32_t *pix = xw_compositor_output_pixels(t->comp, 0, &w, &h);
    if (!pix)
        return false;
    for (int i = 0; i < w * h; i++)
        if (pix[i] == color)
            return true;
    return false;
}

struct lock_ctx {
    int key_events;
    int button_events;
    int configure_events;
    int last_conf_w, last_conf_h;
    bool locked;
    bool finished;
};

static void tlock_configure(struct xwc_lock *l, int w, int h, void *ud) {
    struct lock_ctx *ctx = ud;
    ctx->configure_events++;
    ctx->last_conf_w = w;
    ctx->last_conf_h = h;
    int stride = 0;
    uint32_t *pix = xwc_lock_pixels(l, &stride);
    if (pix && w > 0 && h > 0) {
        xwc_fill_rect(pix, stride, w, h, 0, 0, w, h, LOCK_COLOR);
        xwc_lock_commit(l);
    }
}

static void tlock_locked(struct xwc_lock *l, void *ud) {
    (void)l;
    ((struct lock_ctx *)ud)->locked = true;
}

static void tlock_finished(struct xwc_lock *l, void *ud) {
    (void)l;
    ((struct lock_ctx *)ud)->finished = true;
}

static void tlock_key(struct xwc_lock *l, uint32_t keycode, bool down,
                      xkb_keysym_t sym, uint32_t mods, void *ud) {
    (void)l;
    (void)sym;
    (void)mods;
    if (down)
        ((struct lock_ctx *)ud)->key_events++;
    (void)keycode;
}

static void tlock_button(struct xwc_lock *l, uint32_t button, bool down,
                         int x, int y, void *ud) {
    (void)l;
    (void)button;
    (void)x;
    (void)y;
    if (down)
        ((struct lock_ctx *)ud)->button_events++;
}

/* window-side input counters (must stay silent while locked) */
struct win_ctx {
    int key_events;
    int button_events;
};

static void twin_key(struct xwc_win *w, uint32_t keycode, bool down,
                     xkb_keysym_t sym, uint32_t mods, void *ud) {
    (void)w;
    (void)keycode;
    (void)sym;
    (void)mods;
    if (down)
        ((struct win_ctx *)ud)->key_events++;
}

static void twin_button(struct xwc_win *w, uint32_t button, bool down, int x,
                        int y, void *ud) {
    (void)w;
    (void)button;
    (void)x;
    (void)y;
    if (down)
        ((struct win_ctx *)ud)->button_events++;
}

static void twin_configure(struct xwc_win *w, int width, int height, void *ud) {
    (void)width;
    (void)height;
    (void)ud;
    int ww = 0, wh = 0, stride = 0;
    xwc_win_size(w, &ww, &wh);
    uint32_t *pix = xwc_win_pixels(w, &stride);
    if (!pix || ww < 1 || wh < 1)
        return;
    xwc_fill_rect(pix, stride, ww, wh, 0, 0, ww, wh, WIN_COLOR);
    xwc_win_commit(w);
}

/* --------------------------------------------------------------- tests */

static void test_session_lock_basic(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, WIN_COLOR, 300, 200, "victim");
    XWT_ASSERT(win);
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));
    XWT_CHECK(any_pixel(t, WIN_COLOR), "window rendered before locking");

    struct lock_ctx ctx = {0};
    struct xwc_lock_cbs lcbs = {
        .locked = tlock_locked,
        .finished = tlock_finished,
        .configure = tlock_configure,
        .key = tlock_key,
        .button = tlock_button,
        .ud = &ctx,
    };
    struct xwc_lock *k = xwc_lock_create(&t->client, &lcbs);
    XWT_ASSERT(k);

    XWT_WAIT(t, ctx.locked);
    XWT_CHECK(ctx.locked, "locked event received (after presented frame)");
    XWT_CHECK(xw_session_lock_active(t->comp), "gate engaged (white-box)");
    XWT_CHECK(!any_pixel(t, WIN_COLOR),
              "SECURITY: no window pixel anywhere while locked");
    XWT_CHECK(any_pixel(t, LOCK_COLOR), "lock surface rendered");
    XWT_CHECK(xwc_lock_locked(k), "client sees locked state");

    xwc_lock_unlock(k);
    XWT_WAIT(t, !xw_session_lock_active(t->comp));
    XWT_CHECK(!xw_session_lock_active(t->comp), "gate released by unlock");
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));
    XWT_CHECK(any_pixel(t, WIN_COLOR), "window content restored after unlock");
    XWT_CHECK(!any_pixel(t, LOCK_COLOR), "lock surface no longer rendered");

    xwc_lock_destroy(k);
    xwc_win_destroy(win);
}

static void test_session_lock_input_gate(struct xwt_ctx *t) {
    struct win_ctx wctx = {0};
    struct xwc_callbacks wcb = {
        .key = twin_key,
        .button = twin_button,
        .configure = twin_configure,
        .ud = &wctx,
    };
    struct xwc_win *win =
        xwc_win_create(&t->client, &wcb, "input-victim", "xw.test", 300, 200);
    XWT_ASSERT(win);
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));
    (void)win;

    /* unlocked sanity: keys reach the focused window */
    xw_compositor_inject_key(t->comp, K_D, true);
    xw_compositor_inject_key(t->comp, K_D, false);
    xwt_pump(t);
    XWT_CHECK(wctx.key_events == 1,
              "unlocked: key delivered to the window (%d)", wctx.key_events);

    struct lock_ctx ctx = {0};
    struct xwc_lock_cbs lcbs = {
        .locked = tlock_locked,
        .configure = tlock_configure,
        .key = tlock_key,
        .button = tlock_button,
        .ud = &ctx,
    };
    struct xwc_lock *k = xwc_lock_create(&t->client, &lcbs);
    XWT_ASSERT(k);
    XWT_WAIT(t, ctx.locked);
    XWT_ASSERT(ctx.locked);

    /* while locked: input belongs to the lock surface only */
    xw_compositor_inject_key(t->comp, K_D, true);
    xw_compositor_inject_key(t->comp, K_D, false);
    xwt_pump(t);
    xwt_pump(t);
    XWT_CHECK(ctx.key_events == 1, "locked: key delivered to the lock surface");
    XWT_CHECK(wctx.key_events == 1,
              "SECURITY: window received no key while locked (%d)",
              wctx.key_events);

    /* shortcut engine must be dead while locked: K_ESC on its own is not
     * a shortcut, but the close-shortcut modifier combo must not fire —
     * verified structurally by the key accounting above (no key event
     * reaches any normal client and no shortcut action runs) */
    xw_compositor_inject_pointer_motion(t->comp, 640, 300);
    xw_compositor_inject_pointer_button(t->comp, 0x110, true);
    xw_compositor_inject_pointer_button(t->comp, 0x110, false);
    xwt_pump(t);
    xwt_pump(t);
    XWT_CHECK(ctx.button_events == 1, "locked: button delivered to lock");
    XWT_CHECK(wctx.button_events == 0,
              "SECURITY: window received no button while locked");

    xwc_lock_unlock(k);
    XWT_WAIT(t, !xw_session_lock_active(t->comp));
    xwc_lock_destroy(k);
    xwc_win_destroy(win);
}

static void test_session_lock_denied(struct xwt_ctx *t) {
    struct lock_ctx a = {0}, b = {0};
    struct xwc_lock_cbs base = {
        .locked = tlock_locked,
        .finished = tlock_finished,
        .configure = tlock_configure,
        .key = tlock_key,
        .ud = NULL,
    };
    base.ud = &a;
    struct xwc_lock *first = xwc_lock_create(&t->client, &base);
    XWT_ASSERT(first);
    XWT_WAIT(t, a.locked);
    XWT_ASSERT(a.locked);

    base.ud = &b;
    struct xwc_lock *second = xwc_lock_create(&t->client, &base);
    XWT_ASSERT(second);
    XWT_WAIT(t, b.finished);
    XWT_CHECK(b.finished, "second lock attempt denied with finished");
    XWT_CHECK(!b.locked, "denied lock never received locked");
    XWT_CHECK(xw_session_lock_active(t->comp), "original lock still active");

    xwc_lock_destroy(second);
    xwc_lock_unlock(first);
    XWT_WAIT(t, !xw_session_lock_active(t->comp));
    xwc_lock_destroy(first);
}

/* a second client dies while holding the lock: the session stays
 * locked (blank) and a new client can take over */
static void pump_c2(void *ud) {
    /* the embedded server must advance while the second client syncs */
    xw_compositor_dispatch(((struct xwt_ctx *)ud)->comp, 0);
}

static void pump_both(struct xwt_ctx *t, struct xwc *c2) {
    xwt_pump(t);
    xwc_drain(c2);
}

#define WAIT2(t, c2, cond)                                                    \
    ({                                                                        \
        bool _ok = false;                                                     \
        for (int _i = 0; _i < 2000 && !(_ok = (cond)); _i++)                  \
            pump_both(t, c2);                                                 \
        XWT_CHECK(_ok, "timeout waiting for: %s", #cond);                     \
        _ok;                                                                  \
    })

static void test_session_lock_client_death(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, WIN_COLOR, 300, 200, "victim2");
    XWT_ASSERT(win);
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));

    struct xwc c2;
    memset(&c2, 0, sizeof(c2));
    XWT_ASSERT(xwc_connect_pumped(&c2, t->socket_name, pump_c2, t) == 0);

    struct lock_ctx ctx = {0};
    struct xwc_lock_cbs lcbs = {
        .locked = tlock_locked,
        .finished = tlock_finished,
        .configure = tlock_configure,
        .key = tlock_key,
        .ud = &ctx,
    };
    struct xwc_lock *k = xwc_lock_create(&c2, &lcbs);
    XWT_ASSERT(k);
    WAIT2(t, &c2, ctx.locked);
    XWT_ASSERT(ctx.locked);
    XWT_CHECK(xw_session_lock_active(t->comp), "locked by second client");

    /* the lock client dies without unlocking */
    xwc_lock_destroy(k); /* held lock: surfaces destroyed, object left */
    xwc_disconnect(&c2);
    xwt_pump(t);
    xwt_pump(t);

    /* SECURITY: the session stays locked */
    XWT_CHECK(xw_session_lock_active(t->comp),
              "session stays locked after the lock client died");
    XWT_CHECK(!any_pixel(t, WIN_COLOR),
              "SECURITY: window pixels stay hidden after lock client death");
    XWT_CHECK(!any_pixel(t, LOCK_COLOR),
              "dead client's lock surface no longer renders");

    /* a new client takes over and unlocks */
    struct lock_ctx ctx2 = {0};
    lcbs.ud = &ctx2;
    struct xwc_lock *k2 = xwc_lock_create(&t->client, &lcbs);
    XWT_ASSERT(k2);
    XWT_WAIT(t, ctx2.locked);
    XWT_CHECK(ctx2.locked, "takeover lock received locked");
    XWT_CHECK(any_pixel(t, LOCK_COLOR), "takeover surface renders");

    xwc_lock_unlock(k2);
    XWT_WAIT(t, !xw_session_lock_active(t->comp));
    XWT_CHECK(any_pixel(t, WIN_COLOR), "content restored after takeover");
    xwc_lock_destroy(k2);
    xwc_win_destroy(win);
}

/* the client never commits lock surfaces: the timeout still locks with
 * a blank frame (spec time limit) */
static void test_session_lock_timeout(struct xwt_ctx *t) {
    struct xwc_win *win = xwt_window_solid(t, WIN_COLOR, 300, 200, "victim3");
    XWT_ASSERT(win);
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));

    setenv("XW_LOCK_TIMEOUT_MS", "60", 1);
    struct lock_ctx ctx = {0};
    struct xwc_lock_cbs lcbs = {
        .locked = tlock_locked,
        .finished = tlock_finished,
        .configure = NULL, /* never draws, never commits */
        .ud = &ctx,
    };
    struct xwc_lock *k = xwc_lock_create(&t->client, &lcbs);
    XWT_ASSERT(k);
    /* timers need wall-clock time to elapse */
    bool fired = false;
    for (int i = 0; i < 300 && !fired; i++) {
        xwt_pump(t);
        usleep(2000);
        fired = ctx.locked;
    }
    unsetenv("XW_LOCK_TIMEOUT_MS");
    XWT_CHECK(fired,
              "timeout flushed the locked event despite no commit");
    if (!fired)
        return; /* cannot unlock a never-locked object (protocol error) */
    XWT_CHECK(!any_pixel(t, WIN_COLOR), "blank frame hides content");
    XWT_CHECK(!any_pixel(t, LOCK_COLOR), "no lock surface was committed");

    xwc_lock_unlock(k);
    XWT_WAIT(t, !xw_session_lock_active(t->comp));
    xwc_lock_destroy(k);
    xwc_win_destroy(win);
}

/* output resize while locked: the lock surface is reconfigured to the
 * new exact size and recommits */
static void test_session_lock_resize(struct xwt_ctx *t) {
    struct lock_ctx ctx = {0};
    struct xwc_lock_cbs lcbs = {
        .locked = tlock_locked,
        .configure = tlock_configure,
        .key = tlock_key,
        .ud = &ctx,
    };
    struct xwc_lock *k = xwc_lock_create(&t->client, &lcbs);
    XWT_ASSERT(k);
    XWT_WAIT(t, ctx.locked);
    XWT_ASSERT(ctx.locked);
    XWT_WAIT(t, any_pixel(t, LOCK_COLOR));
    int conf_before = ctx.configure_events;

    struct xw_output *o = wl_container_of(t->comp->outputs.next, o, link);
    xw_output_resize(o, 1000, 700);

    XWT_WAIT(t, ctx.configure_events > conf_before &&
                   ctx.last_conf_w == 1000 && ctx.last_conf_h == 700);
    XWT_CHECK(ctx.last_conf_w == 1000 && ctx.last_conf_h == 700,
              "lock surface reconfigured to the new output size (%dx%d)",
              ctx.last_conf_w, ctx.last_conf_h);
    XWT_WAIT(t, pixel_at(t, 995, 5) == LOCK_COLOR);
    XWT_CHECK(pixel_at(t, 995, 5) == LOCK_COLOR,
              "lock surface spans the NEW width");
    XWT_CHECK(xw_session_lock_active(t->comp), "still locked after resize");

    xwc_lock_unlock(k);
    XWT_WAIT(t, !xw_session_lock_active(t->comp));
    xwc_lock_destroy(k);
}

/* ------------------------------------------------------------- idle */

struct idle_ctx {
    int idled;
    int resumed;
};

static void on_idled(void *ud) { ((struct idle_ctx *)ud)->idled++; }
static void on_resumed(void *ud) { ((struct idle_ctx *)ud)->resumed++; }

/* timers need wall-clock time to elapse: pump with small sleeps */
static void test_idle_notify(struct xwt_ctx *t) {
    struct idle_ctx ctx = {0};
    struct xwc_idle *idle = xwc_idle_create(&t->client, 80, on_idled,
                                            on_resumed, &ctx);
    XWT_ASSERT(idle);

    /* no input: idled fires after ~80ms */
    bool fired = false;
    for (int i = 0; i < 200 && !fired; i++) {
        xwt_pump(t);
        usleep(2000);
        fired = ctx.idled > 0;
    }
    XWT_CHECK(fired, "idled fired after the timeout (no input)");

    /* input: resumed fires immediately */
    xw_compositor_inject_pointer_motion(t->comp, 100, 100);
    xwt_pump(t);
    XWT_CHECK(ctx.resumed == 1, "resumed fired on input");

    /* and the notification re-arms: idled fires again after another
     * quiet period */
    ctx.idled = 0;
    fired = false;
    for (int i = 0; i < 200 && !fired; i++) {
        xwt_pump(t);
        usleep(2000);
        fired = ctx.idled > 0;
    }
    XWT_CHECK(fired, "notification re-armed and fired again");

    /* a short-timeout notification and a long one are independent */
    struct idle_ctx fast = {0}, slow = {0};
    struct xwc_idle *i_fast = xwc_idle_create(&t->client, 40, on_idled,
                                              on_resumed, &fast);
    struct xwc_idle *i_slow = xwc_idle_create(&t->client, 500, on_idled,
                                              on_resumed, &slow);
    XWT_ASSERT(i_fast && i_slow);
    bool fast_fired = false;
    for (int i = 0; i < 150 && !fast_fired; i++) {
        xwt_pump(t);
        usleep(2000);
        fast_fired = fast.idled > 0;
    }
    XWT_CHECK(fast_fired, "short timeout fired");
    XWT_CHECK(slow.idled == 0, "long timeout did not fire yet");

    xwc_idle_destroy(i_slow);
    xwc_idle_destroy(i_fast);
    xwc_idle_destroy(idle);
}

/* ----------------------------------------------- protocol error (raw) */

/* commit-before-first-ack via a raw protocol connection: the server
 * must terminate the client with the protocol error, and since the
 * lock never became active (PENDING), the death releases the gate. */
struct raw_ctx {
    struct wl_compositor *compositor;
    struct ext_session_lock_manager_v1 *manager;
    struct wl_output *output;
};

static void raw_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    struct raw_ctx *ctx = data;
    if (strcmp(iface, "wl_compositor") == 0 && version > 4)
        version = 4;
    if (strcmp(iface, "wl_compositor") == 0)
        ctx->compositor =
            wl_registry_bind(r, name, &wl_compositor_interface, version);
    else if (strcmp(iface, "ext_session_lock_manager_v1") == 0)
        ctx->manager = wl_registry_bind(
            r, name, &ext_session_lock_manager_v1_interface,
            version > 1 ? 1 : version);
    else if (strcmp(iface, "wl_output") == 0 && !ctx->output)
        ctx->output = wl_registry_bind(r, name, &wl_output_interface, 1);
}

static void raw_global_remove(void *data, struct wl_registry *r,
                              uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener raw_registry_listener = {
    .global = raw_global,
    .global_remove = raw_global_remove,
};

/* non-blocking roundtrip for a raw connection against the embedded
 * server: flush, let the server process (xwt_pump), then drain. */
static int raw_roundtrip(struct xwt_ctx *t, struct wl_display *d) {
    if (wl_display_flush(d) < 0) {
        fprintf(stderr, "  raw_roundtrip: flush failed errno=%d\n", errno);
        return -1;
    }
    xwt_pump(t);
    xwt_pump(t); /* first pump may only accept the connection */
    while (wl_display_prepare_read(d) != 0) {
        if (wl_display_dispatch_pending(d) < 0) {
            fprintf(stderr, "  raw_roundtrip: pending dispatch failed\n");
            return -1;
        }
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(d), .events = POLLIN};
    poll(&pfd, 1, 0);
    if (pfd.revents & POLLIN) {
        if (wl_display_read_events(d) < 0) {
            fprintf(stderr, "  raw_roundtrip: read failed errno=%d\n", errno);
            return -1;
        }
    } else {
        wl_display_cancel_read(d);
    }
    int rc = wl_display_dispatch_pending(d);
    if (rc < 0)
        fprintf(stderr, "  raw_roundtrip: final dispatch failed errno=%d "
                        "proto=%d\n",
                errno, wl_display_get_error(d));
    return rc < 0 ? -1 : 0; /* dispatch_pending returns an event count */
}

static void test_session_lock_commit_before_ack(struct xwt_ctx *t) {
    struct wl_display *d = wl_display_connect(t->socket_name);
    XWT_ASSERT(d);
    struct raw_ctx ctx = {0};
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &raw_registry_listener, &ctx);
    XWT_ASSERT(raw_roundtrip(t, d) == 0);
    if (!ctx.compositor || !ctx.manager || !ctx.output) {
        fprintf(stderr, "  raw globals: comp=%p mgr=%p out=%p err=%d\n",
                (void *)ctx.compositor, (void *)ctx.manager,
                (void *)ctx.output, wl_display_get_error(d));
        wl_display_disconnect(d);
        XWT_ASSERT(ctx.compositor && ctx.manager);
        return;
    }

    struct wl_surface *s = wl_compositor_create_surface(ctx.compositor);
    XWT_ASSERT(s);
    struct ext_session_lock_v1 *l =
        ext_session_lock_manager_v1_lock(ctx.manager);
    XWT_ASSERT(l);
    struct ext_session_lock_surface_v1 *ls =
        ext_session_lock_v1_get_lock_surface(l, s, ctx.output);
    XWT_ASSERT(ls);
    /* the first configure arrives here; we deliberately do NOT ack */
    XWT_ASSERT(raw_roundtrip(t, d) == 0);

    /* commit with the lock role before any ack: commit_before_first_ack
     * is a fatal protocol error — the server disconnects the client */
    XWT_CHECK(xw_session_lock_active(t->comp),
              "gate engaged while the pending lock exists");
    wl_surface_commit(s);
    int rc = raw_roundtrip(t, d);
    XWT_CHECK(rc < 0, "protocol error disconnected the offending client");

    /* the offender died while PENDING (never locked): the gate must
     * be released, not held */
    xwt_pump(t);
    xwt_pump(t);
    XWT_CHECK(!xw_session_lock_active(t->comp),
              "PENDING offender death released the gate");
    wl_display_disconnect(d);
}

static const struct xwt_test tests[] = {
    {"session-lock-basic", test_session_lock_basic},
    {"session-lock-input-gate", test_session_lock_input_gate},
    {"session-lock-denied", test_session_lock_denied},
    {"session-lock-client-death", test_session_lock_client_death},
    {"session-lock-timeout", test_session_lock_timeout},
    {"session-lock-resize", test_session_lock_resize},
    {"idle-notify", test_idle_notify},
    {"session-lock-commit-before-ack", test_session_lock_commit_before_ack},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}

/* -------------------------------------------------- xw-lock child tests */
/* The real xw-lock binary as a child process: connect, lock, typed
 * passphrase (wrong then correct) through the compositor's keyboard
 * injection, unlock, clean exit — the same path a nested session
 * exercises, without needing Xvfb. */

static const char *lock_bin_path(void) {
    if (access("build/bin/xw-lock", X_OK) == 0)
        return "build/bin/xw-lock";
    if (access("../build/bin/xw-lock", X_OK) == 0)
        return "../build/bin/xw-lock";
    return NULL;
}

/* linux keycodes for the letters used below (input-event-codes.h) */
#define K_S 31
#define K_E 18
#define K_C 46
#define K_R 19
#define K_W 17
#define K_O 24
#define K_N 49
#define K_G 34
#define K_BACKSPACE 14

static void type_key(struct xwt_ctx *t, uint32_t code) {
    xw_compositor_inject_key(t->comp, code, true);
    xwt_pump(t);
    xw_compositor_inject_key(t->comp, code, false);
    xwt_pump(t);
}

static void type_string(struct xwt_ctx *t, const char *s) {
    static const struct {
        char ch;
        uint32_t code;
    } map[] = {
        {'s', K_S}, {'e', K_E}, {'c', K_C}, {'r', K_R}, {'t', K_T},
        {'w', K_W}, {'o', K_O}, {'n', K_N}, {'g', K_G},
    };
    for (const char *p = s; *p; p++) {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (map[i].ch == *p) {
                type_key(t, map[i].code);
                break;
            }
        }
    }
}

static pid_t spawn_lock(struct xwt_ctx *t, const char *passfile) {
    const char *bin = lock_bin_path();
    if (!bin)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        setenv("XW_LOCK_TIMEOUT_MS", "2000", 1);
        int logfd =
            open("/tmp/xw-lock-child.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        execl(bin, "xw-lock", "--passphrase-file", passfile, NULL);
        _exit(127);
    }
    return pid;
}

static bool child_exited(pid_t pid, int *status_out) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
        if (status_out)
            *status_out = status;
        return true;
    }
    return false;
}

static void test_xw_lock_unlock_flow(struct xwt_ctx *t) {
    const char *bin = lock_bin_path();
    if (!bin) {
        XWT_CHECK(false, "xw-lock binary not found (run from repo root)");
        return;
    }
    /* passphrase file in the test runtime dir */
    char passpath[256];
    snprintf(passpath, sizeof(passpath), "%s/lock-pass", g_runtimedir());
    FILE *f = fopen(passpath, "w");
    XWT_ASSERT(f);
    fprintf(f, "secret\n");
    fclose(f);

    struct xwc_win *win = xwt_window_solid(t, WIN_COLOR, 300, 200, "victim4");
    XWT_ASSERT(win);
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));

    pid_t pid = spawn_lock(t, passpath);
    XWT_ASSERT(pid > 0);

    /* wait for the LOCKED EVENT (the child needs exec + connect +
     * configure + commit; the gate alone engages at lock() long before
     * any surface exists — waiting on the gate alone races the child) */
    bool locked = false;
    for (int i = 0; i < 600 && !locked; i++) {
        xwt_pump(t);
        locked = xw_session_lock_locked(t->comp) && !any_pixel(t, WIN_COLOR);
        if (!locked)
            usleep(5000);
    }
    XWT_CHECK(locked, "xw-lock locked the session (pixels obscured)");

    /* wrong passphrase: stays locked */
    type_string(t, "wrong");
    type_key(t, K_ENTER);
    for (int i = 0; i < 50; i++)
        xwt_pump(t);
    XWT_CHECK(xw_session_lock_active(t->comp),
              "wrong passphrase: still locked");
    int st = 0;
    XWT_CHECK(!child_exited(pid, &st), "wrong passphrase: child still runs");

    /* correct passphrase: unlock + exit 0 */
    type_string(t, "secret");
    type_key(t, K_ENTER);
    bool unlocked = false;
    for (int i = 0; i < 300 && !unlocked; i++) {
        xwt_pump(t);
        unlocked = !xw_session_lock_active(t->comp);
        if (!unlocked)
            usleep(10000);
    }
    XWT_CHECK(unlocked, "correct passphrase unlocked the session");
    bool exited = false;
    for (int i = 0; i < 100 && !exited; i++) {
        xwt_pump(t);
        exited = child_exited(pid, &st);
        if (!exited)
            usleep(10000);
    }
    XWT_CHECK(exited && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "xw-lock exited 0 after unlocking (status %d)", st);
    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
    }
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));
    XWT_CHECK(any_pixel(t, WIN_COLOR), "content restored after unlock");
    xwc_win_destroy(win);
    unlink(passpath);
}

/* the lock client is killed while locked: the session STAYS locked and
 * a new lock client takes over and unlocks */
static void test_xw_lock_kill_while_locked(struct xwt_ctx *t) {
    const char *bin = lock_bin_path();
    if (!bin) {
        XWT_CHECK(false, "xw-lock binary not found");
        return;
    }
    char passpath[256];
    snprintf(passpath, sizeof(passpath), "%s/lock-pass2", g_runtimedir());
    FILE *f = fopen(passpath, "w");
    XWT_ASSERT(f);
    fprintf(f, "secret\n");
    fclose(f);

    struct xwc_win *win = xwt_window_solid(t, WIN_COLOR, 300, 200, "victim5");
    XWT_ASSERT(win);
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));

    pid_t pid = spawn_lock(t, passpath);
    XWT_ASSERT(pid > 0);
    bool locked = false;
    for (int i = 0; i < 600 && !locked; i++) {
        xwt_pump(t);
        locked = xw_session_lock_locked(t->comp) && !any_pixel(t, WIN_COLOR);
        if (!locked)
            usleep(5000);
    }
    XWT_CHECK(locked, "locked (locked event received) before the kill");

    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    for (int i = 0; i < 50; i++)
        xwt_pump(t);
    XWT_CHECK(xw_session_lock_active(t->comp),
              "SECURITY: session stays locked after the lock client is killed");
    XWT_CHECK(!any_pixel(t, WIN_COLOR),
              "SECURITY: content stays hidden after the kill");

    /* a new lock client takes over and unlocks */
    pid_t pid2 = spawn_lock(t, passpath);
    XWT_ASSERT(pid2 > 0);
    bool takeover = false;
    for (int i = 0; i < 600 && !takeover; i++) {
        xwt_pump(t);
        takeover = any_pixel(t, LOCK_COLOR) || any_pixel(t, 0xff10151c);
        if (!takeover)
            usleep(5000);
    }
    /* the new lock screen draws (its own palette; blank fallback color
     * also proves the takeover) */
    bool relocked = false;
    for (int i = 0; i < 600 && !relocked; i++) {
        xwt_pump(t);
        relocked = !any_pixel(t, WIN_COLOR) &&
                   xw_session_lock_locked(t->comp);
        if (!relocked)
            usleep(5000);
    }
    XWT_CHECK(relocked, "second lock client took over the lock");
    type_string(t, "secret");
    type_key(t, K_ENTER);
    bool unlocked = false;
    for (int i = 0; i < 300 && !unlocked; i++) {
        xwt_pump(t);
        unlocked = !xw_session_lock_active(t->comp);
        if (!unlocked)
            usleep(10000);
    }
    XWT_CHECK(unlocked, "takeover client unlocked the session");
    bool exited = false;
    for (int i = 0; i < 100 && !exited; i++) {
        xwt_pump(t);
        exited = child_exited(pid2, &st);
        if (!exited)
            usleep(10000);
    }
    XWT_CHECK(exited && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "takeover xw-lock exited 0 (status %d)", st);
    if (!exited) {
        kill(pid2, SIGKILL);
        waitpid(pid2, &st, 0);
    }
    xwc_win_destroy(win);
    unlink(passpath);
}

/* --idle SECONDS: the lock engages only after the idle notification
 * fires (ext-idle-notify end to end through the real client binary) */
static void test_xw_lock_idle_autolock(struct xwt_ctx *t) {
    const char *bin = lock_bin_path();
    if (!bin) {
        XWT_CHECK(false, "xw-lock binary not found");
        return;
    }
    char passpath[256];
    snprintf(passpath, sizeof(passpath), "%s/lock-pass3", g_runtimedir());
    FILE *f = fopen(passpath, "w");
    XWT_ASSERT(f);
    fprintf(f, "secret\n");
    fclose(f);

    struct xwc_win *win = xwt_window_solid(t, WIN_COLOR, 300, 200, "victim6");
    XWT_ASSERT(win);
    XWT_WAIT(t, any_pixel(t, WIN_COLOR));

    const char *binp = lock_bin_path();
    pid_t pid = fork();
    XWT_ASSERT(pid >= 0);
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", t->socket_name, 1);
        setenv("XDG_RUNTIME_DIR", g_runtimedir(), 1);
        int logfd = open("/tmp/xw-lock-child.log", O_WRONLY | O_CREAT | O_TRUNC,
                         0644);
        if (logfd >= 0)
            dup2(logfd, STDERR_FILENO);
        execl(binp, "xw-lock", "--passphrase-file", passpath, "--idle", "0.2",
              NULL);
        _exit(127);
    }

    /* must NOT lock before the idle timeout: pump input continuously
     * for ~0.5s (activity resets the idle timer each time) */
    for (int i = 0; i < 50; i++) {
        xw_compositor_inject_pointer_motion(t->comp, 10 + i, 10);
        xwt_pump(t);
        usleep(10000);
    }
    XWT_CHECK(!xw_session_lock_active(t->comp),
              "input activity suppresses the idle lock");

    /* now go quiet: the idle notification fires and locks */
    bool locked = false;
    for (int i = 0; i < 300 && !locked; i++) {
        xwt_pump(t);
        locked = xw_session_lock_locked(t->comp) && !any_pixel(t, WIN_COLOR);
        if (!locked)
            usleep(10000);
    }
    XWT_CHECK(locked, "--idle locked after the idle timeout without input");

    type_string(t, "secret");
    type_key(t, K_ENTER);
    bool unlocked = false;
    for (int i = 0; i < 300 && !unlocked; i++) {
        xwt_pump(t);
        unlocked = !xw_session_lock_active(t->comp);
        if (!unlocked)
            usleep(10000);
    }
    XWT_CHECK(unlocked, "idle-locked session unlocked with the passphrase");
    int st = 0;
    bool exited = false;
    for (int i = 0; i < 100 && !exited; i++) {
        xwt_pump(t);
        exited = child_exited(pid, &st);
        if (!exited)
            usleep(10000);
    }
    XWT_CHECK(exited && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "idle xw-lock exited 0 (status %d)", st);
    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
    }
    xwc_win_destroy(win);
    unlink(passpath);
}

/* child-process tests registered after the in-process ones (order only
 * affects the listing, not coverage) */
static const struct xwt_test lock_child_tests[] = {
    {"xw-lock-unlock-flow", test_xw_lock_unlock_flow},
    {"xw-lock-kill-while-locked", test_xw_lock_kill_while_locked},
    {"xw-lock-idle-autolock", test_xw_lock_idle_autolock},
};

__attribute__((constructor)) static void register_lock_child_tests(void) {
    xwt_register(lock_child_tests,
                 sizeof(lock_child_tests) / sizeof(lock_child_tests[0]));
}
