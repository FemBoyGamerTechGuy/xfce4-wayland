/* xw-session-seat.c — seat/session provider abstraction (the "xw-seat"
 * of the architecture docs).
 *
 * A compositor that owns real hardware (DRM/KMS + evdev) must not open
 * /dev/dri and /dev/input device nodes behind the back of the
 * platform's seat manager, and must not assume any particular one. This
 * module gives the compositor one abstraction,
 *
 *   open seat -> acquire session -> open device -> switch/release ->
 *   ack disable -> close seat
 *
 * with three interchangeable implementations:
 *
 *   libseat  external library (build-time optional). Itself wraps
 *            systemd-logind, elogind and seatd, selected by libseat at
 *            runtime. Preferred where installed.
 *   seatd    built-in client for the seatd unix-socket wire protocol
 *            (seatd >= 0.9). Gives first-class seatd support with zero
 *            extra dependencies: no systemd, no elogind, no root.
 *   direct   VT-direct: the compositor takes over its controlling tty
 *            (VT_PROCESS mode), opens devices with the permissions its
 *            login already has (logind/elogind device ACLs of the active
 *            session, or traditional video/input group membership), and
 *            manages VT switching itself. This is the classic path for
 *            plain TTY logins on distributions without a seat daemon.
 *
 * headless/nested backends do not create a seat at all.
 *
 * Session switching contract (identical for all providers): when the
 * session is about to become inactive the registered events.disable()
 * callback runs (the compositor must release scanout resources: DRM
 * master, page flips); the compositor then acknowledges with
 * xw_seat_session_ack_disable(). When the session becomes active again,
 * events.enable() runs (re-acquire, repaint). The direct provider
 * implements the kernel side of this with VT_RELDISP; the seatd client
 * with the protocol's DISABLE_SEAT/ack/ENABLE_SEAT messages.
 */
#include "xw-internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <sys/vt.h>
#include <linux/kd.h>
#include <linux/vt.h>

#ifdef XW_HAVE_LIBSEAT
#include <libseat.h>
#endif

/* ------------------------------------------------------------ wire protocol */
/* seatd unix-socket protocol, seatd 0.9+ (MIT; upstream source:
 * include/protocol.h, spelled out here so the client is self-contained).
 * Framing: { uint16 opcode; uint16 size; } + payload[size]. File
 * descriptors travel in the same recvmsg() as their message, via
 * SCM_RIGHTS. SERVER opcodes are CLIENT opcode | 0x8000. */

enum {
    SEATD_CLIENT_OPEN_SEAT = 1,
    SEATD_CLIENT_CLOSE_SEAT = 2,
    SEATD_CLIENT_OPEN_DEVICE = 3,
    SEATD_CLIENT_CLOSE_DEVICE = 4,
    SEATD_CLIENT_DISABLE_SEAT = 5,
    SEATD_CLIENT_SWITCH_SESSION = 6,
    SEATD_CLIENT_PING = 7,
};
enum {
    SEATD_SERVER_SEAT_OPENED = 0x8001,
    SEATD_SERVER_SEAT_CLOSED = 0x8002,
    SEATD_SERVER_DEVICE_OPENED = 0x8003,
    SEATD_SERVER_DEVICE_CLOSED = 0x8004,
    SEATD_SERVER_DISABLE_SEAT = 0x8005,
    SEATD_SERVER_ENABLE_SEAT = 0x8006,
    SEATD_SERVER_PONG = 0x8007,
    SEATD_SERVER_SESSION_SWITCHED = 0x8008,
    SEATD_SERVER_SEAT_DISABLED = 0x8009,
    SEATD_SERVER_ERROR = 0x7FFF,
};

#define SEATD_MAX_PATH 256
#define SEATD_REQ_TIMEOUT_MS 5000

struct proto_header {
    uint16_t opcode;
    uint16_t size;
};

/* --------------------------------------------------------------- base type */

struct xw_seat_session {
    const struct xw_seat_impl *impl; /* provider vtable */
    struct xw_compositor *comp;      /* for the event loop */
    struct xw_seat_events events;    /* registered by the DRM backend */
    void *events_ud;
    char seat_name[64];
    char desc[64]; /* "libseat", "seatd client", "direct VT" + backend */
    bool active;
    bool dead; /* connection lost / unrecoverable error */
};

void xw_seat_session_set_events(struct xw_seat_session *s,
                        const struct xw_seat_events *ev, void *ud) {
    if (!s)
        return;
    s->events = *ev;
    s->events_ud = ud;
}

const char *xw_seat_session_name(const struct xw_seat_session *s) {
    return s ? s->seat_name : "seat0";
}

const char *xw_seat_session_desc(const struct xw_seat_session *s) {
    return s ? s->desc : "none";
}

bool xw_seat_session_active(const struct xw_seat_session *s) {
    return s && s->active;
}

/* fire the registered callbacks (providers call these) */
static void seat_call_disable(struct xw_seat_session *s) {
    s->active = false;
    if (s->events.disable)
        s->events.disable(s->events_ud);
}

static void seat_call_enable(struct xw_seat_session *s) {
    s->active = true;
    if (s->events.enable)
        s->events.enable(s->events_ud);
}

int xw_seat_session_open_device(struct xw_seat_session *s, const char *path,
                        int *fd_out) {
    if (!s || s->dead) {
        errno = ENOTCONN;
        return -1;
    }
    return s->impl->open_device(s, path, fd_out);
}

int xw_seat_session_close_device(struct xw_seat_session *s, int device_id) {
    if (!s || s->dead)
        return -1;
    return s->impl->close_device(s, device_id);
}

int xw_seat_session_switch_vt(struct xw_seat_session *s, int vt) {
    if (!s || s->dead) {
        errno = ENOTCONN;
        return -1;
    }
    return s->impl->switch_vt(s, vt);
}

