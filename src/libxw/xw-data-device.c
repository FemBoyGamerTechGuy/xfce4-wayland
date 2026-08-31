/* xw-data-device.c — wl_data_device_manager: clipboard selection and
 * drag-and-drop.
 *
 * Selection: set_selection records the owning source; all other clients
 * with bound data devices get a wl_data_offer wrapping the source's mime
 * types via the selection event. receive() forwards the fd straight to
 * the owner (source.send).
 *
 * DnD: start_drag grabs the seat; motion retargets enter/leave between
 * clients; drop delivers the drop event, the target then pulls the data
 * through offer.receive → source.send, and the source gets
 * dnd_drop_performed/dnd_finished.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unistd.h>

#define DATA_DEVICE_VERSION 3
#define XW_MAX_MIMES 16

struct xw_data_device_res {
    struct wl_resource *res;  /* wl_data_device */
    struct xw_seat *seat;
    struct wl_list link;      /* seat.data_devices */
    uint32_t last_selection_serial; /* serial of the last selection sent */
    struct wl_list offers;    /* xw_data_offer.device_link */
};

struct xw_data_source {
    struct wl_resource *res;  /* wl_data_source */
    struct xw_seat *seat;
    char *mimes[XW_MAX_MIMES];
    int n_mimes;
    uint32_t dnd_actions;
    struct wl_list link;      /* sources (module list, for cleanup) */
};

struct xw_data_offer {
    struct wl_resource *res;  /* wl_data_offer */
    struct wl_resource *source; /* NULL = selection-less offer */
    struct xw_data_device_res *device;
    bool in_dnd;
    struct wl_list device_link;
};

static struct wl_global *g_ddm;
static struct xw_compositor *g_ddm_comp;

/* ---------------------------------------------------------- data source */

static void source_offer(struct wl_client *client, struct wl_resource *res,
                         const char *mime_type);
static void source_destroy(struct wl_client *client, struct wl_resource *res);
static void source_set_actions(struct wl_client *client,
                               struct wl_resource *res, uint32_t dnd_actions);

static const struct wl_data_source_interface source_impl = {
    .offer = source_offer,
    .destroy = source_destroy,
    .set_actions = source_set_actions,
};

static struct xw_data_source *source_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void source_offer(struct wl_client *client, struct wl_resource *res,
                         const char *mime_type) {
    (void)client;
    struct xw_data_source *src = source_from_res(res);
    if (!src || src->n_mimes >= XW_MAX_MIMES || !mime_type)
        return;
    src->mimes[src->n_mimes++] = strdup(mime_type);
}

static void source_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void source_set_actions(struct wl_client *client,
                               struct wl_resource *res, uint32_t dnd_actions) {
    (void)client;
    struct xw_data_source *src = source_from_res(res);
    if (src)
        src->dnd_actions = dnd_actions;
}

static void source_resource_destroy(struct wl_resource *res) {
    struct xw_data_source *src = source_from_res(res);
    if (!src)
        return;
    /* unselect if this source is the current selection */
    struct xw_seat *seat = src->seat;
    if (seat && seat->selection_source == res) {
        seat->selection_source = NULL;
        seat->selection_client = NULL;
    }
    if (seat && seat->drag.source == res) {
        seat->drag.active = false;
        seat->drag.source = NULL;
    }
    for (int i = 0; i < src->n_mimes; i++)
        free(src->mimes[i]);
    wl_list_remove(&src->link);
    free(src);
}

/* ---------------------------------------------------------- data offer */

static void offer_accept(struct wl_client *client, struct wl_resource *res,
                         uint32_t serial, const char *mime_type);
static void offer_receive(struct wl_client *client, struct wl_resource *res,
                          const char *mime_type, int32_t fd);
static void offer_destroy(struct wl_client *client, struct wl_resource *res);
static void offer_finish(struct wl_client *client, struct wl_resource *res);
static void offer_set_actions(struct wl_client *client,
                              struct wl_resource *res, uint32_t dnd_actions,
                              uint32_t preferred_action);

static const struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = offer_destroy,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};

static struct xw_data_offer *offer_from_res(struct wl_resource *res) {
    return wl_resource_get_user_data(res);
}

static void offer_accept(struct wl_client *client, struct wl_resource *res,
                         uint32_t serial, const char *mime_type) {
    (void)client;
    (void)serial;
    struct xw_data_offer *off = offer_from_res(res);
    if (off && off->source)
        wl_data_source_send_target(off->source, mime_type);
}

static void offer_receive(struct wl_client *client, struct wl_resource *res,
                          const char *mime_type, int32_t fd) {
    (void)client;
    struct xw_data_offer *off = offer_from_res(res);
    if (off && off->source && mime_type)
        wl_data_source_send_send(off->source, mime_type, fd);
    else if (fd >= 0)
        close(fd); /* nothing to forward to */
}

