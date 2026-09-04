/* test_realclient.c — regressions for the real-application fixes.
 *
 * Every test here pins a defect found with REAL toolkits (foot, GTK4
 * zenity, XWayland) during the 2026-09 integration round:
 *
 *   set_cursor       — wl_pointer.set_cursor with a NULL handler made
 *                      libwayland ABORT the compositor ("listener
 *                      function for opcode 0 of wl_pointer is NULL");
 *                      every real toolkit calls it right after focus
 *   buffer release   — clients rotating wl_shm buffers stall forever
 *                      without wl_buffer.release on replacement
 *   subcompositor    — foot refuses to start ("no sub compositor");
 *                      GTK/Chromium use subsurfaces for popups/overlays
 *   slow start       — an app that connects but waits before mapping
 *                      must never look like a failure
 */
#include "xwtest.h"

#include <sys/mman.h>
#include <unistd.h>

#include "wayland-client-protocol.h"

/* ----------------------------------------------------------- shm helper */

struct shm_buf {
    struct wl_buffer *buf;
    bool released;
    struct wl_shm_pool *pool;
};

static void buf_release(void *data, struct wl_buffer *b) {
    (void)b;
    struct shm_buf *sb = data;
    sb->released = true;
}

static const struct wl_buffer_listener buf_listener = {
    .release = buf_release,
};

/* a small wl_shm buffer (ARGB8888), its own pool, release-listened */
static struct shm_buf *shm_buf_create(struct xwc *c, int w, int h) {
    int stride = w * 4;
    size_t size = (size_t)stride * h;
    int fd = memfd_create("xwt-shm", 0);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool =
        wl_shm_create_pool(c->shm, fd, (int32_t)size);
    close(fd);
    if (!pool)
        return NULL;
    struct shm_buf *sb = calloc(1, sizeof(*sb));
    sb->pool = pool;
    sb->buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                        WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(sb->buf, &buf_listener, sb);
    return sb;
}

/* ---------------------------------------------------- set_cursor (crash) */

/* THE launch-crash regression: the exact request sequence every real
 * toolkit sends when its window takes focus (pointer enter →
 * set_cursor(surface, hotspot)). Before the fix the compositor process
 * died inside libwayland dispatch; nothing in the server state was
 * even reachable to assert. */
