/* xw-backend-drm.c — real DRM/KMS backend for physical TTY sessions.
 *
 * Owns the display hardware through the kernel's KMS interface: device
 * discovery, connector/mode enumeration, CRTCs, dumb-buffer scanout
 * (software rendering via the shared pixman pipeline), page flips and
 * hotplug. All device access goes through the compositor's seat
 * provider (xw-session-seat.c) — never a privileged direct open, never
 * a hardcoded /dev/dri/card0.
 *
 * Rendering path: the generic repaint machinery composites into the
 * output's native a8r8g8b8 buffer (pixman); present() copies it into a
 * dumb scanout buffer (XRGB8888 — same B,G,R,X byte layout) and queues
 * a page flip. Two buffers per output; while a flip is in flight the
 * frame is parked and flips on vblank (frame pacing without tearing).
 * Drivers that reject page flips (virtio without 3d, simple panels)
 * fall back to immediate modeset updates with a logged, honest
 * warning.
 *
 * Session lifecycle: when the seat session is disabled (VT switch away)
 * this backend drops DRM master, suspends input and stops presenting;
 * on enable it re-acquires master and repaints everything (another VT
 * may have drawn on the screen).
 *
 * Pure planning logic (mode selection, connector naming, CRTC
 * assignment) lives in the DRM-independent section below and is
 * white-box tested (tests/suite/test_drm.c); the ioctl paths are
 * covered by the manual hardware checklist (TESTING.md) — they need a
 * real /dev/dri and a monitor.
 */
#include "xw-internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- planning ---
 * Pure functions over plain structs (no libdrm types, no ioctls) so the
 * test suite can exercise them in any build. */

#define XW_DRM_MODE_PREFERRED 0x40 /* == DRM_MODE_TYPE_PREFERRED */

/* Pick the mode to use for a connector: the preferred mode (what the
 * monitor advertises as its native timing — largest if several), else
 * the largest area at the highest refresh, else the first. NULL when
 * there is no mode at all. */
const struct xw_drm_mode *xw_drm_pick_mode(const struct xw_drm_mode *modes,
                                           int count) {
    if (!modes || count <= 0)
        return NULL;
    const struct xw_drm_mode *best = &modes[0];
    for (int i = 1; i < count; i++) {
        const struct xw_drm_mode *m = &modes[i];
        bool mp = m->type & XW_DRM_MODE_PREFERRED;
        bool bp = best->type & XW_DRM_MODE_PREFERRED;
        if (mp && bp) {
            if (m->hdisplay * m->vdisplay > best->hdisplay * best->vdisplay)
                best = m;
        } else if (mp) {
            best = m;
        } else if (bp) {
            continue;
        } else {
            long ma = (long)m->hdisplay * m->vdisplay;
            long ba = (long)best->hdisplay * best->vdisplay;
            if (ma > ba || (ma == ba && m->vrefresh > best->vrefresh))
                best = m;
        }
    }
    return best;
}

/* KMS connector-name mapping — our own constants carrying the numeric
 * values of the kernel's DRM_MODE_CONNECTOR_*, so this table is usable
 * (and testable) without libdrm headers. */
#define XW_C_UNKNOWN 0
#define XW_C_VGA 1
#define XW_C_DVII 2
#define XW_C_DVID 3
#define XW_C_DVIA 4
#define XW_C_COMPOSITE 5
#define XW_C_SVIDEO 6
#define XW_C_LVDS 7
#define XW_C_COMPONENT 8
#define XW_C_9PINDIN 9
#define XW_C_DP 10
#define XW_C_HDMIA 11
#define XW_C_HDMIB 12
#define XW_C_TV 13
#define XW_C_EDP 14
#define XW_C_VIRTUAL 15
#define XW_C_DSI 16
#define XW_C_DPI 17
#define XW_C_WRITEBACK 18
#define XW_C_SPI 19
#define XW_C_USB 20

const char *xw_drm_connector_type_name(uint32_t type) {
    switch (type) {
    case XW_C_VGA: return "VGA";
    case XW_C_DVII: return "DVI-I";
    case XW_C_DVID: return "DVI-D";
    case XW_C_DVIA: return "DVI-A";
    case XW_C_COMPOSITE: return "Composite";
    case XW_C_SVIDEO: return "S-Video";
    case XW_C_LVDS: return "LVDS";
    case XW_C_COMPONENT: return "Component";
    case XW_C_9PINDIN: return "9-Pin-DIN";
    case XW_C_DP: return "DP";
    case XW_C_HDMIA: return "HDMI-A";
    case XW_C_HDMIB: return "HDMI-B";
    case XW_C_TV: return "TV";
    case XW_C_EDP: return "eDP";
    case XW_C_VIRTUAL: return "Virtual";
    case XW_C_DSI: return "DSI";
    case XW_C_DPI: return "DPI";
    case XW_C_WRITEBACK: return "Writeback";
    case XW_C_SPI: return "SPI";
    case XW_C_USB: return "USB";
    default: return "Unknown";
    }
}

void xw_drm_connector_name(uint32_t type, uint32_t type_id, char *buf,
                           size_t len) {
    snprintf(buf, len, "%s-%u", xw_drm_connector_type_name(type), type_id);
}

