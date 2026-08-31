/* xw-shortcuts.c — global keyboard shortcut engine.
 *
 * Default table is the xfwm4 4.20 stock binding list (verified against
 * docs.xfce.org/xfce/xfwm4/4.20/keyboard_shortcuts, generated from
 * xfconf-query defaults) plus the xfce4-settings command defaults
 * (commands/custom channel). Deviations and intentionally-unbound
 * xfwm4 actions are documented in ROADMAP.md.
 *
 * Bindings use the XFCE syntax: "<Shift><Ctrl><Alt><Super>KEYSYM".
 * Dispatch requires an exact modifier match. Duplicate bindings are
 * detected at load time (warned, first one wins).
 */
#include "xw-internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- parsing */

/* bitmask of our tracked modifiers, computed from a seat's xkb indices */
static xkb_mod_mask_t tracked_mod_mask(struct xw_seat *seat) {
    xkb_mod_mask_t m = 0;
    if (seat->mod_shift != XKB_MOD_INVALID)
        m |= 1u << seat->mod_shift;
    if (seat->mod_ctrl != XKB_MOD_INVALID)
        m |= 1u << seat->mod_ctrl;
    if (seat->mod_alt != XKB_MOD_INVALID)
        m |= 1u << seat->mod_alt;
    if (seat->mod_super != XKB_MOD_INVALID)
        m |= 1u << seat->mod_super;
    return m;
}

/* parse one XFCE-style binding string; returns keysym, fills mods */
static xkb_keysym_t parse_binding(struct xw_shortcuts *sc, const char *str,
                                  xkb_mod_mask_t *mods) {
    *mods = 0;
    const char *p = str;
    while (*p == '<') {
        const char *end = strchr(p, '>');
        if (!end)
            return XKB_KEY_NoSymbol;
        size_t len = (size_t)(end - p - 1);
        char tok[16];
        if (len >= sizeof(tok))
            return XKB_KEY_NoSymbol;
        memcpy(tok, p + 1, len);
        tok[len] = 0;
        if (strcasecmp(tok, "shift") == 0)
            *mods |= 1u << sc->mod_shift;
        else if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0 ||
                 strcasecmp(tok, "primary") == 0)
            *mods |= 1u << sc->mod_ctrl;
        else if (strcasecmp(tok, "alt") == 0 || strcasecmp(tok, "mod1") == 0)
            *mods |= 1u << sc->mod_alt;
        else if (strcasecmp(tok, "super") == 0 || strcasecmp(tok, "meta") == 0 ||
                 strcasecmp(tok, "mod4") == 0 || strcasecmp(tok, "win") == 0)
            *mods |= 1u << sc->mod_super;
        else
            return XKB_KEY_NoSymbol; /* unknown modifier token */
        p = end + 1;
    }
    /* remainder is the keysym name */
    while (*p == ' ')
        p++;
    if (!*p)
        return XKB_KEY_NoSymbol;
    return xkb_keysym_from_name(p, XKB_KEYSYM_CASE_INSENSITIVE);
}

static struct xw_shortcut *binding_add(struct xw_shortcuts *sc, int action,
                                       const char *arg, const char *binding) {
    xkb_mod_mask_t mods = 0;
    xkb_keysym_t sym = parse_binding(sc, binding, &mods);
    if (sym == XKB_KEY_NoSymbol || (mods & ~sc->tracked_mods)) {
        xw_log(XW_LOG_WARN, "shortcuts: cannot parse binding '%s'", binding);
        return NULL;
    }
    struct xw_shortcut *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->action = action;
    b->arg = arg ? strdup(arg) : NULL;
    b->binding_str = strdup(binding);
    b->keysym = sym;
    b->mods = mods;

    /* conflict detection: exact duplicate (keysym, mods) */
    struct xw_shortcut *it;
    wl_list_for_each(it, &sc->bindings, link) {
        if (it->keysym == sym && it->mods == mods) {
            xw_log(XW_LOG_WARN,
                   "shortcuts: '%s' conflicts with '%s' (both %s); keeping "
                   "the first",
                   binding, it->binding_str ? it->binding_str : "?", binding);
            sc->conflicts++;
            free(b->arg);
            free(b->binding_str);
            free(b);
            return NULL;
        }
    }
    wl_list_insert(sc->bindings.prev, &b->link);
    return b;
}

