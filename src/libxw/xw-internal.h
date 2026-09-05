/* xw-internal.h — internal structures of libxw. White-box test surface. */
#ifndef XW_INTERNAL_H
#define XW_INTERNAL_H

#include "xw.h"

#include <pixman.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-protocol.h"
#include "xdg-activation-protocol.h"
#include "ext-workspace-protocol.h"
#include "single-pixel-buffer-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "ext-session-lock-protocol.h"
#include "ext-idle-notify-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"
#include "xwayland-shell-protocol.h"
#include "xw-window-control-v1-protocol.h"

#define XW_MAX_WS 12
#define XW_MAX_WINDOWS 512
#define XW_TITLE_MAX 256

struct xw_compositor;

/* ------------------------------------------------------------------ util */
int64_t xw_now_ms(void);
/* Splits a command line into argv (returned as malloc'd array of strdup'd
 * tokens; caller frees tokens+array). NULL on empty input. */
char **xw_command_split(const char *cmdline);
void xw_argv_free(char **argv);
/* Async-ish child spawn: fork + exec via /bin/sh -c. Returns pid or -1.
 * Spawned pids are tracked by the compositor; only tracked children are
 * reaped by the compositor's SIGCHLD source. */
pid_t xw_spawn_command(struct xw_compositor *c, const char *cmdline);
/* Register an externally forked child that the compositor should reap
 * (used by xw_spawn_command after fork). */
void xw_compositor_track_child(struct xw_compositor *c, pid_t pid);

/* ------------------------------------------------------ region16 domain */
/* Region16 boxes are int16 on every axis. The client-facing rect
 * handlers (wl_surface.damage, wl_surface.damage_buffer,
 * wl_region.add/subtract) used to forward protocol ints to pixman
 * verbatim, and pixman has two failure classes for out-of-domain
 * input: NEGATIVE extents make it log "*** BUG *** In
 * pixman_region_union_rect: Invalid rectangle passed" and drop the rect
 * (the 2026-09-06 physical session log carried 11 of these from a real
 * client; geomstorm's damage(0,0,INT32_MAX,INT32_MAX) — x2 truncates
 * into int16 as -1 — produces a saturating ~10 per storm), while
 * coordinates BEYOND the int16 domain wrap silently into garbage
 * extents (x=40000 becomes x=-25536: wrong damage with no diagnostic
 * at all). Damage is a hint, never a wedge: clamp each rect into the
 * representable domain, drop what is empty/inverted or falls outside
 * entirely. Valid rects — including negative origins, which are legal
 * surface-local coordinates — pass through unchanged. Returns false
 * when the rect should be ignored. */
static inline bool xw_region16_rect_clamp(int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0)
        return false;
    /* 64-bit corners: x + w must not overflow int before clamping */
    int64_t x2 = (int64_t)*x + *w, y2 = (int64_t)*y + *h;
    int x1 = *x < INT16_MIN ? INT16_MIN : *x;
    int y1 = *y < INT16_MIN ? INT16_MIN : *y;
    int ex2 = x2 > INT16_MAX ? INT16_MAX : (int)x2;
    int ey2 = y2 > INT16_MAX ? INT16_MAX : (int)y2;
    if (x1 >= ex2 || y1 >= ey2)
        return false; /* entirely beyond the representable domain */
    *x = x1;
    *y = y1;
    *w = ex2 - x1;
    *h = ey2 - y1;
    return true;
}

/* --------------------------------------------------------------- ini file */
struct xw_ini_entry {
    char *key, *value;
    struct wl_list link;
};
struct xw_ini_section {
    char *name;
    struct wl_list entries; /* xw_ini_entry.link */
    struct wl_list link;
};
struct xw_ini {
    struct wl_list sections; /* xw_ini_section.link */
};
struct xw_ini *xw_ini_load(const char *path);
const char *xw_ini_get(const struct xw_ini *ini, const char *section,
                       const char *key);
void xw_ini_free(struct xw_ini *ini);

/* --------------------------------------------------------------- surface */
enum xw_surface_role {
    XW_SURFACE_ROLE_NONE = 0,
    XW_SURFACE_ROLE_XDG_TOPLEVEL,
    XW_SURFACE_ROLE_XDG_POPUP,
    XW_SURFACE_ROLE_LAYER,
    XW_SURFACE_ROLE_SESSION_LOCK,
    XW_SURFACE_ROLE_SUBSURFACE,
    XW_SURFACE_ROLE_XWAYLAND,
};

struct xw_surface {
    struct wl_resource *res;
    struct xw_compositor *comp;
    struct wl_list link; /* compositor.surfaces */

    enum xw_surface_role role;

    /* committed state */
    struct wl_shm_buffer *shm;      /* NULL when no shm buffer attached */
    struct { uint32_t color; } single_pixel;
    bool has_single_pixel;
    int buf_w, buf_h;               /* committed buffer size */
    int scale;                      /* wl_surface.set_buffer_scale */

    /* committed buffer ownership: the wl_buffer resource currently
     * referenced by this surface. Released (wl_buffer.release) when a
     * commit replaces it — clients rotating 2+ buffers stall forever
     * without release events (foot/GTK/XWayland double-buffering).
     * A destroy listener keeps the pointer valid if the client destroys
     * the buffer first. */
    struct wl_resource *committed_buffer;
    struct wl_listener committed_buffer_destroy;

    /* pending state */
    struct wl_resource *pending_buffer;
    bool pending_attach; /* attach seen since the last commit (sticky buffer) */
    pixman_region16_t pending_damage;

    pixman_region16_t input;        /* surface-local; empty = whole surface */
    bool input_set;

    /* this surface is a wl_pointer cursor image (set via set_cursor) */
    bool is_cursor;

    struct wl_list frames;          /* xw_frame.link */

    struct wl_resource *xdg_surface_res; /* role objects */
    void *role_data;                /* xw_window / xw_popup / xw_layer_surface /
                                       xw_subsurface */

    /* subsurface children (role != SUBSURFACE keeps this empty) */
    struct wl_list subsurfaces;     /* xw_subsurface.parent_link */