/* CRTC/encoder planning: pick the CRTC to drive a connector.
 *   conn_enc     encoder currently attached to the connector (0 = none)
 *   enc_crtc     CRTC that encoder drives in the firmware's own
 *                assignment (0 = none) — reuse it like every KMS
 *                compositor does, so the primary display stays on the
 *                CRTC the firmware lit up
 *   enc_possible bitmask over crtc indices the encoder CAN drive
 *   crtc_ids     the available CRTC ids
 *   crtc_taken   in/out: which CRTCs are already claimed
 * Returns the chosen CRTC index (and claims it in crtc_taken), or -1. */
int xw_drm_plan_crtc(int conn_enc, int enc_crtc, uint32_t enc_possible,
                     const int *crtc_ids, int n_crtcs, bool *crtc_taken) {
    if (conn_enc > 0 && enc_crtc > 0) {
        for (int i = 0; i < n_crtcs; i++) {
            if (crtc_ids[i] == enc_crtc && !crtc_taken[i]) {
                crtc_taken[i] = true;
                return i;
            }
        }
    }
    for (int i = 0; i < n_crtcs; i++) {
        if ((enc_possible & (1u << i)) && !crtc_taken[i]) {
            crtc_taken[i] = true;
            return i;
        }
    }
    return -1;
}

/* --------------------------------------------------------------- backend ---
 * Everything below needs libdrm and is compiled only when it was found
 * at build time (XW_HAVE_DRM_BACKEND). */

#ifdef XW_HAVE_DRM_BACKEND

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_MAX_OUTPUTS 8
#define DRM_MAX_MODES 64

/* page-flip watchdog: a vblank is at most ~17ms, so a flip the driver
 * ACCEPTED that still has not completed after 300ms (~20 frames)
 * means the vblank event is never coming. The real-world offender is
 * the NVIDIA proprietary DRM driver: drmModePageFlip() returns 0 on
 * the legacy path but no DRM_EVENT_PAGE_FLIP is ever delivered, so
 * waiting_flip would stay set forever and every later frame would
 * stay parked — a silently frozen display. */
#define XW_FLIP_TIMEOUT_MS 300
/* startup diagnostics window: 2s DRM presentation stats lines */
#define XW_DRM_STATS_MS 2000
#define XW_DRM_STATS_TICKS 15

struct drm_bo { /* one dumb-buffer scanout object */
    uint32_t fb_id;  /* KMS framebuffer id */
    uint32_t handle; /* GEM handle */
    uint32_t pitch;
    uint32_t size;
    uint32_t *map;   /* mmap'd scanout memory */
};

struct drm_output_priv {
    struct drm_backend *b;
    struct xw_output *out; /* the compositor-side output (1:1) */
    uint32_t conn_id;
    uint32_t crtc_id;
    drmModeCrtcPtr saved_crtc; /* original CRTC state for restoration */
    drmModeModeInfo mode;
    char name[32];
    struct drm_bo bos[2];
    int current;       /* index of the buffer being scanned out */
    bool waiting_flip; /* a page flip is in flight */
    bool parked_frame; /* a frame arrived while a flip was in flight */
    bool no_flip;      /* driver rejects page flips: immediate updates */
    int64_t flip_ms;   /* when the in-flight flip was queued (watchdog) */
    /* presentation counters for the startup stats window: presents =
     * frames handed to present(), flips = flips the driver accepted,
     * events = vblank events actually delivered, parked = frames
     * dropped while a flip was in flight */
    uint64_t n_presents, n_flips, n_events, n_parked;
    struct wl_list link;
};

struct drm_backend {
    struct xw_backend base;
    struct xw_seat_session *seat; /* borrowed from the compositor */
    int drm_fd;
    int seat_dev_id; /* device id at the seat provider */
    char dev_path[64];
    struct wl_event_source *drm_src;  /* page flip events */
    struct wl_event_source *udev_src; /* hotplug */
    struct udev *udev;
    struct udev_monitor *mon;
    struct wl_list outputs;
    bool active; /* seat session active (VT visible) */
    bool master; /* we hold DRM master */
    struct wl_event_source *flip_timer; /* recurring flip watchdog */
    struct wl_event_source *stats_src;  /* startup stats window */
    int stats_ticks;
    uint64_t n_watchdog; /* watchdog fallbacks so far */
    char driver[32];     /* kernel driver name — "nvidia" matters here */
};

static struct drm_backend *db_of(struct xw_backend *b) {
    return (struct drm_backend *)b;
}

static struct drm_output_priv *op_of(struct drm_backend *db,
                                     struct xw_output *o) {
    struct drm_output_priv *op;
    wl_list_for_each(op, &db->outputs, link) {
        if (op->out == o)
            return op;
    }
    return NULL;
}

/* ------------------------------------------------------------- dumb buffers */

