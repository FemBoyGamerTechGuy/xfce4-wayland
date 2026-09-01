/* xw-session-lock.c — ext-session-lock-v1 (secure session locking).
 *
 * State machine:
 *
 *   lock()            -> PENDING   security gate engages IMMEDIATELY
 *                                   (rendering is blanked, input is routed
 *                                   to lock surfaces only); a timeout
 *                                   guards against clients that never
 *                                   commit lock surfaces.
 *   every output
 *   covered by a
 *   committed lock
 *   surface + the
 *   frame presented   -> ACTIVE    the `locked` event is sent (the spec
 *                                   forbids sending it before a locked
 *                                   frame is presented; we flush it from
 *                                   the post-present hook).
 *   timeout expiry    -> ACTIVE    blank fallback frame presented, then
 *                                   `locked`.
 *   unlock_and_
 *   destroy           -> RELEASED  gate drops, content repaints.
 *   client dies
 *   while ACTIVE      -> gate stays engaged (owner_dead): every output
 *                                   renders blank; a NEW client may lock()
 *                                   again and take over.
 *   client dies
 *   while PENDING     -> gate drops (the session was never locked: the
 *                                   `locked` event had not been sent).
 *
 * Security invariants (enforced server-side, never trusted to clients):
 *   - While the gate is engaged, xw_render_output draws ONLY lock
 *     surfaces over an opaque blank; no window/layer/popup pixel can
 *     leak (xw-render.c consults xw_session_lock_active()).
 *   - While the gate is engaged, the seat delivers input only to lock
 *     surfaces; shortcuts, interactive move/resize, popup dismissal and
 *     window focus are skipped (xw-seat.c consults the same gate).
 *   - `locked` is sent only after a locked frame was actually presented
 *     (post-present flush) or after the timeout with the blank frame.
 *   - A lock surface covers its whole output: the compositor places it
 *     at the output origin at the output size and enforces the acked
 *     configure dimensions on every commit.
 *
 * Protocol errors:
 *   ext_session_lock_v1:         invalid_destroy, invalid_unlock, role,
 *                                duplicate_output, already_constructed.
 *   ext_session_lock_surface_v1: commit_before_first_ack, null_buffer,
 *                                dimensions_mismatch, invalid_serial.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

#define SESSION_LOCK_VERSION 1
#define XW_LOCK_TIMEOUT_DEFAULT_MS 1000

struct xw_lock_surface {
    struct wl_resource *res;    /* ext_session_lock_surface_v1 */
    struct xw_surface *surface; /* wl_surface (server side) */
    struct xw_output *output;
    struct xw_lock *lock;

    struct wl_list link;        /* lock->surfaces */
    struct wl_list rlink;       /* sl->render_surfaces (while mapped) */

    uint32_t last_serial;       /* serial of the newest configure sent */
    uint32_t acked_serial;      /* serial of the newest configure acked */
    int32_t acked_w, acked_h;   /* dimensions of the acked configure */
    bool acked_any;

    /* serial -> dimensions of recent configures: the client may ack an
     * older configure than the newest sent (it consumes all older
     * events), and the dimensions enforced on commit are those of the
     * acked one. Configures are rare (output resize), so a small fixed
     * ring suffices. */
    struct {
        uint32_t serial;
        int32_t w, h;
    } conf_hist[8];
    int conf_hist_n;

    bool mapped;                /* a buffer has been committed */
};

enum xw_lock_state {
    LOCK_PENDING,   /* gate engaged; waiting for surfaces / timeout */
    LOCK_ACTIVE,    /* `locked` sent; this client owns unlocking */
    LOCK_RELEASED,  /* dead object (denied, unlocked, or never locked) */
};

struct xw_lock {
    struct wl_resource *res;    /* ext_session_lock_v1 */
    struct xw_compositor *comp;
    struct wl_list surfaces;    /* xw_lock_surface.link */

    enum xw_lock_state state;
    bool locked_sent;
    bool flush_wanted;          /* every output covered: flush at the
                                 * next post-present */
    bool force_flush;           /* timeout expired: flush regardless */
    struct wl_event_source *timeout_src;

    struct wl_list link;        /* sl->locks */
};

