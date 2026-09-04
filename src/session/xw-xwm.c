/* xw-xwm.c — the X11 window manager for Xwayland (session helper).
 *
 * Xwayland 24 rootless mode needs three things NO Wayland protocol can
 * provide, and the compositor deliberately contains no X11 code:
 *
 *   1. an X client that composite-redirects the root's subwindows
 *      (Xwayland only creates a wl_surface for windows with
 *      redirectDraw == RedirectDrawManual),
 *   2. a window manager (SubstructureRedirect on the root) that answers
 *      MapRequest/ConfigureRequest so X clients can map windows at all,
 *   3. geometry mirroring and WM_DELETE_WINDOW delivery, keyed by the
 *      WL_SURFACE_SERIAL Xwayland sends both to this helper (X client
 *      message) and to the compositor (xwayland_surface_v1.set_serial).
 *
 * This program IS that X client — speaking the X11 wire protocol
 * directly over the unix socket (byte-level, libc only, no Xlib, no
 * xcb), and the xw_window_control_v1 Wayland protocol for the
 * compositor → X11 direction (geometry mirroring, closes).
 *
 * Everything it does is logged; a session with one xterm should print
 * each lifecycle stage (WM established, window mapped, serial, geometry
 * mirrored).
 *
 * Connection-loss triage: the helper lives on exactly two sockets and
 * exits the moment either side is gone. Both gone (X and the
 * compositor) is an orderly stack teardown — info, exit 0. One gone
 * while the other lives is that peer's crash, reported loudly so the
 * session supervisor (which watches Xwayland itself) and the user see
 * the real event. Before this existed the Wayland side was never
 * checked: a dead compositor left the helper spinning on a HUPed fd
 * until Xwayland noticed and died too, misreporting the failure as
 * "X server connection lost".
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xw-window-control-v1.h" /* generated client header */

#define XWM_MAX_WINDOWS 128

/* predefined X atom: CARDINAL (the numeric property type) */
#define XW_CARDINAL 6

/* the window-control manager binding (defined with the Wayland code;
 * the X event handlers send identity updates through it) */
static struct xw_window_control_manager_v1 *wc;
#define XWM_LOG(lvl, ...)                                                      \
    do {                                                                       \
        if (g_verbose || (lvl)[0] == 'e') {                                    \
            fprintf(stderr, "[xw-xwm] %s: ", lvl);                             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

static bool g_verbose = false;

/* ------------------------------------------------------------- X plumbing */

static int x_fd = -1;
static uint32_t x_root = 0;
static uint16_t x_seq = 0; /* server-side numbering: first request = 1 */
static uint32_t x_id_base = 0, x_id_mask = 0;
static uint32_t x_next_id_off = 0x4000; /* ids we allocate (WM window etc.) */
static uint32_t atom_wm_s0, atom_wm_protocols, atom_wm_delete, atom_serial;
static uint32_t atom_wm_name, atom_net_wm_name, atom_wm_class;
static uint32_t atom_wm_state, atom_wm_hints, atom_wm_normal_hints;
static uint32_t atom_wm_take_focus, atom_utf8_string, atom_net_close_window;
static uint32_t atom_net_supported, atom_net_client_list, atom_net_active_window;
static uint32_t atom_net_current_desktop, atom_net_number_of_desktops;
static uint32_t atom_atom, atom_window_type; /* property type atoms */
static uint32_t op_composite = 0;

/* pending reply capture (one at a time; the helper is sequential) */
static uint8_t reply_buf[512];
static bool reply_ready = false;
static uint16_t reply_seq = 0;

struct xw_win {
    uint32_t xid;
    uint64_t serial;
    bool mapped;
    bool managed;  /* passed through MapRequest */
    bool override; /* override-redirect: popup-class, X-owned geometry */
    bool has_name, has_class;
    char name[160];   /* WM_NAME / _NET_WM_NAME, converted to UTF-8 */
    char klass[96];   /* WM_CLASS res_class */
    /* ICCCM WM_NORMAL_HINTS constraints (0 = unset) */
    int32_t min_w, min_h, max_w, max_h, inc_w, inc_h;
    /* input model: WM_PROTOCOLS contains WM_TAKE_FOCUS; WM_HINTS input */
    bool take_focus; /* WM_TAKE_FOCUS listed */
    bool wants_input;  /* WM_HINTS InputHint value (default true) */
    bool has_input_hint;
    /* last geometry WE applied with ConfigureWindow (loop guard: a
     * client that re-requests its own size after our mirror must not
     * make us reconfigure identical geometry forever) */
    int32_t last_x, last_y, last_w, last_h;
    bool have_last_geom;
    /* geometry from CreateNotify/ConfigureNotify (X truth). x/y/w/h
     * are the X11 INTERIOR geometry (root coordinates); bw is the
     * border width — the wl_surface extent Xwayland sizes its
     * buffers to is interior + 2*bw, and the extent origin sits at
     * (x - bw, y - bw). Every conversion between compositor window
     * geometry (extent space) and X11 geometry (interior space) goes
     * through bw. */
    int32_t x, y, w, h;
    int32_t bw;
};

static struct xw_win wins[XWM_MAX_WINDOWS];
static int n_wins = 0;

/* the X-side managed-client list (EWMH _NET_CLIENT_LIST order: map
 * order) and the current focus window, mirroring the compositor's */
static uint32_t client_list[XWM_MAX_WINDOWS];
static int n_client_list = 0;
static uint32_t focused_xid = 0;       /* xid currently holding X focus */
static uint64_t focus_serial_pend = 0; /* compositor focus seen before the
                                          serial association arrived */
static uint32_t last_x_time = 1;       /* newest X timestamp we have seen;
                                          1 = none (never CurrentTime) */

/* compositor geometry that arrived before the X-side serial association
 * (the compositor maps the window the instant the buffer commits; the
 * WL_SURFACE_SERIAL client message reaches us a moment later). Stash the
 * newest geometry per serial and apply it when the association lands. */
struct pending_geom {
    uint64_t serial;
    int32_t x, y, w, h;
};
static struct pending_geom pend[32];
static int n_pend = 0;

static void pend_store(uint64_t serial, int32_t x, int32_t y, int32_t w,
                       int32_t h) {
    for (int i = 0; i < n_pend; i++) {
        if (pend[i].serial == serial) {
            pend[i].x = x;
            pend[i].y = y;
            pend[i].w = w;
            pend[i].h = h;
            return;
        }
    }
    if (n_pend < 32) {
        pend[n_pend].serial = serial;
        pend[n_pend].x = x;
        pend[n_pend].y = y;
        pend[n_pend].w = w;
        pend[n_pend].h = h;
        n_pend++;
    }
}

static bool pend_take(uint64_t serial, struct pending_geom *out) {
    for (int i = 0; i < n_pend; i++) {
        if (pend[i].serial == serial) {
            *out = pend[i];
            pend[i] = pend[--n_pend];
            return true;
        }
    }
    return false;
}

static void die(const char *why) {
    fprintf(stderr, "[xw-xwm] fatal: %s (%s)\n", why, strerror(errno));
    exit(1);
}

/* ---- little-endian buffer helpers ---- */
static void put8(uint8_t *b, size_t *o, uint8_t v) { b[(*o)++] = v; }
static void put16(uint8_t *b, size_t *o, uint16_t v) {
    b[(*o)++] = v & 0xff;
    b[(*o)++] = (v >> 8) & 0xff;
}
static void put32(uint8_t *b, size_t *o, uint32_t v) {
    b[(*o)++] = v & 0xff;
    b[(*o)++] = (v >> 8) & 0xff;
    b[(*o)++] = (v >> 16) & 0xff;
    b[(*o)++] = (v >> 24) & 0xff;
}
static uint16_t get16(const uint8_t *b, size_t o) {
    return (uint16_t)(b[o] | (b[o + 1] << 8));
}
static uint32_t get32(const uint8_t *b, size_t o) {
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
           ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static size_t pad4(size_t n) { return (n + 3) & ~3u; }

static void x_write(const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(x_fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("X connection write failed");
        }
        p += n;
        len -= (size_t)n;
    }
}

/* read exactly n bytes (blocking) */
static void x_read(void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = read(x_fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            die("X connection read failed");
        }
        if (r == 0)
            die("X server closed the connection");
        p += r;
        n -= (size_t)r;
    }
}

/* process one 32-byte unit; returns false when the connection died */
static bool x_process_unit(uint8_t *unit) {
    if (unit[0] == 0) {
        /* error: log and continue (most are benign under a WM) */
        uint8_t code = unit[1];
        uint16_t seq = get16(unit, 2);
        uint8_t major = unit[10];
        XWM_LOG("warn", "X error %u on request %u (seq %u, bad value 0x%x)",
                code, major, seq, get32(unit, 4));
        return true;
    }
    if (unit[0] == 1) { /* reply */
        uint16_t seq = get16(unit, 2);
        uint32_t extra = get32(unit, 4);
        memcpy(reply_buf, unit, 32);
        if (extra > 0) {
            /* read the value into the buffer (capped), then drain only
             * the REMAINDER — the original code re-read extra*4 bytes
             * after already consuming them, deadlocking the helper on
             * the first reply that carried data (every GetProperty of
             * a non-empty property) */
            size_t total = (size_t)extra * 4;
            size_t want = total;
            if (want > sizeof(reply_buf) - 32)
                want = sizeof(reply_buf) - 32;
            x_read((uint8_t *)reply_buf + 32, want);
            size_t rest = total - want;
            while (rest > 0) {
                uint8_t sink[256];
                size_t chunk = rest > sizeof(sink) ? sizeof(sink) : rest;
                x_read(sink, chunk);
                rest -= chunk;
            }
        }
        reply_ready = true;
        reply_seq = seq;
        return true;
    }
    /* an event: hand to the event sink */
    if (g_verbose && unit[0] != 31 && unit[0] > 1) {
        /* raw wire dump (XWM_WIRE=1): the ground truth for every
         * event-field offset question — Xwayland's bytes ARE the
         * reference, specs have mislead this file twice */
        static int dump_on = -1;
        if (dump_on < 0)
            dump_on = getenv("XWM_WIRE") && atoi(getenv("XWM_WIRE"));
        if (dump_on) {
            fprintf(stderr, "[xw-xwm] wire: ev%u:", unit[0] & 0x7f);
            for (int i = 0; i < 32; i++)
                fprintf(stderr, " %02x", unit[i]);
            fprintf(stderr, "\n");
        }
    }
    extern void xwm_handle_event(const uint8_t *ev);
    xwm_handle_event(unit);
    return true;
}

/* flush pending input; process everything currently readable */
static void x_drain(void) {
    uint8_t unit[32];
    for (;;) {
        struct pollfd pfd = {.fd = x_fd, .events = POLLIN};
        if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLIN))
            return;
        x_read(unit, 32);
        if (g_verbose)
            fprintf(stderr, "[xw-xwm] debug: event/reply code %u\n",
                    unit[0]);
        if (!x_process_unit(unit))
            return;
    }
}