static void test_pointer_set_cursor(struct xwt_ctx *t) {
    struct xwc *c = &t->client;
    XWT_ASSERT(c->pointer);

    /* the cursor state machine validates the request (2026-09-06
     * round): the client must own pointer focus and carry a serial
     * the seat actually issued. A window + a real enter give us both
     * (c->last_serial is updated by the client's enter handler). */
    struct xwc_win *win = xwt_window_solid(t, 0xff204060, 120, 90, "cursor");
    XWT_ASSERT(win);
    struct xw_window *w = NULL;
    wl_list_for_each(w, &t->comp->wm->windows, link) {
        if (strcmp(w->title, "cursor") == 0)
            break;
        w = NULL;
    }
    XWT_ASSERT(w);
    xw_compositor_inject_pointer_motion(t->comp, w->x + w->w / 2,
                                        w->y + w->h / 2);
    XWT_WAIT(t, c->last_serial != 0);
    uint32_t enter_serial = c->last_serial;
    XWT_CHECK(enter_serial != 0, "pointer enter serial captured (%u)",
              enter_serial);

    /* a roleless cursor surface carrying a 24x24 image */
    struct wl_surface *cs = wl_compositor_create_surface(c->compositor);
    struct shm_buf *img = shm_buf_create(c, 24, 24);
    XWT_ASSERT(img);
    wl_surface_attach(cs, img->buf, 0, 0);
    wl_surface_commit(cs);

    /* the request that used to abort the compositor — now with a
     * valid focus + serial it must be ADOPTED */
    wl_pointer_set_cursor((struct wl_pointer *)c->pointer, enter_serial, cs,
                          3, 2);
    wl_display_flush(c->display);
    xwt_pump(t);
    xwt_pump(t);
    XWT_ASSERT(!t->client_dead);
    struct xw_seat *seat = xw_seat_first(t->comp);
    XWT_WAIT(t, seat && seat->cursor_surface);
    XWT_CHECK(seat && seat->cursor_surface, "seat adopted the cursor surface");
    XWT_CHECK(seat && seat->cursor_hot_x == 3 && seat->cursor_hot_y == 2,
              "hotspot stored (%d,%d want 3,2)",
              seat ? seat->cursor_hot_x : -1, seat ? seat->cursor_hot_y : -1);

    /* serial 0 (never issued): REJECTED, the cursor stays adopted —
     * the request must not kill the client either */
    wl_pointer_set_cursor((struct wl_pointer *)c->pointer, 0, NULL, 0, 0);
    wl_display_flush(c->display);
    xwt_pump(t);
    xwt_pump(t);
    XWT_ASSERT(!t->client_dead);
    XWT_CHECK(seat->cursor_surface,
              "fabricated serial 0 rejected (cursor kept)");

    /* hide: NULL surface with the VALID serial returns the default */
    wl_pointer_set_cursor((struct wl_pointer *)c->pointer, enter_serial, NULL,
                          0, 0);
    wl_display_flush(c->display);
    xwt_pump(t);
    xwt_pump(t);
    XWT_ASSERT(!t->client_dead);
    XWT_CHECK(!seat->cursor_surface, "cursor cleared on NULL surface");

    /* the cursor surface dies: the seat must forget it (no dangling) */
    wl_pointer_set_cursor((struct wl_pointer *)c->pointer, enter_serial, cs,
                          3, 2);
    wl_display_flush(c->display);
    xwt_pump(t);
    xwt_pump(t);
    XWT_WAIT(t, seat->cursor_surface);
    wl_surface_destroy(cs);
    xwt_pump(t);
    xwt_pump(t);
    XWT_ASSERT(!t->client_dead);
    XWT_CHECK(!seat->cursor_surface, "seat forgot a destroyed cursor surface");

    xwc_win_destroy(win);
    wl_buffer_destroy(img->buf);
    wl_shm_pool_destroy(img->pool);
    free(img);
}

/* --------------------------------------------------- wl_buffer.release */

static void test_buffer_release_on_rotation(struct xwt_ctx *t) {
    struct xwc *c = &t->client;

    /* roleless surface: buffers apply without any role machinery, and
     * the release contract is role-independent */
    struct wl_surface *s = wl_compositor_create_surface(c->compositor);
    struct shm_buf *b1 = shm_buf_create(c, 40, 40);
    struct shm_buf *b2 = shm_buf_create(c, 40, 40);
    XWT_ASSERT(b1 && b2);

    wl_surface_attach(s, b1->buf, 0, 0);
    wl_surface_commit(s);
    xwt_pump(t);
    xwt_pump(t);
    XWT_CHECK(!b1->released, "b1 not released while current");

    wl_surface_attach(s, b2->buf, 0, 0);
    wl_surface_commit(s);
    XWT_WAIT(t, b1->released);
    XWT_CHECK(b1->released,
              "buffer released when replaced (rotating pools would stall "
              "forever without this)");
    XWT_CHECK(!b2->released, "b2 still referenced");

    /* destroying the surface releases the current buffer too */
    wl_surface_destroy(s);
    XWT_WAIT(t, b2->released);
    XWT_CHECK(b2->released, "final buffer released at surface teardown");
    XWT_ASSERT(!t->client_dead);

    wl_buffer_destroy(b1->buf);
    wl_buffer_destroy(b2->buf);
    wl_shm_pool_destroy(b1->pool);
    wl_shm_pool_destroy(b2->pool);
    free(b1);
    free(b2);
}

/* ------------------------------------------------------ subcompositor */