struct xw_session_lock {
    struct wl_global *global;
    struct xw_compositor *comp;

    struct wl_list locks;           /* xw_lock.link */
    struct wl_list render_surfaces; /* xw_lock_surface.rlink (mapped) */

    struct xw_lock *active;     /* gate holder (PENDING or ACTIVE) */
    bool owner_dead;            /* gate held after the lock client died:
                                 * every output stays blank until a new
                                 * client locks and takes over */
};

/* ------------------------------------------------------------- helpers */

static struct xw_session_lock *sl_of(struct xw_compositor *c) {
    return c ? c->session_lock_state : NULL;
}

static void damage_all(struct xw_compositor *c) {
    struct xw_output *o;
    wl_list_for_each(o, &c->outputs, link)
        xw_output_damage_rect(o, o->x, o->y, o->width, o->height);
}

static int lock_timeout_ms(void) {
    const char *env = getenv("XW_LOCK_TIMEOUT_MS");
    if (env && *env) {
        int v = atoi(env);
        if (v >= 0)
            return v;
    }
    return XW_LOCK_TIMEOUT_DEFAULT_MS;
}

static void refocus_seats(struct xw_compositor *c) {
    struct xw_seat *seat;
    wl_list_for_each(seat, &c->seats, link)
        xw_seat_set_kb_focus(seat, xw_session_lock_kb_owner(c));
}

static void restore_focus(struct xw_compositor *c) {
    if (!c->wm)
        return;
    struct xw_seat *seat;
    wl_list_for_each(seat, &c->seats, link) {
        struct xw_surface *target = NULL;
        if (c->wm->focused && c->wm->focused->surface &&
            xw_wm_window_visible(c->wm, c->wm->focused))
            target = c->wm->focused->surface;
        xw_seat_set_kb_focus(seat, target);
    }
}

/* popups, interactive move/resize and pointer grabs must not survive
 * across the gate: while engaged, input belongs to lock surfaces only. */
static void cancel_interactions(struct xw_compositor *c) {
    struct xw_popup *p, *p2;
    wl_list_for_each_safe(p, p2, &c->popups, link)
        xw_popup_dismiss(p);
    if (c->wm) {
        struct xw_window *iw = xw_wm_interactive_window(c->wm);
        if (iw)
            xw_wm_interactive_end(c->wm, iw);
    }
    struct xw_seat *seat;
    wl_list_for_each(seat, &c->seats, link) {
        seat->drag.active = false;
        seat->drag.source = NULL;
        seat->drag.origin = NULL;
        seat->ptr_grab = NULL;
        seat->ptr_grab_is_drag = false;
        seat->grab_surface = NULL;
        /* a consumed-key release can never arrive now: clear the map so
         * no stray suppression leaks into the unlocked session */
        memset(seat->consumed_keys, 0, sizeof(seat->consumed_keys));
    }
}

/* -------------------------------------------------- lock surface objects */

static struct xw_lock_surface *lsurf_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

/* send a configure with the output's current size, record its serial
 * and dimensions in the history ring. */
static void lsurf_configure(struct xw_lock_surface *ls) {
    if (!ls->res || !ls->output || !ls->lock)
        return;
    uint32_t serial = wl_display_next_serial(ls->lock->comp->display);
    int w = ls->output->width, h = ls->output->height;
    ext_session_lock_surface_v1_send_configure(ls->res, serial,
                                               (uint32_t)w, (uint32_t)h);
    ls->last_serial = serial;
    if (ls->conf_hist_n < (int)(sizeof(ls->conf_hist) / sizeof(*ls->conf_hist))) {
        ls->conf_hist[ls->conf_hist_n].serial = serial;
        ls->conf_hist[ls->conf_hist_n].w = w;
        ls->conf_hist[ls->conf_hist_n].h = h;
        ls->conf_hist_n++;
    } else { /* ring full (eight un-acked resizes): drop the oldest */
        memmove(&ls->conf_hist[0], &ls->conf_hist[1],
                sizeof(ls->conf_hist) - sizeof(ls->conf_hist[0]));
        ls->conf_hist[7].serial = serial;
        ls->conf_hist[7].w = w;
        ls->conf_hist[7].h = h;
    }
}

