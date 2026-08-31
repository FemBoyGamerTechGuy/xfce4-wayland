/* xw-actions.c — the action bus.
 *
 * Every desktop trigger (keyboard shortcut, panel button, protocol
 * request) funnels through xw_actions_dispatch so behavior is uniform
 * and observable by tests via the action hook.
 *
 * Spawn-based commands (terminal, appfinder, exit dialog, lock,
 * screenshot, media keys) resolve from actions.conf [commands], falling
 * back to built-in defaults that name our own binaries.
 */
#include "xw-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void load_commands(struct xw_compositor *c) {
    if (!c->conf.config_dir)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/actions.conf", c->conf.config_dir);
    struct xw_ini *ini = xw_ini_load(path);
    if (ini) {
        const char *v;
        if ((v = xw_ini_get(ini, "commands", "terminal")))
            snprintf(c->cmd_terminal, sizeof(c->cmd_terminal), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "appfinder")))
            snprintf(c->cmd_appfinder, sizeof(c->cmd_appfinder), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "exit-dialog")))
            snprintf(c->cmd_exit, sizeof(c->cmd_exit), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "lock")))
            snprintf(c->cmd_lock, sizeof(c->cmd_lock), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "screenshot")))
            snprintf(c->cmd_screenshot, sizeof(c->cmd_screenshot), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "volume-up")))
            snprintf(c->cmd_vol_up, sizeof(c->cmd_vol_up), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "volume-down")))
            snprintf(c->cmd_vol_down, sizeof(c->cmd_vol_down), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "volume-mute")))
            snprintf(c->cmd_vol_mute, sizeof(c->cmd_vol_mute), "%s", v);
        if ((v = xw_ini_get(ini, "commands", "media")))
            snprintf(c->cmd_media, sizeof(c->cmd_media), "%s", v);
        xw_ini_free(ini);
    }
}

void xw_actions_init(struct xw_compositor *c) {
    /* defaults name our own binaries; $XW_TERMINAL overrides the terminal */
    const char *t = getenv("XW_TERMINAL");
    snprintf(c->cmd_terminal, sizeof(c->cmd_terminal), "%s",
             t && *t ? t : "x-terminal-emulator");
    snprintf(c->cmd_appfinder, sizeof(c->cmd_appfinder), "xw-appfinder");
    snprintf(c->cmd_exit, sizeof(c->cmd_exit), "xw-exit");
    snprintf(c->cmd_lock, sizeof(c->cmd_lock), "xw-lock");
    snprintf(c->cmd_screenshot, sizeof(c->cmd_screenshot), "xw-screenshot");
    load_commands(c);
}

static void run(struct xw_compositor *c, const char *cmd, const char *extra) {
    if (!cmd || !*cmd) {
        xw_log(XW_LOG_WARN, "actions: no command configured");
        return;
    }
    char line[1024];
    if (extra)
        snprintf(line, sizeof(line), "%s %s", cmd, extra);
    else
        snprintf(line, sizeof(line), "%s", cmd);
    pid_t pid = xw_spawn_command(c, line);
    if (pid > 0)
        xw_log(XW_LOG_INFO, "actions: spawned '%s' (pid %d)", line, (int)pid);
}

static int ws_from_arg(const char *arg, int count) {
    if (!arg)
        return -1;
    int n = atoi(arg);
    if (n < 1 || n > count)
        return -1;
    return n - 1;
}