    bool mapped;                    /* role-specific map state */
    bool pending_config;            /* configure sent, awaiting ack+commit */
    uint32_t pending_serial;
};

struct wl_resource *xw_surface_create(struct wl_client *client,
                                      struct xw_compositor *c, uint32_t id,
                                      uint32_t version);
void xw_surface_resource_destroyed(struct wl_resource *res);
void xw_damage_outputs_rect(struct xw_compositor *c, int x, int y, int w, int h);

struct xw_frame {
    struct wl_resource *res;
    struct wl_list link;
};

/* role surface geometry in global (output layout) coordinates */
void xw_surface_get_pos(struct xw_surface *s, int *x, int *y, int *w, int *h);
pixman_image_t *xw_surface_get_image(struct xw_surface *s);
/* true if point (global coords) is inside the surface input region */
bool xw_surface_has_input_at(struct xw_surface *s, int x, int y);
/* canonical global -> surface-local (BUFFER-relative, geometry offset
 * included) — the ONE translation for hit-test AND event delivery */
void xw_surface_to_local(struct xw_surface *s, int gx, int gy, int *lx,
                         int *ly);
/* the wl_surface (buffer) origin in global coords (to_local's inverse;
 *    popup anchoring) */
void xw_surface_buffer_pos(struct xw_surface *s, int *x, int *y);
/* role dispatch (implemented by the respective shell files) */
void xw_role_commit(struct xw_surface *s);
void xw_role_destroy(struct xw_surface *s);
void xw_role_unmap(struct xw_surface *s);

/* ------------------------------------------------------------ output */
struct xw_output;
void xw_output_bind(struct wl_client *client, void *data, uint32_t version,
                    uint32_t id);

struct xw_output {
    struct xw_compositor *comp;
    struct wl_global *global;
    struct wl_list resources; /* wl_resource link for wl_output clients */
    struct wl_list link;      /* compositor.outputs */

    char name[64];
    int x, y;                /* position in layout */
    int width, height;       /* logical size */
    int scale;

    pixman_image_t *logical; /* a8r8g8b8, width x height */
    pixman_image_t *native;  /* a8r8g8b8, width*scale x height*scale */
    uint32_t *native_data;
    pixman_region16_t damage;      /* logical coords */
    int64_t last_frame_ms;

    /* usable area, reduced by layer-shell exclusive zones */
    struct { int x, y, w, h; } usable;
};

void xw_output_set_usable(struct xw_output *o, int x, int y, int w, int h);
void xw_output_damage_rect(struct xw_output *o, int x, int y, int w, int h);
void xw_output_repaint(struct xw_output *o);
/* Shared output lifecycle (used by every backend). Creates the pixman
 * backbuffers, the wl_output global and links it into the layout at
 * (x, y). Fails only on allocation. */
struct xw_output *xw_output_create(struct xw_compositor *c, const char *name,
                                   int x, int y, int w, int h, int scale);
void xw_output_destroy(struct xw_output *o);
/* Resize in place: reallocates the backbuffers, re-announces geometry +
 * mode + done to bound clients, relatches maximized/fullscreen windows
 * and damages everything. No-op when the size is unchanged. */
void xw_output_resize(struct xw_output *o, int w, int h);

/* --------------------------------------------------------------- backend */
/* Backend vtable. `present` is called from the repaint path (after the
 * frame is composited into the output's native buffer) and is where
 * backends hand the pixels to real display hardware or a parent
 * compositor. All ops are optional (NULL = not applicable). */
struct xw_backend;
struct xw_output;

struct xw_backend_ops {
    void (*present)(struct xw_backend *b, struct xw_output *o);
    void (*destroy)(struct xw_backend *b);
};

struct xw_backend {
    struct xw_compositor *comp;
    const char *name;
    const struct xw_backend_ops *ops;
};

struct xw_backend *xw_backend_headless_create(struct xw_compositor *c,
                                              const struct xw_compositor_config *cfg);
struct xw_backend *xw_backend_nested_create(struct xw_compositor *c,
                                             const struct xw_compositor_config *cfg);
struct xw_backend *xw_backend_x11_create(struct xw_compositor *c,
                                          const struct xw_compositor_config *cfg);
#ifdef XW_HAVE_DRM_BACKEND
struct xw_backend *xw_backend_drm_create(struct xw_compositor *c,
                                         const struct xw_compositor_config *cfg);
#endif

/* DRM planning helpers (xw-backend-drm.c, DRM-independent section):
 * pure functions over plain structs, testable without hardware. */
struct xw_drm_mode {
    int hdisplay, vdisplay;
    int vrefresh;
    uint32_t type;
};
const struct xw_drm_mode *xw_drm_pick_mode(const struct xw_drm_mode *modes,
                                           int count);
const char *xw_drm_connector_type_name(uint32_t drm_connector_type);
void xw_drm_connector_name(uint32_t drm_connector_type, uint32_t type_id,
                           char *buf, size_t len);
int xw_drm_plan_crtc(int conn_enc, int enc_crtc, uint32_t enc_possible,
                     const int *crtc_ids, int n_crtcs, bool *crtc_taken);
/* generic teardown (ops->destroy) */
void xw_backend_destroy(struct xw_backend *b);
/* destroys all outputs of the compositor (shared by backend destroys) */
void xw_backend_destroy_outputs(struct xw_compositor *c);

/* ------------------------------------------------------- seat providers */
/* Seat/session acquisition for backends that own real hardware
 * (xw-session-seat.c). Provider-agnostic: libseat (external, optional),
 * a built-in seatd wire-protocol client, or a direct VT session.
 * Nested/headless backends do not create a seat. */
struct xw_seat_session;

struct xw_seat_events {
    /* session is becoming inactive: release scanout resources (DRM
     * master, pending page flips) NOW, then xw_seat_session_ack_disable() */
    void (*disable)(void *ud);
    /* session is active again: re-acquire and repaint */
    void (*enable)(void *ud);
};