static void lsurf_destroy_req(struct wl_client *client,
                              struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void lsurf_ack_configure(struct wl_client *client,
                                struct wl_resource *res, uint32_t serial) {
    (void)client;
    struct xw_lock_surface *ls = lsurf_from_res(res);
    if (!ls)
        return;
    if (serial > ls->last_serial || serial <= ls->acked_serial) {
        wl_resource_post_error(
            res, EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
            "ack_configure serial %u is not a pending configure "
            "(last sent %u, last acked %u)",
            serial, ls->last_serial, ls->acked_serial);
        return;
    }
    /* find the dimensions of the configure being acked */
    int32_t w = -1, h = -1;
    for (int i = ls->conf_hist_n - 1; i >= 0; i--) {
        if (ls->conf_hist[i].serial == serial) {
            w = ls->conf_hist[i].w;
            h = ls->conf_hist[i].h;
            break;
        }
    }
    if (w < 0) {
        /* not in the ring: fall back to the newest sent dimensions
         * (only reachable after 8+ un-acked configures) */
        wl_resource_post_error(
            res, EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
            "ack_configure serial %u references a configure that is no "
            "longer tracked",
            serial);
        return;
    }
    ls->acked_serial = serial;
    ls->acked_w = w;
    ls->acked_h = h;
    ls->acked_any = true;
}

static const struct ext_session_lock_surface_v1_interface
    lock_surface_impl = {
    .destroy = lsurf_destroy_req,
    .ack_configure = lsurf_ack_configure,
};

/* mapped bookkeeping shared by the unmap paths (role destroy, object
 * destroy, output removal, unlock). Also clears any seat focus still
 * pointing at the surface: client-death teardown frees struct
 * xw_surface in wl_map id order (wl_surface before ext_session_lock),
 * so a dangling kb_focus would be dereferenced by the later lock-object
 * destructor's refocus (found by ASan in the xw-lock child test). */
static void lock_surface_unmapped(struct xw_lock_surface *ls);
static void lock_surface_unmapped(struct xw_lock_surface *ls) {
    bool was_mapped = ls->mapped;
    if (ls->mapped) {
        ls->mapped = false;
        if (ls->surface)
            ls->surface->mapped = false;
        wl_list_remove(&ls->rlink);
        wl_list_init(&ls->rlink);
    }
    if (ls->surface && ls->lock && ls->lock->comp) {
        struct xw_seat *seat;
        wl_list_for_each(seat, &ls->lock->comp->seats, link) {
            if (seat->kb_focus == ls->surface)
                seat->kb_focus = NULL; /* re-established on the next
                                          set_kb_focus (refocus_seats) */
            if (seat->ptr_focus == ls->surface)
                seat->ptr_focus = NULL;
            if (seat->grab_surface == ls->surface) {
                seat->ptr_grab = NULL;
                seat->grab_surface = NULL;
            }
        }
    }
    if (was_mapped && ls->output && ls->lock &&
        xw_session_lock_active(ls->lock->comp))
        xw_output_damage_rect(ls->output, ls->output->x, ls->output->y,
                              ls->output->width, ls->output->height);
}

static void lsurf_resource_destroy(struct wl_resource *res) {
    struct xw_lock_surface *ls = lsurf_from_res(res);
    if (!ls)
        return;
    lock_surface_unmapped(ls);
    /* detach from the wl_surface's role (the surface may outlive the
     * lock surface object: the client can destroy it later) */
    if (ls->surface && ls->surface->role_data == ls) {
        ls->surface->role = XW_SURFACE_ROLE_NONE;
        ls->surface->role_data = NULL;
    }
    wl_list_remove(&ls->link);
    struct xw_lock *l = ls->lock;
    free(ls);
    /* a zombie lock (its own resource is gone) is freed by its last
     * lock surface — see lock_resource_destroy */
    if (l && !l->res && wl_list_empty(&l->surfaces))
        free(l);
}

/* ------------------------------------------------------ role hooks */

/* wl_surface commit with the session-lock role (dispatched from
 * xw_role_commit): enforce the strict commit contract, then map (a
 * lock surface always covers its whole output). */
void xw_lock_role_commit(struct xw_surface *s) {
    struct xw_lock_surface *ls = s->role_data;
    if (!ls || !ls->res)
        return;

    if (!ls->acked_any) {
        wl_resource_post_error(
            ls->res,
            EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK,
            "lock surface committed before acking a configure");
        return;
    }
    if (!s->shm && !s->has_single_pixel) {
        wl_resource_post_error(
            ls->res, EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
            "lock surface committed without a buffer");
        return;
    }
    int sc = s->scale > 0 ? s->scale : 1;
    if (s->buf_w / sc != ls->acked_w || s->buf_h / sc != ls->acked_h) {
        wl_resource_post_error(
            ls->res, EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH,
            "lock surface committed %dx%d, acked configure is %dx%d",
            s->buf_w / sc, s->buf_h / sc, ls->acked_w, ls->acked_h);
        return;
    }

    bool first_map = !ls->mapped;
    if (!ls->mapped) {
        ls->mapped = true;
        s->mapped = true; /* frame callbacks ride on this */
        wl_list_insert(sl_of(s->comp)->render_surfaces.prev, &ls->rlink);
    }
    xw_output_damage_rect(ls->output, ls->output->x, ls->output->y,
                          ls->output->width, ls->output->height);

    struct xw_lock *l = ls->lock;
    if (first_map && xw_session_lock_active(s->comp))
        refocus_seats(s->comp); /* hand the keyboard to the lock surface */

    if (l->state == LOCK_PENDING && !l->flush_wanted) {
        struct xw_output *o;
        bool all = true;
        wl_list_for_each(o, &s->comp->outputs, link) {
            bool covered = false;
            struct xw_lock_surface *it;
            wl_list_for_each(it, &l->surfaces, link) {
                if (it->output == o && it->mapped) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                all = false;
                break;
            }
        }
        if (all)
            l->flush_wanted = true; /* flushed after the frame presents */
    }
}

/* the wl_surface died (client destroy request or client death): tear
 * the lock surface down including its object resource. */
void xw_lock_role_destroy(struct xw_surface *s) {
    struct xw_lock_surface *ls = s->role_data;
    if (!ls)
        return;
    s->role = XW_SURFACE_ROLE_NONE;
    s->role_data = NULL;
    if (ls->res) {
        struct wl_resource *r = ls->res;
        ls->res = NULL;
        wl_resource_destroy(r); /* destructor performs the teardown */
    } else {
        lock_surface_unmapped(ls);
        wl_list_remove(&ls->link);
        struct xw_lock *l = ls->lock;
        free(ls);
        if (l && !l->res && wl_list_empty(&l->surfaces))
            free(l);
    }
}

/* ------------------------------------------------------------- render */

/* blit a lock surface at the output origin covering the full output; a
 * stale-sized buffer (resize in flight, recommit pending) is scaled to
 * the interim geometry — exact coverage is what matters for security. */
static void blit_lock(struct xw_output *o, struct xw_lock_surface *ls) {
    struct xw_surface *s = ls->surface;
    pixman_image_t *src = xw_surface_get_image(s);
    if (!src)
        return;
    int gw = o->width, gh = o->height;
    int sc = s->scale > 0 ? s->scale : 1;
    if (s->buf_w >= gw * sc && s->buf_h >= gh * sc) {
        pixman_image_composite(PIXMAN_OP_OVER, src, NULL, o->logical, 0, 0, 0,
                               0, 0, 0, gw, gh);
    } else {
        pixman_transform_t t;
        pixman_fixed_t fx = pixman_int_to_fixed(s->buf_w) / gw;
        pixman_fixed_t fy = pixman_int_to_fixed(s->buf_h) / gh;
        pixman_transform_init_scale(&t, fx, fy);
        pixman_image_set_transform(src, &t);
        pixman_image_composite(PIXMAN_OP_OVER, src, NULL, o->logical, 0, 0, 0,
                               0, 0, 0, gw, gh);
        pixman_image_set_transform(src, NULL);
    }
    pixman_image_unref(src);
}

void xw_session_lock_render(struct xw_output *o) {
    struct xw_session_lock *sl = sl_of(o->comp);
    if (!sl)
        return;
    /* opaque blank first (regardless of the wallpaper color), then
     * exactly the lock surfaces, nothing else. */
    xw_render_fill_rect(o->logical, PIXMAN_OP_SRC, 0xff000000, 0, 0, o->width,
                        o->height);
    struct xw_lock_surface *ls;
    wl_list_for_each(ls, &sl->render_surfaces, rlink) {
        if (ls->output != o || !ls->surface)
            continue;
        if (!ls->surface->shm && !ls->surface->has_single_pixel)
            continue;
        blit_lock(o, ls);
    }
}

/* --------------------------------------------------------- lock objects */

static struct xw_lock *lock_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void lock_destroy_req(struct wl_client *client,
                             struct wl_resource *res) {
    (void)client;
    struct xw_lock *l = lock_from_res(res);
    if (l && l->locked_sent) {
        /* spec: destroying the object that holds the lock is a protocol
         * error; the unlock_and_destroy request must be used. The error
         * disconnects the client, and the session STAYS locked (the
         * resource destructor treats it like client death). */
        wl_resource_post_error(
            res, EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY,
            "destroy requested on a lock object that holds the lock");
        return;
    }
    wl_resource_destroy(res);
}

static void lock_get_lock_surface(struct wl_client *client,
                                  struct wl_resource *res, uint32_t id,
                                  struct wl_resource *surface,
                                  struct wl_resource *output) {
    struct xw_lock *l = lock_from_res(res);
    if (!l)
        return;
    struct xw_compositor *c = l->comp;
    struct xw_surface *s = wl_resource_get_user_data(surface);
    struct xw_output *o = wl_resource_get_user_data(output);

    if (!s) {
        wl_resource_post_error(res, EXT_SESSION_LOCK_V1_ERROR_ROLE,
                               "invalid wl_surface");
        return;
    }
    if (s->role != XW_SURFACE_ROLE_NONE) {
        wl_resource_post_error(res, EXT_SESSION_LOCK_V1_ERROR_ROLE,
                               "surface already has a role");
        return;
    }
    if (s->pending_buffer || s->buf_w > 0 || s->buf_h > 0) {
        wl_resource_post_error(
            res, EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
            "surface already has a buffer attached or committed");
        return;
    }
    if (!o) {
        wl_resource_post_error(res, EXT_SESSION_LOCK_V1_ERROR_ROLE,
                               "invalid output");
        return;
    }
    /* the wl_output resource must be one of ours (defensive: user data
     * is only trustworthy for outputs this compositor created) */
    {
        struct xw_output *it;
        bool ours = false;
        wl_list_for_each(it, &c->outputs, link) {
            if (it == o) {
                ours = true;
                break;
            }
        }
        if (!ours) {
            wl_resource_post_error(res, EXT_SESSION_LOCK_V1_ERROR_ROLE,
                                   "unknown output");
            return;
        }
    }
    /* one lock surface per output per lock (duplicate_output) */
    struct xw_lock_surface *it;
    wl_list_for_each(it, &l->surfaces, link) {
        if (it->output == o) {
            wl_resource_post_error(
                res, EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
                "output already has a lock surface for this lock");
            return;
        }
    }

    struct xw_lock_surface *ls = calloc(1, sizeof(*ls));
    if (!ls) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *lres = wl_resource_create(
        client, &ext_session_lock_surface_v1_interface,
        wl_resource_get_version(res), id);
    if (!lres) {
        free(ls);
        wl_client_post_no_memory(client);
        return;
    }
    ls->res = lres;
    ls->surface = s;
    ls->output = o;
    ls->lock = l;
    wl_list_init(&ls->rlink);
    wl_list_insert(l->surfaces.prev, &ls->link);
    wl_resource_set_implementation(lres, &lock_surface_impl, ls,
                                   lsurf_resource_destroy);
    s->role = XW_SURFACE_ROLE_SESSION_LOCK;
    s->role_data = ls;

    /* spec: the first configure is sent immediately on binding */
    lsurf_configure(ls);
}

static void lock_unlock_and_destroy(struct wl_client *client,
                                    struct wl_resource *res) {
    (void)client;
    struct xw_lock *l = lock_from_res(res);
    struct xw_session_lock *sl = sl_of(l ? l->comp : NULL);
    if (!l || !sl)
        return;
    if (!l->locked_sent) {
        /* spec: unlocking before `locked` was sent is a protocol error */
        wl_resource_post_error(
            res, EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
            "unlock requested but the locked event was never sent");
        return;
    }
    /* release the gate (only the active lock can be locked_sent) */
    if (sl->active == l) {
        sl->active = NULL;
        sl->owner_dead = false;
        l->state = LOCK_RELEASED;
        xw_log(XW_LOG_INFO, "session-lock: unlocked by client");
        /* unmap all surfaces of this lock (their objects stay valid per
         * protocol until the client destroys them, but they stop
         * rendering and receiving input) */
        struct xw_lock_surface *ls, *ls2;
        wl_list_for_each_safe(ls, ls2, &l->surfaces, link)
            lock_surface_unmapped(ls);
        damage_all(l->comp);
        restore_focus(l->comp);
    }
    wl_resource_destroy(res);
}

static const struct ext_session_lock_v1_interface lock_impl = {
    .destroy = lock_destroy_req,
    .get_lock_surface = lock_get_lock_surface,
    .unlock_and_destroy = lock_unlock_and_destroy,
};

static void lock_resource_destroy(struct wl_resource *res) {
    struct xw_lock *l = lock_from_res(res);
    if (!l)
        return;
    struct xw_session_lock *sl = sl_of(l->comp);

    if (l->timeout_src) {
        wl_event_source_remove(l->timeout_src);
        l->timeout_src = NULL;
    }

    /* Lock surface objects OUTLIVE the lock object (spec: "Existing
     * objects created through this interface remain valid") — after
     * unlock_and_destroy or destroy the client destroys them itself;
     * destroying them here would turn the client's well-formed
     * destroy requests into invalid-object protocol errors. On client
     * death their own resource destructors run in the same teardown.
     * The lock struct becomes a zombie (res == NULL) freed by its last
     * lock surface; with none, it frees now. */
    l->res = NULL;

    if (sl) {
        if (sl->active == l) {
            if (l->state == LOCK_ACTIVE || l->locked_sent) {
                /* died or errored while locked: the session STAYS
                 * locked (spec); outputs render blank; a new client
                 * may lock() again and take over. */
                sl->active = NULL;
                sl->owner_dead = true;
                xw_log(XW_LOG_WARN,
                       "session-lock: lock client died while locked; "
                       "session stays locked (blank) until a new lock "
                       "client takes over");
                damage_all(l->comp);
                refocus_seats(l->comp); /* no lock surface: focus NULL */
            } else {
                /* PENDING: never locked -> release the gate */
                sl->active = NULL;
                sl->owner_dead = false;
                xw_log(XW_LOG_INFO,
                       "session-lock: pending lock client died before "
                       "locking; releasing");
                damage_all(l->comp);
                restore_focus(l->comp);
            }
        }
        wl_list_remove(&l->link);
    }
    if (wl_list_empty(&l->surfaces))
        free(l);
}

/* ------------------------------------------------------ timeout (grace) */

static int lock_timeout_cb(void *data) {
    struct xw_lock *l = data;
    struct xw_compositor *c = l->comp;
    if (l->state == LOCK_PENDING && !l->locked_sent) {
        l->force_flush = true;
        /* guarantee a presented frame (blank already covers any output
         * without lock surfaces) so the post-present flush can run */
        damage_all(c);
    }
    /* disarm (one-shot): the source itself is removed on flush or when
     * the lock object is destroyed */
    wl_event_source_timer_update(l->timeout_src, 0);
    return 0;
}

/* --------------------------------------------------------- post-present */

void xw_session_lock_after_present(struct xw_compositor *c) {
    struct xw_session_lock *sl = sl_of(c);
    if (!sl)
        return;
    struct xw_lock *l = sl->active;
    if (!l || l->locked_sent)
        return;
    if (!l->flush_wanted && !l->force_flush)
        return;
    if (l->force_flush && !l->flush_wanted) {
        /* timeout: outputs without lock surfaces are blank (presented
         * above); outputs WITH surfaces are covered by definition */
    }
    ext_session_lock_v1_send_locked(l->res);
    l->locked_sent = true;
    l->state = LOCK_ACTIVE;
    if (l->timeout_src) {
        wl_event_source_remove(l->timeout_src);
        l->timeout_src = NULL;
    }
    xw_log(XW_LOG_INFO, "session-lock: locked");
}

/* ------------------------------------------------------------ manager */

static void manager_lock(struct wl_client *client, struct wl_resource *res,
                         uint32_t id) {
    struct xw_session_lock *sl = wl_resource_get_user_data(res);
    struct xw_compositor *c = sl->comp;

    struct xw_lock *l = calloc(1, sizeof(*l));
    if (!l) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *lres = wl_resource_create(
        client, &ext_session_lock_v1_interface,
        wl_resource_get_version(res), id);
    if (!lres) {
        free(l);
        wl_client_post_no_memory(client);
        return;
    }
    l->res = lres;
    l->comp = c;
    wl_list_init(&l->surfaces);
    wl_list_insert(sl->locks.prev, &l->link);
    wl_resource_set_implementation(lres, &lock_impl, l,
                                    lock_resource_destroy);

    if (sl->active || sl->owner_dead) {
        /* already locked (or held after a lock client died): deny this
         * lock attempt. A dead-owner session accepts the new client as
         * the takeover (spec explicitly allows this recovery). */
        if (sl->owner_dead) {
            sl->owner_dead = false;
            sl->active = l;
            l->state = LOCK_PENDING;
            xw_log(XW_LOG_INFO,
                   "session-lock: new lock client takes over after "
                   "previous lock client died");
            damage_all(c);
            cancel_interactions(c);
            refocus_seats(c);
        } else {
            l->state = LOCK_RELEASED;
            ext_session_lock_v1_send_finished(lres);
            xw_log(XW_LOG_INFO,
                   "session-lock: lock denied (session already locked)");
        }
    } else {
        l->state = LOCK_PENDING;
        sl->active = l;
        xw_log(XW_LOG_INFO, "session-lock: lock requested");
        /* engage the gate immediately: blank + input to lock surfaces
         * only (hiding earlier than the `locked` event is strictly
         * safer) */
        damage_all(c);
        cancel_interactions(c);
        refocus_seats(c);
    }

    if (l->state == LOCK_PENDING) {
        /* grace timer: if the client never covers the outputs, the
         * blank frame becomes the locked frame anyway (spec time
         * limit). A 0 delay would DISARM the timer — use 1 ms. */
        l->timeout_src =
            wl_event_loop_add_timer(c->loop, lock_timeout_cb, l);
        if (l->timeout_src) {
            int to = lock_timeout_ms();
            wl_event_source_timer_update(l->timeout_src, to > 0 ? to : 1);
        }
    }
}

static void manager_destroy(struct wl_client *client,
                            struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct ext_session_lock_manager_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .lock = manager_lock,
};

static void bind_session_lock(struct wl_client *client, void *data,
                              uint32_t version, uint32_t id) {
    if (version > SESSION_LOCK_VERSION)
        version = SESSION_LOCK_VERSION;
    struct wl_resource *res = wl_resource_create(
        client, &ext_session_lock_manager_v1_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &manager_impl, data, NULL);
}

/* ------------------------------------------------------------ lifecycle */

void xw_session_lock_init(struct xw_compositor *c) {
    struct xw_session_lock *sl = calloc(1, sizeof(*sl));
    if (!sl)
        return;
    sl->comp = c;
    wl_list_init(&sl->locks);
    wl_list_init(&sl->render_surfaces);
    sl->global = wl_global_create(
        c->display, &ext_session_lock_manager_v1_interface,
        SESSION_LOCK_VERSION, sl, bind_session_lock);
    if (!sl->global) {
        xw_log(XW_LOG_ERROR, "session lock global creation failed");
        free(sl);
        return;
    }
    c->session_lock_state = sl;
}

void xw_session_lock_fin(struct xw_compositor *c) {
    struct xw_session_lock *sl = sl_of(c);
    if (!sl)
        return;
    /* client resources (locks, lock surfaces) were already destroyed by
     * wl_display_destroy_clients before module fini; only our own
     * allocations remain */
    if (sl->global)
        wl_global_destroy(sl->global);
    struct xw_lock *l, *l2;
    wl_list_for_each_safe(l, l2, &sl->locks, link) {
        if (l->timeout_src)
            wl_event_source_remove(l->timeout_src);
        wl_list_remove(&l->link);
        free(l);
    }
    free(sl);
    c->session_lock_state = NULL;
}

/* ------------------------------------------------------------- queries */

bool xw_session_lock_active(struct xw_compositor *c) {
    struct xw_session_lock *sl = sl_of(c);
    return sl && (sl->active || sl->owner_dead);
}

/* true once the `locked` event was actually sent (LOCK_ACTIVE) or the
 * lock client died after that (owner_dead) — i.e. the session is
 * really locked, not merely engaging. White-box test surface. */
bool xw_session_lock_locked(struct xw_compositor *c) {
    struct xw_session_lock *sl = sl_of(c);
    if (!sl)
        return false;
    if (sl->owner_dead)
        return true;
    return sl->active && sl->active->locked_sent;
}

struct xw_surface *xw_session_lock_kb_owner(struct xw_compositor *c) {
    struct xw_session_lock *sl = sl_of(c);
    if (!sl || !xw_session_lock_active(c))
        return NULL;
    struct xw_lock_surface *ls;
    wl_list_for_each(ls, &sl->render_surfaces, rlink) {
        if (ls->mapped && ls->surface)
            return ls->surface;
    }
    return NULL;
}

/* pointer hit-testing while engaged: only lock surfaces, by output
 * coverage (they always cover their whole output) */
struct xw_surface *xw_session_lock_surface_at(struct xw_compositor *c,
                                              int x, int y) {
    struct xw_session_lock *sl = sl_of(c);
    if (!sl)
        return NULL;
    struct xw_lock_surface *ls;
    wl_list_for_each(ls, &sl->render_surfaces, rlink) {
        if (!ls->mapped || !ls->surface || !ls->output)
            continue;
        struct xw_output *o = ls->output;
        if (x >= o->x && x < o->x + o->width && y >= o->y &&
            y < o->y + o->height)
            return ls->surface;
    }
    return NULL;
}

/* ------------------------------------------------------- output hooks */

void xw_session_lock_reconfigure_output(struct xw_compositor *c,
                                        struct xw_output *o) {
    struct xw_session_lock *sl = sl_of(c);
    if (!sl)
        return;
    struct xw_lock_surface *ls;
    wl_list_for_each(ls, &sl->render_surfaces, rlink) {
        if (ls->output != o)
            continue;
        lsurf_configure(ls); /* new dimensions must be acked + recommitted */
    }
    /* pending (not yet mapped) surfaces of any live lock on this output
     * too (zombies get no configures: their object is gone) */
    struct xw_lock *l;
    wl_list_for_each(l, &sl->locks, link) {
        if (!l->res)
            continue;
        struct xw_lock_surface *s2;
        wl_list_for_each(s2, &l->surfaces, link) {
            if (s2->output != o || s2->mapped)
                continue;
            if (s2->conf_hist_n == 0 || !s2->acked_any)
                lsurf_configure(s2);
        }
    }
    if (xw_session_lock_active(c))
        xw_output_damage_rect(o, o->x, o->y, o->width, o->height);
}

void xw_session_lock_output_removed(struct xw_compositor *c,
                                    struct xw_output *o) {
    struct xw_session_lock *sl = sl_of(c);
    if (!sl)
        return;
    /* destroy the lock surfaces bound to this output (client-side, the
     * client should already have destroyed them on global remove; this
     * is the server-side guarantee that no surface references a freed
     * output) */
    struct xw_lock *l, *l2;
    wl_list_for_each_safe(l, l2, &sl->locks, link) {
        struct xw_lock_surface *ls, *ls2;
        wl_list_for_each_safe(ls, ls2, &l->surfaces, link) {
            if (ls->output != o)
                continue;
            if (ls->res) {
                struct wl_resource *r = ls->res;
                ls->res = NULL;
                wl_resource_destroy(r);
            } else {
                lock_surface_unmapped(ls);
                wl_list_remove(&ls->link);
                free(ls);
            }
        }
    }
}