void xw_actions_dispatch(struct xw_compositor *c, int action, const char *arg) {
    struct xw_wm *wm = c->wm;
    struct xw_window *w = wm ? wm->focused : NULL;

    if (c->action.hook && !c->action.hook(action, arg, c->action.ud))
        return; /* test/monitor hook suppressed the built-in handler */

    switch (action) {
    /* ---------------------------------------------------------- window */
    case XW_ACTION_WINDOW_CLOSE:
        if (w)
            xw_wm_close(wm, w);
        break;
    case XW_ACTION_WINDOW_MINIMIZE:
        if (w)
            xw_wm_minimize(wm, w, true);
        break;
    case XW_ACTION_WINDOW_MAXIMIZE_TOGGLE:
        if (w)
            xw_wm_maximize(wm, w, !w->maximized);
        break;
    case XW_ACTION_WINDOW_FULLSCREEN_TOGGLE:
        if (w)
            xw_wm_fullscreen(wm, w, !w->fullscreen);
        break;
    case XW_ACTION_WINDOW_MOVE: {
        struct xw_seat *seat = xw_seat_first(c);
        if (w && seat)
            xw_wm_interactive_begin_move(wm, w, seat->cursor_x, seat->cursor_y);
        break;
    }
    case XW_ACTION_WINDOW_RESIZE: {
        struct xw_seat *seat = xw_seat_first(c);
        if (w && seat)
            xw_wm_interactive_begin_resize(wm, w, 0, seat->cursor_x,
                                           seat->cursor_y);
        break;
    }
    case XW_ACTION_WINDOW_TILE_LEFT:
        if (w)
            xw_wm_tile(wm, w, XW_EDGE_L);
        break;
    case XW_ACTION_WINDOW_TILE_RIGHT:
        if (w)
            xw_wm_tile(wm, w, XW_EDGE_R);
        break;
    case XW_ACTION_WINDOW_TILE_UP:
        if (w)
            xw_wm_tile(wm, w, XW_EDGE_T);
        break;
    case XW_ACTION_WINDOW_TILE_DOWN:
        if (w)
            xw_wm_tile(wm, w, XW_EDGE_B | XW_EDGE_L | XW_EDGE_R);
        break;
    case XW_ACTION_WINDOW_RAISE:
        if (w)
            xw_wm_raise(wm, w);
        break;
    case XW_ACTION_WINDOW_LOWER:
        if (w) {
            wl_list_remove(&w->stack_link);
            wl_list_insert(wm->stack.prev, &w->stack_link);
            xw_wm_damage_window(wm, w);
        }
        break;
    case XW_ACTION_WINDOW_STICK_TOGGLE:
        if (w) {
            if (w->ws == -1) {
                w->ws = wm->ws_current;
            } else {
                w->ws = -1; /* sticky: visible on all workspaces */
            }
            xw_wm_damage_all(wm);
            xw_ext_workspace_changed(c);
        }
        break;
    case XW_ACTION_CYCLE_WINDOWS:
        xw_wm_cycle(wm, true);
        break;
    case XW_ACTION_CYCLE_WINDOWS_BACK:
        xw_wm_cycle(wm, false);
        break;

    /* ------------------------------------------------------ workspaces */
    case XW_ACTION_WORKSPACE_LEFT:
        xw_wm_switch_workspace(wm, wm->ws_current > 0 ? wm->ws_current - 1
                                                      : wm->ws_count - 1);
        break;
    case XW_ACTION_WORKSPACE_RIGHT:
        xw_wm_switch_workspace(wm, (wm->ws_current + 1) % wm->ws_count);
        break;
    case XW_ACTION_WORKSPACE_N: {
        int idx = ws_from_arg(arg, wm->ws_count);
        if (idx >= 0)
            xw_wm_switch_workspace(wm, idx);
        break;
    }
    case XW_ACTION_MOVE_WINDOW_WORKSPACE_LEFT:
        if (w)
            xw_wm_window_to_workspace(wm, w, w->ws > 0 ? w->ws - 1
                                                        : wm->ws_count - 1);
        break;
    case XW_ACTION_MOVE_WINDOW_WORKSPACE_RIGHT:
        if (w)
            xw_wm_window_to_workspace(wm, w, (w->ws + 1) % wm->ws_count);
        break;
    case XW_ACTION_MOVE_WINDOW_WORKSPACE_N: {
        int idx = ws_from_arg(arg, wm->ws_count);
        if (w && idx >= 0)
            xw_wm_window_to_workspace(wm, w, idx);
        break;
    }
    case XW_ACTION_WORKSPACE_ADD:
        if (wm->ws_count < XW_MAX_WS) {
            wm->ws_count++;
            snprintf(wm->ws_names[wm->ws_count - 1],
                     sizeof(wm->ws_names[0]), "Workspace %d", wm->ws_count);
            xw_log(XW_LOG_INFO, "wm: workspace added (%d total)", wm->ws_count);
            xw_ext_workspace_changed(c);
        }
        break;
    case XW_ACTION_WORKSPACE_DELETE_LAST:
        if (wm->ws_count > 1) {
            wm->ws_count--;
            /* move windows off the removed workspace */
            struct xw_window *w2;
            wl_list_for_each(w2, &wm->windows, link) {
                if (w2->ws == wm->ws_count)
                    w2->ws = wm->ws_count - 1;
            }
            if (wm->ws_current >= wm->ws_count)
                xw_wm_switch_workspace(wm, wm->ws_count - 1);
            xw_log(XW_LOG_INFO, "wm: last workspace removed (%d total)",
                   wm->ws_count);
            xw_ext_workspace_changed(c);
        }
        break;
    case XW_ACTION_SHOW_DESKTOP:
        xw_wm_show_desktop(wm);
        break;

    /* ---------------------------------------------------------- system */
    case XW_ACTION_EXIT_DIALOG:
        run(c, c->cmd_exit, NULL);
        break;
    case XW_ACTION_LOCK:
        run(c, c->cmd_lock, NULL);
        break;
    case XW_ACTION_SCREENSHOT:
        run(c, c->cmd_screenshot, NULL);
        break;
    case XW_ACTION_VOLUME_UP:
        run(c, c->cmd_vol_up, NULL);
        break;
    case XW_ACTION_VOLUME_DOWN:
        run(c, c->cmd_vol_down, NULL);
        break;
    case XW_ACTION_VOLUME_MUTE:
        run(c, c->cmd_vol_mute, NULL);
        break;
    case XW_ACTION_MEDIA_COMMAND:
        run(c, c->cmd_media, arg);
        break;
    case XW_ACTION_RUN_COMMAND:
        run(c, arg, NULL);
        break;
    case XW_ACTION_TERMINAL:
        run(c, c->cmd_terminal, NULL);
        break;
    case XW_ACTION_APPFINDER:
        run(c, c->cmd_appfinder, NULL);
        break;

    default:
        xw_log(XW_LOG_WARN, "actions: unknown action %d", action);
        break;
    }
}