/* send a request; remember its sequence number */
static uint16_t x_send(const void *buf, size_t len) {
    x_seq++;
    if (g_verbose) {
        const uint8_t *b = buf;
        fprintf(stderr, "[xw-xwm] trace: SEND seq=%u op=%u len_field=%u "
                "bytes=%zu\n", x_seq, b[0], (unsigned)get16(b, 2), len);
    }
    x_write(buf, len);
    return x_seq;
}

/* wait for the reply to seq `want`, processing events while waiting */
static const uint8_t *x_wait_reply(uint16_t want) {
    while (!reply_ready || reply_seq != want) {
        uint8_t unit[32];
        x_read(unit, 32);
        if (g_verbose)
            fprintf(stderr,
                    "[xw-xwm] debug: unit code %u (want reply seq %u)\n",
                    unit[0], want);
        if (!x_process_unit(unit))
            die("X connection lost waiting for a reply");
    }
    reply_ready = false;
    return reply_buf;
}

/* ---- connection setup ---- */
static void x_connect(const char *display, const char *auth_data,
                      size_t auth_len) {
    /* display like ":5" or ":5.0" → /tmp/.X11-unix/X5 */
    const char *num = display;
    if (*num == ':')
        num++;
    char sockpath[64];
    snprintf(sockpath, sizeof(sockpath), "/tmp/.X11-unix/X%.12s", num);
    char *dot = strchr(sockpath + strlen("/tmp/.X11-unix/X"), '.');
    if (dot)
        *dot = 0;

    x_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (x_fd < 0)
        die("socket()");
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sockpath);
    if (connect(x_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        die(sockpath);

    /* setup request: byte order 'l', v11, auth MIT-MAGIC-COOKIE-1 */
    uint8_t req[128];
    size_t o = 0;
    put8(req, &o, 'l');
    put8(req, &o, 0);
    put16(req, &o, 11);
    put16(req, &o, 0);
    const char *auth_name = "MIT-MAGIC-COOKIE-1";
    if (auth_data && auth_len == 16) {
        put16(req, &o, (uint16_t)strlen(auth_name));
        put16(req, &o, (uint16_t)auth_len);
    } else {
        put16(req, &o, 0);
        put16(req, &o, 0);
    }
    put16(req, &o, 0);
    if (auth_data && auth_len == 16) {
        memcpy(req + o, auth_name, strlen(auth_name));
        o = pad4(o + strlen(auth_name));
        memcpy(req + o, auth_data, auth_len);
        o = pad4(o + auth_len);
    }
    x_write(req, o);

    /* setup reply */
    uint8_t head[8];
    x_read(head, 8);
    if (g_verbose) {
        fprintf(stderr, "[xw-xwm] debug: setup reply head:");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, " %02x", head[i]);
        fprintf(stderr, "\n");
    }
    if (head[0] != 1) {
        fprintf(stderr, "[xw-xwm] fatal: X setup rejected (code %u)\n",
                head[0]);
        exit(1);
    }
    uint32_t extra = get16(head, 6); /* length in 4-byte units (16-bit!) */
    uint8_t body[4096];
    size_t want = extra * 4 > sizeof(body) ? sizeof(body) : extra * 4;
    x_read(body, want);
    if (extra * 4 > want) { /* drain */
        uint8_t sink[512];
        size_t rest = extra * 4 - want;
        while (rest) {
            size_t c = rest > sizeof(sink) ? sizeof(sink) : rest;
            x_read(sink, c);
            rest -= c;
        }
    }
    /* setup reply "additional data" (xcb_setup layout):
     *   [4 release][4 id-base][4 id-mask][4 motion-buffer]
     *   [2 vendor_len][2 max_req][1 n_screens][1 n_formats]
     *   [4 bitmap/keycode bytes + 2 pad + 2 pad]  = 32 bytes
     *   then vendor (padded), FORMATs (8 each), SCREENs. The root
     * window is the first 4 bytes of the first screen. */
    uint8_t n_screens = body[20];
    uint8_t n_formats = body[21];
    uint16_t vendor_len = get16(body, 16);
    x_id_base = get32(body, 4);
    x_id_mask = get32(body, 8);
    size_t bo = 32 + pad4(vendor_len) + (size_t)n_formats * 8;
    if (bo + 4 > want) {
        fprintf(stderr, "[xw-xwm] fatal: setup reply truncated\n");
        exit(1);
    }
    x_root = get32(body, bo);
    if (n_screens < 1 || !x_root)
        die("X setup: no usable screen");
    XWM_LOG("info", "connected to %s, root window 0x%x, %u screen(s)",
            display, x_root, n_screens);
}