struct xw_seat_impl {
    int (*open_device)(struct xw_seat_session *s, const char *path,
                       int *fd_out); /* returns device id >= 0 */
    int (*close_device)(struct xw_seat_session *s, int device_id);
    int (*switch_vt)(struct xw_seat_session *s, int vt);
    int (*ack_disable)(struct xw_seat_session *s);
    void (*destroy)(struct xw_seat_session *s);
};

struct xw_seat_session *xw_seat_session_open(struct xw_compositor *c, int provider,
                                     const char *seat_name);
void xw_seat_session_set_events(struct xw_seat_session *s,
                        const struct xw_seat_events *ev, void *ud);
int xw_seat_session_open_device(struct xw_seat_session *s, const char *path,
                        int *fd_out);
int xw_seat_session_close_device(struct xw_seat_session *s, int device_id);
int xw_seat_session_switch_vt(struct xw_seat_session *s, int vt);
int xw_seat_session_ack_disable(struct xw_seat_session *s);
void xw_seat_session_destroy(struct xw_seat_session *s);
const char *xw_seat_session_name(const struct xw_seat_session *s);
const char *xw_seat_session_desc(const struct xw_seat_session *s);
bool xw_seat_session_active(const struct xw_seat_session *s);

/* X11 synthetic key-repeat filter (libxw/xw-backend-x11.c; exposed for
 * white-box tests — no X server needed to exercise the logic).
 * With XKB detectable auto-repeat, the X server re-sends KeyPress for
 * held keys; clients already repeat via wl_keyboard.repeat_info, so
 * those synthetic presses must be dropped: a press of an already-down
 * key is a repeat. pressed_words is an 8-word bitmap of linux
 * keycodes; returns whether the event should be forwarded. */
bool xw_x11_key_filter(uint32_t *pressed_words, uint32_t linux_keycode,
                       bool down);

/* --------------------------------------------------------- real input */
/* libinput-backed input source (xw-input-libinput.c). Build-time
 * optional: the module is compiled only when libinput was found
 * (XW_LIBINPUT); callers guard with XW_HAVE_LIBINPUT. */
struct xw_input_libinput;
struct xw_input_libinput *xw_input_libinput_create(struct xw_compositor *c);
void xw_input_libinput_destroy(struct xw_input_libinput *in);
/* session lifecycle: stop/start reading devices while the session is
 * inactive (VT switched away) — libinput_suspend drops the devices,
 * libinput_resume re-opens them through the seat provider. */
void xw_input_libinput_suspend(struct xw_input_libinput *in);
int xw_input_libinput_resume(struct xw_input_libinput *in);

/* Event handlers: the real translation pipeline used by the libinput
 * event loop, callable directly (white-box tests, no hardware).
 * Coordinates are output-layout logical pixels; keycodes/buttons are
 * raw linux input codes. */
void xw_input_handle_key(struct xw_input_libinput *in, uint32_t linux_keycode,
                         bool down);
void xw_input_handle_pointer_rel(struct xw_input_libinput *in, double dx,
                                 double dy);
void xw_input_handle_pointer_abs(struct xw_input_libinput *in, double nx,
                                 double ny);
void xw_input_handle_button(struct xw_input_libinput *in,
                            uint32_t linux_button, bool down);
void xw_input_handle_axis(struct xw_input_libinput *in, uint32_t axis,
                          double value, bool continuous);

/* Explicit input-acquisition failure report: the structured diagnostic
 * printed when the compositor acquired no keyboard AND no pointer
 * through the active seat provider (real TTY sessions). Pure formatter
 * over plain arguments so tests can drive every branch without
 * hardware. fail_path may be NULL; fail_errno 0 = no recorded error. */
void xw_input_log_acquisition_failure(const char *backend_name,
                                      const char *seat_provider,
                                      const char *seat_name,
                                      bool session_active, int nodes_present,
                                      int devices_acquired, int keyboards,
                                      int pointers, const char *fail_path,
                                      int fail_errno);

/* ------------------------------------------------------------------- seat */
struct xw_window;

struct xw_seat {
    struct xw_compositor *comp;
    struct wl_global *global;
    char name[32];
    uint32_t capabilities;

    struct wl_list resources;   /* wl_seat client resources */
    struct wl_list pointers;    /* wl_pointer resources */
    struct wl_list keyboards;   /* wl_keyboard resources */
    struct wl_list data_devices; /* wl_data_device resources */
    struct wl_list link;

    /* xkb state */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    xkb_mod_mask_t ignore_mask; /* caps lock + num lock */
    xkb_mod_index_t mod_shift, mod_ctrl, mod_alt, mod_super;
    char *keymap_area;
    size_t keymap_len;
    int keymap_fd;               /* memfd holding the serialized keymap */

    uint32_t serial;            /* input event serial */
    int32_t cursor_x, cursor_y;

    /* client cursor (wl_pointer.set_cursor): the surface carrying the
     * cursor image + hotspot. NULL → default arrow. Rendered by the
     * software cursor path at (cursor_x - hot_x, cursor_y - hot_y). */
    struct xw_surface *cursor_surface;
    int cursor_hot_x, cursor_hot_y;
    /* cursor state machine: the serial of the pointer enter most
     * recently delivered to the current focus (set_cursor requests
     * are validated against it and against the seat's issued-serial
     * ceiling) and the serial of the last accepted set_cursor */
    uint32_t ptr_enter_serial;
    uint32_t cursor_serial;

    /* key repeat (see xw.h struct xw_compositor_config): advertised to
     * clients via wl_keyboard.repeat_info; the server-side timer
     * repeats only keys consumed by interactive keyboard move/resize
     * (client-visible keys are never server-repeated — clients repeat
     * themselves per the Wayland protocol) */
    int repeat_delay_ms, repeat_period_ms, repeat_rate_hz;
    struct wl_event_source *repeat_src;
    uint32_t repeat_key;        /* linux keycode currently repeating */
    bool repeat_active;

    /* keyboard focus */
    struct xw_surface *kb_focus;
    /* keys consumed by the shortcut engine (suppress release to client) */
    uint32_t consumed_keys[8];  /* bitmap of linux keycodes */
    /* modifier state as last sent to the client */
    uint32_t sent_depressed, sent_latched, sent_locked, sent_group;

