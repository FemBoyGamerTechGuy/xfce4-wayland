/* xwc-lock.c — libxwcl: ext-session-lock + ext-idle-notify client side.
 *
 * The lock surface reuses the window/layer buffer lifecycle: a shm
 * pool with two buffers, retired only after the replacement buffer is
 * committed (the server renders from the attached wl_buffer until the
 * next commit — see the wl_buffer lifetime contract in
 * ARCHITECTURE.md).
 *
 * v0: one lock surface for the bound (first) output. libxwcl tracks a
 * single output; the server side supports any number.
 */
#include "xwc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wayland-client.h"
#include "ext-session-lock.h"
#include "ext-idle-notify.h"

/* pool machinery shared with xwc.c */
extern bool xwc_pool_create(struct xwc *c, int w, int h,
                            struct wl_shm_pool **pool_out,
                            struct wl_buffer **bufs, uint32_t **data_out,
                            size_t *size_out);
extern void xwc_pool_destroy(struct wl_shm_pool *pool, struct wl_buffer **bufs,
                             uint32_t *data, size_t size);
extern void xwc_pool_retired_destroy(struct wl_shm_pool *pool,
                                     struct wl_buffer **bufs, uint32_t *data,
                                     size_t size);
extern void xwc_pool_retire(struct wl_shm_pool **pool, struct wl_buffer **bufs,
                            uint32_t **data, size_t *size,
                            struct wl_shm_pool **old_pool,
                            struct wl_buffer **old_bufs, uint32_t **old_data,
                            size_t *old_size);
extern const struct wl_buffer_listener *xwc_buf_listener(void);
extern void xwc_register_surface(struct xwc *c, void *owner,
                                 const struct xwc_callbacks *cb,
                                 struct wl_surface *surface);
extern void xwc_unregister_surface(struct xwc *c, struct wl_surface *surface);

struct xwc_lock {
    struct xwc *c;
    struct xwc_lock_cbs cb;

    struct ext_session_lock_v1 *lock;
    struct ext_session_lock_surface_v1 *lsurf;
    struct wl_surface *surface;

    struct wl_shm_pool *pool, *old_pool;
    struct wl_buffer *bufs[2], *old_bufs[2];
    uint32_t *data, *old_data;
    size_t pool_size, old_pool_size;
    int cur; /* current back buffer index */
    int w, h;

    bool locked, finished;
};

/* --------------------------------------------------- lock object events */

static void lock_locked_ev(void *data, struct ext_session_lock_v1 *l) {
    (void)l;
    struct xwc_lock *k = data;
    k->locked = true;
    if (k->cb.locked)
        k->cb.locked(k, k->cb.ud);
}

static void lock_finished_ev(void *data, struct ext_session_lock_v1 *l) {
    (void)l;
    struct xwc_lock *k = data;
    k->finished = true;
    if (k->cb.finished)
        k->cb.finished(k, k->cb.ud);
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lock_locked_ev,
    .finished = lock_finished_ev,
};

/* ------------------------------------------------ lock surface configure */

static void lsurf_configure_ev(void *data,
                               struct ext_session_lock_surface_v1 *ls,
                               uint32_t serial, uint32_t w, uint32_t h) {
    struct xwc_lock *k = data;
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    if ((int)w != k->w || (int)h != k->h || !k->data) {
        if (w > 0 && h > 0) {
            /* retire the old pool (destroyed only after the new buffer
             * is committed below — wl_buffer lifetime contract) */
            xwc_pool_retire(&k->pool, k->bufs, &k->data, &k->pool_size,
                            &k->old_pool, k->old_bufs, &k->old_data,
                            &k->old_pool_size);
            k->w = (int)w;
            k->h = (int)h;
            if (xwc_pool_create(k->c, k->w, k->h, &k->pool, k->bufs, &k->data,
                                &k->pool_size)) {
                wl_buffer_add_listener(k->bufs[0], xwc_buf_listener(), k);
                wl_buffer_add_listener(k->bufs[1], xwc_buf_listener(), k);
            } else {
                fprintf(stderr, "xwc: lock pool allocation failed (%ux%u)\n",
                        w, h);
            }
        }
    }
    if (k->cb.configure)
        k->cb.configure(k, k->w, k->h, k->cb.ud);
    /* the configure callback drew + committed the new buffer: the
     * retired pool can go now. Only when the new pool exists — on
     * allocation failure nothing was committed and the old buffer must
     * stay alive (released at destroy time instead, after the surface
     * itself is gone). */
    if (k->data && (k->old_pool || k->old_data)) {
        xwc_pool_retired_destroy(k->old_pool, k->old_bufs, k->old_data,
                                 k->old_pool_size);
        k->old_pool = NULL;
        k->old_bufs[0] = k->old_bufs[1] = NULL;
        k->old_data = NULL;
        k->old_pool_size = 0;
    }
}