void xw_seat_session_destroy(struct xw_seat_session *s) {
    if (s) {
        s->impl->destroy(s);
        free(s);
    }
}

/* ====================================================== built-in seatd client */

struct xw_seat_seatd {
    struct xw_seat_session base;

    int fd;
    struct wl_event_source *src; /* async events on the compositor loop */

    /* receive side: one recvmsg may carry data + fds; both are buffered */
    uint8_t rbuf[1024];
    size_t rlen;
    size_t pending_payload; /* response payload at rbuf[0..] awaiting
                              the caller's parse + drop */
    int fds[4];
    int nfds;

    /* send side (requests are tiny; flushed per call) */
    uint8_t wbuf[SEATD_MAX_PATH + 16];
    size_t wlen;

    /* background events parsed during a blocking request are queued and
     * executed after the response is consumed (order-preserving) */
    int queued_events[8];
    int n_queued;
};

static struct xw_seat_seatd *sd_of(struct xw_seat_session *s) {
    return (struct xw_seat_seatd *)s;
}

static void sd_fail(struct xw_seat_seatd *sd, const char *why) {
    if (sd->base.dead)
        return;
    sd->base.dead = true;
    xw_log(XW_LOG_ERROR, "seat: seatd connection error: %s", why);
    if (sd->base.comp)
        xw_compositor_stop(sd->base.comp);
}

/* ---- wire helpers ---- */

static int sd_put(struct xw_seat_seatd *sd, const void *data, size_t len) {
    if (sd->wlen + len > sizeof(sd->wbuf)) {
        sd_fail(sd, "request too large");
        return -1;
    }
    memcpy(sd->wbuf + sd->wlen, data, len);
    sd->wlen += len;
    return 0;
}

static int sd_flush(struct xw_seat_seatd *sd) {
    while (sd->wlen > 0) {
        /* MSG_NOSIGNAL: a seat manager that died must surface as EPIPE,
         * not as a SIGPIPE that kills the compositor outright */
        ssize_t n = send(sd->fd, sd->wbuf, sd->wlen, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = sd->fd, .events = POLLOUT};
                if (poll(&pfd, 1, SEATD_REQ_TIMEOUT_MS) != 1) {
                    sd_fail(sd, "send timeout");
                    return -1;
                }
                continue;
            }
            sd_fail(sd, strerror(errno));
            return -1;
        }
        memmove(sd->wbuf, sd->wbuf + n, sd->wlen - (size_t)n);
        sd->wlen -= (size_t)n;
    }
    return 0;
}

/* one recvmsg: data into rbuf, fds into the fd queue */
static int sd_read(struct xw_seat_seatd *sd) {
    struct iovec iov = {.iov_base = sd->rbuf + sd->rlen,
                        .iov_len = sizeof(sd->rbuf) - sd->rlen};
    char cmsg[CMSG_SPACE(sizeof(int) * 4)] = {0};
    struct msghdr msg = {.msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = cmsg,
                         .msg_controllen = sizeof(cmsg)};
    ssize_t n = recvmsg(sd->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        sd_fail(sd, strerror(errno));
        return -1;
    }
    if (n == 0) {
        sd_fail(sd, "seat manager closed the connection");
        return -1;
    }
    sd->rlen += (size_t)n;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c;
         c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < cnt && sd->nfds < 4; i++)
                sd->fds[sd->nfds++] = ((int *)CMSG_DATA(c))[i];
            /* extra fds beyond the queue are closed (protocol carries
             * at most one fd per message) */
            for (int i = 4; i < cnt; i++)
                close(((int *)CMSG_DATA(c))[i]);
        }
    }
    return 1;
}

static int sd_take_fd(struct xw_seat_seatd *sd) {
    if (sd->nfds <= 0)
        return -1;
    int fd = sd->fds[0];
    memmove(sd->fds, sd->fds + 1, sizeof(int) * (size_t)(--sd->nfds));
    return fd;
}

/* wait until at least one complete message is buffered */
static int sd_read_more(struct xw_seat_seatd *sd, int timeout_ms) {
    if (sd->rlen < sizeof(struct proto_header)) {
        /* need more data */
    } else {
        struct proto_header h;
        memcpy(&h, sd->rbuf, sizeof(h));
        if (sd->rlen >= sizeof(h) + h.size)
            return 1;
    }
    struct pollfd pfd = {.fd = sd->fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) != 1) {
        if (timeout_ms > 0)
            sd_fail(sd, "response timeout");
        return -1;
    }
    return sd_read(sd);
}

/* message queue walker shared by the sync request path and the async
 * event source. Strips the header of the first COMPLETE message; its
 * payload is left at rbuf[0..size-1] for the caller to parse (and drop
 * with sd_advance). Returns 0 if no complete message is buffered. */
static int sd_next(struct xw_seat_seatd *sd, struct proto_header *hdr) {
    if (sd->rlen < sizeof(*hdr))
        return 0;
    struct proto_header h;
    memcpy(&h, sd->rbuf, sizeof(h));
    if (sd->rlen < sizeof(h) + h.size)
        return 0;
    memmove(sd->rbuf, sd->rbuf + sizeof(h), sd->rlen - sizeof(h));
    sd->rlen -= sizeof(h);
    *hdr = h;
    return 1;
}

/* advance past `n` payload bytes already parsed by the caller */
static void sd_advance(struct xw_seat_seatd *sd, size_t n) {
    if (n == 0)
        return;
    memmove(sd->rbuf, sd->rbuf + n, sd->rlen - n);
    sd->rlen -= n;
}

/* Consume messages from the buffer. expected_opcode != 0: it is the
 * response we are waiting for; when found, its payload is left at
 * rbuf[0..size-1] with sd->pending_payload = size (the caller parses it
 * and then MUST call sd_request_end()). Background events
 * (DISABLE/ENABLE) are queued in order. Returns 1 if the expected
 * response was found, 0 if only background traffic (or nothing) was
 * consumed, -1 on error. */