/* ---- requests ---- */
static uint32_t x_intern_atom(const char *name) {
    uint8_t req[64];
    size_t o = 0;
    uint16_t nlen = (uint16_t)strlen(name);
    put8(req, &o, 16); /* InternAtom */
    put8(req, &o, 0);  /* only-if-exists = false */
    put16(req, &o, (uint16_t)(pad4(8 + nlen) / 4));
    put16(req, &o, nlen);
    put16(req, &o, 0);
    memcpy(req + o, name, nlen);
    o = pad4(o + nlen);
    uint16_t seq = x_send(req, o);
    const uint8_t *rep = x_wait_reply(seq);
    return get32(rep, 8);
}

static bool x_query_extension(const char *name, uint8_t *opcode) {
    uint8_t req[64];
    size_t o = 0;
    uint16_t nlen = (uint16_t)strlen(name);
    put8(req, &o, 98); /* QueryExtension */
    put8(req, &o, 0);
    put16(req, &o, (uint16_t)(pad4(8 + nlen) / 4));
    put16(req, &o, nlen);
    put16(req, &o, 0);
    memcpy(req + o, name, nlen);
    o = pad4(o + nlen);
    uint16_t seq = x_send(req, o);
    const uint8_t *rep = x_wait_reply(seq);
    if (g_verbose) {
        fprintf(stderr, "[xw-xwm] debug: QueryExtension(\"%s\") reply:", name);
        for (int i = 0; i < 16; i++)
            fprintf(stderr, " %02x", rep[i]);
        fprintf(stderr, "\n");
    }
    if (rep[8] == 0)
        return false;
    *opcode = rep[9];
    return true;
}

static void x_change_attributes(uint32_t window, uint32_t mask,
                                uint32_t value) {
    uint8_t req[16];
    size_t o = 0;
    put8(req, &o, 2);
    put8(req, &o, 0);
    put16(req, &o, 4);
    put32(req, &o, window);
    put32(req, &o, mask);
    put32(req, &o, value);
    x_send(req, o);
}

/* a fresh, guaranteed-valid resource id (from the setup's id range) */
static uint32_t x_new_id(void) {
    x_next_id_off += 0x100;
    return x_id_base + (x_next_id_off & x_id_mask);
}

static uint32_t x_create_wm_window(void) {
    uint32_t wid = x_new_id();
    uint8_t req[32];
    size_t o = 0;
    put8(req, &o, 1);  /* CreateWindow */
    put8(req, &o, 0);  /* depth: copy from parent */
    put16(req, &o, 8); /* length: 32 bytes */
    put32(req, &o, wid);
    put32(req, &o, x_root);
    put16(req, &o, 0); /* x */
    put16(req, &o, 0); /* y */
    put16(req, &o, 1); /* width */
    put16(req, &o, 1); /* height */
    put16(req, &o, 0); /* border width */
    put16(req, &o, 2); /* class: InputOnly */
    put32(req, &o, 0); /* visual: CopyFromParent */
    put32(req, &o, 0); /* value mask: none */
    x_send(req, o);
    return wid;
}

static void x_set_selection_owner(uint32_t owner, uint32_t selection) {
    uint8_t req[16];
    size_t o = 0;
    put8(req, &o, 22);
    put8(req, &o, 0);
    put16(req, &o, 4); /* 16 bytes = 4 words */
    put32(req, &o, owner);
    put32(req, &o, selection);
    put32(req, &o, 0); /* CurrentTime */
    x_send(req, o);
}

static void x_map_window(uint32_t window) {
    uint8_t req[8];
    size_t o = 0;
    put8(req, &o, 8);
    put8(req, &o, 0);
    put16(req, &o, 2);
    put32(req, &o, window);
    x_send(req, o);
}

static void x_configure_window(uint32_t window, int x, int y, int w, int h) {
    /* mask: x|y|w|h = 0x0F */
    uint8_t req[32];
    size_t o = 0;
    put8(req, &o, 12);
    put8(req, &o, 0);
    put16(req, &o, 7);
    put32(req, &o, window);
    put16(req, &o, 0x0F);
    put16(req, &o, 0); /* pad to 4 */
    put32(req, &o, (uint32_t)(int32_t)x);
    put32(req, &o, (uint32_t)(int32_t)y);
    put32(req, &o, (uint32_t)(int32_t)w);
    put32(req, &o, (uint32_t)(int32_t)h);
    x_send(req, o);
}

/* forward declarations (definitions live with the property code) */
static bool x_window_protocol_has(uint32_t window, uint32_t protocol);
static int32_t x_get_property(uint32_t window, uint32_t property,
                              uint32_t type, uint32_t max_items);

static void x_destroy_window(uint32_t window) {
    uint8_t req[8];
    size_t o = 0;
    put8(req, &o, 4);
    put8(req, &o, 0);
    put16(req, &o, 2);
    put32(req, &o, window);
    x_send(req, o);
}

