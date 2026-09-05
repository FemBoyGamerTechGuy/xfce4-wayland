/* geomstorm.c — the external geometry-trace volume driver.
 *
 * Session-level regression instrument for the trace-observational
 * contract: XW_INPUT_TRACE / XW_GEOMETRY_TRACE must never change
 * session behavior — in particular logout. The in-suite white-box
 * regression (test_seat.c) storms the compositor in-process; THIS
 * tool storms a REAL compositor process from OUTSIDE, so a test can
 * drive it through the REAL supervisor chain (xw-session), which is
 * how the physical machine actually runs.
 *
 * Volume mechanism: an xdg_toplevel that commits two alternating
 * buffer sizes in a tight loop. Every size-changing commit crosses
 * the WM's toplevel_apply_commit "xdg-commit-size" transition, which
 * prints one full [geom] line under XW_GEOMETRY_TRACE=1 (and nothing
 * without it — the control case proves the storm itself is inert).
 * No input events are needed, no seat is bound, nothing touches the
 * input path: the volume is pure geometry, exactly the instrument
 * whose sink discipline is under test.
 *
 * Usage:   geomstorm <socket-name> [seconds]   (default 30)
 * Prints:  one heartbeat line per second on stdout
 *            storm: N commits (M/sec), running
 *          a summary before exit; exit 0 on SIGINT/SIGTERM/timeout.
 * The heartbeat doubles as the liveness proof for the harness: a
 * compositor wedged on its stderr sink stops consuming commits, the
 * wayland socket fills, and this process blocks in flush — which the
 * harness can see (heartbeat stops) and clean up.
 *
 * Build-time linkage: libwayland-client + the generated xdg-shell
 * protocol glue (same as keyboardprobe, minus xkb).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "wayland-client.h"
#include "xdg-shell.h"

struct storm {
    struct wl_display *d;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surf;
    struct xdg_surface *xdg;
    struct xdg_toplevel *top;

    struct wl_buffer *buf_a; /* 96x96  */
    struct wl_buffer *buf_b; /* 110x84 */

    bool configured; /* first xdg configure arrived + acked */
    bool running;
    long commits;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* full teardown on EVERY exit path: keeps LSan clean under the asan
 * build (same discipline as keyboardprobe). */
static void storm_teardown(struct storm *s) {
    if (s->top)
        xdg_toplevel_destroy(s->top);
    if (s->xdg)
        xdg_surface_destroy(s->xdg);
    if (s->surf)
        wl_surface_destroy(s->surf);
    if (s->buf_a)
        wl_buffer_destroy(s->buf_a);
    if (s->buf_b)
        wl_buffer_destroy(s->buf_b);
    if (s->wm_base)
        xdg_wm_base_destroy(s->wm_base);
    if (s->shm)
        wl_shm_destroy(s->shm);
    if (s->comp)
        wl_compositor_destroy(s->comp);
    if (s->reg)
        wl_registry_destroy(s->reg);
    if (s->d)
        wl_display_disconnect(s->d);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------ protocol */

static void wm_ping(void *data, struct xdg_wm_base *wb, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wb, serial);
}

static const struct xdg_wm_base_listener wm_listener = {
    .ping = wm_ping,
};

static void s_configure(void *data, struct xdg_surface *xdg,
                        uint32_t serial) {
    (void)xdg;
    struct storm *s = data;
    xdg_surface_ack_configure(xdg, serial);
    s->configured = true;
}

static const struct xdg_surface_listener s_listener = {
    .configure = s_configure,
};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
                          int32_t h, struct wl_array *states) {
    (void)data;
    (void)t;
    (void)w;
    (void)h;
    (void)states;
}

static void top_close(void *data, struct xdg_toplevel *t) {
    (void)t;
    struct storm *s = data;
    s->running = false; /* the compositor closed us (e.g. shutdown) */
}

static const struct xdg_toplevel_listener top_listener = {
    .configure = top_configure,
    .close = top_close,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    struct storm *s = data;
    if (strcmp(iface, "wl_compositor") == 0)
        s->comp = wl_registry_bind(r, name, &wl_compositor_interface,
                                   version < 4 ? version : 4);
    else if (strcmp(iface, "wl_shm") == 0)
        s->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(iface, "xdg_wm_base") == 0) {
        s->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
                                      version < 2 ? version : 2);
        xdg_wm_base_add_listener(s->wm_base, &wm_listener, s);
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

/* a solid wl_shm buffer of the given size (contents irrelevant — the
 * WM traces the SIZE transition, and the paint must only be valid
 * enough for a commit to be accepted) */
