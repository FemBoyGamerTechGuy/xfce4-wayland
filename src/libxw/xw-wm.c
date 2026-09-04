/* xw-wm.c — window management: workspaces, focus, stacking, states,
 * interactive move/resize with edge snapping, tiling, window rules.
 *
 * xfwm4-parity behaviors implemented here:
 *   click-to-focus + raise; focus-on-activation; Alt+Tab MRU cycling;
 *   edge snapping while moving (halves, corners, top=maximize);
 *   keyboard move/resize (Alt+F7 / Alt+F8, arrows, Shift = fine, Esc);
 *   workspace switching with wrap-around; show-desktop toggle;
 *   window rules matched at map time.
 */
#include "xw-internal.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* linux keycodes we use for interactive move/resize (input-event-codes) */
#define KEY_ESC_ 1
#define KEY_ENTER_ 28
#define KEY_UP_ 103
#define KEY_DOWN_ 108
#define KEY_LEFT_ 105
#define KEY_RIGHT_ 106

struct xw_seat *xw_seat_first(struct xw_compositor *c) {
    struct xw_seat *s;
    wl_list_for_each(s, &c->seats, link)
        return s;
    return NULL;
}

/* first visible window in MRU order, excluding a given window */
static struct xw_window *next_visible(struct xw_wm *wm, struct xw_window *exclude) {
    struct xw_window *w;
    wl_list_for_each(w, &wm->stack, stack_link) {
        if (w != exclude && xw_wm_window_visible(wm, w))
            return w;
    }
    return NULL;
}

/* ---------------------------------------------------------------- create */

struct xw_wm *xw_wm_create(struct xw_compositor *c, const char *config_dir) {
    struct xw_wm *wm = calloc(1, sizeof(*wm));
    if (!wm)
        return NULL;
    wm->comp = c;
    wl_list_init(&wm->windows);
    wl_list_init(&wm->stack);
    wl_list_init(&wm->or_windows);
    for (int i = 0; i < 4; i++)
        wl_list_init(&wm->layers[i]);
    wm->ws_count = 4;
    wm->ws_current = 0;
    wm->snap_threshold = 8;
    for (int i = 0; i < XW_MAX_WS; i++)
        snprintf(wm->ws_names[i], sizeof(wm->ws_names[i]), "Workspace %d", i + 1);

    if (config_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/workspaces.conf", config_dir);
        struct xw_ini *ini = xw_ini_load(path);
        if (ini) {
            const char *v = xw_ini_get(ini, "workspaces", "count");
            if (v) {
                int n = atoi(v);
                if (n >= 1 && n <= XW_MAX_WS)
                    wm->ws_count = n;
            }
            for (int i = 0; i < XW_MAX_WS; i++) {
                char key[32];
                snprintf(key, sizeof(key), "name_%d", i + 1);
                const char *name = xw_ini_get(ini, "workspaces", key);
                if (name && *name)
                    snprintf(wm->ws_names[i], sizeof(wm->ws_names[i]), "%s", name);
            }
            xw_ini_free(ini);
        }
        snprintf(path, sizeof(path), "%s/rules.conf", config_dir);
        wm->rules = xw_ini_load(path); /* NULL is fine: no rules */
    }
    return wm;
}

void xw_wm_destroy(struct xw_wm *wm) {
    if (!wm)
        return;
    xw_ini_free(wm->rules);
    free(wm);
}

/* ------------------------------------------------------------- damage */

void xw_wm_damage_window(struct xw_wm *wm, struct xw_window *w) {
    xw_damage_outputs_rect(wm->comp, w->x, w->y, w->w, w->h);
}

void xw_wm_damage_all(struct xw_wm *wm) {
    struct xw_output *o;
    wl_list_for_each(o, &wm->comp->outputs, link)
        xw_output_damage_rect(o, o->x, o->y, o->width, o->height);
}

/* ------------------------------------------------------------ lifecycle */

void xw_wm_manage_toplevel(struct xw_wm *wm, struct xw_window *w) {
    wl_list_insert(wm->windows.prev, &w->link);
    wl_list_insert(wm->stack.prev, &w->stack_link); /* bottom of stack */
    w->id = ++wm->comp->next_window_id;
    xw_log(XW_LOG_DEBUG, "wm: window %u managed (surface %u, app '%s')",
           w->id, w->surface ? wl_resource_get_id(w->surface->res) : 0,
           w->app_id);
}

