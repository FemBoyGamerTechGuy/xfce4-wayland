/* keyboardprobe.c — the minimal raw Wayland keyboard client.
 *
 * Physical-debug instrument for the "Backspace types u" family: this
 * program is a RAW wl_keyboard consumer (no toolkit, no libxwcl) that
 * records the EXACT events the compositor puts on the wire and decodes
 * them the way a compliant client must (its own xkb state built from
 * the delivered keymap + modifiers events).
 *
 * Usage:
 *   keyboardprobe <socket-name> [seconds]
 *     <socket-name>  the compositor's wl socket (e.g. xw-0)
 *     [seconds]      run duration, default 60; SIGINT ends early
 *
 * What it prints (stdout, line-buffered; tail it live):
 *   keymap:    size + a spot-check keycode->keysym table. If wl 22 does
 *              not decode to BackSpace here, the keymap itself is in
 *              the wrong keycode space — that is the bug, no key
 *              pressing needed.
 *   enter/leave/modifiers/key/repeat_info: every wire event with its
 *              serial, and for key events the client-side decode
 *              (keysym name + utf8 text) — what a real text app would
 *              "type" for that event.
 *   ANOMALY:   wire-level inconsistencies: keycode < 8, serial not
 *              increasing, key release without a press, enter with
 *              keys already down, or a decode that maps the Backspace
 *              keycode to a printable letter (the literal symptom).
 *   summary:   event counts + anomaly count.
 *
 * Reading it against XW_INPUT_TRACE=1 on the compositor side: every
 * "key: raw=N wl=M ... delivered ... serial=S" line must match a
 * "wire key" line here (same M and S). If they match and the decode
 * here shows 'u' for Backspace, the compositor is fine and the
 * interpretation is broken; if they do not match, delivery is broken.
 *
 * Build-time linkage: libwayland-client + libxkbcommon.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include "wayland-client.h"
#include "xdg-shell.h"

/* ---------------------------------------------------------------- state */

struct probe {
    struct wl_display *d;
    struct wl_registry *reg;
    struct wl_seat *seat;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surf;
    struct xdg_surface *xdg;
    struct xdg_toplevel *top;
    struct wl_buffer *buf;
    struct wl_keyboard *kb;

    /* client-side xkb (built from the delivered keymap) */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    char *keymap_text; /* the mmap'ed keymap text (kept mapped: xkb
                          parses lazily on lookup) */
    size_t keymap_text_size;

    bool configured;
    bool mapped;
    bool running;

    /* counters + sanity */
    uint32_t last_serial;
    int n_keymap, n_enter, n_leave, n_key, n_mods, n_repeat;
    int n_anom;
    /* per-wl-keycode press state: 512 is the wl_keyboard key limit */
    uint8_t pressed[512];
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* full teardown for EVERY exit path: proxies + the client-side xkb
 * objects. Keeps the probe LSan-clean under the asan build (the
 * round-2 note "destroys every proxy" missed the xkb side — and LSan
 * ends the process with _exit, which also skips the stdio flush,
 * eating the summary line; hence the explicit fflush at the end) */
static void probe_teardown(struct probe *p) {
    if (p->state) {
        xkb_state_unref(p->state);
        p->state = NULL;
    }
    if (p->keymap) {
        xkb_keymap_unref(p->keymap);
        p->keymap = NULL;
    }
    if (p->keymap_text) {
        munmap(p->keymap_text, p->keymap_text_size);
        p->keymap_text = NULL;
        p->keymap_text_size = 0;
    }
    if (p->xkb_ctx) {
        xkb_context_unref(p->xkb_ctx);
        p->xkb_ctx = NULL;
    }
    if (p->top)
        xdg_toplevel_destroy(p->top);
    if (p->xdg)
        xdg_surface_destroy(p->xdg);
    if (p->surf)
        wl_surface_destroy(p->surf);
    if (p->buf)
        wl_buffer_destroy(p->buf);
    if (p->kb)
        wl_keyboard_release(p->kb);
    if (p->wm_base)
        xdg_wm_base_destroy(p->wm_base);
    if (p->shm)
        wl_shm_destroy(p->shm);
    if (p->comp)
        wl_compositor_destroy(p->comp);
    if (p->seat)
        wl_seat_release(p->seat);
    if (p->reg)
        wl_registry_destroy(p->reg);
    if (p->d)
        wl_display_disconnect(p->d);
}

static void anomaly(struct probe *p, const char *fmt, ...) {
    va_list ap;
    fputs("ANOMALY: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    p->n_anom++;
}

/* ------------------------------------------------------------ keyboard */

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                      int32_t fd, uint32_t size) {
    (void)kb;
    struct probe *p = data;
    p->n_keymap++;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        anomaly(p, "keymap format %u is not xkb-v1", format);
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        anomaly(p, "keymap mmap failed (%zu bytes)", (size_t)size);
        close(fd);
        return;
    }
    if (p->keymap) {
        xkb_keymap_unref(p->keymap);
        xkb_state_unref(p->state);
        p->keymap = NULL;
        p->state = NULL;
    }
    p->keymap = xkb_keymap_new_from_buffer(
        p->xkb_ctx, map, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!p->keymap) {
        anomaly(p, "delivered keymap failed to compile (%zu bytes)",
                (size_t)size);
        munmap(map, size);
        close(fd);
        return;
    }
    if (p->keymap_text) {
        munmap(p->keymap_text, p->keymap_text_size);
        p->keymap_text = NULL;
        p->keymap_text_size = 0;
    }
    p->keymap_text = map;
    p->keymap_text_size = size;
    p->state = xkb_state_new(p->keymap);
    close(fd);