static void bo_destroy(struct drm_backend *db, struct drm_bo *bo) {
    if (bo->map)
        munmap(bo->map, bo->size);
    if (bo->fb_id)
        drmModeRmFB(db->drm_fd, bo->fb_id);
    if (bo->handle) {
        struct drm_mode_destroy_dumb d = {.handle = bo->handle};
        drmIoctl(db->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    memset(bo, 0, sizeof(*bo));
}

static int bo_create(struct drm_backend *db, struct drm_bo *bo, int w, int h,
                     const char *name) {
    memset(bo, 0, sizeof(*bo));
    struct drm_mode_create_dumb create = {
        .width = (uint32_t)w, .height = (uint32_t)h, .bpp = 32};
    if (drmIoctl(db->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        xw_log(XW_LOG_ERROR,
               "drm: framebuffer allocation failed (dumb buffer for %s, "
               "%dx%d): %s — the driver may not support dumb buffers",
               name, w, h, strerror(errno));
        return -1;
    }
    bo->handle = create.handle;
    bo->pitch = create.pitch;
    bo->size = create.size;

    struct drm_mode_map_dumb map = {.handle = create.handle};
    if (drmIoctl(db->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        xw_log(XW_LOG_ERROR, "drm: cannot map the %s dumb buffer: %s", name,
               strerror(errno));
        bo_destroy(db, bo);
        return -1;
    }
    bo->map = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   db->drm_fd, map.offset);
    if (bo->map == MAP_FAILED) {
        bo->map = NULL;
        xw_log(XW_LOG_ERROR, "drm: mmap of the %s dumb buffer failed: %s",
               name, strerror(errno));
        bo_destroy(db, bo);
        return -1;
    }

    /* register as a KMS framebuffer: 24bpp depth over the 32bpp XRGB
     * buffer — the classic dumb-scanout configuration */
    if (drmModeAddFB(db->drm_fd, w, h, 24, 32, bo->pitch, bo->handle,
                     &bo->fb_id) < 0) {
        xw_log(XW_LOG_ERROR, "drm: drmModeAddFB(%s) failed: %s", name,
               strerror(errno));
        bo_destroy(db, bo);
        return -1;
    }
    return 0;
}

/* copy the composited native frame into a dumb buffer (row by row: the
 * pitch may be padded past width*4) */
static void bo_copy_frame(struct drm_bo *bo, const struct xw_output *o) {
    size_t row_bytes = (size_t)o->width * o->scale * 4;
    for (int y = 0; y < o->height * o->scale; y++) {
        memcpy((uint8_t *)bo->map + (size_t)y * bo->pitch,
               (const uint8_t *)o->native_data + (size_t)y * row_bytes,
               row_bytes);
    }
}

/* ------------------------------------------------------------ page flipping */

static void page_flip_complete(int fd, unsigned int sequence,
                               unsigned int tv_sec, unsigned int tv_usec,
                               void *user_data);

static void output_queue_flip(struct drm_backend *db,
                              struct drm_output_priv *op) {
    if (op->no_flip)
        return;
    int next = 1 - op->current;
    if (drmModePageFlip(db->drm_fd, op->crtc_id, op->bos[next].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, op) == 0) {
        op->current = next;
        op->waiting_flip = true;
        op->flip_ms = xw_now_ms();
        op->n_flips++;
        return;
    }
    if (errno == EINVAL || errno == ENOSPC || errno == EOPNOTSUPP) {
        /* some drivers (virtio/KVM without 3d, simple panels) cannot
         * page flip at all: fall back to immediate modeset updates and
         * say so — an honest degradation, not a silent one */
        xw_log(XW_LOG_WARN,
               "drm: page flips rejected by the driver (%s) on %s — "
               "switching to immediate buffer updates (possible tearing, "
               "fully functional)",
               strerror(errno), op->name);
        op->no_flip = true;
    } else {
        xw_log(XW_LOG_WARN, "drm: page flip on %s failed: %s", op->name,
               strerror(errno));
    }
}

/* present(): called from the repaint path with a finished frame in the
 * output's native buffer */
static void db_present(struct xw_backend *b, struct xw_output *o) {
    struct drm_backend *db = db_of(b);
    if (!db->active)
        return;
    struct drm_output_priv *op = op_of(db, o);
    if (!op)
        return;
    op->n_presents++;
    if (op->no_flip) {
        /* immediate mode: copy into the buffer being scanned out */
        bo_copy_frame(&op->bos[op->current], o);
        return;
    }
    if (op->waiting_flip) {
        /* a flip is in flight: park this frame; it flips on vblank */
        op->n_parked++;
        op->parked_frame = true;
        return;
    }
    bo_copy_frame(&op->bos[1 - op->current], o);
    output_queue_flip(db, op);
}

static void page_flip_complete(int fd, unsigned int sequence,
                               unsigned int tv_sec, unsigned int tv_usec,
                               void *user_data) {
    (void)fd;
    (void)sequence;
    (void)tv_sec;
    (void)tv_usec;
    struct drm_output_priv *op = user_data;
    op->n_events++;
    op->waiting_flip = false;
    if (op->parked_frame) {
        op->parked_frame = false;
        bo_copy_frame(&op->bos[1 - op->current], op->out);
        output_queue_flip(op->b, op);
    }
}

static int db_on_drm_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct drm_backend *db = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        xw_log(XW_LOG_ERROR, "drm: device fd error; stopping");
        xw_compositor_stop(db->base.comp);
        return 0;
    }
    drmEventContext ctx = {.version = DRM_EVENT_CONTEXT_VERSION,
                           .vblank_handler = page_flip_complete};
    drmHandleEvent(db->drm_fd, &ctx);
    return 0;
}

/* --------------------------------------------- flip watchdog + stats --- */

/* Recurring (100ms) watchdog over the page-flip event path — see
 * XW_FLIP_TIMEOUT_MS above for why it must exist. Without it, a
 * driver that accepts flips but never delivers the vblank event
 * freezes the display on the last completed frame for the whole
 * session while the compositor keeps "presenting" into parked
 * buffers — the exact "picture renders, cursor never moves"
 * symptom. The 100ms cadence adds no wakeups beyond the event loop's
 * own 100ms dispatch timeout. */
static int db_flip_watchdog_cb(void *data) {
    struct drm_backend *db = data;
    if (db->active) {
        int64_t now = xw_now_ms();
        struct drm_output_priv *op;
        wl_list_for_each(op, &db->outputs, link) {
            if (!op->waiting_flip ||
                now - op->flip_ms < XW_FLIP_TIMEOUT_MS)
                continue; /* nothing overdue */
            db->n_watchdog++;
            op->waiting_flip = false;
            op->parked_frame = false;
            op->no_flip = true;
            xw_log(XW_LOG_WARN,
                   "drm: page flip on %s never completed — the driver "
                   "ACCEPTED the flip but no vblank event arrived within "
                   "%dms (the NVIDIA proprietary driver does this on the "
                   "legacy page-flip path). Switching %s to immediate "
                   "buffer updates: frames become visible without "
                   "vblank sync (slight tearing possible, otherwise "
                   "fully functional).",
                   op->name, XW_FLIP_TIMEOUT_MS, op->name);
            /* pin scanout to the buffer present() now writes into and
             * show the newest composited frame immediately — this is
             * the moment a frozen display un-freezes */
            if (drmModeSetCrtc(db->drm_fd, op->crtc_id,
                               op->bos[op->current].fb_id, 0, 0,
                               &op->conn_id, 1, &op->mode) < 0)
                xw_log(XW_LOG_ERROR,
                       "drm: cannot re-program %s after the flip "
                       "watchdog tripped: %s — the display may stay "
                       "frozen",
                       op->name, strerror(errno));
            bo_copy_frame(&op->bos[op->current], op->out);
        }
    }
    wl_event_source_timer_update(db->flip_timer, 100);
    return 0;
}

/* 2s INFO presentation stats for the first 30s — the display half of
 * the "cursor does not move" bisect, visible at the DEFAULT log
 * level (right next to input's stats lines). flips>0 with
 * events==0 is the signature of a dead flip path, usually right
 * before the watchdog trips and fixes it. */
static int db_stats_timer_cb(void *data) {
    struct drm_backend *db = data;
    uint64_t presents = 0, flips = 0, events = 0, parked = 0;
    int no_flip = 0, in_flight = 0;
    struct drm_output_priv *op;
    wl_list_for_each(op, &db->outputs, link) {
        presents += op->n_presents;
        flips += op->n_flips;
        events += op->n_events;
        parked += op->n_parked;
        no_flip += op->no_flip;
        in_flight += op->waiting_flip;
    }
    xw_log(XW_LOG_INFO,
           "drm: stats: driver=%s presents=%llu flips=%llu "
           "vblank-events=%llu parked=%llu no-flip=%d in-flight=%d "
           "watchdog=%llu%s",
           db->driver[0] ? db->driver : "?",
           (unsigned long long)presents, (unsigned long long)flips,
           (unsigned long long)events, (unsigned long long)parked,
           no_flip, in_flight, (unsigned long long)db->n_watchdog,
           (flips > 0 && events == 0)
               ? " [flips queued but NO vblank events — dead flip path, "
                 "the watchdog will switch to immediate updates]"
               : "");
    if (++db->stats_ticks >= XW_DRM_STATS_TICKS) {
        wl_event_source_remove(db->stats_src);
        db->stats_src = NULL;
        return 0; /* source removed; nothing more to schedule */
    }
    wl_event_source_timer_update(db->stats_src, XW_DRM_STATS_MS);
    return 0;
}

/* ------------------------------------------------------- session lifecycle */

/* The seat session is going inactive: release scanout resources. The
 * caller (seat module) sends the disable ack after this returns. */
static void db_session_disable(void *ud) {
    struct drm_backend *db = ud;
    xw_log(XW_LOG_INFO, "drm: session inactive: dropping DRM master");
    db->active = false;
#ifdef XW_HAVE_LIBINPUT
    if (db->base.comp->input)
        xw_input_libinput_suspend(db->base.comp->input);
#endif
    if (db->master) {
        drmDropMaster(db->drm_fd);
        db->master = false;
    }
}

/* session active again: re-take master, repaint everything (another VT
 * may have drawn on the screen while we were away) */
static void db_session_enable(void *ud) {
    struct drm_backend *db = ud;
    xw_log(XW_LOG_INFO, "drm: session active: re-acquiring DRM master");
    if (!drmIsMaster(db->drm_fd)) {
        if (drmSetMaster(db->drm_fd) < 0) {
            xw_log(XW_LOG_ERROR,
                   "drm: cannot re-acquire DRM master after session "
                   "activation: %s — the display may be owned by another "
                   "compositor; retrying on the next switch",
                   strerror(errno));
            return; /* stay inactive; a later enable retries */
        }
        db->master = true;
    }
    db->active = true;
#ifdef XW_HAVE_LIBINPUT
    if (db->base.comp->input)
        xw_input_libinput_resume(db->base.comp->input);
#endif
    /* full damage: screen content is unknown after the switch */
    struct drm_output_priv *op;
    wl_list_for_each(op, &db->outputs, link) {
        xw_output_damage_rect(op->out, op->out->x, op->out->y,
                              op->out->width, op->out->height);
        op->parked_frame = false;
        op->waiting_flip = false;
    }
    xw_schedule_repaint(db->base.comp);
}

/* ------------------------------------------------------------ hotplug (udev) */

/* re-enumerate connectors: tear down outputs whose connector went away
 * (monitor unplugged). Connector ADDITIONS are detected and logged; v0
 * does not modeset new monitors live — that needs relayout work and is
 * tracked in the ROADMAP (honest, not hidden). */
static void db_hotplug(struct drm_backend *db) {
    xw_log(XW_LOG_INFO, "drm: hotplug event on %s", db->dev_path);
    drmModeResPtr res = drmModeGetResources(db->drm_fd);
    if (!res)
        return;

    struct drm_output_priv *op, *tmp;
    wl_list_for_each_safe(op, tmp, &db->outputs, link) {
        bool found = false;
        for (int i = 0; i < res->count_connectors; i++) {
            if (res->connectors[i] == op->conn_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            xw_log(XW_LOG_INFO, "drm: connector %s disappeared", op->name);
            bo_destroy(db, &op->bos[0]);
            bo_destroy(db, &op->bos[1]);
            if (op->saved_crtc)
                drmModeFreeCrtc(op->saved_crtc);
            wl_list_remove(&op->link);
            xw_output_destroy(op->out);
            free(op);
        }
    }

    for (int i = 0; i < res->count_connectors; i++) {
        bool known = false;
        wl_list_for_each(op, &db->outputs, link) {
            if (op->conn_id == res->connectors[i])
                known = true;
        }
        if (!known) {
            drmModeConnectorPtr conn =
                drmModeGetConnector(db->drm_fd, res->connectors[i]);
            if (conn && conn->connection == DRM_MODE_CONNECTED) {
                char nm[32];
                xw_drm_connector_name(conn->connector_type,
                                      conn->connector_type_id, nm,
                                      sizeof(nm));
                xw_log(XW_LOG_INFO,
                       "drm: connector %s was plugged in — live modeset "
                       "of new monitors is not implemented yet (see "
                       "ROADMAP); it becomes available after the next "
                       "session start", nm);
            }
            if (conn)
                drmModeFreeConnector(conn);
        }
    }
    drmModeFreeResources(res);
}

static int db_on_udev_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
        return 0;
    struct drm_backend *db = data;
    struct udev_device *dev = udev_monitor_receive_device(db->mon);
    if (!dev)
        return 0;
    const char *node = udev_device_get_devnode(dev);
    if (node && strcmp(node, db->dev_path) == 0)
        db_hotplug(db);
    udev_device_unref(dev);
    return 0;
}

/* ---------------------------------------------------------------- teardown */

static void db_destroy(struct xw_backend *b) {
    struct drm_backend *db = db_of(b);
    xw_log(XW_LOG_INFO, "drm: restoring display state");

    /* stop watching the device before touching it */
    if (db->udev_src)
        wl_event_source_remove(db->udev_src);
    if (db->mon)
        udev_monitor_unref(db->mon);
    if (db->udev)
        udev_unref(db->udev);
    if (db->drm_src)
        wl_event_source_remove(db->drm_src);
    if (db->flip_timer)
        wl_event_source_remove(db->flip_timer);
    if (db->stats_src)
        wl_event_source_remove(db->stats_src);

    /* every output: restore the CRTC the firmware had before us, then
     * release the buffers. Restoring a previously-DISABLED CRTC matters
     * too: a lit black screen is not a restored TTY. */
    struct drm_output_priv *op, *tmp;
    wl_list_for_each_safe(op, tmp, &db->outputs, link) {
        if (op->saved_crtc && db->drm_fd >= 0) {
            if (op->saved_crtc->buffer_id) {
                drmModeSetCrtc(db->drm_fd, op->saved_crtc->crtc_id,
                               op->saved_crtc->buffer_id,
                               op->saved_crtc->x, op->saved_crtc->y,
                               &op->conn_id, 1, &op->saved_crtc->mode);
            } else {
                drmModeSetCrtc(db->drm_fd, op->crtc_id, 0, 0, 0, NULL, 0,
                               NULL);
            }
            drmModeFreeCrtc(op->saved_crtc);
            op->saved_crtc = NULL;
        }
        bo_destroy(db, &op->bos[0]);
        bo_destroy(db, &op->bos[1]);
        wl_list_remove(&op->link);
        free(op); /* the xw_output itself is freed by the generic teardown */
    }

    if (db->master) {
        drmDropMaster(db->drm_fd);
        db->master = false;
    }
    /* release the device through the seat provider */
    if (db->drm_fd >= 0) {
        if (db->seat)
            xw_seat_session_close_device(db->seat, db->seat_dev_id);
        else
            close(db->drm_fd);
    }
    free(db);
}

static const struct xw_backend_ops drm_ops = {
    .present = db_present,
    .destroy = db_destroy,
};

/* ------------------------------------------------------------------ create */

/* create one output for a connected connector: pick the mode, allocate
 * the double-buffered dumb scanout, save + program the CRTC. Returns
 * the output's width (>=0) or -1; the failure diagnostics are logged. */
static int add_output(struct drm_backend *db, drmModeConnectorPtr conn,
                      uint32_t crtc_id, int layout_x) {
    struct drm_output_priv *op = calloc(1, sizeof(*op));
    if (!op)
        return -1;
    op->b = db;
    op->conn_id = conn->connector_id;
    op->crtc_id = crtc_id;
    xw_drm_connector_name(conn->connector_type, conn->connector_type_id,
                          op->name, sizeof(op->name));

    /* convert to the DRM-independent mode structs and pick */
    struct xw_drm_mode modes[DRM_MAX_MODES];
    int n_modes = conn->count_modes > DRM_MAX_MODES ? DRM_MAX_MODES
                                                    : conn->count_modes;
    for (int i = 0; i < n_modes; i++) {
        modes[i].hdisplay = conn->modes[i].hdisplay;
        modes[i].vdisplay = conn->modes[i].vdisplay;
        modes[i].vrefresh = conn->modes[i].vrefresh;
        modes[i].type = conn->modes[i].type;
    }
    const struct xw_drm_mode *picked = xw_drm_pick_mode(modes, n_modes);
    if (!picked) {
        xw_log(XW_LOG_ERROR, "drm: connector %s has no usable mode",
               op->name);
        free(op);
        return -1;
    }
    op->mode.hdisplay = (uint32_t)picked->hdisplay;
    op->mode.vdisplay = (uint32_t)picked->vdisplay;
    op->mode.vrefresh = (uint32_t)picked->vrefresh;
    /* fill the timing details from the original table entry */
    for (int i = 0; i < n_modes; i++) {
        if ((int)conn->modes[i].hdisplay == picked->hdisplay &&
            (int)conn->modes[i].vdisplay == picked->vdisplay &&
            (int)conn->modes[i].vrefresh == picked->vrefresh) {
            op->mode = conn->modes[i];
            break;
        }
    }
    int w = (int)op->mode.hdisplay, h = (int)op->mode.vdisplay;

    /* save the CRTC's current state for restoration on exit/failure */
    op->saved_crtc = drmModeGetCrtc(db->drm_fd, crtc_id);

    /* buffers (two: page flipping) */
    if (bo_create(db, &op->bos[0], w, h, op->name) < 0 ||
        bo_create(db, &op->bos[1], w, h, op->name) < 0) {
        if (op->saved_crtc)
            drmModeFreeCrtc(op->saved_crtc);
        free(op);
        return -1;
    }
    /* start black: a clean screen until the first repaint */
    memset(op->bos[0].map, 0, op->bos[0].size);
    memset(op->bos[1].map, 0, op->bos[1].size);

    /* the compositor output (logical == native: scale 1) */
    op->out = xw_output_create(db->base.comp, op->name, layout_x, 0, w, h, 1);
    if (!op->out) {
        bo_destroy(db, &op->bos[0]);
        bo_destroy(db, &op->bos[1]);
        if (op->saved_crtc)
            drmModeFreeCrtc(op->saved_crtc);
        free(op);
        return -1;
    }

    /* program the CRTC: mode + first buffer on this connector */
    if (drmModeSetCrtc(db->drm_fd, crtc_id, op->bos[0].fb_id, 0, 0,
                       &op->conn_id, 1, &op->mode) < 0) {
        xw_log(XW_LOG_ERROR,
               "drm: cannot set mode %dx%d@%u on %s: %s", w, h,
               op->mode.vrefresh, op->name, strerror(errno));
        bo_destroy(db, &op->bos[0]);
        bo_destroy(db, &op->bos[1]);
        if (op->saved_crtc)
            drmModeFreeCrtc(op->saved_crtc);
        xw_output_destroy(op->out);
        free(op);
        return -1;
    }
    wl_list_insert(db->outputs.prev, &op->link);
    xw_log(XW_LOG_INFO, "drm: output %s: %dx%d@%u (crtc %u)", op->name, w, h,
           op->mode.vrefresh, crtc_id);
    return w;
}

struct xw_backend *xw_backend_drm_create(struct xw_compositor *c,
                                         const struct xw_compositor_config *cfg) {
    struct xw_seat_session *seat = c->seat;

    struct drm_backend *db = calloc(1, sizeof(*db));
    if (!db)
        return NULL;
    db->base.comp = c;
    db->base.name = "drm";
    db->base.ops = &drm_ops;
    db->seat = seat;
    db->drm_fd = -1;
    db->seat_dev_id = -1;
    wl_list_init(&db->outputs);
    if (seat)
        xw_seat_session_set_events(seat,
                           &(struct xw_seat_events){
                               .disable = db_session_disable,
                               .enable = db_session_enable,
                           },
                           db);
    if (cfg->outputs && cfg->n_outputs > 0)
        xw_log(XW_LOG_INFO,
               "drm: -o output specs are ignored on real hardware; the "
               "connector's preferred mode is used");

    /* ---- 1. device discovery: enumerate /dev/dri/cardN (no hardcoded
     * card0); open through the seat provider ---- */
    DIR *d = opendir("/dev/dri");
    if (!d) {
        xw_log(XW_LOG_ERROR,
               "drm: no DRM subsystem (/dev/dri does not exist) — this "
               "machine has no KMS display hardware, or the device nodes "
               "are not exposed to this session");
        free(db);
        return NULL;
    }
    char cards[DRM_MAX_OUTPUTS][32];
    int n_cards = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n_cards < DRM_MAX_OUTPUTS) {
        if (strncmp(de->d_name, "card", 4) == 0 && de->d_name[4] >= '0' &&
            de->d_name[4] <= '9' &&
            strlen(de->d_name) < sizeof(cards[0]) - 10)
            snprintf(cards[n_cards++], sizeof(cards[0]), "/dev/dri/%.20s",
                     de->d_name);
    }
    closedir(d);
    if (n_cards == 0) {
        xw_log(XW_LOG_ERROR,
               "drm: no /dev/dri/card* devices found — no KMS display "
               "hardware available");
        free(db);
        return NULL;
    }

    /* open cards, count connected connectors, prefer a card that
     * actually has a monitor attached */
    int best_fd = -1, best_dev = -1, best_nconn = -1;
    char best_path[64] = {0};
    for (int i = 0; i < n_cards; i++) {
        int fd = -1, dev = -1;
        if (seat) {
            dev = xw_seat_session_open_device(seat, cards[i], &fd);
            if (dev < 0) {
                xw_log(XW_LOG_WARN,
                       "drm: %s: the seat provider (%s) cannot open it: %s"
                       " (possible causes: the user is not permitted to "
                       "access the seat, another compositor owns the "
                       "session, or the seat manager did not expose the "
                       "device)",
                       cards[i], xw_seat_session_desc(seat), strerror(errno));
                continue;
            }
        } else {
            fd = open(cards[i], O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                xw_log(XW_LOG_WARN, "drm: cannot open %s: %s", cards[i],
                       strerror(errno));
                continue;
            }
        }
        int nconn = 0;
        drmModeResPtr res = drmModeGetResources(fd);
        if (res) {
            for (int k = 0; k < res->count_connectors; k++) {
                drmModeConnectorPtr conn =
                    drmModeGetConnector(fd, res->connectors[k]);
                if (conn) {
                    nconn += conn->connection == DRM_MODE_CONNECTED;
                    drmModeFreeConnector(conn);
                }
            }
            drmModeFreeResources(res);
        } else {
            xw_log(XW_LOG_WARN, "drm: %s: not a KMS-capable device: %s",
                   cards[i], strerror(errno));
        }
        if (nconn > best_nconn) {
            if (best_fd >= 0) {
                if (seat)
                    xw_seat_session_close_device(seat, best_dev);
                else
                    close(best_fd);
            }
            best_fd = fd;
            best_dev = dev;
            best_nconn = nconn;
            snprintf(best_path, sizeof(best_path), "%.48s", cards[i]);
        } else {
            if (seat)
                xw_seat_session_close_device(seat, dev);
            else
                close(fd);
        }
    }
    if (best_fd < 0) {
        xw_log(XW_LOG_ERROR,
               "drm: no usable DRM device could be opened (see the "
               "per-device reasons above)");
        free(db);
        return NULL;
    }
    db->drm_fd = best_fd;
    db->seat_dev_id = best_dev;
    snprintf(db->dev_path, sizeof(db->dev_path), "%s", best_path);
    db->active = seat ? xw_seat_session_active(seat) : true;
    xw_log(XW_LOG_INFO, "drm: device %s through %s", best_path,
           seat ? xw_seat_session_desc(seat) : "direct open");

    /* kernel driver identification — "nvidia" predicts the flip-event
     * trouble the watchdog below covers; knowing the driver in the log
     * turns a mystery into a lookup */
    drmVersionPtr drv_ver = drmGetVersion(db->drm_fd);
    if (drv_ver) {
        if (drv_ver->name)
            snprintf(db->driver, sizeof(db->driver), "%.*s",
                     (int)sizeof(db->driver) - 1, drv_ver->name);
        xw_log(XW_LOG_INFO, "drm: kernel driver: %s %d.%d.%d%s",
               db->driver[0] ? db->driver : "?", drv_ver->version_major,
               drv_ver->version_minor, drv_ver->version_patchlevel,
               strncmp(db->driver, "nvidia", 6) == 0
                   ? " (NVIDIA proprietary: page-flip events are not "
                     "delivered on the legacy path; the flip watchdog "
                     "covers it)"
                   : "");
        drmFreeVersion(drv_ver);
    }

    /* ---- 2. DRM master (needed to modeset) ---- */
    if (!drmIsMaster(db->drm_fd)) {
        if (drmSetMaster(db->drm_fd) < 0) {
            int e = errno;
            drmVersionPtr ver = drmGetVersion(db->drm_fd);
            xw_log(XW_LOG_ERROR,
                   "drm: opened %s but cannot become DRM master: %s\n"
                   "  (driver: %s)\n"
                   "  possible causes:\n"
                   "    - another compositor (X server, another wayland\n"
                   "      session) currently owns the display\n"
                   "    - the seat manager did not grant this session\n"
                   "      master rights",
                   best_path, strerror(e),
                   ver && ver->name ? ver->name : "unknown");
            if (ver)
                drmFreeVersion(ver);
            goto fail;
        }
    }
    db->master = drmIsMaster(db->drm_fd);

    /* ---- 3. connectors + modes + crtcs -> outputs ---- */
    drmModeResPtr res = drmModeGetResources(db->drm_fd);
    if (!res) {
        xw_log(XW_LOG_ERROR, "drm: cannot enumerate KMS resources: %s",
               strerror(errno));
        goto fail;
    }
    bool crtc_taken[DRM_MAX_OUTPUTS] = {false};
    int layout_x = 0;
    int n_outputs = 0;
    for (int i = 0;
         i < res->count_connectors && n_outputs < DRM_MAX_OUTPUTS; i++) {
        drmModeConnectorPtr conn =
            drmModeGetConnector(db->drm_fd, res->connectors[i]);
        if (!conn)
            continue;
        if (conn->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(conn);
            continue;
        }
        if (conn->count_modes == 0) {
            char nm[32];
            xw_drm_connector_name(conn->connector_type,
                                  conn->connector_type_id, nm, sizeof(nm));
            xw_log(XW_LOG_ERROR,
                   "drm: connector %s is connected but reports no modes "
                   "(missing EDID?) — skipping it", nm);
            drmModeFreeConnector(conn);
            continue;
        }

        /* CRTC planning: reuse the firmware's encoder/crtc pairing when
         * present, else any free CRTC the connector's encoder can drive */
        int crtc_idx = -1;
        uint32_t crtc_id = 0;
        uint32_t possible = 0;
        int enc_crtc = 0;
        if (conn->encoder_id) {
            drmModeEncoderPtr enc = drmModeGetEncoder(db->drm_fd,
                                                      conn->encoder_id);
            if (enc) {
                enc_crtc = (int)enc->crtc_id;
                possible = enc->possible_crtcs;
                drmModeFreeEncoder(enc);
            }
        } else if (conn->count_encoders > 0) {
            drmModeEncoderPtr enc =
                drmModeGetEncoder(db->drm_fd, conn->encoders[0]);
            if (enc) {
                possible = enc->possible_crtcs;
                drmModeFreeEncoder(enc);
            }
        }
        crtc_idx = xw_drm_plan_crtc((int)conn->encoder_id, enc_crtc,
                                    possible, (const int *)res->crtcs,
                                    res->count_crtcs, crtc_taken);
        if (crtc_idx < 0) {
            char nm[32];
            xw_drm_connector_name(conn->connector_type,
                                  conn->connector_type_id, nm, sizeof(nm));
            xw_log(XW_LOG_ERROR,
                   "drm: no free CRTC for connector %s (all CRTCs are in "
                   "use) — skipping it", nm);
            drmModeFreeConnector(conn);
            continue;
        }
        crtc_id = res->crtcs[crtc_idx];

        int w = add_output(db, conn, crtc_id, layout_x);
        drmModeFreeConnector(conn);
        if (w < 0)
            goto fail;
        layout_x += w;
        n_outputs++;
    }
    if (n_outputs == 0) {
        xw_log(XW_LOG_ERROR,
               "drm: %s opened, but no connector has a connected display "
               "with a usable mode — is a monitor plugged in?",
               best_path);
        drmModeFreeResources(res);
        goto fail;
    }
    drmModeFreeResources(res);

    /* ---- 4. event sources: page flips + hotplug ---- */
    db->drm_src = wl_event_loop_add_fd(c->loop, db->drm_fd,
                                       WL_EVENT_READABLE, db_on_drm_readable,
                                       db);
    if (!db->drm_src)
        goto fail;

    /* flip watchdog + the startup stats window (both on the same
     * loop; both removed in db_destroy) */
    db->flip_timer = wl_event_loop_add_timer(c->loop, db_flip_watchdog_cb, db);
    if (db->flip_timer)
        wl_event_source_timer_update(db->flip_timer, 100);
    else
        xw_log(XW_LOG_WARN,
               "drm: flip watchdog timer unavailable — a driver that "
               "accepts flips but never delivers vblank events would "
               "freeze the display silently");
    db->stats_src = wl_event_loop_add_timer(c->loop, db_stats_timer_cb, db);
    if (db->stats_src)
        wl_event_source_timer_update(db->stats_src, XW_DRM_STATS_MS);

    db->udev = udev_new();
    if (db->udev) {
        db->mon = udev_monitor_new_from_netlink(db->udev, "udev");
        if (db->mon) {
            udev_monitor_filter_add_match_subsystem_devtype(db->mon, "drm",
                                                            NULL);
            udev_monitor_enable_receiving(db->mon);
            db->udev_src = wl_event_loop_add_fd(
                c->loop, udev_monitor_get_fd(db->mon), WL_EVENT_READABLE,
                db_on_udev_readable, db);
        }
    }
    if (!db->udev_src)
        xw_log(XW_LOG_INFO,
               "drm: udev hotplug monitoring unavailable (%s) — display "
               "changes require a session restart",
               strerror(errno));

    /* first frame */
    struct drm_output_priv *op;
    wl_list_for_each(op, &db->outputs, link)
        xw_output_damage_rect(op->out, op->out->x, op->out->y,
                              op->out->width, op->out->height);
    xw_schedule_repaint(c);

    xw_log(XW_LOG_INFO, "drm: %d output(s) on %s (seat: %s)", n_outputs,
           best_path, seat ? xw_seat_session_desc(seat) : "none");
    return &db->base;

fail:
    xw_log(XW_LOG_ERROR,
           "drm: backend initialization failed; restoring previous "
           "display state");
    db_destroy(&db->base);
    return NULL;
}

#endif /* XW_HAVE_DRM_BACKEND */