void xw_wm_window_map(struct xw_wm *wm, struct xw_window *w) {
    if (w->mapped)
        return;
    if (w->xw_override_redirect) {
        /* popup-class X11 windows never enter the managed flow (the
         * role commit routes them to xw_wm_or_map; this guard is the
         * safety net for a late OR conversion racing the commit) */
        xw_wm_or_map(wm, w);
        return;
    }
    w->mapped = true;

    struct xw_output *o = w->output;
    if (!o && !wl_list_empty(&wm->comp->outputs)) {
        o = wl_container_of(wm->comp->outputs.next, o, link);
        w->output = o;
    }

    /* ---- window rules (rules.conf, applied at map time, in order) */
    bool no_focus = false;
    if (wm->rules) {
        struct xw_ini_section *sec;
        wl_list_for_each(sec, &wm->rules->sections, link) {
            if (strncmp(sec->name, "rule", 4) != 0)
                continue;
            const char *app_id = xw_ini_get(wm->rules, sec->name, "match-app-id");
            const char *title = xw_ini_get(wm->rules, sec->name, "match-title");
            bool match = true;
            if (app_id)
                match = match && fnmatch(app_id, w->app_id, 0) == 0;
            if (match && title)
                match = match && fnmatch(title, w->title, 0) == 0;
            if (!match)
                continue;
            const char *v;
            if ((v = xw_ini_get(wm->rules, sec->name, "workspace"))) {
                int ws = atoi(v) - 1;
                if (ws >= 0 && ws < wm->ws_count)
                    w->ws = ws;
            }
            if ((v = xw_ini_get(wm->rules, sec->name, "maximize")) &&
                strcmp(v, "true") == 0)
                w->maximized = true;
            if ((v = xw_ini_get(wm->rules, sec->name, "fullscreen")) &&
                strcmp(v, "true") == 0)
                w->fullscreen = true;
            if ((v = xw_ini_get(wm->rules, sec->name, "sticky")) &&
                strcmp(v, "true") == 0)
                w->ws = -1;
            if ((v = xw_ini_get(wm->rules, sec->name, "no-focus")) &&
                strcmp(v, "true") == 0)
                no_focus = true;
        }
    }

    /* ---- placement (cascade inside the usable area, xfwm-style) */
    static int cascade = 0;
    if (o && !w->maximized && !w->fullscreen) {
        int uw = o->usable.w, uh = o->usable.h;
        int off = (cascade % 8) * 24;
        cascade++;
        w->x = o->usable.x + off + (uw - w->w) / 2;
        w->y = o->usable.y + off + (uh - w->h) / 3;
        if (w->x + w->w > o->usable.x + uw)
            w->x = o->usable.x + (uw - w->w) / 2;
        if (w->y + w->h > o->usable.y + uh)
            w->y = o->usable.y + (uh - w->h) / 2;
    }

    xw_wm_raise(wm, w);
    if (!no_focus && xw_wm_window_visible(wm, w)) {
        xw_wm_focus_window(wm, w, true);
    } else {
        xw_xdg_send_configure(w);
    }
    xw_foreign_toplevel_window_mapped(wm->comp, w);
    xw_foreign_toplevel_notify(wm->comp, w);
    xw_wm_damage_window(wm, w);
    /* a window mapped under the stationary cursor takes pointer focus
     * immediately (motion would do it too, but not before then) */
    xw_seat_repointer(wm->comp);
    xw_log(XW_LOG_INFO,
           "wm: window %u MAPPED (surface %u, app '%s', title '%s', "
           "%dx%d+%d+%d, output '%s', ws %d)",
           w->id, w->surface ? wl_resource_get_id(w->surface->res) : 0,
           w->app_id, w->title, w->w, w->h, w->x, w->y,
           w->output ? w->output->name : "(unset)", w->ws);
}

/* classify an UNMAPPED xwayland window as override-redirect NOW: popups
 * must leave the managed set the moment the WM helper says so — the
 * client may never draw (no buffer commit → no or_map), and a ghost
 * popup sitting in wm->windows would surface in taskbar/Alt+Tab
 * iterations. wl_list_remove works from whichever list holds the
 * link; stack_link was initialized empty at manage. */
void xw_wm_or_reclassify(struct xw_wm *wm, struct xw_window *w) {
    if (w->xw_override_redirect && w->mapped)
        return; /* already an active popup: or_map owns it */
    wl_list_remove(&w->link);
    wl_list_remove(&w->stack_link);
    wl_list_init(&w->stack_link);
    wl_list_insert(wm->or_windows.prev, &w->link);
    xw_log(XW_LOG_DEBUG,
           "wm: window %u reclassified override-redirect (pre-commit)", w->id);
}