    printf("keymap: %u bytes, min_keycode=%u max_keycode=%u\n", size,
           xkb_keymap_min_keycode(p->keymap),
           xkb_keymap_max_keycode(p->keymap));

    /* spot-check table: wl keycode -> keysym name. These are the
     * physical keys of the report matrix; if the right-hand column is
     * shifted by one row (22 -> 'u' instead of BackSpace), the keymap
     * is in a raw/evdev keycode space instead of evdev+8 — that single
     * line is the root cause of "Backspace types u". */
    static const struct {
        uint32_t code;
        const char *wl_name;
        const char *expect;
    } spot[] = {
        {22, "22 (raw 14, KEY_BACKSPACE)", "BackSpace"},
        {23, "23 (raw 15, KEY_TAB)", "Tab"},
        {24, "24 (raw 16, KEY_Q)", "q"},
        {30, "30 (raw 22, KEY_U)", "u"},
        {36, "36 (raw 28, KEY_ENTER)", "Return"},
        {38, "38 (raw 30, KEY_A)", "a"},
        {37, "37 (raw 29, KEY_LEFTCTRL)", "Control_L"},
        {50, "50 (raw 42, KEY_LEFTSHIFT)", "Shift_L"},
        {67, "67 (raw 59, KEY_F1)", "F1"},
    };
    for (size_t i = 0; i < sizeof(spot) / sizeof(spot[0]); i++) {
        xkb_keysym_t sym =
            xkb_state_key_get_one_sym(p->state, spot[i].code);
        char name[64] = "(none)";
        if (sym != XKB_KEY_NoSymbol)
            xkb_keysym_get_name(sym, name, sizeof(name));
        const char *mark = strcmp(name, spot[i].expect) == 0
                               ? ""
                               : "   <-- MISMATCH (keymap space?)";
        printf("keymap: wl %s -> %-12s (expected %s)%s\n", spot[i].wl_name,
               name, spot[i].expect, mark);
        if (mark[0])
            anomaly(p, "keymap spot-check: wl keycode %u decodes to %s, "
                       "expected %s — keycode-space mismatch",
                    spot[i].code, name, spot[i].expect);
    }
    fflush(stdout);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface, struct wl_array *keys) {
    (void)kb;
    struct probe *p = data;
    p->n_enter++;
    p->last_serial = serial;
    printf("enter: serial=%u surface=%u keys_down=%u [", serial,
           (unsigned)(uintptr_t)surface,
           (unsigned)(keys->size / sizeof(uint32_t)));
    uint32_t *k = keys->data;
    for (size_t i = 0; i < keys->size / sizeof(uint32_t); i++)
        printf("%s%u", i ? " " : "", k[i]);
    printf("]\n");
    if (keys->size)
        anomaly(p, "enter carried %zu keys already down (stale state?)",
                keys->size / sizeof(uint32_t));
    /* the probe's press mirror must be empty here too */
    for (uint32_t c = 0; c < 512; c++) {
        if (p->pressed[c] && !(keys->size &&
                               memchr(keys->data, c, keys->size))) {
            anomaly(p, "probe thought wl %u was down at enter", c);
            p->pressed[c] = 0;
        }
    }
    (void)surface;
    fflush(stdout);
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surface) {
    (void)kb;
    (void)surface;
    struct probe *p = data;
    p->n_leave++;
    p->last_serial = serial;
    printf("leave: serial=%u\n", serial);
    fflush(stdout);
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
    (void)kb;
    struct probe *p = data;
    p->n_key++;
    if (serial <= p->last_serial)
        anomaly(p, "key serial %u not > last serial %u", serial,
                p->last_serial);
    p->last_serial = serial;

    const char *st =
        state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release";
    if (key >= 512) {
        anomaly(p, "wl keycode %u out of range", key);
        printf("key: serial=%u time=%u wl=%u %s (OUT OF RANGE)\n", serial,
               time, key, st);
        return;
    }
    if (key < 8)
        anomaly(p, "wl keycode %u < 8 (raw linux code sent un-offset?)",
                key);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (p->pressed[key])
            anomaly(p, "press of wl %u while already down (double "
                       "delivery or lost release)",
                    key);
        p->pressed[key] = 1;
    } else {
        if (!p->pressed[key])
            anomaly(p, "release of wl %u without a matching press", key);
        p->pressed[key] = 0;
    }

    /* the compliant-client decode: this is exactly what a text
     * application computes from the wire stream */
    char symname[64] = "(?)";
    char text[16] = "";
    if (p->state) {
        xkb_state_update_key(p->state, key,
                             state == WL_KEYBOARD_KEY_STATE_PRESSED
                                 ? XKB_KEY_DOWN
                                 : XKB_KEY_UP);
        xkb_keysym_t sym = xkb_state_key_get_one_sym(p->state, key);
        if (sym != XKB_KEY_NoSymbol)
            xkb_keysym_get_name(sym, symname, sizeof(symname));
        ssize_t n = xkb_state_key_get_utf8(p->state, key, text,
                                           sizeof(text));
        if (n < 0)
            n = 0;
        text[n] = 0;
        /* the literal symptom: the BackSpace keycode decoding to a
         * printable letter */
        if (key == 22 && sym >= 0x20 && sym < 0x7f)
            anomaly(p, "wl keycode 22 (BackSpace physical key) decoded "
                       "to printable '%s' — this IS the "
                       "backspace-types-a-letter bug",
                    symname);
    }
    printf("key: serial=%u time=%u wl=%u %s -> client decodes: keysym "
           "%s text='%s'\n",
           serial, time, key, st, symname, text);
    fflush(stdout);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb,
                         uint32_t serial, uint32_t depressed, uint32_t latched,
                         uint32_t locked, uint32_t group) {
    (void)kb;
    struct probe *p = data;
    p->n_mods++;
    if (serial <= p->last_serial)
        anomaly(p, "modifiers serial %u not > last serial %u", serial,
                p->last_serial);
    p->last_serial = serial;
    if (p->state)
        xkb_state_update_mask(p->state, depressed, latched, locked, 0, 0,
                              group);
    printf("mods: serial=%u dep=%u lat=%u lock=%u grp=%u\n", serial,
           depressed, latched, locked, group);
    fflush(stdout);
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                           int32_t delay) {
    (void)kb;
    struct probe *p = data;
    p->n_repeat++;
    printf("repeat_info: rate=%d delay=%d\n", rate, delay);
    (void)p;
    fflush(stdout);
}