static struct wl_buffer *make_buffer(struct storm *s, int w, int h) {
    int stride = w * 4;
    int fd = memfd_create("geomstorm", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, stride * h) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *pix = mmap(NULL, (size_t)stride * h, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (pix == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (int i = 0; i < w * h; i++)
        pix[i] = 0xff334d66u;
    munmap(pix, (size_t)stride * h);
    struct wl_shm_pool *pool =
        wl_shm_create_pool(s->shm, fd, stride * h);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <socket-name> [seconds]\n", argv[0]);
        return 2;
    }
    int seconds = argc > 2 ? atoi(argv[2]) : 30;
    if (seconds <= 0)
        seconds = 30;

    struct storm s;
    memset(&s, 0, sizeof(s));
    s.running = true;

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    s.d = wl_display_connect(argv[1]);
    if (!s.d) {
        fprintf(stderr, "geomstorm: connect to '%s' failed "
                        "(XDG_RUNTIME_DIR?)\n",
                argv[1]);
        return 4;
    }
    s.reg = wl_display_get_registry(s.d);
    wl_registry_add_listener(s.reg, &reg_listener, &s);
    wl_display_roundtrip(s.d);
    if (!s.comp || !s.shm || !s.wm_base) {
        fprintf(stderr, "geomstorm: missing globals (comp=%p shm=%p "
                        "wm_base=%p)\n",
                (void *)s.comp, (void *)s.shm, (void *)s.wm_base);
        storm_teardown(&s);
        return 5;
    }

    s.surf = wl_compositor_create_surface(s.comp);
    s.xdg = xdg_wm_base_get_xdg_surface(s.wm_base, s.surf);
    xdg_surface_add_listener(s.xdg, &s_listener, &s);
    s.top = xdg_surface_get_toplevel(s.xdg);
    xdg_toplevel_add_listener(s.top, &top_listener, &s);
    xdg_toplevel_set_title(s.top, "GeomStorm");
    xdg_toplevel_set_app_id(s.top, "xw.geomstorm");
    wl_surface_commit(s.surf);

    /* wait for the first configure (bounded) */
    int64_t deadline = now_ms() + 5000;
    while (!s.configured && now_ms() < deadline && s.running)
        wl_display_dispatch(s.d);
    if (!s.configured) {
        fprintf(stderr, "geomstorm: no initial configure within 5s\n");
        storm_teardown(&s);
        return 6;
    }

    s.buf_a = make_buffer(&s, 96, 96);
    s.buf_b = make_buffer(&s, 110, 84);
    if (!s.buf_a || !s.buf_b) {
        fprintf(stderr, "geomstorm: buffer creation failed\n");
        storm_teardown(&s);
        return 7;
    }

    /* the storm: alternate the two buffer sizes, committing as fast
     * as the connection can actually drain. Every size change is one
     * "xdg-commit-size" [geom] transition in the compositor. The
     * flush-after-every-commit pacing is the point: the storm rate
     * tracks the compositor's REAL consumption rate, and when the
     * compositor wedges (the pre-fix stderr sink) this process parks
     * in POLLOUT — the heartbeat stops, which is the harness's
     * independent wedge observable. */
    int64_t start = now_ms();
    int64_t last_beat = start;
    long last_commits = 0;
    int64_t stop_at = start + (int64_t)seconds * 1000;
    while (s.running && !g_stop && now_ms() < stop_at) {
        struct wl_buffer *b = (s.commits & 1) ? s.buf_b : s.buf_a;
        wl_surface_attach(s.surf, b, 0, 0);
        wl_surface_damage(s.surf, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(s.surf);
        s.commits++;

        for (;;) {
            if (wl_display_flush(s.d) >= 0)
                break;
            if (wl_display_get_error(s.d)) {
                fprintf(stderr, "geomstorm: connection error\n");
                s.running = false;
                break;
            }
            struct pollfd pf = {.fd = wl_display_get_fd(s.d),
                                .events = POLLOUT};
            int rc = poll(&pf, 1, 200);
            if (rc < 0 && errno != EINTR) {
                fprintf(stderr, "geomstorm: poll failed (%s)\n",
                        strerror(errno));
                s.running = false;
                break;
            }
            if (rc == 1 && (pf.revents & (POLLERR | POLLHUP))) {
                fprintf(stderr, "geomstorm: compositor side went away\n");
                s.running = false;
                break;
            }
        }
        wl_display_dispatch_pending(s.d);
        /* drain the server's event stream too: every size change emits
         * a foreign-toplevel notify, and a client that never reads
         * overflows the server's 4096-byte connection buffer ("Data
         * too big for buffer") and gets dropped mid-storm */
        {
            struct pollfd pf = {.fd = wl_display_get_fd(s.d),
                                .events = POLLIN};
            if (poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN))
                wl_display_dispatch(s.d);
        }

        int64_t now = now_ms();
        if (now - last_beat >= 1000) {
            printf("storm: %ld commits (%ld/sec), %s\n", s.commits,
                   s.commits - last_commits,
                   g_stop ? "stopping" : "running");
            fflush(stdout);
            last_beat = now;
            last_commits = s.commits;
        }
    }

    printf("storm: done, %ld commits in %.1fs\n", s.commits,
           (double)(now_ms() - start) / 1000.0);
    fflush(stdout);
    storm_teardown(&s);
    return 0;
}