static int sd_consume(struct xw_seat_seatd *sd, uint16_t expected) {
    struct proto_header h;
    while (sd_next(sd, &h) == 1) {
        switch (h.opcode) {
        case SEATD_SERVER_DISABLE_SEAT:
            sd_advance(sd, h.size);
            if (sd->n_queued < 8)
                sd->queued_events[sd->n_queued++] = SEATD_SERVER_DISABLE_SEAT;
            continue;
        case SEATD_SERVER_ENABLE_SEAT:
            sd_advance(sd, h.size);
            if (sd->n_queued < 8)
                sd->queued_events[sd->n_queued++] = SEATD_SERVER_ENABLE_SEAT;
            continue;
        case SEATD_SERVER_PONG:
            sd_advance(sd, h.size);
            continue;
        default:
            break;
        }
        if (expected == 0) {
            xw_log(XW_LOG_ERROR,
                   "seat: unexpected seatd message 0x%04x (no request "
                   "pending); connection is out of sync",
                   h.opcode);
            sd_fail(sd, "unexpected message");
            return -1;
        }
        if (h.opcode == expected) {
            sd->pending_payload = h.size;
            return 1;
        }
        if (h.opcode == SEATD_SERVER_ERROR) {
            int code = 0;
            if (h.size >= sizeof(int))
                memcpy(&code, sd->rbuf, sizeof(int));
            sd_advance(sd, h.size);
            errno = code ? code : EPROTO;
            xw_log(XW_LOG_ERROR, "seat: seatd refused the request: %s",
                   strerror(errno));
            sd_fail(sd, "server error");
            return -1;
        }
        xw_log(XW_LOG_ERROR, "seat: unexpected response 0x%04x (wanted "
                             "0x%04x)",
               h.opcode, expected);
        sd_fail(sd, "unexpected response");
        return -1;
    }
    return 0;
}

static void sd_run_queued(struct xw_seat_seatd *sd) {
    /* copy first: a disable handler may (synchronously, and correctly)
     * issue the ack request, which re-enters the queue walker */
    int ev[8];
    int n = sd->n_queued;
    memcpy(ev, sd->queued_events, sizeof(int) * (size_t)n);
    sd->n_queued = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i] == SEATD_SERVER_DISABLE_SEAT) {
            xw_log(XW_LOG_INFO, "seat: seatd: session becoming inactive");
            seat_call_disable(&sd->base);
        } else {
            xw_log(XW_LOG_INFO, "seat: seatd: session active again");
            seat_call_enable(&sd->base);
        }
    }
}

/* Blocking request/response round trip. On success returns the response
 * payload size (>= response_min_size) with the payload at rbuf[0..size);
 * the caller parses it and MUST then call sd_request_end(). Returns -1
 * on error (errno set; connection marked dead). */
static int sd_request(struct xw_seat_seatd *sd, uint16_t response_opcode,
                      size_t response_min_size) {
    if (sd_flush(sd) < 0)
        return -1;
    for (;;) {
        int rc = sd_consume(sd, response_opcode);
        if (rc < 0)
            return -1;
        if (rc > 0)
            break;
        if (sd_read_more(sd, SEATD_REQ_TIMEOUT_MS) < 0)
            return -1;
    }
    if (sd->pending_payload < response_min_size) {
        errno = EBADMSG;
        sd_fail(sd, "short response");
        return -1;
    }
    return (int)sd->pending_payload;
}

/* drop the parsed response payload; then process any background events
 * that arrived behind it, in order */
static int sd_request_end(struct xw_seat_seatd *sd) {
    if (sd->pending_payload) {
        sd_advance(sd, sd->pending_payload);
        sd->pending_payload = 0;
    }
    if (sd_consume(sd, 0) < 0)
        return -1;
    sd_run_queued(sd);
    return 0;
}

/* async fd callback: background events only (no request in flight) */
static int sd_on_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct xw_seat_seatd *sd = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        sd_fail(sd, "seat manager connection lost");
        return 0;
    }
    if (sd_read(sd) < 0)
        return 0;
    if (sd_consume(sd, 0) < 0)
        return 0;
    sd_run_queued(sd);
    return 0;
}

/* ---- provider vtable ---- */

static int sd_open_device(struct xw_seat_session *s, const char *path,
                          int *fd_out) {
    struct xw_seat_seatd *sd = sd_of(s);
    size_t pathlen = strlen(path) + 1;
    if (pathlen > SEATD_MAX_PATH) {
        errno = EINVAL;
        return -1;
    }
    uint8_t req[sizeof(struct proto_header) + 2 + SEATD_MAX_PATH];
    struct proto_header h = {.opcode = SEATD_CLIENT_OPEN_DEVICE,
                             .size = (uint16_t)(2 + pathlen)};
    uint16_t plen = (uint16_t)pathlen;
    memcpy(req, &h, sizeof(h));
    memcpy(req + sizeof(h), &plen, 2);
    memcpy(req + sizeof(h) + 2, path, pathlen);
    if (sd_put(sd, req, sizeof(h) + 2 + pathlen) < 0)
        return -1;
    if (sd_request(sd, SEATD_SERVER_DEVICE_OPENED, 4) < 0)
        return -1;
    int device_id = -1;
    memcpy(&device_id, sd->rbuf, 4);
    int fd = sd_take_fd(sd);
    sd_request_end(sd);
    if (fd < 0) {
        errno = EBADMSG;
        sd_fail(sd, "device opened without an fd");
        return -1;
    }
    if (device_id < 0) {
        close(fd);
        errno = EBADMSG;
        return -1;
    }
    *fd_out = fd;
    return device_id;
}