static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

/* ---------------------------------------------------------- registry */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    struct probe *p = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !p->kb) {
        p->kb = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(p->kb, &kb_listener, p);
        printf("seat: keyboard bound (caps=%u)\n", caps);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && p->kb) {
        wl_keyboard_destroy(p->kb);
        p->kb = NULL;
        printf("seat: keyboard gone (caps=%u)\n", caps);
    }
    fflush(stdout);
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    printf("seat: name=%s\n", name);
    fflush(stdout);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    struct probe *p = data;
    if (strcmp(iface, "wl_seat") == 0) {
        if (version > 8)
            version = 8;
        p->seat = wl_registry_bind(r, name, &wl_seat_interface, version);
        wl_seat_add_listener(p->seat, &seat_listener, p);
    } else if (strcmp(iface, "wl_compositor") == 0) {
        if (version > 6)
            version = 6;
        p->comp =
            wl_registry_bind(r, name, &wl_compositor_interface, version);
    } else if (strcmp(iface, "wl_shm") == 0) {
        p->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        if (version > 6)
            version = 6;
        p->wm_base =
            wl_registry_bind(r, name, &xdg_wm_base_interface, version);
    }
}

static void reg_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

/* ------------------------------------------------------- xdg surface */

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_configure(void *data, struct xdg_surface *s,
                          uint32_t serial) {
    struct probe *p = data;
    xdg_surface_ack_configure(s, serial);
    /* the map commit: per xdg-shell the first commit carrying a buffer
     * must follow the first ack_configure. Attaching before that (the
     * pre-configure commit) only arms the state; THIS one maps the
     * toplevel and makes it focusable. */
    if (!p->configured) {
        p->configured = true;
        wl_surface_attach(p->surf, p->buf, 0, 0);
        wl_surface_damage_buffer(p->surf, 0, 0, 96, 96);
        wl_surface_commit(p->surf);
    }
}

static const struct xdg_surface_listener xdg_listener = {
    .configure = xdg_configure,
};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
                          int32_t h, struct wl_array *states) {
    (void)data;
    (void)states;
    printf("toplevel: configure %dx%d\n", w, h);
    (void)t;
    fflush(stdout);
}

static void top_close(void *data, struct xdg_toplevel *t) {
    (void)t;
    struct probe *p = data;
    p->running = false;
}