/* override-redirect (popup-class) X11 window mapped: X owns the
 * geometry, no rules, no cascade placement, no taskbar presence, no
 * focus — render above every managed window. */
void xw_wm_or_map(struct xw_wm *wm, struct xw_window *w) {
    if (w->mapped)
        return;
    w->mapped = true;
    struct xw_output *o = w->output;
    if (!o && !wl_list_empty(&wm->comp->outputs)) {
        o = wl_container_of(wm->comp->outputs.next, o, link);
        w->output = o;
    }
    wl_list_remove(&w->link);
    wl_list_remove(&w->stack_link);
    wl_list_init(&w->stack_link);
    wl_list_insert(wm->or_windows.prev, &w->link);
    xw_wm_damage_window(wm, w);
    /* a menu that opens under the stationary cursor must receive the
     * pointer immediately (hover before any motion) */
    xw_seat_repointer(wm->comp);
    xw_log(XW_LOG_INFO,
           "wm: X11 popup (OR) window %u MAPPED (surface %u, %dx%d+%d+%d)",
           w->id, w->surface ? wl_resource_get_id(w->surface->res) : 0,
           w->w, w->h, w->x, w->y);
}

void xw_wm_window_unmap(struct xw_wm *wm, struct xw_window *w) {
    if (!w->mapped)
        return;
    if (w->xw_override_redirect) {
        xw_log(XW_LOG_INFO, "wm: X11 popup (OR) window %u UNMAPPED", w->id);
        xw_wm_damage_window(wm, w);
        w->mapped = false;
        /* the pointer may now sit over a different surface */
        xw_seat_repointer(wm->comp);
        return;
    }
    xw_log(XW_LOG_INFO, "wm: window %u UNMAPPED (app '%s', title '%s')",
           w->id, w->app_id, w->title);
    xw_wm_damage_window(wm, w);
    w->mapped = false;
    xw_foreign_toplevel_window_unmapped(wm->comp, w);
    if (wm->focused == w) {
        wm->focused = NULL;
        struct xw_seat *seat = xw_seat_first(wm->comp);
        if (seat)
            xw_seat_set_kb_focus(seat, NULL);
        struct xw_window *alt = next_visible(wm, w);
        if (alt)
            xw_wm_focus_window(wm, alt, true);
    }
    /* focus that pointed at the unmapped window must be re-picked;
     * surface_at skips unmapped windows */
    xw_seat_repointer(wm->comp);
}

void xw_wm_unmanage(struct xw_wm *wm, struct xw_window *w, bool resources_gone) {
    (void)resources_gone;
    xw_log(XW_LOG_DEBUG, "wm: window %u unmanaged (app '%s')", w->id,
           w->app_id);
    if (w->mapped)
        xw_wm_window_unmap(wm, w);
    xw_foreign_toplevel_window_unmapped(wm->comp, w);
    wl_list_remove(&w->link);
    wl_list_remove(&w->stack_link);
    struct xw_foreign_toplevel_res *fr, *fr2;
    wl_list_for_each_safe(fr, fr2, &w->toplevel_handles, link) {
        /* closed is sent; the handle is detached from the dying
         * window but NOT destroyed server-side: the handle id lives
         * in the server range (0xff000000+), and libwayland removes
         * those ids outright on wl_resource_destroy — the client's
         * legitimate destructor request after `closed` would then be
         * a fatal "invalid object" error (connection killed). The
         * client owns the final destroy; requests on the detached
         * handle are inert (window_of_handle no longer matches). */
        zwlr_foreign_toplevel_handle_v1_send_closed(fr->res);
        wl_list_remove(&fr->link);
        wl_list_init(&fr->link); /* survives until the client destroys it */
    }
    xw_workspace_info_window_gone(wm->comp, w);
    free(w);
}

/* ------------------------------------------------------------- focus */