static int sd_close_device(struct xw_seat_session *s, int device_id) {
    struct xw_seat_seatd *sd = sd_of(s);
    if (device_id < 0) {
        errno = EINVAL;
        return -1;
    }
    struct proto_header h = {.opcode = SEATD_CLIENT_CLOSE_DEVICE,
                             .size = 4};
    if (sd_put(sd, &h, sizeof(h)) < 0)
        return -1;
    if (sd_put(sd, &device_id, 4) < 0)
        return -1;
    if (sd_request(sd, SEATD_SERVER_DEVICE_CLOSED, 0) < 0)
        return -1;
    return sd_request_end(sd) < 0 ? -1 : 0;
}

static int sd_switch_vt(struct xw_seat_session *s, int vt) {
    struct xw_seat_seatd *sd = sd_of(s);
    if (vt < 0) {
        errno = EINVAL;
        return -1;
    }
    struct proto_header h = {.opcode = SEATD_CLIENT_SWITCH_SESSION,
                             .size = 4};
    if (sd_put(sd, &h, sizeof(h)) < 0)
        return -1;
    if (sd_put(sd, &vt, 4) < 0)
        return -1;
    if (sd_request(sd, SEATD_SERVER_SESSION_SWITCHED, 0) < 0)
        return -1;
    return sd_request_end(sd) < 0 ? -1 : 0;
}

static int sd_ack_disable(struct xw_seat_session *s) {
    struct xw_seat_seatd *sd = sd_of(s);
    struct proto_header h = {.opcode = SEATD_CLIENT_DISABLE_SEAT, .size = 0};
    if (sd_put(sd, &h, sizeof(h)) < 0)
        return -1;
    if (sd_request(sd, SEATD_SERVER_SEAT_DISABLED, 0) < 0)
        return -1;
    return sd_request_end(sd) < 0 ? -1 : 0;
}

static void sd_destroy(struct xw_seat_session *s) {
    struct xw_seat_seatd *sd = sd_of(s);
    if (!s->dead) {
        struct proto_header h = {.opcode = SEATD_CLIENT_CLOSE_SEAT,
                                 .size = 0};
        if (sd_put(sd, &h, sizeof(h)) == 0 &&
            sd_request(sd, SEATD_SERVER_SEAT_CLOSED, 0) >= 0)
            sd_request_end(sd);
    }
    if (sd->src)
        wl_event_source_remove(sd->src);
    while (sd->nfds > 0)
        close(sd->fds[--sd->nfds]);
    if (sd->fd >= 0)
        close(sd->fd);
}

static const struct xw_seat_impl seatd_impl = {
    .open_device = sd_open_device,
    .close_device = sd_close_device,
    .switch_vt = sd_switch_vt,
    .ack_disable = sd_ack_disable,
    .destroy = sd_destroy,
};

static const char *seatd_socket_path(void) {
    const char *p = getenv("SEATD_SOCK");
    if (p && *p)
        return p;
    return "/run/seatd.sock";
}

/* connect + OPEN_SEAT handshake. NULL on failure (reason logged). */
static struct xw_seat_session *seatd_create(struct xw_compositor *c,
                                            const char *seat_name) {
    const char *sock = seatd_socket_path();
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(sock) >= sizeof(addr.sun_path)) {
        xw_log(XW_LOG_ERROR, "seat: seatd socket path too long: %s", sock);
        return NULL;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        xw_log(XW_LOG_ERROR, "seat: cannot create socket: %s",
               strerror(errno));
        return NULL;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        xw_log(XW_LOG_DEBUG, "seat: seatd socket %s: %s", sock,
               strerror(errno));
        close(fd);
        return NULL;
    }

    struct xw_seat_seatd *sd = calloc(1, sizeof(*sd));
    if (!sd) {
        close(fd);
        return NULL;
    }
    sd->fd = fd;
    sd->base.impl = &seatd_impl;
    sd->base.comp = c;
    sd->base.active = true;
    snprintf(sd->base.seat_name, sizeof(sd->base.seat_name), "%s",
             seat_name ? seat_name : "seat0");

    /* handshake: CLIENT_OPEN_SEAT -> SERVER_SEAT_OPENED(seat name) */
    struct proto_header h = {.opcode = SEATD_CLIENT_OPEN_SEAT, .size = 0};
    if (sd_put(sd, &h, sizeof(h)) < 0 ||
        sd_request(sd, SEATD_SERVER_SEAT_OPENED, 2) < 0) {
        sd_destroy(&sd->base);
        free(sd);
        return NULL;
    }
    uint16_t name_len = 0;
    memcpy(&name_len, sd->rbuf, 2);
    if (name_len == 0 || name_len > sizeof(sd->base.seat_name) - 1 ||
        (size_t)name_len + 2 > sd->pending_payload) {
        errno = EBADMSG;
        xw_log(XW_LOG_ERROR, "seat: malformed SEAT_OPENED message");
        sd_request_end(sd);
        sd_destroy(&sd->base);
        free(sd);
        return NULL;
    }
    /* the name is NUL-terminated in seatd >= 0.6; older servers omitted
     * it, so terminate defensively */
    snprintf(sd->base.seat_name, sizeof(sd->base.seat_name), "%.*s",
             (int)name_len, sd->rbuf + 2);
    sd_request_end(sd);
    snprintf(sd->base.desc, sizeof(sd->base.desc), "seatd (%s)", sock);

    if (c && c->loop) {
        sd->src = wl_event_loop_add_fd(c->loop, fd, WL_EVENT_READABLE,
                                       sd_on_readable, sd);
        if (!sd->src) {
            sd_destroy(&sd->base);
            free(sd);
            return NULL;
        }
    }
    xw_log(XW_LOG_INFO, "seat: opened through seatd at %s (seat %s)", sock,
           sd->base.seat_name);
    return &sd->base;
}

/* =========================================================== libseat wrapper */

#ifdef XW_HAVE_LIBSEAT

struct xw_seat_ls {
    struct xw_seat_session base;
    struct libseat *ls;
    struct wl_event_source *src;
};