static const struct xdg_toplevel_listener top_listener = {
    .configure = top_configure,
    .close = top_close,
};

/* make a 96x96 solid buffer (wl_shm) so the toplevel can commit and
 * map — the compositor focuses mapped toplevels, which is what routes
 * the keyboard to this client */
static bool make_buffer(struct probe *p) {
    int w = 96, h = 96, stride = w * 4;
    int fd = memfd_create("kbprobe", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, stride * h) < 0) {
        close(fd);
        return false;
    }
    uint32_t *pix = mmap(NULL, stride * h, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (pix == MAP_FAILED) {
        close(fd);
        return false;
    }
    for (int i = 0; i < w * h; i++)
        pix[i] = 0xff8844ccu; /* argb: solid purple, hard to miss */
    munmap(pix, stride * h);
    struct wl_shm_pool *pool = wl_shm_create_pool(p->shm, fd, stride * h);
    p->buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                       WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return true;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <socket-name> [seconds]\n", argv[0]);
        return 2;
    }
    int seconds = argc > 2 ? atoi(argv[2]) : 60;
    if (seconds <= 0)
        seconds = 60;

    struct probe p;
    memset(&p, 0, sizeof(p));
    p.running = true;
    p.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!p.xkb_ctx) {
        fprintf(stderr, "xkb_context_new failed\n");
        return 3;
    }

    p.d = wl_display_connect(argv[1]);
    if (!p.d) {
        fprintf(stderr, "connect to '%s' failed (XDG_RUNTIME_DIR?)\n",
                argv[1]);
        probe_teardown(&p);
        return 4;
    }
    p.reg = wl_display_get_registry(p.d);
    wl_registry_add_listener(p.reg, &reg_listener, &p);
    wl_display_roundtrip(p.d);
    if (!p.seat || !p.comp || !p.shm || !p.wm_base) {
        fprintf(stderr,
                "missing globals (seat=%p comp=%p shm=%p wm_base=%p)\n",
                (void *)p.seat, (void *)p.comp, (void *)p.shm,
                (void *)p.wm_base);
        probe_teardown(&p);
        return 5;
    }
    wl_display_roundtrip(p.d); /* seat caps -> wl_keyboard + keymap */

    xdg_wm_base_add_listener(p.wm_base, &wm_base_listener, &p);
    p.surf = wl_compositor_create_surface(p.comp);
    p.xdg = xdg_wm_base_get_xdg_surface(p.wm_base, p.surf);
    xdg_surface_add_listener(p.xdg, &xdg_listener, &p);
    p.top = xdg_surface_get_toplevel(p.xdg);
    xdg_toplevel_add_listener(p.top, &top_listener, &p);
    xdg_toplevel_set_title(p.top, "keyboard-probe");
    xdg_toplevel_set_app_id(p.top, "xw.keyboardprobe");
    if (!make_buffer(&p)) {
        fprintf(stderr, "buffer creation failed\n");
        probe_teardown(&p);
        return 6;
    }
    /* the pre-configure commit (empty role commit): asks the server
     * for the initial configure; the buffer attach happens in the
     * configure callback, after the ack — that order is what maps the
     * window under strict xdg-shell */
    wl_surface_commit(p.surf);
    wl_display_roundtrip(p.d);
    wl_display_roundtrip(p.d);

    printf("probe: window mapped — click it once, then type the matrix "
           "(Backspace, letters, Shift+Backspace, Ctrl+Backspace, "
           "modifier press/release)...\n");
    fflush(stdout);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    alarm(seconds + 5);

    int fd = wl_display_get_fd(p.d);
    long deadline = (long)time(NULL) + seconds;
    while (p.running && !g_stop) {
        wl_display_flush(p.d);
        struct pollfd pf = {fd, POLLIN, 0};
        int r = poll(&pf, 1, 200);
        if (r > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            if (wl_display_dispatch(p.d) < 0) {
                anomaly(&p, "wl_display_dispatch failed (compositor died?)");
                break;
            }
        }
        if ((long)time(NULL) >= deadline)
            break;
    }

    printf("\nsummary: keymap=%d enter=%d leave=%d key=%d mods=%d "
           "repeat=%d anomalies=%d\n",
           p.n_keymap, p.n_enter, p.n_leave, p.n_key, p.n_mods, p.n_repeat,
           p.n_anom);
    printf("verdict: %s\n",
           p.n_anom == 0
               ? (p.n_key > 0 ? "wire stream self-consistent" : "no keys "
                                                          "observed — did "
                                                          "the probe get "
                                                          "focus?")
               : "ANOMALIES ABOVE — each line is a finding");
    fflush(stdout); /* durable before teardown AND before any exit-time
                     * leak report's _exit (which skips the flush) */
    probe_teardown(&p);
    return p.n_anom == 0 ? 0 : 1;
}
