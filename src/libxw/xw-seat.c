/* xw-seat.c — wl_seat, wl_keyboard, wl_pointer, xkbcommon state machine.
 *
 * All input (real or injected) flows through xw_seat_key /
 * xw_seat_pointer_* exactly once, in order:
 *   keyboard: xkb state update → modifiers event → shortcut engine →
 *             interactive move/resize keys → client delivery.
 *   pointer:  hit test (popups → layer shell overlay/top → windows →
 *             bottom/background) → click-to-focus → client delivery.
 *
 * Layer-shell surfaces with exclusive keyboard interactivity take keyboard
 * focus over windows (xfwm4 panel-menu behavior parity).
 */
#include "xw-internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* XFCE defaults: 500 ms delay, 30 repeats per second */
#define XW_REPEAT_DELAY_DEFAULT_MS 500
#define XW_REPEAT_RATE_DEFAULT_HZ 30

#define XW_SEAT_VERSION 8

/* --------------------------------------------------------- wl_keyboard */

static void keyboard_release(struct wl_client *client, struct wl_resource *res);

static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};

static void keyboard_resource_destroy(struct wl_resource *res) {
    struct xw_seat *s = wl_resource_get_user_data(res);
    wl_list_remove(wl_resource_get_link(res));
    (void)s;
}