static struct xw_seat_ls *ls_of(struct xw_seat_session *s) {
    return (struct xw_seat_ls *)s;
}

static void ls_log(enum libseat_log_level level, const char *fmt,
                   va_list args) {
    /* libseat's own backend selection messages ("opened seat through
     * logind", "seatd socket unavailable", ...) are the ONLY place the
     * active backend name is stated: route ERROR as WARN and INFO as
     * INFO so a real-session log answers "which libseat backend is
     * active" without guesswork; DEBUG stays DEBUG */
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    enum xw_log_level xw = XW_LOG_DEBUG;
    if (level == LIBSEAT_LOG_LEVEL_ERROR)
        xw = XW_LOG_WARN;
    else if (level == LIBSEAT_LOG_LEVEL_INFO)
        xw = XW_LOG_INFO;
    xw_log(xw, "seat: libseat: %s", buf);
}

static void ls_on_enable(struct libseat *ls, void *ud) {
    (void)ls;
    struct xw_seat_session *s = ud;
    xw_log(XW_LOG_INFO, "seat: libseat: session active");
    seat_call_enable(s);
}

static void ls_on_disable(struct libseat *ls, void *ud) {
    (void)ls;
    struct xw_seat_session *s = ud;
    xw_log(XW_LOG_INFO, "seat: libseat: session becoming inactive");
    seat_call_disable(s);
}

static int ls_on_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct xw_seat_ls *l = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        if (!l->base.dead) {
            l->base.dead = true;
            xw_log(XW_LOG_ERROR, "seat: libseat connection lost");
            if (l->base.comp)
                xw_compositor_stop(l->base.comp);
        }
        return 0;
    }
    if (libseat_dispatch(l->ls, 0) < 0) {
        if (!l->base.dead) {
            l->base.dead = true;
            xw_log(XW_LOG_ERROR, "seat: libseat dispatch failed: %s",
                   strerror(errno));
            if (l->base.comp)
                xw_compositor_stop(l->base.comp);
        }
    }
    return 0;
}

static int ls_open_device(struct xw_seat_session *s, const char *path,
                          int *fd_out) {
    struct xw_seat_ls *l = ls_of(s);
    int fd = -1;
    int id = libseat_open_device(l->ls, path, &fd);
    if (id < 0) {
        xw_log(XW_LOG_WARN, "seat: libseat cannot open %s: %s", path,
               strerror(errno));
        return -1;
    }
    *fd_out = fd;
    return id;
}

static int ls_close_device(struct xw_seat_session *s, int device_id) {
    return libseat_close_device(ls_of(s)->ls, device_id);
}

static int ls_switch_vt(struct xw_seat_session *s, int vt) {
    return libseat_switch_session(ls_of(s)->ls, vt);
}

static int ls_ack_disable(struct xw_seat_session *s) {
    return libseat_disable_seat(ls_of(s)->ls);
}

static void ls_destroy(struct xw_seat_session *s) {
    struct xw_seat_ls *l = ls_of(s);
    if (l->src)
        wl_event_source_remove(l->src);
    if (l->ls)
        libseat_close_seat(l->ls);
}

static const struct xw_seat_impl ls_impl = {
    .open_device = ls_open_device,
    .close_device = ls_close_device,
    .switch_vt = ls_switch_vt,
    .ack_disable = ls_ack_disable,
    .destroy = ls_destroy,
};

static struct xw_seat_session *libseat_create(struct xw_compositor *c,
                                              const char *seat_name,
                                              const char *force_backend) {
    static bool log_installed;
    if (!log_installed) {
        libseat_set_log_handler(ls_log);
        libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);
        log_installed = true;
    }
    struct xw_seat_ls *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->base.impl = &ls_impl;
    l->base.comp = c;
    l->base.active = true;
    snprintf(l->base.seat_name, sizeof(l->base.seat_name), "%s",
             seat_name ? seat_name : "seat0");

    static const struct libseat_seat_listener listener = {
        .enable_seat = ls_on_enable,
        .disable_seat = ls_on_disable,
    };
    /* libseat's own backend order is seatd-first, logind-second — the
     * opposite of this project's preference. When a specific backend
     * is forced (the elogind/logind path), LIBSEAT_BACKEND makes
     * libseat try exactly that one; the caller's value (or its
     * absence) is restored afterwards so nothing leaks between
     * attempts. A NULL force_backend leaves libseat's own choice and
     * any environment override intact. */
    char saved[64] = "";
    bool had_saved = false;
    if (force_backend) {
        const char *cur = getenv("LIBSEAT_BACKEND");
        had_saved = cur != NULL;
        if (had_saved)
            snprintf(saved, sizeof(saved), "%s", cur);
        setenv("LIBSEAT_BACKEND", force_backend, 1);
        xw_log(XW_LOG_INFO,
               "seat: libseat pinned to its '%s' backend for this attempt",
               force_backend);
    }
    l->ls = libseat_open_seat(&listener, l);
    if (force_backend) {
        if (had_saved)
            setenv("LIBSEAT_BACKEND", saved, 1);
        else
            unsetenv("LIBSEAT_BACKEND");
    }
    if (!l->ls) {
        xw_log(XW_LOG_DEBUG, "seat: libseat backends unavailable: %s",
               strerror(errno));
        free(l);
        return NULL;
    }
    snprintf(l->base.seat_name, sizeof(l->base.seat_name), "%s",
             libseat_seat_name(l->ls));
    snprintf(l->base.desc, sizeof(l->base.desc), "libseat%s%s",
             force_backend ? " (" : "",
             force_backend ? force_backend : "");
    if (force_backend) {
        size_t n = strlen(l->base.desc);
        snprintf(l->base.desc + n, sizeof(l->base.desc) - n, ")");
    }

    int fd = libseat_get_fd(l->ls);
    if (fd < 0)
        goto fail;
    if (c && c->loop) {
        l->src = wl_event_loop_add_fd(c->loop, fd, WL_EVENT_READABLE,
                                      ls_on_readable, l);
        if (!l->src)
            goto fail;
    }
    if (libseat_dispatch(l->ls, 0) < 0)
        goto fail;
    xw_log(XW_LOG_INFO, "seat: opened through libseat (seat %s)",
           l->base.seat_name);
    return &l->base;
