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
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

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
 * Children are reaped by the compositor's SIGCHLD source. */
pid_t xw_spawn_command(struct xw_compositor *c, const char *cmdline);

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

    /* pending state */
    struct wl_resource *pending_buffer;
    pixman_region16_t pending_damage;

    pixman_region16_t input;        /* surface-local; empty = whole surface */
    bool input_set;

    struct wl_list frames;          /* xw_frame.link */

    struct wl_resource *xdg_surface_res; /* role objects */
    void *role_data;                /* xw_window / xw_popup / xw_layer_surface */

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

/* --------------------------------------------------------------- backend */
struct xw_backend {
    struct xw_compositor *comp;
    const char *name;
};

struct xw_backend *xw_backend_headless_create(struct xw_compositor *c,
                                              const struct xw_compositor_config *cfg);
void xw_backend_headless_destroy(struct xw_backend *b);

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
};

enum { XW_EDGE_L = 1, XW_EDGE_R = 2, XW_EDGE_T = 4, XW_EDGE_B = 8 };

struct xw_wm {
    struct xw_compositor *comp;
    struct wl_list windows; /* all windows incl. unmapped (xw_window.link) */
    struct wl_list stack;   /* stacking order, top first (stack_link) */

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

    int x, y, w, h;               /* current geometry, global coords */
    bool mapped;
    struct wl_list link;          /* wm.layers[layer] */
};

void xw_layer_shell_init(struct xw_compositor *c);
void xw_layer_shell_fin(struct xw_compositor *c);
void xw_layer_surface_destroy(struct xw_layer_surface *ls);
/* role hooks called from the role dispatcher in xw-xdg-shell.c */
void xw_layer_role_commit(struct xw_surface *s);
void xw_layer_role_unmap(struct xw_surface *s);
void xw_layer_role_destroy(struct xw_surface *s);

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
    struct wl_list link;
};

void xw_popup_reposition(struct xw_popup *p);
void xw_popup_dismiss(struct xw_popup *p);

/* ------------------------------------------------------------ compositor */
struct xw_compositor {
    struct wl_display *display;
    struct wl_event_loop *loop;
    char socket_path[108];
    bool running;
    int exit_code;

    struct wl_event_source *sigint_src, *sigterm_src, *sigchld_src;

    struct xw_backend *backend;

    struct wl_list outputs;   /* xw_output.link */
    struct wl_list surfaces;  /* xw_surface.link */
    struct wl_list seats;     /* xw_seat.link */
    struct wl_list popups;    /* xw_popup.link */

    struct wl_global *g_compositor, *g_subcompositor, *g_seat, *g_shm;
    struct wl_global *g_data_device_manager;
    struct wl_global *g_xdg_wm_base;
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

    /* module state (structs defined in the respective modules) */
    struct wl_list ft_managers;      /* foreign toplevel managers */
    struct wl_list ws_managers;      /* ext workspace managers */
    struct wl_list activation_tokens; /* xw_activation_token.link */

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
/* straight (non-premultiplied) ARGB8888 rect fill */
void xw_render_fill_rect(pixman_image_t *dst, pixman_op_t op, uint32_t color,
                         int x, int y, int w, int h);

/* protocol errors */
enum {
    XW_ERR_ROLE = 1,          /* wl_surface already has a role */
    XW_ERR_SURFACE_STATE,
    XW_ERR_XDG_STATE,
};

#endif /* XW_INTERNAL_H */
