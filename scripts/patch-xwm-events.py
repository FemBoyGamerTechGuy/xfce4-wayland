#!/usr/bin/env python3
"""Replace the win_forget + xwm_handle_event region of xw-xwm.c with the
new event machinery (CreateNotify/PropertyNotify/ConfigureNotify/mask-aware
ConfigureRequest/EWMH lifecycle). Line-based surgery; verifies markers."""
import sys

PATH = "src/session/xw-xwm.c"
src = open(PATH).read().splitlines(keepends=True)

START = "static void win_forget(uint32_t xid) {"
END_MARK = "/* -------------------------------------------------- Wayland control side */"

si = next(i for i, l in enumerate(src) if l.startswith(START))
ei = next(i for i, l in enumerate(src) if l.startswith(END_MARK))
assert si < ei, "marker order broken"

NEW = r'''static void win_forget(uint32_t xid) {
    for (int i = 0; i < n_wins; i++) {
        if (wins[i].xid == xid) {
            wins[i] = wins[--n_wins];
            return;
        }
    }
}

/* ------------------------------------------------ EWMH client list state */

static void client_list_add(uint32_t xid) {
    for (int i = 0; i < n_client_list; i++)
        if (client_list[i] == xid)
            return;
    if (n_client_list < XWM_MAX_WINDOWS)
        client_list[n_client_list++] = xid;
}

static void client_list_remove(uint32_t xid) {
    for (int i = 0; i < n_client_list; i++) {
        if (client_list[i] == xid) {
            client_list[i] = client_list[--n_client_list];
            return;
        }
    }
}

static void client_list_write(void) {
    if (!atom_net_client_list)
        return;
    x_change_property_32(x_root, atom_net_client_list, atom_window_type,
                         (uint32_t)n_client_list, client_list);
}

/* ------------------------------------------------------ window identity */

/* read everything the compositor wants to know about the window: name,
 * class, input model, size hints, WM protocols */
static void win_read_properties(struct xw_win *w) {
    if (x_read_window_name(w->xid, w->name, sizeof(w->name)))
        w->has_name = true;
    if (x_read_window_class(w->xid, w->klass, sizeof(w->klass)))
        w->has_class = true;
    bool input = true;
    if (x_read_wm_hints(w->xid, &input)) {
        w->has_input_hint = true;
        w->wants_input = input;
    }
    x_read_normal_hints(w);
    w->take_focus = atom_wm_take_focus &&
                    x_window_protocol_has(w->xid, atom_wm_take_focus);
}

/* push identity + hints + OR state to the compositor, if the serial
 * association has landed (the requests are keyed by serial) */
static void win_send_identity(struct xw_win *w) {
    if (!wc || !w || !w->serial)
        return;
    if (w->has_name)
        xw_window_control_manager_v1_set_title(
            wc, (uint32_t)(w->serial >> 32), (uint32_t)w->serial, w->name);
    if (w->has_class)
        xw_window_control_manager_v1_set_app_id(
            wc, (uint32_t)(w->serial >> 32), (uint32_t)w->serial, w->klass);
    xw_window_control_manager_v1_set_size_hints(
        wc, (uint32_t)(w->serial >> 32), (uint32_t)w->serial, w->min_w,
        w->min_h, w->max_w, w->max_h, w->inc_w, w->inc_h);
    if (w->override)
        xw_window_control_manager_v1_set_override_redirect(
            wc, (uint32_t)(w->serial >> 32), (uint32_t)w->serial, w->x,
            w->y, w->w, w->h);
}

/* apply compositor focus to the X side; remember it when the serial
 * association has not landed yet (the focus event can win the race) */
static void focus_apply(uint64_t serial) {
    if (serial == 0) {
        focus_serial_pend = 0;
        x_focus_release();
        return;
    }
    struct xw_win *w = win_find_serial(serial);
    if (!w) {
        focus_serial_pend = serial;
        XWM_LOG("info",
                "focus: serial %llu not yet associated - deferring",
                (unsigned long long)serial);
        return;
    }
    x_focus_window(w);
}

/* close a window exactly like the compositor's taskbar-close path */
static void win_close(struct xw_win *w) {
    if (x_window_supports_wm_delete(w->xid)) {
        XWM_LOG("info", "  delivering WM_DELETE_WINDOW");
        x_send_wm_delete(w->xid);
    } else {
        XWM_LOG("info",
                "  no WM_DELETE_WINDOW support - destroying the window");
        x_destroy_window(w->xid);
    }
}

/* ------------------------------------------------------------- X events */

void xwm_handle_event(const uint8_t *ev) {
    switch (ev[0] & 0x7f) {
    case 16: { /* CreateNotify: learn the window (and its OR flag) */
        uint32_t window = get32(ev, 8);
        struct xw_win *w = win_track(window);
        if (w) {
            w->override = ev[24] != 0;
            w->x = (int16_t)get16(ev, 12);
            w->y = (int16_t)get16(ev, 14);
            w->w = get16(ev, 16);
            w->h = get16(ev, 18);
            if (w->override)
                XWM_LOG("info", "CreateNotify: 0x%x override-redirect "
                        "%dx%d+%d+%d (popup-class)", window, w->w, w->h,
                        w->x, w->y);
        }
        break;
    }
    case 20: { /* MapRequest: parent, window */
        uint32_t parent = get32(ev, 4);
        uint32_t window = get32(ev, 8);
        if (parent != x_root) {
            /* reparented children: let them be */
            x_map_window(window);
            return;
        }
        struct xw_win *w = win_track(window);
        if (!w)
            return;
        w->managed = true;
        w->mapped = true;
        win_read_properties(w);
        XWM_LOG("info", "MapRequest: window 0x%x (name '%s', class '%s', "
                "take-focus=%s input=%s) - mapping",
                window, w->has_name ? w->name : "?",
                w->has_class ? w->klass : "?",
                w->take_focus ? "yes" : "no",
                w->wants_input ? "yes" : "no");
        x_map_window(window);
        x_set_wm_state(window, 1 /* Normal */);
        client_list_add(window);
        client_list_write();
        /* No SetInputFocus here: the compositor owns focus policy (its
         * window-map focus decision arrives on the control channel and
         * this helper mirrors it - one focus model, no X-side guessing). */
        break;
    }
    case 23: { /* ConfigureRequest - grant size, keep compositor position.
         * Wire layout: [23][stack-mode][seq][parent][window][sibling]
         * [value-mask u16 @16][pad2][values from 20, 4 bytes each, in
         * mask-bit order: x,y,w,h,border,sibling,stack-mode]. The old
         * parser read x/y/w/h at FIXED offsets - with a size-only mask
         * (xterm, every toolkit) those bytes were the padding/mask
         * leftovers, which is how xterm ended up 3x14 pixels. */
        uint32_t window = get32(ev, 8);
        uint16_t mask = get16(ev, 16);
        int32_t x = 0, y = 0, wq = 0, hq = 0;
        bool have_x = false, have_y = false, have_w = false, have_h = false;
        size_t off = 20;
        for (int bit = 0; bit <= 6; bit++) {
            if (!(mask & (1u << bit)))
                continue;
            int32_t v = (int32_t)get32(ev, off);
            off += 4;
            switch (bit) {
            case 0: x = v; have_x = true; break;
            case 1: y = v; have_y = true; break;
            case 2: wq = v; have_w = true; break;
            case 3: hq = v; have_h = true; break;
            default: break; /* border/sibling/stack: ignored */
            }
        }
        struct xw_win *wi = win_track(window);
        if (!wi)
            return;
        if (wi->override) {
            /* the X client owns popup geometry: grant as asked */
            if (have_w || have_h || have_x || have_y) {
                x_configure_window(window, have_x ? x : wi->x,
                                   have_y ? y : wi->y,
                                   have_w ? wq : wi->w, have_h ? hq : wi->h);
                if (have_x) wi->x = x;
                if (have_y) wi->y = y;
                if (have_w) wi->w = wq;
                if (have_h) wi->h = hq;
                if (wi->serial && wc)
                    xw_window_control_manager_v1_set_override_redirect(
                        wc, (uint32_t)(wi->serial >> 32),
                        (uint32_t)wi->serial, wi->x, wi->y, wi->w, wi->h);
            }
            return;
        }
        /* managed window: grant the size (clamped to WM_NORMAL_HINTS),
         * keep the position the compositor placed us at (the mirror
         * invariant: X geometry == compositor geometry, else clicks
         * land in the wrong window) */
        int32_t gw = have_w ? wq : wi->w;
        int32_t gh = have_h ? hq : wi->h;
        if (wi->min_w > 0 && gw < wi->min_w) gw = wi->min_w;
        if (wi->min_h > 0 && gh < wi->min_h) gh = wi->min_h;
        if (wi->max_w > 0 && gw > wi->max_w) gw = wi->max_w;
        if (wi->max_h > 0 && gh > wi->max_h) gh = wi->max_h;
        if (wi->inc_w > 0) gw -= gw % wi->inc_w;
        if (wi->inc_h > 0) gh -= gh % wi->inc_h;
        int32_t px = wi->have_last_geom ? wi->last_x : wi->x;
        int32_t py = wi->have_last_geom ? wi->last_y : wi->y;
        if (gw < 1) gw = 1;
        if (gh < 1) gh = 1;
        XWM_LOG("info",
                "ConfigureRequest: window 0x%x wants %dx%d (mask 0x%x) - "
                "granting %dx%d+%d+%d",
                window, wq, hq, mask, gw, gh, px, py);
        if (!wi->have_last_geom || wi->last_x != px || wi->last_y != py ||
            wi->last_w != gw || wi->last_h != gh) {
            x_configure_window(window, px, py, gw, gh);
            wi->have_last_geom = true;
            wi->last_x = px;
            wi->last_y = py;
            wi->last_w = gw;
            wi->last_h = gh;
        }
        wi->w = gw;
        wi->h = gh;
        break;
    }
    case 33: { /* ClientMessage */
        uint32_t window = get32(ev, 4);
        uint32_t type = get32(ev, 8);
        if (type == 0)
            break;
        /* WL_SURFACE_SERIAL arrives as a root-directed client message
         * with the X window as the target; format 32, l[0]=lo, l[1]=hi */
        if (type == atom_serial) {
            uint64_t serial = (uint64_t)get32(ev, 12) |
                              ((uint64_t)get32(ev, 16) << 32);
            struct xw_win *w = win_track(window);
            if (w) {
                w->serial = serial;
                XWM_LOG("info", "window 0x%x <-> serial %llu", window,
                        (unsigned long long)serial);
                /* identity now that the compositor can key on it */
                win_send_identity(w);
                /* apply any geometry the compositor sent before this
                 * association arrived, so the X position matches our
                 * placement (input coordinates) from the first click */
                struct pending_geom pg;
                if (pend_take(serial, &pg)) {
                    XWM_LOG("info",
                            "  applying pending geometry %dx%d+%d+%d",
                            pg.w, pg.h, pg.x, pg.y);
                    x_configure_window(window, pg.x, pg.y, pg.w, pg.h);
                    w->have_last_geom = true;
                    w->last_x = pg.x;
                    w->last_y = pg.y;
                    w->last_w = pg.w;
                    w->last_h = pg.h;
                }
                /* focus that raced ahead of the association */
                if (focus_serial_pend == serial) {
                    focus_serial_pend = 0;
                    x_focus_window(w);
                }
            }
            break;
        }
        if (type == atom_net_close_window) {
            /* an X-side taskbar/pager asked us to close a window: same
             * path as the compositor's taskbar close */
            struct xw_win *w = win_find_xid(window);
            XWM_LOG("info", "_NET_CLOSE_WINDOW: 0x%x", window);
            if (w)
                win_close(w);
            break;
        }
        if (type == atom_net_active_window) {
            /* activation request from an X client (taskbar click). We
             * have no compositor->X activation channel yet; the honest
             * response is to log it (see WORKLOG remaining-work). */
            XWM_LOG("info",
                    "_NET_ACTIVE_WINDOW request for 0x%x - no channel "
                    "to the compositor focus model yet",
                    get32(ev, 16));
        }
        break;
    }
    case 28: { /* PropertyNotify: window, atom, time, state */
        uint32_t window = get32(ev, 4);
        uint32_t atom = get32(ev, 8);
        if (get32(ev, 12))
            last_x_time = get32(ev, 12);
        if (atom != atom_wm_name && atom != atom_net_wm_name &&
            atom != atom_wm_class && atom != atom_wm_normal_hints)
            break;
        struct xw_win *w = win_find_xid(window);
        if (!w || !w->managed)
            break;
        XWM_LOG("info", "PropertyNotify: window 0x%x property 0x%x "
                "changed - re-reading", window, atom);
        if (atom == atom_wm_name || atom == atom_net_wm_name) {
            w->has_name = false;
            if (x_read_window_name(w->xid, w->name, sizeof(w->name)))
                w->has_name = true;
        } else if (atom == atom_wm_class) {
            w->has_class = false;
            if (x_read_window_class(w->xid, w->klass, sizeof(w->klass)))
                w->has_class = true;
        } else {
            x_read_normal_hints(w);
        }
        win_send_identity(w);
        break;
    }
    case 22: { /* ConfigureNotify: X truth geometry (OR windows report
         * their own moves here - e.g. a tooltip following the pointer) */
        uint32_t window = get32(ev, 8);
        struct xw_win *w = win_find_xid(window);
        if (!w)
            break;
        w->x = (int16_t)get16(ev, 16);
        w->y = (int16_t)get16(ev, 18);
        w->w = get16(ev, 20);
        w->h = get16(ev, 22);
        w->override = ev[25] != 0;
        if (w->override && w->serial && wc) {
            XWM_LOG("info", "override-redirect 0x%x moved to %dx%d+%d+%d",
                    window, w->w, w->h, w->x, w->y);
            xw_window_control_manager_v1_set_override_redirect(
                wc, (uint32_t)(w->serial >> 32), (uint32_t)w->serial, w->x,
                w->y, w->w, w->h);
        }
        break;
    }
    case 18: { /* UnmapNotify: event window, unmapped window, from-conf */
        if (get32(ev, 4) != x_root)
            break; /* interior unmaps belong to the client's own tree */
        uint32_t window = get32(ev, 8);
        struct xw_win *w = win_find_xid(window);
        if (!w)
            break;
        if (w->managed) {
            x_set_wm_state(window, 0 /* Withdrawn */);
            client_list_remove(window);
            client_list_write();
            if (focused_xid == window)
                focused_xid = 0;
            XWM_LOG("info", "window 0x%x unmapped (withdrawn)", window);
        }
        win_forget(window);
        break;
    }
    case 17: { /* DestroyNotify: event window, destroyed window */
        if (get32(ev, 4) != x_root)
            break;
        uint32_t window = get32(ev, 8);
        XWM_LOG("info", "window 0x%x destroyed", window);
        if (focused_xid == window)
            focused_xid = 0;
        client_list_remove(window);
        client_list_write();
        win_forget(window);
        break;
    }
    case 19: { /* MapNotify */
        struct xw_win *w = win_find_xid(get32(ev, 8));
        if (w)
            w->mapped = true;
        break;
    }
    case 21: /* ReparentNotify */
    default:
        break;
    }
}

'''

out = src[:si] + [NEW] + src[ei:]
open(PATH, "w").write("".join(out))
print(f"replaced lines {si+1}..{ei} with the new event machinery")