fail:
    ls_destroy(&l->base);
    free(l);
    return NULL;
}
#endif /* XW_HAVE_LIBSEAT */

/* ============================================================= direct VT */

struct xw_seat_direct {
    struct xw_seat_session base;
    int tty_fd;
    struct wl_event_source *rel_src, *acq_src;
    struct termios saved_tio;
    bool tio_saved;
    int saved_kbmode;
    struct vt_mode saved_vtmode;
    bool vtmode_saved;
    int vt_num; /* 0 = unknown */
};

static struct xw_seat_direct *dr_of(struct xw_seat_session *s) {
    return (struct xw_seat_direct *)s;
}

static int dr_open_device(struct xw_seat_session *s, const char *path,
                          int *fd_out) {
    (void)s;
    /* Direct access: valid when the session's permissions already allow
     * it — logind/elogind attach ACLs to the active session's devices,
     * and traditional setups use video/input group membership. The
     * device id is the fd itself. */
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        xw_log(XW_LOG_WARN, "seat: direct: cannot open %s: %s", path,
               strerror(errno));
    else
        *fd_out = fd;
    return fd;
}

static int dr_close_device(struct xw_seat_session *s, int device_id) {
    (void)s;
    return close(device_id);
}

static int dr_switch_vt(struct xw_seat_session *s, int vt) {
    struct xw_seat_direct *d = dr_of(s);
    if (ioctl(d->tty_fd, VT_ACTIVATE, vt) < 0)
        return -1;
    return 0;
}

static int dr_ack_disable(struct xw_seat_session *s) {
    struct xw_seat_direct *d = dr_of(s);
    /* permit the pending VT switch away */
    if (ioctl(d->tty_fd, VT_RELDISP, 1) < 0)
        xw_log(XW_LOG_WARN, "seat: direct: VT_RELDISP: %s", strerror(errno));
    return 0;
}

/* kernel wants to switch away: notify the compositor (it drops DRM
 * master) — the compositor then acks via xw_seat_ack_disable */
static int dr_on_release(int sig, void *data) {
    (void)sig;
    struct xw_seat_direct *d = data;
    xw_log(XW_LOG_INFO, "seat: direct: VT switch away requested (vt %d)",
           d->vt_num);
    seat_call_disable(&d->base);
    return 0;
}

static int dr_on_acquire(int sig, void *data) {
    (void)sig;
    struct xw_seat_direct *d = data;
    xw_log(XW_LOG_INFO, "seat: direct: our VT became active (vt %d)",
           d->vt_num);
    ioctl(d->tty_fd, KDSETMODE, KD_GRAPHICS); /* may have been reset */
    seat_call_enable(&d->base);
    return 0;
}

static void dr_destroy(struct xw_seat_session *s) {
    struct xw_seat_direct *d = dr_of(s);
    if (d->rel_src)
        wl_event_source_remove(d->rel_src);
    if (d->acq_src)
        wl_event_source_remove(d->acq_src);
    /* full state restoration — this is the black-screen safety net for
     * the terminal we took over */
    if (d->vtmode_saved) {
        struct vt_mode m = d->saved_vtmode;
        ioctl(d->tty_fd, VT_SETMODE, &m);
    }
    if (d->tty_fd >= 0) {
        ioctl(d->tty_fd, KDSETMODE, d->saved_kbmode);
        if (d->tio_saved)
            tcsetattr(d->tty_fd, TCSANOW, &d->saved_tio);
        close(d->tty_fd);
    }
}

static const struct xw_seat_impl dr_impl = {
    .open_device = dr_open_device,
    .close_device = dr_close_device,
    .switch_vt = dr_switch_vt,
    .ack_disable = dr_ack_disable,
    .destroy = dr_destroy,
};

static struct xw_seat_session *direct_create(struct xw_compositor *c,
                                             const char *seat_name) {
    int tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty < 0) {
        xw_log(XW_LOG_DEBUG, "seat: direct: cannot open /dev/tty: %s",
               strerror(errno));
        return NULL;
    }
    struct vt_stat vts;
    if (ioctl(tty, VT_GETSTATE, &vts) < 0) {
        xw_log(XW_LOG_DEBUG,
               "seat: direct: /dev/tty is not a virtual terminal (%s) — "
               "no TTY session to take over",
               strerror(errno));
        close(tty);
        return NULL;
    }

    struct xw_seat_direct *d = calloc(1, sizeof(*d));
    if (!d) {
        close(tty);
        return NULL;
    }
    d->tty_fd = tty;
    d->vt_num = vts.v_active;
    d->base.impl = &dr_impl;
    d->base.comp = c;
    d->base.active = true;
    snprintf(d->base.seat_name, sizeof(d->base.seat_name), "%s",
             seat_name ? seat_name : "seat0");
    snprintf(d->base.desc, sizeof(d->base.desc), "direct (vt %d)",
             d->vt_num);

    /* save, then take over the terminal */
    d->tio_saved = tcgetattr(tty, &d->saved_tio) == 0;
    if (ioctl(tty, KDGETMODE, &d->saved_kbmode) < 0)
        d->saved_kbmode = KD_TEXT;
    if (ioctl(tty, VT_GETMODE, &d->saved_vtmode) < 0)
        memset(&d->saved_vtmode, 0, sizeof(d->saved_vtmode));
    else
        d->vtmode_saved = true;

    if (ioctl(tty, KDSETMODE, KD_GRAPHICS) < 0) {
        xw_log(XW_LOG_ERROR, "seat: direct: KDSETMODE(KD_GRAPHICS): %s",
               strerror(errno));
        goto fail;
    }
    struct vt_mode m = {.mode = VT_PROCESS,
                        .waitv = 0,
                        .relsig = SIGUSR1,
                        .acqsig = SIGUSR2,
                        .frsig = 0};
    if (ioctl(tty, VT_SETMODE, &m) < 0) {
        xw_log(XW_LOG_ERROR, "seat: direct: VT_SETMODE(VT_PROCESS): %s",
               strerror(errno));
        goto fail;
    }

    if (c && c->loop) {
        d->rel_src = wl_event_loop_add_signal(c->loop, SIGUSR1,
                                              dr_on_release, d);
        d->acq_src = wl_event_loop_add_signal(c->loop, SIGUSR2,
                                              dr_on_acquire, d);
        if (!d->rel_src || !d->acq_src)
            goto fail;
    }
    xw_log(XW_LOG_INFO, "seat: direct VT session on vt %d (seat %s)",
           d->vt_num, d->base.seat_name);
    return &d->base;