static void offer_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void offer_finish(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    struct xw_data_offer *off = offer_from_res(res);
    if (off && off->source && off->in_dnd)
        wl_data_source_send_dnd_finished(off->source);
}

static void offer_set_actions(struct wl_client *client,
                              struct wl_resource *res, uint32_t dnd_actions,
                              uint32_t preferred_action) {
    (void)client;
    (void)res;
    (void)dnd_actions; /* action negotiation: v0 always "copy" */
    (void)preferred_action;
}

static void offer_resource_destroy(struct wl_resource *res) {
    struct xw_data_offer *off = offer_from_res(res);
    if (!off)
        return;
    wl_list_remove(&off->device_link);
    free(off);
}

/* create an offer resource for a device, wrapping a source's mimes */
static struct xw_data_offer *offer_create(struct xw_data_device_res *dev,
                                          struct wl_resource *source_res) {
    struct wl_client *client = wl_resource_get_client(dev->res);
    struct xw_data_offer *off = calloc(1, sizeof(*off));
    if (!off) {
        wl_client_post_no_memory(client);
        return NULL;
    }
    struct wl_resource *ores = wl_resource_create(
        client, &wl_data_offer_interface,
        wl_resource_get_version(dev->res), 0);
    if (!ores) {
        free(off);
        wl_client_post_no_memory(client);
        return NULL;
    }
    off->res = ores;
    off->source = source_res;
    off->device = dev;
    wl_list_insert(dev->offers.prev, &off->device_link);
    wl_resource_set_implementation(ores, &offer_impl, off,
                                   offer_resource_destroy);
    wl_data_device_send_data_offer(dev->res, ores);
    if (source_res) {
        struct xw_data_source *src = source_from_res(source_res);
        if (src) {
            for (int i = 0; i < src->n_mimes; i++)
                wl_data_offer_send_offer(ores, src->mimes[i]);
            if (wl_resource_get_version(ores) >=
                WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
                wl_data_offer_send_source_actions(
                    ores, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
            if (wl_resource_get_version(ores) >=
                WL_DATA_OFFER_ACTION_SINCE_VERSION)
                wl_data_offer_send_action(ores,
                                          WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        }
    }
    return off;
}

/* --------------------------------------------------------- data device */

static void device_start_drag(struct wl_client *client,
                              struct wl_resource *res,
                              struct wl_resource *source,
                              struct wl_resource *origin,
                              struct wl_resource *icon, uint32_t serial);
static void device_set_selection(struct wl_client *client,
                                 struct wl_resource *res,
                                 struct wl_resource *source, uint32_t serial);
static void device_release(struct wl_client *client, struct wl_resource *res);

static const struct wl_data_device_interface device_impl = {
    .start_drag = device_start_drag,
    .set_selection = device_set_selection,
    .release = device_release,
};

static void device_start_drag(struct wl_client *client,
                              struct wl_resource *res,
                              struct wl_resource *source,
                              struct wl_resource *origin,
                              struct wl_resource *icon, uint32_t serial) {
    (void)icon;
    (void)serial;
    struct xw_data_device_res *dev = wl_resource_get_user_data(res);
    struct xw_seat *seat = dev->seat;
    if (!seat)
        return;
    struct xw_surface *origin_surface = wl_resource_get_user_data(origin);
    if (!source || !origin_surface) {
        xw_log(XW_LOG_WARN, "data-device: start_drag without source/origin");
        return;
    }
    seat->drag.source = source;
    seat->drag.source_client = client;
    seat->drag.origin = origin_surface;
    seat->drag.active = true;
    seat->ptr_grab_is_drag = true;
    seat->grab_surface = origin_surface;
    /* implicit pointer grab: the origin's client resource */
    struct wl_resource *p = NULL;
    wl_list_for_each(p, &seat->pointers, link) {
        if (wl_resource_get_client(p) == client)
            break;
    }
    seat->ptr_grab = p;
    xw_log(XW_LOG_DEBUG, "data-device: drag started");
}

static void device_set_selection(struct wl_client *client,
                                 struct wl_resource *res,
                                 struct wl_resource *source, uint32_t serial) {
    struct xw_data_device_res *dev = wl_resource_get_user_data(res);
    struct xw_seat *seat = dev->seat;
    (void)serial;
    if (!seat)
        return;
    seat->selection_source = source; /* NULL clears the selection */
    seat->selection_client = source ? client : NULL;
    if (source)
        xw_log(XW_LOG_DEBUG, "data-device: selection set");

    /* notify everyone else; a NULL source clears their selection without
     * creating an offer (spec: selection offer may be null) */
    struct xw_data_device_res *d;
    wl_list_for_each(d, &seat->data_devices, link) {
        if (wl_resource_get_client(d->res) == client)
            continue;
        if (!source) {
            wl_data_device_send_selection(d->res, NULL);
            continue;
        }
        struct xw_data_offer *off = offer_create(d, source);
        if (off)
            wl_data_device_send_selection(d->res, off->res);
    }
}

static void device_release(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void device_resource_destroy(struct wl_resource *res) {
    struct xw_data_device_res *dev = wl_resource_get_user_data(res);
    if (!dev)
        return;
    struct xw_data_offer *off, *off2;
    wl_list_for_each_safe(off, off2, &dev->offers, device_link) {
        struct wl_resource *r = off->res;
        off->res = NULL;
        wl_resource_destroy(r);
    }
    wl_list_remove(&dev->link);
    free(dev);
}

/* ------------------------------------------------ data_device_manager */

static struct wl_list g_sources; /* xw_data_source.link */

static void ddm_create_source(struct wl_client *client,
                              struct wl_resource *res, uint32_t id) {
    struct xw_data_source *src = calloc(1, sizeof(*src));
    if (!src) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *sres =
        wl_resource_create(client, &wl_data_source_interface,
                           wl_resource_get_version(res), id);
    if (!sres) {
        free(src);
        wl_client_post_no_memory(client);
        return;
    }
    src->res = sres;
    src->dnd_actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    wl_list_insert(g_sources.prev, &src->link);
    wl_resource_set_implementation(sres, &source_impl, src,
                                   source_resource_destroy);
}

static void ddm_get_data_device(struct wl_client *client,
                                struct wl_resource *res, uint32_t id,
                                struct wl_resource *seat_res) {
    struct xw_seat *seat = wl_resource_get_user_data(seat_res);
    if (!seat)
        return;
    struct xw_data_device_res *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *dres =
        wl_resource_create(client, &wl_data_device_interface,
                           wl_resource_get_version(res), id);
    if (!dres) {
        free(dev);
        wl_client_post_no_memory(client);
        return;
    }
    dev->res = dres;
    dev->seat = seat;
    wl_list_init(&dev->offers);
    wl_list_insert(seat->data_devices.prev, &dev->link);
    wl_resource_set_implementation(dres, &device_impl, dev,
                                   device_resource_destroy);

    /* send the current selection to the newly-bound device */
    if (seat->selection_source &&
        seat->selection_client != wl_resource_get_client(dres)) {
        struct xw_data_offer *off = offer_create(dev, seat->selection_source);
        if (off)
            wl_data_device_send_selection(dres, off->res);
    }
}

static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_source,
    .get_data_device = ddm_get_data_device,
};

static void bind_ddm(struct wl_client *client, void *data, uint32_t version,
                     uint32_t id) {
    if (version > DATA_DEVICE_VERSION)
        version = DATA_DEVICE_VERSION;
    struct wl_resource *res = wl_resource_create(
        client, &wl_data_device_manager_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &ddm_impl, data, NULL);
}

/* --------------------------------------------------- compositor hooks */

void xw_data_device_init(struct xw_compositor *c) {
    g_ddm_comp = c;
    wl_list_init(&g_sources);
    g_ddm = wl_global_create(c->display, &wl_data_device_manager_interface,
                             DATA_DEVICE_VERSION, c, bind_ddm);
    if (!g_ddm)
        xw_log(XW_LOG_ERROR, "data device manager global creation failed");
}

void xw_data_device_fin(struct xw_compositor *c) {
    (void)c;
    /* sources and devices die with their clients */
    g_ddm = NULL;
    g_ddm_comp = NULL;
}

void xw_data_device_notify_focus(struct xw_compositor *c, struct xw_seat *seat) {
    if (!c || !seat || !seat->kb_focus)
        return;
    struct wl_client *focused = wl_resource_get_client(seat->kb_focus->res);
    if (seat->selection_client == focused)
        return;
    xw_data_device_send_selection(c, seat, focused);
}

void xw_data_device_send_selection(struct xw_compositor *c,
                                   struct xw_seat *seat,
                                   struct wl_client *client) {
    (void)c;
    if (!seat || !seat->selection_source || !client)
        return;
    struct xw_data_device_res *d;
    wl_list_for_each(d, &seat->data_devices, link) {
        if (wl_resource_get_client(d->res) == client) {
            struct xw_data_offer *off = offer_create(d, seat->selection_source);
            if (off)
                wl_data_device_send_selection(d->res, off->res);
            return;
        }
    }
}

/* ------------------------------------------------------------- dnd */

static struct xw_surface *drag_surface_at(struct xw_compositor *c,
                                          struct xw_seat *s, int x, int y) {
    /* windows and layers accept drops; reuse the seat's hit test by
     * temporarily clearing the drag grab */
    struct xw_surface *target;
    struct xw_surface *saved = s->grab_surface;
    s->grab_surface = NULL;
    /* inline copy of the seat hit test: popups then layers then windows */
    struct xw_popup *p;
    wl_list_for_each_reverse(p, &c->popups, link) {
        if (p->mapped && p->surface && xw_surface_has_input_at(p->surface, x, y)) {
            s->grab_surface = saved;
            return p->surface;
        }
    }
    for (int layer = 3; layer >= 0; layer--) {
        struct xw_layer_surface *ls;
        wl_list_for_each(ls, &c->wm->layers[layer], link) {
            if (ls->mapped && ls->surface &&
                xw_surface_has_input_at(ls->surface, x, y)) {
                s->grab_surface = saved;
                return ls->surface;
            }
        }
    }
    struct xw_window *w = xw_wm_window_at(c->wm, x, y, NULL);
    target = w ? w->surface : NULL;
    s->grab_surface = saved;
    return target;
}

void xw_data_device_drag_motion(struct xw_compositor *c, struct xw_seat *seat,
                                int x, int y) {
    struct xw_surface *target = drag_surface_at(c, seat, x, y);
    if (target != seat->drag.origin) {
        /* leave to the old target, enter to the new one */
        if (seat->drag.origin) {
            struct xw_data_device_res *d;
            wl_list_for_each(d, &seat->data_devices, link) {
                if (wl_resource_get_client(d->res) ==
                    wl_resource_get_client(seat->drag.origin->res))
                    wl_data_device_send_leave(d->res);
            }
        }
        if (target) {
            struct xw_data_device_res *d;
            wl_list_for_each(d, &seat->data_devices, link) {
                if (wl_resource_get_client(d->res) ==
                    wl_resource_get_client(target->res)) {
                    struct xw_data_offer *off = offer_create(d, seat->drag.source);
                    int tx = 0, ty = 0;
                    xw_surface_get_pos(target, &tx, &ty, NULL, NULL);
                    wl_data_device_send_enter(
                        d->res, seat->serial, target->res,
                        wl_fixed_from_int(x - tx),
                        wl_fixed_from_int(y - ty), off ? off->res : NULL);
                }
            }
        }
        seat->drag.origin = target;
    } else if (target) {
        struct xw_data_device_res *d;
        wl_list_for_each(d, &seat->data_devices, link) {
            if (wl_resource_get_client(d->res) ==
                wl_resource_get_client(target->res))
                wl_data_device_send_motion(d->res, (uint32_t)xw_now_ms(),
                                           wl_fixed_from_int(x),
                                           wl_fixed_from_int(y));
        }
    }
}

void xw_data_device_drag_drop(struct xw_compositor *c, struct xw_seat *seat) {
    (void)c;
    if (seat->drag.origin) {
        struct xw_data_device_res *d;
        wl_list_for_each(d, &seat->data_devices, link) {
            if (wl_resource_get_client(d->res) ==
                wl_resource_get_client(seat->drag.origin->res))
                wl_data_device_send_drop(d->res);
        }
    }
    if (seat->drag.source) {
        wl_data_source_send_dnd_drop_performed(seat->drag.source);
        /* the target pulls data via offer.receive; the source can
         * destroy itself after dnd_finished (v3) */
        if (wl_resource_get_version(seat->drag.source) >=
            WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION)
            wl_data_source_send_dnd_finished(seat->drag.source);
    }
    seat->drag.active = false;
    seat->drag.source = NULL;
    seat->drag.origin = NULL;
    seat->ptr_grab = NULL;
    seat->ptr_grab_is_drag = false;
    seat->grab_surface = NULL;
    xw_log(XW_LOG_DEBUG, "data-device: drag dropped");
}

void xw_data_device_drag_cancel(struct xw_compositor *c,
                                struct xw_seat *seat) {
    (void)c;
    if (seat->drag.origin) {
        struct xw_data_device_res *d;
        wl_list_for_each(d, &seat->data_devices, link) {
            if (wl_resource_get_client(d->res) ==
                wl_resource_get_client(seat->drag.origin->res))
                wl_data_device_send_leave(d->res);
        }
    }
    if (seat->drag.source)
        wl_data_source_send_cancelled(seat->drag.source);
    seat->drag.active = false;
    seat->drag.source = NULL;
    seat->drag.origin = NULL;
    seat->ptr_grab = NULL;
    seat->ptr_grab_is_drag = false;
    seat->grab_surface = NULL;
}