    /* pointer focus */
    struct xw_surface *ptr_focus;

    /* selection (wl_data_device) */
    struct wl_resource *selection_source; /* wl_data_source owned by client */
    struct wl_client *selection_client;

    /* drag and drop */
    struct {
        struct wl_resource *source;
        struct wl_client *source_client;
        struct xw_surface *origin;
        bool active;
    } drag;

    struct wl_resource *ptr_grab;  /* wl_pointer client resource that holds
                                      an implicit grab (popup/drag/move) */
    bool ptr_grab_is_drag;
    struct xw_surface *grab_surface; /* surface receiving grab events */

    /* last input activity (ext-idle-notify; updated by every key,
     * pointer and axis event, injected input included) */
    int64_t last_activity_ms;

    /* instrumentation: session start + first wl_pointer.enter actually
     * delivered to a client (default-level log lines answer "how far
     * did pointer input get" from one run) */
    int64_t started_ms;
    int64_t first_enter_ms;
};

struct xw_seat *xw_seat_create(struct xw_compositor *c, const char *name);
void xw_seat_destroy(struct xw_seat *s);

/* first seat of the compositor (defined in xw-wm.c) */
struct xw_seat *xw_seat_first(struct xw_compositor *c);

void xw_seat_key(struct xw_seat *s, uint32_t keycode, bool down);
void xw_seat_pointer_motion(struct xw_seat *s, int x, int y);
void xw_seat_pointer_button(struct xw_seat *s, uint32_t linux_button, bool down);
void xw_seat_pointer_axis(struct xw_seat *s, uint32_t axis, double value);

/* focus management (called by wm) */
void xw_seat_set_kb_focus(struct xw_seat *s, struct xw_surface *surface);
/* pointer focus follows a popup grab (see xw-seat.c) */
void xw_seat_popup_ptr_focus(struct xw_seat *s, struct xw_surface *popup);

/* re-run the pointer hit-test at the current cursor position for every
 * seat (surface stack changed without motion: map/unmap/destroy) */
void xw_seat_repointer(struct xw_compositor *c);

/* drop every seat reference to a dying surface BEFORE it is freed
 * (ptr_focus/grab/kb-focus/drag origin); called by the surface destroy
 * path — leaving any of them set is a use-after-free */
void xw_seat_forget_surface(struct xw_compositor *c, struct xw_surface *s);
/* damage the current cursor image's extent on every output (motion,
 * cursor swaps, cursor surface teardown) */
void xw_seat_damage_cursor(struct xw_compositor *c);

/* ------------------------------------------------------------ wm/window */
struct xw_rect { int x, y, w, h; };

struct xw_window {
    struct xw_compositor *comp;
    struct wl_client *client;
    uint32_t id;

    struct wl_resource *xdg_surface_res;
    struct wl_resource *toplevel_res;
    struct xw_surface *surface;

    bool mapped;
    bool first_commit_done;
    char title[XW_TITLE_MAX];
    char app_id[XW_TITLE_MAX];

    /* canonical geometry, global coords */
    int x, y, w, h;

    /* xdg state flags as last configured+acked */
    bool maximized, fullscreen, minimized;
    bool activated;
    bool resizing;

    /* restore geometry for unmaximize/unfullscreen */
    struct xw_rect restore;
    /* geometry saved while tiled */
    struct xw_rect untiled;
    int tiled; /* 0 = none, else bitmask L=1 R=2 T=4 B=8 */

    int ws; /* workspace index; -1 = sticky/all */

    struct xw_output *output;

    struct wl_list link;       /* wm.windows (management order) */
    struct wl_list stack_link; /* wm.stack — top at list head */
    struct wl_list wsi_handles; /* xw_wsi_res.link (workspace annotation) */

    /* last configure sent */
    uint32_t last_serial;
    uint32_t acked_serial;
    bool have_config;
    bool need_reconfigure;

    /* xdg_toplevel size hints (0 = unset) */
    int min_w, min_h, max_w, max_h;
    /* xdg_surface.set_window_geometry override (content bounds within the
     * buffer); -1 = not set */
    int geo_x, geo_y, geo_w, geo_h;
    bool geometry_set;

    /* interactive move/resize (pointer or keyboard driven) */
    struct {
        int mode;   /* 0 none, 1 move, 2 resize */
        int edges;  /* resize edge bitmask */
        int grab_dx, grab_dy;
        int start_w, start_h, start_x, start_y;
        int snap;   /* snap candidate bitmask (same as tiled bits) */
    } inter;

    struct wl_list toplevel_handles; /* xw_foreign_toplevel_res.link */

    /* xwayland_surface_v1 serial: correlates this window with the X11
     * window inside Xwayland (both sides receive the same value); the
     * session's WM helper uses it to mirror geometry and deliver closes */
    uint64_t xw_serial;
    bool xw_has_serial;

    /* X-side truth pushed through xw_window_control_v1 (v2):
     * override-redirect windows are popup-class (X-owned geometry,
     * excluded from taskbar/focus/Alt+Tab; live in wm->or_windows);
     * the increments are WM_NORMAL_HINTS resize steps (0 = unset).
     * min/max reuse the xdg hint fields below. */
    bool xw_override_redirect;
    int xw_inc_w, xw_inc_h;

    /* v3: an EWMH fullscreen request that arrived before the window
     * mapped (the helper reads _NET_WM_STATE at serial association,
     * which can precede the first buffer commit). Applied right after
     * the map-time placement so the saved restore geometry is the
     * placed one, exactly as if the request had arrived one moment
     * later. 0 = none, 1 = enter, 2 = leave. */
    uint8_t xw_fs_pending;

    /* a geometry grant we pushed to the X side (notify_geometry) that
     * the X window has not echoed back yet (set_geometry / a
     * matching commit). While set, buffer commits carrying the OLD
     * size are in-flight stale state from before the grant and must
     * NOT be adopted into the model — the granted-vs-committed
     * distinction: "compositor granted 360x240" is not "client
     * committed 360x240". The unfullscreen+resize race that rewound
     * live resizes lived exactly here. */
    bool xw_geom_pending;
};

