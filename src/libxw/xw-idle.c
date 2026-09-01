/* xw-idle.c — ext-idle-notify-v1 (idle notifications for screensavers
 * and auto-lock).
 *
 * Semantics (per protocol): each notification fires `idle` once the
 * seat has seen no input for its timeout milliseconds, and `resumed`
 * when input arrives after that. The notification then re-arms and may
 * fire again after another idle period. Notifications are independent
 * (different timeouts do not interact).
 *
 * Implementation: the seat tracks last_activity_ms (updated by every
 * input entry point via xw_idle_activity()). Each notification owns one
 * event-loop timer armed for (last_activity + timeout - now); the timer
 * callback re-checks the deadline (input may have raced the timer
 * without the notification having been idle) and otherwise sends
 * `idle`. xw_idle_activity() resumes idle notifications immediately.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

#define IDLE_NOTIFY_VERSION 2

struct xw_idle_note {
    struct wl_resource *res;    /* ext_idle_notification_v1 */
    struct xw_compositor *comp;
    struct xw_seat *seat;
    uint32_t timeout_ms;
    bool idle;                  /* `idle` sent, awaiting input */
    struct wl_event_source *timer;
    struct wl_list link;        /* xw_idle.notes */
};

struct xw_idle {
    struct wl_global *global;
    struct xw_compositor *comp;
    struct wl_list notes;       /* xw_idle_note.link */
};

/* ------------------------------------------------------------- helpers */

static struct xw_idle *idle_of(struct xw_compositor *c) {
    return c ? c->idle_state : NULL;
}

/* arm the timer for the remaining idle window. NOTE: a delay of 0
 * would DISARM the timer in libwayland, not fire it — notifications
 * whose deadline already elapsed (created after the seat went idle)
 * must use the minimum 1 ms so they fire on the next loop pass. */
static void note_rearm(struct xw_idle_note *n) {
    if (!n->timer)
        return;
    int64_t now = xw_now_ms();
    int64_t deadline = n->seat->last_activity_ms + (int64_t)n->timeout_ms;
    int delay = deadline > now ? (int)(deadline - now) : 1;
    wl_event_source_timer_update(n->timer, delay);
}

static int note_timer_cb(void *data) {
    struct xw_idle_note *n = data;
    if (!n->res || !n->seat)
        return 0;
    int64_t now = xw_now_ms();
    int64_t deadline = n->seat->last_activity_ms + (int64_t)n->timeout_ms;
    if (now < deadline) {
        /* input raced the timer before we went idle: re-arm */
        note_rearm(n);
        return 0;
    }
    if (!n->idle) {
        n->idle = true;
        ext_idle_notification_v1_send_idled(n->res);
    }
    /* while idle we stay idle until input arrives (which re-arms);
     * disarm so the timer does not spin */
    wl_event_source_timer_update(n->timer, 0);
    return 0;
}

/* ------------------------------------------------------- notifications */