void xw_wm_focus_window(struct xw_wm *wm, struct xw_window *w, bool activate) {
    if (w && w->xw_override_redirect)
        return; /* popup-class windows never take keyboard focus */
    if (w && !xw_wm_window_visible(wm, w)) {
        xw_log(XW_LOG_WARN, "wm: refusing to focus invisible window");
        return;
    }
    xw_log(XW_LOG_DEBUG, "wm: focus -> window %u ('%s', app '%s')",
           w ? w->id : 0, w ? w->title : "(none)", w ? w->app_id : "");
    struct xw_window *prev = wm->focused;
    wm->focused = w;
    if (prev && prev != w) {
        prev->activated = false;
        xw_xdg_send_configure(prev);
        xw_foreign_toplevel_notify(wm->comp, prev);
    }
    if (w) {
        w->activated = activate;
        if (w->minimized)
            xw_wm_minimize(wm, w, false);
        xw_wm_raise(wm, w);
        xw_xdg_send_configure(w);
        xw_foreign_toplevel_notify(wm->comp, w);
    }
    struct xw_seat *seat = xw_seat_first(wm->comp);
    if (seat)
        xw_seat_set_kb_focus(seat, w ? w->surface : NULL);
    xw_data_device_notify_focus(wm->comp, seat);
}

void xw_wm_raise(struct xw_wm *wm, struct xw_window *w) {
    wl_list_remove(&w->stack_link);
    wl_list_insert(&wm->stack, &w->stack_link);
    xw_wm_damage_window(wm, w);
}

void xw_wm_restack_focus(struct xw_wm *wm) {
    (void)wm;
}

void xw_wm_cycle(struct xw_wm *wm, bool forward) {
    /* MRU order == stack order (head = most recent). Collect visible. */
    struct xw_window *visible[XW_MAX_WINDOWS];
    int n = 0;
    struct xw_window *w;
    wl_list_for_each(w, &wm->stack, stack_link) {
        if (xw_wm_window_visible(wm, w))
            visible[n++] = w;
    }
    if (n == 0)
        return;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (visible[i] == wm->focused)
            cur = i;
    int next = forward ? (cur + 1) % n : (cur - 1 + n) % n;
    xw_wm_focus_window(wm, visible[next], true);
}

/* --------------------------------------------------------- workspace ops */

void xw_wm_switch_workspace(struct xw_wm *wm, int idx) {
    if (idx < 0 || idx >= wm->ws_count)
        return;
    if (idx == wm->ws_current)
        return;
    xw_log(XW_LOG_INFO, "wm: workspace %d -> %d", wm->ws_current, idx);
    wm->ws_current = idx;
    xw_wm_damage_all(wm);

    /* refocus topmost visible window on the new workspace */
    struct xw_window *top = NULL;
    struct xw_window *w;
    wl_list_for_each(w, &wm->stack, stack_link) {
        if (xw_wm_window_visible(wm, w)) {
            top = w;
            break;
        }
    }
    if (top)
        xw_wm_focus_window(wm, top, true);
    else {
        wm->focused = NULL;
        struct xw_seat *seat = xw_seat_first(wm->comp);
        if (seat)
            xw_seat_set_kb_focus(seat, NULL);
    }
    xw_foreign_toplevel_notify(wm->comp, NULL);
    xw_ext_workspace_changed(wm->comp);
}

void xw_wm_window_to_workspace(struct xw_wm *wm, struct xw_window *w, int idx) {
    if (idx < 0 || idx >= wm->ws_count || w->ws == idx)
        return;
    xw_wm_damage_window(wm, w);
    w->ws = idx;
    xw_workspace_info_notify(wm->comp, w);
    if (idx != wm->ws_current && wm->focused == w) {
        wm->focused = NULL;
        struct xw_seat *seat = xw_seat_first(wm->comp);
        if (seat)
            xw_seat_set_kb_focus(seat, NULL);
        struct xw_window *alt;
        wl_list_for_each(alt, &wm->stack, stack_link) {
            if (alt != w && xw_wm_window_visible(wm, alt)) {
                xw_wm_focus_window(wm, alt, true);
                break;
            }
        }
    }
    xw_ext_workspace_changed(wm->comp);
}

void xw_wm_show_desktop(struct xw_wm *wm) {
    if (!wm->sd_active) {
        wm->sd_count = 0;
        struct xw_window *w;
        wl_list_for_each(w, &wm->stack, stack_link) {
            if (xw_wm_window_visible(wm, w) && wm->sd_count < XW_MAX_WINDOWS)
                wm->sd_windows[wm->sd_count++] = w;
        }
        for (int i = 0; i < wm->sd_count; i++)
            xw_wm_minimize(wm, wm->sd_windows[i], true);
        if (wm->sd_count > 0) {
            wm->sd_active = true;
            wm->focused = NULL;
            struct xw_seat *seat = xw_seat_first(wm->comp);
            if (seat)
                xw_seat_set_kb_focus(seat, NULL);
        }
    } else {
        for (int i = 0; i < wm->sd_count; i++)
            xw_wm_minimize(wm, wm->sd_windows[i], false);
        wm->sd_active = false;
        wm->sd_count = 0;
    }
}