static void x_kill_client(uint32_t resource) {
    uint8_t req[8];
    size_t o = 0;
    put8(req, &o, 113);
    put8(req, &o, 0);
    put16(req, &o, 2);
    put32(req, &o, resource);
    x_send(req, o);
}

/* does the window list WM_PROTOCOLS containing WM_DELETE_WINDOW? */
static bool x_window_supports_wm_delete(uint32_t window) {
    return x_window_protocol_has(window, atom_wm_delete);
}

static void x_send_wm_delete(uint32_t window) {
    /* SendEvent: [25][propagate=0][len=11][4 destination][4 mask=0]
     *   + 32-byte ClientMessage event:
     *   [33][format=32|0x80][2 seq][4 window][4 WM_PROTOCOLS]
     *   [20 data: WM_DELETE_WINDOW, 0, 0, 0, 0] */
    uint8_t req[44];
    size_t o = 0;
    put8(req, &o, 25); /* SendEvent */
    put8(req, &o, 0);  /* propagate = false */
    put16(req, &o, 11);
    put32(req, &o, window); /* destination */
    put32(req, &o, 0);      /* event mask 0 = to the client itself */
    put8(req, &o, 33);           /* ClientMessage */
    put8(req, &o, 32);            /* format 32. NOT 32|0x80: the
                                    * "this event was generated by
                                    * SendEvent" bit lives on the event
                                    * CODE byte of DELIVERED events and
                                    * is set by the server; setting it
                                    * on the format byte here makes the
                                    * format 160, every SendEvent dies
                                    * with BadValue(0xa0) and the client
                                    * never sees WM_DELETE_WINDOW. */
    put16(req, &o, 0);
    put32(req, &o, window);
    put32(req, &o, atom_wm_protocols);
    put32(req, &o, atom_wm_delete); /* data.l[0] */
    put32(req, &o, 0);
    put32(req, &o, 0);
    put32(req, &o, 0);
    put32(req, &o, 0);
    x_send(req, o);
}

static void x_composite_redirect_subwindows(void) {
    uint8_t req[16];
    size_t o = 0;
    put8(req, &o, op_composite);
    put8(req, &o, 2); /* RedirectSubwindows */
    put16(req, &o, 3);
    put32(req, &o, x_root);
    put32(req, &o, 1); /* Manual */
    x_send(req, o);
}

/* -------------------------------------------------- properties and EWMH */

/* ChangeProperty, 32-bit format, mode Replace. Used for every EWMH
 * root property and for WM_STATE on managed windows. Request
 * layout: [9][mode][len][window][property][type][fmt][pad3][n][data] */
static void x_change_property_32(uint32_t window, uint32_t property,
                                 uint32_t type, uint32_t nitems,
                                 const uint32_t *values) {
    size_t len = 24 + (size_t)nitems * 4;
    uint8_t req[64 + XWM_MAX_WINDOWS * 4];
    size_t o = 0;
    put8(req, &o, 18); /* ChangeProperty (9 is MapSubwindows!) */
    put8(req, &o, 0);  /* mode: Replace */
    put16(req, &o, (uint16_t)(len / 4));
    put32(req, &o, window);
    put32(req, &o, property);
    put32(req, &o, type);
    put8(req, &o, 32); /* format */
    put8(req, &o, 0);
    put16(req, &o, 0);
    put32(req, &o, nitems);
    for (uint32_t i = 0; i < nitems; i++)
        put32(req, &o, values[i]);
    x_send(req, o);
}

/* GetProperty into prop_val (bounded). Returns the number of VALUE
 * BYTES (works for both 8- and 32-bit formats) or -1 when the
 * property is absent/malformed. type 0 means AnyPropertyType.
 *
 * Reply layout: [1]=format, [2..3]=seq, [4..7]=extra length,
 * [8..11]=type, [12..15]=bytes-after, [16..19]=value length (in
 * format units), [20..31]=unused, [32..]=value. (The old inline
 * code in x_window_supports_wm_delete read the format byte at 20 —
 * inside the unused pad — so WM_DELETE support was never detected
 * and every close took the destroy path.) */
static uint8_t prop_val[400];
static int32_t x_get_property(uint32_t window, uint32_t property,
                              uint32_t type, uint32_t max_items) {
    uint8_t req[24];
    size_t o = 0;
    put8(req, &o, 20); /* GetProperty */
    put8(req, &o, 0);  /* delete = false */
    put16(req, &o, 6);
    put32(req, &o, window);
    put32(req, &o, property);
    put32(req, &o, type);
    put32(req, &o, 0); /* offset */
    put32(req, &o, max_items);
    uint16_t seq = x_send(req, o);
    const uint8_t *rep = x_wait_reply(seq);
    if (rep[8] == 0) /* type None: property does not exist */
        return -1;
    uint8_t fmt = rep[1];
    uint32_t nitems = get32(rep, 16);
    if (fmt != 32 && fmt != 8)
        return -1;
    if (nitems == 0)
        return 0;
    size_t nbytes = fmt == 32 ? (size_t)nitems * 4 : nitems;
    if (nbytes > sizeof(prop_val))
        return -1;
    memcpy(prop_val, reply_buf + 32, nbytes);
    return (int32_t)nbytes;
}

/* does WM_PROTOCOLS on the window list the given atom? */
static bool x_window_protocol_has(uint32_t window, uint32_t protocol) {
    int32_t n = x_get_property(window, atom_wm_protocols, 0, 64);
    if (n <= 0 || n % 4 != 0) /* must be 32-bit atoms */
        return false;
    for (int32_t i = 0; i + 4 <= n; i += 4)
        if (get32(prop_val, (size_t)i) == protocol)
            return true;
    return false;
}

/* read the window name: _NET_WM_NAME (UTF8_STRING) preferred, else
 * WM_NAME (STRING, Latin-1 → UTF-8). out is NUL-terminated. */