enum { XW_EDGE_L = 1, XW_EDGE_R = 2, XW_EDGE_T = 4, XW_EDGE_B = 8 };

struct xw_wm {
    struct xw_compositor *comp;
    struct wl_list windows; /* all windows incl. unmapped (xw_window.link) */
    struct wl_list stack;   /* stacking order, top first (stack_link) */
    struct wl_list or_windows; /* override-redirect X11 windows (link):
                                 popup-class, never focus/taskbar */

    int ws_count, ws_current;
    char ws_names[XW_MAX_WS][24];

    struct xw_window *focused;

    int config_border;      /* reserved for SSD (not drawn yet) */
    int snap_threshold;     /* px from edge to trigger snapping */

    /* layer-shell stacking lists, indexed by zwlr_layer (background=0,
     * bottom=1, top=2, overlay=3). Head = topmost. */
    struct wl_list layers[4];

    /* rules loaded from rules.conf */
    struct xw_ini *rules;

    /* show-desktop toggle state */
    bool sd_active;
    int sd_count;
    struct xw_window *sd_windows[XW_MAX_WINDOWS];
};

struct xw_wm *xw_wm_create(struct xw_compositor *c, const char *config_dir);
void xw_wm_destroy(struct xw_wm *wm);

void xw_wm_manage_toplevel(struct xw_wm *wm, struct xw_window *w);
void xw_wm_unmanage(struct xw_wm *wm, struct xw_window *w, bool resources_gone);
void xw_wm_window_map(struct xw_wm *wm, struct xw_window *w);
void xw_wm_or_map(struct xw_wm *wm, struct xw_window *w);
void xw_wm_or_reclassify(struct xw_wm *wm, struct xw_window *w);
void xw_wm_window_unmap(struct xw_wm *wm, struct xw_window *w);

void xw_wm_focus_window(struct xw_wm *wm, struct xw_window *w, bool activate);
void xw_wm_raise(struct xw_wm *wm, struct xw_window *w);
void xw_wm_restack_focus(struct xw_wm *wm);

void xw_wm_switch_workspace(struct xw_wm *wm, int idx);
void xw_wm_window_to_workspace(struct xw_wm *wm, struct xw_window *w, int idx);
void xw_wm_show_desktop(struct xw_wm *wm);

void xw_wm_maximize(struct xw_wm *wm, struct xw_window *w, bool on);
void xw_wm_fullscreen(struct xw_wm *wm, struct xw_window *w, bool on);
void xw_wm_minimize(struct xw_wm *wm, struct xw_window *w, bool on);
void xw_wm_close(struct xw_wm *wm, struct xw_window *w);
void xw_wm_tile(struct xw_wm *wm, struct xw_window *w, int edges);
void xw_wm_center(struct xw_wm *wm, struct xw_window *w);

void xw_wm_update_window_output(struct xw_wm *wm, struct xw_window *w);
/* canonical geometry instrumentation (stderr, gated by XW_GEOMETRY_TRACE):
 * one line with every coordinate space of a window; the pick trace shows
 * what the wm's hit-test resolves for a global point */
void xw_wm_trace_geometry(const struct xw_window *w, const char *tag);
void xw_wm_trace_pick(struct xw_wm *wm, int px, int py);
/* visible = mapped && !minimized && (sticky or on current workspace) */
bool xw_wm_window_visible(struct xw_wm *wm, struct xw_window *w);
struct xw_window *xw_wm_window_at(struct xw_wm *wm, int x, int y,
                                  struct xw_surface **surface_out);
/* Alt+Tab style MRU cycling among visible windows of the current ws */
void xw_wm_cycle(struct xw_wm *wm, bool forward);

/* interactive move/resize (driven by pointer or keyboard through the seat) */
bool xw_wm_interactive_begin_move(struct xw_wm *wm, struct xw_window *w, int px,
                                  int py);
bool xw_wm_interactive_begin_resize(struct xw_wm *wm, struct xw_window *w,
                                    int edges, int px, int py);
void xw_wm_interactive_motion(struct xw_wm *wm, struct xw_window *w, int px,
                              int py);
void xw_wm_interactive_end(struct xw_wm *wm, struct xw_window *w);
/* keyboard move/resize: returns true if the key was used */
bool xw_wm_interactive_key(struct xw_wm *wm, struct xw_window *w, uint32_t code,
                           bool down);
/* the window currently in interactive move/resize, NULL if none */
struct xw_window *xw_wm_interactive_window(struct xw_wm *wm);

void xw_wm_recalculate_usable(struct xw_wm *wm);
void xw_wm_damage_all(struct xw_wm *wm);
void xw_wm_damage_window(struct xw_wm *wm, struct xw_window *w);

/* ------------------------------------------------------------- shortcuts */
struct xw_shortcut {
    int action;          /* enum xw_action */
    char *arg;           /* optional argument (workspace number, command) */
    char *binding_str;   /* as written in config, for diagnostics */
    xkb_keysym_t keysym;
    xkb_mod_mask_t mods; /* exact modifier set (shift/ctrl/alt/super) */
    struct wl_list link;
};

struct xw_shortcuts {
    struct xw_compositor *comp;
    struct wl_list bindings;
    struct wl_list commands;   /* xw_shortcut with action RUN_COMMAND */
    int conflicts;             /* detected at load time */
    /* modifier indices (from the first seat's keymap) */
    xkb_mod_index_t mod_shift, mod_ctrl, mod_alt, mod_super;
    xkb_mod_mask_t tracked_mods;
    xkb_mod_mask_t ignore_mask;
};

struct xw_shortcuts *xw_shortcuts_create(struct xw_compositor *c,
                                         const char *config_dir);
void xw_shortcuts_destroy(struct xw_shortcuts *sc);
/* Load XFCE-parity default table. */
void xw_shortcuts_load_defaults(struct xw_shortcuts *sc);
bool xw_shortcuts_load_file(struct xw_shortcuts *sc, const char *path);
/* Try to match and dispatch; returns true if consumed. */
bool xw_shortcuts_dispatch(struct xw_shortcuts *sc, struct xw_seat *seat,
                           uint32_t keycode, bool down);