/* ------------------------------------------------------------- actions */

static int action_by_name(const char *name, char argbuf[16]) {
    argbuf[0] = 0;
    struct entry {
        const char *name;
        int action;
        const char *arg;
    };
    static const struct entry table[] = {
        {"close-window", XW_ACTION_WINDOW_CLOSE, NULL},
        {"minimize-window", XW_ACTION_WINDOW_MINIMIZE, NULL},
        {"hide-window", XW_ACTION_WINDOW_MINIMIZE, NULL},
        {"maximize-window", XW_ACTION_WINDOW_MAXIMIZE_TOGGLE, NULL},
        {"fullscreen-window", XW_ACTION_WINDOW_FULLSCREEN_TOGGLE, NULL},
        {"stick-window", XW_ACTION_WINDOW_STICK_TOGGLE, NULL},
        {"move-window", XW_ACTION_WINDOW_MOVE, NULL},
        {"resize-window", XW_ACTION_WINDOW_RESIZE, NULL},
        {"raise-window", XW_ACTION_WINDOW_RAISE, NULL},
        {"lower-window", XW_ACTION_WINDOW_LOWER, NULL},
        {"tile-window-left", XW_ACTION_WINDOW_TILE_LEFT, NULL},
        {"tile-window-right", XW_ACTION_WINDOW_TILE_RIGHT, NULL},
        {"tile-window-up", XW_ACTION_WINDOW_TILE_UP, NULL},
        {"tile-window-down", XW_ACTION_WINDOW_TILE_DOWN, NULL},
        {"cycle-windows", XW_ACTION_CYCLE_WINDOWS, NULL},
        {"cycle-windows-back", XW_ACTION_CYCLE_WINDOWS_BACK, NULL},
        {"show-desktop", XW_ACTION_SHOW_DESKTOP, NULL},
        {"workspace-left", XW_ACTION_WORKSPACE_LEFT, NULL},
        {"workspace-right", XW_ACTION_WORKSPACE_RIGHT, NULL},
        {"move-window-workspace-left", XW_ACTION_MOVE_WINDOW_WORKSPACE_LEFT,
         NULL},
        {"move-window-workspace-right", XW_ACTION_MOVE_WINDOW_WORKSPACE_RIGHT,
         NULL},
        {"add-workspace", XW_ACTION_WORKSPACE_ADD, NULL},
        {"delete-workspace", XW_ACTION_WORKSPACE_DELETE_LAST, NULL},
        {"exit-dialog", XW_ACTION_EXIT_DIALOG, NULL},
        {"logout", XW_ACTION_EXIT_DIALOG, NULL},
        {"lock", XW_ACTION_LOCK, NULL},
        {"screenshot", XW_ACTION_SCREENSHOT, NULL},
        {"volume-up", XW_ACTION_VOLUME_UP, NULL},
        {"volume-down", XW_ACTION_VOLUME_DOWN, NULL},
        {"volume-mute", XW_ACTION_VOLUME_MUTE, NULL},
        {"terminal", XW_ACTION_TERMINAL, NULL},
        {"appfinder", XW_ACTION_APPFINDER, NULL},
        {"run-command", XW_ACTION_RUN_COMMAND, NULL},
    };
    /* workspace-N / move-window-workspace-N */
    if (strncmp(name, "workspace-", 10) == 0 && isdigit((unsigned char)name[10])) {
        snprintf(argbuf, 16, "%s", name + 10);
        return XW_ACTION_WORKSPACE_N;
    }
    if (strncmp(name, "move-window-workspace-", 22) == 0 &&
        isdigit((unsigned char)name[22])) {
        snprintf(argbuf, 16, "%s", name + 22);
        return XW_ACTION_MOVE_WINDOW_WORKSPACE_N;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0) {
            if (table[i].arg)
                snprintf(argbuf, 16, "%s", table[i].arg);
            return table[i].action;
        }
    return XW_ACTION_NONE;
}