static const struct ext_session_lock_surface_v1_listener lsurf_listener = {
    .configure = lsurf_configure_ev,
};

/* --------------------------------------------------- input routing thunks */
/* The owner map dispatches (struct xxc_win *)-typed callbacks; the lock
 * surface is registered as an owner and the thunks cast back. */

static void lock_key_thunk(struct xwc_win *w, uint32_t keycode, bool down,
                           xkb_keysym_t keysym, uint32_t mods, void *ud) {
    struct xwc_lock *k = (struct xwc_lock *)w;
    if (k && k->cb.key)
        k->cb.key(k, keycode, down, keysym, mods, ud);
}

static void lock_button_thunk(struct xwc_win *w, uint32_t button, bool down,
                              int x, int y, void *ud) {
    struct xwc_lock *k = (struct xwc_lock *)w;
    if (k && k->cb.button)
        k->cb.button(k, button, down, x, y, ud);
}

static void lock_motion_thunk(struct xwc_win *w, int x, int y, void *ud) {
    struct xwc_lock *k = (struct xwc_lock *)w;
    if (k && k->cb.motion)
        k->cb.motion(k, x, y, ud);
}

static void lock_configure_thunk(struct xwc_win *w, int width, int height,
                                 void *ud) {
    (void)width;
    (void)height;
    (void)ud;
    (void)w; /* lock surfaces configure through lsurf_configure_ev */
}

static void lock_close_thunk(struct xwc_win *w, void *ud) {
    (void)w;
    (void)ud; /* lock surfaces have no close event */
}

/* -------------------------------------------------------------- lifecycle */

struct xwc_lock *xwc_lock_create(struct xwc *c, const struct xwc_lock_cbs *cb) {
    if (!c || !c->lock_mgr || !c->output || !c->compositor)
        return NULL;
    struct xwc_lock *k = calloc(1, sizeof(*k));
    if (!k)
        return NULL;
    k->c = c;
    if (cb)
        k->cb = *cb;

    k->surface = wl_compositor_create_surface(c->compositor);
    if (!k->surface)
        goto fail;
    struct xwc_callbacks routing = {
        .key = lock_key_thunk,
        .button = lock_button_thunk,
        .motion = lock_motion_thunk,
        .configure = lock_configure_thunk,
        .close = lock_close_thunk,
        .ud = k->cb.ud,
    };
    xwc_register_surface(c, k, &routing, k->surface);

    k->lock = ext_session_lock_manager_v1_lock(c->lock_mgr);
    if (!k->lock)
        goto fail;
    ext_session_lock_v1_add_listener(k->lock, &lock_listener, k);

    k->lsurf =
        ext_session_lock_v1_get_lock_surface(k->lock, k->surface, c->output);
    if (!k->lsurf)
        goto fail;
    ext_session_lock_surface_v1_add_listener(k->lsurf, &lsurf_listener, k);

    /* the first configure (with the output size) arrives with the
     * sync answer; lsurf_configure_ev allocates the pool and calls the
     * user's configure callback */
    xwc_sync(c);
    return k;

fail:
    xwc_lock_destroy(k);
    return NULL;
}