/* --------------------------------------------------------------- actions */
void xw_actions_init(struct xw_compositor *c);
void xw_actions_dispatch(struct xw_compositor *c, int action, const char *arg);

/* -------------------------------------------------------------- layer shell */
struct xw_layer_surface {
    struct xw_compositor *comp;
    struct xw_surface *surface;
    struct wl_resource *res;      /* zwlr_layer_surface_v1 */
    struct wl_resource *shell_client_res;
    struct xw_output *output;
    enum zwlr_layer_shell_v1_layer layer;
    uint32_t anchors;
    int exclusive_zone;           /* -1 = auto from committed size */
    struct { int top, right, bottom, left; } margin;
    bool keyboard_interactivity;
    int configured_w, configured_h;
    bool configured_sent;       /* initial configure answered a commit */

    int x, y, w, h;               /* current geometry, global coords */
    bool mapped;
    char namespace[32];           /* client-declared identity, for logs */
    struct wl_list link;          /* wm.layers[layer] */
};

void xw_layer_shell_init(struct xw_compositor *c);
void xw_layer_shell_fin(struct xw_compositor *c);
void xw_layer_surface_destroy(struct xw_layer_surface *ls);
/* role hooks called from the role dispatcher in xw-xdg-shell.c */
void xw_layer_role_commit(struct xw_surface *s);
void xw_layer_role_unmap(struct xw_surface *s);
void xw_layer_role_destroy(struct xw_surface *s);

/* session-lock role hooks (xw-session-lock.c) */
void xw_lock_role_commit(struct xw_surface *s);
void xw_lock_role_destroy(struct xw_surface *s);
/* output geometry changed: relayout + reconfigure its layer surfaces
 * (anchored surfaces must learn the new output size, per layer-shell
 * semantics) and refresh the usable area. Called by xw_output_resize. */
void xw_layer_reconfigure_output(struct xw_compositor *c, struct xw_output *o);

/* an output appeared: adopt output-less layer surfaces and configure
 * them (they were held unconfigured since creation) */
void xw_layer_output_added(struct xw_compositor *c, struct xw_output *o);

/* an output is being destroyed: re-anchor its layer surfaces to the
 * next output, or send .closed when none remains */
void xw_layer_output_removed(struct xw_compositor *c, struct xw_output *o);

/* --------------------------------------------------- foreign toplevel mgmt */
struct xw_foreign_toplevel_res {
    struct wl_resource *res;
    struct xw_compositor *comp;
    struct wl_list link;  /* window.toplevel_handles */
    struct wl_list mgr_link;
};

void xw_foreign_toplevel_init(struct xw_compositor *c);
void xw_foreign_toplevel_fin(struct xw_compositor *c);
void xw_foreign_toplevel_window_mapped(struct xw_compositor *c, struct xw_window *w);
void xw_foreign_toplevel_window_unmapped(struct xw_compositor *c, struct xw_window *w);
void xw_foreign_toplevel_notify(struct xw_compositor *c, struct xw_window *w);

/* ------------------------------------------------------------ ext-workspace */
void xw_ext_workspace_init(struct xw_compositor *c);
void xw_ext_workspace_fin(struct xw_compositor *c);
void xw_ext_workspace_changed(struct xw_compositor *c);

/* ------------------------------------------------- xw workspace annotation */
/* xw-workspace-info-v1: per-toplevel workspace events for panels and
 * pagers (the wlr foreign toplevel protocol carries no workspace). */
struct xw_wsi_res {
    struct wl_resource *res; /* xw_workspace_toplevel_v1 */
    struct xw_window *w;     /* NULL once the window is gone */
    struct wl_list link;     /* window.wsi_handles */
};
void xw_workspace_info_init(struct xw_compositor *c);
void xw_workspace_info_fin(struct xw_compositor *c);
/* push w->ws (+done) to every annotation of the window */
void xw_workspace_info_notify(struct xw_compositor *c, struct xw_window *w);
/* the window is being destroyed: detach annotations (client proxies
 * are released when their toplevel handles close) */
void xw_workspace_info_window_gone(struct xw_compositor *c, struct xw_window *w);

/* ------------------------------------------------------------- activation */
void xw_activation_init(struct xw_compositor *c);
void xw_activation_fin(struct xw_compositor *c);

/* ------------------------------------------------------------ data device */
void xw_data_device_init(struct xw_compositor *c);
void xw_data_device_fin(struct xw_compositor *c);
void xw_data_device_notify_focus(struct xw_compositor *c, struct xw_seat *seat);
void xw_data_device_send_selection(struct xw_compositor *c, struct xw_seat *seat,
                                   struct wl_client *client);
/* drag lifecycle, called from the seat */
void xw_data_device_drag_motion(struct xw_compositor *c, struct xw_seat *seat,
                                int x, int y);
void xw_data_device_drag_drop(struct xw_compositor *c, struct xw_seat *seat);
void xw_data_device_drag_cancel(struct xw_compositor *c, struct xw_seat *seat);

/* --------------------------------------------------------- xdg shell glue */
void xw_xdg_shell_init(struct xw_compositor *c);
void xw_xdg_shell_fin(struct xw_compositor *c);
/* (Re)send xdg_toplevel.configure with the window's target state. */
void xw_xdg_send_configure(struct xw_window *w);

/* popups */
struct xw_popup {
    struct xw_compositor *comp;
    struct xw_surface *surface;
    struct wl_resource *res;       /* xdg_popup */
    struct wl_resource *xdg_surface_res;
    struct xw_surface *parent;     /* may be NULL (unparented) */
    void *pos;                     /* struct xw_positioner (copy) */
    int anchor_x, anchor_y;        /* computed position (global coords) */
    int w, h;
    bool mapped;
    bool grabbed;
    bool done_sent; /* popup_done is once-per-lifetime (xdg-shell) */
    struct wl_list link;
};

