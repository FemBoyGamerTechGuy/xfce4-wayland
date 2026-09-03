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
static uint32_t op_composite = 0;

/* pending reply capture (one at a time; the helper is sequential) */
static uint8_t reply_buf[512];
static bool reply_ready = false;
static uint16_t reply_seq = 0;

struct xw_win {
    uint32_t xid;
    uint64_t serial;
    bool mapped;
    bool managed; /* passed through MapRequest */
};

static struct xw_win wins[XWM_MAX_WINDOWS];
static int n_wins = 0;

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
        XWM_LOG("warn", "X error %u on request %u (seq %u)", code, major, seq);
        return true;
    }
    if (unit[0] == 1) { /* reply */
        uint16_t seq = get16(unit, 2);
        uint32_t extra = get32(unit, 4);
        memcpy(reply_buf, unit, 32);
        if (extra > 0) {
            size_t want = extra * 4;
            if (want > sizeof(reply_buf) - 32)
                want = sizeof(reply_buf) - 32; /* truncate oversize */
            x_read((uint8_t *)reply_buf + 32, want);
            /* drain any remaining extra words */
            size_t total = (size_t)extra * 4;
            uint8_t sink[256];
            while (total > 0) {
                size_t chunk = total > sizeof(sink) ? sizeof(sink) : total;
                x_read(sink, chunk);
                total -= chunk;
            }
        }
        reply_ready = true;
        reply_seq = seq;
        return true;
    }
    /* an event: hand to the event sink */
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

static void x_set_input_focus(uint32_t window) {
    uint8_t req[16];
    size_t o = 0;
    put8(req, &o, 42);
    put8(req, &o, 1); /* revert-to: PointerRoot */
    put16(req, &o, 3);
    put32(req, &o, window);
    put32(req, &o, 0);
    x_send(req, o);
}

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
    uint8_t req[24];
    size_t o = 0;
    put8(req, &o, 20); /* GetProperty */
    put8(req, &o, 0);  /* delete = false */
    put16(req, &o, 6);
    put32(req, &o, window);
    put32(req, &o, atom_wm_protocols);
    put32(req, &o, 0); /* AnyPropertyType */
    put32(req, &o, 0); /* long offset */
    put32(req, &o, 64); /* long length */
    uint16_t seq = x_send(req, o);
    const uint8_t *rep = x_wait_reply(seq);
    if (rep[8] == 0) /* property type None: no WM_PROTOCOLS */
        return false;
    uint32_t nitems = get32(rep, 16);
    uint8_t fmt = rep[20];
    if (fmt != 32 || nitems == 0)
        return false;
    /* atoms live in the reply's extra section starting at byte 32 */
    for (uint32_t i = 0; i < nitems && 32 + (size_t)i * 4 + 4 <= sizeof(reply_buf); i++)
        if (get32(reply_buf, 32 + (size_t)i * 4) == atom_wm_delete)
            return true;
    return false;
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
    put8(req, &o, 32 | 0x80);    /* format 32, generated */
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

/* ------------------------------------------------------------- X events */

void xwm_handle_event(const uint8_t *ev) {
    switch (ev[0] & 0x7f) {
    case 20: { /* MapRequest: parent, window */
        uint32_t parent = get32(ev, 4);
        uint32_t window = get32(ev, 8);
        if (parent != x_root) {
            /* reparented children: let them be */
            x_map_window(window);
            return;
        }
        struct xw_win *w = win_track(window);
        if (w) {
            w->managed = true;
            w->mapped = true;
        }
        XWM_LOG("info", "MapRequest: window 0x%x — mapping and focusing",
                window);
        x_map_window(window);
        x_set_input_focus(window);
        break;
    }
    case 23: { /* ConfigureRequest: grant with the requested geometry */
        uint32_t window = get32(ev, 8);
        int16_t x = (int16_t)get16(ev, 20);
        int16_t y = (int16_t)get16(ev, 22);
        uint16_t w = get16(ev, 24);
        uint16_t h = get16(ev, 26);
        XWM_LOG("info",
                "ConfigureRequest: window 0x%x wants %dx%d+%d+%d — granting",
                window, w, h, x, y);
        x_configure_window(window, x, y, w, h);
        break;
    }
    case 33: { /* ClientMessage */
        uint32_t window = get32(ev, 4);
        uint32_t type = get32(ev, 8);
        if (type == 0 || !atom_serial)
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
                /* apply any geometry the compositor sent before this
                 * association arrived, so the X position matches our
                 * placement (input coordinates) from the first click */
                struct pending_geom pg;
                if (pend_take(serial, &pg)) {
                    XWM_LOG("info",
                            "  applying pending geometry %dx%d+%d+%d",
                            pg.w, pg.h, pg.x, pg.y);
                    x_configure_window(window, pg.x, pg.y, pg.w, pg.h);
                }
            }
        }
        break;
    }
    case 18: /* UnmapNotify */
        win_forget(get32(ev, 8));
        break;
    case 17: /* DestroyNotify */
        XWM_LOG("info", "window 0x%x destroyed", get32(ev, 8));
        win_forget(get32(ev, 8));
        break;
    case 19: /* MapNotify */
    case 21: /* ReparentNotify */
    case 22: /* ConfigureNotify */
    default:
        break;
    }
}

/* -------------------------------------------------- Wayland control side */

static struct wl_display *wl_dpy = NULL;
static struct xw_window_control_manager_v1 *wc = NULL;

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
    x_configure_window(w->xid, x, y, width, height);
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
    if (x_window_supports_wm_delete(w->xid)) {
        XWM_LOG("info", "  delivering WM_DELETE_WINDOW");
        x_send_wm_delete(w->xid);
    } else {
        XWM_LOG("info",
                "  no WM_DELETE_WINDOW support — destroying the window");
        x_destroy_window(w->xid);
    }
}

static const struct xw_window_control_manager_v1_listener wc_listener = {
    .geometry = wc_geometry,
    .close = wc_close,
};

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(iface, "xw_window_control_manager_v1") == 0) {
        wc = wl_registry_bind(r, name,
                              &xw_window_control_manager_v1_interface, 1);
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
    x_change_attributes(x_root, 0x800 /* CWEventMask */,
                        0x80000u /* SubstructureNotifyMask (1<<19) */ |
                            0x100000u /* SubstructureRedirectMask (1<<20) */);
    uint32_t wm_win = x_create_wm_window();
    if (atom_wm_s0)
        x_set_selection_owner(wm_win, atom_wm_s0);
    XWM_LOG("info", "window manager established on %s (wm window 0x%x)",
            display, wm_win);

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
        wl_display_flush(wl_dpy);
        int rc = poll(pfds, 2, 500);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            die("poll()");
        if (pfds[0].revents & (POLLHUP | POLLERR))
            die("X server connection lost");
        if (pfds[0].revents & POLLIN)
            x_drain();
        if (pfds[1].revents & POLLIN)
            wl_display_dispatch(wl_dpy);
        else if (rc == 0)
            wl_display_dispatch_pending(wl_dpy);
    }
    return 0;
}