void xwc_lock_destroy(struct xwc_lock *k) {
    if (!k)
        return;
    xwc_unregister_surface(k->c, k->surface);
    /* role objects and surface first, pools after: requests are
     * processed in order, so the server stops rendering from the
     * attached buffer before the wl_buffer (and its server-side
     * wl_shm_buffer) is destroyed. */
    if (k->lsurf)
        ext_session_lock_surface_v1_destroy(k->lsurf);
    if (k->surface)
        wl_surface_destroy(k->surface);
    if (k->lock && (!k->locked || k->finished)) {
        /* Safe to destroy: either the lock was never held, or the
         * server sent `finished` (the object is dead by its own
         * protocol — destroying it is the documented reaction and
         * releases the client-side proxy). */
        ext_session_lock_v1_destroy(k->lock);
    } else if (k->lock) {
        /* the lock is HELD (locked && !finished): sending destroy
         * would be a protocol error (server: invalid_destroy), and the
         * server must keep the session locked after we disconnect
         * (spec). Free the client-side proxy WITHOUT a request: the
         * object dies with the connection, the memory is ours. (A raw
         * wl_proxy_destroy sends nothing; the lsurf child was already
         * destroyed above.) */
        wl_proxy_destroy((struct wl_proxy *)k->lock);
    }
    k->lock = NULL;
    k->lsurf = NULL;
    k->surface = NULL;
    xwc_pool_destroy(k->pool, k->bufs, k->data, k->pool_size);
    xwc_pool_retired_destroy(k->old_pool, k->old_bufs, k->old_data,
                             k->old_pool_size);
    if (k->c && k->c->focused_owner == k) {
        k->c->focused_owner = NULL;
        k->c->has_focus = false;
    }
    free(k);
}

uint32_t *xwc_lock_pixels(struct xwc_lock *k, int *stride) {
    if (!k->data)
        return NULL;
    if (stride)
        *stride = k->w;
    return k->data + (size_t)k->cur * k->w * k->h;
}

void xwc_lock_size(struct xwc_lock *k, int *w, int *h) {
    if (w) *w = k->w;
    if (h) *h = k->h;
}

void xwc_lock_commit(struct xwc_lock *k) {
    if (!k->bufs[0])
        return;
    wl_surface_attach(k->surface, k->bufs[k->cur], 0, 0);
    wl_surface_damage(k->surface, 0, 0, k->w, k->h);
    wl_surface_commit(k->surface);
    k->cur ^= 1;
}

bool xwc_lock_locked(struct xwc_lock *k) { return k ? k->locked : false; }
bool xwc_lock_finished(struct xwc_lock *k) { return k ? k->finished : false; }

void xwc_lock_unlock(struct xwc_lock *k) {
    if (!k || !k->lock)
        return;
    /* unlock_and_destroy is a destructor: the proxy is gone after the
     * call. The sync guarantees the server processed the request (the
     * protocol's note about exiting right after unlocking). Lock
     * surface objects remain valid per protocol — the client destroys
     * them (xwc_lock_destroy) after unlocking. */
    ext_session_lock_v1_unlock_and_destroy(k->lock);
    k->lock = NULL;
    xwc_sync(k->c);
}

/* ------------------------------------------------------------ idle notify */

struct xwc_idle {
    struct xwc *c;
    struct ext_idle_notification_v1 *note;
    void (*idled)(void *ud);
    void (*resumed)(void *ud);
    void *ud;
};

static void idle_idled_ev(void *data, struct ext_idle_notification_v1 *n) {
    (void)n;
    struct xwc_idle *i = data;
    if (i->idled)
        i->idled(i->ud);
}

static void idle_resumed_ev(void *data, struct ext_idle_notification_v1 *n) {
    (void)n;
    struct xwc_idle *i = data;
    if (i->resumed)
        i->resumed(i->ud);
}

static const struct ext_idle_notification_v1_listener idle_note_listener = {
    .idled = idle_idled_ev,
    .resumed = idle_resumed_ev,
};

struct xwc_idle *xwc_idle_create(struct xwc *c, uint32_t timeout_ms,
                                 void (*idled)(void *ud),
                                 void (*resumed)(void *ud), void *ud) {
    if (!c || !c->idle_notifier || !c->seat)
        return NULL;
    struct xwc_idle *i = calloc(1, sizeof(*i));
    if (!i)
        return NULL;
    i->c = c;
    i->idled = idled;
    i->resumed = resumed;
    i->ud = ud;
    i->note = ext_idle_notifier_v1_get_idle_notification(
        c->idle_notifier, timeout_ms, c->seat);
    if (!i->note) {
        free(i);
        return NULL;
    }
    ext_idle_notification_v1_add_listener(i->note, &idle_note_listener, i);
    xwc_sync(c);
    return i;
}

void xwc_idle_destroy(struct xwc_idle *i) {
    if (!i)
        return;
    if (i->note)
        ext_idle_notification_v1_destroy(i->note);
    free(i);
}
