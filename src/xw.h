/* xw.h — public API of the xfce4-wayland compositor library (libxw).
 *
 * This is the interface used by the xw-compositor binary, by session
 * tooling, and by the automated test suite. It deliberately exposes a
 * headless-safe, deterministic subset: input injection goes through the
 * backend abstraction, pixel access through the software renderer.
 */
#ifndef XW_H
#define XW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- logging */
enum xw_log_level {
    XW_LOG_DEBUG = 0,
    XW_LOG_INFO,
    XW_LOG_WARN,
    XW_LOG_ERROR,
};

void xw_log_set_level(int level);
void xw_log_set_callback(void (*fn)(int level, const char *msg, void *ud),
                         void *ud);
void xw_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---------------------------------------------------------------- actions */
/* The actions bus: named, testable operations the desktop can trigger
 * (from keyboard shortcuts, panel buttons, or protocol requests). */
enum xw_action {
    XW_ACTION_NONE = 0,
    /* window */
    XW_ACTION_WINDOW_CLOSE,
    XW_ACTION_WINDOW_MINIMIZE,
    XW_ACTION_WINDOW_MAXIMIZE_TOGGLE,
    XW_ACTION_WINDOW_FULLSCREEN_TOGGLE,
    XW_ACTION_WINDOW_MOVE,
    XW_ACTION_WINDOW_RESIZE,
    XW_ACTION_WINDOW_TILE_LEFT,
    XW_ACTION_WINDOW_TILE_RIGHT,
    XW_ACTION_WINDOW_TILE_UP,    /* maximize */
    XW_ACTION_WINDOW_TILE_DOWN,  /* center / unmaximize */
    XW_ACTION_WINDOW_RAISE,
    XW_ACTION_WINDOW_LOWER,
    XW_ACTION_WINDOW_STICK_TOGGLE, /* on all workspaces */
    XW_ACTION_CYCLE_WINDOWS,     /* Alt+Tab */
    XW_ACTION_CYCLE_WINDOWS_BACK,
    /* workspaces */
    XW_ACTION_WORKSPACE_LEFT,
    XW_ACTION_WORKSPACE_RIGHT,
    XW_ACTION_WORKSPACE_N,       /* arg = "1".."12" */
    XW_ACTION_MOVE_WINDOW_WORKSPACE_LEFT,
    XW_ACTION_MOVE_WINDOW_WORKSPACE_RIGHT,
    XW_ACTION_MOVE_WINDOW_WORKSPACE_N,
    XW_ACTION_WORKSPACE_ADD,       /* xfwm4 Alt+Insert */
    XW_ACTION_WORKSPACE_DELETE_LAST, /* xfwm4 Alt+Delete */
    XW_ACTION_SHOW_DESKTOP,
    /* system */
    XW_ACTION_EXIT_DIALOG,
    XW_ACTION_LOCK,
    XW_ACTION_SCREENSHOT,
    /* media (delegated to configured command, may be unset) */
    XW_ACTION_VOLUME_UP,
    XW_ACTION_VOLUME_DOWN,
    XW_ACTION_VOLUME_MUTE,
    XW_ACTION_MEDIA_COMMAND,     /* arg = command line */
    /* generic */
    XW_ACTION_RUN_COMMAND,       /* arg = command line */
    XW_ACTION_TERMINAL,          /* arg = configured terminal command */
    XW_ACTION_APPFINDER,
};

/* ------------------------------------------------------------- compositor */

struct xw_compositor;

struct xw_output_spec {
    const char *name;   /* NULL → auto */
    int width, height;  /* logical size */
    int scale;          /* integer scale, >= 1 */
};

/* Backend selection. headless renders into memory (tests/CI/DRM-less
 * development); nested runs as a window inside a parent Wayland
 * compositor; x11 runs as a window inside an X11/XLibre server. */
enum xw_backend_type {
    XW_BACKEND_HEADLESS = 0,
    XW_BACKEND_NESTED,
    XW_BACKEND_X11,
};

/* Real-input source selection (libinput; build-time optional, see
 * XW_LIBINPUT in BUILDING.md).
 *
 * AUTO (default) never touches system devices unless XW_INPUT_DEVICES
 * explicitly lists evdev nodes (a deliberate opt-in that also works
 * for headless debugging); nested/X11 backends always inherit input
 * from the parent session instead.
 * LIBINPUT always runs the real-input source (udev seat mode, or path
 * mode when XW_INPUT_DEVICES is set).
 * NONE compiles it out of this instance entirely. */