void xw_popup_reposition(struct xw_popup *p);
void xw_popup_dismiss(struct xw_popup *p);

/* -------------------------------------------------------- subcompositor */
/* wl_subcompositor / wl_subsurface. Subsurfaces are wl_surface children
 * of a parent wl_surface (window/popup/layer or another subsurface),
 * positioned at (x, y) relative to the parent, stacked above or below
 * the parent's own buffer by place_above/place_below. synced subsurfaces
 * apply their state at the parent's next commit (we approximate the
 * spec's atomicity by gating DAMAGE on the parent commit); desynced ones
 * apply immediately. */
struct xw_subsurface {
    struct xw_compositor *comp;
    struct xw_surface *surface;   /* the child surface (role SUBSURFACE) */
    struct xw_surface *parent;    /* owner of the children list */
    struct wl_resource *res;      /* wl_subsurface */
    int x, y;                     /* committed position, parent-relative */
    int pending_x, pending_y;
    bool synced;                  /* default per spec */
    bool below_parent;            /* stacked under the parent's buffer */
    bool has_pending;             /* child committed while synced */
    struct wl_list parent_link;   /* parent->subsurfaces (order = stacking,
                                     tail = topmost) */
    struct wl_list link;          /* comp->subsurfaces (global registry) */
};

void xw_subcompositor_init(struct xw_compositor *c);
void xw_subcompositor_fin(struct xw_compositor *c);
/* the parent committed: apply+damage synced children */
void xw_subsurface_parent_committed(struct xw_surface *parent);
/* the parent is being destroyed: unrole+damage all children */
void xw_subsurface_parent_destroyed(struct xw_surface *parent);
/* the seat needs to forget a dying cursor/subsurface reference */
/* XW_INPUT_TRACE=1 instrument (xw-seat.c): pointer/cursor/focus
 * event lines on stderr for physical debugging */
void xw_input_trace(const char *fmt, ...);
/* the never-blocking diagnostic sink (xw-util.c): every line-level
 * diagnostic (trace instruments, default log sink) goes through it.
 * prefix + printf body + '\n', ONE non-blocking write; when stderr
 * stalls (pipe nobody drains, dead consumer) lines are dropped and
 * counted instead of blocking the event loop — the observational
 * contract: XW_INPUT_TRACE / XW_GEOMETRY_TRACE must never change
 * compositor, signal-dispatch, or shutdown behavior. */