/* ------------------------------------------------------------ defaults */

void xw_shortcuts_load_defaults(struct xw_shortcuts *sc) {
    struct def {
        const char *name;
        const char *binding;
    };
    /* xfwm4 4.20 defaults + xfce4-settings command defaults; media keys
     * from the XFCE multimedia table */
    static const struct def defs[] = {
        /* window manager (xfwm4) */
        {"close-window", "<Alt>F4"},
        {"stick-window", "<Alt>F6"},
        {"move-window", "<Alt>F7"},
        {"resize-window", "<Alt>F8"},
        {"hide-window", "<Alt>F9"},
        {"maximize-window", "<Alt>F10"},
        {"fullscreen-window", "<Alt>F11"},
        {"add-workspace", "<Alt>Insert"},
        {"delete-workspace", "<Alt>Delete"},
        {"cycle-windows", "<Alt>Tab"},
        {"cycle-windows-back", "<Alt><Shift>Tab"},
        {"minimize-window", "<Shift><Alt>Page_Down"},
        {"raise-window", "<Shift><Alt>Page_Up"},
        {"show-desktop", "<Ctrl><Alt>d"},
        {"workspace-left", "<Ctrl><Alt>Left"},
        {"workspace-right", "<Ctrl><Alt>Right"},
        {"move-window-workspace-left", "<Ctrl><Alt>Home"},
        {"move-window-workspace-right", "<Ctrl><Alt>End"},
        {"workspace-1", "<Ctrl>F1"},
        {"workspace-2", "<Ctrl>F2"},
        {"workspace-3", "<Ctrl>F3"},
        {"workspace-4", "<Ctrl>F4"},
        {"workspace-5", "<Ctrl>F5"},
        {"workspace-6", "<Ctrl>F6"},
        {"workspace-7", "<Ctrl>F7"},
        {"workspace-8", "<Ctrl>F8"},
        {"workspace-9", "<Ctrl>F9"},
        {"workspace-10", "<Ctrl>F10"},
        {"workspace-11", "<Ctrl>F11"},
        {"workspace-12", "<Ctrl>F12"},
        {"move-window-workspace-1", "<Ctrl><Alt>KP_1"},
        {"move-window-workspace-2", "<Ctrl><Alt>KP_2"},
        {"move-window-workspace-3", "<Ctrl><Alt>KP_3"},
        {"move-window-workspace-4", "<Ctrl><Alt>KP_4"},
        {"move-window-workspace-5", "<Ctrl><Alt>KP_5"},
        {"move-window-workspace-6", "<Ctrl><Alt>KP_6"},
        {"move-window-workspace-7", "<Ctrl><Alt>KP_7"},
        {"move-window-workspace-8", "<Ctrl><Alt>KP_8"},
        {"move-window-workspace-9", "<Ctrl><Alt>KP_9"},
        /* commands (xfce4-settings) */
        {"appfinder", "<Alt>F3"},
        {"exit-dialog", "<Ctrl><Alt>Delete"},
        {"lock", "<Ctrl><Alt>l"},
        {"terminal", "<Ctrl><Alt>t"},
        {"screenshot", "<Alt>Print"},
        /* media keys (XFCE multimedia defaults) */
        {"volume-up", "XF86AudioRaiseVolume"},
        {"volume-down", "XF86AudioLowerVolume"},
        {"volume-mute", "XF86AudioMute"},
    };
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        char arg[16];
        int action = action_by_name(defs[i].name, arg);
        if (action != XW_ACTION_NONE)
            binding_add(sc, action, arg[0] ? arg : NULL, defs[i].binding);
    }
}

/* ------------------------------------------------------------ loading */