static void keyboard_release(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

/* --------------------------------------------------------- wl_pointer */

static void pointer_release(struct wl_client *client, struct wl_resource *res);

static const struct wl_pointer_interface pointer_impl = {
    .release = pointer_release,
};

static void pointer_resource_destroy(struct wl_resource *res) {
    wl_list_remove(wl_resource_get_link(res));
}

static void pointer_release(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

/* ------------------------------------------------------------ wl_seat */

static void send_modifiers(struct xw_seat *s);
static void disarm_interactive_repeat(struct xw_seat *s);
static const char *surface_desc(struct xw_surface *s, char *buf, size_t n);

static void seat_get_pointer(struct wl_client *client, struct wl_resource *res,
                             uint32_t id) {
    struct xw_seat *s = wl_resource_get_user_data(res);
    uint32_t v = wl_resource_get_version(res);
    struct wl_resource *p =
        wl_resource_create(client, &wl_pointer_interface, v > 8 ? 8 : v, id);
    if (!p) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_list_insert(s->pointers.prev, wl_resource_get_link(p));
    wl_resource_set_implementation(p, &pointer_impl, s, pointer_resource_destroy);
    /* if this client already owns pointer focus (surface focused
     * before the pointer object existed — e.g. the client created its
     * wl_pointer after mapping), deliver the enter it never saw.
     * Keyboard has the same contract above; pointer was missing it,
     * which left a correctly-focused surface with a pointer that
     * received motion but never enter (clients gate on enter). */
    if (s->ptr_focus &&
        wl_resource_get_client(s->ptr_focus->res) == client) {
        int sx = 0, sy = 0;
        xw_surface_get_pos(s->ptr_focus, &sx, &sy, NULL, NULL);
        wl_pointer_send_enter(p, ++s->serial, s->ptr_focus->res,
                              wl_fixed_from_int(s->cursor_x - sx),
                              wl_fixed_from_int(s->cursor_y - sy));
        wl_pointer_send_frame(p);
        char dbuf[64];
        xw_log(XW_LOG_DEBUG,
               "wayland: pointer enter replayed to a late wl_pointer (%s)",
               surface_desc(s->ptr_focus, dbuf, sizeof(dbuf)));
    }
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *res,
                              uint32_t id) {
    struct xw_seat *s = wl_resource_get_user_data(res);
    uint32_t v = wl_resource_get_version(res);
    struct wl_resource *k =
        wl_resource_create(client, &wl_keyboard_interface, v > 8 ? 8 : v, id);
    if (!k) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_list_insert(s->keyboards.prev, wl_resource_get_link(k));
    wl_resource_set_implementation(k, &keyboard_impl, s,
                                   keyboard_resource_destroy);
    /* deliver the shared keymap (memfd is dup'ed per client; libwayland
     * closes the fd after writing it to the socket) */
    if (s->keymap_fd >= 0) {
        int fd = dup(s->keymap_fd);
        if (fd < 0) {
            xw_log(XW_LOG_ERROR, "seat: keymap dup failed");
            return;
        }
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                (uint32_t)s->keymap_len);
    }
    /* repeat rate/delay (wayland: the CLIENT repeats, the server only
     * advertises the parameters — v4+) */
    if (wl_resource_get_version(k) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(k, s->repeat_rate_hz, s->repeat_delay_ms);
    /* if this client already owns keyboard focus (surface mapped and
     * focused before the keyboard object existed), send enter now */
    if (s->kb_focus &&
        wl_resource_get_client(s->kb_focus->res) == client) {
        struct wl_array empty;
        wl_array_init(&empty);
        wl_keyboard_send_enter(k, ++s->serial, s->kb_focus->res, &empty);
        wl_array_release(&empty);
        s->sent_depressed = ~0u; /* force modifiers re-send */
        send_modifiers(s);
    }
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *res,
                           uint32_t id) {
    (void)client;
    (void)res;
    (void)id;
    /* touch unsupported in v0; capabilities never advertise touch */
}

static void seat_release(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void bind_seat(struct wl_client *client, void *data, uint32_t version,
                      uint32_t id) {
    struct xw_seat *s = data;
    if (version > XW_SEAT_VERSION)
        version = XW_SEAT_VERSION;
    struct wl_resource *res =
        wl_resource_create(client, &wl_seat_interface, version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &seat_impl, s, NULL);
    wl_list_insert(s->resources.prev, wl_resource_get_link(res));
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER |
                                       WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(res, s->name);
}

/* ------------------------------------------------------------ internals */

/* topmost layer-shell surface claiming keyboard (exclusive mode) */
static struct xw_layer_surface *layer_keyboard_owner(struct xw_compositor *c) {
    for (int layer = 3; layer >= 2; layer--) {
        struct xw_layer_surface *ls;
        wl_list_for_each(ls, &c->wm->layers[layer], link) {
            if (ls->mapped && ls->keyboard_interactivity)
                return ls;
        }
    }
    return NULL;
}

/* short identity string for focus/button logs: role + name. Kept
 * static (xw-internal.h types only) so every pointer-path line says
 * WHICH surface got the event — the difference between "focus went
 * somewhere" and "focus went to the panel" in a real-session log. */
static const char *surface_desc(struct xw_surface *s, char *buf, size_t n) {
    if (!s)
        return "(none)";
    switch (s->role) {
    case XW_SURFACE_ROLE_LAYER: {
        struct xw_layer_surface *ls = s->role_data;
        snprintf(buf, n, "layer-shell '%.32s'",
                 ls && ls->namespace[0] ? ls->namespace : "?");
        return buf;
    }
    case XW_SURFACE_ROLE_XDG_TOPLEVEL: {
        struct xw_window *w = s->role_data;
        snprintf(buf, n, "window '%.32s'",
                 w && w->title[0] ? w->title : "?");
        return buf;
    }
    case XW_SURFACE_ROLE_XDG_POPUP:
        return "xdg-popup";
    case XW_SURFACE_ROLE_SESSION_LOCK:
        return "session-lock";
    default:
        return "(no role)";
    }
}

/* topmost surface at global point, excluding windows (used for layers);
 * windows are hit-tested by the wm. While the session lock gate is
 * engaged, ONLY lock surfaces are hit-testable: input belongs to the
 * lock client (ext-session-lock security requirement). */
static struct xw_surface *surface_at(struct xw_seat *s, int x, int y) {
    struct xw_compositor *c = s->comp;

    if (xw_session_lock_active(c))
        return xw_session_lock_surface_at(c, x, y);

    /* active grabs capture everything */
    if (s->grab_surface)
        return s->grab_surface;

    /* popups: list tail = topmost */
    struct xw_popup *p;
    wl_list_for_each_reverse(p, &c->popups, link) {
        if (p->mapped && p->surface && xw_surface_has_input_at(p->surface, x, y))
            return p->surface;
    }

    /* layer shell: overlay/top/bottom/background, head = topmost */
    for (int layer = 3; layer >= 0; layer--) {
        struct xw_layer_surface *ls;
        wl_list_for_each(ls, &c->wm->layers[layer], link) {
            if (ls->mapped && ls->surface &&
                xw_surface_has_input_at(ls->surface, x, y))
                return ls->surface;
        }
    }

    struct xw_window *w = xw_wm_window_at(c->wm, x, y, NULL);
    return w ? w->surface : NULL;
}

/* send pointer events to every wl_pointer resource owned by surface's
 * client; helper macro keeps the send sites short */
#define PTR_FOR_EACH(surf, ptr)                                              \
    struct wl_client *_cl =                                                  \
        (surf) ? wl_resource_get_client((surf)->res) : NULL;                 \
    if (_cl)                                                                 \
        wl_list_for_each(ptr, &(s)->pointers, link)                          \
            if (wl_resource_get_client(ptr) == _cl)

static void send_modifiers(struct xw_seat *s) {
    xkb_mod_mask_t dep = xkb_state_serialize_mods(s->xkb_state,
                                                  XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t lat = xkb_state_serialize_mods(s->xkb_state,
                                                  XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t loc = xkb_state_serialize_mods(s->xkb_state,
                                                  XKB_STATE_MODS_LOCKED);
    xkb_mod_mask_t grp = xkb_state_serialize_layout(s->xkb_state,
                                                    XKB_STATE_LAYOUT_EFFECTIVE);
    if (dep == s->sent_depressed && lat == s->sent_latched &&
        loc == s->sent_locked && grp == s->sent_group)
        return;
    s->sent_depressed = dep;
    s->sent_latched = lat;
    s->sent_locked = loc;
    s->sent_group = grp;
    if (!s->kb_focus || wl_list_empty(&s->keyboards))
        return;
    struct wl_client *cl = wl_resource_get_client(s->kb_focus->res);
    struct wl_resource *k;
    wl_list_for_each(k, &s->keyboards, link) {
        if (wl_resource_get_client(k) == cl)
            wl_keyboard_send_modifiers(k, ++s->serial, dep, lat, loc, grp);
    }
}

void xw_seat_set_kb_focus(struct xw_seat *s, struct xw_surface *surface) {
    disarm_interactive_repeat(s);
    /* session lock: lock surfaces own the keyboard absolutely while
     * engaged (shortcuts are dead, clients never see keys) */
    if (xw_session_lock_active(s->comp)) {
        struct xw_surface *lk = xw_session_lock_kb_owner(s->comp);
        if (lk != surface) {
            surface = lk;
        }
    }
    /* exclusive layer interactivity overrides window focus */
    struct xw_layer_surface *owner = layer_keyboard_owner(s->comp);
    if (owner && owner->surface != surface)
        surface = owner->surface;

    if (s->kb_focus == surface)
        return;
    struct wl_client *old =
        s->kb_focus ? wl_resource_get_client(s->kb_focus->res) : NULL;
    struct wl_client *newc =
        surface ? wl_resource_get_client(surface->res) : NULL;

    if (old && old != newc) {
        struct wl_resource *k;
        wl_list_for_each(k, &s->keyboards, link) {
            if (wl_resource_get_client(k) == old)
                wl_keyboard_send_leave(k, ++s->serial, s->kb_focus->res);
        }
    }
    s->kb_focus = surface;
    if (newc) {
        struct wl_resource *k;
        wl_list_for_each(k, &s->keyboards, link) {
            if (wl_resource_get_client(k) == newc) {
                struct wl_array empty;
                wl_array_init(&empty);
                wl_keyboard_send_enter(k, ++s->serial, surface->res, &empty);
                wl_array_release(&empty);
            }
        }
        /* bring the new client up to date with current modifier state */
        s->sent_depressed = ~0u; /* force re-send */
        send_modifiers(s);
    }
    xw_data_device_notify_focus(s->comp, s);
}

static void set_ptr_focus(struct xw_seat *s, struct xw_surface *surface) {
    if (s->ptr_focus == surface)
        return;
    char dbuf[64], nbuf[64];
    const char *desc_old = surface_desc(s->ptr_focus, dbuf, sizeof(dbuf));
    const char *desc_new = surface_desc(surface, nbuf, sizeof(nbuf));
    struct wl_client *old =
        s->ptr_focus ? wl_resource_get_client(s->ptr_focus->res) : NULL;
    struct wl_client *newc =
        surface ? wl_resource_get_client(surface->res) : NULL;

    if (old && old != newc && !s->ptr_grab) {
        struct wl_resource *p;
        wl_list_for_each(p, &s->pointers, link) {
            if (wl_resource_get_client(p) == old) {
                wl_pointer_send_leave(p, ++s->serial, s->ptr_focus->res);
                wl_pointer_send_frame(p);
                xw_log(XW_LOG_DEBUG, "wayland: pointer leave %s (serial %u)",
                       desc_old, s->serial);
            }
        }
    } else if (old && old != newc && s->ptr_focus && s->ptr_grab) {
        /* leave suppressed under grab; the grab owner already has focus */
    }
    s->ptr_focus = surface;
    if (newc) {
        struct wl_resource *p;
        wl_list_for_each(p, &s->pointers, link) {
            if (wl_resource_get_client(p) == newc) {
                int sx = 0, sy = 0;
                xw_surface_get_pos(surface, &sx, &sy, NULL, NULL);
                wl_pointer_send_enter(p, ++s->serial, surface->res,
                                      wl_fixed_from_int(s->cursor_x - sx),
                                      wl_fixed_from_int(s->cursor_y - sy));
                wl_pointer_send_frame(p);
                if (!s->first_enter_ms) {
                    s->first_enter_ms = xw_now_ms();
                    xw_log(XW_LOG_INFO,
                           "pointer: first enter delivered (%s at %d,%d — "
                           "wl_pointer.enter reached a client %.1fs after "
                           "startup)",
                           desc_new, s->cursor_x, s->cursor_y,
                           (s->first_enter_ms - s->started_ms) / 1000.0);
                }
                xw_log(XW_LOG_DEBUG,
                       "wayland: pointer enter %s at %d,%d (serial %u)",
                       desc_new, s->cursor_x, s->cursor_y, s->serial);
            }
        }
    }
    xw_log(XW_LOG_DEBUG, "pointer: focus %s -> %s", desc_old, desc_new);
}

/* Re-evaluate pointer focus from the CURRENT cursor position. Called
 * whenever the surface stack changes under a stationary cursor: a
 * surface mapping (the cursor was already over the bar when the panel
 * came up), unmapping, or dying — motion alone re-runs surface_at, but
 * a stacking change without motion used to leave focus stale. */
void xw_seat_repointer(struct xw_compositor *c) {
    if (!c)
        return;
    struct xw_seat *s;
    wl_list_for_each(s, &c->seats, link) {
        if (s->grab_surface)
            continue; /* an implicit grab pins focus until release */
        struct xw_surface *target = surface_at(s, s->cursor_x, s->cursor_y);
        set_ptr_focus(s, target);
    }
}

/* A surface is being destroyed: drop every seat reference to it BEFORE
 * the memory is freed. This is not optional bookkeeping — ptr_focus is
 * dereferenced (res -> wl_client) by the next motion event, so a stale
 * pointer here is a use-after-free that also poisons all later focus
 * decisions (the classic "panel visible, cursor moves, nothing reacts"
 * decay after any hovered surface dies). Leave is NOT sent: the surface
 * resource is going away, and enter/leave reference it. */
void xw_seat_forget_surface(struct xw_compositor *c, struct xw_surface *s) {
    if (!c || !s)
        return;
    struct xw_seat *seat;
    wl_list_for_each(seat, &c->seats, link) {
        if (seat->ptr_focus == s) {
            char dbuf[64];
            xw_log(XW_LOG_DEBUG, "pointer: focus surface destroyed (%s)",
                   surface_desc(s, dbuf, sizeof(dbuf)));
            seat->ptr_focus = NULL;
        }
        if (seat->grab_surface == s) {
            seat->ptr_grab = NULL;
            seat->grab_surface = NULL;
            seat->ptr_grab_is_drag = false;
        }
        if (seat->drag.origin == s)
            seat->drag.origin = NULL;
        if (seat->kb_focus == s)
            xw_seat_set_kb_focus(seat, NULL);
    }
}

/* ----------------------------------------------------------------- keys */

static bool is_consumed(struct xw_seat *s, uint32_t code) {
    return (s->consumed_keys[code / 32] >> (code % 32)) & 1u;
}

static void mark_consumed(struct xw_seat *s, uint32_t code, bool on) {
    if (on)
        s->consumed_keys[code / 32] |= 1u << (code % 32);
    else
        s->consumed_keys[code / 32] &= ~(1u << (code % 32));
}

/* ------------------------------------------------ interactive repeat */

static void arm_interactive_repeat(struct xw_seat *s, uint32_t keycode) {
    s->repeat_key = keycode;
    s->repeat_active = true;
    wl_event_source_timer_update(s->repeat_src, s->repeat_delay_ms);
}

static void disarm_interactive_repeat(struct xw_seat *s) {
    if (!s->repeat_active)
        return;
    s->repeat_active = false;
    wl_event_source_timer_update(s->repeat_src, 0);
}

static int interactive_repeat_cb(void *data) {
    struct xw_seat *s = data;
    if (!s->repeat_active)
        return 0;
    struct xw_window *iw = xw_wm_interactive_window(s->comp->wm);
    if (!iw) {
        /* interactive mode ended without a key release */
        disarm_interactive_repeat(s);
        return 0;
    }
    xw_wm_interactive_key(s->comp->wm, iw, s->repeat_key, true);
    wl_event_source_timer_update(s->repeat_src, s->repeat_period_ms);
    return 0;
}

void xw_seat_key(struct xw_seat *s, uint32_t keycode, bool down) {
    if (!s->xkb_state)
        return;

    /* wayland/xkbcommon keycodes are evdev + 8; the injection API and
     * the wm use raw linux keycodes */
    xkb_state_update_key(s->xkb_state, keycode + 8,
                         down ? XKB_KEY_DOWN : XKB_KEY_UP);
    send_modifiers(s);
    xw_idle_activity(s);

    uint32_t time = (uint32_t)xw_now_ms();

    /* session lock: input goes ONLY to the focused lock surface — no
     * shortcuts, no interactive move/resize (security gate). The
     * keyboard focus is pinned to a lock surface by
     * xw_seat_set_kb_focus while the gate is engaged. */
    if (xw_session_lock_active(s->comp)) {
        if (!s->kb_focus || wl_list_empty(&s->keyboards))
            return;
        struct wl_client *cl = wl_resource_get_client(s->kb_focus->res);
        struct wl_resource *k;
        wl_list_for_each(k, &s->keyboards, link) {
            if (wl_resource_get_client(k) == cl)
                wl_keyboard_send_key(k, ++s->serial, time, keycode + 8,
                                     down ? WL_KEYBOARD_KEY_STATE_PRESSED
                                          : WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        return;
    }

    /* 1. shortcut engine (key-down only; releases of consumed keys are
     *    suppressed so clients never see a stray release) */
    if (down && s->comp->shortcuts &&
        xw_shortcuts_dispatch(s->comp->shortcuts, s, keycode, true)) {
        mark_consumed(s, keycode, true);
        return;
    }
    if (!down && is_consumed(s, keycode)) {
        /* the press was consumed (shortcut or interactive move/resize):
         * the release never reaches the client, and any interactive
         * auto-repeat of this key stops here */
        if (s->repeat_active && keycode == s->repeat_key)
            disarm_interactive_repeat(s);
        mark_consumed(s, keycode, false);
        return;
    }

    /* 2. interactive move/resize keys (server-side repeat: held arrow
     *    keys keep moving; client-visible keys are never repeated by
     *    the server — clients repeat via repeat_info). The press
     *    is consumed like a shortcut press so its release never leaks
     *    to the client as a stray event. */
    struct xw_window *iw = xw_wm_interactive_window(s->comp->wm);
    if (iw && xw_wm_interactive_key(s->comp->wm, iw, keycode, down)) {
        if (down) {
            arm_interactive_repeat(s, keycode);
            mark_consumed(s, keycode, true);
        } else if (s->repeat_active && keycode == s->repeat_key)
            disarm_interactive_repeat(s);
        return;
    }
    if (!down && s->repeat_active && keycode == s->repeat_key)
        disarm_interactive_repeat(s);

    /* 3. deliver to the focused client (evdev + 8) */
    if (!s->kb_focus || wl_list_empty(&s->keyboards))
        return;
    struct wl_client *cl = wl_resource_get_client(s->kb_focus->res);
    struct wl_resource *k;
    wl_list_for_each(k, &s->keyboards, link) {
        if (wl_resource_get_client(k) == cl)
            wl_keyboard_send_key(k, ++s->serial, time, keycode + 8,
                                 down ? WL_KEYBOARD_KEY_STATE_PRESSED
                                      : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
}

/* -------------------------------------------------------------- pointer */

static void damage_cursor(struct xw_compositor *c, int x, int y) {
    xw_damage_outputs_rect(c, x - 2, y - 2, 16, 21);
}

void xw_seat_pointer_motion(struct xw_seat *s, int x, int y) {
    struct xw_compositor *c = s->comp;
    int old_x = s->cursor_x, old_y = s->cursor_y;
    s->cursor_x = x;
    s->cursor_y = y;
    xw_idle_activity(s);

    if (s->drag.active && !xw_session_lock_active(c)) {
        xw_data_device_drag_motion(c, s, x, y);
        damage_cursor(c, old_x, old_y);
        damage_cursor(c, x, y);
        return;
    }

    if (!s->grab_surface) {
        struct xw_surface *target = surface_at(s, x, y);
        set_ptr_focus(s, target);
    }

    /* interactive window move/resize follows the cursor (impossible
     * while locked: cancel_interactions ran when the gate engaged) */
    struct xw_window *iw = xw_wm_interactive_window(c->wm);
    if (iw && s->ptr_grab && !xw_session_lock_active(c)) {
        xw_wm_interactive_motion(c->wm, iw, x, y);
        damage_cursor(c, old_x, old_y);
        damage_cursor(c, x, y);
        return;
    }

    if (s->ptr_focus && !wl_list_empty(&s->pointers)) {
        struct wl_resource *p;
        int sx = 0, sy = 0;
        xw_surface_get_pos(s->ptr_focus, &sx, &sy, NULL, NULL);
        PTR_FOR_EACH(s->ptr_focus, p) {
            wl_pointer_send_motion(p, (uint32_t)xw_now_ms(),
                                   wl_fixed_from_int(x - sx),
                                   wl_fixed_from_int(y - sy));
            wl_pointer_send_frame(p);
        }
    }
    damage_cursor(c, old_x, old_y);
    damage_cursor(c, x, y);
}

void xw_seat_pointer_button(struct xw_seat *s, uint32_t linux_button,
                            bool down) {
    struct xw_compositor *c = s->comp;
    uint32_t time = (uint32_t)xw_now_ms();
    uint32_t state =
        down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
    xw_idle_activity(s);

    /* session lock: buttons go only to the lock surface under the
     * pointer (surface_at pins ptr_focus while engaged); a press on a
     * lock surface also moves keyboard focus to it (multi-output). No
     * popups, no wm focus/raise, no grabs, no drag-drop. */
    if (xw_session_lock_active(c)) {
        if (s->ptr_focus && !wl_list_empty(&s->pointers)) {
            if (down)
                xw_seat_set_kb_focus(s, s->ptr_focus);
            struct wl_resource *p;
            PTR_FOR_EACH(s->ptr_focus, p) {
                wl_pointer_send_button(p, ++s->serial, time, linux_button,
                                       state);
                wl_pointer_send_frame(p);
            }
        }
        return;
    }

    /* drag-and-drop: release performs the drop */
    if (!down && s->drag.active) {
        xw_data_device_drag_drop(c, s);
        return;
    }

    /* grab release */
    if (!down && s->ptr_grab) {
        /* deliver the final release to the grab surface first */
        if (s->grab_surface && !wl_list_empty(&s->pointers)) {
            struct wl_resource *p;
            struct wl_client *cl = wl_resource_get_client(s->grab_surface->res);
            int sx = 0, sy = 0;
            xw_surface_get_pos(s->grab_surface, &sx, &sy, NULL, NULL);
            wl_list_for_each(p, &s->pointers, link) {
                if (wl_resource_get_client(p) == cl) {
                    wl_pointer_send_button(p, ++s->serial, time, linux_button,
                                           state);
                    wl_pointer_send_frame(p);
                }
            }
            (void)sx;
            (void)sy;
        }
        if (!s->ptr_grab_is_drag) {
            /* end interactive move/resize (if any) */
            struct xw_window *iw = xw_wm_interactive_window(c->wm);
            if (iw)
                xw_wm_interactive_end(c->wm, iw);
            s->ptr_grab = NULL;
            s->grab_surface = NULL;
            /* re-hit-test under the cursor */
            struct xw_surface *target = surface_at(s, s->cursor_x, s->cursor_y);
            set_ptr_focus(s, target);
        }
        return;
    }

    /* a press outside the topmost popup dismisses the popup chain
     * (menu/tooltip behavior: xfwm4 closes menus on outside clicks) */
    if (down && !s->ptr_grab) {
        struct xw_popup *p;
        wl_list_for_each_reverse(p, &c->popups, link) {
            if (!p->mapped)
                continue;
            if (s->ptr_focus && p->surface == s->ptr_focus)
                break; /* the press is on this popup: keep it (and its
                          * parents, below us in the list) open */
            xw_popup_dismiss(p);
        }
    }

    /* click-to-focus + raise on window press (xfwm4 default) */
    if (down && s->ptr_focus && !s->ptr_grab) {
        /* find the window under the pointer; wl_list_for_each yields the
         * head sentinel (not a valid window) when nothing matches */
        struct xw_window *w;
        bool hit = false;
        wl_list_for_each(w, &c->wm->stack, stack_link) {
            if (w->surface == s->ptr_focus) {
                hit = true;
                break;
            }
        }
        if (hit) {
            if (w->ws != -1 && w->ws != c->wm->ws_current) {
                /* clicking a window of another workspace can't happen
                 * (invisible), defensive only */
            }
            xw_wm_focus_window(c->wm, w, true);
        } else if (s->ptr_focus) {
            /* layer surfaces with on-demand interactivity take focus on
             * click; exclusive ones already own it.
             *
             * NOTE: wl_list_for_each on an EMPTY list leaves the
             * iterator pointing at the list head (sentinel) — and a
             * NULL assignment inside the body would make the loop
             * increment dereference it.  A separate found-variable
             * avoids both (found by UBSan via the panel click test:
             * OVERLAY empty, panel on TOP). */
            struct xw_layer_surface *ls = NULL, *it;
            for (int layer = 3; layer >= 0 && !ls; layer--) {
                wl_list_for_each(it, &c->wm->layers[layer], link) {
                    if (it->mapped && it->surface == s->ptr_focus) {
                        ls = it;
                        break;
                    }
                }
            }
            if (ls && ls->keyboard_interactivity)
                xw_seat_set_kb_focus(s, ls->surface);
        }

        /* begin implicit grab: all further pointer input follows this
         * surface until button release */
        s->ptr_grab = NULL; /* set below if a pointer resource exists */
        struct wl_client *cl = wl_resource_get_client(s->ptr_focus->res);
        struct wl_resource *p;
        wl_list_for_each(p, &s->pointers, link) {
            if (wl_resource_get_client(p) == cl) {
                s->ptr_grab = p;
                break;
            }
        }
        s->ptr_grab_is_drag = false;
        s->grab_surface = s->ptr_focus;
    }

    if (s->ptr_focus && !wl_list_empty(&s->pointers)) {
        struct wl_resource *p;
        int sx = 0, sy = 0;
        xw_surface_get_pos(s->ptr_focus, &sx, &sy, NULL, NULL);
        (void)sx;
        (void)sy;
        char dbuf[64];
        xw_log(XW_LOG_DEBUG, "wayland: pointer button %u %s surface=%s",
               linux_button, down ? "pressed" : "released",
               surface_desc(s->ptr_focus, dbuf, sizeof(dbuf)));
        PTR_FOR_EACH(s->ptr_focus, p) {
            wl_pointer_send_button(p, ++s->serial, time, linux_button, state);
            wl_pointer_send_frame(p);
        }
    }
}

void xw_seat_pointer_axis(struct xw_seat *s, uint32_t axis, double value) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL &&
        axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        return;
    xw_idle_activity(s);
    if (s->ptr_focus && !wl_list_empty(&s->pointers)) {
        struct wl_resource *p;
        PTR_FOR_EACH(s->ptr_focus, p) {
            wl_pointer_send_axis(p, (uint32_t)xw_now_ms(), axis,
                                 wl_fixed_from_double(value));
            wl_pointer_send_frame(p);
        }
    }
}

/* ------------------------------------------------------------- lifecycle */

struct xw_seat *xw_seat_create(struct xw_compositor *c, const char *name) {
    struct xw_seat *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->comp = c;
    snprintf(s->name, sizeof(s->name), "%s", name);
    wl_list_init(&s->resources);
    wl_list_init(&s->pointers);
    wl_list_init(&s->keyboards);
    wl_list_init(&s->data_devices);
    s->keymap_fd = -1;
    s->last_activity_ms = xw_now_ms(); /* idle-notify baseline */
    s->started_ms = s->last_activity_ms;

    s->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!s->xkb_ctx)
        goto fail;

    /* empty RMLVO would compile a raw non-evdev keymap (keycode 29 = 'y',
     * no modifiers) — always default to the evdev ruleset */
    struct xkb_rule_names names = {
        .rules = c->conf.xkb_rules ? c->conf.xkb_rules : "evdev",
        .model = c->conf.xkb_model ? c->conf.xkb_model : "pc105",
        .layout = c->conf.xkb_layout ? c->conf.xkb_layout : "us",
        .variant = c->conf.xkb_variant,
        .options = c->conf.xkb_options,
    };
    s->keymap = xkb_keymap_new_from_names(s->xkb_ctx, &names,
                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!s->keymap)
        goto fail;
    s->xkb_state = xkb_state_new(s->keymap);
    if (!s->xkb_state)
        goto fail;

    s->mod_shift = xkb_keymap_mod_get_index(s->keymap, "Shift");
    s->mod_ctrl = xkb_keymap_mod_get_index(s->keymap, "Control");
    s->mod_alt = xkb_keymap_mod_get_index(s->keymap, "Mod1");
    s->mod_super = xkb_keymap_mod_get_index(s->keymap, "Mod4");
    xkb_mod_index_t caps = xkb_keymap_mod_get_index(s->keymap, "Lock");
    xkb_mod_index_t num = xkb_keymap_mod_get_index(s->keymap, "Mod2");
    s->ignore_mask = 0;
    if (caps != XKB_MOD_INVALID)
        s->ignore_mask |= 1u << caps;
    if (num != XKB_MOD_INVALID)
        s->ignore_mask |= 1u << num;

    /* serialize keymap to a memfd shared with clients */
    char *km = xkb_keymap_get_as_string(s->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!km)
        goto fail;
    s->keymap_len = strlen(km) + 1;
    int fd = memfd_create("xw-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        free(km);
        goto fail;
    }
    if (ftruncate(fd, (off_t)s->keymap_len) < 0 ||
        write(fd, km, s->keymap_len) < 0) {
        close(fd);
        free(km);
        goto fail;
    }
    lseek(fd, 0, SEEK_SET);
    s->keymap_fd = fd;
    s->keymap_area = km;

    /* key repeat parameters (config: keyboard.conf / xw_compositor_config;
     * see the defaults note at the top of this file) */
    s->repeat_delay_ms =
        c->conf.repeat_delay_ms > 0 ? c->conf.repeat_delay_ms
                                    : XW_REPEAT_DELAY_DEFAULT_MS;
    s->repeat_rate_hz =
        c->conf.repeat_rate_hz > 0 ? c->conf.repeat_rate_hz
                                   : XW_REPEAT_RATE_DEFAULT_HZ;
    s->repeat_period_ms = 1000 / s->repeat_rate_hz;
    if (s->repeat_period_ms < 1)
        s->repeat_period_ms = 1; /* clamp for very high rates */
    s->repeat_active = false;
    s->repeat_key = 0;
    s->repeat_src =
        wl_event_loop_add_timer(c->loop, interactive_repeat_cb, s);
    if (!s->repeat_src)
        goto fail;

    s->global = wl_global_create(c->display, &wl_seat_interface,
                                 XW_SEAT_VERSION, s, bind_seat);
    if (!s->global)
        goto fail;

    wl_list_insert(c->seats.prev, &s->link);
    xw_log(XW_LOG_INFO, "seat %s ready (xkb layout=%s model=%s)", s->name,
           c->conf.xkb_layout ? c->conf.xkb_layout : "default",
           c->conf.xkb_model ? c->conf.xkb_model : "default");
    return s;

fail:
    xw_log(XW_LOG_ERROR, "seat creation failed");
    xw_seat_destroy(s);
    return NULL;
}

void xw_seat_destroy(struct xw_seat *s) {
    if (!s)
        return;
    wl_list_remove(&s->link);
    if (s->global)
        wl_global_destroy(s->global);
    /* idle notifications bound to this seat die with it (no events) */
    xw_idle_seat_destroyed(s->comp, s);
    /* client-side resources (keyboards/pointers/data devices) are destroyed
     * with their clients; just unlink ours */
    while (!wl_list_empty(&s->keyboards))
        wl_list_remove(s->keyboards.next);
    while (!wl_list_empty(&s->pointers))
        wl_list_remove(s->pointers.next);
    while (!wl_list_empty(&s->resources))
        wl_list_remove(s->resources.next);
    if (s->repeat_src)
        wl_event_source_remove(s->repeat_src);
    if (s->keymap_fd >= 0)
        close(s->keymap_fd);
    free(s->keymap_area);
    if (s->xkb_state)
        xkb_state_unref(s->xkb_state);
    if (s->keymap)
        xkb_keymap_unref(s->keymap);
    if (s->xkb_ctx)
        xkb_context_unref(s->xkb_ctx);
    free(s);
}