static void note_destroy_req(struct wl_client *client,
                             struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct ext_idle_notification_v1_interface note_impl = {
    .destroy = note_destroy_req,
};

static void note_resource_destroy(struct wl_resource *res) {
    struct xw_idle_note *n = wl_resource_get_user_data(res);
    if (!n)
        return;
    if (n->timer)
        wl_event_source_remove(n->timer);
    wl_list_remove(&n->link);
    free(n);
}

/* ------------------------------------------------------------ notifier */

static void notifier_get_idle_notification(struct wl_client *client,
                                           struct wl_resource *res,
                                           uint32_t id, uint32_t timeout,
                                           struct wl_resource *seat) {
    struct xw_idle *idle = wl_resource_get_user_data(res);
    struct xw_compositor *c = idle->comp;
    struct xw_seat *s = seat ? wl_resource_get_user_data(seat) : NULL;

    /* the wl_seat resource must be one of ours (defensive) */
    bool ours = false;
    if (s) {
        struct xw_seat *it;
        wl_list_for_each(it, &c->seats, link) {
            if (it == s) {
                ours = true;
                break;
            }
        }
    }
    if (!ours) {
        wl_resource_post_error(res, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "get_idle_notification: unknown seat");
        return;
    }

    struct xw_idle_note *n = calloc(1, sizeof(*n));
    if (!n) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *nres = wl_resource_create(
        client, &ext_idle_notification_v1_interface,
        wl_resource_get_version(res), id);
    if (!nres) {
        free(n);
        wl_client_post_no_memory(client);
        return;
    }
    n->res = nres;
    n->comp = c;
    n->seat = s;
    n->timeout_ms = timeout;
    wl_list_insert(idle->notes.prev, &n->link);
    wl_resource_set_implementation(nres, &note_impl, n,
                                   note_resource_destroy);
    n->timer = wl_event_loop_add_timer(c->loop, note_timer_cb, n);
    note_rearm(n);
}

/* v2: input-only idle. Our only activity source is input events (there
 * are no sensors or other presence signals in this compositor), so the
 * behavior is identical to get_idle_notification — documented in
 * ARCHITECTURE.md. */
static void notifier_get_input_idle_notification(
    struct wl_client *client, struct wl_resource *res, uint32_t id,
    uint32_t timeout, struct wl_resource *seat) {
    notifier_get_idle_notification(client, res, id, timeout, seat);
}

static void notifier_destroy(struct wl_client *client,
                             struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct ext_idle_notifier_v1_interface notifier_impl = {
    .destroy = notifier_destroy,
    .get_idle_notification = notifier_get_idle_notification,
    .get_input_idle_notification = notifier_get_input_idle_notification,
};

static void bind_idle_notifier(struct wl_client *client, void *data,
                               uint32_t version, uint32_t id) {
    if (version > IDLE_NOTIFY_VERSION)
        version = IDLE_NOTIFY_VERSION;
    struct wl_resource *res = wl_resource_create(
        client, &ext_idle_notifier_v1_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &notifier_impl, data, NULL);
}

/* ------------------------------------------------------------ lifecycle */

void xw_idle_init(struct xw_compositor *c) {
    struct xw_idle *idle = calloc(1, sizeof(*idle));
    if (!idle)
        return;
    idle->comp = c;
    wl_list_init(&idle->notes);
    idle->global = wl_global_create(c->display, &ext_idle_notifier_v1_interface,
                                    IDLE_NOTIFY_VERSION, idle,
                                    bind_idle_notifier);
    if (!idle->global) {
        xw_log(XW_LOG_ERROR, "idle notifier global creation failed");
        free(idle);
        return;
    }
    c->idle_state = idle;
}

void xw_idle_fin(struct xw_compositor *c) {
    struct xw_idle *idle = idle_of(c);
    if (!idle)
        return;
    /* note resources were already destroyed by
     * wl_display_destroy_clients; only our own allocations remain */
    if (idle->global)
        wl_global_destroy(idle->global);
    struct xw_idle_note *n, *n2;
    wl_list_for_each_safe(n, n2, &idle->notes, link) {
        if (n->timer)
            wl_event_source_remove(n->timer);
        wl_list_remove(&n->link);
        free(n);
    }
    free(idle);
    c->idle_state = NULL;
}

/* ------------------------------------------------------------- activity */

void xw_idle_activity(struct xw_seat *s) {
    struct xw_compositor *c = s ? s->comp : NULL;
    struct xw_idle *idle = idle_of(c);
    if (!idle)
        return;
    s->last_activity_ms = xw_now_ms();
    struct xw_idle_note *n;
    wl_list_for_each(n, &idle->notes, link) {
        if (n->seat != s || !n->idle)
            continue;
        n->idle = false;
        ext_idle_notification_v1_send_resumed(n->res);
        note_rearm(n);
    }
}

void xw_idle_seat_destroyed(struct xw_compositor *c, struct xw_seat *s) {
    struct xw_idle *idle = idle_of(c);
    if (!idle)
        return;
    /* destroy the notifications bound to this seat; their resources die
     * with them (no events are sent) */
    struct xw_idle_note *n, *n2;
    wl_list_for_each_safe(n, n2, &idle->notes, link) {
        if (n->seat == s)
            wl_resource_destroy(n->res);
    }
}