static bool x_read_window_name(uint32_t window, char *out, size_t outsz) {
    int32_t n = -1;
    if (atom_net_wm_name)
        n = x_get_property(window, atom_net_wm_name, atom_utf8_string,
                           sizeof(prop_val));
    if (n > 0) {
        size_t len = (size_t)n < outsz - 1 ? (size_t)n : outsz - 1;
        memcpy(out, prop_val, len);
        out[len] = 0;
        /* sanity: reject embedded NULs (a broken client) */
        if (memchr(out, 0, len))
            return false;
        return true;
    }
    n = x_get_property(window, atom_wm_name, 0, sizeof(prop_val));
    if (n <= 0)
        return false;
    /* X STRING is Latin-1: convert to UTF-8 */
    size_t oi = 0;
    for (int32_t i = 0; i < n && oi + 2 < outsz; i++) {
        uint8_t ch = prop_val[i];
        if (ch < 0x80) {
            if (ch == 0)
                break; /* the property is a list of strings; take the first */
            out[oi++] = (char)ch;
        } else {
            out[oi++] = (char)(0xC0 | (ch >> 6));
            out[oi++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[oi] = 0;
    return oi > 0;
}

/* read WM_CLASS res_class (the second NUL-terminated string) */
static bool x_read_window_class(uint32_t window, char *out, size_t outsz) {
    int32_t n = x_get_property(window, atom_wm_class, 0, 200);
    if (n <= 0)
        return false;
    /* res_name is first; skip it, then copy res_class */
    size_t i = 0;
    while (i < (size_t)n && prop_val[i])
        i++;
    i++; /* NUL */
    if (i >= (size_t)n)
        return false;
    size_t len = 0;
    while (i + len < (size_t)n && prop_val[i + len] && len < outsz - 1)
        len++;
    if (len == 0)
        return false;
    memcpy(out, prop_val + i, len);
    out[len] = 0;
    return true;
}

/* WM_HINTS: extract InputHint (the ICCCM input model). The input
 * field is the FIRST field after the flags word, so when InputHint
 * is flagged it sits at byte offset 4. */
static bool x_read_wm_hints(uint32_t window, bool *input_out) {
    int32_t n = x_get_property(window, atom_wm_hints, 0, 24);
    if (n < 8)
        return false; /* no WM_HINTS, or too short to carry input */
    uint32_t flags = get32(prop_val, 0);
    if (!(flags & 0x1)) /* InputHint not set */
        return false;
    *input_out = get32(prop_val, 4) != 0;
    return true;
}

/* WM_NORMAL_HINTS: min/max size and resize increments (PMinSize=0x10,
 * PMaxSize=0x20, PResizeInc=0x40, PBaseSize=0x100). */
static bool x_read_normal_hints(struct xw_win *w) {
    int32_t n = x_get_property(w->xid, atom_wm_normal_hints, 0, 24);
    if (n < 4)
        return false;
    uint32_t flags = get32(prop_val, 0);
    w->min_w = w->min_h = w->max_w = w->max_h = w->inc_w = w->inc_h = 0;
    /* walk the optional field groups in wire order */
    size_t off = 4;
    if (flags & (0x1 | 0x4)) off += 8; /* x, y */
    if (flags & (0x2 | 0x8)) off += 8; /* width, height */
    if (flags & 0x10) { /* PMinSize */
        if (off + 8 > (size_t)n) return false;
        w->min_w = (int32_t)get32(prop_val, off);
        w->min_h = (int32_t)get32(prop_val, off + 4);
        off += 8;
    }
    if (flags & 0x20) { /* PMaxSize */
        if (off + 8 > (size_t)n) return false;
        w->max_w = (int32_t)get32(prop_val, off);
        w->max_h = (int32_t)get32(prop_val, off + 4);
        off += 8;
    }
    if (flags & 0x40) { /* PResizeInc */
        if (off + 8 > (size_t)n) return false;
        w->inc_w = (int32_t)get32(prop_val, off);
        w->inc_h = (int32_t)get32(prop_val, off + 4);
        off += 8;
    }
    return true;
}

/* ICCCM WM_STATE on a managed window: Normal=1, Iconic=3, Withdrawn=0 */
static void x_set_wm_state(uint32_t window, uint32_t state) {
    uint32_t v[2] = {state, 0};
    x_change_property_32(window, atom_wm_state, atom_wm_state, 2, v);
}

/* SendEvent helper: a 32-bit ClientMessage to the window's client */
static void x_send_client_message32(uint32_t window, uint32_t type,
                                    uint32_t l0, uint32_t l1, uint32_t l2,
                                    uint32_t l3, uint32_t l4) {
    uint8_t req[44];
    size_t o = 0;
    put8(req, &o, 25); /* SendEvent */
    put8(req, &o, 0);  /* propagate = false */
    put16(req, &o, 11);
    put32(req, &o, window);
    put32(req, &o, 0); /* event mask 0 = to the client itself */
    put8(req, &o, 33);
    put8(req, &o, 32); /* format 32 — see x_send_wm_delete: no 0x80 */
    put16(req, &o, 0);
    put32(req, &o, window);
    put32(req, &o, type);
    put32(req, &o, l0);
    put32(req, &o, l1);
    put32(req, &o, l2);
    put32(req, &o, l3);
    put32(req, &o, l4);
    x_send(req, o);
}

/* focus routing, the ICCCM way:
 *   - WM_HINTS InputHint false → never SetInputFocus; the WM_TAKE_FOCUS
 *     message is the only focus notification the window accepts
 *   - WM_TAKE_FOCUS listed → deliver the protocol message (with a real
 *     timestamp, never CurrentTime)
 *   - else (passive input) → SetInputFocus directly */
static void x_focus_window(struct xw_win *w) {
    if (!w)
        return;
    focused_xid = w->xid;
    bool input_hint = true;
    if (w->has_input_hint)
        input_hint = w->wants_input;
    if (input_hint) {
        uint8_t req[16];
        size_t o = 0;
        put8(req, &o, 42);
        put8(req, &o, 2); /* revert-to: Parent */
        put16(req, &o, 3);
        put32(req, &o, w->xid);
        put32(req, &o, last_x_time);
        x_send(req, o);
    }
    if (w->take_focus)
        x_send_client_message32(w->xid, atom_wm_protocols,
                                atom_wm_take_focus, last_x_time, 0, 0, 0);
    XWM_LOG("info", "focus: window 0x%x (input=%s take-focus=%s)",
            w->xid, input_hint ? "yes" : "no", w->take_focus ? "yes" : "no");
    if (atom_net_active_window) {
        uint32_t v[1] = {w->xid};
        x_change_property_32(x_root, atom_net_active_window,
                             atom_window_type, 1, v);
    }
}

static void x_focus_release(void) {
    if (focused_xid == 0)
        return;
    focused_xid = 0;
    uint8_t req[16];
    size_t o = 0;
    put8(req, &o, 42);
    put8(req, &o, 0); /* revert-to: None */
    put16(req, &o, 3);
    put32(req, &o, 0); /* focus window None */
    put32(req, &o, last_x_time);
    x_send(req, o);
    if (atom_net_active_window) {
        uint32_t v[1] = {0};
        x_change_property_32(x_root, atom_net_active_window,
                             atom_window_type, 1, v);
    }
    XWM_LOG("info", "focus: released (no X window has compositor focus)");
}

/* --------------------------------------------------------- window table */

static struct xw_win *win_find_xid(uint32_t xid) {
    for (int i = 0; i < n_wins; i++)
        if (wins[i].xid == xid)
            return &wins[i];
    return NULL;
}

static struct xw_win *win_find_serial(uint64_t serial) {
    for (int i = 0; i < n_wins; i++)
        if (wins[i].serial == serial && serial)
            return &wins[i];
    return NULL;
}

static struct xw_win *win_track(uint32_t xid) {
    struct xw_win *w = win_find_xid(xid);
    if (w)
        return w;
    if (n_wins >= XWM_MAX_WINDOWS)
        return NULL;
    w = &wins[n_wins++];
    memset(w, 0, sizeof(*w));
    w->xid = xid;
    return w;
}

static void win_forget(uint32_t xid) {
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
        /* extent space: Xwayland's surface covers interior + border */
        xw_window_control_manager_v1_set_override_redirect(
            wc, (uint32_t)(w->serial >> 32), (uint32_t)w->serial,
            w->x - w->bw, w->y - w->bw, w->w + 2 * w->bw, w->h + 2 * w->bw);
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
            /* wire layout: [16][pad][seq][parent @4][window @8]
             * [x i16 @12][y i16 @14][w u16 @16][h u16 @18]
             * [border u16 @20][override-redirect BOOL @22][pad].
             * The OR byte is at 22 — the previous code read 24 (padding)
             * and OR windows were never classified. */
            w->override = ev[22] != 0;
            w->x = (int16_t)get16(ev, 12);
            w->y = (int16_t)get16(ev, 14);
            w->w = get16(ev, 16);
            w->h = get16(ev, 18);
            w->bw = get16(ev, 20);
            if (w->override)
                XWM_LOG("info", "CreateNotify: 0x%x override-redirect "
                        "%dx%d+%d+%d border %d (popup-class)", window,
                        w->w, w->h, w->x, w->y, w->bw);
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
        /* PropertyNotify on the CLIENT window needs the WM's own
         * selection on that window (per-client, per-window masks —
         * this cannot clobber the client's own selections). Without
         * it, terminals retitle themselves and the taskbar never
         * hears about it: the v0 helper never saw a single
         * PropertyNotify. */
        x_change_attributes(window, 0x800 /* CWEventMask */,
                            0x400000u /* PropertyChangeMask (1<<22) */);
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
         * Event layout (xConfigureRequest event, NOT the request):
         * [23][stack-mode][seq][parent][window][sibling]
         * [x i16 @16][y i16 @18][w u16 @20][h u16 @22][border @24]
         * [value-mask u16 @26][pad]. The inline x/y/w/h carry the full
         * proposed geometry (server-filled for unspecified fields);
         * the mask marks which the client actually asked to change.
         * The v0 parser read w/h from the border/mask slots, and the
         * first fix misread the mask from the x/y slots — both made
         * xterm-sized windows come out 1x1 / 3x14. */
        uint32_t window = get32(ev, 8);
        uint16_t mask = get16(ev, 26);
        int32_t x = (int16_t)get16(ev, 16);
        int32_t y = (int16_t)get16(ev, 18);
        int32_t wq = get16(ev, 20);
        int32_t hq = get16(ev, 22);
        bool have_x = mask & 0x1, have_y = mask & 0x2;
        bool have_w = mask & 0x4, have_h = mask & 0x8;
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
                    /* extent space: Xwayland's surface covers the
                     * border too */
                    xw_window_control_manager_v1_set_override_redirect(
                        wc, (uint32_t)(wi->serial >> 32),
                        (uint32_t)wi->serial, wi->x - wi->bw, wi->y - wi->bw,
                        wi->w + 2 * wi->bw, wi->h + 2 * wi->bw);
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
        /* the granted resize is X truth the compositor must adopt:
         * push it as the window's EXTENT (Xwayland only sends a new
         * surface buffer when the client draws — an undrawn resize
         * would otherwise leave the compositor's model stale, and
         * the taskbar/snap geometry wrong). Not echoed back: the X
         * side already has this state. */
        if (wi->serial && wc) {
            int32_t bwx = wi->bw > 0 ? wi->bw : 0;
            xw_window_control_manager_v1_set_geometry(
                wc, (uint32_t)(wi->serial >> 32), (uint32_t)wi->serial,
                px - bwx, py - bwx, gw + 2 * bwx, gh + 2 * bwx);
        }
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
                    /* pendings are extent-space (compositor coords) */
                    int32_t pbw = w->bw > 0 ? w->bw : 0;
                    int32_t piw = pg.w - 2 * pbw, pih = pg.h - 2 * pbw;
                    if (piw < 1) piw = 1;
                    if (pih < 1) pih = 1;
                    XWM_LOG("info",
                            "  applying pending geometry %dx%d+%d+%d "
                            "(interior %dx%d)",
                            pg.w, pg.h, pg.x, pg.y, piw, pih);
                    x_configure_window(window, pg.x + pbw, pg.y + pbw, piw,
                                       pih);
                    w->have_last_geom = true;
                    w->last_x = pg.x + pbw;
                    w->last_y = pg.y + pbw;
                    w->last_w = piw;
                    w->last_h = pih;
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
         * their own moves here - e.g. a tooltip following the pointer).
         * Wire layout matches CreateNotify from x on: window @8,
         * x i16 @12? no — event-window @4, window @8, x @16? see
         * below; override-redirect BOOL @22 (NOT 25: that byte is
         * padding, reading it never classified OR moves). */
        uint32_t window = get32(ev, 8);
        struct xw_win *w = win_find_xid(window);
        if (!w)
            break;
        w->x = (int16_t)get16(ev, 16);
        w->y = (int16_t)get16(ev, 18);
        w->w = get16(ev, 20);
        w->h = get16(ev, 22);
        w->bw = get16(ev, 24);
        w->override = ev[26] != 0;
        if (w->override && w->serial && wc) {
            XWM_LOG("info", "override-redirect 0x%x moved to %dx%d+%d+%d",
                    window, w->w, w->h, w->x, w->y);
            /* extent space: Xwayland's surface covers interior+border */
            xw_window_control_manager_v1_set_override_redirect(
                wc, (uint32_t)(w->serial >> 32), (uint32_t)w->serial,
                w->x - w->bw, w->y - w->bw, w->w + 2 * w->bw,
                w->h + 2 * w->bw);
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

/* -------------------------------------------------- Wayland control side */

static struct wl_display *wl_dpy = NULL;

static void wc_geometry(void *data,
                        struct xw_window_control_manager_v1 *mgr,
                        uint32_t serial_hi, uint32_t serial_lo, int32_t x,
                        int32_t y, int32_t width, int32_t height) {
    (void)data;
    (void)mgr;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    struct xw_win *w = win_find_serial(serial);
    if (!w) {
        XWM_LOG("info",
                "geometry for not-yet-associated serial %llu - stashing",
                (unsigned long long)serial);
        pend_store(serial, x, y, width, height);
        return;
    }
    XWM_LOG("info", "mirroring geometry: serial %llu -> 0x%x %dx%d+%d+%d",
            (unsigned long long)serial, w->xid, width, height, x, y);
    if (w->override) {
        /* override-redirect geometry is X-owned; the compositor has
         * nothing authoritative to say about it */
        return;
    }
    /* The compositor speaks EXTENT (the wl_surface covers the X11
     * window including its border); ConfigureWindow speaks INTERIOR.
     * Converting naively (as v0 did) reconfigures the window to its
     * own extent, the new extent lands as a bigger surface, the
     * compositor mirrors back, and the window grows 2*border per
     * round — the border ratchet. interior = extent + bw, because
     * the X11 interior origin sits bw pixels INSIDE the extent. */
    int32_t bw = w->bw > 0 ? w->bw : 0;
    int32_t ix = x + bw, iy = y + bw;
    int32_t iw = width - 2 * bw, ih = height - 2 * bw;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    if (w->have_last_geom && w->last_x == ix && w->last_y == iy &&
        w->last_w == iw && w->last_h == ih)
        return; /* already there: a no-op reconfigure would ping-pong */
    x_configure_window(w->xid, ix, iy, iw, ih);
    w->have_last_geom = true;
    w->last_x = ix;
    w->last_y = iy;
    w->last_w = iw;
    w->last_h = ih;
}

static void wc_close(void *data, struct xw_window_control_manager_v1 *mgr,
                     uint32_t serial_hi, uint32_t serial_lo) {
    (void)data;
    (void)mgr;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    struct xw_win *w = win_find_serial(serial);
    if (!w) {
        XWM_LOG("warn", "close for unknown serial %llu — killing the "
                        "resource as a fallback",
                (unsigned long long)serial);
        x_kill_client(serial ? (uint32_t)serial : 0);
        return;
    }
    XWM_LOG("info", "close: serial %llu -> window 0x%x",
            (unsigned long long)serial, w->xid);
    win_close(w);
}

static void wc_focus(void *data, struct xw_window_control_manager_v1 *mgr,
                     uint32_t serial_hi, uint32_t serial_lo) {
    (void)data;
    (void)mgr;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    focus_apply(serial);
}

static const struct xw_window_control_manager_v1_listener wc_listener = {
    .geometry = wc_geometry,
    .close = wc_close,
    .focus = wc_focus,
};

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, "xw_window_control_manager_v1") == 0) {
        if (version < 2)
            XWM_LOG("warn",
                    "compositor offers window-control v%u (need 2 for "
                    "identity/focus) — binding anyway", version);
        wc = wl_registry_bind(
            r, name, &xw_window_control_manager_v1_interface,
            version >= 2 ? 2 : version);
        xw_window_control_manager_v1_add_listener(wc, &wc_listener, NULL);
        XWM_LOG("info", "window-control manager bound");
    }
}

static void registry_global_remove(void *data, struct wl_registry *r,
                                   uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------- auth file */

/* the session writes: family=WILD, empty address, empty number, name
 * "MIT-MAGIC-COOKIE-1", 16 data bytes — a single cookie for every
 * local connection. Parse it back out for the setup handshake. */
static bool load_auth_cookie(const char *path, uint8_t out[16]) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = false;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) == 12) {
        size_t o = 0;
        (void)get16(hdr, o); /* family */
        o = 2;
        uint16_t addr_len = get16(hdr, o);
        o = 4;
        uint16_t num_len = get16(hdr, o);
        o = 6;
        uint16_t name_len = get16(hdr, o);
        o = 8;
        uint16_t data_len = get16(hdr, o);
        if (fseek(f, 12 + pad4(addr_len) + pad4(num_len) + pad4(name_len),
                  SEEK_SET) == 0 &&
            data_len == 16 && fread(out, 1, 16, f) == 16)
            ok = true;
    }
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ main */