/* ------------------------------------------------------------- state ops */

void xw_wm_maximize(struct xw_wm *wm, struct xw_window *w, bool on) {
    if (w->maximized == on)
        return;
    struct xw_output *o = w->output;
    if (!o)
        return;
    xw_wm_damage_window(wm, w);
    if (on) {
        if (!w->fullscreen)
            w->restore = (struct xw_rect){w->x, w->y, w->w, w->h};
        w->maximized = true;
        w->x = o->usable.x;
        w->y = o->usable.y;
        w->w = o->usable.w;
        w->h = o->usable.h;
        w->tiled = 0;
    } else {
        w->maximized = false;
        w->x = w->restore.x;
        w->y = w->restore.y;
        w->w = w->restore.w;
        w->h = w->restore.h;
    }
    xw_xdg_send_configure(w);
    /* same as fullscreen: X11 windows learn geometry through the
     * window-control channel, not xdg configure */
    if (w->surface && w->surface->role == XW_SURFACE_ROLE_XWAYLAND)
        xw_xwayland_notify_geometry(w);
    xw_foreign_toplevel_notify(wm->comp, w);
    xw_wm_damage_window(wm, w);
}

void xw_wm_fullscreen(struct xw_wm *wm, struct xw_window *w, bool on) {
    if (w->fullscreen == on)
        return;
    struct xw_output *o = w->output;
    if (!o)
        return;
    xw_wm_damage_window(wm, w);
    if (on) {
        if (!w->maximized)
            w->restore = (struct xw_rect){w->x, w->y, w->w, w->h};
        w->fullscreen = true;
        w->x = o->x;
        w->y = o->y;
        w->w = o->width;
        w->h = o->height;
        w->tiled = 0;
    } else {
        w->fullscreen = false;
        w->x = w->restore.x;
        w->y = w->restore.y;
        w->w = w->restore.w;
        w->h = w->restore.h;
    }
    xw_xdg_send_configure(w);
    /* xdg configure is a no-op for XWAYLAND-role windows (they have no
     * configure channel); their "configure" IS the geometry event,
     * which the helper mirrors into an X11 ConfigureWindow. Without
     * this, an EWMH fullscreen request changed the compositor model
     * but the X window kept its old size: half-screen fullscreen. */
    if (w->surface && w->surface->role == XW_SURFACE_ROLE_XWAYLAND)
        xw_xwayland_notify_geometry(w);
    xw_foreign_toplevel_notify(wm->comp, w);
    xw_wm_damage_window(wm, w);
}

void xw_wm_minimize(struct xw_wm *wm, struct xw_window *w, bool on) {
    if (w->minimized == on)
        return;
    xw_wm_damage_window(wm, w);
    w->minimized = on;
    if (on && wm->focused == w) {
        wm->focused = NULL;
        struct xw_seat *seat = xw_seat_first(wm->comp);
        if (seat)
            xw_seat_set_kb_focus(seat, NULL);
        struct xw_window *alt;
        wl_list_for_each(alt, &wm->stack, stack_link) {
            if (alt != w && xw_wm_window_visible(wm, alt)) {
                xw_wm_focus_window(wm, alt, true);
                break;
            }
        }
    } else if (!on) {
        /* restore from minimized: bring to front but keep focus stable */
        xw_wm_raise(wm, w);
    }
    xw_foreign_toplevel_notify(wm->comp, w);
}

void xw_wm_close(struct xw_wm *wm, struct xw_window *w) {
    (void)wm;
    if (w->surface && w->surface->role == XW_SURFACE_ROLE_XWAYLAND) {
        /* no xdg_toplevel to send close to; the X window is asked to go
         * through the surface teardown path instead (see
         * xw-xwayland-shell.c for the honest limits) */
        xw_xwayland_window_close(w);
        return;
    }
    if (w->toplevel_res)
        xdg_toplevel_send_close(w->toplevel_res);
}