enum xw_input_mode {
    XW_INPUT_AUTO = 0,
    XW_INPUT_LIBINPUT,
    XW_INPUT_NONE,
};

struct xw_compositor_config {
    const char *config_dir;    /* directory with INI config; NULL = defaults */
    const char *socket_name;   /* NULL = automatic */
    const char *seat_name;     /* NULL = "seat0" */
    const char *xkb_rules, *xkb_model, *xkb_layout, *xkb_variant, *xkb_options;
    struct xw_output_spec *outputs;
    int n_outputs;             /* 0 = one 1280x720 headless output */
    int log_level;
    int backend;               /* enum xw_backend_type; 0 = headless */
    int input_mode;            /* enum xw_input_mode; 0 = auto */
    /* parent display for nested backends: a WAYLAND_DISPLAY name for
     * XW_BACKEND_NESTED (NULL = inherit $WAYLAND_DISPLAY) or an X
     * display string for XW_BACKEND_X11 (NULL = inherit $DISPLAY) */
    const char *parent_display;
    /* test hook: while the nested backend performs its blocking initial
     * handshake with the parent, call this to keep the parent (e.g. an
     * in-process compositor) making progress. NULL = none */
    void (*nested_pump)(void *ud);
    void *nested_pump_ud;

    /* key repeat advertised to clients via wl_keyboard.repeat_info
     * (clients implement the repeat themselves per the Wayland
     * protocol; the seat additionally auto-repeats only the keys used
     * by interactive keyboard move/resize). 0 selects the defaults
     * (500 ms delay, 30 Hz rate). Runtime overrides: keyboard.conf
     * [keyboard] repeat_delay_ms / repeat_rate_hz, then
     * $XW_REPEAT_DELAY_MS / $XW_REPEAT_RATE_HZ. */
    int repeat_delay_ms;
    int repeat_rate_hz;
};

struct xw_compositor *xw_compositor_create(const struct xw_compositor_config *cfg);
void xw_compositor_destroy(struct xw_compositor *c);

/* Runs the event loop until xw_compositor_stop() is called (possibly from
 * a signal source or a client request). Returns a process exit code. */
int xw_compositor_run(struct xw_compositor *c);
void xw_compositor_stop(struct xw_compositor *c);

/* One iteration of the event loop (process requests, flush clients,
 * repaint damaged outputs). Used by tests and by embedded tooling. */
void xw_compositor_dispatch(struct xw_compositor *c, int timeout_ms);

const char *xw_compositor_socket_path(const struct xw_compositor *c);

/* Output introspection. */
int xw_compositor_n_outputs(const struct xw_compositor *c);
bool xw_compositor_output_info(const struct xw_compositor *c, int index,
                               int *x, int *y, int *w, int *h, int *scale);

/* Read the current rendered contents of an output (ARGB8888, native
 * resolution = logical * scale). Pointer stays valid until the next
 * repaint. Used by tests and by the screenshot action. */
const uint32_t *xw_compositor_output_pixels(const struct xw_compositor *c,
                                            int index, int *w, int *h);

/* ------------------------------------------------------ input injection ---
 * These route through the backend exactly like real input events would
 * (xkbcommon state machine, shortcut engine, client delivery).
 */
void xw_compositor_inject_key(struct xw_compositor *c, uint32_t linux_keycode,
                              bool down);
void xw_compositor_inject_pointer_motion(struct xw_compositor *c, int x, int y);
void xw_compositor_inject_pointer_button(struct xw_compositor *c,
                                         uint32_t linux_button, bool down);
void xw_compositor_inject_pointer_axis(struct xw_compositor *c,
                                       uint32_t axis, double value);

/* --------------------------------------------------- test/monitor hooks ---
 * Observe every action dispatched by the shortcut engine (and by any
 * other action trigger, e.g. protocol requests). Returning false from
 * the hook suppresses the built-in handler for this action — this is how
 * tests assert dispatch without side effects.
 */
void xw_compositor_set_action_hook(struct xw_compositor *c,
                                    bool (*hook)(int action, const char *arg,
                                                 void *ud),
                                    void *ud);

/* Window manager introspection (white-box test surface; also used to
 * report state in dev tooling). All functions are safe on NULL args. */
int xw_compositor_window_count(const struct xw_compositor *c);
int xw_compositor_workspace_count(const struct xw_compositor *c);
int xw_compositor_workspace_current(const struct xw_compositor *c);
/* Focused window title ("" if none). */
void xw_compositor_focused_title(const struct xw_compositor *c, char *buf,
                                 size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XW_H */