fail:
    dr_destroy(&d->base);
    free(d);
    return NULL;
}

/* ------------------------------------------------------------- open + pick */

/* Read-only probes of the machine's seat-management environment — the
 * facts a real-session log must state up front so "which provider
 * should have been used" can be answered from one run: is libseat
 * compiled in, is a seat manager actually present. Never used for
 * selection (selection is by trying, above), only for diagnostics. */
static void seat_report_environment(void) {
#ifdef XW_HAVE_LIBSEAT
    const char *libseat = "compiled in";
#else
    const char *libseat = "not compiled into this build";
#endif
    bool seatd_sock = access(seatd_socket_path(), F_OK) == 0;
    bool logind = access("/run/systemd/seats/", F_OK) == 0;
    bool elogind = access("/run/elogind/", F_OK) == 0;
    bool dbus =
        (access("/run/dbus/system_bus_socket", F_OK) == 0) ||
        (getenv("DBUS_SYSTEM_BUS_ADDRESS") != NULL);
    const char *sock_env = getenv("SEATD_SOCK");
    const char *session_id = getenv("XDG_SESSION_ID");
    xw_log(XW_LOG_INFO,
           "seat: environment: libseat %s; seatd socket %s%s%s; %s",
           libseat, seatd_socket_path(), sock_env && *sock_env ? " ($SEATD_SOCK)" : "",
           seatd_sock ? ": present" : ": absent", logind
                          ? "systemd-logind is booted (/run/systemd/seats)"
                          : elogind ? "elogind is present (/run/elogind)"
                                    : "no logind/elogind");
    xw_log(XW_LOG_INFO, "seat: environment: system d-bus %s",
           dbus ? "present" : "absent (the logind/elogind backend needs "
                              "it)");
    xw_log(XW_LOG_INFO,
           "seat: environment: $XDG_SESSION_ID %s%s%s",
           session_id && *session_id ? "= " : "",
           session_id && *session_id ? session_id : "is unset",
           session_id && *session_id
               ? " — this login is registered as a logind/elogind "
                 "session"
               : " — this login has no registered session (the logind "
                 "backend will try PID/user-based discovery)");
}

int xw_seat_session_ack_disable(struct xw_seat_session *s) {
    if (!s || !s->impl->ack_disable)
        return 0;
    return s->impl->ack_disable(s);
}