void xw_wm_tile(struct xw_wm *wm, struct xw_window *w, int edges) {
    struct xw_output *o = w->output;
    if (!o)
        return;
    if (edges == 0) {
        /* untilde */
        if (w->tiled) {
            xw_wm_damage_window(wm, w);
            w->x = w->untiled.x;
            w->y = w->untiled.y;
            w->w = w->untiled.w;
            w->h = w->untiled.h;
            w->tiled = 0;
            xw_xdg_send_configure(w);
            xw_wm_damage_window(wm, w);
        }
        return;
    }
    if (w->maximized || w->fullscreen)
        return;
    if (!w->tiled)
        w->untiled = (struct xw_rect){w->x, w->y, w->w, w->h};
    xw_wm_damage_window(wm, w);
    w->tiled = edges;
    int ux = o->usable.x, uy = o->usable.y, uw = o->usable.w, uh = o->usable.h;
    w->x = ux;
    w->y = uy;
    w->w = uw;
    w->h = uh;
    if (edges == XW_EDGE_T) { /* tile up = maximize */
        w->tiled = 0;
        xw_wm_maximize(wm, w, true);
        return;
    }
    if (edges & XW_EDGE_L) w->w = uw / 2;
    if (edges & XW_EDGE_R) { w->x = ux + uw / 2; w->w = uw - uw / 2; }
    if (edges & XW_EDGE_T) w->h = uh / 2;
    if (edges & XW_EDGE_B) { w->y = uy + uh / 2; w->h = uh - uh / 2; }
    xw_xdg_send_configure(w);
    xw_wm_damage_window(wm, w);
}

void xw_wm_center(struct xw_wm *wm, struct xw_window *w) {
    struct xw_output *o = w->output;
    if (!o)
        return;
    xw_wm_damage_window(wm, w);
    w->x = o->usable.x + (o->usable.w - w->w) / 2;
    w->y = o->usable.y + (o->usable.h - w->h) / 2;
    xw_wm_damage_window(wm, w);
}

void xw_wm_update_window_output(struct xw_wm *wm, struct xw_window *w) {
    /* pick output containing the largest part of the window */
    struct xw_output *best = NULL;
    int64_t best_area = -1;
    struct xw_output *o;
    wl_list_for_each(o, &wm->comp->outputs, link) {
        int ix0 = w->x > o->x ? w->x : o->x;
        int iy0 = w->y > o->y ? w->y : o->y;
        int ix1 = w->x + w->w < o->x + o->width ? w->x + w->w : o->x + o->width;
        int iy1 = w->y + w->h < o->y + o->height ? w->y + w->h : o->y + o->height;
        if (ix1 > ix0 && iy1 > iy0) {
            int64_t a = (int64_t)(ix1 - ix0) * (iy1 - iy0);
            if (a > best_area) {
                best_area = a;
                best = o;
            }
        }
    }
    w->output = best;
}

/* ------------------------------------------------------------- visibility */

bool xw_wm_window_visible(struct xw_wm *wm, struct xw_window *w) {
    if (!w->mapped || w->minimized)
        return false;
    return w->ws == -1 || w->ws == wm->ws_current;
}