static void usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n"
           "\n"
           "The X11 window manager for Xwayland (xfce4-wayland session).\n"
           "Speaks the X wire protocol directly (no Xlib) and mirrors\n"
           "compositor geometry via the xw_window_control_v1 protocol.\n"
           "\n"
           "Options:\n"
           "  -d, --display NAME      X display to manage (e.g. :5)\n"
           "  -a, --auth-file PATH    MIT-MAGIC-COOKIE-1 authority file\n"
           "  -w, --wayland NAME      WAYLAND_DISPLAY of xw-compositor\n"
           "  -v, --verbose           log every event\n"
           "  -h, --help              this help\n",
           prog);
}

/* ------------------------------------------------------- loss triage */

/* Is the peer on fd dead RIGHT NOW (0-timeout poll)? POLLHUP/ERR/NVAL
 * on a stream socket is direct kernel evidence; it is the same signal
 * the main loop reacts to, asked ad-hoc when the OTHER side died first
 * and we need to tell "stack teardown" from "one peer crashed". */
static bool peer_gone(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (fd < 0)
        return true;
    if (poll(&p, 1, 0) < 0)
        return true; /* cannot even ask — treat as gone */
    return (p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

/* The compositor side died (poll flags, dispatch or flush error).
 * Distinguish the two meanings before dying: X also gone = teardown;
 * X alive = the compositor itself crashed. A wl PROTOCOL error is
 * reported as such — it is a compositor bug, not a lost socket. */
static void wl_lost(void) {
    int err = wl_display_get_error(wl_dpy);
    uint32_t pcode = 0, pop = 0;
    const struct wl_interface *pi = NULL;
    /* wl_display_get_protocol_error is only meaningful when
     * wl_display_get_error returned EPROTO; ask unconditionally and
     * report what it finds, if anything — a non-zero protocol code is
     * a compositor bug, not a lost socket */
    pcode = wl_display_get_protocol_error(wl_dpy, &pi, &pop);
    if (peer_gone(x_fd)) {
        XWM_LOG("info", "X stack teardown: compositor connection gone "
                "(%s) and the X server is too — exiting",
                err ? strerror(err) : "socket closed by the peer");
        exit(0);
    }
    if (pcode) {
        fprintf(stderr,
                "[xw-xwm] fatal: compositor protocol error %u on "
                "%s opcode %u (X server still up) — this is a "
                "compositor bug, please report it\n",
                pcode, pi ? pi->name : "(unknown interface)", pop);
        exit(1);
    }
    fprintf(stderr,
            "[xw-xwm] fatal: compositor connection lost (%s; the X "
            "server is still up)\n",
            err ? strerror(err) : "socket closed by the peer");
    exit(1);
}

/* ------------------------------------------------------------ main loop */

int main(int argc, char **argv) {
    const char *display = getenv("DISPLAY");
    const char *auth_file = NULL;
    const char *wl_name = getenv("WAYLAND_DISPLAY");

    static const struct option longopts[] = {
        {"display", required_argument, NULL, 'd'},
        {"auth-file", required_argument, NULL, 'a'},
        {"wayland", required_argument, NULL, 'w'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "d:a:w:vh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'd': display = optarg; break;
        case 'a': auth_file = optarg; break;
        case 'w': wl_name = optarg; break;
        case 'v': g_verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }
    if (!display || !*display) {
        fprintf(stderr, "[xw-xwm] fatal: no DISPLAY (use -d or $DISPLAY)\n");
        return 1;
    }

    uint8_t cookie[16];
    const uint8_t *auth = NULL;
    if (auth_file && load_auth_cookie(auth_file, cookie))
        auth = cookie;

    x_connect(display, (const char *)auth, auth ? 16 : 0);

    /* 1. become the window manager */
    atom_wm_s0 = x_intern_atom("WM_S0");
    atom_wm_protocols = x_intern_atom("WM_PROTOCOLS");
    atom_wm_delete = x_intern_atom("WM_DELETE_WINDOW");
    atom_serial = x_intern_atom("WL_SURFACE_SERIAL");
    atom_wm_name = x_intern_atom("WM_NAME");
    atom_net_wm_name = x_intern_atom("_NET_WM_NAME");
    atom_wm_class = x_intern_atom("WM_CLASS");
    atom_wm_state = x_intern_atom("WM_STATE");
    atom_wm_hints = x_intern_atom("WM_HINTS");
    atom_wm_normal_hints = x_intern_atom("WM_NORMAL_HINTS");
    atom_wm_take_focus = x_intern_atom("WM_TAKE_FOCUS");
    atom_utf8_string = x_intern_atom("UTF8_STRING");
    atom_net_close_window = x_intern_atom("_NET_CLOSE_WINDOW");
    atom_net_supported = x_intern_atom("_NET_SUPPORTED");
    atom_net_client_list = x_intern_atom("_NET_CLIENT_LIST");
    atom_net_active_window = x_intern_atom("_NET_ACTIVE_WINDOW");
    atom_net_current_desktop = x_intern_atom("_NET_CURRENT_DESKTOP");
    atom_net_number_of_desktops = x_intern_atom("_NET_NUMBER_OF_DESKTOPS");
    atom_atom = x_intern_atom("ATOM");
    atom_window_type = x_intern_atom("WINDOW");
    x_change_attributes(x_root, 0x800 /* CWEventMask */,
                        0x80000u   /* SubstructureNotifyMask (1<<19) */ |
                            0x100000u  /* SubstructureRedirectMask (1<<20) */ |
                            0x400000u /* PropertyChangeMask (1<<22) */);
    uint32_t wm_win = x_create_wm_window();
    if (atom_wm_s0)
        x_set_selection_owner(wm_win, atom_wm_s0);
    XWM_LOG("info", "window manager established on %s (wm window 0x%x)",
            display, wm_win);

    /* 1b. EWMH identity on the root: exactly what we implement, no
     * more (advertising an atom means honoring it). The X side sees
     * one desktop; the compositor's workspace model is not mirrored
     * into EWMH (documented remaining work). */
    if (atom_net_supported && atom_atom) {
        uint32_t supported[4];
        uint32_t n = 0;
        supported[n++] = atom_net_client_list;
        supported[n++] = atom_net_active_window;
        supported[n++] = atom_net_close_window;
        supported[n++] = atom_net_current_desktop;
        x_change_property_32(x_root, atom_net_supported, atom_atom, n,
                             supported);
        uint32_t v1[1];
        v1[0] = 1;
        x_change_property_32(x_root, atom_net_number_of_desktops,
                             XW_CARDINAL, 1, v1);
        v1[0] = 0;
        x_change_property_32(x_root, atom_net_current_desktop, XW_CARDINAL,
                             1, v1);
        v1[0] = 0;
        x_change_property_32(x_root, atom_net_active_window,
                             atom_window_type, 1, v1);
        client_list_write();
        XWM_LOG("info", "EWMH: _NET_SUPPORTED (%u atoms), "
                "_NET_CLIENT_LIST, _NET_ACTIVE_WINDOW, 1 desktop", n);
    }

    /* 2. composite-redirect every root subwindow: Xwayland only creates
     * wl_surfaces for manually redirected windows in rootless mode */
    uint8_t comp_op;
    if (!x_query_extension("Composite", &comp_op)) {
        fprintf(stderr, "[xw-xwm] fatal: the X server has no COMPOSITE "
                        "extension — Xwayland cannot run rootless\n");
        return 1;
    }
    op_composite = comp_op;
    x_composite_redirect_subwindows();
    XWM_LOG("info", "COMPOSITE redirect established (opcode %u)",
            op_composite);

    /* 3. Wayland side: geometry + close events from the compositor */
    wl_dpy = wl_display_connect(wl_name ? wl_name : NULL);
    if (!wl_dpy) {
        fprintf(stderr, "[xw-xwm] fatal: cannot connect to the compositor "
                        "(WAYLAND_DISPLAY=%s)\n",
                wl_name ? wl_name : "(unset)");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(wl_dpy);
    if (!wc) {
        fprintf(stderr, "[xw-xwm] fatal: the compositor does not expose "
                        "xw_window_control_manager_v1\n");
        return 1;
    }
    XWM_LOG("info", "xw-xwm ready: managing %s, mirroring geometry", display);

    /* 4. event loop */
    int wl_fd = wl_display_get_fd(wl_dpy);
    for (;;) {
        struct pollfd pfds[2] = {
            {.fd = x_fd, .events = POLLIN},
            {.fd = wl_fd, .events = POLLIN},
        };
        /* flush: -1 with EAGAIN is a full buffer (normal); -1 with
         * anything else (EPIPE, ECONNRESET) means the compositor is
         * gone and the socket will never drain */
        if (wl_display_flush(wl_dpy) < 0 && errno != EAGAIN)
            wl_lost();
        int rc = poll(pfds, 2, 500);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            die("poll()");
        if (pfds[0].revents & (POLLHUP | POLLERR)) {
            if (peer_gone(wl_fd)) {
                /* the compositor died with (or before) the X server:
                 * the whole stack is being torn down — nothing to
                 * manage and nobody to report to */
                XWM_LOG("info", "X stack teardown: X server connection "
                        "closed and the compositor is gone — exiting");
                exit(0);
            }
            /* do not report stale errno here: wl_display_flush leaves
             * EAGAIN on a healthy non-blocking socket constantly; the
             * poll flags are the actual evidence */
            fprintf(stderr,
                    "[xw-xwm] fatal: X server connection lost "
                    "(x_fd=%d revents=0x%x, wl revents=0x%x, %s)\n",
                    x_fd, (unsigned)pfds[0].revents,
                    (unsigned)pfds[1].revents,
                    (pfds[0].revents & POLLHUP)
                        ? "server closed the socket"
                        : "socket error pending");
            exit(1);
        }
        /* the wl fd was NEVER checked here before: a dead compositor
         * left the helper spinning at 100% CPU on a POLLHUP-only fd
         * (no POLLIN, so no dispatch, and poll never blocks) */
        if (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL))
            wl_lost();
        if (pfds[0].revents & POLLIN)
            x_drain();
        if (pfds[1].revents & POLLIN) {
            if (wl_display_dispatch(wl_dpy) < 0)
                wl_lost();
        } else if (rc == 0) {
            if (wl_display_dispatch_pending(wl_dpy) < 0)
                wl_lost();
        }
    }
    return 0;
}