struct xw_seat_session *xw_seat_session_open(struct xw_compositor *c, int provider,
                                     const char *seat_name) {
    if (provider == XW_SEAT_PROVIDER_NONE)
        return NULL;

    const char *seat = seat_name && *seat_name ? seat_name : "seat0";

    if (provider == XW_SEAT_PROVIDER_ELOGIND) {
#ifdef XW_HAVE_LIBSEAT
        struct xw_seat_session *s = libseat_create(c, seat, "logind");
        if (!s)
            xw_log(XW_LOG_ERROR,
                   "seat: elogind/logind requested but libseat's logind "
                   "backend could not acquire a session: %s\n"
                   "  elogind and logind speak the same "
                   "org.freedesktop.login1 D-Bus API; the usual causes:\n"
                   "  - this login is not a registered session ($XDG_SESSION_ID "
                   "unset and PID/user discovery failed — a TTY login needs "
                   "pam_elogind/pam_systemd in its PAM stack)\n"
                   "  - the session exists but is not active (another session "
                   "holds the seat; switch to this login's VT and log in "
                   "fresh)\n"
                   "  - the system d-bus is unreachable",
                   strerror(errno));
        return s;
#else
        xw_log(XW_LOG_ERROR,
               "seat: elogind/logind requested, but this build has no "
               "libseat support (libseat development files were absent at "
               "build time) — the logind/elogind path needs libseat");
        return NULL;
#endif
    }
    if (provider == XW_SEAT_PROVIDER_LIBSEAT) {
#ifdef XW_HAVE_LIBSEAT
        struct xw_seat_session *s = libseat_create(c, seat, NULL);
        if (!s)
            xw_log(XW_LOG_ERROR,
                   "seat: libseat requested but no libseat backend could "
                   "acquire the seat: %s",
                   strerror(errno));
        return s;
#else
        xw_log(XW_LOG_ERROR,
               "seat: libseat requested, but this build has no libseat "
               "support (libseat development files were absent at build "
               "time)");
        return NULL;
#endif
    }
    if (provider == XW_SEAT_PROVIDER_SEATD) {
        struct xw_seat_session *s = seatd_create(c, seat);
        if (!s) {
            int e = errno;
            xw_log(XW_LOG_ERROR,
                   "seat: seatd requested but the seatd socket is unusable "
                   "(%s): %s\n"
                   "  Is the seatd daemon running, and is this user "
                   "permitted to connect?\n"
                   "  (Artix/Arch: enable the seatd service and add this "
                   "user to the 'seat' group; see BUILDING.md)",
                   seatd_socket_path(), strerror(e ? e : ENOENT));
        }
        return s;
    }
    if (provider == XW_SEAT_PROVIDER_DIRECT) {
        struct xw_seat_session *s = direct_create(c, seat);
        if (!s)
            xw_log(XW_LOG_ERROR,
                   "seat: a direct VT session was requested but cannot be "
                   "taken: /dev/tty is not a virtual terminal (%s)\n"
                   "  A direct session needs a real TTY login (the "
                   "controlling terminal must be a VT, e.g. "
                   "Ctrl+Alt+F3).\n"
                   "  When launched from a container, an X/Wayland session "
                   "or a display manager without a VT, use seatd/libseat "
                   "instead (see BUILDING.md)",
                   strerror(errno ? errno : ENOTTY));
        return s;
    }

    /* XW_SEAT_PROVIDER_AUTO: capability detection, in explicit
     * preference order: elogind/logind first (a session manager that
     * grants device ACLs to the active login), then the seatd socket,
     * then a direct VT takeover. libseat's own internal order is the
     * opposite (seatd before logind), so the elogind attempt pins
     * libseat to its logind backend via $LIBSEAT_BACKEND — elogind
     * implements the same org.freedesktop.login1 D-Bus API. Every
     * accept/reject is logged at INFO: "which provider did the session
     * actually use and why" must be answerable from one --verbose
     * run. */
    {
        seat_report_environment();

        /* 1. elogind/logind session (through libseat's logind backend) */
        bool login1 = access("/run/elogind/", F_OK) == 0 ||
                      access("/run/systemd/seats/", F_OK) == 0;
        bool dbus = (access("/run/dbus/system_bus_socket", F_OK) == 0) ||
                    (getenv("DBUS_SYSTEM_BUS_ADDRESS") != NULL);
        if (login1 && dbus) {
#ifdef XW_HAVE_LIBSEAT
            xw_log(XW_LOG_INFO,
                   "seat: elogind/logind detected (/run/elogind or "
                   "/run/systemd/seats, d-bus up) — trying libseat's "
                   "logind backend first");
            struct xw_seat_session *s = libseat_create(c, seat, "logind");
            if (s)
                return s;
            xw_log(XW_LOG_INFO,
                   "seat: the logind/elogind backend did not grant a "
                   "session (%s) — continuing with seatd",
                   strerror(errno ? errno : EPERM));
#else
            xw_log(XW_LOG_INFO,
                   "seat: elogind/logind is present, but this build has "
                   "no libseat support — the logind path is unavailable");
#endif
        } else if (login1) {
            xw_log(XW_LOG_INFO,
                   "seat: elogind/logind dirs present but the system "
                   "d-bus is unreachable — skipping the logind backend");
        } else {
            xw_log(XW_LOG_INFO,
                   "seat: no elogind/logind (/run/elogind and "
                   "/run/systemd/seats both absent) — skipping the "
                   "logind path");
        }

        /* 2. seatd daemon (built-in wire client, no library) */
        if (access(seatd_socket_path(), F_OK) == 0) {
            xw_log(XW_LOG_INFO,
                   "seat: seatd socket present — trying the built-in "
                   "seatd client (%s)", seatd_socket_path());
            struct xw_seat_session *sd = seatd_create(c, seat);
            if (sd)
                return sd;
            xw_log(XW_LOG_INFO, "seat: seatd socket not usable (%s): %s",
                   seatd_socket_path(), strerror(errno ? errno : ECONNREFUSED));
        } else {
            xw_log(XW_LOG_INFO,
                   "seat: no seatd socket (%s) — skipping the seatd "
                   "client", seatd_socket_path());
        }

        /* 3. direct VT takeover */
        xw_log(XW_LOG_INFO, "seat: trying a direct VT session (/dev/tty)");
        struct xw_seat_session *dr = direct_create(c, seat);
        if (dr)
            return dr;

        /* 4. last resort: libseat with its own backend choice
         * (seatd -> logind -> builtin). The builtin backend can open a
         * degenerate VT-less seat when the user's own permissions
         * already cover the devices — DRM often works that way (video
         * group) while input does not, so this step exists to keep the
         * session alive and let the input acquisition report explain
         * exactly what is and is not reachable. */
#ifdef XW_HAVE_LIBSEAT
        xw_log(XW_LOG_INFO,
               "seat: last resort — libseat with its own backend order "
               "(seatd, logind, builtin)");
        struct xw_seat_session *ls = libseat_create(c, seat, NULL);
        if (ls)
            return ls;
        xw_log(XW_LOG_INFO, "seat: libseat found no usable backend (%s)",
               strerror(errno));
#endif

        /* nothing worked: the honest combined diagnostic */
        xw_log(XW_LOG_ERROR,
               "seat: unable to acquire a seat\n"
               "\n"
               "Tried seat/session providers:\n"
#ifdef XW_HAVE_LIBSEAT
               "  libseat logind backend (elogind/logind): %s\n"
#else
               "  libseat: not compiled into this build (the "
               "elogind/logind path needs it)\n"
#endif
               "  seatd socket %s: %s\n"
               "  direct VT session: no usable virtual terminal\n"
               "\n"
               "For a TTY session, install/configure a supported seat "
               "manager such as elogind or seatd (see BUILDING.md, "
               "\"Seat and session management\").\n"
               "This compositor never falls back to running as root.",
#ifdef XW_HAVE_LIBSEAT
               login1 ? "did not grant this login a session"
                      : "no elogind/logind present",
#endif
               seatd_socket_path(),
               access(seatd_socket_path(), F_OK) == 0 ? "unusable"
                                                      : "absent");
        return NULL;
    }
}