bool xw_shortcuts_load_file(struct xw_shortcuts *sc, const char *path) {
    struct xw_ini *ini = xw_ini_load(path);
    if (!ini)
        return false;
    /* config entries override the defaults for the same action name:
     * remove existing bindings of that action, then add the new one */
    struct xw_ini_section *sec;
    wl_list_for_each(sec, &ini->sections, link) {
        if (strcmp(sec->name, "shortcuts") != 0)
            continue;
        struct xw_ini_entry *e;
        wl_list_for_each(e, &sec->entries, link) {
            char arg[16];
            int action = action_by_name(e->key, arg);
            if (action == XW_ACTION_NONE) {
                xw_log(XW_LOG_WARN, "shortcuts: unknown action '%s'", e->key);
                continue;
            }
            struct xw_shortcut *b, *b2;
            wl_list_for_each_safe(b, b2, &sc->bindings, link) {
                if (b->action == action) {
                    wl_list_remove(&b->link);
                    free(b->arg);
                    free(b->binding_str);
                    free(b);
                }
            }
            if (action == XW_ACTION_RUN_COMMAND) {
                /* run-command = binding | value = command line */
                char spec[512];
                snprintf(spec, sizeof(spec), "%s", e->value);
                char *pipe = strchr(spec, '|');
                const char *cmd = NULL;
                if (pipe) {
                    *pipe = 0;
                    cmd = pipe + 1;
                    while (*cmd == ' ')
                        cmd++;
                }
                binding_add(sc, XW_ACTION_RUN_COMMAND, cmd,
                            spec[0] ? spec : e->value);
            } else {
                binding_add(sc, action, arg[0] ? arg : NULL, e->value);
            }
        }
    }
    xw_ini_free(ini);
    return true;
}

/* ---------------------------------------------------------- lifecycle */

struct xw_shortcuts *xw_shortcuts_create(struct xw_compositor *c,
                                        const char *config_dir) {
    struct xw_shortcuts *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return NULL;
    sc->comp = c;
    wl_list_init(&sc->bindings);
    wl_list_init(&sc->commands);

    struct xw_seat *seat = xw_seat_first(c);
    if (!seat) {
        xw_log(XW_LOG_ERROR, "shortcuts: no seat");
        free(sc);
        return NULL;
    }
    sc->mod_shift = seat->mod_shift;
    sc->mod_ctrl = seat->mod_ctrl;
    sc->mod_alt = seat->mod_alt;
    sc->mod_super = seat->mod_super;
    sc->tracked_mods = tracked_mod_mask(seat);
    sc->ignore_mask = seat->ignore_mask;

    xw_shortcuts_load_defaults(sc);
    if (config_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/shortcuts.conf", config_dir);
        xw_shortcuts_load_file(sc, path);
    }
    return sc;
}

void xw_shortcuts_destroy(struct xw_shortcuts *sc) {
    if (!sc)
        return;
    struct xw_shortcut *b, *b2;
    wl_list_for_each_safe(b, b2, &sc->bindings, link) {
        wl_list_remove(&b->link);
        free(b->arg);
        free(b->binding_str);
        free(b);
    }
    free(sc);
}

/* ----------------------------------------------------------- dispatch */

bool xw_shortcuts_dispatch(struct xw_shortcuts *sc, struct xw_seat *seat,
                           uint32_t keycode, bool down) {
    if (!down)
        return false;
    /* keycode is a raw linux keycode; xkb expects evdev + 8 */
    xkb_keysym_t sym = xkb_state_key_get_one_sym(seat->xkb_state, keycode + 8);
    if (sym == XKB_KEY_NoSymbol)
        return false;
    xkb_mod_mask_t mods = xkb_state_serialize_mods(seat->xkb_state,
                                                   XKB_STATE_MODS_DEPRESSED) &
                          ~seat->ignore_mask & sc->tracked_mods;

    struct xw_shortcut *b;
    wl_list_for_each(b, &sc->bindings, link) {
        if (b->keysym != sym || b->mods != mods)
            continue;
        xw_actions_dispatch(sc->comp, b->action, b->arg);
        return true;
    }
    return false;
}