struct xw_window *xw_wm_window_at(struct xw_wm *wm, int x, int y,
                                  struct xw_surface **surface_out) {
    struct xw_window *w;
    /* override-redirect windows sit above all managed windows */
    wl_list_for_each(w, &wm->or_windows, link) {
        if (!w->mapped || !w->surface)
            continue;
        if (xw_surface_has_input_at(w->surface, x, y)) {
            if (surface_out)
                *surface_out = w->surface;
            return w;
        }
    }
    wl_list_for_each(w, &wm->stack, stack_link) {
        if (!xw_wm_window_visible(wm, w) || !w->surface)
            continue;
        if (xw_surface_has_input_at(w->surface, x, y)) {
            if (surface_out)
                *surface_out = w->surface;
            return w;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------- interactive */

bool xw_wm_interactive_begin_move(struct xw_wm *wm, struct xw_window *w, int px,
                                  int py) {
    if (!w || w->fullscreen || w->xw_override_redirect)
        return false;
    if (w->maximized) {
        /* xfwm4: moving a maximized window restores it under the cursor */
        xw_wm_maximize(wm, w, false);
        w->x = px - w->w / 2;
        w->y = py - w->h / 2;
    }
    w->inter.mode = 1;
    w->inter.grab_dx = px - w->x;
    w->inter.grab_dy = py - w->y;
    w->inter.snap = 0;
    return true;
}

bool xw_wm_interactive_begin_resize(struct xw_wm *wm, struct xw_window *w,
                                    int edges, int px, int py) {
    (void)wm;
    (void)px;
    (void)py;
    if (!w || w->fullscreen || w->maximized || w->xw_override_redirect)
        return false;
    if (!edges)
        edges = XW_EDGE_B | XW_EDGE_R;
    w->inter.mode = 2;
    w->inter.edges = edges;
    w->inter.start_w = w->w;
    w->inter.start_h = w->h;
    w->inter.start_x = w->x;
    w->inter.start_y = w->y;
    w->inter.grab_dx = px;
    w->inter.grab_dy = py;
    return true;
}

static int snap_candidate(struct xw_wm *wm, struct xw_output *o, int px, int py) {
    int t = wm->snap_threshold;
    int ux = o->usable.x, uy = o->usable.y, uw = o->usable.w, uh = o->usable.h;
    int snap = 0;
    if (px <= ux + t) snap |= XW_EDGE_L;
    if (px >= ux + uw - t) snap |= XW_EDGE_R;
    if (py <= uy + t) snap |= XW_EDGE_T;
    if (py >= uy + uh - t) snap |= XW_EDGE_B;
    /* bottom edge alone does not snap in xfwm4 */
    if (snap == XW_EDGE_B)
        snap = 0;
    return snap;
}

void xw_wm_interactive_motion(struct xw_wm *wm, struct xw_window *w, int px,
                              int py) {
    if (w->inter.mode == 0)
        return;
    xw_wm_damage_window(wm, w);
    if (w->inter.mode == 1) {
        w->x = px - w->inter.grab_dx;
        w->y = py - w->inter.grab_dy;
        xw_wm_update_window_output(wm, w);
        struct xw_output *o = w->output;
        if (o)
            w->inter.snap = snap_candidate(wm, o, px, py);
    } else if (w->inter.mode == 2) {
        /* size constraints: xdg set_min/max_size and (for Xwayland
         * windows) WM_NORMAL_HINTS pushed over the window-control
         * channel — one clamp for both window families */
        int min_w = w->min_w > 0 ? w->min_w : 50;
        int min_h = w->min_h > 0 ? w->min_h : 50;
        int max_w = w->max_w, max_h = w->max_h; /* 0 = unset */
        int inc_w = w->xw_inc_w > 0 ? w->xw_inc_w : 0;
        int inc_h = w->xw_inc_h > 0 ? w->xw_inc_h : 0;
        if (w->inter.edges & XW_EDGE_R) {
            w->w = px - w->x > min_w ? px - w->x : min_w;
            if (max_w > 0 && w->w > max_w) w->w = max_w;
            if (inc_w > 1) w->w -= w->w % inc_w;
        }
        if (w->inter.edges & XW_EDGE_B) {
            w->h = py - w->y > min_h ? py - w->y : min_h;
            if (max_h > 0 && w->h > max_h) w->h = max_h;
            if (inc_h > 1) w->h -= w->h % inc_h;
        }
        if (w->inter.edges & XW_EDGE_L) {
            int neww = w->inter.start_x + w->inter.start_w - px;
            if (neww < min_w)
                neww = min_w;
            if (max_w > 0 && neww > max_w)
                neww = max_w;
            if (inc_w > 1 && (neww / inc_w) * inc_w >= min_w)
                neww = (neww / inc_w) * inc_w;
            w->x = w->inter.start_x + w->inter.start_w - neww;
            w->w = neww;
        }
        if (w->inter.edges & XW_EDGE_T) {
            int newh = w->inter.start_y + w->inter.start_h - py;
            if (newh < min_h)
                newh = min_h;
            if (max_h > 0 && newh > max_h)
                newh = max_h;
            if (inc_h > 1 && (newh / inc_h) * inc_h >= min_h)
                newh = (newh / inc_h) * inc_h;
            w->y = w->inter.start_y + w->inter.start_h - newh;
            w->h = newh;
        }
        xw_xdg_send_configure(w);
        xw_xwayland_notify_geometry(w);
    }
    xw_wm_damage_window(wm, w);
    xw_xwayland_notify_geometry(w);
}

void xw_wm_interactive_end(struct xw_wm *wm, struct xw_window *w) {
    if (w->inter.mode == 0)
        return;
    if (w->inter.mode == 1 && w->inter.snap) {
        xw_wm_tile(wm, w, w->inter.snap);
    }
    w->inter.mode = 0;
    w->inter.snap = 0;
    w->inter.edges = 0;
    xw_xwayland_notify_geometry(w);
}

struct xw_window *xw_wm_interactive_window(struct xw_wm *wm) {
    struct xw_window *w;
    wl_list_for_each(w, &wm->stack, stack_link) {
        if (w->inter.mode != 0)
            return w;
    }
    return NULL;
}

bool xw_wm_interactive_key(struct xw_wm *wm, struct xw_window *w, uint32_t code,
                           bool down) {
    if (w->inter.mode == 0 || !down)
        return false;
    int step = 1; /* Shift held = fine steps; read from seat modifiers */
    struct xw_seat *seat = xw_seat_first(wm->comp);
    if (seat) {
        xkb_mod_mask_t mods = xkb_state_serialize_mods(seat->xkb_state,
                                                       XKB_STATE_MODS_DEPRESSED);
        if (!(mods & (1u << seat->mod_shift)))
            step = 10;
    }
    switch (code) {
    case KEY_ESC_:
        if (w->inter.mode == 1) {
            w->x = w->inter.start_x;
            w->y = w->inter.start_y;
        } else {
            w->w = w->inter.start_w;
            w->h = w->inter.start_h;
        }
        w->inter.mode = 0;
        xw_wm_damage_window(wm, w);
        return true;
    case KEY_ENTER_:
        xw_wm_interactive_end(wm, w);
        return true;
    case KEY_UP_:
    case KEY_DOWN_:
    case KEY_LEFT_:
    case KEY_RIGHT_: {
        int dx = code == KEY_LEFT_ ? -step : code == KEY_RIGHT_ ? step : 0;
        int dy = code == KEY_UP_ ? -step : code == KEY_DOWN_ ? step : 0;
        xw_wm_damage_window(wm, w);
        if (w->inter.mode == 1) {
            w->x += dx;
            w->y += dy;
        } else {
            if (dx > 0 && (w->inter.edges & XW_EDGE_R))
                w->w += step;
            if (dx < 0 && (w->inter.edges & XW_EDGE_L)) {
                w->x -= step;
                w->w += step;
            }
            if (dy > 0 && (w->inter.edges & XW_EDGE_B))
                w->h += step;
            if (dy < 0 && (w->inter.edges & XW_EDGE_T)) {
                w->y -= step;
                w->h += step;
            }
            xw_xdg_send_configure(w);
        }
        xw_wm_damage_window(wm, w);
        return true;
    }
    default:
        return false;
    }
}

/* ---------------------------------------------------- usable area (layers) */

void xw_wm_recalculate_usable(struct xw_wm *wm) {
    struct xw_output *o;
    wl_list_for_each(o, &wm->comp->outputs, link) {
        o->usable.x = o->x;
        o->usable.y = o->y;
        o->usable.w = o->width;
        o->usable.h = o->height;
        for (int layer = 0; layer < 4; layer++) {
            struct xw_layer_surface *ls;
            wl_list_for_each(ls, &wm->layers[layer], link) {
                if (!ls->mapped || ls->output != o)
                    continue;
                int zone = ls->exclusive_zone;
                if (zone < 0)
                    zone = ls->h; /* top bar default height */
                if (zone == 0)
                    continue;
                if (ls->anchors & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
                    o->usable.y += zone + ls->margin.bottom;
                    o->usable.h -= zone + ls->margin.bottom;
                } else if (ls->anchors & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
                    o->usable.h -= zone + ls->margin.top;
                }
                if (ls->anchors & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
                    o->usable.x += zone + ls->margin.right;
                    o->usable.w -= zone + ls->margin.right;
                } else if (ls->anchors & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
                    o->usable.w -= zone + ls->margin.left;
                }
            }
        }
    }
    /* maximized/fullscreen windows follow the new usable area */
    struct xw_window *w;
    wl_list_for_each(w, &wm->windows, link) {
        if (!w->mapped || !w->output)
            continue;
        if (w->maximized) {
            xw_wm_damage_window(wm, w);
            w->x = w->output->usable.x;
            w->y = w->output->usable.y;
            w->w = w->output->usable.w;
            w->h = w->output->usable.h;
            xw_wm_damage_window(wm, w);
            xw_xdg_send_configure(w);
        } else if (w->fullscreen) {
            xw_wm_damage_window(wm, w);
            w->x = w->output->x;
            w->y = w->output->y;
            w->w = w->output->width;
            w->h = w->output->height;
            xw_wm_damage_window(wm, w);
            xw_xdg_send_configure(w);
        }
    }
    xw_wm_damage_all(wm);
}