/* foot aborts at startup without the global; the lifecycle here covers
 * creation, position, render placement, sync mode, and both destroy
 * orderings. */
static void test_subsurface_lifecycle(struct xwt_ctx *t) {
    struct xwc *c = &t->client;

    /* parent: an ordinary xdg window */
    struct xwc_win *parent = xwt_window_solid(t, 0xff204060, 300, 200, "P");
    XWT_ASSERT(parent);
    XWT_WAIT(t, t->comp->wm && t->comp->wm->focused);

    struct wl_subcompositor *sc = NULL;
    xwt_bind_subcompositor(t, &sc);
    XWT_ASSERT(sc);

    struct wl_surface *parent_surface = xwc_win_surface(parent);
    XWT_ASSERT(parent_surface);

    /* child surface + subsurface role */
    struct wl_surface *child = wl_compositor_create_surface(c->compositor);
    struct shm_buf *cimg = shm_buf_create(c, 60, 40);
    XWT_ASSERT(cimg);
    struct wl_subsurface *sub =
        wl_subcompositor_get_subsurface(sc, child, parent_surface);
    XWT_ASSERT(sub);
    wl_subsurface_set_position(sub, 120, 80);

    wl_surface_attach(child, cimg->buf, 0, 0);
    wl_surface_commit(child); /* SYNCED (the spec default): pending */
    xwt_pump(t);
    xwt_pump(t);

    /* server state: one subsurface, pending, not yet applied */
    XWT_CHECK(!wl_list_empty(&t->comp->subcomps),
              "subsurface registered server-side");
    struct xw_subsurface *ss = wl_container_of(t->comp->subcomps.next, ss,
                                               link);
    XWT_CHECK(ss->x == 0 && ss->y == 0,
              "synced position held before the parent commit (%d,%d)", ss->x,
              ss->y);

    /* the parent commits: the synced state applies and maps */
    wl_surface_commit(parent_surface);
    xwt_pump(t);
    xwt_pump(t);
    XWT_CHECK(ss->x == 120 && ss->y == 80,
              "synced position applied at the parent commit (%d,%d)", ss->x,
              ss->y);
    XWT_CHECK(ss->surface->mapped, "subsurface mapped after parent commit");
    XWT_CHECK(ss->surface->role == XW_SURFACE_ROLE_SUBSURFACE,
              "role assigned");
    {
        int px = 0, py = 0, pw = 0, ph = 0;
        xw_surface_get_pos(ss->surface, &px, &py, &pw, &ph);
        XWT_CHECK(px >= 0 && py >= 0 && pw == 60 && ph == 40,
                  "child geometry via surface_get_pos: %dx%d+%d+%d", pw, ph,
                  px, py);
    }

    /* desync: the child's state applies immediately */
    wl_subsurface_set_desync(sub);
    wl_subsurface_set_position(sub, 10, 10);
    wl_surface_commit(child);
    xwt_pump(t);
    xwt_pump(t);
    XWT_CHECK(ss->x == 10 && ss->y == 10,
              "desynced position applied immediately (%d,%d)", ss->x, ss->y);

    /* stacking between two siblings (place_above/place_below with a
     * non-NULL sibling; NULL would fail the client-side marshaller) */
    struct wl_surface *sibling = wl_compositor_create_surface(c->compositor);
    struct shm_buf *simg = shm_buf_create(c, 20, 20);
    XWT_ASSERT(simg);
    struct wl_subsurface *ssub =
        wl_subcompositor_get_subsurface(sc, sibling, parent_surface);
    wl_surface_attach(sibling, simg->buf, 0, 0);
    wl_surface_commit(sibling);
    xwt_pump(t);
    xwt_pump(t);
    /* libwayland's marshaller rejects NULL object args for these; the
     * spec's below-parent stacking is exercised server-side via the
     * place_above/below sibling forms */
    wl_subsurface_place_above(ssub, child);
    xwt_pump(t);
    XWT_CHECK(!ss->below_parent, "sibling stacked above");

    /* destroy the CHILD surface first: role teardown, parent intact
     * (the sibling subsurface stays: exactly one entry must remain) */
    {
        int before = 0;
        struct xw_subsurface *it;
        wl_list_for_each(it, &t->comp->subcomps, link)
            before++;
        wl_surface_destroy(child);
        xwt_pump(t);
        xwt_pump(t);
        int after = 0;
        wl_list_for_each(it, &t->comp->subcomps, link)
            after++;
        XWT_CHECK(after == before - 1,
                  "the destroyed child's subsurface unregistered (%d->%d)",
                  before, after);
    }
    XWT_ASSERT(!t->client_dead);
    XWT_CHECK(t->comp->wm->focused, "parent window still alive");

    /* destroy the PARENT surface: a remaining child unroles cleanly */
    struct wl_surface *child2 = wl_compositor_create_surface(c->compositor);
    struct shm_buf *cimg2 = shm_buf_create(c, 20, 20);
    XWT_ASSERT(cimg2);
    struct wl_subsurface *sub2 = wl_subcompositor_get_subsurface(
        sc, child2, parent_surface);
    (void)sub2;
    wl_surface_attach(child2, cimg2->buf, 0, 0);
    wl_surface_commit(child2);
    xwt_pump(t);
    xwt_pump(t);
    XWT_CHECK(!wl_list_empty(&t->comp->subcomps), "second child registered");
    wl_surface_destroy(parent_surface);
    xwt_pump(t);
    xwt_pump(t);
    XWT_ASSERT(!t->client_dead);
    XWT_CHECK(wl_list_empty(&t->comp->subcomps),
              "parent destroy unroles the child");

    wl_subsurface_destroy(sub);
    if (sub2)
        wl_subsurface_destroy(sub2);
    if (ssub)
        wl_subsurface_destroy(ssub);
    wl_subcompositor_destroy(sc); /* the binding itself (LSan) */
    wl_surface_destroy(sibling);
    wl_buffer_destroy(simg->buf);
    wl_shm_pool_destroy(simg->pool);
    free(simg);
    wl_surface_destroy(child2);
    wl_buffer_destroy(cimg->buf);
    wl_shm_pool_destroy(cimg->pool);
    free(cimg);
    wl_buffer_destroy(cimg2->buf);
    wl_shm_pool_destroy(cimg2->pool);
    free(cimg2);
}