void xw_diag_line(const char *prefix, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void xw_diag_vline(const char *prefix, const char *fmt, va_list ap);

void xw_seat_forget_cursor_surface(struct xw_compositor *c,
                                   struct xw_surface *s);
/* role dispatch (xw-subcompositor.c, called from xw-surface.c) */
void xw_subsurface_role_commit(struct xw_surface *s);
void xw_subsurface_role_destroy(struct xw_surface *s);
void xw_subsurface_get_pos(struct xw_surface *s, int *x, int *y, int *w,
                           int *h);
/* hit-test: topmost subsurface of parent at (gx,gy), else NULL */
struct xw_surface *xw_subsurface_at(struct xw_surface *parent, int gx, int gy);

/* ---------------------------------------------------- xwayland shell */
/* xwayland_shell_v1: Xwayland (24+) requires this protocol for rootless
 * windows — each X11 toplevel arrives as a wl_surface carrying the
 * xwayland_surface role. The role behaves like a toplevel WITHOUT the
 * xdg configure/ack dance: commit-with-buffer maps the window, a null
 * commit or surface destroy unmaps it. set_serial is recorded for
 * diagnostics only (the X-side correlation lives inside Xwayland).
 * Windows created here are ordinary xw_windows: the SAME focus, raise,
 * move, resize, workspace, taskbar (foreign-toplevel/workspace-info)
 * and activation model as native toplevels — one window-management
 * path, no parallel X11 special case. */
void xw_xwayland_shell_init(struct xw_compositor *c);
void xw_xwayland_shell_fin(struct xw_compositor *c);
/* role dispatch (xw-xwayland-shell.c) */
void xw_xwayland_role_commit(struct xw_surface *s);
void xw_xwayland_role_destroy(struct xw_surface *s);
/* taskbar/panel "close" on an Xwayland window: forwards to the session
 * WM helper over xw_window_control_v1 (returns false when impossible) */
bool xw_xwayland_window_close(struct xw_window *w);
/* compositor geometry changed for an Xwayland window: mirror it to the
 * WM helper (no-op for other roles / no serial yet) */
void xw_xwayland_notify_geometry(struct xw_window *w);
/* push the compositor keyboard focus to the WM helper (X input focus
 * routing). Called from the seat's focus funnel; NULL/native surface =
 * release the X focus. */
void xw_xwayland_notify_focus(struct xw_compositor *c,
                               struct xw_surface *focus_surface);

/* ------------------------------------------------------------ compositor */

/* Children spawned by the compositor itself (xw_spawn_command). Only these
 * are reaped by the compositor's SIGCHLD source: children forked by the
 * embedding process (session manager, test harness, panel plugins) belong
 * to their spawner, and stealing their exit status breaks waitpid-based
 * supervision. */
#define XW_MAX_CHILDREN 16

struct xw_compositor {
    struct wl_display *display;
    struct wl_event_loop *loop;
    char socket_path[108];
    bool running;
    int exit_code;

    struct wl_event_source *sigint_src, *sigterm_src, *sigchld_src,
        *sighup_src;
    pid_t children[XW_MAX_CHILDREN];
    int n_children;

    struct xw_backend *backend;

    /* seat/session provider for hardware-owning backends (DRM); NULL
     * for nested/headless. Created before the backend, destroyed after
     * it: the backend's teardown releases devices through it. */
    struct xw_seat_session *seat;

    /* real-input source (libinput; NULL when absent or disabled) */
    struct xw_input_libinput *input;

    struct wl_list outputs;   /* xw_output.link */
    struct wl_list surfaces;  /* xw_surface.link */
    struct wl_list seats;     /* xw_seat.link */
    struct wl_list popups;    /* xw_popup.link */
    struct wl_list subcomps;  /* xw_subsurface.link (all live subsurfaces) */

    struct wl_global *g_compositor, *g_subcompositor, *g_seat, *g_shm;
    struct wl_global *g_data_device_manager;
    struct wl_global *g_xdg_wm_base;
    struct wl_global *g_xwayland_shell;
    struct wl_global *g_window_control;
    struct wl_global *g_single_pixel;
    /* sub-initializers register their own globals */

    struct xw_wm *wm;
    struct xw_shortcuts *shortcuts;

    struct xw_compositor_config conf;
    char *conf_dir_owned;

    struct {
        bool (*hook)(int action, const char *arg, void *ud);
        void *ud;
    } action;

    uint32_t bg_color; /* wallpaper fill (a8r8g8b8) */

    /* module state (structs defined in the respective modules; kept as
     * void* here so the structs stay private). Per-compositor: several
     * compositors can live in one process (nested backend tests, the
     * session manager embedding), so no file-static module state. */
    void *layer_shell_state;   /* struct xw_layer_shell */
    void *session_lock_state;  /* struct xw_session_lock (xw-session-lock.c) */
    void *idle_state;          /* struct xw_idle (xw-idle.c) */
    void *ws_state;            /* ext-workspace manager state */
    void *activation_state;    /* struct xw_activation */
    struct wl_global *ddm_global; /* wl_data_device_manager global */
    struct wl_list ddm_sources;   /* xw_data_source.link */

    struct wl_list ft_managers;      /* foreign toplevel managers */
    struct wl_list wsi_managers;      /* xw workspace annotation managers */
    struct wl_list ws_managers;      /* ext workspace managers */
    struct wl_list activation_tokens; /* xw_activation_token.link */
    struct wl_list wc_managers;       /* xw_wc_manager (window control) */
    struct wl_list xw_pending_idents; /* xw_pending_ident: helper identity
                                         racing the set_serial arrival */
    uint64_t xw_focus_serial;        /* last X focus serial pushed to the
                                        WM helper (dedupe; focus(0) when no
                                        X window is focused) */
    bool xw_focus_serial_set;

    /* resolved commands for spawn-based actions (actions.conf) */
    char cmd_terminal[256], cmd_appfinder[256], cmd_exit[256], cmd_lock[256];
    char cmd_screenshot[256], cmd_vol_up[256], cmd_vol_down[256];
    char cmd_vol_mute[256], cmd_media[256];

    struct wl_event_source *repaint_idle;
    bool repaint_scheduled;

    uint32_t next_window_id;
};

void xw_schedule_repaint(struct xw_compositor *c);
void xw_render_output(struct xw_output *o);
/* crash-time state summary: clients/windows/focused — printed by the
 * fatal-signal diagnostics path in the compositor binary */
void xw_compositor_dump_state(struct xw_compositor *c);
/* straight (non-premultiplied) ARGB8888 rect fill */
void xw_render_fill_rect(pixman_image_t *dst, pixman_op_t op, uint32_t color,
                         int x, int y, int w, int h);

/* protocol errors */
enum {
    XW_ERR_ROLE = 1,          /* wl_surface already has a role */
    XW_ERR_SURFACE_STATE,
    XW_ERR_XDG_STATE,
};

/* ---------------------------------------------------- session lock (xw-session-lock.c) */
/* ext-session-lock-v1 server. While a lock is engaged (from the lock()
 * request until unlock_and_destroy, or indefinitely after the lock
 * client dies) the compositor stops rendering and delivering input to
 * normal clients: only lock surfaces render and receive input, all
 * other content is blanked. See xw-session-lock.c for the state
 * machine and the security invariants. */
void xw_session_lock_init(struct xw_compositor *c);
void xw_session_lock_fin(struct xw_compositor *c);
/* the security gate is engaged (lock pending, locked, or lock client
 * died while locked). Renders/seat code must consult this. */
bool xw_session_lock_active(struct xw_compositor *c);
/* the `locked` event was actually sent (or the lock client died after
 * locking): the session is really locked, not merely engaging.
 * White-box test surface. */
bool xw_session_lock_locked(struct xw_compositor *c);
/* render the lock layer for an output (blank + lock surfaces + nothing
 * else). Called by xw_render_output when the gate is engaged. */
void xw_session_lock_render(struct xw_output *o);
/* lock surface keyboard-focus owner (topmost mapped lock surface), for
 * the focus override in xw_seat_set_kb_focus */
struct xw_surface *xw_session_lock_kb_owner(struct xw_compositor *c);
/* pointer hit-testing while engaged: lock surfaces only, by output
 * coverage (they always cover their whole output) */
struct xw_surface *xw_session_lock_surface_at(struct xw_compositor *c,
                                              int x, int y);
/* called after each repaint cycle presented all damaged outputs: sends
 * the pending ext_session_lock_v1.locked event once a locked frame has
 * actually been presented. */
void xw_session_lock_after_present(struct xw_compositor *c);
/* output lifecycle: re-send configure on resize (lock surfaces must
 * track the exact output size); a removed output's lock surfaces are
 * destroyed server-side; a new output is blanked until the lock client
 * covers it. */
void xw_session_lock_reconfigure_output(struct xw_compositor *c,
                                         struct xw_output *o);
void xw_session_lock_output_removed(struct xw_compositor *c,
                                     struct xw_output *o);

/* ------------------------------------------------------ idle (xw-idle.c) */
/* ext-idle-notify-v1 server. Seat activity timestamps live in the seat
 * (last_activity_ms); every input entry point must call
 * xw_idle_activity() to keep them current. */
void xw_idle_init(struct xw_compositor *c);
void xw_idle_fin(struct xw_compositor *c);
/* input arrived on this seat: update the timestamp and resume any
 * notifications that had gone idle. Cheap; call from every entry. */
void xw_idle_activity(struct xw_seat *s);
/* the seat is going away: destroy its notifications (no events sent). */
void xw_idle_seat_destroyed(struct xw_compositor *c, struct xw_seat *s);

#endif /* XW_INTERNAL_H */