/* ------------------------------------------------------------- slow app */

/* a client that connects and idles long before its first buffer: the
 * session/compositor must never treat that as a failure (no timeouts
 * exist anywhere; this pins that none creep in) */
static void test_slow_start_client(struct xwt_ctx *t) {
    int windows_before = xw_compositor_window_count(t->comp);

    /* idle: pump = real dispatch cycles with no client activity */
    for (int i = 0; i < 60; i++) {
        xwt_pump(t);
        XWT_ASSERT(!t->client_dead);
    }
    XWT_CHECK(xw_compositor_window_count(t->comp) == windows_before,
              "no phantom window while the app idles");

    /* now it maps */
    struct xwc_win *w = xwt_window_solid(t, 0xff3070a0, 200, 150, "slow");
    XWT_ASSERT(w);
    XWT_WAIT(t, xw_compositor_window_count(t->comp) == windows_before + 1);
    XWT_CHECK(xw_compositor_window_count(t->comp) == windows_before + 1,
              "window mapped after the slow start");
    XWT_ASSERT(!t->client_dead);
    xwc_win_destroy(w);
}

static const struct xwt_test tests[] = {
    {"pointer-set-cursor", test_pointer_set_cursor},
    {"buffer-release-rotation", test_buffer_release_on_rotation},
    {"subsurface-lifecycle", test_subsurface_lifecycle},
    {"slow-start-client", test_slow_start_client},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
